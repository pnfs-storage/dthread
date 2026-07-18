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
 * dt_segops.c  dthread shared memory segment (shmseg) ops
 * 16-Jul-2026  chuck@ece.cmu.edu
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include "dt_internal.h"

static int establish(dthread_shmsrc_t *src, int idx);

/*
 * establish shared memory segment mappings.   return 0 on success,
 * error otherwise.
 */
int dthread_shmseg_establish(dthread_shmsrc_t *shmsrctab, int n) {
    int lcv, rv;
    dthread_shmseg_md_t *md;
    for (lcv = 0 ; lcv < n ; lcv++) {
        rv = establish(&shmsrctab[lcv], lcv);
        if (rv)
            return(rv);
        /* rank 0 must setup the metadata structure */
        if (dtrs->mpi_rank == 0) {
            md = (dthread_shmseg_md_t *) dtrs->shmmap[lcv].mapping;
            md->seg_md_magic = DTHREAD_SEG_MD_MAGIC;
            md->self.dt_shmid = lcv;
            md->self.dt_offset = 0;
            md->self.dt_length = dtrs->shmmap[lcv].size;
            md->allocated = dtrs->pagesize;  /* reserve first page in SEG */
            /*
             * XXX: assume we can init md spinlock before shm malloc avail.
             * if this does not work, we could use the remaining space
             * in segment md page to help setup the spinlock?
             */
            rv = dthread_spin_init(&md->slock, DTHREAD_PROCESS_SHARED);
            if (rv)
                return(rv);
        }
    }
    return(0);
}

/*
 * establish one shared memory mapping.
 */
static int establish(dthread_shmsrc_t *src, int idx) {
    int rank = dtrs->mpi_rank;
    int type = src->dt_srcflags & DTHREAD_SRC_MASK;
    int fd, rv, flags;
    void *mapping;
    struct stat st;

    /*
     * ensure we have at least 2 pages in a shmsrc
     */
    if (src->dt_mmsize < 2 * dtrs->pagesize) {
        mlog(SHM_ERR, "seg_establish: %s: segment too small", src->dt_src);
        return(EINVAL);
    }

    /*
     * device files are easy, we just mmap them.
     */
    if (type == DTHREAD_SRC_DEV) {
        fd = open(src->dt_src, O_RDWR);
        if (fd < 0) {
            rv = errno;
            mlog(SHM_ERR, "seg_establish: DEV: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        mapping = mmap(NULL, src->dt_mmsize, PROT_READ|PROT_WRITE, MAP_SHARED,
                       fd, src->dt_mmoffset);
        rv = (mapping == MAP_FAILED) ? errno : 0;
        close(fd);
        if (rv == 0) {       /* install mapping on success */
            dtrs->shmmap[idx].mapping = mapping;
            dtrs->shmmap[idx].size = src->dt_mmsize;
            mlog(SHM_DBG, "seg_establish: %s mmap @ %p", src->dt_src,
                 mapping);
        } else {
            mlog(SHM_ERR, "seg_establish: mmap DEV: %s: %s", src->dt_src,
                 strerror(rv));
        }
        return(rv);
    }

    /*
     * regular files and shared memory objects get created by rank 0
     * first, then the other ranks attach to them.  this is mainly
     * for debugging on a single node without shared memory hardware.
     * CXL-based shared memory hw uses a mmap'd dax device file (above).
     *
     * XXX: should we provide a reuse option (r0 always resets)?
     * XXX: we currently do not unlink these files.
     */
    if (type == DTHREAD_SRC_FILE || type == DTHREAD_SRC_PSHM) {
        flags = (rank == 0) ? O_RDWR|O_CREAT : O_RDWR;
        if (type == DTHREAD_SRC_FILE)
            fd = open(src->dt_src, flags, 0666);
        else
            fd = shm_open(src->dt_src, flags, 0666);
        if (fd < 0) {
            rv = errno;
            mlog(SHM_ERR, "seg_establish: file: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        if (rank == 0) {                  /* grow file if needed */
            if (fstat(fd, &st) < 0) {
                rv = errno;
                close(fd);
                mlog(SHM_ERR, "seg_establish: fstat: %s: %s", src->dt_src,
                     strerror(rv));
                return(rv);
            }
            if (st.st_size < src->dt_mmoffset + src->dt_mmsize) {
                if (ftruncate(fd, src->dt_mmoffset + src->dt_mmsize) < 0) {
                    rv = errno;
                    close(fd);
                    mlog(SHM_ERR, "seg_establish: ftruc: %s: %s", src->dt_src,
                         strerror(rv));
                    return(rv);
                }

            }
        }
        mapping = mmap(NULL, src->dt_mmsize, PROT_READ|PROT_WRITE, MAP_SHARED,
                       fd, src->dt_mmoffset);
        rv = (mapping == MAP_FAILED) ? errno : 0;
        close(fd);
        if (rv == 0) {       /* install mapping on success */
            dtrs->shmmap[idx].mapping = mapping;
            dtrs->shmmap[idx].size = src->dt_mmsize;
            mlog(SHM_DBG, "seg_establish: %s mmap @ %p", src->dt_src,
                 mapping);
        } else {
            mlog(SHM_ERR, "seg_establish: mmap file: %s: %s", src->dt_src,
                 strerror(rv));
        }
        return(rv);
    }

    /*
     * unknown SRC type
     */
    mlog(SHM_ERR, "seg_establish: %s: unknown type %d", src->dt_src, type);
    return(EINVAL);
}

/*
 * snapshot of how much space is left in shmid segment at this time.
 * this value can change after we return (not locked by us).
 */
int dthread_shmsegavail(uint64_t shmid, dthread_shmref_t *got) {
    dthread_shmseg_md_t *md;

    md = (shmid >= dtrs->nshmsrc) ? NULL : dtrs->shmmap[shmid].mapping;
    if (!md || md->seg_md_magic != DTHREAD_SEG_MD_MAGIC) {
        return(EINVAL);
    }

    if (got) {
        got->dt_shmid = shmid;
        got->dt_offset = md->allocated;
        if (got->dt_offset > dtrs->shmmap[shmid].size) {
            got->dt_offset = dtrs->shmmap[shmid].size;
            got->dt_length = 0;
        } else {
            got->dt_length = dtrs->shmmap[shmid].size - got->dt_offset;
        }
    }

    return(0);
}

/*
 * low-level shm segment memory allocator.  allocated segment memory
 * is never released/freed back to the segment.   if want == 0, we
 * allocate the rest of the segment. returns local ptr to allocation
 * and a ref (if requested).
 */
void *dthread_shmseg_alloc(uint64_t shmid, uint64_t want,
                           dthread_shmref_t *ref) {
    dthread_shmseg_md_t *md;
    void *rv = NULL;

    md = (shmid >= dtrs->nshmsrc) ? NULL : dtrs->shmmap[shmid].mapping;
    if (!md || md->seg_md_magic != DTHREAD_SEG_MD_MAGIC) {
        mlog(SHM_ERR, "dthread_shmseg_alloc: bad seg %" PRIu64, shmid);
        return(NULL);
    }

    /* note: segalloc was restricted to rank 0 prior to adding the spinlock */
    if (dthread_spin_lock(&md->slock) != 0) {
        mlog(SHM_ERR, "dthread_shmseg_alloc: spin fail: seg %" PRIu64, shmid);
        return(NULL);
    }

    if (md->allocated >= dtrs->shmmap[shmid].size)
        goto done;

    if (want == 0) {
        want = dtrs->shmmap[shmid].size - md->allocated;
    } else if (want > dtrs->shmmap[shmid].size - md->allocated) {
        goto done;
    }

    rv = (char *)dtrs->shmmap[shmid].mapping + md->allocated;
    if (ref) {
        ref->dt_shmid = shmid;
        ref->dt_offset = md->allocated;
        ref->dt_length = want;
    }
    md->allocated += want;

done:
    dthread_spin_unlock(&md->slock);
    mlog(SHM_INFO, "dthread_shmseg_alloc: alloc %" PRIu64 " in seg %"
         PRIu64 " ret=%p", want, shmid, rv);
    return(rv);
}
