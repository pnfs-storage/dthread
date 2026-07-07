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
 * dt_syncops_pshared.c  pthreads-based PSHARED sync ops
 * 30-Jun-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dt_internal.h"

/*
 * the pshared syncops are going to be simple since the dthread
 * api is aligned with the pthread API.  but we do want to ensure
 * that PSHARED mode is on, as using a PTHREAD_PROCESS_PRIVATE-like mode
 * with dthreads does not make any sense in the distributed context...
 *
 * unfortunately there are no pthread pre-processor initializer macros
 * (e.g. like PTHREAD_MUTEX_INITIALIZER) that inits objects in PSHARED mode,
 * so we need to force users to use the init functions with the
 * PSHARED attribute set.   this is annoying, but pshared mode is
 * mainly for testing, so we will just live with it.
 */

/* spin locks */
static int pshared_spin_init(dthread_spinlock_t *lock, int pshared) {
    pshared = PTHREAD_PROCESS_SHARED;   /* ignore user provided value */

    return(pthread_spin_init(&lock->psh_spinlock, pshared));
}

static int pshared_spin_destroy(dthread_spinlock_t *lock) {
    return(pthread_spin_destroy(&lock->psh_spinlock));
}

static int pshared_spin_lock(dthread_spinlock_t *lock) {
    return(pthread_spin_lock(&lock->psh_spinlock));
}

static int pshared_spin_trylock(dthread_spinlock_t *lock) {
    return(pthread_spin_trylock(&lock->psh_spinlock));
}

static int pshared_spin_unlock(dthread_spinlock_t *lock) {
    return(pthread_spin_unlock(&lock->psh_spinlock));
}

/* mutex */
static int pshared_mutex_init(dthread_mutex_t *mutex,
                              dthread_mutexattr_t *attr) {
    dthread_mutexattr_t defat;
    int rv, ps;

    /* setup a pshared attr if none provided */
    if (attr == NULL) {

        if ((rv = pthread_mutexattr_init(&defat)) < 0)
            return(rv);
        rv = pthread_mutexattr_setpshared(&defat, PTHREAD_PROCESS_SHARED);
        if (rv < 0) {
            pthread_mutexattr_destroy(&defat);
            return(rv);
        }
        attr = &defat;

    } else {

        /* verify pshared mode, set if not */
        rv = pthread_mutexattr_getpshared(attr, &ps);
        if (ps != PTHREAD_PROCESS_SHARED) {
            rv = pthread_mutexattr_setpshared(attr, PTHREAD_PROCESS_SHARED);
            if (rv < 0)
                return(rv);
        }

    }

    rv = pthread_mutex_init(&mutex->psh_mutex, attr);

   if (attr == &defat) {
       pthread_mutexattr_destroy(&defat);
   }

   return(rv);
}

static int pshared_mutex_destroy(dthread_mutex_t *lock) {
    return(pthread_mutex_destroy(&lock->psh_mutex));
}

static int pshared_mutex_lock(dthread_mutex_t *lock) {
    return(pthread_mutex_lock(&lock->psh_mutex));
}

static int pshared_mutex_trylock(dthread_mutex_t *lock) {
    return(pthread_mutex_trylock(&lock->psh_mutex));
}

static int pshared_mutex_unlock(dthread_mutex_t *lock) {
    return(pthread_mutex_unlock(&lock->psh_mutex));
}

static int pshared_mutex_timedlock(dthread_mutex_t *mutex,
                                   const struct timespec *timeout) {
    return(pthread_mutex_timedlock(&mutex->psh_mutex, timeout));
}

/* cond var */
static int pshared_cond_init(dthread_cond_t *cond,
                              dthread_condattr_t *attr) {
    dthread_condattr_t defat;
    int rv, ps;

    /* setup a pshared attr if none provided */
    if (attr == NULL) {

        if ((rv = pthread_condattr_init(&defat)) < 0)
            return(rv);
        rv = pthread_condattr_setpshared(&defat, PTHREAD_PROCESS_SHARED);
        if (rv < 0) {
            pthread_condattr_destroy(&defat);
            return(rv);
        }
        attr = &defat;

    } else {

        /* verify pshared mode, set if not */
        rv = pthread_condattr_getpshared(attr, &ps);
        if (ps != PTHREAD_PROCESS_SHARED) {
            rv = pthread_condattr_setpshared(attr, PTHREAD_PROCESS_SHARED);
            if (rv < 0)
                return(rv);
        }

    }

    rv = pthread_cond_init(&cond->psh_cond, attr);

   if (attr == &defat) {
       pthread_condattr_destroy(&defat);
   }

   return(rv);
}

static int pshared_cond_destroy(dthread_cond_t *cond) {
    return(pthread_cond_destroy(&cond->psh_cond));
}

static int pshared_cond_broadcast(dthread_cond_t *cond) {
    return(pthread_cond_broadcast(&cond->psh_cond));
}

static int pshared_cond_signal(dthread_cond_t *cond) {
    return(pthread_cond_signal(&cond->psh_cond));
}

static int pshared_cond_wait(dthread_cond_t *cond, dthread_mutex_t *mutex) {
    return(pthread_cond_wait(&cond->psh_cond, &mutex->psh_mutex));
}

static int pshared_cond_timedwait(dthread_cond_t *cond, dthread_mutex_t *mutex,
                                  const struct timespec *abstime) {
    return(pthread_cond_timedwait(&cond->psh_cond, &mutex->psh_mutex, abstime));
}

/* rwlock */
static int pshared_rwlock_init(dthread_rwlock_t *rwlock,
                              dthread_rwlockattr_t *attr) {
    dthread_rwlockattr_t defat;
    int rv, ps;

    /* setup a pshared attr if none provided */
    if (attr == NULL) {

        if ((rv = pthread_rwlockattr_init(&defat)) < 0)
            return(rv);
        rv = pthread_rwlockattr_setpshared(&defat, PTHREAD_PROCESS_SHARED);
        if (rv < 0) {
            pthread_rwlockattr_destroy(&defat);
            return(rv);
        }
        attr = &defat;

    } else {

        /* verify pshared mode, set if not */
        rv = pthread_rwlockattr_getpshared(attr, &ps);
        if (ps != PTHREAD_PROCESS_SHARED) {
            rv = pthread_rwlockattr_setpshared(attr, PTHREAD_PROCESS_SHARED);
            if (rv < 0)
                return(rv);
        }

    }

    rv = pthread_rwlock_init(&rwlock->psh_rwlock, attr);

   if (attr == &defat) {
       pthread_rwlockattr_destroy(&defat);
   }

   return(rv);
}

static int pshared_rwlock_destroy(dthread_rwlock_t *rwlock) {
    return(pthread_rwlock_destroy(&rwlock->psh_rwlock));
}

static int pshared_rwlock_rdlock(dthread_rwlock_t *rwlock) {
    return(pthread_rwlock_rdlock(&rwlock->psh_rwlock));
}

static int pshared_rwlock_timedrdlock(dthread_rwlock_t *rwlock,
                                      const struct timespec *abstime) {
    return(pthread_rwlock_timedrdlock(&rwlock->psh_rwlock, abstime));
}

static int pshared_rwlock_tryrdlock(dthread_rwlock_t *rwlock) {
    return(pthread_rwlock_tryrdlock(&rwlock->psh_rwlock));
}

static int pshared_rwlock_wrlock(dthread_rwlock_t *rwlock) {
    return(pthread_rwlock_wrlock(&rwlock->psh_rwlock));
}

static int pshared_rwlock_timedwrlock(dthread_rwlock_t *rwlock,
                                      const struct timespec *abstime) {
    return(pthread_rwlock_timedwrlock(&rwlock->psh_rwlock, abstime));
}

static int pshared_rwlock_trywrlock(dthread_rwlock_t *rwlock) {
    return(pthread_rwlock_trywrlock(&rwlock->psh_rwlock));
}

static int pshared_rwlock_unlock(dthread_rwlock_t *rwlock) {
    return(pthread_rwlock_unlock(&rwlock->psh_rwlock));
}

/* barrier */
static int pshared_barrier_init(dthread_barrier_t *barrier,
                              dthread_barrierattr_t *attr,
                              unsigned int count) {
    dthread_barrierattr_t defat;
    int rv, ps;

    /* setup a pshared attr if none provided */
    if (attr == NULL) {

        if ((rv = pthread_barrierattr_init(&defat)) < 0)
            return(rv);
        rv = pthread_barrierattr_setpshared(&defat, PTHREAD_PROCESS_SHARED);
        if (rv < 0) {
            pthread_barrierattr_destroy(&defat);
            return(rv);
        }
        attr = &defat;

    } else {

        /* verify pshared mode, set if not */
        rv = pthread_barrierattr_getpshared(attr, &ps);
        if (ps != PTHREAD_PROCESS_SHARED) {
            rv = pthread_barrierattr_setpshared(attr, PTHREAD_PROCESS_SHARED);
            if (rv < 0)
                return(rv);
        }

    }

    rv = pthread_barrier_init(&barrier->psh_barrier, attr, count);

   if (attr == &defat) {
       pthread_barrierattr_destroy(&defat);
   }

   return(rv);
}

static int pshared_barrier_destroy(dthread_barrier_t *barrier) {
    return(pthread_barrier_destroy(&barrier->psh_barrier));
}

static int pshared_barrier_wait(dthread_barrier_t *barrier) {
    return(pthread_barrier_wait(&barrier->psh_barrier));
}

dthread_syncops_t pshared_syncops = {
    .id = DTHREAD_SYNCOP_ID_PSHARED,
    .name = "pshared",

    .spin_init = pshared_spin_init, .spin_destroy = pshared_spin_destroy,
    .spin_lock = pshared_spin_lock, .spin_trylock = pshared_spin_trylock,
    .spin_unlock = pshared_spin_unlock,

    .mutex_init = pshared_mutex_init, .mutex_destroy = pshared_mutex_destroy,
    .mutex_lock = pshared_mutex_lock, .mutex_trylock = pshared_mutex_trylock,
    .mutex_unlock = pshared_mutex_unlock,
    .mutex_timedlock = pshared_mutex_timedlock,

    .cond_init = pshared_cond_init, .cond_destroy = pshared_cond_destroy,
    .cond_broadcast = pshared_cond_broadcast,
    .cond_signal = pshared_cond_signal, .cond_wait = pshared_cond_wait,
    .cond_timedwait = pshared_cond_timedwait,

    .rwlock_init = pshared_rwlock_init,
    .rwlock_destroy = pshared_rwlock_destroy,
    .rwlock_rdlock = pshared_rwlock_rdlock,
    .rwlock_timedrdlock = pshared_rwlock_timedrdlock,
    .rwlock_tryrdlock = pshared_rwlock_tryrdlock,
    .rwlock_wrlock = pshared_rwlock_wrlock,
    .rwlock_timedwrlock = pshared_rwlock_timedwrlock,
    .rwlock_trywrlock = pshared_rwlock_trywrlock,
    .rwlock_unlock = pshared_rwlock_unlock,

    .barrier_init = pshared_barrier_init,
    .barrier_destroy = pshared_barrier_destroy,
    .barrier_wait = pshared_barrier_wait,
};
