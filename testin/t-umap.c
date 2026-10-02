/*
 * t-umap  test uniform memory mapping feature
 * 29-Sep-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <dthread/dthread.h>

#define MSG "This is a test"

/*
 * available thread startups and dispatch table.
 */
int app_main(int argc, char **argv);
dthread_argret_t rem_proc(dthread_argret_t *dt_arg);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
    { "proc", { .start = rem_proc }, NULL, NULL },
};

/*
 * shared memory sources
 */
dthread_shmsrc_t shmsrctable[] = {
    { "/tmp/dt.shm", DTHREAD_SRC_FILE|DTHREAD_SRC_UMAP, 0, 4*1024*1024 },
    { "/tmp/d2.shm", DTHREAD_SRC_FILE, 0, 4*1024*1024 },
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
    int errcnt, rv, lcv;
    dthread_shmref_t uar, xar, upref[3], xpref[3];
    void *uptr[3], *xptr[3], *rptr;
    dthread_argret_t arg, ret;
    dthread_t child;

    errcnt = 0;
    printf("app_main: running\n");

    rv = dthread_shm_new_arena(0, "bsd", 0, &uar);
    if (rv) {
        printf("dthead_shm_new_arena: unexpected fail1 %s\n", strerror(rv));
        errcnt++;
        goto done;
    }

    rv = dthread_shm_new_arena(1, "bsd", 0, &xar);
    if (rv) {
        printf("dthead_shm_new_arena: unexpected fail2 %s\n", strerror(rv));
        errcnt++;
        goto done;
    }

    printf("uar: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">, r0_VA=%p\n",
           uar.dt_shmid, uar.dt_offset, uar.dt_length,
           dthread_shmref2ptr(&uar, 0));
    printf("xar: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">, r0_VA=%p\n",
           xar.dt_shmid, xar.dt_offset, xar.dt_length,
           dthread_shmref2ptr(&xar, 0));

    /* do 3 allocations of 481 in uar */
    for (lcv = 0 ; lcv < 3; lcv++) {
        uptr[lcv] = dthread_shm_malloc(&uar, 481, &upref[lcv]);
        if (uptr[lcv] == NULL) {
            printf("dthread_shm_malloc: FAILED %d\n", lcv);
            errcnt++;
            goto done;
        }
    }
    printf("allocated OK in uar\n");

    /* do 3 allocations of 481 in xar */
    for (lcv = 0 ; lcv < 3; lcv++) {
        xptr[lcv] = dthread_shm_malloc(&xar, 481, &xpref[lcv]);
        if (xptr[lcv] == NULL) {
            printf("dthread_shm_malloc: FAILED %d\n", lcv);
            errcnt++;
            goto done;
        }
    }
    printf("allocated OK in xar\n");

    printf("putting MSG in entry 2 of both allocations\n");
    printf("uar[2]: local=%p\n", uptr[2]);
    printf("uar[2]: shmref <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">\n",
           upref[2].dt_shmid, upref[2].dt_offset, upref[2].dt_length);
    memcpy(uptr[2], MSG, sizeof(MSG));
    printf("xar[2]: local=%p\n", xptr[2]);
    printf("xar[2]: shmref <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">\n",
           xpref[2].dt_shmid, xpref[2].dt_offset, xpref[2].dt_length);
    memcpy(xptr[2], MSG, sizeof(MSG));

    printf("app_main: testing umap on 4 children\n");
    for (lcv = 0 ; lcv < 4 ; lcv++) {
        arg.dt_argret_type = DTHREAD_SHMREF;
        arg.u.dt_shm = upref[2];
        rv = dthread_ncreate(&child, NULL, rem_proc, &arg);
        if (rv != 0) {
            printf("app_main: ncreate failed! (%s)\n", strerror(rv));
            errcnt++;
            continue;
        } else {
            printf("app_main: created!\n");
        }
        printf("join child\n");
        rv = dthread_njoin(child, &ret);
        if (rv) {
            printf("app_main: join error %s", strerror(rv));
            errcnt++;
        } else {
            printf("app_main: join success!\n");
            if (ret.dt_argret_type != DTHREAD_INLINE ||
                ret.dt_inlinelen != sizeof(rptr)) {
                printf("app_main: bad return value!\n");
                errcnt++;
            } else {
                memcpy(&rptr, ret.u.dt_inline, sizeof(rptr));
                printf("app_main: child pointer %p\n", rptr);
                if (rptr != uptr[2]) {
                    printf("app_main: UMAP PTR MISMATCH %p != %p\n",
                           rptr, uptr[2]);
                    errcnt++;
                } else {
                    printf("app_main: umap pointer match AOK!\n");
                }
            }
        }
    }

    printf("app_main: testing xmap on 4 children\n");
    for (lcv = 0 ; lcv < 4 ; lcv++) {
        arg.dt_argret_type = DTHREAD_SHMREF;
        arg.u.dt_shm = xpref[2];
        rv = dthread_ncreate(&child, NULL, rem_proc, &arg);
        if (rv != 0) {
            printf("app_main: ncreate failed! (%s)\n", strerror(rv));
            errcnt++;
            continue;
        } else {
            printf("app_main: created!\n");
        }
        printf("join child\n");
        rv = dthread_njoin(child, &ret);
        if (rv) {
            printf("app_main: join error %s", strerror(rv));
            errcnt++;
        } else {
            printf("app_main: join success!\n");
            if (ret.dt_argret_type != DTHREAD_INLINE ||
                ret.dt_inlinelen != sizeof(rptr)) {
                printf("app_main: bad return value!\n");
                errcnt++;
            } else {
                memcpy(&rptr, ret.u.dt_inline, sizeof(rptr));
                printf("app_main: child pointer %p\n", rptr);
                if (rptr != xptr[2]) {
                    printf("app_main: diff !umap ptr: %p != %p (is ok!)\n",
                           rptr, xptr[2]);
                } else {
                    printf("app_main: same !umap ptr: %p (is ok!)\n", rptr);
                }
            }
        }
    }

    printf("freeing memory\n");
    for (lcv = 0 ; lcv < 3 ; lcv++) {
        dthread_shm_free(&uar, &upref[lcv]);
    }
    for (lcv = 0 ; lcv < 3 ; lcv++) {
        dthread_shm_free(&xar, &xpref[lcv]);
    }

done:
    printf("app_main: return %d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

dthread_argret_t rem_proc(dthread_argret_t *dt_arg) {
    dthread_shmref_t ar;
    void *ptr;
    dthread_argret_t ret;
    int cmp;

    ret.dt_argret_type = DTHREAD_NODATA;
    printf("rem_proc: running\n");
    printf("rem_proc: arg_ok=%d\n", dt_arg->dt_argret_type == DTHREAD_SHMREF);
    ar = dt_arg->u.dt_shm;
    ptr = dthread_shmref2ptr(&ar, 0);
    printf("child-ref: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 "> ptr=%p\n",
           ar.dt_shmid, ar.dt_offset, ar.dt_length, ptr);
    if (ptr && (cmp = memcmp(ptr, MSG, sizeof(MSG))) == 0) {
        printf("child: DATA OK!\n");
        ret.dt_argret_type = DTHREAD_INLINE;
        ret.dt_inlinelen = sizeof(ptr);
        memcpy(ret.u.dt_inline, &ptr, sizeof(ptr));
    } else {
        printf("child: DATA ERROR\n");
    }
    printf("rem_proc: done\n");
    return(ret);
}
