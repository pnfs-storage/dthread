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

#ifndef _DTHREAD_QUEUING_H_
#define _DTHREAD_QUEUING_H_

/*
 * dt_queuing.h  dthread queuing (between local threads)
 * 06-May-2026  chuck@ece.cmu.edu
 */

#include <pthread.h>
#include <sys/queue.h>

#include "dt_reqmsg.h"

/*
 * this module handles manager/mpi thread and application/manager
 * thread communications and locking.  it is based on mvp_queuing
 * from mvpnet.
 */

/*
 * config.  the messages we send over mpi are relatively small,
 * so we can inline them init the mpiq_entry structure.
 */
#define DTQ_MAXMSGSIZE   sizeof(union dtmsg_all)

/*
 * draindown states
 */
#define DTQ_DD_INIT      0    /* no draindown active */
#define DTQ_DD_DRAINING  1    /* drain in progress */
#define DTQ_DD_DRAINED   2    /* drain complete, we are done */

/*
 * stats we collect about queuing
 */
struct dtq_stats {
    int nmqe;                               /* # mpi queue entries alloc'd */
    int nreq;                               /* # int req structs alloc'd */
    int mpimaxsqlen;                        /* max size sendq has grown to */
    int mpimaxrqlen;                        /* max size recvq has grown to */
    int mgrmaxrqlen;                        /* max size reqq has grown to */
};

/*
 * mpi queue entry ("mqe"): shared by send and recv paths.  the mpi
 * thread consumes mqes from mpisendq produces entries on mpirecvq.
 * if the outbound mqe is for our rank, we can just directly enqueue
 * it on mpirecvq rather than send it through MPI.
 */
struct dtq_mpiqentry {
    char frame[DTQ_MAXMSGSIZE];      /* data to send/recv */
    int flen;                        /* length of the frame buffer */
    int peer;                        /* MPI rank of peer */
    TAILQ_ENTRY(dtq_mpiqentry) mql;  /* linkage */
};

/* mqelist is a list of dtq_mpiqentry structs */
TAILQ_HEAD(mqelist, dtq_mpiqentry);

/*
 * dtq_state: top-level queue state structure shared by the manager,
 * MPI, and local app threads.
 */
struct dtq_state {
    pthread_mutex_t dtqlock;      /* the queuing lock */

    struct reqlist mgrreqq;       /* manager request input queue */
    int mgrrqlen;                 /* #reqs on mgrreqq */
    int mgrmaxrqlen;              /* max size mgrreqq has grown to */
    pthread_cond_t mgr_notify;    /* manager notification cond w/dtlockq */
    int notifytimelimit;          /* cond_wait notify time limit (secs) */
    int nreq;                     /* number of reqs allocated w/malloc */
    struct reqlist reqfree;       /* internal (non-ltab) req structures */

    struct mqelist mpisendq;      /* MPI send queue */
    int mpisqlen;                 /* #mqes on sendq */
    int mpimaxsqlen;              /* max size sendq has grown to */
    int mpindequeuedsends;        /* #dqeueued mqes mpi thread is sending */
    int mpisq_draindown;          /* mpisendq drain down state (DTQ_DD_*) */

    struct mqelist mpirecvq;      /* MPI recv queue */
    int mpirqlen;                 /* #mqes on recvq */
    int mpimaxrqlen;              /* max size recvq has grown to */

    int nmqe;                     /* number of mqes allocated */
    struct mqelist mqefree;       /* list of free mpiq_entry structs */
};

/* api function prototypes */
void dthread_notifywait(int *morep);
void dtq_notifytimelimit(int newval);

dthread_request_t *dtq_req_alloc(void);

void dtq_req_enqueue(dthread_request_t *req);
dthread_request_t *dtq_req_dequeue(int *morep);
void dtq_req_release(dthread_request_t *req);

struct dtq_mpiqentry *dtq_mqe_alloc(void);
void dtq_mqe_unalloc(struct dtq_mpiqentry *mqe);

struct dtq_mpiqentry *dtq_send_enqueue(struct dtq_mpiqentry *mqe);
struct dtq_mpiqentry *dtq_send_dequeue();
void dtq_send_release(struct dtq_mpiqentry *mqe);

void dtq_recv_enqueue(struct dtq_mpiqentry *mqe);
struct dtq_mpiqentry *dtq_recv_dequeue(int *morep);
void dtq_recv_release(struct dtq_mpiqentry *mqe);

int dtq_send_draindown(void);
int dtq_send_drainstate(void);
void dtq_stats(struct dtq_stats *qs);

int dtq_init(void);
void dtq_finalize(void);

#endif /* _DTHREAD_QUEUING_H_ */
