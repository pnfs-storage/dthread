/*
 * t-startup  basic dthread startup test
 * 17-Jun-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
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
    int lcv, oldcan;

    printf("app_main: running\n");
    printf("app_main: argc = %d\n", argc);
    for (lcv = 0 ; lcv < argc ; lcv++) {
        printf("app_main: argv[%d] = %s\n", lcv, argv[lcv]);
    }

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldcan);
    pthread_setcancelstate(oldcan, NULL);
    if (oldcan == PTHREAD_CANCEL_ENABLE) {
        printf("app_main: cancel enabled, as expected!\n");
    } else {
        printf("app_main: cancel not enabled (%d)!   error out!\n", oldcan);
        return(1);
    }

    printf("app_main: return 0\n");
    return(0);
}
