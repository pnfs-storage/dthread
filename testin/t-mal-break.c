/*
 * t-mal-break  test break malloc
 * 06-Jul-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dthread/dthread.h>

/*
 * available thread startups and dispatch table.
 */
int app_main(int argc, char **argv);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
};

/*
 * shared memory sources
 */
dthread_shmsrc_t shmsrctable[] = {
    { "/tmp/dt.shm", DTHREAD_SRC_FILE, 0, 4*1024*1024 },
};

/*
 * process main
 */
int main(int argc, char **argv) {
    int lcv, rv;

    setlinebuf(stdout);

    printf("main: calling dthread_init\n");
    rv = dthread_init(&argc, &argv);
    if (rv != 0)
        errx(1, "dthread_init failed: %s (%d)", strerror(rv), rv);
    printf("main: dthread_init pass\n");

    /*
     * we want to allow either main or app_main to process argc/argv.
     *
     * having main process argc/argv allows it to use the args
     * to control how we call dthread_run(), but the processing
     * happens before dthreads is running (so you cannot do dthread ops).
     *
     * for this test, we manually scan for '-s' in main but
     * let app_main have argc/argv.
     */
    for (lcv = 0 ; lcv < argc - 1 ; lcv++) {
        if (strcmp(argv[lcv], "-s") == 0)
            shmsrctable[0].dt_src = argv[lcv+1];
    }
    printf("main: shmsrctable[0].dt_src = %s\n", shmsrctable[0].dt_src);

    printf("main: calling dthread_run\n");
    dthread_run(disptable, sizeof(disptable)/sizeof(disptable[0]),
                shmsrctable, sizeof(shmsrctable)/sizeof(shmsrctable[0]),
                NULL, 0,
                DTHREAD_SYNCOP_ID_PSHARED, 10, argc, argv);

    printf("main: dthread_run unexpectedly returned\n");
    exit(1);
}

int app_main(int argc, char **argv) {
    int rv, errcnt;
    dthread_shmref_t ar, ar2;
    void *vp;
    char *p[3], *q[3];
    dthread_shmref_t pr[3];

    errcnt = 0;
    printf("app_main: running\n");

    rv = dthread_shm_new_arena(0, "nope", 8192, &ar);
    if (rv == ENOENT) {
        printf("dthread_shm_new_arena: handled invalid allocator OK!\n");
    } else {
        printf("dthread_shm_new_arena: invalid allocator: unexpected %d", rv);
        errcnt++;
    }

    rv = dthread_shm_new_arena(0, "break", 100, &ar);
    if (rv == EINVAL) {
        printf("dthread_shm_new_arena: handled too small allocator OK!\n");
    } else {
        printf("dthread_shm_new_arena: small alloc: unexpected %d", rv);
        errcnt++;
    }

    rv = dthread_shm_new_arena(0, "break", 0, &ar);
    if (rv) {
        printf("dthread_shm_new_arena: unexpected fail %s\n", strerror(rv));
        errcnt++;
        goto done;
    }

    printf("break: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">\n",
           ar.dt_shmid, ar.dt_offset, ar.dt_length);
    vp = dthread_shmref2ptr(&ar, 0);
    printf("break: base = %p\n", vp);
    if (dthread_ptr2shmref(vp, ar.dt_length, &ar2) != 0) {
        printf("dthread_ptr2shmref: arena check failed\n");
        errcnt++;
    } else if (ar.dt_shmid != ar2.dt_shmid || ar.dt_offset != ar2.dt_offset ||
               ar.dt_length != ar2.dt_length) {
        printf("dthread_shm_ptr2shmref: arena mismatch!\n");
        errcnt++;
    } else {
        printf("dthread_ptr2shmref: arena check OK!\n");
    }

    p[0] = dthread_shm_malloc(NULL, 1, &pr[0]);
    if (p[0]) {
        printf("dthread_shm_malloc: worked without DEFAULT!\n");
        errcnt++;
    } else {
        printf("dthread_shm_malloc: default arena ok\n");
    }

    rv = dthread_shm_set_defaultarena(&ar);
    if (rv == 0) {
        printf("dthread_shm_set_defaultarena: default arena set\n");
    } else {
        printf("dthread_shm_set_defaultarena: default arena failed %d\n", rv);
        errcnt++;
    }

    p[0] = dthread_shm_malloc(NULL, 1, &pr[0]);
    if (p[0] == NULL) {
        printf("dthread_shm_malloc: alloc p[0] failed!\n");
        errcnt++;
    } else {
        printf("dthread_shm_malloc: alloc p[0] OK!  %p\n", p[0]);
        printf("dthread_shm_malloc: p0 <%" PRIu64 ",%" PRIu64 ",%"
               PRIu64 ">\n", pr[0].dt_shmid, pr[0].dt_offset, pr[0].dt_length);
    }

    vp = dthread_shmref2ptr(&pr[0], 0);
    if (vp != p[0]) {
        printf("dthread_shmref2ptr on p0 failed! vp=%p, p[0]=%p\n", vp, p[0]);
        errcnt++;
    } else {
        printf("dthread_shmref2ptr on p0 OK!\n");
    }

    p[1] = dthread_shm_malloc(NULL, 128, &pr[1]);
    if (p[1] == NULL) {
        printf("dthread_shm_malloc: alloc p[1] failed!\n");
        errcnt++;
    } else {
        printf("dthread_shm_malloc: alloc p[1] OK!  %p\n", p[1]);
        printf("dthread_shm_malloc: p1 <%" PRIu64 ",%" PRIu64 ",%"
               PRIu64 ">\n", pr[1].dt_shmid, pr[1].dt_offset, pr[1].dt_length);
    }

    /* we know break internal layout, so we know where p[1] should be */
    if (p[0] + alignof(max_align_t) + (sizeof(uint32_t) * 4) != p[1]) {
        printf("dthread_shm_malloc: p[1] in unexpected loc for break!\n");
        errcnt++;
    } else {
        printf("dthread_shm_malloc: p[1] location OK!\n");
    }

    p[2] = dthread_shm_malloc(NULL, 1, &pr[2]);
    if (p[0] == NULL) {
        printf("dthread_shm_malloc: alloc p[2] failed!\n");
        errcnt++;
    } else {
        printf("dthread_shm_malloc: alloc p[2] OK!  %p\n", p[2]);
    }

    /* we also know where p[2] should be... */
    if (p[1] + 128 + (sizeof(uint32_t) * 4) != p[2]) {
        printf("dthread_shm_malloc: p[2] in unexpected loc for break!\n");
        errcnt++;
    } else {
        printf("dthread_shm_malloc: p[2] location OK!\n");
        printf("dthread_shm_malloc: p2 <%" PRIu64 ",%" PRIu64 ",%"
               PRIu64 ">\n", pr[2].dt_shmid, pr[2].dt_offset, pr[2].dt_length);
    }

    printf("freeing ALL memory to reset break\n");
    dthread_shm_free(NULL, p[1]);
    dthread_shm_free_ref(NULL, &pr[0]);
    dthread_shm_free(NULL, p[2]);
    printf("done freeing memory\n");

    printf("duplicating previous allocations\n");
    q[0] = dthread_shm_malloc(NULL, 1, NULL);
    q[1] = dthread_shm_malloc(NULL, 128, NULL);
    q[2] = dthread_shm_malloc(NULL, 1, NULL);
    printf("done duplicating allocations\n");

    printf("dthread_shm_malloc: q: %p, %p, %p\n", q[0], q[1], q[2]);

   /* the dup allocations should match the initial ones! */
   if (p[0] != q[0] || p[1] != q[1] || p[2] != q[2]) {
        printf("dthread_shm_malloc: q allocations did NOT match\n");
        errcnt++;
   } else {
        printf("dthread_shm_malloc: q allocations OK!\n");
   }

done:
    printf("app_main: return %d\n", errcnt);
    return((errcnt) ? 1 : 0);
}
