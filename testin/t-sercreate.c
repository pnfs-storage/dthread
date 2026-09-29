/*
 * t-sercreates  serially create/join some threads and then exit
 * 29-Sep-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <dthread/dthread.h>

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
    int errcnt, lcv, rv;
    dthread_argret_t arg, ret;
    dthread_t child;

    errcnt = 0;
    printf("app_main: running\n");

    printf("app_main: creating children and joining them...\n");
    for (lcv = 0 ; lcv < 16 ; lcv++) {
        arg.dt_argret_type = DTHREAD_NODATA;
        rv = dthread_ncreate(&child, NULL, rem_proc, &arg);
        if (rv != 0) {
            printf("app_main: ncreate failed! (%s)\n", strerror(rv));
            errcnt++;
            continue;
        } else {
            printf("app_main: created %d!\n", lcv);
        }
        printf("app_main: join %d\n", lcv);
        rv = dthread_njoin(child, &ret);
        if (rv) {
            printf("app_main: join error: %d: %s", lcv, strerror(rv));
            errcnt++;
        } else {
            printf("app_main: join success!  %d\n", lcv);
        }
    }

    printf("app_main: return %d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

dthread_argret_t rem_proc(dthread_argret_t *dt_arg) {
    dthread_argret_t ret;

    printf("rem_proc: start\n");
    ret.dt_argret_type = DTHREAD_NODATA;
    printf("rem_proc: done\n");

    return(ret);
}
