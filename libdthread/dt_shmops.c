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

    mlog(SHM_DBG, "break_malloc: %s ret=%p a=%p <%" PRIu64 ",%" PRIu64
             ",%" PRIu64 ">", (rv == NULL) ? "NUL" : "AOK", rv, arena,
             arena->dt_shmid, arena->dt_offset, arena->dt_length);
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
    mlog(SHM_DBG, "break_free: AOK nalloc=%d, p=%p", n, ptr);

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
        mlog(SHM_DBG, "break_realloc: no growth req for %p", ptr);
        return(ptr);
    }

    /* grow allocation: alloc new buf, copy, free old buf */
    mlog(SHM_DBG, "break_realloc: growing allocation for %p", ptr);
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
 * START: "bsd" malloc driver
 *
 * this is the old bsd/caltech malloc ported to the dthread shm
 * environment.   we use a break line in the arena to simulate
 * the sbrk() system call.   memory allocated from the break is
 * never given back to the break (it goes on free lists instead).
 * buffers are allocated in fixed bucket sizes.  we prepend each
 * allocated block with a header (the 'overhead' union).  for
 * allocated blocks the overhead contains a magic number and the id
 * of the bucket the buffer was allocated from.  for free blocks
 * the overhead is used to store the offset of the next buffer on
 * the bucket's free list.   once memory is assigned to a bucket
 * of a given size it always stays in that bucket.   bsd_morecore()
 * is the function that allocates new memory from the break
 * and places it in the specified bucket.   if the bucket's size
 * is less than one page we allocate a single page and divide
 * it up into bucket-sized blocks.  for allocations greater than
 * or equal to one page we allocate a single block of the requested
 * bucket's size plus one extra page.  the overhead goes at the
 * start of these blocks.  malloc returns a pointer to space after
 * the overhead to the user.   we set the size of the smallest
 * bucket to 16 bytes (matches the alignment on 64 bit systems).
 */

/*	NetBSD: malloc.c,v 1.2 2003/08/07 16:42:01 agc Exp	*/
/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * malloc.c (Caltech) 2/21/82
 * Chris Kingsley, kingsley@cit-20.
 *
 * This is a very fast storage allocator.  It allocates blocks of a small
 * number of different sizes, and keeps free lists of each size.  Blocks
 * that don't exactly fit are passed up to the next larger size.
 */

/*
 * defines
 */
#define NBUCKETS    60          /* number of size buckets */
#define	MAGIC       0x5555      /* magic # on accounting info */

/* bucket# to size in bytes - smallest bucket is 16 bytes */
#define BUCKSIZE(X) (1 << ((X) + 4))

/* macros to read/set magic number and index for allocated blocks */
#define OV_MAGIC(X) ((X) & 0xffff)
#define OV_INDEX(X) (((X) >> 16) & 0xff)

#define OV_MAGIDX(I) ((((I) & 0xff) << 16) | MAGIC)

/*
 * The overhead of an allocated block.  When free, this space contains
 * the shm offset to the next free block.  When in use, it has
 * a magic number and the size index.  any remaining overhead bytes
 * are for alignment to max_align_t. The overhead is prepended to the
 * pointer returned to the user.
 */
union overhead {
    uint64_t ov_next;            /* offset of next block (when free) */
    uint64_t ov_magidx;          /* magic#/index of block (when allocated) */
};

/*
 * global state for this malloc (pointed to by the arena shmref).
 * nextf[i] is the offset of the next free block of size 2^(i+4).
 * The smallest allocatable block is 16 bytes (but will be larger
 * if max_align_t is).  we pad this structure's size up so that
 * it ends on a max_align_t boundary.
 */
struct bsd_malloc_md {
    dthread_shm_alloc_md_t amd;  /* generic metadata */
    int oversize;                /* size to reserve for overhead */
    int minbucket;               /* min we can use w/overhead and max_align_t */
    int pagesz;                  /* page size */
    int pagebucket;              /* page size bucket */
    uint64_t nextf[NBUCKETS];    /* free lists offset, by bucket */
    uint64_t nmalloc[NBUCKETS];  /* #mallocs - #frees for a block size */
    uint64_t mycore;             /* offset of free arena core */
    uint64_t core_resid;         /* how much core we have left */
};

/*
 * bsd_mstats - log statistics about bsd malloc.  caller should be holding
 * the spin lock.  for each active bucket print the freelist size and
 * number of mallocs-frees.
 */
static void bsd_mstats(struct bsd_malloc_md *md) {
    uint64_t totfree, totused, bfree, bused, fp;
    dthread_shmref_t nxt;
    int i, sz;
    union overhead *op;

    totfree = totused = 0;
    nxt.dt_shmid = md->amd.self.dt_shmid;
    nxt.dt_length = md->oversize;

    /*
     * note: reported byte counts do not account for overhead space
     * or extra page space for allocs >= pagesize.
     */

    mlog(SHM_INFO, "bsd memory allocation stats <%" PRIu64 ",%" PRIu64">",
         md->amd.self.dt_shmid, md->amd.self.dt_offset);
    for (i = 0 ; i < NBUCKETS ; i++) {
        sz = BUCKSIZE(i);
        bfree = bused = 0;

        /* walk the free list and count free blocks */
        for (fp = md->nextf[i] ; fp != 0 ; bfree += sz) {
            nxt.dt_offset = fp;
            op = dthread_shmref2ptr(&nxt, md->oversize);
            if (op == NULL) {
                mlog(SHM_INFO, "bsd_mstats: unexpected error in list %d", i);
                return;
            }
            fp = op->ov_next;
        }
        totfree += bfree;

        bused = md->nmalloc[i] * sz;
        if (bfree == 0 && bused == 0)
            continue;

        totused += bused;

        mlog(SHM_INFO, "bucket=%d, size=%d, used=%" PRIu64 ", free=%" PRIu64,
             i, sz, bused, bfree);
    }

    mlog(SHM_INFO, "total in use: %" PRIu64 ", total free: %" PRIu64,
	    totused, totfree);
}

/*
 * Allocate more memory to the specified bucket.   called with the
 * spin lock held.  the allocated memory is used for both overhead
 * and user data (carving out overhead space is handled at a higher
 * level).
 */
static void bsd_morecore(struct bsd_malloc_md *md, int bucket) {
    uint64_t sz, amt, nblks, curoff;
    dthread_shmref_t oref;
    union overhead *op;

    if (bucket < md->minbucket || bucket >= NBUCKETS) {
        mlog(SHM_ERR, "bsd_morecore: bad bucket %d", bucket);
        return;
    }
    sz = BUCKSIZE(bucket);

    /*
     * for allocations less than one page we allocate a single page
     * and divide it up into nblks blocks of size sz.
     *
     * for allocations greater than or equal to one page we allocate
     * one block of the requested size plus an extra page.
     */
    if (sz < md->pagesz) {
        amt = md->pagesz;       /* allocate a whole page and divide it up */
        nblks = amt / sz;
    } else {
        amt = sz + md->pagesz;  /* allocate req'd size plus a page */
        nblks = 1;
    }

    if (md->core_resid < amt) {
        mlog(SHM_WARN, "bsd_morecore: out of memory (b=%d,resid=%" PRId64 ")",
             bucket, md->core_resid);
        return;                 /* fail.  no space! */
    }

    oref.dt_shmid = md->amd.self.dt_shmid;
    oref.dt_offset = md->mycore;
    oref.dt_length = amt;
    op = dthread_shmref2ptr(&oref, amt);
    if (op == NULL) {
        mlog(SHM_CRIT, "bsd_morecore: dthread_shmref2ptr failed!");
        return;                 /* should not happen */
    }

    mlog(SHM_INFO, "bsd_morecore: bucket/size=%d/%" PRId64 ": amt=%" PRId64
                   " nblks=%" PRId64, bucket, sz, amt, nblks);

    curoff = oref.dt_offset;
    md->mycore += amt;
    md->core_resid -= amt;
    mlog(SHM_INFO, "bsd_morecore: curoff=%" PRId64 ", core now @ %" PRId64
                   " resid=%" PRId64, curoff, md->mycore, md->core_resid);
    mlog(SHM_DBG, "bsd_morecore: md=%p, op=%p", md, op);

    /*
     * Add new memory allocated to that on
     * free list for this hash bucket.
     */
    md->nextf[bucket] = curoff;
    mlog(SHM_DBG, "bsd_morecore: new: bk=%d, offset=%" PRIu64 ", op=%p",
         bucket, curoff, op);
    while (--nblks > 0) {
        curoff += sz;
        op->ov_next = curoff;
        op = (union overhead *)((char *) op + sz);
        mlog(SHM_DBG, "bsd_morecore: new: bk=%d, offset=%" PRIu64 ", op=%p",
             bucket, curoff, op);
    }
    op->ov_next = 0;            /* new end of list */
}

/*
 * bsd malloc operations
 */
static int bsd_init(dthread_shmref_t *arena) {
    size_t mdsize;
    struct bsd_malloc_md *bmd;

    mdsize = mdalign_size(arena, sizeof(*bmd));
    bmd = dthread_shmref2ptr(arena, mdsize);
    if (bmd == NULL) {
        mlog(SHM_ERR, "bsd_init: failed, arena=%p", arena);
        return(ENOMEM);
    }

    dthread_spin_lock(&bmd->amd.alock);

    /* adjust amd: account for our alignment and extra metadata */
    bmd->amd.min_uoffset = bmd->amd.self.dt_offset + mdsize;
    bmd->amd.badalign_bits = alignsz - 1;

    /* make overhead size a multiple of alignsz */
    for (bmd->oversize = 0 ; bmd->oversize < sizeof(union overhead) ; ) {
         bmd->oversize += alignsz;
    }

    /* determine minbucket (bucket for 1 byte user data + overhead) */
    bmd->minbucket = 0;
    while (BUCKSIZE(bmd->minbucket) < 1 + bmd->oversize)
        bmd->minbucket++;

    bmd->pagesz = getpagesize();
    bmd->pagebucket = 0;
    while (BUCKSIZE(bmd->pagebucket) < bmd->pagesz)
        bmd->pagebucket++;
    memset(&bmd->nextf[0], 0, sizeof(bmd->nextf));
    memset(&bmd->nmalloc[0], 0, sizeof(bmd->nmalloc));

    bmd->mycore = arena->dt_offset + mdsize;
    bmd->core_resid = arena->dt_length - mdsize;
    dthread_spin_unlock(&bmd->amd.alock);

    mlog(SHM_INFO, "bsd_init: OK %p <%" PRIu64 ",%" PRIu64 ",%" PRIu64
         ">", arena, arena->dt_shmid, arena->dt_offset, arena->dt_length);
    mlog(SHM_INFO, "bsd_init: mdsize=%zd, oversize=%d, min/page bucket=%d/%d",
                    mdsize, bmd->oversize, bmd->minbucket, bmd->pagebucket);
    mlog(SHM_INFO, "bsd_init: core off=%" PRId64 ", res=%" PRId64,
         bmd->mycore, bmd->core_resid);
    return(0);
}

static int bsd_finalize(dthread_shmref_t *arena) {
    struct bsd_malloc_md *bmd;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct bsd_malloc_md));

    dthread_spin_lock(&bmd->amd.alock);
    bsd_mstats(bmd);
    dthread_spin_unlock(&bmd->amd.alock);

    return(0);
}

static void *bsd_malloc(dthread_shmref_t *arena, size_t size,
                        dthread_shmref_t *newref) {
    struct bsd_malloc_md *bmd;
    size_t amt, n;
    int bucket;
    uint64_t ovoff;
    dthread_shmref_t ovref;
    union overhead *ovp;
    void *rv;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct bsd_malloc_md));

    /*
     * Convert amount of memory requested into closest block size
     * stored in hash buckets which satisfies request.
     * Account for space used per block for accounting.
     *
     * for allocations less than 1 page, bsd_morecore() allocates
     * a page and divides it up by the bucketsize.  we have to
     * reserve bmd->oversize in the buffer for overhead.
     *
     * for allocations >= 1 page, bsd_morecore() allocates the bucketsize
     * plus one page bytes, but we need bmd->oversize bytes of the
     * extra page for overhead.
     */
    if (size <= (bmd->pagesz / 2) - bmd->oversize) {
        amt = BUCKSIZE(bmd->minbucket);
        bucket = bmd->minbucket;
        n = -bmd->oversize;               /* remove overhead */
    } else {
        amt = bmd->pagesz;
        bucket = bmd->pagebucket;
        n = bmd->pagesz - bmd->oversize;  /* add free part of extra page */
    }

    while (size > amt + n) {          /* determine the bucket to use */
        amt <<= 1;
        if (amt == 0)
            return (NULL);
        bucket++;
    }
    mlog(SHM_DBG, "bsd_malloc(%zd): bucket=%d/%d (adj=%zd,actual=%zd)",
         size, bucket, BUCKSIZE(bucket), n, BUCKSIZE(bucket)+n);

    dthread_spin_lock(&bmd->amd.alock);
    /*
     * If nothing in hash bucket right now,
     * request more memory from the system.
     */
    if ((ovoff = bmd->nextf[bucket]) == 0) {
        bsd_morecore(bmd, bucket);
        if ((ovoff = bmd->nextf[bucket]) == 0) {
            dthread_spin_unlock(&bmd->amd.alock);
            mlog(SHM_ERR, "bsd_malloc: no MEM %p <%" PRIu64 ",%" PRIu64
                 ",%" PRIu64 ">", arena, arena->dt_shmid, arena->dt_offset,
                 arena->dt_length);
            return(NULL);
        }
    }

    ovref.dt_shmid = bmd->amd.self.dt_shmid;
    ovref.dt_offset = ovoff;
    ovref.dt_length = bmd->oversize;

    ovp = dthread_shmref2ptr(&ovref, bmd->oversize);
    if (ovp == NULL) {
        mlog(SHM_ERR, "bsd_malloc: shmref2ptr fail %p <%" PRIu64 ",%" PRIu64
             ",%" PRIu64 ">", arena, arena->dt_shmid, arena->dt_offset,
             arena->dt_length);
        rv = NULL;
    } else {
        bmd->nextf[bucket] = ovp->ov_next;   /* remove from linked list */
        ovp->ov_magidx = OV_MAGIDX(bucket);
  	bmd->nmalloc[bucket]++;
        rv = (char *) ovp + bmd->oversize;
        if (newref) {
            newref->dt_shmid = ovref.dt_shmid;
            newref->dt_offset = ovref.dt_offset + bmd->oversize;
            newref->dt_length = size;
        }
    }

    dthread_spin_unlock(&bmd->amd.alock);

    mlog(SHM_DBG, "bsd_malloc: %s ret=%" PRId64 "/%p a=%p <%" PRIu64
                  ",%" PRIu64 ",%" PRIu64 ">",
                  (rv == NULL) ? "NUL" : "AOK",
                  ovref.dt_offset + bmd->oversize, rv, arena,
                  arena->dt_shmid, arena->dt_offset, arena->dt_length);
    return(rv);
}

static void bsd_free(dthread_shmref_t *arena, dthread_shmref_t *ref) {
    struct bsd_malloc_md *bmd;
    void *ptr;
    union overhead *ovp;
    uint64_t curoff;
    int bucket;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct bsd_malloc_md));

    /*
     * caller has already validated ref is in arena and alignemnt is ok.
     * we need to check for overhead space.
     */
    if (ref->dt_offset < bmd->amd.min_uoffset + bmd->oversize) {
        mlog(SHM_ERR, "bsd_free: bad iref - no overhead space");
        return;
    }

    ptr = dthread_shmref2ptr(ref, 0);   /* already validated, so !NULL */
    ovp = (union overhead *)(void *)((char *) ptr - bmd->oversize);

    if (OV_MAGIC(ovp->ov_magidx) != MAGIC) {
        mlog(SHM_ERR, "bsd_free: buffer %p missing magic number", ovp);
        return;
    }

    curoff = ref->dt_offset - bmd->oversize;
    bucket = OV_INDEX(ovp->ov_magidx);
    if (bucket < 0 || bucket >= NBUCKETS) {
        mlog(SHM_ERR, "bsd_free: buffer %p: bad bucket %d", ovp, bucket);
        return;
    }

    dthread_spin_lock(&bmd->amd.alock);
    ovp->ov_next = bmd->nextf[bucket];    /* also clobbers magic */
    bmd->nextf[bucket] = curoff;
    bmd->nmalloc[bucket]--;
    dthread_spin_unlock(&bmd->amd.alock);

    mlog(SHM_DBG, "bsd_free: bucket=%d, p/o=%p/%" PRId64, bucket, ovp, curoff);
}

/*
 * note: support for the old "compaction" mechanism of calling
 * realloc() on a chunk of memory that has already been freed
 * (and thus is on the nextf[] free list) has been removed.
 *
 * realloc itself does not require locking (locking will be
 * handled by any calls made to malloc/free).
 */
static void *bsd_realloc(dthread_shmref_t *arena, dthread_shmref_t inref,
                         size_t new_size, dthread_shmref_t *newref) {
    struct bsd_malloc_md *bmd;
    void *ptr, *newptr;
    union overhead *ovp;
    int bucket;
    uint64_t bucksize, prevbucksize;

    /* will not fail as init has already setup/checked arena metadata */
    bmd = dthread_shmref2ptr(arena, sizeof(struct bsd_malloc_md));

    /*
     * caller has already validated inref is in arena and alignemnt is ok.
     * we need to check for overhead space.
     */
    if (inref.dt_offset < bmd->amd.min_uoffset + bmd->oversize) {
        mlog(SHM_ERR, "bsd_realloc: bad iref - no overhead space");
        return(NULL);
    }

    ptr = dthread_shmref2ptr(&inref, 0);   /* already validated, so !NULL */
    ovp = (union overhead *)(void *)((char *) ptr - bmd->oversize);

    if (OV_MAGIC(ovp->ov_magidx) != MAGIC) {
        mlog(SHM_ERR, "bsd_realloc: buffer missing magic number: %p", ovp);
        return(NULL);
    }

    bucket = OV_INDEX(ovp->ov_magidx);
    if (bucket < bmd->minbucket || bucket >= NBUCKETS) {
        mlog(SHM_ERR, "bsd_realloc: buffer bad bucket %d (%p)", bucket, ovp);
        return(NULL);
    }

    bucksize = BUCKSIZE(bucket);
    if (bucksize < bmd->pagesz) {       /* adjust bucksize for overhead */
        bucksize -= bmd->oversize;
    } else {
        bucksize += (bmd->pagesz - bmd->oversize);
    }

    /* determine size of previous bucket, if there is one */
    if (bucket <= bmd->minbucket) {
        prevbucksize = 0;             /* already in smallest bucket */
    } else {
        prevbucksize = BUCKSIZE(bucket - 1);
        if (prevbucksize < bmd->pagesz) {
            prevbucksize -= bmd->oversize;
        } else {
            prevbucksize += (bmd->pagesz - bmd->oversize);
        }
    }
    /* we adjusted bucksize/prevbucksize so we can compare with new_size */

    /* not growing?  avoid the copy if we need the same sized block */
    if (new_size <= bucksize &&
        (prevbucksize == 0 || new_size > prevbucksize)) {
            if (newref) {
                newref->dt_shmid = inref.dt_shmid;
                newref->dt_offset = inref.dt_offset;
                newref->dt_length = new_size;
            }
            mlog(SHM_DBG, "bsd_realloc: no change %d (%p)", bucket, ovp);
            return(ptr);         /* no change! */
    }

    /*
     * size grew/shrank enough for a new bucket size.  malloc new
     * buffer, copy data, free old buffer, and return new buffer.
     */
    newptr = bsd_malloc(arena, new_size, newref);
    if (newptr == NULL) {
        mlog(SHM_ERR, "bsd_realloc: realloc malloc failed");
        return(NULL);
    }

    memcpy(newptr, ptr, (new_size < bucksize) ? new_size : bucksize);
    bsd_free(arena, &inref);
    mlog(SHM_DBG, "bsd_realloc: obucket=%d/%d, new_size=%" PRIu64
         ", new_off=%" PRIu64 ", p=%p",
         bucket, BUCKSIZE(bucket), new_size, newref->dt_offset, newptr);
    return(newptr);
}

/*
 * END: "bsd" malloc driver
 */

/*
 * list of internal allocators
 */
static dthread_shm_alloc_ops_t internal_mops[] = {
    { "break", break_init, break_finalize,
       break_malloc, break_free, break_realloc },
    { "bsd", bsd_init, bsd_finalize, bsd_malloc, bsd_free, bsd_realloc },
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
int dthread_shm_new_arena(uint64_t shmid, char *shmalloc_name,
                         size_t size, dthread_shmref_t *newarena) {
    int mopid, rv;
    dthread_shm_alloc_ops_t *mops;
    dthread_shm_alloc_md_t *md;

    /* size should be at least one page */
    if (size && size < dtrs->pagesize) {
        mlog(SHM_ERR, "dthread_shm_new_arena: size too small");
        return(EINVAL);
    }

    /* use name to select correct operations */
    mopid = dthread_get_mopid(shmalloc_name);
    if (mopid < 0) {
        mlog(SHM_ERR, "dthread_shm_new_arena: bad type %s", shmalloc_name);
        return(ENOENT);
    }
    mops = mopid2ops(mopid);
    if (!mops) {
        mlog(SHM_ERR, "dthread_shm_new_arena: no mops (%s)", shmalloc_name);
        return(ENOENT);
    }

    /* use low-level segment allocator to get block of shm */
    md = dthread_shmseg_alloc(shmid, size, newarena);
    if (!md) {
        mlog(SHM_ERR, "dthread_shm_new_arena: %s: segalloc failed",
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
        mlog(SHM_CRIT, "dthread_shm_new_arena: %s init fail (%s), sz=%zd",
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
 * finalize arena
 *
 * currently finalize does not do anything other than log some stats
 * when logging is enabled (e.g. for debugging), so calling it is
 * not required.
 */
int dthread_shm_finalize_arena(dthread_shmref_t *arena) {
    dthread_shm_alloc_md_t *md;
    dthread_shm_alloc_ops_t *mops;

    mops = dthread_shm_validate_args("finalize", &arena, &md, NULL, NULL, NULL);
    if (mops == NULL)
        return(EINVAL);

    return( mops->finalize(arena) );
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
