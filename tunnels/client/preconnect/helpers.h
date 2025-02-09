#pragma once
#include "loggers/network_logger.h"
#include "types.h"


enum
{
    kPreconnectDelayShort = 10,
    kPreconnectDelayLong  = 750
};

static void addConnection(thread_box_t *box, preconnect_client_con_state_t *con)
{
    con->next      = box->root.next;
    box->root.next = con;
    con->prev      = &box->root;
    if (con->next)
    {
        con->next->prev = con;
    }
    box->length += 1;
}
static void removeConnection(thread_box_t *box, preconnect_client_con_state_t *con)
{

    con->prev->next = con->next;
    if (con->next)
    {
        con->next->prev = con->prev;
    }
    box->length -= 1;
}

static preconnect_client_con_state_t *createCstate(wid_t tid)
{
    preconnect_client_con_state_t *cstate = memoryAllocate(sizeof(preconnect_client_con_state_t));
    memorySet(cstate, 0, sizeof(preconnect_client_con_state_t));
    cstate->u = newLine(tid);
    return cstate;
}

static void destroyCstate(preconnect_client_con_state_t *cstate)
{
    lineDestroy(cstate->u);
    memoryFree(cstate);
}
static void doConnect(struct connect_arg *cg)
{
    tunnel_t                      *self   = cg->t;
    preconnect_client_con_state_t *cstate = createCstate(cg->tid);
    memoryFree(cg);
    LSTATE_MUT(cstate->u) = cstate;
    self->up->upStream(self->up, contextCreateInit(cstate->u));
}

static void connectTimerFinished(wtimer_t *timer)
{
    doConnect(weventGetUserdata(timer));
    wtimerDelete(timer);
}
static void beforeConnect(wevent_t *ev)
{
    struct connect_arg *cg            = weventGetUserdata(ev);
    wtimer_t           *connect_timer = wtimerAdd(getWorkerLoop(cg->tid), connectTimerFinished, cg->delay, 1);
    if (connect_timer)
    {
        weventSetUserData(connect_timer, cg);
    }
    else
    {
        doConnect(cg);
    }
}

static void initiateConnect(tunnel_t *self, bool delay)
{
    preconnect_client_state_t *state = TSTATE(self);

    if (state->unused_cons >= state->min_unused_cons)
    {
        return;
    }

    wid_t tid = 0;
    if (getWorkersCount() > 0)
    {
        tid = atomicAddExplicit(&(state->round_index), 1, memory_order_relaxed);

        if (tid >= getWorkersCount())
        {
            atomicStoreExplicit(&(state->round_index), 0, memory_order_relaxed);
            tid = 0;
        }
    }

    wloop_t *worker_loop = getWorkerLoop(tid);

    wevent_t            ev = {.loop = worker_loop, .cb = beforeConnect};
    struct connect_arg *cg = memoryAllocate(sizeof(struct connect_arg));
    ev.userdata            = cg;
    cg->t                  = self;
    cg->tid                = tid;
    cg->delay              = delay ? kPreconnectDelayLong : kPreconnectDelayShort;

    wloopPostEvent(worker_loop, &ev);
}
