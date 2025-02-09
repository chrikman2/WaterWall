#pragma once
#include "widle_table.h"
#include "loggers/network_logger.h"
#include "tunnel.h"
#include "types.h"
#include <stdbool.h>

enum
{
    kHandShakeByte               = 0xFF,
    kHandShakeLength             = 96,
    kPreconnectDelayShort        = 10,
    kPreconnectDelayLong         = 750,
    kConnectionStarvationTimeOut = 45000
};

static void onLinePausedU(void *cstate)
{
    pauseLineUpSide(((reverse_client_con_state_t *) cstate)->d);
}

static void onLineResumedU(void *cstate)
{
    resumeLineUpSide(((reverse_client_con_state_t *) cstate)->d);
}
static void onLinePausedD(void *cstate)
{
    pauseLineUpSide(((reverse_client_con_state_t *) cstate)->u);
}

static void onLineResumedD(void *cstate)
{
    resumeLineUpSide(((reverse_client_con_state_t *) cstate)->u);
}

static reverse_client_con_state_t *createCstate(tunnel_t *self, wid_t tid)
{
    reverse_client_con_state_t *cstate = memoryAllocate(sizeof(reverse_client_con_state_t));
    line_t                     *up     = newLine(tid);
    line_t                     *dw     = newLine(tid);
    // reserveChainStateIndex(dw); // we always take one from the down line
    setupLineDownSide(up, onLinePausedU, cstate, onLineResumedU);
    setupLineDownSide(dw, onLinePausedD, cstate, onLineResumedD);
    *cstate = (reverse_client_con_state_t){.u = up, .d = dw, .idle_handle = NULL, .self = self};
    return cstate;
}

static void cleanup(reverse_client_con_state_t *cstate)
{
    if (cstate->idle_handle)
    {
        reverse_client_state_t *state = TSTATE(cstate->self);
        idleTableRemoveIdleItemByHash(cstate->u->tid, state->starved_connections, (hash_t) (size_t) (cstate));
    }
    doneLineDownSide(cstate->u);
    doneLineDownSide(cstate->d);
    lineDestroy(cstate->u);
    lineDestroy(cstate->d);

    memoryFree(cstate);
}
static void doConnect(struct connect_arg *cg)
{
    tunnel_t *self = cg->t;
    // reverse_client_state_t     *state  = TSTATE(self);
    reverse_client_con_state_t *cstate = createCstate(self, cg->tid);
    memoryFree(cg);
    context_t *hello_data_ctx = contextCreate(cstate->u);
    self->up->upStream(self->up, contextCreateInit(cstate->u));

    if (! lineIsAlive(cstate->u))
    {
        contextDestroy(hello_data_ctx);
        return;
    }
    hello_data_ctx->payload = bufferpoolGetLargeBuffer(contextGetBufferPool(hello_data_ctx));
    sbufSetLength(hello_data_ctx->payload, kHandShakeLength);
    memorySet(sbufGetMutablePtr(hello_data_ctx->payload), kHandShakeByte, kHandShakeLength);
    self->up->upStream(self->up, hello_data_ctx);
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

static void initiateConnect(tunnel_t *self, wid_t tid, bool delay)
{
    reverse_client_state_t *state = TSTATE(self);

    if (state->threadlocal_pool[tid].unused_cons_count + state->threadlocal_pool[tid].connecting_cons_count >=
        state->min_unused_cons)
    {
        return;
    }
    state->threadlocal_pool[tid].connecting_cons_count += 1;
    // bool more_delay = state->threadlocal_pool[tid].unused_cons_count <= 0;
    // state->threadlocal_pool[tid].unused_cons_count += 1;

    // int tid = 0;
    // if (workers_count > 0)
    // {
    //     tid = atomicAddExplicit(&(state->round_index), 1, memory_order_relaxed);

    //     if (tid >= workers_count)
    //     {
    //         atomicStoreExplicit(&(state->round_index), 0, memory_order_relaxed);
    //         tid = 0;
    //     }
    // }

    wloop_t *worker_loop = getWorkerLoop(tid);

    wevent_t            ev = {.loop = worker_loop, .cb = beforeConnect};
    struct connect_arg *cg = memoryAllocate(sizeof(struct connect_arg));
    ev.userdata            = cg;
    cg->t                  = self;
    cg->tid                = tid;
    cg->delay              = delay ? kPreconnectDelayLong : kPreconnectDelayShort;

    wloopPostEvent(worker_loop, &ev);
}

static void onStarvedConnectionExpire(idle_item_t *idle_con)
{
    reverse_client_con_state_t *cstate = idle_con->userdata;
    tunnel_t                   *self   = cstate->self;
    reverse_client_state_t     *state  = TSTATE(self);
    if (cstate->idle_handle == NULL)
    {
        // this can happen if we are unlucky and 2 events are passed to eventloop in
        //  a bad order, first connection to peer succeeds and also the starvation cb call
        //  is already in the queue
        assert(cstate->pair_connected);
        return;
    }

    assert(! cstate->pair_connected);

    state->threadlocal_pool[cstate->u->tid].unused_cons_count -= 1;
    LOGW("ReverseClient: a idle connection detected and closed");

    cstate->idle_handle = NULL;
    initiateConnect(self, cstate->u->tid, false);

    context_t *fc = contextCreateFin(cstate->u);
    cleanup(cstate);
    self->up->upStream(self->up, fc);
}
