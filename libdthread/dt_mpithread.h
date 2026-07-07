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

#ifndef _DTHREAD_MPITHREAD_H_
#define _DTHREAD_MPITHREAD_H_

/*
 * dt_mpithread.h  mpi i/o thread interface
 * 06-May-2026  chuck@ece.cmu.edu
 */
#include <inttypes.h>

/*
 * stats collected by mpi thread
 */
typedef struct {
    int mts_maxslots;          /* max number of mpi testsome slots */
    uint64_t mts_ntestsome;    /* number of MPI_Testsome() calls */
    uint64_t mts_got1;         /* #times testsome returned 1 entry */
    uint64_t mts_gotmore;      /* #times testsome return >1 entry */
    uint64_t mts_gotmax;       /* max# entries testsome returned */
    uint64_t isend_cnt;        /* number of MPI_Isend calls */
    uint64_t isend_bytes;      /* number of bytes send with MPI_Isend */
    uint64_t iprobe_cnt;       /* number of MPI_Iprobe calls */
    uint64_t recv_cnt;         /* number of MPI_Recv calls */
    uint64_t recv_bytes;       /* number of bytes recv'd with MPI_Recv */
} mpit_stats_t;

void *mpit_main(void *unused);
void mpit_stop(void);
#endif /* _DTHREAD_MPITHREAD_H_ */
