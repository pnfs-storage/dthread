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
 * mpi_thread.c  mpi i/o thread
 * 05-May-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <sched.h>      /* sched_yield() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dt_internal.h"

#define MPIMTS_INITSZ    16     /* starting #slots to allocate */

/*
 * manage combined state for MPI_Testsome() calls.
 */
struct mpimtsmgr {
    /* slot allocation counters */
    int nalloc;                 /* number of test slots allocated */
    int nused;                  /* number of test slots in use (<=nalloc) */

    /* stat counters */
    uint64_t ntestsome;         /* number of MPI_Testsome() calls */
    uint64_t got1;              /* #times testsome returned 1 entry */
    uint64_t gotmore;           /* #times testsome return >1 entry */
    uint64_t gotmax;            /* max# entries testsome returned */

    /* arrays (all contain nalloc entries) */
    struct dtq_mpiqentry **mqes;/* array of send mqes (in sync with reqs[]) */
    MPI_Request *reqs;          /* array of requests we are testing */
    int *idxout;                /* array of completed index vals */
    MPI_Status *statout;        /* array of output status vals */
};

#define MPIMTSMGR_INIT                                                         \
    (struct mpimtsmgr) {                                                       \
        .nalloc = 0, .nused = 0, .ntestsome = 0, .got1 = 0, .gotmore = 0,      \
        .gotmax = 0, .mqes = NULL, .reqs = NULL, .idxout = NULL,               \
        .statout = NULL                                                        \
    }

/*
 * ensure there are at least 'n' slots in the mpimtsmgr.
 * realloc arrays if they are too small.   return 0 on
 * success, -1 on memory allocation failure.
 */
static int mpimts_setslots(struct mpimtsmgr *mts, int n) {
    void *tmp;

    if (n <= mts->nalloc)    /* already large enough? */
        return(0);

    tmp = realloc(mts->mqes, n * sizeof(mts->mqes[0]));
    if (!tmp)
        return(-1);
    mts->mqes = tmp;

    tmp = realloc(mts->reqs, n * sizeof(mts->reqs[0]));
    if (!tmp)
        return(-1);
    mts->reqs = tmp;

    tmp = realloc(mts->idxout, n * sizeof(mts->idxout[0]));
    if (!tmp)
        return(-1);
    mts->idxout = tmp;

    tmp = realloc(mts->statout, n * sizeof(mts->statout[0]));
    if (!tmp)
        return(-1);
    mts->statout = tmp;

    mts->nalloc = n;
    return(0);
}

/*
 * free arrays and reset.  caller should empty out pending
 * requests before calling this.
 */
static void mpimts_afree(struct mpimtsmgr *mts) {
    if (mts->mqes)
        free(mts->mqes);
    if (mts->reqs)
        free(mts->reqs);
    if (mts->idxout)
        free(mts->idxout);
    if (mts->statout)
        free(mts->statout);
    *mts = MPIMTSMGR_INIT;   /* reset */
}

/*
 * init mpimtsmgr.   return 0 on succes, -1 on allocation failure.
 */
static int mpimts_init(struct mpimtsmgr *mts, int n) {
    int ret;

    *mts = MPIMTSMGR_INIT;
    ret = mpimts_setslots(mts, n);
    if (ret == -1) {
        mpimts_afree(mts);
        return(-1);
    }
    mlog(MPI_INFO, "mts init to %d testsome slots", n);
    return(0);
}

/*
 * add send mqe and request to test arrays.   return 0 on success,
 * -1 on failure.
 */
static int mpimts_add(struct mpimtsmgr *mts, struct dtq_mpiqentry *mqe,
                      MPI_Request req) {
    int slot;

    /* if all array slots are in use, grow mgr */
    if (mts->nused == mts->nalloc) {
        if (mpimts_setslots(mts, mts->nalloc * 2) == -1)
            return(-1);
        mlog(MPI_INFO, "mts grew testsome slot count to %d", mts->nalloc);
    }

    /* put new entry at the end of the arrays (same slot#) */
    slot = mts->nused;
    mts->mqes[slot] = mqe;
    mts->reqs[slot] = req;
    mts->nused++;
    mlog(MPI_DBG, "send mqe %p added to mts slot %d.  nused now %d.",
         mqe, slot, mts->nused);
   return(0);
}

/*
 * mqe send is complete, release the mqe.
 */
static void mpimts_done(struct dtq_mpiqentry *mqe) {
    dtq_send_release(mqe);
}

/*
 * do a MPI_Testsome now.   return 0 on success, -1 if we get
 * a general error.
 */
static int mpimts_test(struct mpimtsmgr *mts) {
    int rv, got, lcv, firsthole, i, err, new_nused, pullfrom;
    struct dtq_mpiqentry *mqe;

    if (mts->nused == 0)     /* nothing to test? */
        return(0);

    rv = MPI_Testsome(mts->nused, mts->reqs, &got, mts->idxout, mts->statout);
    mts->ntestsome++;

    if (rv == MPI_UNDEFINED) {
        /* this should never happen */
        mlog(MPI_ERR, "MPI_Testsome returned MPI_UNDEFINED, reset!");
        mts->nused = 0;
        return(0);
    }

    if (rv != MPI_SUCCESS && rv != MPI_ERR_IN_STATUS) {
        mlog(MPI_CRIT, "MPI_Testsome returned unexpected val %d", rv);
        return(-1);    /* this shouldn't happen either... */
    }

    if (got == 0)     /* if nothing finished, we are done */
        return(0);

     /* update stats */
    if (got == 1)
        mts->got1++;
    else if (got > 1)
        mts->gotmore++;
    if (got > mts->gotmax)
        mts->gotmax = got;

    /* release complete send mqes, finding firsthole in the process */
    for (lcv = 0, firsthole = mts->idxout[0] ; lcv < got ; lcv++) {
        i = mts->idxout[lcv];
        if (i < firsthole)
            firsthole = i;
        mqe = mts->mqes[i];
        mts->mqes[i] = NULL;   /* create corrsponding hole in mqes[] */
        err = (rv == MPI_SUCCESS) ? MPI_SUCCESS : mts->statout[i].MPI_ERROR;
        if (err != MPI_SUCCESS) {
            mlog(MPI_ERR, "mts Isend send mqe %p %d->%d failed (mpierr=%d)",
                 mqe, dtrs->mpi_rank, mqe->peer, err);
        }
        mlog(MPI_DBG, "mts send mqe done: err=%d, mqe=%p", err, mqe);
        mpimts_done(mqe);
    }

    if (got >= mts->nused) {    /* if everything is done, just reset */
        mts->nused = 0;
        mlog(MPI_DBG, "mts update: nused=0 (got=%d)", got);
        return(0);
    }

    /*
     * we need to compact out all the holes in mts->sqes[] while
     * keeping mts->reqs[] in sync.   we know where the first hole
     * is and we know the new smaller size of the array.
     */
    new_nused = mts->nused - got;
    pullfrom = new_nused;  /* use ones from here to fill holes */
    for (lcv = firsthole ; lcv < new_nused ; lcv++) {

        if (mts->mqes[lcv] != NULL)  /* already full, skip to next one */
            continue;

         /* scan past end of new array for a non-null entry we can pull */
         while (pullfrom < mts->nused && mts->mqes[pullfrom] == NULL)
             pullfrom++;
        if (pullfrom >= mts->nused) {
             mlog(MPI_ERR, "mts: pull overflow - should not happen!");
             new_nused = 0;  /* reset, may leak memory */
             break;
        }

        /* fill the hole, keeping the slot# in sync */
        mts->mqes[lcv] = mts->mqes[pullfrom];
        mts->reqs[lcv] = mts->reqs[pullfrom];
        pullfrom++;      /* consumed, advance to next one */
    }
    mts->nused = new_nused;
    mlog(MPI_DBG, "mts update: nused=%d (got=%d)", mts->nused, got);

    return(0);
}

/*
 * finalize mpimts before exiting.   the mts is no longer active.
 */
static void mpimts_finalize(struct mpimtsmgr *mts) {
    int lcv;

    for (lcv = 0 ; lcv < mts->nused ; lcv++) {
        if (mts->reqs[lcv] != MPI_REQUEST_NULL)
            MPI_Request_free(&mts->reqs[lcv]);  /* ignore failures */
        if (mts->mqes[lcv]) {
            mlog(MPI_DBG, "mts send mqe finalize-drp: mqe=%p", mts->mqes[lcv]);
            mpimts_done(mts->mqes[lcv]);
        }
    }
    mts->nused = 0;
    mpimts_afree(mts);
}

/*
 * MPI_Iprobe says we have data that can be received.
 * allocate mqe, recv the data into it, and queue result
 * for processing by the manager thread.
 * return number of bytes we recv or 0 if we got an error.
 */
static int mpimqerecv(MPI_Status *status) {
    int count, overflow, ret;
    struct dtq_mpiqentry *mqe;

    if (MPI_Get_count(status, MPI_BYTE, &count) != MPI_SUCCESS)
        mlog_exit(1, MPI_CRIT, "MPI_Get_count: unexpectedly failed!?!");
    if (count < 1)
        mlog_exit(1, MPI_CRIT, "MPI_Get_count: impossible count %d", count);
    overflow = (count > DTQ_MAXMSGSIZE) ? count - DTQ_MAXMSGSIZE : 0;

    /* allocate mqe recv buffer ... */
    mqe = dtq_mqe_alloc();
    if (!mqe) {
        /*
         * there's no easy way to recover from this.  we could drop
         * the msg and keep going, but that will just cause problems
         * down the line.   so at this point it's fatal...
         */
        mlog_exit(1, MPI_CRIT, "recv mqe alloc failed!  fatal error.");
    }
    mlog(MPI_DBG, "recv alloc mqe %p for %d bytes", mqe, count);

    /* receive data into our buffer and queue it for processing */
    mqe->flen = count;
    mqe->peer = status->MPI_SOURCE;
    ret = MPI_Recv(mqe->frame, mqe->flen, MPI_BYTE, status->MPI_SOURCE,
                   status->MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (overflow || ret != MPI_SUCCESS) {
        mlog(MPI_CRIT, "MPI_Recv error %d, overflow %d, mqe=%p/%d: mqe",
             ret, overflow, mqe, count);
        dtq_recv_release(mqe);
        return(0);
    }
    mlog(MPI_DBG, "MPI_Recv: recv mqe=%p, success.  queue for mgr", mqe);
    dtq_recv_enqueue(mqe);         /* will notify if needed */
    return(count);
}

/*
 * main routine for the mpi thread.  the main thread becomes the manager
 * thread after it creates us.
 */
void *mpit_main(void *unused) {
    struct mpimtsmgr mts = MPIMTSMGR_INIT;
    int mpiabort = 0;
    struct dtq_mpiqentry *mqe;
    int ret, pflag;
    MPI_Request newreq;
    MPI_Status status;

    pthread_mutex_lock(&dtrs->mpit_slock);

    if (dtrs->mpit_state != MTS_START) {      /* should not happen */
        dtrs->mpit_state = MTS_DONE;
        pthread_cond_broadcast(&dtrs->mpit_scond);
        pthread_mutex_unlock(&dtrs->mpit_slock);
        mlog(MPI_CRIT, "mpit state failure");
        return(NULL);
    }

    /*
     * allocate state for MPI_Testsome management
     */
    if (mpimts_init(&mts, MPIMTS_INITSZ) != 0) {
        /* slot memory allocation failure */
        pthread_cond_broadcast(&dtrs->mpit_scond);
        pthread_mutex_unlock(&dtrs->mpit_slock);
        mlog(MPI_CRIT, "mpimts_init failed (initsz=%d)", MPIMTS_INITSZ);
        return(NULL);
    }

    dtrs->mpit_state = MTS_RUN;
    pthread_cond_broadcast(&dtrs->mpit_scond);
    pthread_mutex_unlock(&dtrs->mpit_slock);

    while (dtrs->mpit_state == MTS_RUN) {    /* main mpi thread loop */

        /*
         * send side: check for complete async MPI_Isend ops using mts.
         * we retire all completed ops (no further processing required).
         */
        if (mpimts_test(&mts) != 0) {
            mlog(MPI_ERR, "mpi_main: mpimts_test failed!  exit.");
            mpiabort++;
            goto done;
        }

        /*
         * send side: check for pending sends and start w/MPI_Isend.
         */
        if ((mqe = dtq_send_dequeue()) != NULL) {

            mlog(MPI_DBG, "deque send mqe %p: MPI_Isend %d bytes %d->%d",
                 mqe, mqe->flen, dtrs->mpi_rank, mqe->peer);
            ret = MPI_Isend(mqe->frame, mqe->flen, MPI_BYTE, mqe->peer,
                            0, MPI_COMM_WORLD, &newreq);

            if (ret != MPI_SUCCESS) {

                mlog(MPI_ERR, "mts mqe Isend-drop: err=%d, mqe=%p", ret, mqe);
                mpimts_done(mqe);

            } else {

                dtrs->mpit_st.isend_cnt++;
                dtrs->mpit_st.isend_bytes += mqe->flen;
                if (mpimts_add(&mts, mqe, newreq) == -1) {
                    mlog(MPI_ERR, "mts mqe add-drop: mqe=%p", mqe);
                    MPI_Request_free(&newreq);
                    mpimts_done(mqe);
                } else {
                    /* mpimts_test will release when complete */
                }

            }

        }

        /*
         * recv side: probe to see if data can be received.
         */
        ret = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD,
                         &pflag, &status);
        dtrs->mpit_st.iprobe_cnt++;

        if (ret != MPI_SUCCESS) {
            mlog(MPI_ERR, "mpi_main: MPI_Iprobe failed %d!  exit.", ret);
            mpiabort++;
            goto done;
        }
        if (pflag) {
            ret = mpimqerecv(&status);  /* recv into mqe and queue */
            if (ret > 0) {
                dtrs->mpit_st.recv_cnt++;
                dtrs->mpit_st.recv_bytes += ret;
            }
        }

        /*
         * since there's no way to properly block and release
         * the CPU waiting for MPI work to do, we just spin.
         * to minimize the impact of the spin on other threads
         * that might have useful work to do, we put a scheduler
         * yield in the loop.
         */
        sched_yield();
    }

done:

    pthread_mutex_lock(&dtrs->mpit_slock);
    mlog(MPI_NOTE, "exit loop, state=%d, mpiabort=%d",
         dtrs->mpit_state, mpiabort);

    mpimts_finalize(&mts);
    dtrs->mpit_state = MTS_DONE;
    pthread_cond_broadcast(&dtrs->mpit_scond);
    pthread_mutex_unlock(&dtrs->mpit_slock);

    if (mpiabort) {
        mlog(MPI_CRIT, "abort mpi job due to error");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    return(NULL);
}

/*
 * tell the mpi thread to stop and then wait for it to actually stop.
 */
void mpit_stop() {
    pthread_mutex_lock(&dtrs->mpit_slock);
    if (dtrs->mpit_state == MTS_RUN) {
        dtrs->mpit_state = MTS_STOP;
        while (dtrs->mpit_state != MTS_DONE) {
            pthread_cond_wait(&dtrs->mpit_scond, &dtrs->mpit_slock);
        }
    }
    pthread_mutex_unlock(&dtrs->mpit_slock);
}
