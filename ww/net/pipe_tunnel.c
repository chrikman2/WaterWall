#include "pipe_tunnel.h"
#include "context.h"
#include "generic_pool.h"
#include "loggers/internal_logger.h"
#include "managers/node_manager.h"
#include "tunnel.h"
#include "loggers/internal_logger.h"

typedef struct pipetunnel_line_state_s
{
    atomic_int    refc;
    atomic_bool   closed;
    _Atomic(wid_t) left_wid;
    _Atomic(wid_t) right_wid;
    bool          active;
    bool          left_open;
    bool          right_open;

} pipetunnel_line_state_t;

typedef struct pipetunnel_msg_event_s
{
    tunnel_t *tunnel;
    context_t ctx;

} pipetunnel_msg_event_t;

size_t pipeLineGetMesageSize(void)
{
    return sizeof(pipetunnel_msg_event_t);
}

static void initializeLineState(pipetunnel_line_state_t *ls, wid_t wid_to)
{
    atomicStoreExplicit(&ls->refc, 0, memory_order_relaxed);
    atomicStoreExplicit(&ls->closed, false, memory_order_relaxed);
    atomicStoreExplicit(&ls->left_wid, getWID(), memory_order_relaxed);
    atomicStoreExplicit(&ls->right_wid, wid_to, memory_order_relaxed);
    ls->active     = true;
    ls->left_open  = true;
    ls->right_open = true;
}

static void deinitializeLineState(pipetunnel_line_state_t *ls)
{
    atomicStoreExplicit(&ls->refc, 0, memory_order_relaxed);
    atomicStoreExplicit(&ls->closed, false, memory_order_relaxed);
    atomicStoreExplicit(&ls->left_wid, 0, memory_order_relaxed);
    atomicStoreExplicit(&ls->right_wid, 0, memory_order_relaxed);
    ls->active     = false;
    ls->left_open  = false;
    ls->right_open = false;
}

static void lock(pipetunnel_line_state_t *ls)
{
    int old_refc = atomicAddExplicit(&ls->refc, 1, memory_order_relaxed);

    (void) old_refc;
}

static void unlock(pipetunnel_line_state_t *ls)
{
    int old_refc = atomicAddExplicit(&ls->refc, -1, memory_order_relaxed);
    if (old_refc == 1)
    {
        deinitializeLineState(ls);
    }
}

static void sendMessageUp(pipetunnel_line_state_t *ls, pipetunnel_msg_event_t *msg, wid_t wid_to);
static void sendMessageDown(pipetunnel_line_state_t *ls, pipetunnel_msg_event_t *msg, wid_t wid_to);

static void onMsgReceivedUp(wevent_t *ev)
{
    pipetunnel_msg_event_t  *msg_ev = weventGetUserdata(ev);
    tunnel_t                *t      = msg_ev->tunnel;
    line_t                  *l      = msg_ev->ctx.line;
    wid_t                    tid    = wloopGetWID(weventGetLoop(ev));
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(t, l);

    if (atomicLoadRelaxed(&(lstate->right_wid)) != getWID())
    {
        sendMessageUp(lstate, msg_ev, atomicLoadRelaxed(&(lstate->right_wid)));
        unlock(lstate);
        return;
    }

    if (! lstate->right_open)
    {
        if (msg_ev->ctx.payload != NULL)
        {
            contextReusePayload(&msg_ev->ctx);
        }
    }
    else
    {
        contextApplyOnTunnelU(&msg_ev->ctx, (tunnel_t *) tunnelGetState(t));
    }
    genericpoolReuseItem(getWorkerPipeTunnelMsgPool(tid), msg_ev);
    unlock(lstate);
}

static void sendMessageUp(pipetunnel_line_state_t *ls, pipetunnel_msg_event_t *msg, wid_t wid_to)
{

    lock(ls);
    // struct msg_event *evdata = genericpoolGetItem(getWorkerPipeTunnelMsgPool(wid_from));
    // *evdata = (struct msg_event){.ls = ls, .function = *(void **) (&fn), .arg = arg, .target_tid = wid_to};

    wevent_t ev;
    memorySet(&ev, 0, sizeof(ev));
    ev.loop = getWorkerLoop(wid_to);
    ev.cb   = onMsgReceivedUp;
    weventSetUserData(&ev, msg);
    wloopPostEvent(getWorkerLoop(wid_to), &ev);
}

static void onMsgReceivedDown(wevent_t *ev)
{
    pipetunnel_msg_event_t  *msg_ev = weventGetUserdata(ev);
    tunnel_t                *t      = msg_ev->tunnel;
    line_t                  *l      = msg_ev->ctx.line;
    wid_t                    tid    = wloopGetWID(weventGetLoop(ev));
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(t, l);

    if (atomicLoadRelaxed(&(lstate->left_wid)) != getWID())
    {
        sendMessageDown(lstate, msg_ev, atomicLoadRelaxed(&(lstate->left_wid)));
        unlock(lstate);
        return;
    }

    if (! lstate->left_open)
    {
        if (msg_ev->ctx.payload != NULL)
        {
            contextReusePayload(&msg_ev->ctx);
        }
    }
    else
    {
        contextApplyOnTunnelD(&msg_ev->ctx, t->dw);
    }
    genericpoolReuseItem(getWorkerPipeTunnelMsgPool(tid), msg_ev);
    unlock(lstate);
}

static void sendMessageDown(pipetunnel_line_state_t *ls, pipetunnel_msg_event_t *msg, wid_t wid_to)
{

    lock(ls);

    wevent_t ev;
    memorySet(&ev, 0, sizeof(ev));
    ev.loop = getWorkerLoop(wid_to);
    ev.cb   = onMsgReceivedUp;
    weventSetUserData(&ev, msg);
    wloopPostEvent(getWorkerLoop(wid_to), &ev);
}

void pipetunnelDefaultUpStreamInit(tunnel_t *self, line_t *line)
{

    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnInitU(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .init = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageUp(lstate, msg, lstate->right_wid);
}

void pipetunnelDefaultUpStreamEst(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnEstU(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .est = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageUp(lstate, msg, lstate->right_wid);
}

void pipetunnelDefaultUpStreamFin(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnFinU(child, line);
        return;
    }

    lstate->left_open = false;

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }

    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .fin = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageUp(lstate, msg, lstate->right_wid);
    unlock(lstate);
}

void pipetunnelDefaultUpStreamPayload(tunnel_t *self, line_t *line, sbuf_t *payload)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnPayloadU(child, line, payload);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        bufferpoolResuesBuffer(getWorkerBufferPool(getWID()), payload);
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .payload = payload};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageUp(lstate, msg, lstate->right_wid);
}

void pipetunnelDefaultUpStreamPause(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnPauseU(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .pause = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageUp(lstate, msg, lstate->right_wid);
}

void pipetunnelDefaultUpStreamResume(tunnel_t *self, line_t *line)
{

    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnResumeU(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .resume = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageUp(lstate, msg, lstate->right_wid);
}

/*
    Downstream
*/

void pipetunnelDefaultdownStreamInit(tunnel_t *self, line_t *line)
{
    (void) self;
    (void) line;
    assert(false); // unreachable code
}

void pipetunnelDefaultdownStreamEst(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnEstD(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }

    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .est = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageDown(lstate, msg, lstate->left_wid);
}

void pipetunnelDefaultdownStreamFin(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnFinD(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .fin = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageDown(lstate, msg, lstate->left_wid);
    unlock(lstate);
}

void pipetunnelDefaultdownStreamPayload(tunnel_t *self, line_t *line, sbuf_t *payload)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnPayloadD(child, line, payload);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        bufferpoolResuesBuffer(getWorkerBufferPool(getWID()), payload);
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .payload = payload};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageDown(lstate, msg, lstate->left_wid);
}

void pipetunnelDefaultDownStreamPause(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnPauseD(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .pause = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageDown(lstate, msg, lstate->left_wid);
}

void pipetunnelDefaultDownStreamResume(tunnel_t *self, line_t *line)
{
    pipetunnel_line_state_t *lstate = (pipetunnel_line_state_t *) lineGetState(self, line);
    tunnel_t                *child  = tunnelGetState(self);

    if (! lstate->active)
    {
        child->fnResumeD(child, line);
        return;
    }

    if (atomicLoadExplicit(&lstate->closed, memory_order_relaxed))
    {
        return;
    }
    pipetunnel_msg_event_t *msg = genericpoolGetItem(getWorkerPipeTunnelMsgPool(getWID()));
    context_t               ctx = {.line = line, .resume = true};

    msg->tunnel = self;
    msg->ctx    = ctx;

    sendMessageDown(lstate, msg, lstate->left_wid);
}

void pipetunnelDefaultOnChain(tunnel_t *t, tunnel_chain_t *tc)
{
    tunnel_t *child = tunnelGetState(t);

    tunnelchainInsert(tc, t);
    tunnelBind(t, child);
    child->onChain(child, tc);
}

void pipetunnelDefaultOnIndex(tunnel_t *t, tunnel_array_t *arr, uint16_t *index, uint16_t *mem_offset)
{
    tunnelarrayInesert(arr, t);
    tunnel_t *child = tunnelGetState(t);

    t->chain_index   = *index;
    t->lstate_offset = *mem_offset;

    *mem_offset += t->lstate_size;
    (*index)++;

    child->onIndex(child, arr, index, mem_offset);
}

void pipetunnelDefaultOnPrepair(tunnel_t *t)
{
    tunnel_t *child = tunnelGetState(t);
    child->onStart(child);
}

void pipetunnelDefaultOnStart(tunnel_t *t)
{
    tunnel_t *child = tunnelGetState(t);
    child->onStart(child);
}

tunnel_t *pipetunnelCreate(tunnel_t *child)
{
    tunnel_t *pt = tunnelCreate(tunnelGetNode(child), tunnelGetStateSize(child) + sizeof(tunnel_t),
                                tunnelGetLineStateSize(child) + sizeof(line_t) + sizeof(pipetunnel_line_state_t));
    if (pt == NULL) {
        // Handle memory allocation failure
        return NULL;
    }

    pt->fnInitU    = &pipetunnelDefaultUpStreamInit;
    pt->fnInitD    = &pipetunnelDefaultdownStreamInit;
    pt->fnPayloadU = &pipetunnelDefaultUpStreamPayload;
    pt->fnPayloadD = &pipetunnelDefaultdownStreamPayload;
    pt->fnEstU     = &pipetunnelDefaultUpStreamEst;
    pt->fnEstD     = &pipetunnelDefaultdownStreamEst;
    pt->fnFinU     = &pipetunnelDefaultUpStreamFin;
    pt->fnFinD     = &pipetunnelDefaultdownStreamFin;
    pt->fnPauseU   = &pipetunnelDefaultUpStreamPause;
    pt->fnPauseD   = &pipetunnelDefaultDownStreamPause;
    pt->fnResumeU  = &pipetunnelDefaultUpStreamResume;
    pt->fnResumeD  = &pipetunnelDefaultDownStreamResume;

    pt->onChain   = &pipetunnelDefaultOnChain;
    pt->onIndex   = &pipetunnelDefaultOnIndex;
    pt->onPrepair = &pipetunnelDefaultOnPrepair;
    pt->onStart   = &pipetunnelDefaultOnStart;

    tunnelSetState(pt, child);

    return pt;
}

void pipetunnelDestroy(tunnel_t *t)
{
    tunnelDestroy(tunnelGetState(t));
    tunnelDestroy(t);
}

void pipeTo(tunnel_t *t, line_t *l, wid_t wid_to)
{
    tunnel_t                *master = (tunnel_t *) (((uint8_t *) t) - sizeof(tunnel_t));
    pipetunnel_line_state_t *ls     = (pipetunnel_line_state_t *) lineGetState(master, l);

    if (ls->active)
    {
        LOGW("double pipe (beta)");
        
        if (atomicLoadExplicit(&ls->closed, memory_order_relaxed))
        {
            return;
        }
        atomicStoreExplicit(&ls->right_wid, wid_to, memory_order_relaxed);
    }
    else
    {
        initializeLineState(ls, wid_to);
    }
    t->fnInitU(t, l);
}
