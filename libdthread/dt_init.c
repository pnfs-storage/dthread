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
 * dt_init.c  dthread init routines
 * 14-Apr-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>   /* for strgen() */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MKMLOGHDR_DEF_FNAMES
#include "dt_internal.h"

/*
 * static bootstrap segment allocations made by rank 0 before the system
 * is fully up.
 */
typedef struct {
    dthread_shmref_t boot_gtab;       /* global thread table */
} dthread_shm_bootstrap_t;

/*
 * setup the rank's internal state info.  we'll normally access this
 * using the private const pointer 'dtrs' defined in dt_internal.h.
 */
dthread_dtrs_t internal_dthread_rank_state = DTHREAD_DTRS_INIT;

#ifdef DTHREAD_MLOG
/*
 * generate a newly malloced string from the given parts.
 * part list must end in a NULL (for stdarg to know when to stop).
 * return size of malloced buffer (including the \0), or 0 on error.
 * caller is responsible for freeing the returned string.
 */
static size_t strgen(char **newstr, ...) {
    va_list ap;
    size_t newlen;
    char *n, *nstr, *part;

    /* pass 1: compute length we need */
    va_start(ap, newstr);
    newlen = 1;   /* include byte for null termination */
    while ((part = va_arg(ap, char *)) != NULL) {
         newlen += strlen(part);
    }
    va_end(ap);

    /* pass 2: malloc and assemble new string */
    n = nstr = malloc(newlen);    /* includes space for null */
    if (!nstr)
        return(0);
    va_start(ap, newstr);
    while ((part = va_arg(ap, char *)) != NULL) {
        while (*part) {
            *n++ = *part++;
        }
    }
    va_end(ap);
    *n = '\0';

    *newstr = nstr;
    return(newlen);
}
#endif

/*
 * shared memory bootstrap.  currently we just setup the global
 * thread table.   we do all allocations through segment 0.
 */
static int dthread_shm_bootstrap(dthread_shm_bootstrap_t *sboot,
                                 int maxthreads) {
    void *ptr;
    uint64_t gtsize;

    gtsize = maxthreads * sizeof(dtrs->gtab[0]);
    ptr = dthread_shmseg_alloc(0, gtsize, &sboot->boot_gtab);
    if (ptr == NULL)
        return(ENOMEM);
    memset(ptr, 0, gtsize);

    return(0);
}

/*
 * dthread_init: fire up MPI and init our rank state.   dtrs was
 * init'd at compile time in dt_internal.h.  we just need to handle
 * MPI now.  the remaining setup will be handled later when dthread_run()
 * is called.
 */
int dthread_init(int *argcp, char ***argvp) {
    int rv;

    if (dtrs->mpi_wsize)            /* in case we've already been called */
        return(EBUSY);

    dtrs->prog = *(argvp[0]);

    /* none of this should fail... if it does, it's fatal. */
    if (MPI_Init(argcp, argvp))
        return(EIO);
    if (MPI_Comm_rank(MPI_COMM_WORLD, &dtrs->mpi_rank) ||
        MPI_Comm_size(MPI_COMM_WORLD, &dtrs->mpi_wsize)) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    dtrs->seqsrc = dtrs->mpi_rank + dtrs->mpi_wsize;  /* incr by wsize */
    dtrs->pagesize = getpagesize();
    rv = pthread_key_create(&dtrs->ltabkey, NULL);
    if (rv)   /* should not ever fail... */
        errx(1, "dthread_init: key create: %s", strerror(rv));

    return(0);
}

/*
 * dthread_run: called by the app's main thread.  we finish dtrs init.
 * we launch the mpi thread on all ranks.  on rank 0 we also launch the
 * application's initial (using the first entry in the dispatch table).
 * then we become the manager thread for this rank.   we use maxthreads
 * to size thread arrays. this function does not return: it either exits
 * when we are done or aborts the MPI job on error.
 */
void dthread_run(dthread_dispatch_t *dsps, int ndsps,
                 dthread_shmsrc_t *shms, int nshms,
                 dthread_shm_alloc_ops_t *usrmops, int nusrmops,
                 int syncop_id, int maxthreads,
                 int argc, char **argv) {
    int lcv, rv, success, total;
    dthread_shm_bootstrap_t shmboot;
    char *prog, *cp;
#ifdef DTHREAD_MLOG
    char *mlog_log, *mlog_tag;
    int mlog_bufsz;
    char *mlog_logdir = getenv("DTHREAD_MLOG_DIR");
    char *mlog_defpri = getenv("DTHREAD_MLOG_DEFPRI");
    char *mlog_stderrpri = getenv("DTHREAD_MLOG_STDERRPRI");
    char *mlog_bufsz_str = getenv("DTHREAD_MLOG_BUFSZ");
    char *mlog_mask = getenv("DTHREAD_MLOG_MASK");
    char ranktag[32];

    /* open mlog */
    if (mlog_defpri == NULL)
        mlog_defpri = "WARN";
    if (mlog_stderrpri == NULL)
        mlog_stderrpri = "CRIT";
    mlog_bufsz = (mlog_bufsz_str) ? atoi(mlog_bufsz_str) : 4096;
    mlog_log = NULL;
#endif

    prog = dtrs->prog;
    /* limit prog to its basename, advance past any path before that */
    if ((cp = strrchr(prog, '/')) != NULL)
        prog = cp + 1;

#ifdef DTHREAD_MLOG
    snprintf(ranktag, sizeof(ranktag), "%d", dtrs->mpi_rank);
    if (mlog_logdir && strgen(&mlog_log, mlog_logdir, "/dthread.", ranktag,
                              ".log", NULL) < 1) {
        warnx("strgen failed");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    for (lcv = 0 ; mkmloghdr_facdef[lcv].sname != NULL ; lcv++) {
        /*null*/;
    }
    if (strgen(&mlog_tag, prog, ranktag, NULL) < 1)
        errx(1, "unable to generate mlog_tag!");
    if (mlog_open(mlog_tag, lcv, mlog_str2pri(mlog_defpri),
                  mlog_str2pri(mlog_stderrpri), mlog_log,
                  mlog_bufsz, MLOG_LOGPID|MLOG_OUTTTY, 0) != 0) {
        warnx("unable to open log");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    free(mlog_tag);
    for (lcv = 0 ; mkmloghdr_facdef[lcv].sname != NULL ; lcv++) {
        if (mlog_namefacility(lcv, mkmloghdr_facdef[lcv].sname,
                              mkmloghdr_facdef[lcv].lname) != 0) {
            warnx("unable to name mlog facility!");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
    if (mlog_mask)
        mlog_setmasks(mlog_mask, -1);
#endif

    /* mlog() calls are OK from this point on */
    mlog(DTH_INFO, "mlog opened");

    /* check syncop_id value */
    dtrs->syops = dt_getsyncops(syncop_id);
    if (!dtrs->syops) {
        if (!dtrs->mpi_rank)
            fprintf(stderr, "dthread_run: bad syncop id %d\n", syncop_id);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* check maxthreads value */
    if (maxthreads < 1) {
        if (!dtrs->mpi_rank)
            fprintf(stderr, "dthread_run: bad maxthreads %d\n", maxthreads);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* install dispatch table, must be at least 1 entry for startup0 */
    if (ndsps < 1) {
        if (!dtrs->mpi_rank)
            fprintf(stderr, "dthread_run: bad ndsps %d\n", ndsps);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    dtrs->dsps = dsps;
    dtrs->ndsps = ndsps;

    /* install shared memory sources */
    if (nshms < 1) {
        if (!dtrs->mpi_rank)
            fprintf(stderr, "dthread_run: bad nshms %d\n", nshms);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    dtrs->shmsrc = shms;
    dtrs->nshmsrc = nshms;
    dtrs->shmmap = calloc(nshms, sizeof(*dtrs->shmmap));
    if (!dtrs->shmmap) {
        fprintf(stderr, "dthread_run: %d: malloc shmmap\n", dtrs->mpi_rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    /* MPI_Bcast distributes shmboot, acts as a barrier to let r0 go first */
    rv = 0;                            /* quiet compiler warning */
    if (dtrs->mpi_rank == 0) {
        rv = dthread_shmseg_establish(shms, nshms);
        if (rv == 0) {
            rv = dthread_shm_bootstrap(&shmboot, maxthreads);
        }
    }
    if (MPI_Bcast(&shmboot, sizeof(shmboot), MPI_BYTE, 0, MPI_COMM_WORLD)) {
        fprintf(stderr, "dthread_run: MPI_Bcast failed?\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    if (dtrs->mpi_rank != 0) {
        rv = dthread_shmseg_establish(shms, nshms);
    }
    success = (rv == 0) ? 1 : 0;
    rv = MPI_Allreduce(&success, &total, 1, MPI_INT,
                       MPI_SUM, MPI_COMM_WORLD);
    if (rv) {
        if (!dtrs->mpi_rank)
            fprintf(stderr, "dthread_run: allreduce err %d\n", rv);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    if (total < dtrs->mpi_wsize) {
        if (!dtrs->mpi_rank)
            fprintf(stderr, "dthread_run: shm_establish: %d short\n",
                    dtrs->mpi_wsize - total);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* install user-provided mallocs, if any */
    if (usrmops && nusrmops > 0) {
        dtrs->mop = usrmops;
        dtrs->nusrmops = nusrmops;
    } else {
        dtrs->mop = NULL;
        dtrs->nusrmops = 0;
    }

    /* setup the thread tables */
    dtrs->nmaxthread = maxthreads;
    dtrs->gtab = dthread_shmref2ptr(&shmboot.boot_gtab, 0);
    dtrs->ltab = calloc(maxthreads, sizeof(dtrs->ltab[0]));
    if (dtrs->gtab == NULL || dtrs->ltab == NULL) {
        fprintf(stderr, "dthread_run: %d: thread table error (%p,%p)\n",
                dtrs->mpi_rank, dtrs->gtab, dtrs->ltab);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    for (lcv = 0 ; lcv < dtrs->nmaxthread ; lcv++) {
        dtrs->ltab[lcv].req.req_ltabidx = lcv;   /* back pointer */
        pthread_mutex_init(&dtrs->ltab[lcv].req.reqlock, NULL);
        pthread_cond_init(&dtrs->ltab[lcv].req.reqnotify, NULL);
    }

    /* init queuing structures */
    if (dtq_init() < 0) {
        fprintf(stderr, "dthread_run: %d: dtq_init fail\n", dtrs->mpi_rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* create the mpi thread */
    pthread_mutex_lock(&dtrs->mpit_slock);
    rv = pthread_create(&dtrs->mpit_pthread, NULL, mpit_main, NULL);
    if (rv) {
        fprintf(stderr, "dthread_run: %d: mpit create %d\n",
                dtrs->mpi_rank, rv);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    dtrs->mpit_state = MTS_START;
    while (dtrs->mpit_state == MTS_START) {
        pthread_cond_wait(&dtrs->mpit_scond, &dtrs->mpit_slock);
    }
    if (dtrs->mpit_state != MTS_RUN) {
        fprintf(stderr, "dthread_run: %d: mpit state error %d\n",
                dtrs->mpi_rank, dtrs->mpit_state);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    pthread_mutex_unlock(&dtrs->mpit_slock);

    /*
     * the main thread of this rank is now ready to be the manager thread.
     * the manager on rank 0 will launch the main (initial) app thread using
     * the first entry of the dispatch table.
     */
    exit(mgr_main(argc, argv));
}
