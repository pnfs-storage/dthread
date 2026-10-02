/*
 * t-reserve  test shm seg reserve option
 * 02-Oct-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <dthread/dthread.h>
#include <dthread/dthread_seginfo.h>

#define URESFRONT 1
#define URESBACK 1
#define XRESFRONT 4096
#define XRESBACK 4096

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
    { "/tmp/dt.shm", DTHREAD_SRC_FILE|DTHREAD_SRC_UMAP, 0, 4*1024*1024,
      NULL, URESFRONT, URESBACK},
    { "/tmp/d2.shm", DTHREAD_SRC_FILE, 0, 4*1024*1024,
      NULL, XRESFRONT, XRESBACK },
};

static void print_seginfo(char *tag, dthread_seginfo_t *sip) {
    printf("%s: mapping=%p, size=%" PRId64 ", mdoff=%" PRId64 "\n", tag,
           sip->si_mapping, sip->si_size, sip->si_mdoffset);
    printf("%s: srcflags=%#" PRIx64 ", umap=%p, first=%" PRId64
           ", end=%" PRId64 "\n", tag, sip->si_srcflags,
           (void*)sip->si_umapaddr, sip->si_firstavail, sip->si_endavail);
}

static uint64_t rup(uint64_t val, uint64_t sz) {
    uint64_t pad = val % sz;
    if (pad) {
        val = val + (sz - pad);
    }
    return(val);
}

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
    int errcnt = 0, pgsz = getpagesize();
    int rv, lcv;
    dthread_seginfo_t si, nsi;
    dthread_shmref_t uar, upref[3], xar, xpref[3];
    void *uptr[3], *xptr[3];

    printf("app_main: running\n");

    printf("\nRUNNING: segment 0 (umap, contains gtab, etc.)\n\n");
    rv = dthread_shm_seginfo(0, &si);
    if (rv) {
        printf("dthread_shm_seginfo: unexpected ufail %s\n", strerror(rv));
        errcnt++;
        goto done;
    }
    print_seginfo("0-U", &si);
    if (si.si_mdoffset != rup(URESFRONT, pgsz)) {
        printf("dthread_shm_seginfo: bad mdoffset\n");
        errcnt++;
        goto done;
    }
    if (si.si_umapaddr && (void*) si.si_umapaddr != si.si_mapping) {
        printf("dthread_shm_seginfo: bad umap info in md\n");
        errcnt++;
        goto done;
    }
    if (si.si_firstavail < si.si_mdoffset + pgsz) {
        printf("dthread_shm_seginfo: short first avail val\n");
        errcnt++;
        goto done;
    }
    printf("0-U: initial allocation %" PRId64 "\n",  /* gtab, etc. */
           si.si_firstavail - (si.si_mdoffset + pgsz));


    printf("0-U: using rest of segment for bsd malloc\n");
    rv = dthread_shm_new_arena(0, "bsd", 0, &uar);
    if (rv) {
        printf("dthead_shm_new_arena: unexpected fail1 %s\n", strerror(rv));
        errcnt++;
        goto done;
    }
    rv = dthread_shm_seginfo(0, &nsi);
    if (rv) {
        printf("dthread_shm_seginfo: unexpected ufail %s\n", strerror(rv));
        errcnt++;
        goto done;
    }
    print_seginfo("0-Ub", &nsi);
    if (nsi.si_firstavail != nsi.si_endavail) {
        printf("dthread_shm_seginfo: did not use up segment?\n");
        errcnt++;
        goto done;
    }

    printf("0-Ua: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">, r0_VA=%p\n",
           uar.dt_shmid, uar.dt_offset, uar.dt_length,
           dthread_shmref2ptr(&uar, 0));
    if (uar.dt_shmid != 0 || uar.dt_offset != si.si_firstavail ||
        uar.dt_length != si.si_endavail - si.si_firstavail) {
        printf("dthread_shm_seginfo: unexpected u arena %s\n", strerror(rv));
        errcnt++;
        goto done;
    }

    /* do 3 allocations of 481 in uar */
    for (lcv = 0 ; lcv < 3; lcv++) {
        uptr[lcv] = dthread_shm_malloc(&uar, 481, &upref[lcv]);
        if (uptr[lcv] == NULL) {
            printf("dthread_shm_malloc: FAILED %d\n", lcv);
            errcnt++;
            goto done;
        }
        if (upref[lcv].dt_shmid != 0 ||
            upref[lcv].dt_offset < si.si_firstavail ||
            upref[lcv].dt_offset >= si.si_endavail - 481) {
            printf("dthread_shm_malloc: RANGE ERROR %d\n", lcv);
            errcnt++;
            goto done;
        }
    }
    printf("allocated OK in uar\n");

    printf("\nRUNNING: segment 1 (non-umap)\n\n");
    rv = dthread_shm_seginfo(1, &si);
    if (rv) {
        printf("dthread_shm_seginfo: unexpected xfail %s\n", strerror(rv));
        errcnt++;
        goto done;
    }
    print_seginfo("1-X", &si);
    if (si.si_mdoffset != rup(XRESFRONT, pgsz)) {
        printf("dthread_shm_seginfo: bad mdoffset\n");
        errcnt++;
        goto done;
    }
    if (si.si_umapaddr && (void*) si.si_umapaddr != si.si_mapping) {
        printf("dthread_shm_seginfo: bad umap info in md\n");
        errcnt++;
        goto done;
    }
    if (si.si_firstavail < si.si_mdoffset + pgsz) {
        printf("dthread_shm_seginfo: short first avail val\n");
        errcnt++;
        goto done;
    }
    printf("0-X: initial allocation %" PRId64 "\n",  /* should be zero */
           si.si_firstavail - (si.si_mdoffset + pgsz));
    if (si.si_firstavail - (si.si_mdoffset + pgsz) != 0) {
        printf("dthread_shm_seginfo: seg1 init alloc should be zero!\n");
        errcnt++;
        goto done;
    }

    printf("0-X: using rest of segment for bsd malloc\n");
    rv = dthread_shm_new_arena(1, "bsd", 0, &xar);
    if (rv) {
        printf("dthead_shm_new_arena: unexpected fail2 %s\n", strerror(rv));
        errcnt++;
        goto done;
    }
    rv = dthread_shm_seginfo(1, &nsi);
    if (rv) {
        printf("dthread_shm_seginfo: unexpected ufail %s\n", strerror(rv));
        errcnt++;
        goto done;
    }
    print_seginfo("0-Xb", &nsi);
    if (nsi.si_firstavail != nsi.si_endavail) {
        printf("dthread_shm_seginfo: did not use up segment?\n");
        errcnt++;
        goto done;
    }

    printf("0-Xa: arena <%" PRIu64 ",%" PRIu64 ",%" PRIu64 ">, r0_VA=%p\n",
           xar.dt_shmid, xar.dt_offset, xar.dt_length,
           dthread_shmref2ptr(&xar, 0));
    if (xar.dt_shmid != 1 || xar.dt_offset != si.si_firstavail ||
        xar.dt_length != si.si_endavail - si.si_firstavail) {
        printf("dthread_shm_seginfo: unexpected x arena %s\n", strerror(rv));
        errcnt++;
        goto done;
    }

    /* do 3 allocations of 481 in xar */
    for (lcv = 0 ; lcv < 3; lcv++) {
        xptr[lcv] = dthread_shm_malloc(&xar, 481, &xpref[lcv]);
        if (xptr[lcv] == NULL) {
            printf("dthread_shm_malloc: FAILED %d\n", lcv);
            errcnt++;
            goto done;
        }
        if (xpref[lcv].dt_shmid != 1 ||
            xpref[lcv].dt_offset < si.si_firstavail ||
            xpref[lcv].dt_offset >= si.si_endavail - 481) {
            printf("dthread_shm_malloc: RANGE ERROR %d\n", lcv);
            errcnt++;
            goto done;
        }
    }
    printf("allocated OK in xar\n");

    printf("\nRUNNING: freeing memory\n\n");
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
