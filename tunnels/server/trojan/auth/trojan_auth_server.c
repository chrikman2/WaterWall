#include "trojan_auth_server.h"
#include "loggers/network_logger.h"
#include "managers/node_manager.h"
#include "utils/jsonutils.h"
#include "utils/objects/user.h"

#define i_type hmap_users_t    // NOLINT
#define i_key  hash_t          // NOLINT
#define i_val  trojan_user_t * // NOLINT

enum
{
    kVecCap  = 100,
    kCRLFLen = 2
};

#include "stc/hmap.h"

typedef struct trojan_auth_server_state_s
{
    config_file_t *config_file;
    tunnel_t      *fallback;
    int            fallback_delay;
    hmap_users_t   users;

} trojan_auth_server_state_t;

typedef struct trojan_auth_server_con_state_s
{
    bool authenticated;
    bool init_sent;
    bool first_packet_received;

} trojan_auth_server_con_state_t;

struct timer_eventdata
{
    tunnel_t  *self;
    context_t *c;
};

static struct timer_eventdata *newTimerData(tunnel_t *self, context_t *c)
{
    struct timer_eventdata *result = memoryAllocate(sizeof(struct timer_eventdata));
    result->self                   = self;
    result->c                      = c;
    return result;
}

static void onFallbackTimer(wtimer_t *timer)
{
    struct timer_eventdata     *data  = weventGetUserdata(timer);
    tunnel_t                   *self  = data->self;
    trojan_auth_server_state_t *state = TSTATE(self);
    context_t                  *c     = data->c;

    memoryFree(data);
    wtimerDelete(timer);

    if (! lineIsAlive(c->line))
    {
        if (c->payload != NULL)
        {
            contextReusePayload(c);
        }
        contextDestroy(c);
        return;
    }
    state->fallback->upStream(state->fallback, c);
}

static void upStream(tunnel_t *self, context_t *c)
{
    trojan_auth_server_state_t     *state  = TSTATE(self);
    trojan_auth_server_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {
        if (cstate->authenticated)
        {
            self->up->upStream(self->up, c);
        }
        else if (! cstate->first_packet_received)
        {
            cstate->first_packet_received = true;
            // struct timeval tv1, tv2;
            // getTimeOfDay(&tv1, NULL);
            {

                // beware! trojan auth will not use stream buffer, at least the auth chunk must come in first sequence
                // the payload must not come buffered here (gfw can do this and detect trojan authentication
                // but the client is not supposed to send small segments)
                // so , if its incomplete we go to fallback!
                // this is also mentioned in standard trojan docs (first packet also contains part of final payload)
                size_t len = sbufGetBufLength(c->payload);
                if (len < (sizeof(sha224_hex_t) + kCRLFLen))
                {
                    // invalid protocol
                    LOGW("TrojanAuthServer: detected non trojan protocol, rejected");
                    goto failed;
                }

                if (((unsigned char *) sbufGetRawPtr(c->payload))[sizeof(sha224_hex_t)] != '\r' ||
                    ((unsigned char *) sbufGetRawPtr(c->payload))[sizeof(sha224_hex_t) + 1] != '\n')
                {
                    LOGW("TrojanAuthServer: detected non trojan protocol, rejected");
                    goto failed;
                }

                hash_t kh = calcHashBytes(sbufGetRawPtr(c->payload), sizeof(sha224_hex_t));

                hmap_users_t_iter find_result = hmap_users_t_find(&(state->users), kh);
                if (find_result.ref == hmap_users_t_end(&(state->users)).ref)
                {
                    // user not in database
                    LOGW("TrojanAuthServer: a trojan-user rejected because not found in database");
                    goto failed;
                }
                trojan_user_t *tuser = (find_result.ref->second);
                if (! tuser->user.enable)
                {
                    // user disabled
                    LOGW("TrojanAuthServer: user \"%s\" rejected because not enabled", tuser->user.name);

                    goto failed;
                }
                LOGD("TrojanAuthServer: user \"%s\" accepted", tuser->user.name);
                cstate->authenticated = true;
                lineAuthenticate(c->line);
                cstate->init_sent = true;
                self->up->upStream(self->up, contextCreateInit(c->line));
                if (! lineIsAlive(c->line))
                {
                    contextReusePayload(c);
                    contextDestroy(c);
                    return;
                }

                sbufShiftRight(c->payload, sizeof(sha224_hex_t) + kCRLFLen);
                self->up->upStream(self->up, c);
            }
            // getTimeOfDay(&tv2, NULL);
            // double time_spent = (double)(tv2.tv_usec - tv1.tv_usec) / 1000000 + (double)(tv2.tv_sec -
            // tv1.tv_sec); LOGD("Auth: took %lf sec", time_spent);
        }
        else
        {
            goto failed;
        }
    }
    else
    {
        if (c->init)
        {
            cstate        = memoryAllocate(sizeof(trojan_auth_server_con_state_t));
            *cstate       = (trojan_auth_server_con_state_t) {0};
            CSTATE_MUT(c) = cstate;
            contextDestroy(c);
        }
        else if (c->fin)
        {
            bool init_sent = cstate->init_sent;
            bool auth      = cstate->authenticated;
            memoryFree(CSTATE(c));
            CSTATE_DROP(c);
            if (init_sent)
            {
                if (auth)
                {
                    self->up->upStream(self->up, c);
                }
                else
                {
                    state->fallback->upStream(state->fallback, c);
                }
            }
            else
            {
                contextDestroy(c);
            }
        }
    }

    return;
failed:
    if (state->fallback != NULL)
    {
        goto fallback;
    }

    // disconnect:
    contextReusePayload(c);
    memoryFree(CSTATE(c));
    CSTATE_DROP(c);
    context_t *reply = contextCreateFinFrom(c);
    contextDestroy(c);
    self->dw->downStream(self->dw, reply);
    return;
fallback:
    if (! cstate->init_sent)
    {
        cstate->init_sent = true;
        state->fallback->upStream(state->fallback, contextCreateInit(c->line));
        if (! lineIsAlive(c->line))
        {
            contextReusePayload(c);
            contextDestroy(c);
            return;
        }
    }
    if (state->fallback_delay <= 0)
    {
        state->fallback->upStream(state->fallback, c);
    }
    else
    {
        wtimer_t *t = wtimerAdd(getWorkerLoop(c->line), onFallbackTimer, state->fallback_delay, 1);
        weventSetUserData(t, newTimerData(self, c));
    }
}

static void downStream(tunnel_t *self, context_t *c)
{
    if (c->fin)
    {
        memoryFree(CSTATE(c));
        CSTATE_DROP(c);
    }
    self->dw->downStream(self->dw, c);
}

static void parse(tunnel_t *t, cJSON *settings, node_instance_context_t *instance_info)
{
    trojan_auth_server_state_t *state = t->state;
    if (! (cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: TrojanAuthServer->Settings (object field) was empty or invalid");
        exit(1);
    }
    const cJSON *users_array = cJSON_GetObjectItemCaseSensitive(settings, "users");
    if (! (cJSON_IsArray(users_array) && users_array->child != NULL))
    {
        LOGF("JSON Error: TrojanAuthServer->Settings->Users (array field) was empty or invalid");
        exit(1);
    }
    cJSON *element = NULL;

    unsigned int total_parsed = 0;
    unsigned int total_users  = 0;
    cJSON_ArrayForEach(element, users_array)
    {
        user_t *user = parseUserFromJsonObject(element);
        if (user == NULL)
        {
            LOGW("TrojanAuthServer: 1 User json-parse failed, please check the json");
        }
        else
        {
            total_parsed++;
            trojan_user_t *tuser = memoryAllocate(sizeof(trojan_user_t));
            memorySet(tuser, 0, sizeof(trojan_user_t));
            tuser->user = *user;
            memoryFree(user);
            sha224((uint8_t *) tuser->user.uid, strlen(tuser->user.uid), &(tuser->sha224_of_user_uid[0]));

            for (size_t i = 0; i < sizeof(sha224_t); i++)
            {
                sprintf((char *) &(tuser->hexed_sha224_of_user_uid[i * 2]), "%02x", (tuser->sha224_of_user_uid[i]));
            }
            LOGD("TrojanAuthServer: user \"%s\" parsed, sha224: %.12s...", tuser->user.name,
                 tuser->hexed_sha224_of_user_uid);

            tuser->hash_of_hexed_sha224_of_user_uid =
                calcHashBytes(tuser->hexed_sha224_of_user_uid, sizeof(sha224_hex_t));

            if (! hmap_users_t_insert(&(state->users), tuser->hash_of_hexed_sha224_of_user_uid, tuser).inserted)
            {
                LOGW("TrojanAuthServer: duplicate passwords, 2 users have exactly same password");
            }
        }

        total_users++;
    }
    LOGI("TrojanAuthServer: %zu users parsed (out of total %zu) and can connect", total_parsed, total_users);

    char *fallback_node_name = NULL;
    if (! getStringFromJsonObject(&fallback_node_name, settings, "fallback"))
    {
        LOGW("TrojanAuthServer: no fallback provided in json, standard trojan requires fallback");
    }
    else
    {

        getIntFromJsonObject(&(state->fallback_delay), settings, "fallback-intence-delay");
        if (state->fallback_delay < 0)
        {
            state->fallback_delay = 0;
        }

        hash_t  hash_next     = calcHashBytes(fallback_node_name, strlen(fallback_node_name));
        node_t *fallback_node = nodemanagerGetNode(instance_info->node_manager_config, hash_next);
        if (fallback_node == NULL)
        {
            LOGF("TrojanAuthServer: fallback node not found");
            exit(1);
        }
        if (fallback_node->instance == NULL)
        {
            nodemanagerRunNode(instance_info->node_manager_config, fallback_node, instance_info->chain_index + 1);
        }
        state->fallback = fallback_node->instance;

        if (state->fallback != NULL)
        {
            state->fallback->dw = t;
        }
    }
    memoryFree(fallback_node_name);
}

tunnel_t *newTrojanAuthServer(node_instance_context_t *instance_info)
{
    trojan_auth_server_state_t *state = memoryAllocate(sizeof(trojan_auth_server_state_t));
    memorySet(state, 0, sizeof(trojan_auth_server_state_t));
    state->users    = hmap_users_t_with_capacity(kVecCap);
    cJSON *settings = instance_info->node_settings_json;

    if (! (cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: TrojanAuthServer->settings (object field) : The object was empty or invalid");
        return NULL;
    }
    tunnel_t *t = tunnelCreate();
    t->state    = state;

    t->upStream   = &upStream;
    t->downStream = &downStream;
    parse(t, settings, instance_info);

    return t;
}

api_result_t apiTrojanAuthServer(tunnel_t *self, const char *msg)
{

    (void) (self);
    (void) (msg);
    return (api_result_t) {0};
}

tunnel_t *destroyTrojanAuthServer(tunnel_t *self)
{
    (void) self;
    return NULL;
}

tunnel_metadata_t getMetadataTrojanAuthServer(void)
{
    return (tunnel_metadata_t) {.version = 0001, .flags = 0x0};
}
