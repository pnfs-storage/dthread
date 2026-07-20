/*
 * t-mal-bsd  test bsd malloc
 * 20-Jul-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <inttypes.h>
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
    int rv, errcnt, lcv;
    dthread_shmref_t ar;
    void *ptr[5], *ps, *nptr, *sptr;
    dthread_shmref_t pref[5], psav, nref, sref;

    /* t-mal-break already does basic arena api tests, don't dup here */

    errcnt = 0;
    printf("app_main: running\n");

    rv = dthread_shm_new_arena(0, "bsd", 0, &ar);
    if (rv) {
        printf("dthead_shm_new_arena: unexpected fail %s\n", strerror(rv));
        errcnt++;
        goto done;
    }

    printf("bsd: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">\n",
           ar.dt_shmid, ar.dt_offset, ar.dt_length);

    rv = dthread_shm_set_defaultarena(&ar);
    if (rv == 0) {
        printf("dthread_shm_set_defaultarena: default arena set\n");
    } else {
        printf("dthread_shm_set_defaultarena: default arena failed %d\n", rv);
        errcnt++;
    }

    for (lcv = 0 ; lcv < 3; lcv++) {
        ptr[lcv] = dthread_shm_malloc(NULL, 481, &pref[lcv]);
        if (ptr[lcv] == NULL) {
            printf("dthread_shm_malloc: FAILED %d\n", lcv);
            errcnt++;
            goto done;
        }
        printf("dthread_shm_malloc: OK %d\n", lcv);
    }

    ps = ptr[2];
    psav = pref[2];
    dthread_shm_free(NULL, ptr[2]);

    for (lcv = 2 ; lcv < 5; lcv++) {
        ptr[lcv] = dthread_shm_malloc(NULL, 481, &pref[lcv]);
        if (lcv == 2) {
            if (ptr[2] != ps || memcmp(&psav, &pref[2], sizeof(psav)) != 0) {
                printf("dthread_shm_malloc: free list alloc FAILED %d\n", lcv);
                printf(" -- check: %d %d\n", ptr[2] != ps,
                       memcmp(&psav, &pref[2], sizeof(psav)));
                errcnt++;
            } else {
                printf("dthread_shm_malloc: free list alloc OK %d\n", lcv);
            }
        }
        if (ptr[lcv] == NULL) {
            printf("dthread_shm_malloc: FAILED %d\n", lcv);
            errcnt++;
            goto done;
        }
        printf("dthread_shm_malloc: OK %d\n", lcv);
    }

    nptr = dthread_shm_realloc(NULL, ptr[4], 481, &nref);
    if (nptr != ptr[4] || memcmp(&nref, &pref[4], sizeof(nref)) != 0) {
        printf("realloc-no-change: FAILED! %d %d\n", nptr != ptr[4],
               memcmp(&nref, &pref[4], sizeof(nref)));
        errcnt++;
    } else {
        printf("realloc-no-change: AOK!\n");
    }

#define MSG "testing realloc!"
    memcpy(nptr, MSG, sizeof(MSG));

    nptr = dthread_shm_realloc(NULL, nptr, 4000, &nref);
    if (nptr == NULL || nptr == ptr[4] || 
        memcmp(nptr, MSG, sizeof(MSG)) != 0) {
        printf("grow realloc failed!  %p\n", nptr);
        errcnt++;
    } else {
        printf("grow realloc AOK!\n");
    }

#define MSG2 "REtesting realloc!!"
    memcpy(nptr, MSG2, sizeof(MSG2));

    sptr = dthread_shm_realloc(NULL, nptr, 64, &sref);
    if (sptr == NULL || sptr == nptr || 
        memcmp(sptr, MSG2, sizeof(MSG2)) != 0) {
        printf("shrink realloc failed!  %p\n", sptr);
        errcnt++;
    } else {
        printf("shrink realloc AOK!\n");
    }

    nptr = dthread_shm_malloc(NULL, 481, &nref);
    if (nptr != ptr[4] || memcmp(&nref, &pref[4], sizeof(nref)) != 0) {
        printf("orig size freelist reclaim failed!\n");
        errcnt++;
    } else {
        printf("orig size freelist reclaim AOK!\n");
    }

done:
    printf("app_main: return %d\n", errcnt);
    return((errcnt) ? 1 : 0);
}
