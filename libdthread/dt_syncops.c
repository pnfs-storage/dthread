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
 * dt_syncops.c  generic sync op code
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
 * configure an array of all enabled syncops
 */
#ifdef DTHREAD_SYNC_PSHARED
extern dthread_syncops_t pshared_syncops;
#endif

static dthread_syncops_t *myops[] = {
#ifdef DTHREAD_SYNC_PSHARED
    &pshared_syncops,
#endif
};

/*
 * return syncop for a given id, or NULL if not found.
 */
dthread_syncops_t *dt_getsyncops(int id) {
    int lcv;
    for ( lcv = 0 ; lcv < sizeof(myops) / sizeof(myops[0]) ; lcv++) {
        if (myops[lcv]->id == id)
            return(myops[lcv]);
    }
    return(NULL);
}

/* generic routines that vector out to the selected syncop */
int dthread_spin_init(dthread_spinlock_t *lock, int pshared) {
    return(dtrs->syops->spin_init(lock, pshared));
}

int dthread_spin_destroy(dthread_spinlock_t *lock) {
    return(dtrs->syops->spin_destroy(lock));
}

int dthread_spin_lock(dthread_spinlock_t *lock) {
    return(dtrs->syops->spin_lock(lock));
}

int dthread_spin_trylock(dthread_spinlock_t *lock) {
    return(dtrs->syops->spin_trylock(lock));
}

int dthread_spin_unlock(dthread_spinlock_t *lock) {
    return(dtrs->syops->spin_unlock(lock));
}

int dthread_mutex_init(dthread_mutex_t *mutex, dthread_mutexattr_t *attr) {
    return(dtrs->syops->mutex_init(mutex, attr));
}

int dthread_mutex_destroy(dthread_mutex_t *mutex) {
    return(dtrs->syops->mutex_destroy(mutex));
}

int dthread_mutex_lock(dthread_mutex_t *mutex) {
    return(dtrs->syops->mutex_lock(mutex));
}

int dthread_mutex_trylock(dthread_mutex_t *mutex) {
    return(dtrs->syops->mutex_trylock(mutex));
}

int dthread_mutex_unlock(dthread_mutex_t *mutex) {
    return(dtrs->syops->mutex_unlock(mutex));
}

int dthread_mutex_timedlock(dthread_mutex_t *mutex,
                            const struct timespec *timeout) {
    return(dtrs->syops->mutex_timedlock(mutex, timeout));
}

int dthread_cond_init(dthread_cond_t *cond, dthread_condattr_t *attr) {
    return(dtrs->syops->cond_init(cond, attr));
}

int dthread_cond_destroy(dthread_cond_t *cond) {
    return(dtrs->syops->cond_destroy(cond));
}

int dthread_cond_broadcast(dthread_cond_t *cond) {
    return(dtrs->syops->cond_broadcast(cond));
}

int dthread_cond_signal(dthread_cond_t *cond) {
    return(dtrs->syops->cond_signal(cond));
}

int dthread_cond_wait(dthread_cond_t *cond, dthread_mutex_t *mutex) {
    return(dtrs->syops->cond_wait(cond, mutex));
}

int dthread_cond_timedwait(dthread_cond_t *cond, dthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    return(dtrs->syops->cond_timedwait(cond, mutex, abstime));
}

int dthread_rwlock_init(dthread_rwlock_t *rwlock, dthread_rwlockattr_t *attr) {
    return(dtrs->syops->rwlock_init(rwlock, attr));
}

int dthread_rwlock_destroy(dthread_rwlock_t *rwlock) {
    return(dtrs->syops->rwlock_destroy(rwlock));
}

int dthread_rwlock_rdlock(dthread_rwlock_t *rwlock) {
    return(dtrs->syops->rwlock_rdlock(rwlock));
}

int dthread_rwlock_timedrdlock(dthread_rwlock_t *rwlock,
                               const struct timespec *abstime) {
    return(dtrs->syops->rwlock_timedrdlock(rwlock, abstime));
}

int dthread_rwlock_tryrdlock(dthread_rwlock_t *rwlock) {
    return(dtrs->syops->rwlock_tryrdlock(rwlock));
}

int dthread_rwlock_wrlock(dthread_rwlock_t *rwlock) {
    return(dtrs->syops->rwlock_wrlock(rwlock));
}

int dthread_rwlock_timedwrlock(dthread_rwlock_t *rwlock,
                               const struct timespec *abstime) {
    return(dtrs->syops->rwlock_timedwrlock(rwlock, abstime));
}

int dthread_rwlock_trywrlock(dthread_rwlock_t *rwlock) {
    return(dtrs->syops->rwlock_trywrlock(rwlock));
}

int dthread_rwlock_unlock(dthread_rwlock_t *rwlock) {
    return(dtrs->syops->rwlock_unlock(rwlock));
}

int dthread_barrier_init(dthread_barrier_t *barrier,
                         dthread_barrierattr_t *attr, unsigned int count) {
    return(dtrs->syops->barrier_init(barrier, attr, count));
}

int dthread_barrier_destroy(dthread_barrier_t *barrier) {
    return(dtrs->syops->barrier_destroy(barrier));
}

int dthread_barrier_wait(dthread_barrier_t *barrier) {
    return(dtrs->syops->barrier_wait(barrier));
}
