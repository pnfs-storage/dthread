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
 * dt_shmops.c  dthread shared memory establish
 * 07-May-2026  chuck@ece.cmu.edu
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
 * establish shared memory mappings.   return 0 on success, error
 * otherwise.
 */
int dthread_shm_establish(dthread_shmsrc_t *shmsrctab, int n) {
    int lcv, rv;
    dthread_shm_md_t *md;
    for (lcv = 0 ; lcv < n ; lcv++) {
        rv = establish(&shmsrctab[lcv], lcv);
        if (rv)
            return(rv);
        /* rank 0 must setup the metadata structure */
        if (dtrs->mpi_rank == 0) {
            md = (dthread_shm_md_t *) dtrs->shmmap[lcv].mapping;
            md->shm_md_magic = DTHREAD_SHM_MD_MAGIC;
            md->self.dt_shmid = lcv;
            md->self.dt_offset = 0;
            md->self.dt_length = dtrs->shmmap[lcv].size;
            md->alloctype = DTHREAD_SHM_SEG;
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
 * establish one shared memory mapping
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
        mlog(SHM_ERR, "shm_establish: %s: segment too small", src->dt_src);
        return(EINVAL);
    }

    /*
     * device files are easy, we just mmap them.
     */
    if (type == DTHREAD_SRC_DEV) {
        fd = open(src->dt_src, O_RDWR);
        if (fd < 0) {
            rv = errno;
            mlog(SHM_ERR, "shm_establish: DEV: %s: %s", src->dt_src,
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
            mlog(SHM_DBG, "shm_establish: %s mmap @ %p", src->dt_src,
                 mapping);
        } else {
            mlog(SHM_ERR, "shm_establish: mmap DEV: %s: %s", src->dt_src,
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
            mlog(SHM_ERR, "shm_establish: file: %s: %s", src->dt_src,
                 strerror(rv));
            return(rv);
        }
        if (rank == 0) {                  /* grow file if needed */
            if (fstat(fd, &st) < 0) {
                rv = errno;
                close(fd);
                mlog(SHM_ERR, "shm_establish: fstat: %s: %s", src->dt_src,
                     strerror(rv));
                return(rv);
            }
            if (st.st_size < src->dt_mmoffset + src->dt_mmsize) {
                if (ftruncate(fd, src->dt_mmoffset + src->dt_mmsize) < 0) {
                    rv = errno;
                    close(fd);
                    mlog(SHM_ERR, "shm_establish: ftruc: %s: %s", src->dt_src,
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
            mlog(SHM_DBG, "shm_establish: %s mmap @ %p", src->dt_src,
                 mapping);
        } else {
            mlog(SHM_ERR, "shm_establish: mmap file: %s: %s", src->dt_src,
                 strerror(rv));
        }
        return(rv);
    }

    /*
     * unknown SRC type
     */
    mlog(SHM_ERR, "shm_establish: %s: unknown type %d", src->dt_src, type);
    return(EINVAL);
}

/*
 * snapshot of how much space is left in shmid segment at this time.
 * this value can change after we return (not locked by us).
 */
int dthread_shm_segavail(uint64_t shmid, dthread_shmref_t *got) {
    dthread_shm_md_t *md;

    md = (shmid >= dtrs->nshmsrc) ? NULL : dtrs->shmmap[shmid].mapping;
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        md->alloctype != DTHREAD_SHM_SEG) {
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
void *dthread_shm_segalloc(uint64_t shmid, uint64_t want,
                           dthread_shmref_t *ref) {
    dthread_shm_md_t *md;
    void *rv = NULL;

    md = (shmid >= dtrs->nshmsrc) ? NULL : dtrs->shmmap[shmid].mapping;
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        md->alloctype != DTHREAD_SHM_SEG) {
        mlog(SHM_ERR, "dthread_shm_segalloc: bad seg %" PRIu64, shmid);
        return(NULL);
    }

    /* note: segalloc was restricted to rank 0 prior to adding the spinlock */
    if (dthread_spin_lock(&md->slock) != 0) {
        mlog(SHM_ERR, "dthread_shm_segalloc: spin fail: seg %" PRIu64, shmid);
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
    mlog(SHM_INFO, "dthread_shm_segalloc: alloc %" PRIu64 " in seg %"
         PRIu64 " ret=%p", want, shmid, rv);
    return(rv);
}

/*
 * shmref to local rank pointer
 */
void *dthread_shm_ref2ptr(dthread_shmref_t *refp, uint64_t len) {
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
int dthread_shm_ptr2ref(void *ptr, uint64_t len, dthread_shmref_t *refp) {
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

/*
 * shmalloc ops and API
 */

/*
 * a simple "break" allocator (for cases where we are unlikely to
 * need to free memory).   we have a break line between allocated
 * and unallocated line that we advance on allocation (e.g. like
 * brk()/sbrk()).   we will reset and release memory only when
 * the number of active allocations drops to zero.
 */
struct break_malloc_md {
    dthread_shm_md_t dt_md;     /* generic metadata */
    /* additional "break" specific data */
    int nalloc;                 /* simple allocation counter */
};

/*
 * header contains 4 uint32: BREAK_BEFORE LENhi LENlo BREAK_AFTER
 * where LEN is the allocation size stored in a uint64_t.
 */
#define BREAK_BEFORE 0xbef01e12     /* magic number before header */
#define BREAK_AFTER  0xaf1e1345     /* magic number after header */

static int break_init(dthread_shmref_t *arena) {
    struct break_malloc_md *bmd;

    bmd = dthread_shm_ref2ptr(arena, sizeof(struct break_malloc_md));
    if (bmd == NULL) {
        mlog(SHM_ERR, "break_init: failed, arena=%p", arena);
        return(ENOMEM);
    }

    /* updated allocated to include extra metadata in break_malloc_md */
    dthread_spin_lock(&bmd->dt_md.slock);
    bmd->dt_md.allocated = sizeof(struct break_malloc_md);
    bmd->nalloc = 0;
    dthread_spin_unlock(&bmd->dt_md.slock);

    mlog(SHM_INFO, "break_init: OK %p <%" PRIu64 ",%" PRIu64 ",%" PRIu64
         ">", arena, arena->dt_shmid, arena->dt_offset, arena->dt_length);
    return(0);
}

static int break_finalize(dthread_shmref_t *arena) {
    struct break_malloc_md *bmd;
    int nallocs;
    
    bmd = dthread_shm_ref2ptr(arena, sizeof(struct break_malloc_md));
    if (bmd == NULL) {
        mlog(SHM_ERR, "break_finalize: failed, arena=%p", arena);
        return(ENOMEM);
    }

    dthread_spin_lock(&bmd->dt_md.slock);
    nallocs = bmd->nalloc;
    dthread_spin_unlock(&bmd->dt_md.slock);

    if (nallocs)
        mlog(SHM_INFO, "break_finalize: nallocs=%d (>0)", nallocs);

    return(0);
}

static void *break_malloc(dthread_shmref_t *arena, size_t size,
                          dthread_shmref_t *ref) {
    struct break_malloc_md *bmd;
    size_t pad, total;
    uint32_t *hdr;
    void *rv = NULL;

    bmd = dthread_shm_ref2ptr(arena, sizeof(struct break_malloc_md));
    if (bmd == NULL) {
        mlog(SHM_ERR, "break_malloc: noMD %p <%" PRIu64 ",%" PRIu64 
             ",%" PRIu64 ">", arena, arena->dt_shmid, arena->dt_offset,
             arena->dt_length);
        return(NULL);
    }

    pad = size % sizeof(uint64_t);
    if (pad)
        pad = sizeof(uint64_t) - pad;        /* pad len to multiple of 8 */
    total = size + pad + sizeof(uint32_t)*4; /* add room for header */

    dthread_spin_lock(&bmd->dt_md.slock);
    if (total <= bmd->dt_md.self.dt_length - bmd->dt_md.allocated) {
        hdr = (uint32_t *) ( (char *) dtrs->shmmap[arena->dt_shmid].mapping +
                                      arena->dt_offset + bmd->dt_md.allocated );
        hdr[0] = BREAK_BEFORE;
        hdr[1] = ((uint64_t) size) >> 32;
        hdr[2] = ((uint64_t) size) & 0xffffffff;
        hdr[3] = BREAK_AFTER;
        rv = hdr + 4;    /* skip over header */
        if (ref) {
            ref->dt_shmid = arena->dt_shmid;
            ref->dt_offset = arena->dt_offset + bmd->dt_md.allocated + 
                             sizeof(uint32_t)*4;
            ref->dt_length = size;
        }
        bmd->dt_md.allocated += total;
        bmd->nalloc++;
    }
    dthread_spin_unlock(&bmd->dt_md.slock);

    mlog(SHM_DBG, "break_malloc: ret=%p a=%p <%" PRIu64 ",%" PRIu64 
             ",%" PRIu64 ">", rv, arena, arena->dt_shmid, arena->dt_offset,
             arena->dt_length);
    return(rv);
}

static void break_free(dthread_shmref_t *arena, dthread_shmref_t *ref) {
    struct break_malloc_md *bmd;
    void *ptr;
    uint32_t *hdr;
    int n;

    bmd = dthread_shm_ref2ptr(arena, sizeof(struct break_malloc_md));
    if (bmd == NULL) {
        mlog(SHM_ERR, "break_free: noMD %p <%" PRIu64 ",%" PRIu64 
             ",%" PRIu64 ">", arena, arena->dt_shmid, arena->dt_offset,
             arena->dt_length);
        return;
    }

    /* make sure ref falls within arena bounds */
    if (ref->dt_shmid != arena->dt_shmid ||
        ref->dt_offset < arena->dt_offset + sizeof(struct break_malloc_md) ||
        ref->dt_length > (arena->dt_length -
                          (ref->dt_offset - arena->dt_offset) ) ) {
        mlog(SHM_ERR, "break_free: bad ref - out of arena bounds");
        return;
    }

    ptr = dthread_shm_ref2ptr(ref, 0);
    if (!ptr || ((uintptr_t) ptr) % sizeof(uint64_t) != 0) {
        mlog(SHM_ERR, "break_free: bad input ref ptr %p", ptr);
        return;
    }
        
    hdr = ((uint32_t *) ptr) - 4;
    if (hdr[0] != BREAK_BEFORE || hdr[3] != BREAK_AFTER) {
        mlog(SHM_ERR, "break_free: bad header on input ptr %p", ptr);
        return;
    }

    dthread_spin_lock(&bmd->dt_md.slock);
    hdr[0] = 0xdeaddead;      /* invalidate header */
    if (bmd->nalloc > 0)
        bmd->nalloc--;
    if (bmd->nalloc == 0)     /* all allocations freed, reset */
        bmd->dt_md.allocated = sizeof(struct break_malloc_md);
    n = bmd->nalloc;
    dthread_spin_unlock(&bmd->dt_md.slock);
    mlog(SHM_DBG, "break_free: %p (nalloc=%d)", ptr, n);

    if (n == 0)
        mlog(SHM_INFO, "break_free: all free.  reset allocator!");
}

static void *break_realloc(dthread_shmref_t *arena, dthread_shmref_t ref_in,
                           size_t new_size, dthread_shmref_t *ref) {
    struct break_malloc_md *bmd;
    void *ptr;
    uint32_t *hdr;
    uint64_t old_size;
    void *newptr;

    bmd = dthread_shm_ref2ptr(arena, sizeof(struct break_malloc_md));
    if (bmd == NULL) {
        mlog(SHM_ERR, "break_realloc: noMD %p <%" PRIu64 ",%" PRIu64 
             ",%" PRIu64 ">", arena, arena->dt_shmid, arena->dt_offset,
             arena->dt_length);
        return(NULL);
    }

    /* make sure ref falls within arena bounds */
    if (ref_in.dt_shmid != arena->dt_shmid ||
        ref_in.dt_offset < arena->dt_offset + sizeof(struct break_malloc_md) ||
        ref_in.dt_length > (arena->dt_length -
                           (ref_in.dt_offset - arena->dt_offset) ) ) {
        mlog(SHM_ERR, "break_realloc: bad ref - out of arena bounds");
        return(NULL);
    }

    ptr = dthread_shm_ref2ptr(&ref_in, 0);
    if (!ptr || ((uintptr_t) ptr) % sizeof(uint64_t) != 0) {
        mlog(SHM_ERR, "break_realloc: bad input ref ptr %p", ptr);
        return(NULL);
    }
        
    hdr = ((uint32_t *) ptr) - 4;
    if (hdr[0] != BREAK_BEFORE || hdr[3] != BREAK_AFTER) {
        mlog(SHM_ERR, "break_realloc: bad header on input ptr %p", ptr);
        return(NULL);
    }
    old_size = ((uint64_t)hdr[1] << 32) | hdr[2];

    /* new size less or equal to current?  keep current buffer */
    if (new_size <= old_size) {
        if (ref) {
            ref->dt_shmid = arena->dt_shmid;
            ref->dt_offset =  ((char *) ptr) - 
                ((char *)dtrs->shmmap[arena->dt_shmid].mapping);
            ref->dt_length = new_size;
        }
        mlog(SHM_DBG, "break_realloc: %p: no growth req", ptr);
        return(ptr);
    }

    /* grow allocation: alloc new buf, copy, free old buf */
    mlog(SHM_DBG, "break_realloc: %p: growing allocation", ptr);
    newptr = break_malloc(arena, new_size, ref);
    if (newptr == NULL)
        return(NULL);
    memcpy(newptr, ptr, old_size);
    break_free(arena, &ref_in);
    return(newptr);
}

/*
 * end of break allocator
 */

/*
 * list of internal allocators
 */
static dthread_shmalloc_ops_t internal_mops[] = {
    { "break", break_init, break_finalize,
       break_malloc, break_free, break_realloc },
};

#define NINT_MOPS (sizeof(internal_mops)/sizeof(internal_mops[0]))

/*
 * lookup malloc ops by name and return alloctype.  return -1 on err.
 * alloctype 0 is reserved (maps to segment allocator, but external
 * users do not have access to that).  alloctype 1 is first internal
 * allocator.   external allocators follow the internal ones.
 */
static int dthread_get_alloctype(char *name) {
    int lcv;

    /* look at user-provided mallocs first, if any */
    for (lcv = 0 ; lcv < dtrs->nusrmops ; lcv++) {
        if (strcmp(dtrs->mop[lcv].name, name) == 0)
            return(1 + NINT_MOPS + lcv);
    }

    /* check for internal allocator */
    for (lcv = 0 ; lcv < NINT_MOPS ; lcv++) {
        if (strcmp(internal_mops[lcv].name, name) == 0)
            return(1 + lcv);
    }

    /* unknown name */
    return(-1);
}

/*
 * convert alloctype to shmalloc_ops
 */
static dthread_shmalloc_ops_t *alloctype_mops(int alloctype) {
    if (alloctype < 1)
        return(NULL);
    if (alloctype <= NINT_MOPS)
        return(&internal_mops[alloctype-1]);
    if (alloctype <= NINT_MOPS + dtrs->nusrmops)
        return(&dtrs->mop[alloctype - NINT_MOPS - 1]);
    return(NULL);
}

/*
 * set default arena for shm_malloc.  can only be set once.
 * prob a good idea to have all ranks set the same default.
 * XXX: consider moving setting from dtrs to shared memory?
 */
int dthread_default_shmarena(dthread_shmref_t *dflt) {
    int rv;
    pthread_mutex_lock(&dtrs->arenalock);
    if (dtrs->defarena == NULL) {
        dtrs->da_store = *dflt;
        dtrs->defarena = &dtrs->da_store;
        rv = 0;
    } else {
        rv = EEXIST;
    }
    pthread_mutex_unlock(&dtrs->arenalock);
    mlog(SHM_DBG, "default-arena: %p -> %p <%" PRIu64 ",%" PRIu64 ",%" PRIu64
         ">", dflt, dtrs->defarena, dflt->dt_shmid, dflt->dt_offset,
         dflt->dt_length);
    return(rv);
}

/*
 * allocate a new shared memory arena for shmalloc ops.  return 0
 * on success.
 */
int dthread_alloc_shmarena(uint64_t shmid, char *shmalloc_name,
                           size_t size, dthread_shmref_t *newarena) {
    int alloctype;
    dthread_shmalloc_ops_t *mops;
    dthread_shm_md_t *md;
    int rv;
   
    /* size should be at least one page */
    if (size && size < dtrs->pagesize) {
        mlog(SHM_ERR, "dthread_alloc_shmarena: size too small");
        return(EINVAL);
    }

    /* use name to select correct operator */
    alloctype = dthread_get_alloctype(shmalloc_name);
    if (alloctype < 1) {
        mlog(SHM_ERR, "dthread_alloc_shmarena: bad type %s", shmalloc_name);
        return(ENOENT);
    }
    mops = alloctype_mops(alloctype);
    if (!mops) {
        mlog(SHM_ERR, "dthread_alloc_shmarena: no mops (%s)", shmalloc_name);
        return(ENOENT);
    }

    /* use low-level segment allocator to get block of shm */
    md = dthread_shm_segalloc(shmid, size, newarena);
    if (!md) {
        mlog(SHM_ERR, "dthread_alloc_shmarena: %s: segalloc failed",
             shmalloc_name);
        return(ENOMEM);
    }

    md->shm_md_magic = DTHREAD_SHM_MD_MAGIC;
    md->self = *newarena;
    md->alloctype = alloctype;
    md->allocated = sizeof(md);   /* driver should increase this as needed */
    dthread_spin_init(&md->slock, DTHREAD_PROCESS_SHARED);

    rv = mops->init(newarena);    /* should not fail */
    if (rv != 0) {
        /* XXX: no way to give back segment memory, so it's lost... */
        mlog(SHM_CRIT, "dthread_alloc_shmarena: %s init fail (%s), sz=%zd",
             shmalloc_name, strerror(rv), size);
    }

    return(rv);
}

/*
 * arena-based shared memory allocate
 */
void *dthread_shm_malloc(dthread_shmref_t *arena, uint64_t len,
                         dthread_shmref_t *ref) {
    dthread_shm_md_t *md;
    dthread_shmalloc_ops_t *mops;

    if (arena == NULL) {  /* sub in default arena if it is set */
        arena = dtrs->defarena;
        if (arena == NULL) {
            mlog(SHM_ERR, "shm_malloc: no default arena");
            return(NULL);
        }
    }

    md = dthread_shm_ref2ptr(arena, sizeof(*md));

    /* validate metadata */
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        memcmp(arena, &md->self, sizeof(md->self)) != 0) {
        mlog(SHM_ERR, "shm_malloc: invalid metadata");
        return(NULL);
    }

    /* get the ops and call them */
    mops = alloctype_mops(md->alloctype);
    if (mops == NULL) {
        mlog(SHM_ERR, "shm_malloc: no mops");
        return(NULL);
    }
    return( mops->malloc(arena, len, ref) );
}

/*
 * free memory
 */
void dthread_shm_free(dthread_shmref_t *arena, void *ptr) {
    dthread_shmref_t ref;
    dthread_shm_md_t *md;
    dthread_shmalloc_ops_t *mops;

    if (ptr == NULL)    /* freeing NULL is a noop */
        return;

    if (arena == NULL) {  /* sub in default arena if it is set */
        arena = dtrs->defarena;
        if (arena == NULL) {
            mlog(SHM_ERR, "shm_free: no default arena");
            return;
        }
    }

    if (dthread_shm_ptr2ref(ptr, 0, &ref) != 0) {
        mlog(SHM_ERR, "shm_free: bad ptr %p", ptr);
        return;
    }

    md = dthread_shm_ref2ptr(arena, sizeof(*md));

    /* validate metadata */
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        memcmp(arena, &md->self, sizeof(md->self)) != 0) {
        mlog(SHM_ERR, "shm_free: invalid metadata");
        return;
    }

    /* get the ops and call them */
    mops = alloctype_mops(md->alloctype);
    if (mops == NULL) {
        mlog(SHM_ERR, "shm_free: no mops");
        return;
    }

    mops->free(arena, &ref);
}

/*
 * free memory ref
 */
void dthread_shm_free_ref(dthread_shmref_t *arena, dthread_shmref_t *ref) {
    dthread_shm_md_t *md;
    dthread_shmalloc_ops_t *mops;

    if (arena == NULL) {  /* sub in default arena if it is set */
        arena = dtrs->defarena;
        if (arena == NULL) {
            mlog(SHM_ERR, "shm_free_ref: no default arena");
            return;
        }
    }

    md = dthread_shm_ref2ptr(arena, sizeof(*md));

    /* validate metadata */
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        memcmp(arena, &md->self, sizeof(md->self)) != 0) {
        mlog(SHM_ERR, "shm_free_ref: invalid metadata");
        return;
    }

    /* get the ops and call them */
    mops = alloctype_mops(md->alloctype);
    if (mops == NULL) {
        mlog(SHM_ERR, "shm_free_ref: no mops");
        return;
    }

    mops->free(arena, ref);
}

/*
 * realloc memory
 */
void *dthread_shm_realloc(dthread_shmref_t *arena, void *ptr, uint64_t len,
                          dthread_shmref_t *newref) {
    dthread_shmref_t ref;
    dthread_shm_md_t *md;
    dthread_shmalloc_ops_t *mops;

    if (ptr == NULL)    /* realloc w/null is a malloc */
        return(dthread_shm_malloc(arena, len, newref));

    if (arena == NULL) {  /* sub in default arena if it is set */
        arena = dtrs->defarena;
        if (arena == NULL) {
            mlog(SHM_ERR, "shm_realloc: no default arena");
            return(NULL);
        }
    }

    if (dthread_shm_ptr2ref(ptr, 0, &ref) != 0) {
        mlog(SHM_ERR, "shm_realloc: bad ptr %p", ptr);
        return(NULL);
    }

    md = dthread_shm_ref2ptr(arena, sizeof(*md));

    /* validate metadata */
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        memcmp(arena, &md->self, sizeof(md->self)) != 0) {
        mlog(SHM_ERR, "shm_realloc: invalid metadata");
        return(NULL);
    }

    /* get the ops and call them */
    mops = alloctype_mops(md->alloctype);
    if (mops == NULL) {
        mlog(SHM_ERR, "shm_realloc: no mops");
        return(NULL);
    }

    return( mops->realloc(arena, ref, len, newref) );
}

/*
 * realloc memory ref
 */
void *dthread_shm_realloc_ref(dthread_shmref_t *arena, dthread_shmref_t *ref,
                              uint64_t len, dthread_shmref_t *newref) {
    dthread_shm_md_t *md;
    dthread_shmalloc_ops_t *mops;

    if (arena == NULL) {  /* sub in default arena if it is set */
        arena = dtrs->defarena;
        if (arena == NULL) {
            mlog(SHM_ERR, "shm_realloc_ref: no default arena");
            return(NULL);
        }
    }

    md = dthread_shm_ref2ptr(arena, sizeof(*md));

    /* validate metadata */
    if (!md || md->shm_md_magic != DTHREAD_SHM_MD_MAGIC ||
        memcmp(arena, &md->self, sizeof(md->self)) != 0) {
        mlog(SHM_ERR, "shm_realloc_ref: invalid metadata");
        return(NULL);
    }

    /* get the ops and call them */
    mops = alloctype_mops(md->alloctype);
    if (mops == NULL) {
        mlog(SHM_ERR, "shm_realloc_ref: no mops");
        return(NULL);
    }

    return( mops->realloc(arena, *ref, len, newref) );
}
