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

#ifndef _DTHREAD_DTHREAD_H_
#define _DTHREAD_DTHREAD_H_

/*
 * dthread.h  dthread public API
 * 14-Apr-2026  chuck@ece.cmu.edu
 */

#include <inttypes.h>
#include <pthread.h>

#include <dthread/dthread_config.h>

/*
 * dthread types
 */

/*
 * dthread synchronization objects: spin locks, mutexes,
 * cond vars, read/write locks, barriers.   we encapsulate the
 * options in unions configured by compile time options.
 */
#define DTHREAD_SYNCOP_ID_PSHARED 1        /* single machine pthread/pshared */

/* dthread process shared alias.  prob always want this in dthread env */
#define DTHREAD_PROCESS_SHARED PTHREAD_PROCESS_SHARED

typedef union {
#ifdef DTHREAD_SYNC_PSHARED
    pthread_spinlock_t psh_spinlock;
#endif
} dthread_spinlock_t;

typedef union {
#ifdef DTHREAD_SYNC_PSHARED
    pthread_mutex_t psh_mutex;
#endif
} dthread_mutex_t;

typedef union {
#ifdef DTHREAD_SYNC_PSHARED
    pthread_cond_t psh_cond;
#endif
} dthread_cond_t;

typedef union {
#ifdef DTHREAD_SYNC_PSHARED
    pthread_rwlock_t psh_rwlock;
#endif
} dthread_rwlock_t;

typedef union {
#ifdef DTHREAD_SYNC_PSHARED
    pthread_barrier_t psh_barrier;
#endif
} dthread_barrier_t;

/*
 * thread related structures
 */

/*
 * dthread_t is a handle to a dthread that we can use to identify
 * and perform ops on a dthread.   it can be passed between ranks.
 */
typedef struct {
    uint32_t dt_index;           /* index in global thread table */
    uint32_t dt_seq;             /* generation sequence number */
} dthread_t;

/*
 * dthread attributes are used when creating a thread.  we can
 * reuse the pthread_attr_t for this.
 *
 * XXX: should we make dthread-named functions/macros that redirect
 *      to the pthread functions?  or just have apps use the pthread
 *      ones directly?
 */
typedef pthread_attr_t dthread_attr_t;

/*
 * shared memory interface types
 */

/*
 * reference to shared memory area in a shared memory segment.
 * the ref can be passed between ranks.  internally each rank
 * keeps a table mapping shmid to the virtual address it is
 * mapped to in that rank.
 */
typedef struct {
    uint64_t dt_shmid;           /* shm segment id (same across ranks) */
    uint64_t dt_offset;          /* byte offset */
    uint64_t dt_length;          /* length (should not pass bounds) */
} dthread_shmref_t;

#define DTHREAD_SRC_DEV    0x1   /* device file */
#define DTHREAD_SRC_FILE   0x2   /* normal file */
#define DTHREAD_SRC_PSHM   0x3   /* posix shm file */
#define DTHREAD_SRC_MASK   0x3   /* source bitmask */

/*
 * shared memory area source info.  all rank use the same source info
 * to establish shared memory mappings.
 */
typedef struct {
    char *dt_src;                /* label/filename of src (depends on type) */
    uint64_t dt_srcflags;        /* info on how to interpret dt_src */
    uint64_t dt_mmoffset;        /* offset for mmap */
    uint64_t dt_mmsize;          /* size for mmap */
} dthread_shmsrc_t;

/*
 * shared memory malloc operations and malloc metadata
 */

/*
 * shm malloc driver ops.  we support both user-provided and
 * internal operations.   the arena shmref arg points to malloc
 * metadata (see below).
 */
typedef struct {
    char *name;
    int (*init)(dthread_shmref_t *arena);
    int (*finalize)(dthread_shmref_t *arena);
    void *(*malloc)(dthread_shmref_t *arena, size_t size,
                    dthread_shmref_t *newref);
    void (*free)(dthread_shmref_t *arena, dthread_shmref_t *ref);
    void *(*realloc)(dthread_shmref_t *arena, dthread_shmref_t inref,
                     size_t new_size, dthread_shmref_t *newref);
} dthread_shm_alloc_ops_t;

/*
 * malloc metadata lives at the start of the malloc arena.
 * the metadata starts with generic malloc info (described
 * in the structure below).  this structure is typically
 * embedded as the first entry in a malloc driver specific
 * metadata structure.  all fields (except alock) will not
 * change after they are set (at init time).
 */
#define DTHREAD_SHM_MD_MAGIC UINT64_C(0x647468626d643030) /* magic number */

typedef struct {
    uint64_t mdmagic;           /* magic number (set when created) */
    dthread_shmref_t self;      /* our arena (including metadata) */
    uint64_t min_uoffset;       /* min user data offset in arena */
    uint64_t max_uoffset;       /* max user data offset in arena */
    uint64_t badalign_bits;     /* bad bits for proper alignment */
    int mopid;                  /* malloc-ops id (of malloc driver) */
    dthread_spinlock_t alock;   /* alloc md spin lock */
} dthread_shm_alloc_md_t;

/*
 * native dthread arg/return type (argret)
 */

#define DTHREAD_INLINE_SIZE    64    /* max inline arg size, >= 16 */

/* type bits */
#define DTHREAD_NODATA          1    /* no data in argret */
#define DTHREAD_INLINE          2    /* data is inline */
#define DTHREAD_SHMREF          3    /* data is dthread_shmref_t */
#define DTHREAD_CANCELED        4    /* no ret, thread was canceled */

/*
 * argret
 */
typedef struct {
    uint32_t dt_argret_type;               /* type of arg */
    uint32_t dt_inlinelen;                 /* only used if data inline */
    union {                                /* 64 bit alignment, no padding */
        char dt_inline[DTHREAD_INLINE_SIZE];  /* limits max dt_inlinelen */
        dthread_shmref_t dt_shm;
    } u;
} dthread_argret_t;

/*
 * dispatch table types
 */

/* proc ops for dthread_proc_pth_t */
#define DTHREAD_PROC_ENCODE     1    /* encode void* data to an argret */
#define DTHREAD_PROC_DECODE     2    /* decode an argret to void *data */
#define DTHREAD_PROC_FREE       3    /* free argret, void* */

/* main app thread start function (on rank 0) */
typedef int (*dthread_start_app0_t)(int argc, char **argv);

/* native dispatch start function signature */
typedef dthread_argret_t (*dthread_start_t)(dthread_argret_t *dt_arg);

/* pthread-style dispatch start function signature */
typedef void *(*dthread_start_pth_t)(void *arg);

/* argret proc routine for pthread-style dispatch entries, null for native */
typedef int (*dthread_proc_pth_t)(int procop, dthread_argret_t *arg,
                                  void **pth_arg);
/*
 * dispatch table entry
 */
typedef struct {
    char *dt_dispname;                     /* name */
    union {
        dthread_start_app0_t start0;       /* first entry in table */
        dthread_start_t start;             /* native, NULL proc fns */
        dthread_start_pth_t pthstart;      /* pthread-style w/proc fns */
    } st;
    dthread_proc_pth_t dt_argproc;         /* arg proc for pth-style */
    dthread_proc_pth_t dt_retproc;         /* ret proc for pth-style */
} dthread_dispatch_t;

/* reuse pthread attr structs for dthreads XXX */
typedef pthread_mutexattr_t dthread_mutexattr_t;
typedef pthread_condattr_t dthread_condattr_t;
typedef pthread_rwlockattr_t dthread_rwlockattr_t;
typedef pthread_barrierattr_t dthread_barrierattr_t;
/* reuse pthread attr structs for dthreads XXX */

/*
 * macros/function prototypes
 */

/*
 * shared memory references/pointer conversions (within a shmid).
 */

/*
 * turn a shmref into a local pointer.  len is optional (number of
 * bytes the user expects).
 */
void *dthread_shmref2ptr(dthread_shmref_t *refp, uint64_t len);

/*
 * generate a shmref for an area of local memory, if possible.
 */
int dthread_ptr2shmref(void *ptr, uint64_t len, dthread_shmref_t *refp);

/*
 * snapshot of current free space in a segment
 */
int dthread_shm_segavail(uint64_t shmid, dthread_shmref_t *got);

/*
 * create arena for shared memory allocator (directly from shmid)
 */
int dthread_shm_new_arena(uint64_t shmid, char *shmalloc_name,
                          size_t size, dthread_shmref_t *newarena);

/*
 * finalize arena
 *
 * currently finalize does not do anything other than log some stats
 * when logging is enabled (e.g. for debugging), so calling it is
 * not required.
 */
int dthread_shm_finalize_arena(dthread_shmref_t *arena);

/*
 * set default arena (can only be set once)
 */
int dthread_shm_set_defaultarena(dthread_shmref_t *dflt);

/*
 * shared memory malloc.  returns local pointer to allocated memory
 * or NULL on error.  also fills in shmref, if provided.
 */
void *dthread_shm_malloc(dthread_shmref_t *arena, uint64_t len,
                         dthread_shmref_t *newref);

/*
 * shared memory free.
 */
void dthread_shm_free(dthread_shmref_t *arena, void *ptr);

/*
 * shared memory free by shmref.
 */
void dthread_shm_free_ref(dthread_shmref_t *arena, dthread_shmref_t *ref);

/*
 * shared memory realloc
 */
void *dthread_shm_realloc(dthread_shmref_t *arena, void *ptr, uint64_t len,
                          dthread_shmref_t *newref);
/*
 * shared memory realloc by shmref
 */
void *dthread_shm_realloc_ref(dthread_shmref_t *arena, dthread_shmref_t *inref,
                              uint64_t len, dthread_shmref_t *newref);

/*
 * init/startup/shutdown
 */

/*
 * initial init (called early for MPI_Init() call).  return 0 on
 * success, otherwise error number.
 */
int dthread_init(int *argcp, char ***argvp);

/*
 * run (called after init) to bring up internal threads across
 * all ranks and launch application's initial thread on rank 0.
 * the app start function is defined to be entry 0 in the dsps array.
 * we use maxthreads to size thread arrays.  the intent is that
 * the main posix thread calls this, it creates the MPI thread,
 * then (on rank 0) the init app theread, and then the main thread
 * becomes the manager thread.  this function does not return...
 * it either exits when we are done or aborts the MPI on error.
 */
void dthread_run(dthread_dispatch_t *dsps, int ndsps,
                 dthread_shmsrc_t *shms, int nshms,
                 dthread_shm_alloc_ops_t *usrmalloc, int nusrmalloc,
                 int syncop_id, int maxthreads,
                 int argc, char **argv);

/*
 * thread query functions
 */
dthread_t dthread_self(void);

/*
 * thread lifetime functions
 */
/* native API create */
int dthread_ncreate(dthread_t *dthread, dthread_attr_t *attr,
                    dthread_start_t dtstart, dthread_argret_t *arg);

/* pthread-like create */
int dthread_create(dthread_t *dthread, dthread_attr_t *attr,
                   dthread_start_pth_t dtstartpth, void *arg);

/* XXX: do we make dthread_attr fns or just use the pthread ones? */

/* detach */
int dthread_detach(dthread_t thread);

/* cancel */
int dthread_cancel(dthread_t thread);

/* exit */
void dthread_nexit(dthread_argret_t *ret);
void dthread_exit(void *value_ptr);

/* join */
int dthread_join(dthread_t thread, void **retptr);
int dthread_njoin(dthread_t thread, dthread_argret_t *ret);

/*
 * synchronization operations
 */

int dthread_spin_init(dthread_spinlock_t *lock, int pshared);
int dthread_spin_destroy(dthread_spinlock_t *lock);
int dthread_spin_lock(dthread_spinlock_t *lock);
int dthread_spin_trylock(dthread_spinlock_t *lock);
int dthread_spin_unlock(dthread_spinlock_t *lock);

int dthread_mutex_init(dthread_mutex_t *mutex, dthread_mutexattr_t *attr);
int dthread_mutex_destroy(dthread_mutex_t *mutex);
int dthread_mutex_lock(dthread_mutex_t *mutex);
int dthread_mutex_trylock(dthread_mutex_t *mutex);
int dthread_mutex_unlock(dthread_mutex_t *mutex);
int dthread_mutex_timedlock(dthread_mutex_t *mutex,
         const struct timespec *timeout);
/* XXX: pthread_mutex_getprioceiling(), pthread_mutex_setprioceiling() */

int dthread_cond_init(dthread_cond_t *cond, dthread_condattr_t *attr);
int dthread_cond_destroy(dthread_cond_t *cond);
int dthread_cond_broadcast(dthread_cond_t *cond);
int dthread_cond_signal(dthread_cond_t *cond);
int dthread_cond_wait(dthread_cond_t *cond, dthread_mutex_t *mutex);
int dthread_cond_timedwait(dthread_cond_t *cond, dthread_mutex_t *mutex,
         const struct timespec *abstime);

int dthread_rwlock_init(dthread_rwlock_t *lock, dthread_rwlockattr_t *attr);
int dthread_rwlock_destroy(dthread_rwlock_t *lock);
int dthread_rwlock_rdlock(dthread_rwlock_t *lock);
int dthread_rwlock_timedrdlock(dthread_rwlock_t *lock,
         const struct timespec *abstime);
int dthread_rwlock_tryrdlock(dthread_rwlock_t *lock);
int dthread_rwlock_wrlock(dthread_rwlock_t *lock);
int dthread_rwlock_timedwrlock(dthread_rwlock_t *lock,
         const struct timespec *abstime);
int dthread_rwlock_trywrlock(dthread_rwlock_t *lock);
int dthread_rwlock_unlock(dthread_rwlock_t *lock);

int dthread_barrier_init(dthread_barrier_t *barrier,
         dthread_barrierattr_t *attr, unsigned int count);
int dthread_barrier_destroy(dthread_barrier_t *barrier);
int dthread_barrier_wait(dthread_barrier_t *barrier);

#endif /* _DTHREAD_DTHREAD_H_ */
