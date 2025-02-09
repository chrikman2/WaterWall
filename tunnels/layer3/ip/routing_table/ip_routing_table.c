#include "ip_routing_table.h"
#include "loggers/network_logger.h"
#include "managers/node_manager.h"
#include "packet_types.h"
#include "utils/jsonutils.h"


enum mode_dynamic_value_status
{
    kDvsSourceMode = kDvsFirstOption,
    kDvsDestMode
};

enum default_action_dynamic_value_status
{
    kDvsDrop = kDvsFirstOption
};

typedef struct
{
    union {
        struct in_addr  ip4;
        struct in6_addr ip6;
    } ip;

    union {
        struct in_addr  mask4;
        struct in6_addr mask6;
    } mask;

    tunnel_t *next;
    bool      v4;

} routing_rule_t;

typedef struct layer3_ip_overrider_state_s
{
    routing_rule_t routes[8];
    int            default_rule;
    bool           default_drop;
    uint8_t        routes_len;

} layer3_ip_overrider_state_t;

typedef struct layer3_ip_overrider_con_state_s
{
    void *_;
} layer3_ip_overrider_con_state_t;

static void upStreamSrcMode(tunnel_t *self, context_t *c)
{
    layer3_ip_overrider_state_t *state = TSTATE(self);

    packet_mask *packet = (packet_mask *) (sbufGetMutablePtr(c->payload));

    if (packet->ip4_header.version == 4)
    {
        for (unsigned int i = 0; i < state->routes_len; i++)
        {
            const struct in_addr addr = {.s_addr = packet->ip4_header.saddr};
            if (state->routes[i].v4 && checkIPRange4(addr, state->routes[i].ip.ip4, state->routes[i].mask.mask4))
            {
                state->routes[i].next->upStream(state->routes[i].next, c);
                return;
            }
        }
    }
    else if (packet->ip6_header.version == 6)
    {
        for (unsigned int i = 0; i < state->routes_len; i++)
        {
            if ((! state->routes[i].v4) &&
                checkIPRange6(packet->ip6_header.saddr, state->routes[i].ip.ip6, state->routes[i].mask.mask6))
            {
                state->routes[i].next->upStream(state->routes[i].next, c);
                return;
            }
        }
    }

    if (state->default_drop)
    {
        LOGD("Layer3IpRoutingTable: dropped a packet that did not match any rule");
        contextReusePayload(c);
        contextDestroy(c);
    }
    else
    {
        state->routes[state->default_rule].next->upStream(state->routes[state->default_rule].next, c);
    }
}

static void upStreamDestMode(tunnel_t *self, context_t *c)
{
    layer3_ip_overrider_state_t *state = TSTATE(self);

    packet_mask *packet = (packet_mask *) (sbufGetMutablePtr(c->payload));

    if (packet->ip4_header.version == 4)
    {
        for (unsigned int i = 0; i < state->routes_len; i++)
        {
            const struct in_addr addr = {.s_addr = packet->ip4_header.saddr};
            if (checkIPRange4(addr, state->routes[i].ip.ip4, state->routes[i].mask.mask4))
            {
                state->routes[i].next->upStream(state->routes[i].next, c);
            }
        }
    }
    else if (packet->ip4_header.version == 6)
    {
        for (unsigned int i = 0; i < state->routes_len; i++)
        {
            if (checkIPRange6(packet->ip6_header.daddr, state->routes[i].ip.ip6, state->routes[i].mask.mask6))
            {
                state->routes[i].next->upStream(state->routes[i].next, c);
            }
        }
    }

    if (state->default_drop)
    {
        LOGD("Layer3IpRoutingTable: dropped a packet that did not match any rule");
        contextReusePayload(c);
        contextDestroy(c);
    }
    else
    {
        state->routes[state->default_rule].next->upStream(state->routes[state->default_rule].next, c);
    }
}

static void downStream(tunnel_t *self, context_t *c)
{
    (void) (self);
    (void) (c);
    assert(false);

    if (c->payload)
    {
        contextReusePayload(c);
    }
    contextDestroy(c);
}

static routing_rule_t parseRule(struct node_manager_config_s *cfg, unsigned int chain_index, const cJSON *rule_obj)
{
    char *temp = NULL;

    if (! getStringFromJsonObject(&(temp), rule_obj, "ip") || ! verifyIPCdir(temp, getNetworkLogger()))
    {
        LOGF("JSON Error: Layer3IpRoutingTable->settings->rules invalid rule");
        exit(1);
    }

    routing_rule_t rule  = {0};
    int            ipver = parseIPWithSubnetMask((struct in6_addr *) &rule.ip, temp, (struct in6_addr *) &rule.mask);
    if (ipver != 4 && ipver != 6)
    {
        LOGF("JSON Error: Layer3IpRoutingTable->settings->rules rule parse failed");
    }
    rule.v4 = ipver == 4;
    memoryFree(temp);
    temp = NULL;

    if (! getStringFromJsonObject(&(temp), rule_obj, "next"))
    {
        LOGF("JSON Error: Layer3IpRoutingTable->settings->rules next tunnel not specified");
        exit(1);
    }
    hash_t  hash_node_name = calcHashBytes(temp, strlen(temp));
    node_t *node           = nodemanagerGetNode(cfg, hash_node_name);

    if (node == NULL)
    {
        LOGF("JSON Error: Layer3IpRoutingTable->settings->rules node %s not found", temp);
        exit(1);
    }

    if (node->instance == NULL)
    {
        nodemanagerRunNode(cfg, node, chain_index + 1);
        if (node->instance == NULL)
        {
            exit(1);
        }
    }
    memoryFree(temp);

    rule.next = node->instance;

    return rule;
}

tunnel_t *newLayer3IpRoutingTable(node_instance_context_t *instance_info)
{
    layer3_ip_overrider_state_t *state = memoryAllocate(sizeof(layer3_ip_overrider_state_t));
    memorySet(state, 0, sizeof(layer3_ip_overrider_state_t));
    cJSON *settings = instance_info->node_settings_json;

    if (! (cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: Layer3IpRoutingTable->settings (object field) : The object was empty or invalid");
        return NULL;
    }

    dynamic_value_t def_action = parseDynamicNumericValueFromJsonObject(settings, "default-action", 1, "drop");

    if (def_action.status == kDvsConstant)
    {
        state->default_drop = false;
        state->default_rule = (int) def_action.status;
    }
    else
    {
        state->default_drop = true;
    }
    dynamicvalueDestroy(def_action);

    dynamic_value_t mode_dv = parseDynamicNumericValueFromJsonObject(settings, "mode", 2, "source-ip", "dest-ip");

    if ((int) mode_dv.status != kDvsDestMode && (int) mode_dv.status != kDvsSourceMode)
    {
        LOGF("Layer3IpRoutingTable: Layer3IpRoutingTable->settings->mode (string field)  mode is not set or invalid, "
             "do you "
             "want to filter based on source ip or dest ip?");
        exit(1);
    }
    dynamicvalueDestroy(mode_dv);


    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(settings, "rules");
    if (! cJSON_IsArray(rules))
    {
        LOGF("JSON Error: Layer3IpRoutingTable->settings->rules (array field) : The arary was empty or invalid");
        exit(1);
    }

    unsigned int i         = 0;
    const cJSON *list_item = NULL;
    cJSON_ArrayForEach(list_item, rules)
    {
        state->routes[i++] = parseRule(instance_info->node_manager_config, instance_info->chain_index, list_item);
    }

    if (i == 0)
    {
        LOGF("Layer3IpRoutingTable: no rules");
        exit(1);
    }
    if (i > ARRAY_SIZE(state->routes))
    {
        LOGF("Layer3IpRoutingTable: too much rules");
        exit(1);
    }
    state->routes_len = i;

    tunnel_t *t = tunnelCreate();

    t->state      = state;
    t->upStream   = ((int) mode_dv.status == kDvsDestMode) ? &upStreamDestMode : &upStreamSrcMode;
    t->downStream = &downStream;

    return t;
}
api_result_t apiLayer3IpRoutingTable(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t) {0};
}

tunnel_t *destroyLayer3IpRoutingTable(tunnel_t *self)
{
    (void) (self);
    return NULL;
}

tunnel_metadata_t getMetadataLayer3IpRoutingTable(void)
{
    return (tunnel_metadata_t) {.version = 0001, .flags = 0x0};
}
