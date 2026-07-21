/*
 * t-cancel  basic dthread cancel test
 * 19-Jun-2026  chuck@ece.cmu.edu
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
dthread_argret_t rem_loop(dthread_argret_t *dt_arg);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
    { "loop", { .start = rem_loop }, NULL, NULL },
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

    /* redirect stdout to stderr for testwrap script parser */
    setbuf(stdout, 0);
    dup2(STDERR_FILENO, STDOUT_FILENO);

    printf("main: cancel test\n");
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
    int ch, errcnt, rv;
    dthread_t me, notme, child;
    dthread_argret_t arg, ret;

    /* can add additional flags here if desired */
    while ((ch = getopt(argc, argv, "s:")) != -1) {
        switch (ch) {
        case 's':   /* main already handled it */
            break;
        default:
            printf("usage: %s [-s shmfile]\n", *argv);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    errcnt = 0;
    me = dthread_self();
    printf("app_main: thread=<%d,%d>\n", me.dt_index, me.dt_seq);

    printf("app_main: checking bad cancel request\n");
    notme = me;
    notme.dt_seq++;

    rv = dthread_cancel(notme);

    if (rv == ESRCH) {
        printf("app_main: OK - bad cancel returned correct error\n");
    } else {
        printf("app_main: ERR - bad cancel returned = %s\n", strerror(rv));
        errcnt++;
    }

    printf("app_main: testing cancel child thread\n");

    arg.dt_argret_type = DTHREAD_NODATA;
    rv = dthread_ncreate(&child, NULL, rem_loop, &arg);
    if (rv != 0) {
        printf("app_main: ncreate failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: created!  sleep 5\n");
    }

    sleep(5);

    printf("app_main: cancel child\n");
    rv = dthread_cancel(child);
    if (rv != 0) {
        printf("app_main: cancel failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: child canceled.  going to join!\n");
    }

    memset(&ret, 0, sizeof(ret));
    rv = dthread_njoin(child, &ret);
    if (rv != 0) {
        printf("app_main: join failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: canceled child joined!\n");
        if (ret.dt_argret_type == DTHREAD_CANCELED) {
            printf("app_main: canceled child returned canceled token\n");
        } else {
            printf("app_main: canceled child returned WRONG token (%d)\n",
                   ret.dt_argret_type);
            errcnt++;
        }
    }

    printf("app_main: DONE, errcnt=%d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

dthread_argret_t rem_loop(dthread_argret_t *dt_arg) {
    dthread_argret_t ret;
    ret.dt_argret_type = DTHREAD_NODATA;
    printf("rem_loop: running\n");
    while (1) {
        printf("rem_loop: tick...\n");
        sleep(2);
    }
    printf("rem_loop: done\n");
    return(ret);
}
