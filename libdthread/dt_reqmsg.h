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

#ifndef _DTHREAD_REQMSG_H_
#define _DTHREAD_REQMSG_H_

/*
 * dt_reqmsg.h  dthread request and mpi message interfaces
 * 19-May-2026  chuck@ece.cmu.edu
 */
#include <sys/queue.h>

#include <dthread/dthread.h>

/*
 * defines
 */
/* request state; thread req are part of ltab[], mgr req is malloc'd by mgr */
#define DTREQ_IDLE         0              /* idle thread req (owner=thread) */
#define DTREQ_BUSY         1              /* busy thread req (owner=mgr) */
#define DTREQ_DONE         2              /* done thread req (owner=thread) */
#define DTREQ_INTERNAL     3              /* internal mgr req (owner=mgr) */
/* note: internal req is only accessed by mgr thread, so no locking required */

/* dthread request/message operations */
#define DTOP_NONE           0             /* no operation */
#define DTOP_CREATE         1             /* create thread */
#define DTOP_CREATED        2             /* result of create */
#define DTOP_START          3             /* start thread on target */
#define DTOP_STARTED        4             /* result of a start */
#define DTOP_DETACH         5             /* detach thread */
#define DTOP_DETACHED       6             /* result of a detach */
#define DTOP_CANCEL         7             /* cancel thread */
#define DTOP_CANCELED       8             /* result of a cancel op */
#define DTOP_JOIN           9             /* join thread */
#define DTOP_JOINED        10             /* result of a join op */
#define DTOP_JOINWAIT      11             /* join blocked waiting on thread */
#define DTOP_RETCLEAN      12             /* thread exit via return/cleanup */
#define DTOP_TERMINATED    13             /* thread terminated */
#define DTOP_APP0_RET      14             /* main app thread on r0 returned */
#define DTOP_BCASTSHUTDOWN 15             /* shutdown broadcast */
/*
 * notes on operations:
 *   thread req + mqe message, expects reply: CREATE, DETACH, CANCEL, JOIN
 *   terminal thread req, no reply: RETCLEAN, APP0_RET (on rank0 only)
 *   internal rank 0 req + mqe message: START
 *   internal rank 0 req: JOINWAIT
 *   reply mqe message, no req: CREATED, STARTED, DETACHED, CANCELED, JOINED
 *   notification mqe message to rank 0 mgr: TERMINATED
 *   broadcast mqe message, started by rank 0: BCASTSHUTDOWN
 */

/*
 * info from a CREATE msg that <mgr,0> saves in the START req for <mgr,T>
 * that is needed to generate a CREATED msg.
 */
typedef struct {
    dthread_t c_thread;                   /* thread being created */
    int c_peer;                           /* rank that sent CREATE */
    uint32_t c_xidseq;                    /* xidseq of CREATE request */
    
} dthread_createinfo_t;

/*
 * request from an app thread or an internally generated request
 */
typedef struct dthread_request {
    int req_ltabidx;                      /* ltab backptr or -1 if internal */
    int reqstate;                         /* req owner/state */
    int reqpeer;                          /* peer rank */
    int reqop;                            /* operation (DTOP_*) */
    struct dtq_mpiqentry *mqe;            /* passed to mpi to send req */
    uint32_t xidseq;                      /* xid/seq# (filled by mgr) */
    int reqerror;                         /* error code (if done) */
    union {
        dthread_t newdthread;             /* ret when done w/CREATE */
        dthread_argret_t *retval;         /* retval when done w/JOIN */
        dthread_createinfo_t creinfo;     /* CREATE info */
        dthread_t waitthread;             /* thread we wait for (JOINWAIT) */
    } u;

    /* reqlock and reqnotify are only used if !DTREQ_INTERNAL */
    pthread_mutex_t reqlock;              /* for syncing notifications */
    pthread_cond_t reqnotify;             /* to wait for req to be done */

    TAILQ_ENTRY(dthread_request) rl;      /* request list linkage */
} dthread_request_t;

/* reqlist is a list of dthread_request_t structs */
TAILQ_HEAD(reqlist, dthread_request);

/*
 * common header for all message types
 */
struct dtmsg_header {
    uint32_t op;                          /* operation (DTOP_*) */
    uint32_t xidseq;                      /* transaction id/seq# */
};

/*
 * create thread.  <app,S> attaches this in a mqe to the create req
 * it sends to <mgr,S> as part of a dthread create API call.
 * <mgr,S> will strip the mqe from the req and send it to <mgr,0>.
 */
struct dtmsg_create {
    struct dtmsg_header hdr;              /* header */
    dthread_attr_t attrs;                 /* attrs for new thread */
    uint32_t disp_idx;                    /* start fn index in dispatch[] */
    dthread_argret_t arg;                 /* input arg */
};

/*
 * created thread.  <mgr,0> reply to <mgr,S> with result of create op.
 */
struct dtmsg_created {
    struct dtmsg_header hdr;              /* header */
    int errstatus;                        /* error status of call */
    dthread_t newthread;                  /* new thread, if success */
};

/*
 * start thread.  sent from <mgr,0> to <mgr,T> to start thread on T
 * as part of processing a create msg.
 */
struct dtmsg_start {
    struct dtmsg_header hdr;              /* header */
    dthread_t thread;                     /* handle for new thread */
    dthread_attr_t attrs;                 /* attrs for new thread */
};

/*
 * started thread.  <mgr,T> reply to <mgr,0> with result of start op.
 */
struct dtmsg_started {
    struct dtmsg_header hdr;              /* header */
    int errstatus;                        /* error status of call */
};

/*
 * detach thread.  generated by app thread when calling dthread_detach().
 */
struct dtmsg_detach {
    struct dtmsg_header hdr;              /* header */
    dthread_t thread;                     /* thread to detach */
};

/*
 * detached thread.  reply from <mgr,0> with result of detach op.
 */
struct dtmsg_detached {
    struct dtmsg_header hdr;              /* header */
    int errstatus;                        /* error status of call */
};

/*
 * cancel thread.  generated by app thread when calling dthread_cancel().
 */
struct dtmsg_cancel {
    struct dtmsg_header hdr;              /* header */
    dthread_t thread;                     /* thread to cancel */
};

/*
 * canceled thread.  reply from <mgr,T> with result of cancel op.
 */
struct dtmsg_canceled {
    struct dtmsg_header hdr;              /* header */
    int errstatus;                        /* error status of call */
};

/*
 * join thread.  generated by app thread when calling dthread join API.
 */
struct dtmsg_join {
    struct dtmsg_header hdr;              /* header */
    dthread_t thread;                     /* thread to join */
};

/*
 * joined thread.  reply from <mgr,0> with result of join op.
 */
struct dtmsg_joined {
    struct dtmsg_header hdr;              /* header */
    int errstatus;                        /* error status of call */
    dthread_argret_t joinret;             /* return arg from join */
};

/*
 * terminated thread.  sent from mgr on the rank the thread was
 * running on to the mgr on rank 0 (e.g. for handling joins, etc.).
 */
struct dtmsg_terminated {
    struct dtmsg_header hdr;              /* header */
    dthread_t thread;                     /* handle of terminated thread */
    uint32_t finalstate;                  /* reason why it went */
};

/*
 * union covering all message types (used to size mqe's frame[] buffer)
 */
union dtmsg_all {
    struct dtmsg_header header;
    struct dtmsg_create create;
    struct dtmsg_created created;
    struct dtmsg_start start;
    struct dtmsg_started started;
    struct dtmsg_detach detach;
    struct dtmsg_detach detached;
    struct dtmsg_cancel cancel;
    struct dtmsg_canceled canceled;
    struct dtmsg_join join;
    struct dtmsg_join joined;
    struct dtmsg_terminated terminated;
};
#endif /* _DTHREAD_REQMSG_H_ */
