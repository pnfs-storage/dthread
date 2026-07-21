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
 * dt_threadops.c  application-level thread operations
 * 01-Jun-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dt_internal.h"

/*
 * lookup a function in dispatch table to get its dispatch index.
 * returns didx or 0 on failure (0 is reserved for app0 main thread and
 * cannot be called by a create op).
 */
static uint32_t get_dispatch_idx(void *function) {
    uint32_t didx;

    for (didx = 1 ; didx < dtrs->ndsps ; didx++) {
        if (function == dtrs->dsps[didx].st.start)
            return(didx);
    }
    return(0);
}

/*
 * get my thread's req data structure and mqe (if needed).
 */
static dthread_request_t *get_my_req(struct dtq_mpiqentry **mqep) {
    dthread_ltab_t *lt;

    /* recover our ltab[] entry using TLS */
    lt = pthread_getspecific(dtrs->ltabkey);
    if (!lt) {
        warnx("dt_threadsops: unable to get ltabkey for thread");
        return(NULL);
    }

    /* our thread req must be idle since we are not using it */
    if (lt->req.reqstate != DTREQ_IDLE) {
        warnx("dt_threadsops: thread req is not idle!");
        return(NULL);
    }

    /* allocate mqe if requested */
    if (mqep) {
        *mqep = dtq_mqe_alloc();
        if (!*mqep)
            return(NULL);
    }

    return(&lt->req);
}

/*
 * common part of create/ncreate operation.  should be called with
 * cancel blocked.  returns 0 or error.
 */
static int dthread_create_common(dthread_t *dthread, dthread_attr_t *attr,
                                 uint32_t didx, dthread_argret_t *arg) {
    dthread_request_t *treq;
    struct dtq_mpiqentry *mqe;
    int rv, inh;
    struct dtmsg_create *cre;

    /* get this thread's IDLE req from ltab[] so we can use it */
    treq = get_my_req(&mqe);    /* also get a mqe */
    if (!treq) {
        return(ENOMEM);
    }

    /* setup message */
    cre = (struct dtmsg_create *) mqe->frame;
    mqe->flen = sizeof(*cre);
    mqe->peer = 0;                /* all create reqs go to rank 0 */
    cre->hdr.op = DTOP_CREATE;
    if (attr) {
        cre->attrs = *attr;       /* structure copy */
    } else {
        pthread_attr_init(&cre->attrs);
    }
    /* cannot inheritsched across procs, try to convert to explicit */
    pthread_attr_getinheritsched(&cre->attrs, &inh);
    if (inh != PTHREAD_EXPLICIT_SCHED) {
        int policy;
        struct sched_param param;
        if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
            pthread_attr_setschedpolicy(&cre->attrs, policy);
            pthread_attr_setschedparam(&cre->attrs, &param);
        }
        /* XXX: no posix api to read scope, so can't set */
        /* XXX: stick with what ever we were given */
        /* pthread_attr_setscope(&cre->attrs, contentionscope); */
        pthread_attr_setinheritsched(&cre->attrs, PTHREAD_EXPLICIT_SCHED);
    }
    cre->disp_idx = didx;
    if (arg) {
        cre->arg = *arg;          /* structure copy */
    } else {
        memset(&cre->arg, 0, sizeof(cre->arg));
        cre->arg.dt_argret_type = DTHREAD_NODATA;
    }

    /* lock, setup, and wait for req */
    pthread_mutex_lock(&treq->reqlock);
    treq->reqstate = DTREQ_BUSY;
    treq->reqpeer = mqe->peer;
    treq->reqop = cre->hdr.op;
    treq->mqe = mqe;
    treq->reqerror = 0;
    dtq_req_enqueue(treq);

    /* wait for it to get processed */
    while (treq->reqstate == DTREQ_BUSY) {
        pthread_cond_wait(&treq->reqnotify, &treq->reqlock);  /* block! */
    }

    /* collect results */
    rv = treq->reqerror;
    if (rv == 0)
        *dthread = treq->u.newdthread;

    /* return to idle state and drop lock */
    treq->reqstate = DTREQ_IDLE;
    pthread_mutex_unlock(&treq->reqlock);

    return(rv);
}

/*
 * create a dthread (native API).  on success thread is returned
 * in *dthread.
 */
int dthread_ncreate(dthread_t *dthread, dthread_attr_t *attr,
                    dthread_start_t dtstart, dthread_argret_t *arg) {
    uint32_t didx;
    int rv, oldcanstate;

    if (!dthread)
        return(EINVAL);
    didx = get_dispatch_idx(dtstart);
    if (didx == 0)
        return(ENOENT);

    /* trying to apply native interface to a pthread-style dispatch? */
    if (dtrs->dsps[didx].dt_argproc || dtrs->dsps[didx].dt_retproc)
        return(EOPNOTSUPP);

    /* block cancel while we use thread's req and a mqe */
    rv = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    if (rv)
        return(rv);

    /* call common function to do the work */
    rv = dthread_create_common(dthread, attr, didx, arg);

    /* restore cancel state and return */
    pthread_setcancelstate(oldcanstate, NULL);
    return(rv);
}

/*
 * create a dthread (pthread API).  on success thread is returned
 * in *dthread.
 */
int dthread_create(dthread_t *dthread, dthread_attr_t *attr,
                   dthread_start_pth_t dtstartpth, void *arg) {
    uint32_t didx;
    dthread_proc_pth_t proc;
    int rv, oldcanstate;
    dthread_argret_t procarg;

    if (!dthread)
        return(EINVAL);
    didx = get_dispatch_idx(dtstartpth);
    if (didx == 0)
        return(ENOENT);

    /* trying to apply pthread-style interface to a native dispatch? */
    if (!dtrs->dsps[didx].dt_argproc || !dtrs->dsps[didx].dt_retproc)
        return(EOPNOTSUPP);
    proc = dtrs->dsps[didx].dt_argproc;

    /* block cancel while we setup arg and use thread's req and a mqe */
    rv = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    if (rv)
        return(rv);

    /* convert void* arg to a argret using proc function */
    rv = (*proc)(DTHREAD_PROC_ENCODE, &procarg, &arg);
    if (rv)
        goto done;     /* proc failed, abort call */

    /* call common function to do the work */
    rv = dthread_create_common(dthread, attr, didx, &procarg);

    /* if the call failed, we need to free encode procarg */
    if (rv) {
        /* ignore return, it can't fail */
        (*proc)(DTHREAD_PROC_FREE, &procarg, NULL);
    }

done:
    /* restore cancel state and return */
    pthread_setcancelstate(oldcanstate, NULL);
    return(rv);
}

/*
 * detach a dthread.  ret 0 on success.  we need to inform rank0 manager
 * about the detach so that it can do something in case there is a pending
 * join on the thread.
 */
int dthread_detach(dthread_t thread) {
    int rv, oldcanstate;
    dthread_request_t *treq;
    struct dtq_mpiqentry *mqe;
    struct dtmsg_detach *det;

    /* prevalidate thread */
    if (thread.dt_index >= dtrs->nmaxthread)
        return(ESRCH);

    /* block cancel while we use thread's req and a mqe */
    rv = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    if (rv)
        return(rv);

    /* get this thread's IDLE req from ltab[] so we can use it */
    treq = get_my_req(&mqe);    /* also get a mqe */
    if (!treq) {
        rv = ENOMEM;
        goto done;
    }

    /* setup message */
    det = (struct dtmsg_detach *) mqe->frame;
    mqe->flen = sizeof(*det);
    mqe->peer = 0;                /* all detach reqs go to rank 0 */
    det->hdr.op = DTOP_DETACH;
    det->thread = thread;

    /* lock, setup, and wait for req */
    pthread_mutex_lock(&treq->reqlock);
    treq->reqstate = DTREQ_BUSY;
    treq->reqpeer = mqe->peer;
    treq->reqop = det->hdr.op;
    treq->mqe = mqe;
    treq->reqerror = 0;
    dtq_req_enqueue(treq);

    /* wait for it to get processed */
    while (treq->reqstate == DTREQ_BUSY) {
        pthread_cond_wait(&treq->reqnotify, &treq->reqlock);  /* block! */
    }

    /* collect results */
    rv = treq->reqerror;

    /* return to idle state and drop lock */
    treq->reqstate = DTREQ_IDLE;
    pthread_mutex_unlock(&treq->reqlock);

done:
    /* restore cancel state and return */
    pthread_setcancelstate(oldcanstate, NULL);
    return(rv);
}

/*
 * cancel a dthread.  ret 0 on success.  we send the cancel directly to
 * the rank the thread is running on.  if/when it delivers the cancel to
 * the thread, the retclean/TERMINATED path will be invoked so that
 * rank0 manager can clean up (including any pending joins).
 */
int dthread_cancel(dthread_t thread) {
    int peer, rv, oldcanstate;
    dthread_request_t *treq;
    struct dtq_mpiqentry *mqe;
    struct dtmsg_cancel *can;

    /* prevalidate thread */
    if (thread.dt_index >= dtrs->nmaxthread ||
        dtrs->gtab[thread.dt_index].allocated == 0 ||
        dtrs->gtab[thread.dt_index].seq != thread.dt_seq)
        return(ESRCH);
     peer = dtrs->gtab[thread.dt_index].rank;

    /* block cancel while we use thread's req and a mqe */
    rv = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    if (rv)
        return(rv);

    /* get this thread's IDLE req from ltab[] so we can use it */
    treq = get_my_req(&mqe);    /* also get a mqe */
    if (!treq) {
        rv = ENOMEM;
        goto done;
    }

    /* setup message */
    can = (struct dtmsg_cancel *) mqe->frame;
    mqe->flen = sizeof(*can);
    mqe->peer = peer;           /* send cancel directly to peer */
    can->hdr.op = DTOP_CANCEL;
    can->thread = thread;

    /* lock, setup, and wait for req */
    pthread_mutex_lock(&treq->reqlock);
    treq->reqstate = DTREQ_BUSY;
    treq->reqpeer = mqe->peer;
    treq->reqop = can->hdr.op;
    treq->mqe = mqe;
    treq->reqerror = 0;
    dtq_req_enqueue(treq);

    /* wait for it to get processed */
    while (treq->reqstate == DTREQ_BUSY) {
        pthread_cond_wait(&treq->reqnotify, &treq->reqlock);  /* block! */
    }

    /* collect results */
    rv = treq->reqerror;

    /* return to idle state and drop lock */
    treq->reqstate = DTREQ_IDLE;
    pthread_mutex_unlock(&treq->reqlock);

done:
    /* restore cancel state and return */
    pthread_setcancelstate(oldcanstate, NULL);
    return(rv);
}

/*
 * common part of exit/nexit operation.  does not return (calls pthread_exit()).
 */
static void dthread_exit_common(int native, dthread_argret_t *ret, void *v) {
    dthread_ltab_t *lt;
    dthread_gtab_t *gt;
    uint32_t didx;
    int native_didx;

    /* permanently block cancel for rest of thread life (shouldn't fail) */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    /* recover our ltab[] entry using TLS and use it to get gtab[] */
    lt = pthread_getspecific(dtrs->ltabkey);
    if (!lt) {
        warnx("dt_threadsops: FATAL!  exit unable to get ltabkey for thread");
        exit(1);    /* no way to recover from this, panic time */
    }
    if (lt->gtab_idx >= dtrs->nmaxthread) {
        warnx("dt_threadsops: FATAL!  exit found corrupt ltab for thread");
        exit(1);    /* no way to recover from this, panic time */
    }
    gt = &dtrs->gtab[lt->gtab_idx];
    if (lt->seq != gt->seq) {
        warnx("dt_threadsops: FATAL!  exit found seq error for thread");
        exit(1);    /* no way to recover from this, panic time */
    }
    didx = gt->disp_idx;
    gt->ret.dt_argret_type = DTHREAD_NODATA;  /* start with NODATA */

    if (didx >= dtrs->ndsps) {
        warnx("dt_threadops: exit - bad disp_idx in gtab[] %d", didx);
    } else {

        native_didx = (dtrs->dsps[didx].dt_argproc == NULL &&
                       dtrs->dsps[didx].dt_retproc == NULL);

        if (native) {

            if (native_didx) {
                if (ret)
                    gt->ret = *ret;
            } else {
                warnx("dt_threadops: nexit - called with non-native thread");
            }

        } else {

            if (native_didx) {
                warnx("dt_threadops: exit - called with native thread");
            } else {
                int prv;
                dthread_proc_pth_t proc = dtrs->dsps[didx].dt_retproc;

                prv = (*proc)(DTHREAD_PROC_ENCODE, &gt->ret, &v);
                if (prv) {
                    warnx("dt_threadops: exit, proc fn failed");
                    /*
                     * XXX: thread is exiting, so can't return an error.
                     * we'll warn the user and code it as DTHREAD_CANCELED.
                     */
                    (*proc)(DTHREAD_PROC_FREE, NULL, &v);
                    gt->ret.dt_argret_type = DTHREAD_CANCELED;

                }
            }
       }

    }

    /* update state to indicate this thread called a dthread exit fn */
    pthread_mutex_lock(&lt->req.reqlock);
    lt->pth_state = LT_EXITED;
    /* exit sent to <mgr,0> with TERMINATED msg (for gtab[] update) */
    pthread_mutex_unlock(&lt->req.reqlock);

    pthread_exit(NULL);   /* triggers cleanup routine, never returns */
    errx(1, "pthread_exit() returned?");
}

/*
 * exit current thread (native API).  does not return.  we do the
 * setup and let the pushed pthread cleanup routine finish.
 */
void dthread_nexit(dthread_argret_t *ret) {
    dthread_exit_common(1, ret, NULL);
}

/*
 * exit current thread (pthread API).  does not return.  we do the
 * setup and let the pushed pthread cleanup routine finish.
 */
void dthread_exit(void *ret) {
    dthread_exit_common(0, NULL, ret);
}

/*
 * common part of join/njoin operation.  called w/cancel blocked.
 */
int dthread_join_common(dthread_t dthread, dthread_argret_t *ret) {
    dthread_request_t *treq;
    struct dtq_mpiqentry *mqe;
    struct dtmsg_join *join;
    int rv;

    /* get this thread's IDLE req from ltab[] so we can use it */
    treq = get_my_req(&mqe);    /* also get a mqe */
    if (!treq) {
        return(ENOMEM);
    }

    /* setup message */
    join = (struct dtmsg_join *) mqe->frame;
    mqe->flen = sizeof(*join);
    mqe->peer = 0;                /* all join reqs go to rank 0 */
    join->hdr.op = DTOP_JOIN;
    join->thread = dthread;

    /* lock, setup, and wait for req */
    pthread_mutex_lock(&treq->reqlock);
    treq->reqstate = DTREQ_BUSY;
    treq->reqpeer = mqe->peer;
    treq->u.retval = ret;
    treq->reqop = join->hdr.op;
    treq->mqe = mqe;
    treq->reqerror = 0;
    dtq_req_enqueue(treq);

    /* wait for it to get processed */
    while (treq->reqstate == DTREQ_BUSY) {
        pthread_cond_wait(&treq->reqnotify, &treq->reqlock);  /* block! */
    }

    /* collect results */
    rv = treq->reqerror;
    if (rv) {
        ret->dt_argret_type = DTHREAD_NODATA;  /* just in case */
    } else {
        /*
         * the mgr already updated the structure pointed to by treq->u.retval
         * with the retval from the JOINED message, so we don't need to
         * copy anything else out of the req.
         */
    }

    /* return to idle state and drop lock */
    treq->reqstate = DTREQ_IDLE;
    pthread_mutex_unlock(&treq->reqlock);

    return(rv);
}

/*
 * join thread running a native API call.  return EOPNOTSUPP if we
 * try and njoin a thread using the pthread API.
 */
int dthread_njoin(dthread_t thread, dthread_argret_t *ret) {
    dthread_ltab_t *lt;
    uint32_t didx;
    int rv, oldcanstate;

    /* validate thread */
    if (thread.dt_index >= dtrs->nmaxthread ||
        dtrs->gtab[thread.dt_index].allocated == 0 ||
        thread.dt_seq != dtrs->gtab[thread.dt_index].seq)
        return(ESRCH);

    if ( (lt = pthread_getspecific(dtrs->ltabkey)) &&
        lt->gtab_idx == thread.dt_index)
        return(EDEADLK);    /* tried to join itself */

    didx = dtrs->gtab[thread.dt_index].disp_idx;
    if (didx >= dtrs->ndsps)   /* this shouldn't happen */
        return(EIO);

    /* trying to apply native interface to a pthread-style dispatch? */
    if (dtrs->dsps[didx].dt_argproc || dtrs->dsps[didx].dt_retproc)
        return(EOPNOTSUPP);

    /* block cancel while we use thread's req and a mqe */
    rv = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    if (rv)
        return(rv);

    /* call common function to do the work */
    rv = dthread_join_common(thread, ret);

    /* restore cancel state and return */
    pthread_setcancelstate(oldcanstate, NULL);
    return(rv);
}

/*
 * join thread running with the pthread-style API.  return EOPNOTSUPP
 * if we try and join a native API thread.
 */
int dthread_join(dthread_t thread, void **retptr) {
    dthread_ltab_t *lt;
    uint32_t didx;
    dthread_proc_pth_t proc;
    int rv, oldcanstate;
    dthread_argret_t procret;

    /* validate thread */
    if (thread.dt_index >= dtrs->nmaxthread ||
        dtrs->gtab[thread.dt_index].allocated == 0 ||
        thread.dt_seq != dtrs->gtab[thread.dt_index].seq)
        return(ESRCH);

    if ( (lt = pthread_getspecific(dtrs->ltabkey)) &&
        lt->gtab_idx == thread.dt_index)
        return(EDEADLK);    /* tried to join itself */

    didx = dtrs->gtab[thread.dt_index].disp_idx;
    if (didx >= dtrs->ndsps)   /* this shouldn't happen */
        return(EIO);

    /* trying to apply pthread-style interface to a native dispatch? */
    if (!dtrs->dsps[didx].dt_argproc || !dtrs->dsps[didx].dt_retproc)
        return(EOPNOTSUPP);
    proc = dtrs->dsps[didx].dt_retproc;

    /* block cancel while we setup arg and use thread's req and a mqe */
    rv = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcanstate);
    if (rv)
        return(rv);

    /* call common function to do the work */
    rv = dthread_join_common(thread, &procret);
    if (rv)
        goto done;     /* join failed, just return the error */

    /* use proc fn to convert procret to a void* that we can return */
    rv = (*proc)(DTHREAD_PROC_DECODE, &procret, retptr);

    if (rv) {
        warnx("dthread_join: proc decode failed, ret CANCEL instead");
        *retptr = PTHREAD_CANCELED;
    }

    /* free procret now that it's been decoded */
    (*proc)(DTHREAD_PROC_FREE, &procret, NULL);  /* ignore return */

done:
    /* restore cancel state and return */
    pthread_setcancelstate(oldcanstate, NULL);
    return(rv);
}
