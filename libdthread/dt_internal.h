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

#ifndef _DTHREAD_INTERNAL_H_
#define _DTHREAD_INTERNAL_H_

/*
 * dt_internal.h  dthread internal headers
 * 29-Apr-2026  chuck@ece.cmu.edu
 */
#include <mpi.h>
#include <dthread/dthread.h>

#ifdef DTHREAD_MLOG

#include "dt_mlog.h"

#else

/* macros to null out normal mlog calls when DTHREAD_MLOG is not defined */
#include <err.h>
#define mlog(LEVEL, ...)
#define mlog_abort(LEVEL, ...) do {                                           \
    warnx(__VA_ARGS__);                                                       \
    abort();                                                                  \
} while(0);
#define mlog_exit(STATUS, LEVEL, ...) do {                                    \
    errx(STATUS, __VA_ARGS__);                                                \
} while(0);

#endif

#include "dt_mgrthread.h"
#include "dt_mpithread.h"
#include "dt_queuing.h"


/* for avoiding -Wunused-but-set-variable warnings for #if'd out code  */
#define DT_UNUSED(VAR) (void)(VAR)

/*
 * shared memory
 */
/*
 * we keep an array indexed by shmid to map it to its local mapping
 * on the current rank.
 */
typedef struct {
    void *mapping;          /* virtual address we are mapped to in this rank */
    uint64_t size;          /* size of mapping */
} dthread_shmmap_t;

/*
 * shared memory segment metadata.
 */
#define DTHREAD_SEG_MD_MAGIC UINT64_C(0x64746873676d6431) /* magic number */

typedef struct {
    uint64_t seg_md_magic;      /* magic number (set when created) */
    dthread_shmref_t self;      /* describes the entire block */
    uint64_t allocated;         /* bytes allocated in block (optional) */
    dthread_spinlock_t slock;   /* low-level spin lock */
} dthread_shmseg_md_t;

/*
 * thread structures
 */

/*
 * global thread table entry (in shared memory).  indexed by dt_index.
 */
typedef struct {
    uint32_t allocated;    /* set if rank0 has allocated this slot */
    uint32_t rank;         /* rank assigned to thread by rank0 */
    uint32_t seq;          /* seq number set by r0 (matches dthread_t) */
    uint32_t disp_idx;     /* dispatch table index */
    dthread_argret_t arg;  /* thread input arg */
    dthread_argret_t ret;  /* thread return value */
    uint32_t detached;     /* dthread detached? */
    uint32_t terminated;   /* thread terminated, final state saved here */
    uint32_t has_ltab;     /* set by local rank after ltab_idx is alloced */
    uint32_t ltab_idx;     /* index into ltab on the rank we run on */
} dthread_gtab_t;

/*
 * state values for pth_state
 */
#define LT_NONE      0     /* no valid thread running in slot */
#define LT_STARTING  1     /* mgr allocated slot, but thread not up yet */
#define LT_RUNNING   2     /* pth valid and running */
#define LT_RETURNED  3     /* thread returned to caller */
#define LT_EXITED    4     /* thread called dthread_exit()-like fn */
#define LT_CANCELED  5     /* thread has been canceled */
#define LT_CLEANUP   6     /* thread exited via cleanup */

#define LT_NEED_FINALSTATE(X) ((X) <= LT_RUNNING)  /* need final state? */

/*
 * local thread table entry (private to rank).  index w/ltab_idx.
 */
typedef struct {
    uint32_t gtab_idx;     /* back pointer to gtab */
    uint32_t seq;          /* seq number (to match with dthread_t) */
    pthread_t pth;         /* local pthread running our dthread */
    uint32_t pth_state;    /* pth state (LT_*), use reqlock if thread active */
    dthread_request_t req; /* request for this local thread */
} dthread_ltab_t;

/*
 * state values for mpi thread
 */
#define MTS_INIT  0        /* mpi thread has not been started */
#define MTS_START 1        /* mpi thread started, not fully up */
#define MTS_RUN   2        /* mpi thread running */
#define MTS_STOP  3        /* mpi thread told to stop */
#define MTS_DONE  4        /* mpi thread completed */

/*
 * synchronization operation implementations
 */
typedef struct {
    int id;                /* id of these ops */
    char *name;            /* name (mainly for debug/logging) */
    /* XXX: do we need a variable for flags? */

    int (*spin_init)(dthread_spinlock_t *lock, int pshared);
    int (*spin_destroy)(dthread_spinlock_t *lock);
    int (*spin_lock)(dthread_spinlock_t *lock);
    int (*spin_trylock)(dthread_spinlock_t *lock);
    int (*spin_unlock)(dthread_spinlock_t *lock);

    int (*mutex_init)(dthread_mutex_t *mutex, dthread_mutexattr_t *attr);
    int (*mutex_destroy)(dthread_mutex_t *mutex);
    int (*mutex_lock)(dthread_mutex_t *mutex);
    int (*mutex_trylock)(dthread_mutex_t *mutex);
    int (*mutex_unlock)(dthread_mutex_t *mutex);
    int (*mutex_timedlock)(dthread_mutex_t *mutex,
             const struct timespec *timeout);
    /* XXX: mutex_getprioceiling(), mutex_setprioceiling() */

    int (*cond_init)(dthread_cond_t *cond, dthread_condattr_t *attr);
    int (*cond_destroy)(dthread_cond_t *cond);
    int (*cond_broadcast)(dthread_cond_t *cond);
    int (*cond_signal)(dthread_cond_t *cond);
    int (*cond_wait)(dthread_cond_t *cond, dthread_mutex_t *mutex);
    int (*cond_timedwait)(dthread_cond_t *cond, dthread_mutex_t *mutex,
             const struct timespec *abstime);

    int (*rwlock_init)(dthread_rwlock_t *lock, dthread_rwlockattr_t *attr);
    int (*rwlock_destroy)(dthread_rwlock_t *lock);
    int (*rwlock_rdlock)(dthread_rwlock_t *lock);
    int (*rwlock_timedrdlock)(dthread_rwlock_t *lock,
             const struct timespec *abstime);
    int (*rwlock_tryrdlock)(dthread_rwlock_t *lock);
    int (*rwlock_wrlock)(dthread_rwlock_t *lock);
    int (*rwlock_timedwrlock)(dthread_rwlock_t *lock,
             const struct timespec *abstime);
    int (*rwlock_trywrlock)(dthread_rwlock_t *lock);
    int (*rwlock_unlock)(dthread_rwlock_t *lock);

    int (*barrier_init)(dthread_barrier_t *barrier,
             dthread_barrierattr_t *attr, unsigned int count);
    int (*barrier_destroy)(dthread_barrier_t *barrier);
    int (*barrier_wait)(dthread_barrier_t *barrier);

} dthread_syncops_t;

/*
 * overall dthread rank state
 */
typedef struct {
    /* this block is setup by dthread_init() */
    char *prog;                  /* orig argv[0] */
    int mpi_rank;                /* my rank number */
    int mpi_wsize;               /* size of MPI job */
    uint32_t seqsrc;             /* seq/gen# source */
    int pagesize;                /* system page size */
    pthread_key_t ltabkey;       /* key to our ltab entry */

    dthread_dispatch_t *dsps;    /* dispatch table */
    int ndsps;                   /* size of dispatch table */

    dthread_shmsrc_t *shmsrc;    /* array of shared memory sources */
    dthread_shmmap_t *shmmap;    /* mappings for the above */
    int nshmsrc;                 /* size of shm array */
    pthread_mutex_t arenalock;   /* lock on default arena setting */
    dthread_shmref_t *defarena;  /* default arena (or NULL if not set) */
    dthread_shmref_t da_store;   /* default arena storage */

    dthread_shm_alloc_ops_t *mop;/* user-provided mallocs */
    int nusrmops;                /* size of the above array */

    int nmaxthread;              /* alloc size of the following tables */
    dthread_gtab_t *gtab;        /* global thread table (in shm) */
    dthread_ltab_t *ltab;        /* local threaad table (in rank memory) */

    pthread_mutex_t mpit_slock;  /* lock on mpi thread state */
    pthread_cond_t mpit_scond;   /* wait on state change here */
    pthread_t mpit_pthread;      /* handle to internal MPI thread */
    int mpit_state;              /* state of mpi thread */
    mpit_stats_t mpit_st;        /* stats */

    struct dtq_state dtq;        /* local thread queues */

    dthread_syncops_t *syops;    /* synchronication ops */
} dthread_dtrs_t;

/* non-zero defaults that are static */
#define DTHREAD_DTRS_INIT                                                     \
    (dthread_dtrs_t) {                                                        \
        .arenalock = PTHREAD_MUTEX_INITIALIZER,                               \
        .mpit_slock = PTHREAD_MUTEX_INITIALIZER,                              \
        .mpit_scond = PTHREAD_COND_INITIALIZER, .mpit_state = MTS_INIT        \
    }

/*
 * this rank's state lives in 'internal_dthread_rank_state' which
 * is a global variable.   for internal use we provide a shorter const
 * pointer 'dtrs' alias for this structure to save on typing.
 * since there should be only one of these per process, this save
 * us from having to pass around our state in function args (and
 * lets us more closely match the pthread api).
 */
extern dthread_dtrs_t internal_dthread_rank_state;
static dthread_dtrs_t *const dtrs = &internal_dthread_rank_state;  /* alias */

/*
 * shared memory segment ops
 */

/*
 * establish shared memory segment mappings.   called by each rank at
 * startup time.   mappings are retained in an internal dthread_shmmap_t
 * table.   return 0 on success, error number on error.
 */
int dthread_shmseg_establish(dthread_shmsrc_t *shmsrctab, int n);

/*
 * low-level segment memory allocator.
 */
void *dthread_shmseg_alloc(uint64_t shmid, uint64_t want,
                           dthread_shmref_t *ref);

/*
 * get a snapshot of how much space is available in a shmseg
 */
int dthread_shmseg_avail(uint64_t shmid, dthread_shmref_t *got);

/*
 * syncops
 */
dthread_syncops_t *dt_getsyncops(int id);

#endif /* _DTHREAD_INTERNAL_H_ */
