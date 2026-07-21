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
 * dt_ops.c  general dthread ops
 * 18-Jun-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dt_internal.h"

/*
 * return current thread's handle
 */
dthread_t dthread_self() {
    dthread_t rv;
    dthread_ltab_t *lt;

    /* init rv to an invalid handle in case we get an error */
    rv.dt_index = dtrs->nmaxthread + 1;
    rv.dt_seq = 0;

    /* recover our ltab[] entry using TLS */
    lt = pthread_getspecific(dtrs->ltabkey);
    if (!lt) {
        mlog(DTH_CRIT, "dthread_self: unable to get my ltabkey");
        return(rv);
    }

    if (lt->pth_state != LT_RUNNING) {
        mlog(DTH_CRIT, "dthread_self: non-running thread (%zd/%d/%d)",
             lt - dtrs->ltab, lt->gtab_idx, lt->seq);
        return(rv);
    }

    rv.dt_index = lt->gtab_idx;
    rv.dt_seq = lt->seq;
    return(rv);
}

/*
 * shmref to local rank pointer
 */
void *dthread_shmref2ptr(dthread_shmref_t *refp, uint64_t len) {
    if (refp == NULL)
        return(NULL);
    if (refp->dt_shmid >= dtrs->nshmsrc)
        return(NULL);     /* bad shmid? */
    if (refp->dt_offset >= dtrs->shmmap[refp->dt_shmid].size)
        return(NULL);     /* offset past end of shm? */
    if (refp->dt_length > dtrs->shmmap[refp->dt_shmid].size - refp->dt_offset)
        return(NULL);     /* length past end of shm? */
    if (len && len > refp->dt_length)
        return(NULL);     /* user wants more data than we have */
    return((char *) dtrs->shmmap[refp->dt_shmid].mapping + refp->dt_offset);
}

/*
 * generate a shmref for an area of local memory, if possible.
 * return 0 on sucess, error on failure.
 */
int dthread_ptr2shmref(void *ptr, uint64_t len, dthread_shmref_t *refp) {
    int shmid;
    uint64_t offset;

    /* see if ptr is in a shm region and if so, get the shmid */
    for (shmid = 0 ; shmid < dtrs->nshmsrc ; shmid++) {
        if (ptr >= dtrs->shmmap[shmid].mapping) {
            offset = (char *) ptr - (char *) dtrs->shmmap[shmid].mapping;
            if (offset < dtrs->shmmap[shmid].size)
                break;
        }
    }
    if (shmid >= dtrs->nshmsrc)      /* not a pointer to shm */
        return(EINVAL);

    /* check to make sure ptr+len still fits in the shmid region */
    if (len > dtrs->shmmap[shmid].size - offset)
        return(EINVAL);

     /* everything fits!  fill in the shmref. */
     refp->dt_shmid = shmid;
     refp->dt_offset = offset;
     refp->dt_length = len;
     return(0);
}
