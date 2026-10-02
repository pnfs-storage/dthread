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

#ifndef _DTHREAD_DTHREAD_SEGINFO_H_
#define _DTHREAD_DTHREAD_SEGINFO_H_

/*
 * dthread_seginfo.h  dthread seginfo API
 * 02-Oct-2026  chuck@ece.cmu.edu
 */

#include <inttypes.h>

/*
 * this is a diagnostic API used to get internal shm segment info.
 * it is not part of the main public API (and may change), so it is
 * not part of the main <dthread/dthread.h> header.
 */

typedef struct {
    void *si_mapping;        /* segment's mapping on this rank */
    uint64_t si_size;        /* size of segment */
    uint64_t si_mdoffset;    /* metadata structure offset */
    uint64_t si_srcflags;    /* srcflags used to create seg */
    uintptr_t si_umapaddr;   /* UMAP address (or 0 if !umap) */
    uint64_t si_firstavail;  /* snapshot of offset of first available bytes */
    uint64_t si_endavail;    /* snapshot of offset of end of available bytes */
} dthread_seginfo_t;

/*
 * get seginfo
 */
int dthread_shm_seginfo(uint64_t shmid, dthread_seginfo_t *sip);

#endif /* _DTHREAD_DTHREAD_SEGINFO_H_ */
