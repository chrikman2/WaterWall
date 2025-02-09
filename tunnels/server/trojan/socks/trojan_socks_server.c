#include "trojan_socks_server.h"
#include "buffer_stream.h"
#include "wsocket.h"
#include "loggers/network_logger.h"
#include "shiftbuffer.h"
#include "tunnel.h"


enum
{
    kCrlfLen = 2
};

enum trojan_cmd
{
    kTrojancmdConnect      = 0X1,
    kTrojancmdUdpAssociate = 0X3,
};
enum trojan_atyp
{
    kTrojanatypIpV4       = 0X1,
    kTrojanatypDomainName = 0X3,
    kTrojanatypIpV6       = 0X4,
};

typedef struct trojan_socks_server_state_s
{
    void *_;

} trojan_socks_server_state_t;

typedef struct trojan_socks_server_con_state_s
{

    buffer_stream_t *udp_stream;
    bool             init_sent;
    bool             first_packet_received;
    bool             udp_logged;

} trojan_socks_server_con_state_t;
static void cleanup(trojan_socks_server_con_state_t *cstate)
{
    if (cstate->udp_stream)
    {
        bufferstreamDestroy(cstate->udp_stream);
    }
    memoryFree(cstate);
}
static void encapsulateUdpPacket(context_t *c)
{
    uint16_t packet_len = sbufGetBufLength(c->payload);
    packet_len          = packet_len > 8192 ? 8192 : packet_len;

    sbufShiftLeft(c->payload, kCrlfLen);
    sbufWrite(c->payload, (unsigned char *) "\r\n", 2);

    sbufShiftLeft(c->payload, 2); // LEN
    sbufWriteUnAlignedUI16(c->payload, htons(packet_len));

    uint16_t port = sockaddrPort(&(c->line->dest_ctx.address));
    sbufShiftLeft(c->payload, 2); // port
    sbufWriteUnAlignedUI16(c->payload, htons(port));

    switch (c->line->dest_ctx.address_type)
    {
    case kSatIPV6:
        sbufShiftLeft(c->payload, 16);
        sbufWrite(c->payload, &(c->line->dest_ctx.address.sin6.sin6_addr), 16);
        sbufShiftLeft(c->payload, 1);
        sbufWriteUnAlignedUI8(c->payload, kTrojanatypIpV6);
        break;

    case kSatIPV4:
    default:
        sbufShiftLeft(c->payload, 4);
        sbufWrite(c->payload, &(c->line->dest_ctx.address.sin.sin_addr), 4);
        sbufShiftLeft(c->payload, 1);
        sbufWriteUnAlignedUI8(c->payload, kTrojanatypIpV4);
        break;
    }
}

static bool parseAddress(context_t *c)
{
    if (sbufGetBufLength(c->payload) < 2)
    {
        return false;
    }
    connection_context_t *dest_context = &(c->line->dest_ctx);
    enum trojan_cmd   command      = ((unsigned char *) sbufGetRawPtr(c->payload))[0];
    enum trojan_atyp  address_type = ((unsigned char *) sbufGetRawPtr(c->payload))[1];
    dest_context->address_type     = (enum socket_address_type)(address_type);
    sbufShiftRight(c->payload, 2);

    switch (command)
    {
    case kTrojancmdConnect:
        dest_context->address_protocol = kSapTcp;
        switch (address_type)
        {
        case kTrojanatypIpV4:
            if (sbufGetBufLength(c->payload) < 4)
            {
                return false;
            }
            dest_context->address.sa.sa_family = AF_INET;
            memoryCopy(&(dest_context->address.sin.sin_addr), sbufGetRawPtr(c->payload), 4);
            sbufShiftRight(c->payload, 4);
            LOGD("TrojanSocksServer: tcp connect ipv4");
            break;
        case kTrojanatypDomainName:
            if (sbufGetBufLength(c->payload) < 1)
            {
                return false;
            }
            uint8_t addr_len = ((uint8_t *) sbufGetRawPtr(c->payload))[0];
            sbufShiftRight(c->payload, 1);
            if (sbufGetBufLength(c->payload) < addr_len)
            {
                return false;
            }
            LOGD("TrojanSocksServer: tcp connect domain %.*s", addr_len, sbufGetRawPtr(c->payload));
            connectionContextDomainSet(dest_context, sbufGetRawPtr(c->payload), addr_len);
            sbufShiftRight(c->payload, addr_len);
            break;
        case kTrojanatypIpV6:
            if (sbufGetBufLength(c->payload) < 16)
            {
                return false;
            }
            dest_context->address.sa.sa_family = AF_INET6;
            memoryCopy(&(dest_context->address.sin.sin_addr), sbufGetRawPtr(c->payload), 16);
            sbufShiftRight(c->payload, 16);
            LOGD("TrojanSocksServer: tcp connect ipv6");
            break;

        default:
            LOGD("TrojanSocksServer: address_type was incorrect (%02X) ", (unsigned int) (address_type));
            return false;
            break;
        }
        break;
    case kTrojancmdUdpAssociate:
        dest_context->address_protocol = kSapUdp;
        switch (address_type)
        {

        case kTrojanatypIpV4:
            if (sbufGetBufLength(c->payload) < 4)
            {
                return false;
            }
            sbufShiftRight(c->payload, 4);
            dest_context->address.sa.sa_family = AF_INET;
            // LOGD("TrojanSocksServer: udp associate ipv4");

            break;
        case kTrojanatypDomainName:
            // LOGD("TrojanSocksServer: udp domain ");

            if (sbufGetBufLength(c->payload) < 1)
            {
                return false;
            }
            uint8_t addr_len = ((uint8_t *) sbufGetRawPtr(c->payload))[0];
            sbufShiftRight(c->payload, 1);
            if (sbufGetBufLength(c->payload) < addr_len)
            {
                return false;
            }
            sbufShiftRight(c->payload, addr_len);

            break;
        case kTrojanatypIpV6:
            if (sbufGetBufLength(c->payload) < 16)
            {
                return false;
            }
            sbufShiftRight(c->payload, 16);
            // LOGD("TrojanSocksServer: connect ipv6");

            break;
        default:
            LOGD("TrojanSocksServer: address_type was incorrect (%02X) ", (unsigned int) (address_type));
            return false;
            break;
        }
        break;
    default:
        LOGE("TrojanSocksServer: command was incorrect (%02X) ", (unsigned int) (command));
        return false;
        break;
    }
    // port(2) + crlf(2)
    if (sbufGetBufLength(c->payload) < 4)
    {
        return false;
    }
    memoryCopy(&(dest_context->address.sin.sin_port), sbufGetRawPtr(c->payload), 2);
    sbufShiftRight(c->payload, 2 + kCrlfLen);
    return true;
}

static bool processUdp(tunnel_t *self, trojan_socks_server_con_state_t *cstate, line_t *line)
{
    buffer_stream_t *bstream = cstate->udp_stream;
    if (bufferstreamLen(bstream) <= 0)
    {
        return true;
    }

    uint8_t  address_type = bufferstreamViewByteAt(bstream, 0);
    uint16_t packet_size  = 0;
    uint16_t full_len     = 0;
    uint8_t  domain_len   = 0;
    switch (address_type)
    {
    case kTrojanatypIpV4:
        // address_type | DST.ADDR | DST.PORT | Length |  CRLF   | Payload
        //       1      |    4     |    2     |   2    |    2

        if (bufferstreamLen(bstream) < 1 + 4 + 2 + 2 + 2)
        {
            return true;
        }

        {
            uint8_t packet_size_h = bufferstreamViewByteAt(bstream, 1 + 4 + 2);
            uint8_t packet_size_l = bufferstreamViewByteAt(bstream, 1 + 4 + 2 + 1);
            packet_size           = (packet_size_h << 8) | packet_size_l;
            if (packet_size > 8192)
            {
                return false;
            }
        }
        full_len = 1 + 4 + 2 + 2 + 2 + packet_size;

        break;
    case kTrojanatypDomainName:
        // address_type | DST.ADDR | DST.PORT | Length |  CRLF   | Payload
        //      1       | x(1) + x |    2     |   2    |    2
        if (bufferstreamLen(bstream) < 1 + 1 + 2 + 2 + 2)
        {
            return true;
        }
        domain_len = bufferstreamViewByteAt(bstream, 1);

        if ((int) bufferstreamLen(bstream) < 1 + 1 + domain_len + 2 + 2 + 2)
        {
            return true;
        }
        {
            uint8_t packet_size_h = bufferstreamViewByteAt(bstream, 1 + 1 + domain_len + 2);
            uint8_t packet_size_l = bufferstreamViewByteAt(bstream, 1 + 1 + domain_len + 2 + 1);
            packet_size           = (packet_size_h << 8) | packet_size_l;
            if (packet_size > 8192)
            {
                return false;
            }
        }
        full_len = 1 + 1 + domain_len + 2 + 2 + 2 + packet_size;

        break;
    case kTrojanatypIpV6:
        // address_type | DST.ADDR | DST.PORT | Length |  CRLF   | Payload
        //      1       |   16     |    2     |   2    |    2

        if (bufferstreamLen(bstream) < 1 + 16 + 2 + 2 + 2)
        {
            return true;
        }
        {

            uint8_t packet_size_h = bufferstreamViewByteAt(bstream, 1 + 16 + 2);
            uint8_t packet_size_l = bufferstreamViewByteAt(bstream, 1 + 16 + 2 + 1);
            packet_size           = (packet_size_h << 8) | packet_size_l;
            if (packet_size > 8192)
            {
                return false;
            }
        }

        full_len = 1 + 16 + 2 + 2 + 2 + packet_size;

        break;

    default:
        return false;
        break;
    }
    if (bufferstreamLen(bstream) < full_len)
    {
        return true;
    }

    context_t        *c            = contextCreate(line);
    connection_context_t *dest_context = &(c->line->dest_ctx);
    c->payload                     = bufferstreamReadExact(bstream, full_len);

    if (cstate->init_sent)
    {
        sbufShiftRight(c->payload, full_len - packet_size);
        self->up->upStream(self->up, c);
        if (! lineIsAlive(line))
        {
            return true;
        }
        return processUdp(self, cstate, line);
    }

    dest_context->address.sa.sa_family = AF_INET;
    sbufShiftRight(c->payload, 1);

    switch (address_type)
    {
    case kTrojanatypIpV4:
        dest_context->address.sa.sa_family = AF_INET;
        dest_context->address_type         = kSatIPV4;
        memoryCopy(&(dest_context->address.sin.sin_addr), sbufGetRawPtr(c->payload), 4);
        sbufShiftRight(c->payload, 4);
        if (! cstate->udp_logged)
        {
            cstate->udp_logged = true;
            LOGD("TrojanSocksServer: udp ipv4");
        }

        break;
    case kTrojanatypDomainName:
        dest_context->address_type = kSatDomainName;
        // size_t addr_len = (unsigned char)(sbufGetRawPtr(c->payload)[0]);
        sbufShiftRight(c->payload, 1);
        if (! cstate->udp_logged)
        {
            cstate->udp_logged = true;
            LOGD("TrojanSocksServer: udp domain %.*s", domain_len, sbufGetRawPtr(c->payload));
        }

        connectionContextDomainSet(dest_context, sbufGetRawPtr(c->payload), domain_len);
        sbufShiftRight(c->payload, domain_len);

        break;
    case kTrojanatypIpV6:
        dest_context->address_type         = kSatIPV6;
        dest_context->address.sa.sa_family = AF_INET6;
        memoryCopy(&(dest_context->address.sin.sin_addr), sbufGetRawPtr(c->payload), 16);
        sbufShiftRight(c->payload, 16);
        if (! cstate->udp_logged)
        {
            cstate->udp_logged = true;
            LOGD("TrojanSocksServer: udp ipv6");
        }
        break;

    default:
        contextReusePayload(c);
        contextDestroy(c);
        return false;
        break;
    }

    // port(2)
    if (sbufGetBufLength(c->payload) < 2)
    {
        return false;
    }
    memoryCopy(&(dest_context->address.sin.sin_port), sbufGetRawPtr(c->payload), 2);

    // port 2 length 2 crlf 2
    sbufShiftRight(c->payload, 2 + 2 + kCrlfLen);
    assert(sbufGetBufLength(c->payload) == packet_size);

    // send init ctx
    if (! cstate->init_sent)
    {
        context_t *up_init_ctx = contextCreateFrom(c);
        up_init_ctx->init      = true;
        self->up->upStream(self->up, up_init_ctx);
        if (! lineIsAlive(c->line))
        {
            LOGW("TrojanSocksServer: next node instantly closed the init with fin");
            contextReusePayload(c);
            contextDestroy(c);
            return true;
        }
        cstate->init_sent = true;
    }

    self->up->upStream(self->up, c);

    if (! lineIsAlive(line))
    {
        return true;
    }
    return processUdp(self, cstate, line);
}

static void upStream(tunnel_t *self, context_t *c)
{
    if (c->payload != NULL)
    {
        trojan_socks_server_con_state_t *cstate = CSTATE(c);

        if (! cstate->first_packet_received)
        {
            cstate->first_packet_received = true;
            if (parseAddress(c))
            {
                connection_context_t *dest_context = &(c->line->dest_ctx);

                if (dest_context->address_protocol == kSapTcp)
                {
                    context_t *up_init_ctx = contextCreateFrom(c);
                    up_init_ctx->init      = true;
                    self->up->upStream(self->up, up_init_ctx);
                    if (! lineIsAlive(c->line))
                    {
                        LOGW("TrojanSocksServer: next node instantly closed the init with fin");
                        contextReusePayload(c);
                        contextDestroy(c);
                        return;
                    }
                    cstate->init_sent = true;
                }
                else if (dest_context->address_protocol == kSapUdp)
                {
                    // udp will not call init here since no dest_context addr is available right now
                    cstate->udp_stream = bufferstreamCreate(contextGetBufferPool(c));
                }

                if (sbufGetBufLength(c->payload) <= 0)
                {
                    contextReusePayload(c);
                    contextDestroy(c);
                    return;
                }
                if (dest_context->address_protocol == kSapTcp)
                {
                    self->up->upStream(self->up, c);
                    return;
                }
                if (dest_context->address_protocol == kSapUdp)
                {
                    bufferStreamPushContextPayload(cstate->udp_stream, c);

                    if (! processUdp(self, cstate, c->line))
                    {
                        LOGE("TrojanSocksServer: udp packet could not be parsed");

                        if (cstate->init_sent)
                        {
                            self->up->upStream(self->up, contextCreateFin(c->line));
                        }

                        cleanup(cstate);
                        CSTATE_DROP(c);
                        context_t *fin_dw = contextCreateFinFrom(c);
                        contextDestroy(c);
                        self->dw->downStream(self->dw, fin_dw);
                    }
                    else
                    {
                        contextDestroy(c);
                    }
                }
                else
                {
                    LOGF("unreachable trojan-socket_server");
                    exit(1);
                }
            }
            else
            {
                contextReusePayload(c);
                cleanup(cstate);
                CSTATE_DROP(c);
                context_t *reply = contextCreateFinFrom(c);
                contextDestroy(c);
                self->dw->downStream(self->dw, reply);
            }
        }
        else
        {

            if (c->line->dest_ctx.address_protocol == kSapUdp)
            {
                bufferStreamPushContextPayload(cstate->udp_stream, c);

                if (! processUdp(self, cstate, c->line))
                {
                    LOGE("TrojanSocksServer: udp packet could not be parsed");

                    if (cstate->init_sent)
                    {
                        self->up->upStream(self->up, contextCreateFin(c->line));
                    }
                    if (cstate->udp_stream)
                    {
                        bufferstreamDestroy(cstate->udp_stream);
                        cstate->udp_stream = NULL;
                    }
                    cleanup(cstate);
                    CSTATE_DROP(c);
                    context_t *fin_dw = contextCreateFinFrom(c);
                    contextDestroy(c);
                    self->dw->downStream(self->dw, fin_dw);
                }
                else
                {
                    contextDestroy(c);
                }
            }
            else
            {
                self->up->upStream(self->up, c);
            }
        }
    }
    else
    {
        if (c->init)
        {
            CSTATE_MUT(c) = memoryAllocate(sizeof(trojan_socks_server_con_state_t));
            memorySet(CSTATE(c), 0, sizeof(trojan_socks_server_con_state_t));
            contextDestroy(c);
        }
        else if (c->fin)
        {
            trojan_socks_server_con_state_t *cstate    = CSTATE(c);
            bool                             init_sent = cstate->init_sent;
            cleanup(cstate);
            CSTATE_DROP(c);
            if (init_sent)
            {
                self->up->upStream(self->up, c);
            }
            else
            {
                contextDestroy(c);
            }
        }
    }
}

static void downStream(tunnel_t *self, context_t *c)
{

    if (c->fin)
    {
        trojan_socks_server_con_state_t *cstate = CSTATE(c);

        cleanup(cstate);
        CSTATE_DROP(c);
        self->dw->downStream(self->dw, c);
        return;
    }
    if (c->line->dest_ctx.address_protocol == kSapUdp && c->payload != NULL)
    {
        encapsulateUdpPacket(c);
    }
    self->dw->downStream(self->dw, c);
}

tunnel_t *newTrojanSocksServer(node_instance_context_t *instance_info)
{
    (void) instance_info;
    trojan_socks_server_state_t *state = memoryAllocate(sizeof(trojan_socks_server_state_t));
    memorySet(state, 0, sizeof(trojan_socks_server_state_t));

    tunnel_t *t   = tunnelCreate();
    t->state      = state;
    t->upStream   = &upStream;
    t->downStream = &downStream;

    return t;
}
api_result_t apiTrojanSocksServer(tunnel_t *self, const char *msg)
{
    (void) self;
    (void) msg;
    return (api_result_t) {0};
}

tunnel_t *destroyTrojanSocksServer(tunnel_t *self)
{
    (void) self;
    return NULL;
}

tunnel_metadata_t getMetadataTrojanSocksServer(void)
{
    return (tunnel_metadata_t) {.version = 0001, .flags = 0x0};
}
