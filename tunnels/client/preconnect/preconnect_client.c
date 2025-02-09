#include "preconnect_client.h"
#include "helpers.h"
#include "loggers/network_logger.h"
#include "managers/node_manager.h"
#include "types.h"
#include "utils/jsonutils.h"

static void upStream(tunnel_t *self, context_t *c)
{

    preconnect_client_state_t *state = TSTATE(self);
    if (c->payload != NULL)
    {
        preconnect_client_con_state_t *cstate = CSTATE(c);
        switch (cstate->mode)
        {

        case kConnectedDirect:
            self->up->upStream(self->up, c);
            break;

        case kConnectedPair:
            self->up->upStream(self->up, contextSwitchLine(c, cstate->u));
            break;
        case kNotconnected:
        default:
            LOGF("PreConnectClient: invalid value of connection state (memory error?)");
            exit(1);

            break;
        }
    }
    else
    {
        const wid_t   tid     = getWID();
        thread_box_t *this_tb = &(state->workers[tid]);
        if (c->init)
        {

            if (this_tb->length > 0)
            {
                atomicAddExplicit(&(state->unused_cons), -1, memory_order_relaxed);
                atomicAddExplicit(&(state->active_cons), 1, memory_order_relaxed);

                preconnect_client_con_state_t *ucon = this_tb->root.next;
                removeConnection(this_tb, ucon);
                ucon->d       = c->line;
                ucon->mode    = kConnectedPair;
                CSTATE_MUT(c) = ucon;
                self->dw->downStream(self->dw, contextCreateEst(c->line));
                initiateConnect(self, false);
            }
            else
            {
                atomicAddExplicit(&(state->active_cons), 1, memory_order_relaxed);
                preconnect_client_con_state_t *dcon = createCstate(getWID());
                CSTATE_MUT(c)                       = dcon;
                dcon->mode                          = kConnectedDirect;
                self->up->upStream(self->up, c);
                return;
            }
            contextDestroy(c);
        }
        else if (c->fin)
        {
            preconnect_client_con_state_t *dcon = CSTATE(c);
            CSTATE_DROP(c);
            atomicAddExplicit(&(state->active_cons), -1, memory_order_relaxed);

            switch (dcon->mode)
            {
            case kConnectedDirect:
                destroyCstate(dcon);
                self->up->upStream(self->up, c);
                break;

            case kConnectedPair: {
                line_t *u_line = dcon->u;
                LSTATE_DROP(dcon->u);
                context_t *fctx = contextSwitchLine(c, u_line); // created here to prevent destruction of line
                destroyCstate(dcon);
                self->up->upStream(self->up, fctx);
            }

            break;
            case kNotconnected:
            default:
                LOGF("PreConnectClient: invalid value of connection state (memory error?)");
                exit(1);

                break;
            }
        }
    }
}

static void downStream(tunnel_t *self, context_t *c)
{
    preconnect_client_state_t *state = TSTATE(self);
    if (c->payload != NULL)
    {

        preconnect_client_con_state_t *cstate = CSTATE(c);

        switch (cstate->mode)
        {
        case kConnectedDirect:
            self->dw->downStream(self->dw, c);
            break;

        case kConnectedPair:
            self->dw->downStream(self->dw, contextSwitchLine(c, cstate->d));
            break;

        case kNotconnected:
            LOGE("PreConnectClient: this node is not purposed to handle downstream data before pairing");
            // fallthrough
        default:
            LOGF("PreConnectClient: invalid value of connection state (memory error?)");
            exit(1);

            break;
        }
    }
    else
    {
        const unsigned int             tid     = getWID();
        thread_box_t                  *this_tb = &(state->workers[tid]);
        preconnect_client_con_state_t *ucon    = CSTATE(c);

        if (c->fin)
        {
            CSTATE_DROP(c);

            switch (ucon->mode)
            {
            case kConnectedDirect:
                atomicAddExplicit(&(state->active_cons), -1, memory_order_relaxed);
                destroyCstate(ucon);
                self->dw->downStream(self->dw, c);
                initiateConnect(self, true);

                break;

            case kConnectedPair: {
                atomicAddExplicit(&(state->active_cons), -1, memory_order_relaxed);
                line_t *d_line = ucon->d;
                LSTATE_DROP(ucon->d);
                destroyCstate(ucon);
                self->dw->downStream(self->dw, contextSwitchLine(c, d_line));
                initiateConnect(self, false);
            }
            break;

            case kNotconnected:
                if (ucon->prev != NULL)
                {
                    // fin after est
                    atomicAddExplicit(&(state->unused_cons), -1, memory_order_relaxed);
                    removeConnection(this_tb, ucon);
                }
                destroyCstate(ucon);
                contextDestroy(c);
                initiateConnect(self, true);

                break;

            default:
                LOGF("PreConnectClient: invalid value of connection state (memory error?)");
                exit(1);

                break;
            }
            LOGD("PreConnectClient: disconnected, unused: %d active: %d", state->unused_cons, state->active_cons);
        }
        else if (c->est)
        {
            if (ucon->mode == kNotconnected)
            {
                addConnection(this_tb, ucon);
                contextDestroy(c);
                unsigned int unused = atomicAddExplicit(&(state->unused_cons), 1, memory_order_relaxed);
                LOGI("PreConnectClient: connected,    unused: %d active: %d", unused + 1, state->active_cons);
                initiateConnect(self, false);
            }
            else
            {
                self->dw->downStream(self->dw, c);
            }
        }
    }
}

static void startPreconnect(wtimer_t *timer)
{
    tunnel_t                  *self  = weventGetUserdata(timer);
    preconnect_client_state_t *state = TSTATE(self);

    for (unsigned int i = 0; i < getWorkersCount(); i++)
    {
        const size_t cpt = state->connection_per_thread;
        for (size_t ci = 0; ci < cpt; ci++)
        {
            initiateConnect(self, true);
        }
    }

    wtimerDelete(timer);
}

tunnel_t *newPreConnectClient(node_instance_context_t *instance_info)
{
    const size_t start_delay_ms = 150;

    preconnect_client_state_t *state =
        memoryAllocate(sizeof(preconnect_client_state_t) + (getWorkersCount() * sizeof(thread_box_t)));
    memorySet(state, 0, sizeof(preconnect_client_state_t) + (getWorkersCount() * sizeof(thread_box_t)));
    const cJSON *settings = instance_info->node_settings_json;

    getIntFromJsonObject((int *) &(state->min_unused_cons), settings, "minimum-unused");

    state->min_unused_cons       = min(max((getWorkersCount() * (ssize_t) 4), state->min_unused_cons), 128);
    state->connection_per_thread = min(4, state->min_unused_cons / getWorkersCount());

    tunnel_t *t   = tunnelCreate();
    t->state      = state;
    t->upStream   = &upStream;
    t->downStream = &downStream;

    wtimer_t *start_timer = wtimerAdd(getWorkerLoop(0), startPreconnect, start_delay_ms, 1);
    weventSetUserData(start_timer, t);

    return t;
}

api_result_t apiPreConnectClient(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t) {0};
}

tunnel_t *destroyPreConnectClient(tunnel_t *self)
{
    (void) (self);
    return NULL;
}

tunnel_metadata_t getMetadataPreConnectClient(void)
{
    return (tunnel_metadata_t) {.version = 0001, .flags = 0x0};
}
