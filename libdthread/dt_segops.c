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
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <dthread/dthread_seginfo.h>

#include "dt_internal.h"

/*
 * we expect that all shared memory mappings are done at a page
 * level.  so the offset of data within a page will be the same
 * across the ranks even if each rank uses a different virtual
 * address (e.g. due to ASLR).  this also means that data alignment
 * will match across ranks.
 */

/*
 * macros
 */
#define close_invalidate(FDP) do {                                            \
    close(*(FDP));                                                            \
    *(FDP) = -1;                                                              \
} while (0)

/*
 * static helper functions
 */

/*
 * helper function to establish setup a mapping for a shmsrc.
 * we will create files and init metadata (if 'initialize' is set).
 * if we use mmap, we return the open file descriptor in *fd (in
 * case we need to move the mapping for UMAP mode).  *fd is set to -1
 * if no file descriptor was opened.   the mapping is saved in
 * dtrs->shmmap[idx].  returns 0 on success, error otherwise.
 */
static int establish(dthread_shmsrc_t *src, int idx, void *hint, int *fd,
                     int initialize) {
    uint64_t totalreserve, newmdoffset;
    int type, rv, flags, pad;
    char *newmapping, *mdptr;
    struct stat st;
    off_t expectedsize;
    dthread_shmseg_md_t *md;

    *fd = -1;     /* initial value: no open fd (yet) */

    /*
     * ensure that the total reserve is not larger than our mapping.
     * then make sure we have at least 2 pages the shmsrc beyond that.
     */
    totalreserve = src->dt_reserve_front + src->dt_reserve_back;
    if (totalreserve  >= src->dt_mmsize) {
        mlog(SHM_ERR, "seg_establish: %s: reserve too large", src->dt_src);
        return(EINVAL);
    }
    if ((src->dt_mmsize - totalreserve) < 2 * dtrs->pagesize) {
        mlog(SHM_ERR, "seg_establish: %s: segment too small", src->dt_src);
        return(EINVAL);
    }

    /*
     * determine type and switch on it.  each switch case will either
     * directly return an error code or set 'newmapping' to point to
     * the new shared memory mapping.  if mmap was used, the open file
     * descriptor will be saved in *fd.
     */
    type = src->dt_srcflags & DTHREAD_SRC_MASK;
    switch (type) {
    case DTHREAD_SRC_DEV:                 /* device file (e.g. cxl dax dev) */
        *fd = open(src->dt_src, O_RDWR);
        if (*fd < 0) {
            rv = errno;
            mlog(SHM_ERR, "seg_establish: DEV: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        newmapping = mmap(hint, src->dt_mmsize, PROT_READ|PROT_WRITE,
                          MAP_SHARED, *fd, src->dt_mmoffset);
        rv = (newmapping == MAP_FAILED) ? errno : 0;
        if (rv) {
            close_invalidate(fd);
            mlog(SHM_ERR, "seg_establish: mmap DEV: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        mlog(SHM_DBG, "seg_establish: %s mmap-dev @ %p", src->dt_src,
                 newmapping);
        break;

    /* these are mainly for debugging (XXX: we do not unlink the entries) */
    case DTHREAD_SRC_FILE:                /* plain file */
    case DTHREAD_SRC_PSHM:                /* posix shm */
        flags = O_RDWR | ( (initialize) ? O_CREAT : 0);
        if (type == DTHREAD_SRC_FILE)
            *fd = open(src->dt_src, flags, 0666);
        else
            *fd = shm_open(src->dt_src, flags, 0666);
        if (*fd < 0) {
            rv = errno;
            mlog(SHM_ERR, "seg_establish: file%d: %s: %s", type,
                 src->dt_src, strerror(rv));
            return(rv);
        }

        /* get file size and grow/shrink if necessary */
        if (fstat(*fd, &st) < 0) {
            rv = errno;
            close_invalidate(fd);
            mlog(SHM_ERR, "seg_establish: fstat: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        expectedsize = src->dt_mmoffset + src->dt_mmsize;
        if (initialize && st.st_size != expectedsize) {
            if (ftruncate(*fd, expectedsize) < 0) {
                rv = errno;
                close_invalidate(fd);
                mlog(SHM_ERR, "seg_establish: ftruc: %s: %s", src->dt_src,
                     strerror(rv));
                return(rv);
            }
        } else if (st.st_size != expectedsize) {  /* already init'd */
            rv = EIO;
            close_invalidate(fd);
            mlog(SHM_ERR, "seg_establish: size-mismatch: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        newmapping = mmap(hint, src->dt_mmsize, PROT_READ|PROT_WRITE,
                          MAP_SHARED, *fd, src->dt_mmoffset);
        rv = (newmapping == MAP_FAILED) ? errno : 0;
        if (rv) {
            close_invalidate(fd);
            mlog(SHM_ERR, "seg_establish: mmap file: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        mlog(SHM_DBG, "seg_establish: %s mmap-file%d @ %p", src->dt_src,
             type, newmapping);
        break;

    case DTHREAD_SRC_ADDR:                /* already mapped memory */
        if (src->dt_srcflags & DTHREAD_SRC_UMAP) {
            mlog(SHM_ERR, "seg_establish: SRC_ADDR w/SRC_UMAP is invalid");
            return(EINVAL);
        }
        newmapping = src->dt_addr;
        mlog(SHM_DBG, "seg_establish: %s mmap-addr @ %p", src->dt_src,
                 newmapping);
        break;

    default:
        mlog(SHM_ERR, "seg_establish: %s: bad type %d", src->dt_src, type);
        return(EINVAL);
    }

    /*
     * we have a valid newmapping.   determine desired metadata offset
     * and get pointer to metadata.
     */
    mdptr = newmapping + src->dt_reserve_front;   /* add front reserve */
    pad = ((uintptr_t) mdptr) % dtrs->pagesize;   /* page align md addr */
    if (pad) {
        pad = dtrs->pagesize - pad;
        mdptr += pad;
    }
    newmdoffset = mdptr - newmapping;
    md = (dthread_shmseg_md_t *) mdptr;

    /*
     * if the initialize flag is set we init the new metadata.
     * otherwise we sanity check our metadata pointer.
     */
    if (initialize) {     /* set up new metadata? */
        md->seg_md_magic = DTHREAD_SEG_MD_MAGIC;
        md->self.dt_shmid = idx;
        md->self.dt_offset = 0;
        md->self.dt_length = src->dt_mmsize;
        md->srcflags = src->dt_srcflags;
        md->umapaddr = 0;            /* set later if in UMAP mode */
        md->first_avail = newmdoffset + dtrs->pagesize;
        md->end_avail = src->dt_mmsize - src->dt_reserve_back;
        if (md->first_avail >= md->end_avail) {
            mlog(SHM_ERR, "seg_establish: %s: not enough space!", src->dt_src);
            rv = EINVAL;
            goto unmap_and_fail;
        }
        /*
         * XXX: assume we can init md spinlock before shm malloc avail.
         * if this does not work, we could use the remaining space
         * in segment md page to help setup the spinlock?
         */
        rv = dthread_spin_init(&md->slock, DTHREAD_PROCESS_SHARED);
        if (rv) {
            mlog(SHM_ERR, "seg_establish: %s: spin init fail!", src->dt_src);
            goto unmap_and_fail;
        }
    } else {
        if (md->seg_md_magic != DTHREAD_SEG_MD_MAGIC) {
            mlog(SHM_ERR, "seg_establish: %s: failed magic check!",
                 src->dt_src);
            rv = EINVAL;
            goto unmap_and_fail;
        }
        if (md->self.dt_shmid != idx || md->self.dt_offset != 0 ||
            md->self.dt_length != src->dt_mmsize) {
            mlog(SHM_ERR, "seg_establish: %s: failed self ref check!",
                 src->dt_src);
            rv = EINVAL;
            goto unmap_and_fail;
        }

    }

    /* success!  install new mapping info in dtrs->shmmap[] */
    dtrs->shmmap[idx].mapping = newmapping;
    dtrs->shmmap[idx].size = src->dt_mmsize;
    dtrs->shmmap[idx].md_offset = newmdoffset;

    return(0);

unmap_and_fail:
    if (*fd >= 0) {
        munmap(newmapping, src->dt_mmsize);
        close_invalidate(fd);
    }
    return(rv);
}

/*
 * establish shared memory segment mappings as specified in shmsrc.
 * makes collective MPI calls to sync up mappings across ranks.
 * we map one segment at a time on all ranks to help us keep the
 * virtual address mappings in sync when the UMAP option is set.
 * caller should have already checked that n is > 0.
 * return 0 on success, error otherwise.
 */
int dthread_shmseg_establish(dthread_shmsrc_t *shmsrctab, int n) {
    int lcv, fd, umap, rv, retries, i, type;
    uintptr_t r0info[2], inmin[1], inmax[2], outmin[1], outmax[2];
    void *hint, *mapping, *min, *max;
    char *mdp;
    dthread_shmseg_md_t *md;

    for (lcv = 0 ; lcv < n ; lcv++) {

        /* init vars and have rank0 establish a mapping w/initialize==1 */
        fd = -1;
        umap = (shmsrctab[lcv].dt_srcflags & DTHREAD_SRC_UMAP) != 0;
        rv = 0;
        r0info[0] = r0info[1] = 0;

        if (dtrs->mpi_rank == 0) {
            r0info[0] = establish(&shmsrctab[lcv], lcv, NULL, &fd, 1);
            if (r0info[0]) {
                mlog(SHM_ERR, "dthread_shmseg_establish: %s: fail %s",
                     shmsrctab[lcv].dt_src, strerror(r0info[0]));
            } else {
                /* hint for UMAP mode */
                r0info[1] = (uintptr_t) dtrs->shmmap[lcv].mapping;
            }
        }

        /* bcast result from rank0 to all ranks, err out on failure */
        if (MPI_Bcast(&r0info[0], 2, dtrs->mpi_uintptr, 0, MPI_COMM_WORLD)) {
            fprintf(stderr, "dthread_shmseg_establish: MPI_Bcast fail?\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        /* error out if rank0 got an error */
        if (r0info[0] != 0) {
            rv = (int) r0info[0];
            goto failed;
        }

        if (umap) {
            hint = (void *)r0info[1];   /* initial hint value */
            retries = 9;                /* retries to get uniform mapping */
        } else {
            hint = NULL;                /* !umap, no hint required */
            retries = 0;
        }

retry:
        /* all ranks without mappings should try and get one */
        mapping = dtrs->shmmap[lcv].mapping;
        if (mapping == NULL) {
            if (fd == -1) {             /* no fd?  initial attempt */
                rv = establish(&shmsrctab[lcv], lcv, hint, &fd, 0);
                mapping = (rv) ? MAP_FAILED : dtrs->shmmap[lcv].mapping;
            } else {
                /* umap retry to move mapping to a uniform address */
                mapping = mmap(hint, shmsrctab[lcv].dt_mmsize,
                               PROT_READ|PROT_WRITE, MAP_SHARED, fd,
                               shmsrctab[lcv].dt_mmoffset);
                if (mapping == MAP_FAILED) {
                    rv = errno;
                } else {
                    rv = 0;
                    /* mapping changed, but not size or md_offset */
                    dtrs->shmmap[lcv].mapping = mapping;
                }
            }
            /*
             * if mapping != MAP_FAILED, then rv==0 and the mapping
             * is valid and has been installed in dtrs->shmmap[lcv].
             *
             * if mapping == MAP_FAILED, then there is an error code
             * in rv and dtrs->shmmap[lcv].mapping is NULL.
             */
        }

        /* use MPI to get min/max of mapping and max(rv) across all ranks */
        inmin[0] = inmax[0] = (uintptr_t) mapping;
        inmax[1] = rv;
        if (MPI_Allreduce(inmin, outmin, 1, dtrs->mpi_uintptr, MPI_MIN,
                          MPI_COMM_WORLD)) {
            fprintf(stderr, "dthread_shmseg_establish: Allreduce MIX fail");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        if (MPI_Allreduce(inmax, outmax, 2, dtrs->mpi_uintptr, MPI_MAX,
                          MPI_COMM_WORLD)) {
            fprintf(stderr, "dthread_shmseg_establish: Allreduce MAX fail");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        min = (void *) outmin[0];
        max = (void *) outmax[0];
        rv = (int) outmax[1];

        /* failed if we see an error */
        if (min == MAP_FAILED || max == MAP_FAILED || rv != 0) {
            if (dtrs->mpi_rank == 0)
                mlog(SHM_ERR, "dthread_shmseg_establish: %s: fail!"
                     "  mf=%d,rv=%d", shmsrctab[lcv].dt_src,
                     (min == MAP_FAILED || max == MAP_FAILED), rv);
            if (rv == 0)
                rv = EIO;     /* no err?  make one up */
            goto failed;
        }

        /* extra steps required for umap */
        if (umap) {

            if (min != max) {
                /* can we retry the mapping? */
                if (retries-- > 0) {
                    hint = max;             /* hint to max (assume grows up) */
                    if (mapping != hint) {  /* drop non-matching mappings */
                        /* ignore unmap errors */
                        munmap(mapping, shmsrctab[lcv].dt_mmsize);
                        dtrs->shmmap[lcv].mapping = NULL;
                    }
                    if (dtrs->mpi_rank == 0)
                        mlog(SHM_DBG, "dthread_shmseg_establish: %s "
                         "retrying umap mmap", shmsrctab[lcv].dt_src);
                    goto retry;
                }

                /* failed after retrying */
                rv = EAGAIN;
                if (dtrs->mpi_rank == 0)
                    mlog(SHM_ERR, "dthread_shmseg_establish: %s "
                         "UMAP sync fail!", shmsrctab[lcv].dt_src);
                goto failed;
            }

            /* sucessfully umap (min == max) */
            if (dtrs->mpi_rank == 0) {
                /* save umap addr in metadata (for debugging) */
                mdp = (char *)dtrs->shmmap[lcv].mapping +
                              dtrs->shmmap[lcv].md_offset;
                md = (dthread_shmseg_md_t *) mdp;
                md->umapaddr = (uintptr_t) dtrs->shmmap[lcv].mapping;
            }

        }          /* umap */

        close_invalidate(&fd);    /* success!  dispose of fd (if set) */

    }   /* end of lcv loop */

    return(0);

failed:    /* release any resources we allocated and return an error */
    close_invalidate(&fd);
    for (i = 0 ; i <= lcv ; i++) {
        type = shmsrctab[i].dt_srcflags & DTHREAD_SRC_MASK;
        if (type == DTHREAD_SRC_DEV || type == DTHREAD_SRC_FILE ||
            type == DTHREAD_SRC_PSHM) {
            if (dtrs->shmmap[i].mapping != NULL) {
                munmap(dtrs->shmmap[i].mapping, dtrs->shmmap[i].size);
            }
        }
        memset(&dtrs->shmmap[i], 0, sizeof(dtrs->shmmap[i]));
    }
    return(rv);

}

/*
 * get shm seginfo.   fills out sip and returns 0 on success.
 * otherwise returns error code.
 */
int dthread_shm_seginfo(uint64_t shmid, dthread_seginfo_t *sip) {
    dthread_shmseg_md_t *md;

    if (shmid >= dtrs->nshmsrc)
        return(ENOENT);

    md = (dthread_shmseg_md_t *)((char *)dtrs->shmmap[shmid].mapping +
                                         dtrs->shmmap[shmid].md_offset);
    if (md->seg_md_magic != DTHREAD_SEG_MD_MAGIC ||
        md->self.dt_shmid != shmid || md->self.dt_offset != 0 ||
        md->self.dt_length != dtrs->shmmap[shmid].size) {
        return(EINVAL);
    }

    sip->si_mapping = dtrs->shmmap[shmid].mapping;
    sip->si_size = dtrs->shmmap[shmid].size;
    sip->si_mdoffset = dtrs->shmmap[shmid].md_offset;
    sip->si_srcflags = md->srcflags;
    sip->si_umapaddr = md->umapaddr;
    sip->si_firstavail = md->first_avail;
    sip->si_endavail = md->end_avail;

    return(0);
}

/*
 * snapshot of how much usable space is left in shmid segment at this time.
 * this value can change after we return (not locked by us).
 */
int dthread_shm_segavail(uint64_t shmid, dthread_shmref_t *got) {
    dthread_shmseg_md_t *md;

    if (shmid >= dtrs->nshmsrc) {
        md = NULL;
    } else {
        md = (dthread_shmseg_md_t *)((char *)dtrs->shmmap[shmid].mapping +
                                             dtrs->shmmap[shmid].md_offset);
    }

    if (!md || md->seg_md_magic != DTHREAD_SEG_MD_MAGIC) {
        return(EINVAL);
    }

    if (got) {
        got->dt_shmid = shmid;
        got->dt_offset = md->first_avail;
        if (got->dt_offset > dtrs->shmmap[shmid].size ||
            got->dt_offset > md->end_avail) {
            got->dt_offset = md->end_avail;
            got->dt_length = 0;
        } else {
            got->dt_length = md->end_avail - got->dt_offset;
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
    uint64_t pad, total;
    void *rv = NULL;

    /* XXX: more sanity check? */
    if (shmid >= dtrs->nshmsrc) {
        md = NULL;
    } else {
        md = (dthread_shmseg_md_t *)((char *)dtrs->shmmap[shmid].mapping +
                                             dtrs->shmmap[shmid].md_offset);
    }

    if (!md || md->seg_md_magic != DTHREAD_SEG_MD_MAGIC) {
        mlog(SHM_ERR, "dthread_shmseg_alloc: bad seg %" PRIu64, shmid);
        return(NULL);
    }

    /* compute padding needed to align 'want' bytes */
    pad = want % alignof(max_align_t);
    if (pad)
        pad = alignof(max_align_t) - pad;    /* convert to padding */
    total = want + pad;

    /* note: segalloc was restricted to rank 0 prior to adding the spinlock */
    if (dthread_spin_lock(&md->slock) != 0) {
        mlog(SHM_ERR, "dthread_shmseg_alloc: spin fail: seg %" PRIu64, shmid);
        return(NULL);
    }

    if (md->first_avail >= dtrs->shmmap[shmid].size ||
        md->first_avail >= md->end_avail)
        goto done;

    if (total == 0) {    /* use remaining space */
        total = want = md->end_avail - md->first_avail;
    } else if (total > md->end_avail - md->first_avail) {
        goto done;
    }

    rv = (char *)dtrs->shmmap[shmid].mapping + md->first_avail;
    if (ref) {
        ref->dt_shmid = shmid;
        ref->dt_offset = md->first_avail;
        ref->dt_length = total;
    }
    md->first_avail += total;

done:
    dthread_spin_unlock(&md->slock);
    mlog(SHM_INFO, "dthread_shmseg_alloc: alloc %" PRIu64 " in seg %"
         PRIu64 " pad=%" PRIu64 " ret=%p", want, shmid, pad, rv);
    return(rv);
}
