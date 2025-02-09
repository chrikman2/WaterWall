#include "reality_server.h"

#include "buffer_pool.h"
#include "buffer_stream.h"
#include "loggers/network_logger.h"
#include "managers/node_manager.h"
#include "reality_helpers.h"
#include "shiftbuffer.h"
#include "tunnel.h"
#include "utils/jsonutils.h"

#include <openssl/evp.h>

enum connection_auth_state
{
    kConAuthPending,
    kConUnAuthorized,
    kConAuthorized
};

typedef struct reality_server_state_s
{

    tunnel_t        *dest;
    EVP_MD_CTX     **threadlocal_sign_context;
    EVP_CIPHER_CTX **threadlocal_cipher_context;

    // settings
    uint8_t      hashes[EVP_MAX_MD_SIZE];
    char         context_password[kSignPasswordLen];
    uint32_t     counter_threshold;
    char        *password;
    unsigned int password_length;

} reality_server_state_t;

typedef struct reality_server_con_state_s
{
    EVP_MD                    *msg_digest;
    EVP_PKEY                  *sign_key;
    EVP_MD_CTX                *sign_context;
    EVP_CIPHER_CTX            *cipher_context;
    buffer_stream_t           *read_stream;
    uint8_t                    giveup_counter;
    enum connection_auth_state auth_state;
    uint32_t                   reply_sent_tit;

} reality_server_con_state_t;

static void cleanup(tunnel_t *self, context_t *c)
{
    reality_server_con_state_t *cstate = CSTATE(c);

    bufferstreamDestroy(cstate->read_stream);
    // EVP_CIPHER_CTX_free(cstate->cipher_context);
    // EVP_MD_CTX_free(cstate->sign_context);
    EVP_MD_free(cstate->msg_digest);
    EVP_PKEY_free(cstate->sign_key);

    memoryFree(cstate);
    CSTATE_DROP(c);
}

static void upStream(tunnel_t *self, context_t *c)
{
    reality_server_state_t     *state  = TSTATE(self);
    reality_server_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {

        switch (cstate->auth_state)
        {
        case kConAuthPending: {

            bufferStreamPushContextPayload(cstate->read_stream, c);

            uint8_t tls_header[1 + 2 + 2];

            while (lineIsAlive(c->line) && bufferstreamLen(cstate->read_stream) >= kTLSHeaderlen)
            {
                bufferstreamViewBytesAt(cstate->read_stream, 0, tls_header, kTLSHeaderlen);
                uint16_t length = ntohs(*(uint16_t *) (tls_header + 3));

                if ((int) bufferstreamLen(cstate->read_stream) >= kTLSHeaderlen + length)
                {
                    sbuf_t *record_buf = bufferstreamReadExact(cstate->read_stream, kTLSHeaderlen + length);
                    sbufShiftRight(record_buf, kTLSHeaderlen);

                    if (verifyMessage(record_buf, cstate->msg_digest, cstate->sign_context, cstate->sign_key))
                    {
                        contextReusePayload(c);
                        cstate->auth_state = kConAuthorized;

                        state->dest->upStream(state->dest, contextCreateFinFrom(c));
                        self->up->upStream(self->up, contextCreateInit(c->line));
                        if (! lineIsAlive(c->line))
                        {
                            bufferpoolResuesBuffer(contextGetBufferPool(c), record_buf);
                            contextDestroy(c);

                            return;
                        }

                        record_buf = genericDecrypt(record_buf, cstate->cipher_context, state->context_password,
                                                    contextGetBufferPool(c));
                        context_t *plain_data_ctx = contextCreateFrom(c);
                        plain_data_ctx->payload   = record_buf;
                        self->up->upStream(self->up, plain_data_ctx);

                        if (! lineIsAlive(c->line))
                        {
                            contextDestroy(c);
                            return;
                        }
                        goto authorized;
                    }
                    else
                    {
                        bufferpoolResuesBuffer(contextGetBufferPool(c), record_buf);
                    }
                }
                else
                {
                    break;
                }
            }

            cstate->giveup_counter -= 0;
            if (cstate->giveup_counter == 0)
            {
                bufferstreamEmpty(cstate->read_stream);
                cstate->auth_state = kConUnAuthorized;
            }
        }
            // fallthrough

        case kConUnAuthorized:
            state->dest->upStream(state->dest, c);

            break;
        case kConAuthorized: {
            bufferStreamPushContextPayload(cstate->read_stream, c);
        authorized: {
            uint8_t tls_header[1 + 2 + 2];
            while (lineIsAlive(c->line) && bufferstreamLen(cstate->read_stream) >= kTLSHeaderlen)
            {
                bufferstreamViewBytesAt(cstate->read_stream, 0, tls_header, kTLSHeaderlen);
                uint16_t length = ntohs(*(uint16_t *) (tls_header + 3));
                if ((int) bufferstreamLen(cstate->read_stream) >= kTLSHeaderlen + length)
                {
                    sbuf_t *buf = bufferstreamReadExact(cstate->read_stream, kTLSHeaderlen + length);
                    bool            is_tls_applicationdata = ((uint8_t *) sbufGetRawPtr(buf))[0] == kTLS12ApplicationData;
                    uint16_t        tls_ver_b;
                    memoryCopy(&tls_ver_b, ((uint8_t *) sbufGetRawPtr(buf)) + 1, sizeof(uint16_t));
                    bool is_tls_33 = tls_ver_b == kTLSVersion12;
                    sbufShiftRight(buf, kTLSHeaderlen);

                    if (! verifyMessage(buf, cstate->msg_digest, cstate->sign_context, cstate->sign_key) ||
                        ! is_tls_applicationdata || ! is_tls_33)
                    {
                        LOGE("RealityServer: verifyMessage failed");
                        bufferpoolResuesBuffer(contextGetBufferPool(c), buf);
                        goto failed;
                    }

                    buf = genericDecrypt(buf, cstate->cipher_context, state->context_password, contextGetBufferPool(c));

                    context_t *plain_data_ctx = contextCreateFrom(c);
                    plain_data_ctx->payload   = buf;
                    self->up->upStream(self->up, plain_data_ctx);
                }
                else
                {
                    break;
                }
            }
            contextDestroy(c);
        }
        }

        break;
        }
    }
    else
    {

        if (c->init)
        {
            cstate = CSTATE_MUT(c) = memoryAllocate(sizeof(reality_server_con_state_t));
            memorySet(CSTATE(c), 0, sizeof(reality_server_con_state_t));
            cstate->auth_state     = kConAuthPending;
            cstate->giveup_counter = state->counter_threshold;
            cstate->cipher_context = state->threadlocal_cipher_context[getWID()];
            cstate->sign_context   = state->threadlocal_sign_context[getWID()];
            cstate->read_stream    = bufferstreamCreate(contextGetBufferPool(c));
            cstate->msg_digest     = (EVP_MD *) EVP_get_digestbynid(MSG_DIGEST_ALG);
            int sk_size            = EVP_MD_size(cstate->msg_digest);
            cstate->sign_key       = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, state->hashes, sk_size);

            state->dest->upStream(state->dest, c);
        }
        else if (c->fin)
        {

            tunnel_t *next = (cstate->auth_state == kConAuthorized) ? self->up : state->dest;
            cleanup(self, c);
            next->upStream(next, c);
        }
    }
    return;
failed: {
    context_t *fail_context_up = contextCreateFinFrom(c);
    self->up->upStream(self->up, fail_context_up);

    context_t *fail_context = contextCreateFinFrom(c);
    cleanup(self, c);
    contextDestroy(c);
    self->dw->downStream(self->dw, fail_context);
}
}

static void downStream(tunnel_t *self, context_t *c)
{
    reality_server_state_t     *state  = TSTATE(self);
    reality_server_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {

        switch (cstate->auth_state)
        {
        case kConAuthPending:
        case kConUnAuthorized:
            self->dw->downStream(self->dw, c);
            break;
        case kConAuthorized: {
            sbuf_t *buf           = c->payload;
            c->payload                    = NULL;
            const unsigned int chunk_size = (kMaxSSLChunkSize - (kSignLen + (2 * kEncryptionBlockSize) + kIVlen));

            if (sbufGetBufLength(buf) < chunk_size)
            {
                buf = genericEncrypt(buf, cstate->cipher_context, state->context_password, contextGetBufferPool(c));
                signMessage(buf, cstate->msg_digest, cstate->sign_context, cstate->sign_key);
                appendTlsHeader(buf);
                assert(sbufGetBufLength(buf) % 16 == 5);
                c->payload = buf;
                self->dw->downStream(self->dw, c);
            }
            else
            {
                while (sbufGetBufLength(buf) > 0 && lineIsAlive(c->line))
                {
                    const uint16_t  remain = (uint16_t) min(sbufGetBufLength(buf), chunk_size);
                    sbuf_t *chunk  = bufferpoolGetLargeBuffer(contextGetBufferPool(c));
                    chunk = sbufMoveTo(chunk, buf, remain);
                    chunk =
                        genericEncrypt(chunk, cstate->cipher_context, state->context_password, contextGetBufferPool(c));
                    signMessage(chunk, cstate->msg_digest, cstate->sign_context, cstate->sign_key);
                    appendTlsHeader(chunk);
                    context_t *cout = contextCreateFrom(c);
                    cout->payload   = chunk;
                    assert(sbufGetBufLength(chunk) % 16 == 5);
                    self->dw->downStream(self->dw, cout);
                }
                bufferpoolResuesBuffer(contextGetBufferPool(c), buf);
                contextDestroy(c);
            }
        }
        break;
        }
    }
    else
    {
        if (c->est)
        {
            if (cstate->auth_state == kConAuthorized)
            {
                contextDestroy(c);
                return;
            }
            self->dw->downStream(self->dw, c);
            return;
        }
        if (c->fin)
        {
            cleanup(self, c);
            self->dw->downStream(self->dw, c);
        }
    }
}

tunnel_t *newRealityServer(node_instance_context_t *instance_info)
{
    reality_server_state_t *state = memoryAllocate(sizeof(reality_server_state_t));
    memorySet(state, 0, sizeof(reality_server_state_t));
    const cJSON *settings = instance_info->node_settings_json;

    state->threadlocal_cipher_context = memoryAllocate(sizeof(EVP_CIPHER_CTX *) * getWorkersCount());
    state->threadlocal_sign_context   = memoryAllocate(sizeof(EVP_MD_CTX *) * getWorkersCount());

    for (unsigned int i = 0; i < getWorkersCount(); i++)
    {
        state->threadlocal_cipher_context[i] = EVP_CIPHER_CTX_new();
        state->threadlocal_sign_context[i]   = EVP_MD_CTX_create();
    }

    if (! (cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: RealityServer->settings (object field) : The object was empty or invalid");
        return NULL;
    }

    if (! getStringFromJsonObject(&(state->password), settings, "password"))
    {
        LOGF("JSON Error: RealityServer->settings->password (string field) : The data was empty or invalid");
        return NULL;
    }
    getIntFromJsonObjectOrDefault((int *) &(state->counter_threshold), settings, "sniffing-counter", 7);

    state->password_length = (int) strlen(state->password);
    if (state->password_length < 3)
    {
        LOGF("JSON Error: RealityServer->settings->password (string field) : password is too short");
        return NULL;
    }
    // memorySet already made buff 0
    memoryCopy(state->context_password, state->password, state->password_length);
    if (EVP_MAX_MD_SIZE % sizeof(uint64_t) != 0)
    {
        LOGF("Assert Error: RealityServer-> EVP_MAX_MD_SIZE not a multiple of 8");
        return NULL;
    }
    uint64_t *p64 = (uint64_t *) state->hashes;
    p64[0]        = calcHashBytes(state->password, strlen(state->password));
    for (int i = 1; i < (int) (EVP_MAX_MD_SIZE / sizeof(uint64_t)); i++)
    {
        p64[i] = p64[i - 1];
    }

    char *dest_node_name = NULL;
    if (! getStringFromJsonObject(&dest_node_name, settings, "destination"))
    {
        LOGW("RealityServer: no destination node provided in json");
        return NULL;
    }

    hash_t  hash_next = calcHashBytes(dest_node_name, strlen(dest_node_name));
    node_t *next_node = nodemanagerGetNode(instance_info->node_manager_config, hash_next);
    if (next_node == NULL)
    {
        LOGF("RealityServer: destination node not found");
        exit(1);
    }

    if (next_node->instance == NULL)
    {
        nodemanagerRunNode(instance_info->node_manager_config, next_node, instance_info->chain_index + 1);
    }

    state->dest = next_node->instance;
    memoryFree(dest_node_name);

    tunnel_t *t = tunnelCreate();
    tunnelBindDown(t, state->dest);
    t->state      = state;
    t->upStream   = &upStream;
    t->downStream = &downStream;

    return t;
}

api_result_t apiRealityServer(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t) {0};
}

tunnel_t *destroyRealityServer(tunnel_t *self)
{
    (void) (self);
    return NULL;
}

tunnel_metadata_t getMetadataRealityServer(void)
{
    return (tunnel_metadata_t) {.version = 0001, .flags = 0x0};
}
