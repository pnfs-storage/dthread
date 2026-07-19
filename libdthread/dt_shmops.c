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
 * dt_shmops.c  dthread shared memory malloc ops
 * 07-May-2026  chuck@ece.cmu.edu
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

#include "dt_internal.h"

/* use alignof to determine desired malloc alignment */
static int alignsz = alignof(max_align_t);

/*
 * mdalign_size: compute how much arena space we should reserve
 * for the metadata plus any needed padding so that it ends on
 * an alignsz boundary.   we assume that mappings have the same
 * alignment across ranks.
 */
static size_t mdalign_size(dthread_shmref_t *arena, size_t md_basesize) {
    char *base;
    size_t pad;

    /* cannot return null, since we already setup dthread_shm_alloc_md_t */
    base = dthread_shmref2ptr(arena, 0);
    base += md_basesize;     /* advance to end of metadata */
    pad = ((uintptr_t) base) % alignsz;
    if (pad)
        pad = alignsz - pad; /* convert to padding */

    return(md_basesize + pad);
}


/*
 * internal malloc drivers.
 */

/*
 * START: "break" malloc driver
 *
 * a simple break-based memory allocator (for cases where we are
 * unlikely to need to free memory).  we have a break line between
 * allocated and unallocated memory that we advance on allocation
 * (e.g. like sbrk()).   we will reset and release memory only
 * when the number of active allocations drops to zero.
 */

struct break_malloc_md {
    dthread_shm_alloc_md_t amd;  /* generic alloc metadata (must be first!) */
    uint64_t allocated;          /* #bytes allocated (break location) */
    int hdrlen;                  /* size of header */
    int nalloc;                  /* number of active allocations */
};

/*
 * header contains 4 uint32: BREAK_BEFORE LENhi LENlo BREAK_AFTER
 * where LEN is the allocation size stored in a uint64_t.
 */
#define BREAK_BEFORE 0xbef01e12     /* magic number before header */
#define BREAK_AFTER  0xaf1e1345     /* magic number after header */

static int break_init(dthread_shmref_t *arena) {
    uint64_t mdsize;
    struct break_malloc_md *bmd;
    int pad;

    mdsize = mdalign_size(arena, sizeof(*bmd));
    bmd = dthread_shmref2ptr(arena, mdsize);
    if (bmd == NULL) {
        mlog(SHM_ERR, "break_init: failed, arena=%p", arena);
        return(ENOMEM);
    }

    /* setup our metadata */
    dthread_spin_lock(&bmd->amd.alock);

    /* adjust amd: account for our alignment and extra metadata */
    bmd->amd.min_uoffset = bmd->amd.self.dt_offset + mdsize;
    bmd->amd.badalign_bits = alignsz - 1;

    bmd->allocated = mdsize;
    bmd->hdrlen = 4 * sizeof(uint32_t);
    pad = bmd->hdrlen % alignsz;
    if (pad)
        bmd->hdrlen = bmd->hdrlen + (alignsz - pad);
    bmd->nalloc = 0;
    dthread_spin_unlock(&bmd->amd.alock);

    mlog(SHM_INFO, "break_init: OK %p <%" PRIu64 ",%" PRIu64 ",%" PRIu64
         ">", arena, arena->dt_shmid, arena->dt_offset, arena->dt_length);
    return(0);
}

static int break_finalize(dthread_shmref_t *arena) {
    struct break_malloc_md *bmd;
    int nallocs;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct break_malloc_md));

    dthread_spin_lock(&bmd->amd.alock);
    nallocs = bmd->nalloc;
    dthread_spin_unlock(&bmd->amd.alock);

    if (nallocs)
        mlog(SHM_INFO, "break_finalize: nallocs=%d (>0)", nallocs);

    return(0);
}

static void *break_malloc(dthread_shmref_t *arena, size_t size,
                          dthread_shmref_t *newref) {
    struct break_malloc_md *bmd;
    size_t pad, total;
    uint32_t *hdr;
    void *rv = NULL;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct break_malloc_md));

    pad = size % alignsz;
    if (pad)
        pad = alignsz - pad;           /* pad len to multiple of alignment */

    total = size + pad + bmd->hdrlen;  /* with padding and header */

    dthread_spin_lock(&bmd->amd.alock);
    if (total <= bmd->amd.self.dt_length - bmd->allocated) {
        hdr = (uint32_t *) ( (char *) dtrs->shmmap[arena->dt_shmid].mapping +
                                      arena->dt_offset + bmd->allocated );
        hdr[0] = BREAK_BEFORE;
        hdr[1] = ((uint64_t) size) >> 32;
        hdr[2] = ((uint64_t) size) & 0xffffffff;
        hdr[3] = BREAK_AFTER;
        rv = (char *)hdr + bmd->hdrlen;   /* skip over header */
        if (newref) {
            newref->dt_shmid = arena->dt_shmid;
            newref->dt_offset = arena->dt_offset + bmd->allocated +
                                bmd->hdrlen;
            newref->dt_length = size;
        }
        bmd->allocated += total;
        bmd->nalloc++;
    }
    dthread_spin_unlock(&bmd->amd.alock);

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

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct break_malloc_md));

    /*
     * caller has already validated ref is in arena and alignment is ok.
     * we need to check for header space.
     */
    if (ref->dt_offset < bmd->amd.min_uoffset + bmd->hdrlen) {
        mlog(SHM_ERR, "break_free: input ref %p has no header", ref);
        return;
    }

    ptr = dthread_shmref2ptr(ref, 0);   /* already validated, so !NULL */
    hdr = (uint32_t *)(((char *) ptr) - bmd->hdrlen);

    if (hdr[0] != BREAK_BEFORE || hdr[3] != BREAK_AFTER) {
        mlog(SHM_ERR, "break_free: bad header on input ptr %p", ptr);
        return;
    }

    dthread_spin_lock(&bmd->amd.alock);
    hdr[0] = 0xdeaddead;      /* invalidate header */
    if (bmd->nalloc > 0)
        bmd->nalloc--;
    if (bmd->nalloc == 0)     /* all allocations freed, reset to mdsize */
        bmd->allocated = bmd->amd.min_uoffset - bmd->amd.self.dt_offset;
    n = bmd->nalloc;
    dthread_spin_unlock(&bmd->amd.alock);
    mlog(SHM_DBG, "break_free: %p (nalloc=%d)", ptr, n);

    if (n == 0)
        mlog(SHM_INFO, "break_free: all free.  reset allocator!");
}

static void *break_realloc(dthread_shmref_t *arena, dthread_shmref_t inref,
                           size_t new_size, dthread_shmref_t *newref) {
    struct break_malloc_md *bmd;
    void *ptr;
    uint32_t *hdr;
    uint64_t old_size;
    void *newptr;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct break_malloc_md));

    /*
     * caller has already validated inref is in arena and alignment is ok.
     * we need to check for header space.
     */
    if (inref.dt_offset < bmd->amd.min_uoffset + bmd->hdrlen) {
        mlog(SHM_ERR, "break_free: input ref ptr has no header room");
        return(NULL);
    }
    ptr = dthread_shmref2ptr(&inref, 0);   /* already validated */
    hdr = (uint32_t *)(((char *) ptr) - bmd->hdrlen);

    if (hdr[0] != BREAK_BEFORE || hdr[3] != BREAK_AFTER) {
        mlog(SHM_ERR, "break_realloc: bad header on input ptr %p", ptr);
        return(NULL);
    }

    old_size = ((uint64_t)hdr[1] << 32) | hdr[2];
    /* new size less or equal to current?  keep current buffer */
    if (new_size <= old_size) {
        if (newref) {
            newref->dt_shmid = arena->dt_shmid;
            newref->dt_offset =  ((char *) ptr) -
                ((char *)dtrs->shmmap[arena->dt_shmid].mapping);
            newref->dt_length = new_size;
        }
        mlog(SHM_DBG, "break_realloc: %p: no growth req", ptr);
        return(ptr);
    }

    /* grow allocation: alloc new buf, copy, free old buf */
    mlog(SHM_DBG, "break_realloc: %p: growing allocation", ptr);
    newptr = break_malloc(arena, new_size, newref);
    if (newptr == NULL)
        return(NULL);
    memcpy(newptr, ptr, old_size);
    break_free(arena, &inref);
    return(newptr);
}

/*
 * END: "break" malloc driver
 */

/*
 * list of internal allocators
 */
static dthread_shm_alloc_ops_t internal_mops[] = {
    { "break", break_init, break_finalize,
       break_malloc, break_free, break_realloc },
};

#define NINT_MOPS (sizeof(internal_mops)/sizeof(internal_mops[0]))

/*
 * lookup malloc ops by name and return mopid.  return -1 on err.
 * mopid 0 is the first internal allocator.  external allocators
 * follow the internal ones.
 */
static int dthread_get_mopid(char *name) {
    int lcv;

    /* look at user-provided mallocs first, if any */
    for (lcv = 0 ; lcv < dtrs->nusrmops ; lcv++) {
        if (strcmp(dtrs->mop[lcv].name, name) == 0)
            return(NINT_MOPS + lcv);
    }

    /* check for internal allocator */
    for (lcv = 0 ; lcv < NINT_MOPS ; lcv++) {
        if (strcmp(internal_mops[lcv].name, name) == 0)
            return(lcv);
    }

    /* unknown name */
    return(-1);
}

/*
 * convert mopid to dthread_shm_alloc_ops_t
 */
static dthread_shm_alloc_ops_t *mopid2ops(int mopid) {
    if (mopid < 0)
        return(NULL);
    if (mopid < NINT_MOPS)
        return(&internal_mops[mopid]);
    if (mopid < NINT_MOPS + dtrs->nusrmops)
        return(&dtrs->mop[mopid - NINT_MOPS]);
    return(NULL);
}

/*
 * set default arena for shm_malloc.  can only be set once.
 * prob a good idea to have all ranks set the same default.
 * XXX: consider moving setting from dtrs to shared memory?
 */
int dthread_shm_set_defaultarena(dthread_shmref_t *dflt) {
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
int dthead_shm_new_arena(uint64_t shmid, char *shmalloc_name,
                         size_t size, dthread_shmref_t *newarena) {
    int mopid, rv;
    dthread_shm_alloc_ops_t *mops;
    dthread_shm_alloc_md_t *md;

    /* size should be at least one page */
    if (size && size < dtrs->pagesize) {
        mlog(SHM_ERR, "dthead_shm_new_arena: size too small");
        return(EINVAL);
    }

    /* use name to select correct operations */
    mopid = dthread_get_mopid(shmalloc_name);
    if (mopid < 0) {
        mlog(SHM_ERR, "dthead_shm_new_arena: bad type %s", shmalloc_name);
        return(ENOENT);
    }
    mops = mopid2ops(mopid);
    if (!mops) {
        mlog(SHM_ERR, "dthead_shm_new_arena: no mops (%s)", shmalloc_name);
        return(ENOENT);
    }

    /* use low-level segment allocator to get block of shm */
    md = dthread_shmseg_alloc(shmid, size, newarena);
    if (!md) {
        mlog(SHM_ERR, "dthead_shm_new_arena: %s: segalloc failed",
             shmalloc_name);
        return(ENOMEM);
    }

    md->mdmagic = DTHREAD_SHM_MD_MAGIC;
    md->self = *newarena;

    /* mops init will increase min_uoffset as needed */
    md->min_uoffset = md->self.dt_offset + sizeof(*md);
    md->max_uoffset = md->self.dt_offset + md->self.dt_length;
    md->badalign_bits = 0;      /* let mops set this as desired */
    md->mopid = mopid;
    dthread_spin_init(&md->alock, DTHREAD_PROCESS_SHARED);

    rv = mops->init(newarena);    /* should not fail */
    if (rv != 0) {
        /* XXX: no way to give back segment memory, so it's lost... */
        mlog(SHM_CRIT, "dthead_shm_new_arena: %s init fail (%s), sz=%zd",
             shmalloc_name, strerror(rv), size);
    }

    return(rv);
}

/*
 * generic helper function that validates malloc/free/realloc args.
 * args are:
 * tag - a tag string for logging (in)
 * arenap - pointer to arena pointer, returns default if ptr==NULL (in/out)
 * mdp - pointer to metadata pointer (out)
 * ptr - optional local pointer to shared memory (in, can be NULL)
 * outptref - ptr converted to shmref (out, must != NULL if ptr is provided)
 * inref - optional shmref to shared memory (in, can be NULL)
 *
 * return: dthread_shm_alloc_ops_t on success, NULL on failure
 */
static dthread_shm_alloc_ops_t *dthread_shm_validate_args(char *tag,
        dthread_shmref_t **arenap, dthread_shm_alloc_md_t **mdp,
        void *ptr, dthread_shmref_t *outptref, dthread_shmref_t *inref) {

    dthread_shmref_t *arena;
    dthread_shm_alloc_md_t *md;
    dthread_shm_alloc_ops_t *mops;

    /* get arena, falling back to default if none give */
    arena = *arenap;
    if (arena == NULL) {     /* try to sub in default */
        arena = dtrs->defarena;
        if (arena == NULL) {
            mlog(SHM_ERR, "shm_validate: %s: no default arena", tag);
            return(NULL);
        }
        *arenap = arena;
    }

    /* extract metadata ptr and validate it */
    md = dthread_shmref2ptr(arena, sizeof(*md));
    if (!md || md->mdmagic != DTHREAD_SHM_MD_MAGIC ||
        memcmp(arena, &md->self, sizeof(md->self)) != 0) {
        mlog(SHM_ERR, "shm_validate: %s: invalid metadata", tag);
        return(NULL);
    }
    *mdp = md;

    /* get the ops so we can return them on success */
    mops = mopid2ops(md->mopid);
    if (mops == NULL) {
        mlog(SHM_ERR, "shm_validate: %s: no malloc ops", tag);
        return(NULL);
    }

    /* if we have a ptr, validate and cvt to outptrref */
    if (ptr != NULL) {
        if (dthread_ptr2shmref(ptr, 0, outptref) != 0) {
            mlog(SHM_ERR, "shm_validate: %s: bad ptr", tag);
            return(NULL);
        }

        if (outptref->dt_shmid != arena->dt_shmid ||
            outptref->dt_offset < md->min_uoffset ||
            outptref->dt_offset >= md->max_uoffset ||
            (uintptr_t) ptr & md->badalign_bits) {
            mlog(SHM_ERR, "shm_validate: %s: bad ptr value/range", tag);
            return(NULL);
        }
    }

    /* if we have an inref, validate it */
    if (inref) {
        if (inref->dt_shmid != arena->dt_shmid ||
            inref->dt_offset < md->min_uoffset ||
            inref->dt_offset >= md->max_uoffset ||
            (inref->dt_offset & md->badalign_bits) != 0) {
            mlog(SHM_ERR, "shm_validate: %s: invalid inref", tag);
            return(NULL);
        }
    }

    return(mops);    /* success! */
}

/*
 * arena-based shared memory allocate
 */
void *dthread_shm_malloc(dthread_shmref_t *arena, uint64_t len,
                         dthread_shmref_t *newref) {
    dthread_shm_alloc_md_t *md;
    dthread_shm_alloc_ops_t *mops;

    mops = dthread_shm_validate_args("malloc", &arena, &md, NULL, NULL, NULL);
    if (mops == NULL)
        return(NULL);

    return( mops->malloc(arena, len, newref) );
}

/*
 * free memory
 */
void dthread_shm_free(dthread_shmref_t *arena, void *ptr) {
    dthread_shm_alloc_md_t *md;
    dthread_shmref_t ref;
    dthread_shm_alloc_ops_t *mops;

    if (ptr == NULL)    /* freeing NULL is a noop */
        return;

    mops = dthread_shm_validate_args("free", &arena, &md, ptr, &ref, NULL);
    if (mops == NULL)
        return;

    mops->free(arena, &ref);
}

/*
 * free memory ref
 */
void dthread_shm_free_ref(dthread_shmref_t *arena, dthread_shmref_t *ref) {
    dthread_shm_alloc_md_t *md;
    dthread_shm_alloc_ops_t *mops;

    if (ref == NULL || ref->dt_length == 0)    /* freeing NULL/0 is a noop */
        return;

    mops = dthread_shm_validate_args("free_ref", &arena, &md, NULL, NULL, ref);
    if (mops == NULL)
        return;

    mops->free(arena, ref);
}

/*
 * realloc memory
 */
void *dthread_shm_realloc(dthread_shmref_t *arena, void *ptr, uint64_t len,
                          dthread_shmref_t *newref) {
    dthread_shm_alloc_md_t *md;
    dthread_shmref_t ref;
    dthread_shm_alloc_ops_t *mops;

    if (ptr == NULL)    /* realloc w/null is a malloc */
        return(dthread_shm_malloc(arena, len, newref));

    mops = dthread_shm_validate_args("realloc", &arena, &md, ptr, &ref, NULL);
    if (mops == NULL)
        return(NULL);

    return( mops->realloc(arena, ref, len, newref) );
}

/*
 * realloc memory ref
 */
void *dthread_shm_realloc_ref(dthread_shmref_t *arena, dthread_shmref_t *inref,
                              uint64_t len, dthread_shmref_t *newref) {
    dthread_shm_alloc_md_t *md;
    dthread_shm_alloc_ops_t *mops;

    if (inref == NULL || inref->dt_length == 0)  /* realloc !inref: malloc */
        return(dthread_shm_malloc(arena, len, newref));

    mops = dthread_shm_validate_args("realloc_ref", &arena, &md,
                                     NULL, NULL, inref);
    if (mops == NULL)
        return(NULL);

    return( mops->realloc(arena, *inref, len, newref) );
}
