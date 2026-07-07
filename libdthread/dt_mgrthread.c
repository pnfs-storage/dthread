/*
 * Copyright (c) 2026, Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * dt_mgrthread.c  manager thread interface
 * 12-May-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dt_internal.h"

/*
 * the "mgr" manager thread:
 *  - receives thread op requests from app threads running in this rank
 *     - each app thread has a req structure in the ltab[] for this
 *  - creates internal requests for mgr-generated requests
 *     - internal requests only occur on <mgr,0> for START and JOINWAIT
 *  - incomplete requests are stored in the mgr's pending queue
 *  - receives/sends point-to-point MPI msgs from/to other ranks
 *     - these are fixed sized dtmsg_* structures
 *  - handles ltab allocations for app threads running in this rank
 *  - handles startup, wrapping, and termination of app threads in this rank
 *     - this includes handling the differences between native and
 *       pthread-style app functions (for the pthread-style calls
 *       the mgr uses proc callout to convert dthread_argret_t args
 *       to pthread-style void* args and back).
 *
 * the manager on rank 0 (<mgr,0>) is also responsible for starting
 * the initial main app thread (always runs on rank 0), handling gtab
 * allocations, assigning app threads to ranks, coordinating join requests,
 * and performing orderly shutdowns.
 */

/*
 * internal manager state.  some fields are only used on <mgr,0>.
 */
struct dt_mgrstate {
    /* fields used by managers on all ranks */
    int draining;             /* we are draining sends for shutdown */
    struct reqlist rpending;  /* pending requests waiting for input */
    int ltab_alloc;           /* number of ltab[]s on this rank allocated */
    int ltab_lastslot;        /* last slot in ltab used */

    /* fields only used on <mgr,0> (i.e. the manager on rank 0) */
    int app0exitval;          /* exit() value from app0 thread */
    int threads_alloc;        /* allocated gtab[] slots */
    int threads_starting;     /* incr after start, drop on started reply */
    int threads_running;      /* incr after started reply, drop on terminate */
    int last_slot;            /* last slot in gtab used */
    int next_rank;            /* next rank to round-robin allocate thread on */
};

static struct dt_mgrstate mgrstate = { 0 };

/*
 * args for the main (initial) app thread started by <mgr,0> with app0wrap().
 * the main app thread has main() style argc/argv args.
 */
struct app0args {
    dthread_start_app0_t app0main;
    int argc;
    char **argv;
};

/*
 * call state for a wrapped dthread start function.  used by the app
 * thread wrapper code with pthread_cleanup_push() to pass call state
 * info to the cleanup code.  the pthread_cleanup_push() call occurs
 * prior to calling out to application code.
 */
struct dt_wrapstate {
    dthread_ltab_t *lt;            /* ltab for this thread */
    dthread_dispatch_t *disp;      /* dispatch/proc functions */
    int native;                    /* native-style interface? */
    void *pth_decarg;              /* decoded arg argret for pthread-style */
    void *pth_ret;                 /* return from pthread-style app code */
    int returned;                  /* app code returned to caller */
};

/*
 * prototypes
 */
static void dthreadcleanup(void *arg);
static void *dthreadwrap(void *arg);
static void process_mqe(struct dtq_mpiqentry *mqe, int *morep);
static void process_req(dthread_request_t *rqe, int *morep);

/*
 * recycle a received dtmsg mqe for reuse (e.g. to send a reply back).
 * on error, the mqe can be freed with dtq_mqe_unalloc().  note that
 * all dtmsg structures start with a dtmsg_header.
 * returns pointer to mqe->frame (as a void*).
 */
static void *recycle_dtmsg_mqe(struct dtq_mpiqentry *mqe, void **from,
                               size_t to_len, uint32_t to_op,
                               uint32_t to_xidseq) {

    struct dtmsg_header *hdr = (struct dtmsg_header *) mqe->frame;

    mlog(MGR_DBG, "recycle_dtmsg_mqe: %p [%d,%d] => [%d,%d]", mqe,
         hdr->op, hdr->xidseq, to_op, to_xidseq);

    mqe->flen = to_len;
    /* caller can adjust mqe->peer themselves if needed */
    hdr->op = to_op;
    hdr->xidseq = to_xidseq;

    *from = NULL;       /* invalidate old pointer (to be safe) */
    return(hdr);
}

/* helpful macro frontend to call recycle_dtmsg_mqe() */
#define RECYCLE_DTMSG_MQE(MQE, FROM, TO, OP, XID)                         \
        recycle_dtmsg_mqe(MQE, (void **)&(FROM), sizeof(*(TO)), OP, XID)

/*
 * convert a valid dthread_t give by the app into its gtab pointer.
 * if it is not valid, return NULL.
 */
static dthread_gtab_t *validate_thread(dthread_t *thread) {
    dthread_gtab_t *gt;

    if (thread->dt_index < 0 || thread->dt_index >= dtrs->nmaxthread)
        return(NULL);

    gt = &dtrs->gtab[thread->dt_index];
    if (gt->allocated == 0 || thread->dt_seq != gt->seq)
        return(NULL);

    return(gt);
}

/*
 * find a pending request on mgr's rpending list.  we use the seq and op
 * as the search key.
 */
static struct dthread_request *find_pending_req(int op, uint32_t seq) {
    struct dthread_request *rqe;

    for (rqe = TAILQ_FIRST(&mgrstate.rpending) ; rqe != NULL ;
         rqe = TAILQ_NEXT(rqe, rl)) {
        if (rqe->xidseq == seq && rqe->reqop == op)
            return(rqe);
    }
    return(NULL);
}

/*
 * get next seq for this rank and advance seqsrc.  seq numbers are
 * used as rpc-like transaction ids (xidseq) and to validate that
 * a data structure we are using has not be freed and reused for
 * someone else (e.g. gtab[] entries).  if an op is provided
 * (op != DTOP_NONE) then we ensure that the op/seq chosen is not
 * already on the pending list as we do not want to have more than
 * one req with the same key on the rpending list.
 */
static uint32_t get_free_seq(int op) {
    uint32_t rv;

    while (1) {
        rv = dtrs->seqsrc;
        dtrs->seqsrc += dtrs->mpi_wsize;
        if (dtrs->seqsrc < rv)            /* unlikely wrap */
            dtrs->seqsrc = dtrs->mpi_rank + dtrs->mpi_wsize;
        if (op == DTOP_NONE || find_pending_req(op, rv) == NULL)
            break;
    }

    return(rv);
}

/*
 * find and return the pending request of a given operation type
 * for the xidseq in a message.  used when we receive a reply
 * message to find the matching orig request on the pending list.
 * if a req is found, we remove it from the processing list.
 * we also verify the message length is what we expect.
 * on success we return the req and set the msg pointer.
 * otherwise we return NULL.
 */
static struct dthread_request *find_pending_msgreq(struct dtq_mpiqentry *mqe,
    int pending_op, size_t msgsize, void **msgptr) {
    struct dtmsg_header *hdr = (struct dtmsg_header *) mqe->frame;
    struct dthread_request *rv;

    if (mqe->flen < sizeof(*hdr) || mqe->flen < msgsize ||
        (rv = find_pending_req(pending_op, hdr->xidseq)) == NULL)
        return(NULL);

    /* remove from pending */
    TAILQ_REMOVE(&mgrstate.rpending, rv, rl);

    if (msgptr)
        *msgptr = (void *) mqe->frame;
    return(rv);
}

/*
 * helper macro to call find_pending_msgreq()
 */
#define FIND_PENDING_MSGREQ(MQE, PENDOP, PTR) \
        find_pending_msgreq((MQE), (PENDOP), sizeof(*(PTR)), (void *)&(PTR))

/*
 * search the pending list for a pending JOINWAIT req for a given
 * thread and return it.  JOINWAIT requests only occur on <mgr,0>.
 * if a JOINWAIT is found then we will remove it from the pending
 * list if requested (rm!=0).
 */
static struct dthread_request *joinwait_search(dthread_t *thread, int rm) {
    struct dthread_request *rqe;

    for (rqe = TAILQ_FIRST(&mgrstate.rpending) ; rqe != NULL ;
         rqe = TAILQ_NEXT(rqe, rl)) {

        if (rqe->reqop != DTOP_JOINWAIT)
            continue;
        if (rqe->u.waitthread.dt_index == thread->dt_index &&
            rqe->u.waitthread.dt_seq == thread->dt_seq) {

            if (rm) {
                TAILQ_REMOVE(&mgrstate.rpending, rqe, rl);
            }
            mlog(MGR_DBG, "joinwait_search: <%d,%d> hit (rm=%d)!",
                 thread->dt_index, thread->dt_seq, rm);
            return(rqe);
        }

    }

    mlog(MGR_DBG, "joinwait_search: <%d,%d> miss!", thread->dt_index,
             thread->dt_seq);
    return(NULL);
}

/*
 * send a JOINED to end a JOINWAIT and release the JOINWAIT.
 * all JOINWANTs are handled by <mgr,0>, so only <mgr,0> sends JOINED.
 */
static void joinwait_end_release(struct dthread_request *rqe, int *morep) {
    struct dtq_mpiqentry *mqe;
    struct dtmsg_joined *joined;

    if (rqe->reqop != DTOP_JOINWAIT) {
        mlog(MGR_ERR, "joinwait_end_release: rqe %p !JOINWAIT (%d)", rqe,
             rqe->reqop);
    } else {
        mqe = dtq_mqe_alloc();
        if (!mqe) {
            /* error: caller is going to get stuck in join... */
            mlog(MGR_CRIT, "joinwait_end_release: msg alloc failed!");
        } else {
            joined = (struct dtmsg_joined *) mqe->frame;
            mqe->flen = sizeof(*joined);
            mqe->peer = rqe->reqpeer;
            joined->hdr.op = DTOP_JOINED;
            joined->hdr.xidseq = rqe->xidseq;
            joined->errstatus = rqe->reqerror;
            if (joined->errstatus == 0) {
                /* struct copy */
                joined->joinret = dtrs->gtab[rqe->u.waitthread.dt_index].ret;
            } else {
                joined->joinret.dt_argret_type = DTHREAD_NODATA;
            }
            mlog(MGR_INFO, "joinwait_end_release: JOINED %p to <%d,%d> arg=%d",
                 mqe, mqe->peer, joined->hdr.xidseq,
                 joined->joinret.dt_argret_type);
            mqe = dtq_send_enqueue(mqe);
            if (mqe == NULL) {     /* sent to queue? */
                if (morep)
                    *morep = 1;
            } else {
                /* failed to queue, draindown/shutdown in progress */
                mlog(MGR_CRIT, "joinwait_end_release: enqueue JOINED failed");
                dtq_mqe_unalloc(mqe);
            }
        }
    }

    mlog(MGR_DBG, "joinwait_end_release: release rqe %p", rqe);
    dtq_req_release(rqe);
}

/*
 * cleanup a terminating detached thread's return value (needed
 * if it is using pthread-style void* args). called on <mgr,0>
 * when a detached thread sends a TERMINATED notification.
 */
static void detached_ret_cleanup(dthread_gtab_t *gt) {
    dthread_dispatch_t *disp;
    dthread_proc_pth_t proc;

     /* validate disp_idx */
    if (gt->disp_idx < 1 || gt->disp_idx >= dtrs->ndsps)
        return;

    disp = &dtrs->dsps[gt->disp_idx];   /* get dispatch */

    /* cleanup only required for encoded pthread-style argret return */
    proc = disp->dt_retproc;
    if (proc == NULL)
       return;

    mlog(MGR_DBG, "detached_ret_cleanup: procfree gidx=%zd, disp=%d",
         gt - dtrs->gtab, gt->disp_idx);

    /* free it */
    (*proc)(DTHREAD_PROC_FREE, &gt->ret, NULL);
    gt->ret.dt_argret_type = DTHREAD_NODATA;     /* tidy */
}

/*
 * gtab_alloc: allocate a gtab[] entry and return the allocation's index.
 * <mgr,0> uses this as part of CREATE op for new app threads.
 * returns -1 if the table is full.
 */
static int gtab_alloc(int rank, int disp_idx, dthread_argret_t *argp) {
    int p;
    dthread_gtab_t *gt;

    if (mgrstate.threads_alloc >= dtrs->nmaxthread)
        return(-1);

    p = mgrstate.last_slot;   /* last slot allocated, likely still in use */
    while (1) {
        p++;
        if (p >= dtrs->nmaxthread)
            p = 0;
        if (dtrs->gtab[p].allocated == 0)
            break;
        if (p == mgrstate.last_slot)
            return(-1);
    }

    /* init the state and mark as allocated */
    gt = &dtrs->gtab[p];
    gt->rank = rank;
    gt->seq = get_free_seq(DTOP_NONE);
    gt->disp_idx = disp_idx;
    gt->arg = *argp;
    gt->ret.dt_argret_type = DTHREAD_NODATA;
    gt->detached = gt->terminated = gt->has_ltab = 0;
    gt->allocated = 1;
    mgrstate.threads_alloc++;
    mgrstate.last_slot = p;

    mlog(MGR_INFO, "gtab_alloc: <%d,%d>, rank=%d, disp=%d", p, gt->seq,
         rank, disp_idx);

    return(p);
}

/*
 * gtab_free: free an allocated gtab[] entry.  caller should ensure
 * that the thread is no longer running and does not have a ltab[]
 * allocated on the ran it ran on.  used by <mgr,0> during app thread
 * termination.
 */
static void gtab_free(int gidx) {
    dthread_gtab_t *gt = &dtrs->gtab[gidx];

    if (gt->allocated) {
        mlog(MGR_INFO, "gtab_free: gidx=%d, rank=%d, disp=%d, seq=%d",
             gidx, gt->rank, gt->disp_idx, gt->seq);
        gt->allocated = 0;
        gt->seq = gt->has_ltab = 0;   /* to be safe */
        mgrstate.threads_alloc--;
    }
}

/*
 * ltab_alloc: allocate a ltab[] entry and return the allocation's index.
 * used by <mgr,T> on target rank when doing a START on new app thread.
 * returns -1 if the table is full.
 */
static int ltab_alloc(dthread_t *thread) {
    int p;
    dthread_ltab_t *lt;

    if (mgrstate.ltab_alloc >= dtrs->nmaxthread)
        return(-1);

    if (mgrstate.ltab_alloc == 0) {
        p = 0;                        /* ltab[] is all free, use [0] */
    } else {
        p = mgrstate.ltab_lastslot;   /* last slot allocated, likely in use */
        while (1) {
            p++;
            if (p >= dtrs->nmaxthread)
                p = 0;
            if (dtrs->ltab[p].pth_state == LT_NONE)
                break;
            if (p == mgrstate.ltab_lastslot)
                return(-1);
        }
    }

    /* setup ltab entry */
    lt = &dtrs->ltab[p];
    lt->gtab_idx = thread->dt_index;
    lt->seq = thread->dt_seq;
    lt->pth_state = LT_STARTING;   /* pth not valid yet */
    mgrstate.ltab_alloc++;
    mgrstate.ltab_lastslot = p;
    mlog(MGR_INFO, "ltab_alloc: for <%d,%d> ltab[%d]", lt->gtab_idx,
         lt->seq, p);

    return(p);
}

/*
 * ltab_free: free an allocated ltab[] entry.  caller should ensure
 * that the thread is no longer running (or never ran in the first
 * place, e.g. pthread_create() failed) so that locking is not required.
 * used by <mgr,T> on target rank as part of thread teardown.
 */
static void ltab_free(int lidx) {
    dthread_ltab_t *lt = &dtrs->ltab[lidx];

    if (lt->pth_state != LT_NONE) {   /* to be safe, should be true */
        mlog(MGR_INFO, "ltab_free: for <%d,%d> ltab[%d]", lt->gtab_idx,
             lt->seq, lidx);
        lt->seq = 0;                  /* invalidate it */
        lt->pth_state = LT_NONE;      /* mark free */
        mgrstate.ltab_alloc--;
    }
}

/*
 * routing function for a simple binary broadcast tree on top of
 * MPI ranks.   each rank has 0, 1, or 2 next hops depending on
 * the size of the tree and their location in it.  this function
 * returns the number of children this rank has in the tree.
 * if there are children (i.e. our return value is > 0), their
 * rank numbers are returned in the children[] array.
 */
static int binary_router(int root_rank, int world_size,
                         int my_rank, int children[2]) {
    int my_adjusted_rank, adjusted_child0;

    /* shift all ranks so that root_rank is rank 0 in the adjusted world */
    my_adjusted_rank = my_rank - root_rank;
    if (my_adjusted_rank < 0)
        my_adjusted_rank += world_size;

    adjusted_child0 = (my_adjusted_rank * 2) + 1;
    if (adjusted_child0 >= world_size) {
        mlog(MGR_DBG, "binary_router: root=%d, me=%d, children=[]",
             root_rank, my_rank);
        return(0);    /* no children */
    }
    children[0] = (adjusted_child0 + root_rank) % world_size;
    if (adjusted_child0 == world_size - 1) {
        mlog(MGR_DBG, "binary_router: root=%d, me=%d, children=[%d]",
             root_rank, my_rank, children[0]);
        return(1);    /* one child */
    }
    children[1] = (children[0] + 1) % world_size;
    mlog(MGR_DBG, "binary_router: root=%d, me=%d, children=[%d,%d]",
         root_rank, my_rank, children[0], children[1]);

    return(2);        /* two children */
}

/*
 * broadcast shutdown.   shutdown broadcast is always rooted at rank 0.
 */
static void broadcast_shutdown() {
    int n, children[2], lcv;
    struct dtq_mpiqentry *mqe;
    struct dtmsg_header *hdr;

    n = binary_router(0, dtrs->mpi_wsize, dtrs->mpi_rank, children);
    for (lcv = 0 ; lcv < n ; lcv++) {
        mqe = dtq_mqe_alloc();
        if (!mqe) {
            mlog(MGR_CRIT, "broadcast_shutdown: mqe alloc failed");
            /* skip it, this will trigger a hard shutdown via MPI */
            continue;
        }
        hdr = (struct dtmsg_header *) mqe->frame;
        mqe->flen = sizeof(*hdr);
        mqe->peer = children[lcv];
        hdr->op = DTOP_BCASTSHUTDOWN;
        hdr->xidseq = 0;       /* one way broadcast, so not needed */

        mqe = dtq_send_enqueue(mqe);   /* queue for sending! */
        if (mqe) {
            mlog(MGR_CRIT, "broadcast_shutdown: shutdown mqe queue failed");
            dtq_mqe_unalloc(mqe);
        }
    }

    /* broadcast is queued, start draindown and release request. */
    mlog(MGR_INFO, "broadcast_shutdown: start draindown");
    dtq_send_draindown();    /* XXX log return value? */
    mgrstate.draining = 1;
    dtq_notifytimelimit(10); /* XXX */
}

/*
 * have an app thread do a RETCLEAN request to the local <mgr,T>.
 * used by thread wrapper code, retclean_request is done right
 * before an app thread exits (to trigger <mgr,T> to clean up
 * after it).   the app thread's request should be idle since
 * the thread is about to exit (dthread code never exits a thread
 * in the middle of a request).  also note that dthread blocks
 * cancel() when requests are in progress, so pending cancel-related
 * cleanup calls will be blocked until the request is complete
 * and cancel is unblocked.
 *
 * dthread_cancel() and dthread_exit() ops set pth_state to
 * LT_CANCELED or LT_EXITED, respectively.  returning to the
 * caller will set pth_state to LT_RETURNED.  if the app code
 * mistakenly directly calls pthread_cancel()/pthread_exit()
 * then pth_state will still be in a non-finalized state
 * (e.g. LT_RUNNING) and we have no interface to figure out
 * which pthread API trigged the cleanup.  under normal dthread
 * usage this should not happen, but if it does we set pth_state
 * to finalstate (typically LT_CLEANUP) and have the thread
 * return DTHREAD_CANCEL.
 */
static void retclean_request(dthread_ltab_t *lt, int finalstate) {
    pthread_mutex_lock(&lt->req.reqlock);

    /* if the state hasn't been finalized yet, do it now */
    if (LT_NEED_FINALSTATE(lt->pth_state)) {
        lt->pth_state = finalstate;
    }

    /* set return argret_type to cancel for canceled/cleanup */
    if (lt->pth_state == LT_CANCELED || lt->pth_state == LT_CLEANUP) {
        dtrs->gtab[lt->gtab_idx].ret.dt_argret_type = DTHREAD_CANCELED;
        if (lt->pth_state == LT_CLEANUP) {
            mlog(MGR_INFO, "retclean_request: set CLEANUP for <%d,%d>",
                 lt->gtab_idx, lt->seq);
        }
    }

    if (lt->req.reqstate != DTREQ_IDLE) {
        /* should not happen, print an error if it does */
        mlog(MGR_CRIT, "retclean_request: error: req was not idle (%d)",
              lt->req.reqstate);
    }

    lt->req.reqstate = DTREQ_BUSY;
    lt->req.reqpeer = 0;              /* not used (but clear it to be tidy) */
    lt->req.reqop = DTOP_RETCLEAN;
    lt->req.mqe = NULL;               /* also not used */
    pthread_mutex_unlock(&lt->req.reqlock);


    /*
     * queue req for manager.  RETCLEAN is a one way request, we do
     * not expect a reply.  the caller should pthread_exit() after
     * sending the RETCLEAN.
     */
    mlog(MGR_INFO, "retclean: for <%d,%d> ltab[%zd]", lt->gtab_idx,
             lt->seq, lt - dtrs->ltab);
    dtq_req_enqueue(&lt->req);
}

/*
 * <mgr,0> create thread dtmsg handler, called from process_mqe().
 * caller should free mqe (we do not recyle).
 */
static void create_thread(struct dtq_mpiqentry *mqe, int *morep) {
    struct dtmsg_create *cre = (struct dtmsg_create *) mqe->frame;
    int errstatus, detachit, target;
    void *stk_addr;
    size_t stk_size;
    struct dthread_request *startreq;
    struct dtq_mpiqentry *startmqe;
    int gidx;
    struct dtmsg_start *start;

    /* validate dispatch index and check for a free thread slot */
    errstatus = 0;
    if (cre->disp_idx == 0 || cre->disp_idx >= dtrs->ndsps) {
        mlog(MGR_ERR, "mgr:create_thread: bad disp_idx %d", cre->disp_idx);
        errstatus = EINVAL;
    } else if (mgrstate.threads_alloc >= dtrs->nmaxthread) {
        mlog(MGR_CRIT, "mgr:create_thread: hit max# threads (%d)",
             dtrs->nmaxthread);
        errstatus = EAGAIN;
    }
    if (errstatus)
        goto send_created_error;

    /* we manage detach at <mgr,0>, no need to detach underlying thread */
    detachit = PTHREAD_CREATE_JOINABLE;
    pthread_attr_getdetachstate(&cre->attrs, &detachit);
    if (detachit == PTHREAD_CREATE_DETACHED) {
        pthread_attr_setdetachstate(&cre->attrs, PTHREAD_CREATE_JOINABLE);
    }

    /* ensure we are not trying to set the stackaddr */
    if (pthread_attr_getstack(&cre->attrs, &stk_addr, &stk_size) == 0 ||
        stk_addr != NULL) {
        pthread_attr_setstack(&cre->attrs, NULL, stk_size);
    }

    mlog(MGR_INFO, "mgr0: CREATE: srcrank=%d, seq=%d, disp=%d", mqe->peer,
         cre->hdr.xidseq, cre->disp_idx);

    /* choose target rank using round robin */
    target = mgrstate.next_rank;
    mgrstate.next_rank = (mgrstate.next_rank + 1) % dtrs->mpi_wsize;

    /* allocate start internal req, mqe, and gidx for thread */
    startreq = dtq_req_alloc();
    startmqe = (startreq) ? dtq_mqe_alloc() : NULL;
    gidx = (startmqe) ? gtab_alloc(target, cre->disp_idx, &cre->arg) : -1;
    if (gidx == -1) {
        errstatus = ENOMEM;
        goto deallocate_and_error;
    }

    /* init START mqe */
    start = (struct dtmsg_start *)startmqe->frame;
    startmqe->flen = sizeof(*start);
    startmqe->peer = target;
    start->hdr.op = DTOP_START;
    start->hdr.xidseq = get_free_seq(DTOP_START);
    start->thread.dt_index = gidx;
    start->thread.dt_seq = dtrs->gtab[gidx].seq;
    start->attrs = cre->attrs;

    /* init START req */
    startreq->reqpeer = startmqe->peer;
    startreq->reqop = start->hdr.op;
    startreq->mqe = NULL;
    startreq->xidseq = start->hdr.xidseq;
    startreq->reqerror = 0;

    /* info we save so we can make a CREATED msg later */
    startreq->u.creinfo.c_thread = start->thread;  /* struct copy */
    startreq->u.creinfo.c_peer = mqe->peer;
    startreq->u.creinfo.c_xidseq = cre->hdr.xidseq;

    mlog(MGR_INFO, "mgr0: START on %d: seq=%d, <%d,%d>, rqe=%p on %d, seq=%d",
         target, start->hdr.xidseq, gidx, dtrs->gtab[gidx].seq, startreq,
         mqe->peer, cre->hdr.xidseq);

    /* enqueue for sending */
    startmqe = dtq_send_enqueue(startmqe);

    if (startmqe == NULL) {
        /* success!  req now pending waiting for STARTED reply */
        mgrstate.threads_starting++;
        *morep = 1;
        TAILQ_INSERT_TAIL(&mgrstate.rpending, startreq, rl);
        return;               /* done! */
    }

    /* send enqueue failed: free send error (also likely to fail) */
    mlog(MGR_CRIT, "mgr0: CREATE: unable to queue START, failed");
    errstatus = EIO;

deallocate_and_error:  /* falls through to send_created_error ... */
    if (startreq)
        dtq_req_release(startreq);
    if (startmqe)
        dtq_mqe_unalloc(startmqe);
    if (gidx != -1)
        gtab_free(gidx);

send_created_error: {
        struct dtq_mpiqentry *errmqe;
        struct dtmsg_created *created;
        errmqe = dtq_mqe_alloc();
        if (!errmqe) {
            mlog(MGR_CRIT, "mgr0: CREATE: unable to alloc err mqe");
        } else {
            created = (struct dtmsg_created *) errmqe->frame;
            errmqe->flen = sizeof(*created);
            errmqe->peer = mqe->peer;
            created->hdr.op = DTOP_CREATED;
            created->hdr.xidseq = cre->hdr.xidseq;
            created->errstatus = errstatus;
            created->newthread.dt_index = dtrs->nmaxthread + 1; /* invalid */
            errmqe = dtq_send_enqueue(errmqe);
            if (errmqe) {
                mlog(MGR_CRIT, "mgr0: CREATE: enqueue CREATED failed!");
                dtq_mqe_unalloc(errmqe);
            } else {
                mlog(MGR_INFO, "mgr: CREATE: sent CREATED to %d seq=%d,e=%d",
                     mqe->peer, cre->hdr.xidseq, errstatus);
                *morep = 1;
            }
        }
    }
    return;
}

/*
 * <mgr,T> start thread dtmsg handler, called from process_mqe()
 * when <mgr,0> sends us a start dtmsg to start an app thread.
 * caller should free mqe (we do not recyle).
 */
static void start_thread(struct dtq_mpiqentry *mqe, int *morep) {
    struct dtmsg_start *start = (struct dtmsg_start *) mqe->frame;
    struct dtq_mpiqentry *startedmqe;
    struct dtmsg_started *started;
    int lidx;

    /* we always send a started message, so allocate it first */
    startedmqe = dtq_mqe_alloc();
    if (!startedmqe) {
        /* no recovery possible.  log and keep going */
        mlog(MGR_CRIT, "mgr: start_thread: unable to alloc STARTED mqe");
        return;
    }
    started = (struct dtmsg_started *) startedmqe->frame;
    startedmqe->flen = sizeof(*started);
    startedmqe->peer = 0;
    started->hdr.op = DTOP_STARTED;
    started->hdr.xidseq = start->hdr.xidseq;
    started->errstatus = 0;                   /* assume success */

    /* validate start args */
    if (start->thread.dt_index >= dtrs->nmaxthread ||
        start->thread.dt_seq != dtrs->gtab[start->thread.dt_index].seq ||
        dtrs->gtab[start->thread.dt_index].allocated == 0 ||
        dtrs->gtab[start->thread.dt_index].disp_idx == 0 ||
        dtrs->gtab[start->thread.dt_index].disp_idx >= dtrs->ndsps ||
        dtrs->gtab[start->thread.dt_index].has_ltab) {

        mlog(MGR_ERR, "mgr:start_thread: invalid start thread");
        started->errstatus = EINVAL;
        goto done;
    }

    lidx = ltab_alloc(&start->thread);     /* pth_state is LT_STARTING */

    mlog(MGR_INFO, "start_thread: create pthread for <%d,%d>, lidx=%d",
         start->thread.dt_index, start->thread.dt_seq, lidx);

    /* now we can created the dthread's pthread */
    started->errstatus = pthread_create(&dtrs->ltab[lidx].pth, &start->attrs,
                                        dthreadwrap, &dtrs->ltab[lidx]);

    /*
     * if pthread_create failed, drop the ltab and just send the errstatus.
     * otherwise dthreadwrap() in the new pthread will finish the startup.
     */
    if (started->errstatus) {
        ltab_free(lidx);
    }

done:
    mlog(MGR_INFO, "mgr: STARTED <%d,%d> seq=%d, errstatus=%d",
         start->thread.dt_index, start->thread.dt_seq, started->hdr.xidseq,
         started->errstatus);

    startedmqe = dtq_send_enqueue(startedmqe);
    if (startedmqe) {
        /* thread will be stuck in starting state */
        mlog(MGR_CRIT, "mgr:start_thread: err send enqueue failed!");
        dtq_mqe_unalloc(startedmqe);
    } else {
        *morep = 1;
    }
    return;
}

/*
 * dthread wrapper for dthreads created by the application.  this
 * is the pthread start_routine for app threads created by <mgr,T>
 * with the start_thread().
 */
void *dthreadwrap(void *arg) {
    int oldcanstate, lidx, gidx, disp_idx, rv;
    struct dt_wrapstate ws;

    /* block cancel in new thread and recover state */
    oldcanstate = PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    ws.lt = arg;
    lidx = ws.lt - dtrs->ltab;

    /* sanity check args, these checks should never fail */
    if (lidx < 0 || lidx >= dtrs->nmaxthread) {
        mlog(MGR_CRIT, "dthreadwrap: ERROR - bad lidx %d", lidx);
        return(NULL);
    }
    gidx = ws.lt->gtab_idx;
    if (gidx < 0 || gidx >= dtrs->nmaxthread) {
        mlog(MGR_CRIT, "dthreadwrap: ERROR - bad gtab_idx %d", gidx);
        return(NULL);
    }
    if (dtrs->gtab[gidx].seq != ws.lt->seq) {
        mlog(MGR_CRIT, "dthreadwrap: ERROR - ltab/gtab seq mismatch!");
        return(NULL);
    }

    /*
     * from this point on if we get an unexpected error we convert
     * it to being canceled.   in all cases when we are done we a
     * post RETCLEAN to <mgr,T> and pthread_exit.   the mgr will finish
     * processing for this thread and generate a TERMINATED msg
     * for <mgr,0>.
     */

    /* setup the TLS to track our ltab entry */
    if (pthread_setspecific(dtrs->ltabkey, ws.lt)) {
        mlog(MGR_CRIT, "dthreadwrap: ERROR - pthread_setspecific failed?");
        retclean_request(ws.lt, LT_CANCELED);   /* unlikely */
        return(NULL);
    }

    /* get dispatch, note dsps[0] is reserved for app0main */
    disp_idx = dtrs->gtab[gidx].disp_idx;
    if (disp_idx < 1 || disp_idx >= dtrs->ndsps) {
        mlog(MGR_ERR, "dthreadwrap: disp_idx out of range (%d)", disp_idx);
        retclean_request(ws.lt, LT_CANCELED);   /* unlikely */
        return(NULL);
    }
    ws.disp = &dtrs->dsps[disp_idx];
    ws.native = (ws.disp->dt_argproc == NULL && ws.disp->dt_retproc == NULL);
    ws.returned = 0;

    /* if pthread-style interface: decode the arg argret to void* */
    if (!ws.native) {
        mlog(MGR_INFO, "dthreadwrap: pth-syle args <%d,%d>, disp=%d",
             dtrs->ltab[lidx].gtab_idx, dtrs->ltab[lidx].seq, disp_idx);
        rv = (*ws.disp->dt_argproc)(DTHREAD_PROC_DECODE, &dtrs->gtab[gidx].arg,
                                    &ws.pth_decarg);
        if (rv) {
            mlog(MGR_ERR, "dthreadwrap: proc_pth_t decode failed (%d)",
                 disp_idx);
            retclean_request(ws.lt, LT_CANCELED);
            return(NULL);
        }
        ws.pth_ret = NULL;
    } else {
        ws.pth_decarg = ws.pth_ret = NULL;    /* not used w/native */
    }

    /* lock, set state to running, update gtab[], unlock */
    pthread_mutex_lock(&ws.lt->req.reqlock);
    dtrs->gtab[gidx].ltab_idx = lidx;
    dtrs->gtab[gidx].has_ltab = 1;
    ws.lt->pth_state = LT_RUNNING;
    pthread_mutex_unlock(&ws.lt->req.reqlock);

    /* establish a cleanup function for thread exit/cancel */
    pthread_cleanup_push(dthreadcleanup, &ws);

    /* unblock cancel and call user code, then reblock */
    pthread_setcancelstate(oldcanstate, NULL);

    if (ws.native) {
        dthread_start_t st = ws.disp->st.start;

        mlog(MGR_INFO, "dthreadwrap: APP RUN <%d,%d>, disp=%d, style=native",
             dtrs->ltab[lidx].gtab_idx, dtrs->ltab[lidx].seq, disp_idx);

        /* we call out to app code w/native-style interface here! */
        dtrs->gtab[gidx].ret = (*st)(&dtrs->gtab[gidx].arg);
    } else {
        dthread_start_pth_t pst = ws.disp->st.pthstart;

        mlog(MGR_INFO, "dthreadwrap: APP RUN <%d,%d>, disp=%d, style=pth",
             dtrs->ltab[lidx].gtab_idx, dtrs->ltab[lidx].seq, disp_idx);

        /* we call out to app code w/pthread-style interface here! */
        ws.pth_ret = (*pst)(ws.pth_decarg);

    }

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    mlog(MGR_INFO, "dthreadwrap: APP returned <%d,%d>, disp=%d",
         dtrs->ltab[lidx].gtab_idx, dtrs->ltab[lidx].seq, disp_idx);

    ws.returned = 1;         /* tell cleanup we returned to caller */
    pthread_cleanup_pop(1);  /* run and pop cleanup routine */

    /*
     * the cleanup routine has wrapped things up and sent a RETCLEAN
     * request to our manager.  return NULL to finish the pthread.
     */
    return(NULL);
}

/*
 * the dthread cleanup routine is called when a local dthread terminates.
 * this can happen in multiple ways:
 *  - dthread_exit() calls pthread_exit()
 *     => dthread sets pth_state to LT_EXITED before pthread_exit()
 *  - mgr calls pthread_cancel() to handle a dthread_cancel() op
 *     => mgr will set pth_state to LT_CANCELED before canceling
 *  - app returned to caller and we call pthread_cleanup_pop(1)
 *     => we set ws.returned to 1 before calling pthread_cleanup_pop(1)
 *  - app directly calls pthread exit/cancel without using dthread API
 *     => pth_state will be LT_RUNNING, we set it to LT_CLEANUP
 *        and treat it as an unexpected cancel operation
 *
 * in all cases we need to finish up processing, post a RETCLEAN
 * request to <mgr,T>, and return.   we expect the app pthread
 * to exit after the cleanup.
 */
static void dthreadcleanup(void *arg) {
    struct dt_wrapstate *wsp = arg;
    int rv, finalstate;

    /* determine final state to use if pth_state not already set */
    if (wsp->returned)
        finalstate = LT_RETURNED; /* app returned to caller */
     else
        finalstate = LT_CLEANUP;  /* catch direct pthread exit/cancel calls */

    mlog(MGR_INFO, "dthreadwrap: APP wrap cleanup <%d,%d>, native=%d",
         wsp->lt->gtab_idx, wsp->lt->seq, wsp->native);

    /* pthread-style needs to do more arg cleanup */
    if (!wsp->native) {
        /* free the input args (ignore errors) */
        (wsp->disp->dt_argproc)(DTHREAD_PROC_FREE,
                                &dtrs->gtab[wsp->lt->gtab_idx].arg,
                                &wsp->pth_decarg);

        /* if we returned to caller, encode result */
        if (wsp->returned) {
            rv = (wsp->disp->dt_retproc)(DTHREAD_PROC_ENCODE,
                                         &dtrs->gtab[wsp->lt->gtab_idx].ret,
                                         &wsp->pth_ret);
            if (rv) {
                /* encode failed, log and cvt to cancel */
                mlog(MGR_ERR, "dthreadcleanup: encode failed?!");
                dtrs->gtab[wsp->lt->gtab_idx].ret.dt_argret_type =
                    DTHREAD_CANCELED;
                finalstate = LT_CANCELED;  /* make it a cancel */
            }
        }
    }     /* !native */

    /*
     * thread makes a final RETCLEAN request (does not generate a reply)
     * and terminates.  mgr will get the RETCLEAN and continue the teardown
     * and send a TERMINATED to <mgr,0>.   the finalstate value we provide
     * is only used if a final state is still needed.  if the finalstate
     * is not needed (because it is already set) then retclean_request()
     * leaves the current in pth_state as-is and ignores the finalstate arg.
     */
    retclean_request(wsp->lt, finalstate);
}

/*
 * <mgr,0> detach thread dtmsg handler, called from process_mqe().
 * we recycle the mqe, so the caller should NOT release it.
 */
static void detach_thread(struct dtq_mpiqentry *mqe, int *morep) {
    struct dtmsg_detach *detach = (struct dtmsg_detach *) mqe->frame;
    struct dtmsg_detached *detached;
    dthread_gtab_t *gt;
    dthread_request_t *rqe;

    gt = validate_thread(&detach->thread);
    rqe = NULL;
    if (gt) {
        if (gt->detached) {
            /* undefined behavior, we let it go through */
            mlog(MGR_NOTE, "mgr: DETACH <%d,%d> f=%d: already detached",
                 detach->thread.dt_index, detach->thread.dt_seq, mqe->peer);
        }
        gt->detached = 1;

        /* see if join was pending for thread we just detached */
        rqe = joinwait_search(&detach->thread, 1);
    }

    /* recycle detach into detached, mqe->peer value is already ok */
    detached = RECYCLE_DTMSG_MQE(mqe, detach, detached, DTOP_DETACHED,
                                 detach->hdr.xidseq);
    detached->errstatus = (gt == NULL) ? ESRCH : 0;

    if (gt) {
        mlog(MGR_INFO, "mgr: DETACH <%zd,%d> errstatus=%d, peer=%d",
             gt - dtrs->gtab, gt->seq, detached->errstatus, mqe->peer);
    } else {
        mlog(MGR_INFO, "mgr: DETACH <> errstatus=%d, peer=%d",
             detached->errstatus, mqe->peer);
    }

    mqe = dtq_send_enqueue(mqe);
    if (mqe) {
        mlog(MGR_CRIT, "mgr: detach_thread: send enqueue err failed, dropping");
        dtq_mqe_unalloc(mqe);
    } else {
        *morep = 1;
    }

    /*
     * was there a pending JOINWAIT on thread we just detached?
     * in pthreads what happens to the JOIN is undefined behavior.
     * we will kill the pending JOINWAIT with EINVAL.
     */
    if (rqe) {
        mlog(MGR_INFO, "mgr: DETACH: <%zd,%d> had a joinwait to cancel",
             gt - dtrs->gtab, gt->seq);
        rqe->reqerror = EINVAL;
        joinwait_end_release(rqe, morep);
    }

    return;
}

/*
 * <mgr,T> cancel thread dtmsg handler, called from process_mqe().
 * we recycle the mqe, so the caller should NOT release it.
 */
static void cancel_thread(struct dtq_mpiqentry *mqe, int *morep) {
    struct dtmsg_cancel *cancel = (struct dtmsg_cancel *) mqe->frame;
    struct dtmsg_canceled *canceled;
    dthread_gtab_t *gt;
    int errstatus = 0;
    dthread_ltab_t *lt;

    mlog(MGR_INFO, "mgr: CANCEL <%d,%d> seq=%d, attempt",
         cancel->thread.dt_index, cancel->thread.dt_seq,
         cancel->hdr.xidseq);

    gt = validate_thread(&cancel->thread);
    if (gt == NULL) {
        errstatus = ESRCH;
    } else if (gt->rank != dtrs->mpi_rank) {
        errstatus = EINVAL;   /* cannot cancel a thread we do not manage */
    } else if (gt->has_ltab &&
          gt->ltab_idx >= 0 && gt->ltab_idx < dtrs->nmaxthread &&
          dtrs->ltab[gt->ltab_idx].seq == gt->seq) {
        lt = &dtrs->ltab[gt->ltab_idx];

        /* attempt to cancel underlying pthread */
        pthread_mutex_lock(&lt->req.reqlock);
        if (lt->pth_state == LT_RUNNING) {
            pthread_cancel(lt->pth);    /* triggers DTOP_RETCLEAN/terminate */
            lt->pth_state = LT_CANCELED;
        }
        pthread_mutex_unlock(&lt->req.reqlock);
    }

    /* recycle cancel into canceled, mqe->peer value is already ok */
    canceled = RECYCLE_DTMSG_MQE(mqe, cancel, canceled, DTOP_CANCELED,
                                 cancel->hdr.xidseq);
    canceled->errstatus = errstatus;

    mlog(MGR_INFO, "mgr: CANCELED seq=%d, errstatus=%d", 
         canceled->hdr.xidseq, errstatus);

    mqe = dtq_send_enqueue(mqe);
    if (mqe) {
        mlog(MGR_CRIT, "mgr: cancel_thread: send enqueue err failed, drop");
        dtq_mqe_unalloc(mqe);
    } else {
        *morep = 1;
    }

    return;
}

/*
 * <mgr,0> join thread dtmsg handler, called from process_mqe().
 * we recycle the mqe, so the caller should NOT release it.
 */
static void join_thread(struct dtq_mpiqentry *mqe, int *morep) {
    struct dtmsg_join *join = (struct dtmsg_join *) mqe->frame;
    struct dtmsg_joined *joined;
    dthread_gtab_t *gt;
    dthread_request_t *prev, *rqe;

    mlog(MGR_INFO, "mgr: JOIN <%d,%d> seq=%d, attempt",
         join->thread.dt_index, join->thread.dt_seq,
         join->hdr.xidseq);

    gt = validate_thread(&join->thread);

    /* case 1: invalid thread or detached thread => JOINED error */
    if (gt == NULL || gt->detached) {
        joined = RECYCLE_DTMSG_MQE(mqe, join, joined, DTOP_JOINED,
                                   join->hdr.xidseq);
        joined->errstatus = (gt == NULL) ? ESRCH : EINVAL;
        joined->joinret.dt_argret_type = DTHREAD_NODATA;   /* tidy */

        mlog(MGR_INFO, "mgr: JOINED case 1, seq=%d, errstatus=%d",
             joined->hdr.xidseq, joined->errstatus);

        mqe = dtq_send_enqueue(mqe);
        if (mqe) {
            mlog(MGR_CRIT, "mgr: join_thread: send enqueue err failed, drop");
            dtq_mqe_unalloc(mqe);
        } else {
            *morep = 1;
        }
        return;
    }

    /* case 2: thread is a zombie and can be joined now */
    if (!LT_NEED_FINALSTATE(gt->terminated)) {
        joined = RECYCLE_DTMSG_MQE(mqe, join, joined, DTOP_JOINED,
                                   join->hdr.xidseq);
        joined->errstatus = 0;
        joined->joinret = gt->ret;   /* structure copy */

        mlog(MGR_INFO, "mgr: JOINED case 2, seq=%d", joined->hdr.xidseq);
        mqe = dtq_send_enqueue(mqe);
        if (mqe) {
            mlog(MGR_CRIT, "mgr: join_thread: send enqueue joined failed");
            dtq_mqe_unalloc(mqe);
        } else {
            *morep = 1;
        }
        gtab_free( gt - &dtrs->gtab[0] );
        return;
    }

    /* case 3: thread still running: try making internal JOINWAIT req */

    /* make sure there isn't a previous joinwait already running */
    prev = joinwait_search(&join->thread, 0);
    rqe = (prev) ? NULL : dtq_req_alloc();   /* alloc only if no prev */

    if (rqe) {
        rqe->reqpeer = mqe->peer;
        rqe->reqop = DTOP_JOINWAIT;
        rqe->mqe = NULL;
        rqe->xidseq = join->hdr.xidseq;
        rqe->reqerror = 0;
        rqe->u.waitthread = join->thread;    /* struct copy */
        TAILQ_INSERT_TAIL(&mgrstate.rpending, rqe, rl);
        mlog(MGR_INFO, "mgr: JOINED case 3, joinwait <%d,%d> seq=%d, rqe=%p",
             join->thread.dt_index, join->thread.dt_seq, rqe->xidseq, rqe);

        /* not recycling received join mqe, just release it */
        dtq_recv_release(mqe);
        return;
    }

    /*
     * failed, need to send a joined with an error code.
     * either we have prev joinwait (EINVAL) or rqe alloc failed (ENOMEM)
     */
    joined = RECYCLE_DTMSG_MQE(mqe, join, joined, DTOP_JOINED,
                               join->hdr.xidseq);
    joined->errstatus = (prev) ? EINVAL : ENOMEM;
    joined->joinret.dt_argret_type = DTHREAD_NODATA;   /* tidy */

    mlog(MGR_INFO, "mgr: JOINED case 3err, seq=%d, err=%d",
         joined->hdr.xidseq, joined->errstatus);

    mqe = dtq_send_enqueue(mqe);
    if (mqe) {
        mlog(MGR_CRIT, "mgr: join_thread: send enqueue 3e joined err failed");
        dtq_mqe_unalloc(mqe);
    } else {
        *morep = 1;
    }

    return;
}

/*
 * <mgr,0> thread termination dtmsg handler.  called from process_mqe().
 */
static void terminated_thread(dthread_t *thr, uint32_t finstate, int *morep) {
    dthread_gtab_t *gt = &dtrs->gtab[thr->dt_index];
    struct dthread_request *rqe;

    mlog(MGR_INFO, "terminated_thread <%d,%d>, detached=%d",
         thr->dt_index, thr->dt_seq, gt->detached);

    /* make it a zombie by setting terminated.  ensure ltab is detached. */
    gt->terminated = finstate;
    mgrstate.threads_running--;    /* set terminated, no longer running */
    gt->has_ltab = 0;              /* retclean should have already done this */

    /* look for and remove any joinwait req waiting on this thread */
    rqe = joinwait_search(thr, 1);

    /* generate a JOINED if there was JOINWAIT request pending */
    if (rqe) {
        if (gt->detached) {           /* undefined case in pthreads */
            rqe->reqerror = EINVAL;   /* resolve by returning an error */
        }
        mlog(MGR_INFO, "terminated_thread <%d,%d> has a joinwait to release",
             thr->dt_index, thr->dt_seq);
        joinwait_end_release(rqe, morep);
    }

    if (gt->detached) {
        /* cleanup ret value for detached thread (e.g. for pthread-style) */
        detached_ret_cleanup(gt);
    }

    /*
     * if we are detached or if we were able to queue a JOINED then
     * we no longer need the zombie around and can free the gtab.
     * otherwise we need to leave the zombie gtab entry around until
     * the app joins it.
     */
    if (gt->detached || rqe != NULL) {
        mlog(MGR_INFO, "terminated_thread <%d,%d>: no zombie needed",
         thr->dt_index, thr->dt_seq);
        gtab_free(thr->dt_index);     /* no need for zombie */
    } else {
        mlog(MGR_INFO, "terminated_thread <%d,%d>: leaving zombie for join",
         thr->dt_index, thr->dt_seq);
    }
    return;
}

/*
 * app0cleanup runs on rank0 in the main app thread's context (app0).
 * it is called when the app0 thread calls dthread_exit()/pthread_exit()
 * or is terminated by a cancel operation.
 */
static void app0cleanup(void *arg) {
    /* arg points to app0args, but we do not need it */
    dthread_ltab_t *lt = &dtrs->ltab[0];  /* we are app0, so it's [0] */

    mlog(MGR_INFO, "mgr0: app0 cleanup routine triggered!");

    /* disable cancel, thread is already ending */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    /*
     * use retclean_request() to let the mgr know app0 is done.
     * app0 does not require any special handling for this case.
     */
    retclean_request(lt, LT_CLEANUP);

    return;   /* pthread terminates here, mgr handles DTOP_RETCLEAN */
}

/*
 * wrapper for app0start routine.  we are running on rank 0 in a new
 * pthread launched by mgr_startapp() to be the main app thread (app0).
 * we do some basic setup and turn control over to the app0main function.
 * we want to catch cases where the app returns to us, calls dthread_exit(),
 * or gets hit by dthread_cancel().   if the app returns to us, we want to
 * kill any remaining threads and trigger an orderly shutdown of the MPI
 * job.  if the app does a dthread exit/cancel, then we just terminate the
 * app0 thread and keep running (as long other threads are running).
 */
static void *app0wrap(void *arg) {
    struct app0args *ap;
    dthread_ltab_t *lt = &dtrs->ltab[0];  /* we are app0, so it's [0] */

    mlog(MGR_INFO, "mgr0: app0 wrapper running");

    ap = (struct app0args *)dtrs->gtab[lt->gtab_idx].arg.u.dt_inline;

    /* establish a cleanup function for thread exit/cancel */
    pthread_cleanup_push(app0cleanup, ap);

    /* setup the TLS to track our ltab entry */
    if (pthread_setspecific(dtrs->ltabkey, lt)) {
        /* unable to set TSL key -- this is unlikely but fatal... */
        mlog(MGR_CRIT, "pthread_setspecific: unable to set ltabkey");
        pthread_exit(NULL);   /* bail out!  cleanup will catch this. */
    }

    /* give control to the app! */
    mgrstate.app0exitval = ap->app0main(ap->argc, ap->argv);

    /* disable cancel, thread is already ending */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    /* pop off the cleanup function from stack (do not run it now) */
    pthread_cleanup_pop(0);

    /* set state to returned and end the thread by returning NULL */
    pthread_mutex_lock(&lt->req.reqlock);
    lt->pth_state = LT_RETURNED;

    mlog(MGR_INFO, "mgr0: app0 returned to caller");

    /*
     * if the app0 main thread returns to us, we want to kill all
     * remaining running threads and shutdown the MPI job.  this
     * matches what pthreads does when the main thread returns
     * (it does a posix exit() with the int return value).  we want
     * to do this from the context of the manager thread, so we
     * send it a DTOP_APP0_RET request and terminate our thread
     * by returning.
     */
    if (lt->req.reqstate != DTREQ_IDLE) {
        /* log, but keep going and hope for the best */
        mlog(MGR_CRIT, "app0wrap: req was not idle as expected (%d)",
             lt->req.reqstate);
    }
    lt->req.reqstate = DTREQ_BUSY;
    lt->req.reqpeer = 0;                 /* sending it to ourselves */
    lt->req.reqop = DTOP_APP0_RET;
    lt->req.mqe = NULL;                  /* to be safe */
    pthread_mutex_unlock(&lt->req.reqlock);

    dtq_req_enqueue(&lt->req);           /* hand off to manager */
    return(NULL);                        /* pthread terminates here */
}

/*
 * start app0 thread (run only on rank 0 by <mgr,0>).
 */
static int mgr_startapp(int argc, char **argv) {
    dthread_gtab_t *gt = &dtrs->gtab[0];   /* gtab[0] is always app0 */
    dthread_ltab_t *lt = &dtrs->ltab[0];
    struct app0args *a0a;
    int rv;

    /*
     * thread args cannot be on our stack since they are passed to
     * child pthread via pthread_create().   instead, we make it
     * an inline arg using gt->arg.
     */
    if (sizeof(*a0a) > DTHREAD_INLINE_SIZE)
        errx(1, "mgr_startapp: inline size too small"); /* panic! */

    mgrstate.ltab_alloc = 1;
    mgrstate.ltab_lastslot = 0;
    mgrstate.threads_alloc = 1;
    mgrstate.threads_running = 1;   /* skip over starting state */
    mgrstate.last_slot = 0;
    mgrstate.next_rank = (mgrstate.next_rank + 1) % dtrs->mpi_wsize;

    gt->allocated = 1;
    gt->rank = 0;
    gt->seq = get_free_seq(DTOP_NONE);
    gt->disp_idx = 0;              /* must be start routine */
    gt->arg.dt_argret_type = DTHREAD_INLINE;
    gt->arg.dt_inlinelen = sizeof(*a0a);
    a0a = (struct app0args *) gt->arg.u.dt_inline;
    gt->ret.dt_argret_type = DTHREAD_NODATA;
    gt->detached = 0;
    gt->has_ltab = 1;
    gt->ltab_idx = 0;

    lt->gtab_idx = 0;
    lt->seq = gt->seq;
    lt->pth_state = LT_RUNNING;
    /* note: dthread_run() already inited reqlock/reqnotify */

    a0a->app0main = dtrs->dsps[0].st.start0;   /* first entry of dsps */
    a0a->argc = argc;
    a0a->argv = argv;

    /*
     * if pthread_create fails we are going to abort, so no need
     * to cleanup lt or gt.   just return an error.
     */
    mlog(MGR_INFO, "created inital thread (via app0wrap)");

    rv = pthread_create(&lt->pth, NULL, app0wrap, NULL);
    return(rv);
}

/*
 * manager main routine
 */
int mgr_main(int argc, char **argv) {
    int rv, morework;
    struct dtq_mpiqentry *mqe;
    dthread_request_t *req;

    mlog(MGR_INFO, "mgr_main now starting");

    /* non-zero state init */
    TAILQ_INIT(&mgrstate.rpending);

    /* <mgr,0> launches app0start thread */
    if (dtrs->mpi_rank == 0) {
        rv = mgr_startapp(argc, argv);
        if (rv) {
            /* only fails if pthread_create() fails, unlikely */
            mlog(MGR_CRIT, "mgr_main: unable to start app0");
            MPI_Abort(MPI_COMM_WORLD, 1);
            exit(1);
        }
    }

    /*
     * main loop
     */
    mlog(MGR_INFO, "mgr_main entering main loop");
    morework = 0;
    while (1) {

        /*
         * rank 0 triggers a shutdown if the number of app threads
         * running drops to zero and we are not already shutting down.
         */
        if (dtrs->mpi_rank == 0 && mgrstate.threads_running == 0 &&
            mgrstate.draining == 0) {
            if (mgrstate.threads_starting) {
                /*
                 * should not happen, we cannot have starting threads
                 * without a running thread doing a create op.
                 */
                mlog(MGR_WARN, "mgr: shutdown with starting threads?");
            }
            broadcast_shutdown();
        }

        /* if we are draining for a shutdown, see if we are done */
        if (mgrstate.draining && dtq_send_drainstate() == DTQ_DD_DRAINED) {
            break;       /* all sends complete, we can stop mpithread */
        }

        /* wait for work unless we already know some is queued */
        if (morework == 0) {
            dthread_notifywait(&morework);        /* block here! */
        }

        /* check for inbound messages from MPI */
        if (morework && (mqe = dtq_recv_dequeue(&morework)) != NULL) {

            /* handle inbound mqe messages */
            process_mqe(mqe, &morework);

        }

        /* check for inbound reqs */
        if (morework && (req = dtq_req_dequeue(&morework)) != NULL) {

            /* handle inbound requests */
            process_req(req, &morework);

        }
    }
    mlog(MGR_INFO, "mgr_main exited main loop");

    /*
     * we only exit the main mgr loop after we have entered a shutdown
     * phase (forwarding along the shutdown message to our "broadcast"
     * children, if any).  we have fully drained/flushed the MPI send
     * queue (so we will not send any more MPI messages).  we can
     * stop the MPI thread.
     *
     * any remaining running application threads are going to be
     * forcibly terminated when we exit.  we could attempt to cancel
     * the threads before we exit, but there is no guarantee that
     * they have not blocked cancel ops w/pthread_setcancelstate().
     * we do not want that to keep us from shutting everything down,
     * so it seems better to just let posix exit() kill any remaining
     * threads (since posix exit() cannot be blocked).
     */

    /*
     * XXX: we could walk ltab[] and count the number of running threads
     * remaining and log it if it is non-zero.
     */

    mpit_stop();
    MPI_Finalize();
    dtq_finalize();
    return((dtrs->mpi_rank == 0) ? mgrstate.app0exitval : 0);
}

/*
 * process_mqe: called when the mgr receives an inbound mqe with
 * a dtmsg in it.   inbound mqes are normally generated by the
 * MPI thread when it receives a point-to-point message or when
 * the mgr queues a dtmsg for sending to its own rank (loopback).
 *
 * note that there are no DTOP_JOINWAIT, DTOP_RETCLEAN,
 * or DTOP_APP0_RET dtmsg messages.  those DTOPs are only
 * used with requests.
 *
 * the mgr needs to release or recycle the incoming mqe (we
 * note in a comment when we use an API that does recycling).
 */
static void process_mqe(struct dtq_mpiqentry *mqe, int *morep) {
    struct dtmsg_header *hdr;
    struct dtmsg_created *created;
    struct dtmsg_started *started;
    struct dtmsg_detached *detached;
    struct dtmsg_canceled *canceled;
    struct dtmsg_joined *joined;
    struct dtmsg_terminated *term;
    struct dthread_request *req;

    /* get message header */
    if (mqe->flen < sizeof(*hdr)) {
        mlog(MGR_ERR, "process_mqe: discarding runt mqe (%d)", mqe->flen);
        dtq_recv_release(mqe);
        return;
    }
    hdr = (struct dtmsg_header *) mqe->frame;

    /* sanity check for unexpected msg type on non-rank 0 procs */
    if (dtrs->mpi_rank && (hdr->op == DTOP_CREATE || hdr->op == DTOP_STARTED ||
                           hdr->op == DTOP_DETACH || hdr->op == DTOP_JOIN ||
                           hdr->op == DTOP_TERMINATED) ) {
        /*
         * this shouldn't happen, but it is likely to cause problems.
         * log in case things hang and drop the message.
         */
        mlog(MGR_ERR, "process_mqe: rank %d discarding rank 0 msg %d",
             dtrs->mpi_rank, hdr->op);
        dtq_recv_release(mqe);
        return;
    }

    mlog(MGR_INFO, "process_mqe: op=%d, xidseq=%d, src=%d, dst=%d",
         hdr->op, hdr->xidseq, mqe->peer, dtrs->mpi_rank);

    switch (hdr->op) {
    case DTOP_CREATE:
        /*
         * we are <mgr,0> and have been asked to create a new
         * application thread.
         */
        if (mqe->flen >= sizeof(struct dtmsg_create)) {
            create_thread(mqe, morep);
        } else {
            mlog(MGR_INFO, "process_mqe: runt create request dropped");
        }
        dtq_recv_release(mqe);
        break;

    case DTOP_CREATED:
        /*
         * we are <mgr,S> that sent a CREATE request to <mgr,0>
         * and we've just gotten the reply.  we need to relay that
         * to the application thread blocked in the CREATE call.
         */
        req = FIND_PENDING_MSGREQ(mqe, DTOP_CREATE, created /* gets set */);
        if (req == NULL) {
            mlog(MGR_ERR, "process_mqe: CREATED msg w/o pending req - drop!");
        } else {
            req->reqerror = created->errstatus;
            if (req->reqerror == 0) {
                req->u.newdthread = created->newthread;
                mlog(MGR_INFO, "mgr: CREATED <%d,%d>, seq=%d",
                     created->newthread.dt_index, created->newthread.dt_seq,
                     created->hdr.xidseq);
            } else {
                mlog(MGR_INFO, "mgr: CREATED: failed, seq=%d, err=%d",
                     created->hdr.xidseq, created->errstatus);
            }
            dtq_req_release(req);     /* wakes blocked app thread */
        }
        dtq_recv_release(mqe);    /* done with msg */
        break;

    case DTOP_START:
        /*
         * we are <mgr,T> and have been asked by <mgr,0> to start
         * a new thread on this rank.
         */
        if (mqe->flen >= sizeof(struct dtmsg_start)) {
            start_thread(mqe, morep);
        } else {
            mlog(MGR_INFO, "process_mqe: runt start request dropped");
        }
        dtq_recv_release(mqe);
        break;

    case DTOP_STARTED:
        /*
         * we are <mgr,0> and have sent a START request to <mgr,T>
         * and we've just gotten the STARTED reply.  we need to relay
         * that to the application thread in the CREATE call.  in this
         * case we can recycle our STARTED mqe into a CREATED mqe.
         */
        req = FIND_PENDING_MSGREQ(mqe, DTOP_START, started /* gets set */);
        if (req == NULL) {
            mlog(MGR_INFO, "mgr: STARTED msg without pending req - drop!");
        } else {
            int myerrstatus = started->errstatus;   /* copy out result */
            mgrstate.threads_starting--;

            mlog(MGR_INFO, "mgr: STARTED seq=%d, err=%d - cvt to created",
                 started->hdr.xidseq, myerrstatus);

            /* recycle started into created, used CREATE's xidseq */
            created = RECYCLE_DTMSG_MQE(mqe, started, created, DTOP_CREATED,
                                        req->u.creinfo.c_xidseq);
            mqe->peer = req->u.creinfo.c_peer;  /* sender of CREATE */
            created->errstatus = myerrstatus;
            if (myerrstatus == 0) {
                created->newthread = req->u.creinfo.c_thread;
                mgrstate.threads_running++;
            } else {
                /* start failed, free allocated gtab since we never ran */
                gtab_free(req->u.creinfo.c_thread.dt_index);
            }

            mqe = dtq_send_enqueue(mqe);   /* queue CREATED for send back */
            if (mqe) {
                mlog(MGR_CRIT, "process_mqe: started failed sending created");
                dtq_mqe_unalloc(mqe);
                mqe = NULL;
            } else {
                *morep = 1;
            }
            dtq_req_release(req);     /* free internal START req */
        }
        if (mqe)
            dtq_recv_release(mqe);   /* only on error, o.w. recycled */

        break;

    case DTOP_DETACH:
        /*
         * we are <mgr,0> and have been asked to detach an app thread.
         * detach_thread() will recycle/free the mqe, so we do not free it.
         */
        if (mqe->flen >= sizeof(struct dtmsg_detach)) {
            detach_thread(mqe, morep);
        } else {
            mlog(MGR_INFO, "process_mqe: runt detach request dropped");
        }
        break;

    case DTOP_DETACHED:
        /*
         * we are a <mgr,S> that sent a DETACH request to <mgr,0>
         * and we've just gotten the reply.  we need to relay that
         * to the application thread blocked in the DETACH call.
         */
        req = FIND_PENDING_MSGREQ(mqe, DTOP_DETACH, detached /* gets set */);
        if (req == NULL) {
            mlog(MGR_INFO, "process_mqe: DETACHED msg without pending req");
        } else {
            mlog(MGR_INFO, "mgr: DETACHED, seq=%d, err=%d",
                 detached->hdr.xidseq, detached->errstatus);
            req->reqerror = detached->errstatus;
            dtq_req_release(req);     /* wakes blocked app thread */
        }
        dtq_recv_release(mqe);    /* done with msg */
        break;

    case DTOP_CANCEL:
        /*
         * we are <mgr,T> and have been asked to cancel an app thread.
         * the app thread should be managed by rank T.  cancel_thread()
         * will recycle/free the mqe, so we do not free it.
         */
        if (mqe->flen >= sizeof(struct dtmsg_cancel)) {
            cancel_thread(mqe, morep);
        } else {
            mlog(MGR_INFO, "process_mqe: runt cancel request dropped");
        }
        break;

    case DTOP_CANCELED:
        /*
         * we are <mgr,S> that sent a CANCEL request to <mgr,T>
         * and we've just gotten the reply.  we need to relay that
         * to the application thread blocked in the CANCEL call.
         */
        req = FIND_PENDING_MSGREQ(mqe, DTOP_CANCEL, canceled /* gets set */);
        if (req == NULL) {
            mlog(MGR_INFO, "process_mqe: CANCELED msg without pending req");
        } else {
            mlog(MGR_INFO, "mgr: CANCELED, seq=%d, err=%d",
                 canceled->hdr.xidseq, canceled->errstatus);
            req->reqerror = canceled->errstatus;
            dtq_req_release(req);     /* wakes blocked app thread */
        }
        dtq_recv_release(mqe);    /* done with msg */
        break;

    case DTOP_JOIN:
        /*
         * we are <mgr,0> and have been asked to join an app thread
         * for a thread managed by <mgr,S>.  join_thread() will
         * recycle/free the mqe, so we do not free it.
         */
        if (mqe->flen >= sizeof(struct dtmsg_join)) {
            join_thread(mqe, morep);
        } else {
            mlog(MGR_INFO, "process_mqe: runt join request dropped");
            dtq_recv_release(mqe);
        }
        break;

    case DTOP_JOINED:
        /*
         * we are <mgr,S> that sent a JOIN request to <mgr,0>
         * and we've just gotten the reply.  we need to relay that
         * to the application thread blocked in the JOIN call.
         */
        req = FIND_PENDING_MSGREQ(mqe, DTOP_JOIN, joined /* gets set */);
        if (req == NULL) {
            mlog(MGR_INFO, "process_mqe: JOINED msg without pending req");
        } else {
            mlog(MGR_INFO, "mgr: JOINED, seq=%d, err=%d",
                 joined->hdr.xidseq, joined->errstatus);
            req->reqerror = joined->errstatus;
            if (req->u.retval)
                *req->u.retval = joined->joinret;  /* struct copy */
            dtq_req_release(req);     /* wakes blocked app thread */
        }
        dtq_recv_release(mqe);    /* done with msg */
        break;

    case DTOP_TERMINATED:
        /*
         * we are <mgr,0> and are being notified of an app thread
         * that has terminated either by returning to ther caller or
         * exiting through a pushed cleanup function.  we make the
         * thread a zombie in gtab[] (the ltab[] entry for the thread
         * has already been released by the rank that hosted the
         * thread and sent us this TERMINATED message).
         *
         * if the thread is detached, we can dispose of it now.
         * if there is a pending join on this thread, we can send
         * a JOINED message to the joining thread and dispose of
         * the gtab[] entry for this zombie.   otherwise we leave
         * the thread in the zombie state waiting for someone to
         * join it (or detach it) later...
         */
        term = (struct dtmsg_terminated *) mqe->frame;
        if (dtrs->mpi_rank) {
            mlog(MGR_INFO, "process_mqe: drop terminate to wrong rank (%d)",
                  dtrs->mpi_rank);
        } else if (mqe->flen != sizeof(*term)) {
            mlog(MGR_INFO, "process_mqe: drop wrong-sized term mqe (%d)",
                 mqe->flen);
        } else if (term->thread.dt_index >= dtrs->nmaxthread ||
                   dtrs->gtab[term->thread.dt_index].seq !=
                                                        term->thread.dt_seq) {
            mlog(MGR_INFO, "process_mqe: drop term mqe with bad thread");
        } else if (term->finalstate == LT_RUNNING) {
            /* should not happen, cannot terminate to RUNNING state */
            mlog(MGR_INFO, "process_mqe: drop term mqe with RUNNING state");
        } else {
            /* validated mqe request, call helper do to the work */
            terminated_thread(&term->thread, term->finalstate, morep);
        }
        dtq_recv_release(mqe);
        break;

    case DTOP_BCASTSHUTDOWN:
        /*
         * we received a broadcast shutdown notification from rank 0.
         * send it down the broadcast tree and trigger our shutdown.
         */
        mlog(MGR_INFO, "mgr: received BCASTSHUTDOWN msg");
        broadcast_shutdown();
        *morep = 1;
        dtq_recv_release(mqe);
        break;

    default:
        mlog(MGR_INFO, "process_mqe: dropping unknown mqe %d", hdr->op);
        dtq_recv_release(mqe);
        break;
    }
}

/*
 * the mgr received a request rqe from an application thread running
 * on this rank via the mgrreqq queue.   the rqe belongs to the
 * app thread's ltab[] entry and should be in state DTREQ_BUSY.
 * depending on the rqe type, it may or may not have a mqe (w/dtmsg)
 * attached to it.
 */
static void process_req(dthread_request_t *rqe, int *morep) {
    dthread_ltab_t *lt;
    struct dtq_mpiqentry *mqe;
    struct dtmsg_header *hdr;
    struct dtmsg_terminated *term;

    /* recover the ltab[] entry from the req and sanity check it */
    if (rqe->req_ltabidx < 0 || rqe->req_ltabidx >= dtrs->nmaxthread ||
        rqe != &dtrs->ltab[rqe->req_ltabidx].req) {
        /* should not happen */
        mlog(MGR_INFO, "mgr: process_req: bad request!  dropping.");
        dtq_req_release(rqe);
        return;
    }
    lt = &dtrs->ltab[rqe->req_ltabidx];
    if (lt->req.reqstate != DTREQ_BUSY) {
        /* should not happen */
        mlog(MGR_INFO, "mgr: process_req: request in invalidate state!  drop");
        dtq_req_release(rqe);
        return;
    }

    switch (rqe->reqop) {
    case DTOP_CREATE:
    case DTOP_DETACH:
    case DTOP_CANCEL:
    case DTOP_JOIN:
        /*
         * request from a local application thread that expects a reply.
         * our job is to remove the mqe from the rqe and forward it on
         * to the manager on the peer rank for processing.  as part of
         * we need to generate an xidseq and plug it into both the rqe
         * and the header of the dtmsg in the mqe.  we put the
         * local thread's req on our pending queue waiting for the reply.
         * the local thread blocks on the rqe waiting for a reply.
         */
        mqe = rqe->mqe;
        rqe->mqe = NULL;
        rqe->xidseq = get_free_seq(rqe->reqop);
        hdr = (struct dtmsg_header *)mqe->frame;
        hdr->xidseq = rqe->xidseq;
        if (mqe == NULL) {       /* user gave us a bad rqe */
            mlog(MGR_INFO, "mgr: process_req: rqe w/o expected message mqe!");
            rqe->reqerror = EINVAL;
            dtq_req_release(rqe);
            break;
        }
        mqe = dtq_send_enqueue(mqe);
        *morep = 1;
        if (mqe) {
            mlog(MGR_INFO, "mgr: process_req: rqe message send failed!");
            dtq_mqe_unalloc(mqe);
            rqe->reqerror = EIO;
            dtq_req_release(rqe);
            break;
        }
        mlog(MGR_INFO, "mgr: req %p, op=%d, seq=%d: pending", rqe,
             rqe->reqop, rqe->xidseq);
        TAILQ_INSERT_TAIL(&mgrstate.rpending, rqe, rl);  /* now pending */
        break;

    case DTOP_RETCLEAN:
        /*
         * a local thread either returned to caller or invoked its
         * cleanup function (due to pthread_exit or pthread_cancel).
         *
         * for the cleanup function: if it was invoked by a dthread
         * API call we should have noted this in our pth_state.
         * otherwise it is due to an unexpected direct call to the
         * pthread API (lt->pth_state == LT_CLEANUP).   for the later
         * case we cannot tell which pthread call trigged this.  the
         * the code that set the pth_state to LT_CLEANUP should also
         * have set return argret type to DTHREAD_CANCELED.
         *
         * we need to generate a TERMINATED msg to send to <mgr,0>.
         * we also clear out and release our ltab entry for reuse.
         * note that there is no reply to a TERMINATED message (so
         * no need for a pending request).   there is also no need
         * to lock the req as the thread is no longer running (or
         * is done touching data and in the process of exiting), so
         * we (the mgr) have exclusive access to it.
         */
        mqe = dtq_mqe_alloc();
        if (!mqe) {
            /* cannot make progress: warn user and leak thread */
            mlog(MGR_INFO, "mgr: process_req: terminated msg alloc failed "
                  "leaking thread!");
        } else {
            void *notused;

            /*
             * we must disconnect our ltab entry from gtab before we
             * send the DTOP_TERMINATED msg to <mgr,0> since <mgr,0>
             * is free to dispose of the gtab entry once it has
             * got the terminated msg.
             */
            if (dtrs->gtab[lt->gtab_idx].has_ltab &&
                dtrs->gtab[lt->gtab_idx].ltab_idx == rqe->req_ltabidx &&
                dtrs->gtab[lt->gtab_idx].seq == lt->seq) {

                    dtrs->gtab[lt->gtab_idx].has_ltab = 0;  /* disconnect! */

            } else {
                mlog(MGR_CRIT, "mgr: process_req: retclean gtab mismatch "
                               "has_ltab=%d, ltidx %d %d, seq %d %d",
                               dtrs->gtab[lt->gtab_idx].has_ltab,
                               dtrs->gtab[lt->gtab_idx].ltab_idx,
                               rqe->req_ltabidx,
                               dtrs->gtab[lt->gtab_idx].seq, lt->seq);
            }

            /*
             * get rid of any zombie pthread left by the app thread that
             * completed.  this should not block much (if any) since the
             * thread sends retclean right before completing.   our return
             * is via gtab[], so we do not need/use the pthread return.
             */
            pthread_join(lt->pth, &notused);

            /* prepare and send terminated msg to <mgr,0> */
            term = (struct dtmsg_terminated *) mqe->frame;
            mqe->flen = sizeof(*term);
            mqe->peer = 0;
            term->hdr.op = DTOP_TERMINATED;
            term->hdr.xidseq = 0;   /* one way, so not needed */
            term->thread.dt_index = lt->gtab_idx;
            term->thread.dt_seq = lt->seq;
            term->finalstate = lt->pth_state;
            mqe = dtq_send_enqueue(mqe);
            if (mqe) {
                /* enqueue failed (must be in draindown?) */
                mlog(MGR_INFO, "mgr: process_req: terminated msg enqueue "
                               "failed leaking thread!");
                dtq_mqe_unalloc(mqe);
            } else {
                *morep = 1;                /* just queued a msg */
                mlog(MGR_INFO, "mgr: enqueued terminated mqe for lt=%d",
                     rqe->req_ltabidx);
            }

            /* we can now free our ltab entry (invalidates seq too) */
            ltab_free(rqe->req_ltabidx);
        }

        dtq_req_release(rqe);
        break;

    case DTOP_APP0_RET:
        /*
         * we are <mgr,0> and the main app thread (app0) returned
         * to caller.  we want to broadcast a shutdown msg to all
         * ranks and enter draindown.  once the MPI sends have cleared
         * the main loop will break and we'll complete the shutdown.
         */
        if (dtrs->mpi_rank) {
            /* should not happen, puts us in an unexpected state */
            mlog(MGR_INFO, "mgr: process_req: APP0_RET on non-zero rank %d",
                  dtrs->mpi_rank);
        } else {
            mlog(MGR_INFO, "mgr0: got APP0_RET request");
            broadcast_shutdown();
            *morep = 1;
        }
        dtq_req_release(rqe);
        break;

    default:
        mlog(MGR_INFO, "process_req: dropping unknown req %d", rqe->reqop);
        dtq_req_release(rqe);
        break;
    }

    /* done! */
}
