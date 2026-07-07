/*
 * t-join  basic dthread join test
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
dthread_argret_t rem_sleep2(dthread_argret_t *dt_arg);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
    { "sleep2", { .start = rem_sleep2 }, NULL, NULL },
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

    printf("main: join test\n");
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

#define MSIZE 4

int app_main(int argc, char **argv) {
    int ch, rv, errcnt, lcv, tmrv[MSIZE];
    dthread_t me, tst, tm[MSIZE];
    dthread_argret_t ret, arg;

    /* can add additional flags here if desired */
    while ((ch = getopt(argc, argv, "s:")) != -1) {
        switch (ch) {
        case 's':   /* main already handled it */
            break;
        default:
            printf("usage: %s [-b] [-s shmfile]\n", *argv);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    errcnt = 0;
    me = dthread_self();
    printf("app_main: thread=<%d,%d>\n", me.dt_index, me.dt_seq);

    printf("app_main: checking bad join requests\n");

    tst = me;
    rv = dthread_njoin(tst, &ret);
    if (rv != EDEADLK) {
        printf("app_main: self join != EDEADLK (%s)\n", strerror(rv));
        errcnt++;
    }

    tst.dt_seq++;     /* invalidate seq */
    rv = dthread_njoin(tst, &ret);
    if (rv != ESRCH) {
        printf("app_main: bad seq join != ESRCH (%s)\n", strerror(rv));
        errcnt++;
    }

    tst.dt_index = 0xdeadcafe;
    tst.dt_seq = 0;
    if (rv != ESRCH) {
        printf("app_main: bad seq join != ESRCH (%s)\n", strerror(rv));
        errcnt++;
    }

    printf("app_main: DONE checking bad join requests\n");

    printf("app_main: testing join of completed thread\n");

    arg.dt_argret_type = DTHREAD_NODATA;
    rv = dthread_ncreate(&tst, NULL, rem_sleep2, &arg);
    if (rv != 0) {
        printf("app_main: ncreate failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: created!  sleep 4 to wait for thread to stop\n");
        sleep(4);
        printf("app_main: joining thread\n");
        rv = dthread_njoin(tst, &ret);
        if (rv == 0 && ret.dt_argret_type == DTHREAD_NODATA) {
            printf("app_main: join completed thread: success!\n");
        } else {
            printf("app_main: join completed thread: failed! (%d,%d)\n",
                   rv, ret.dt_argret_type);
            errcnt++;
        }
    }

    printf("app_main: testing blocking join of running thread\n");

    arg.dt_argret_type = DTHREAD_NODATA;
    rv = dthread_ncreate(&tst, NULL, rem_sleep2, &arg);
    if (rv != 0) {
        printf("app_main: ncreate failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: created!  waiting for thread to stop\n");
        printf("app_main: joining thread\n");
        rv = dthread_njoin(tst, &ret);
        if (rv == 0 && ret.dt_argret_type == DTHREAD_NODATA) {
            printf("app_main: join completed thread: success!\n");
        } else {
            printf("app_main: join completed thread: failed! (%d,%d)\n",
                   rv, ret.dt_argret_type);
            errcnt++;
        }
    }

    printf("app_main: testing multicreate %d threads\n", MSIZE);
    for (lcv = 0 ; lcv < MSIZE ; lcv++) {
        tmrv[lcv] = dthread_ncreate(&tm[lcv], NULL, rem_sleep2, &arg);
        if (tmrv[lcv]) {
            printf("app_main: ncreate %d failed! (%s)\n",
                   lcv, strerror(tmrv[lcv]));
            errcnt++;
        }
    }
    printf("app_main: multicreate join\n");
    for (lcv = 0 ; lcv < MSIZE ; lcv++) {
        if (tmrv[lcv]) continue;   /* create failed? */
        rv = dthread_njoin(tm[lcv], &ret);
        if (rv == 0 && ret.dt_argret_type == DTHREAD_NODATA) {
            printf("app_main: multijoin completed thread %d: success!\n", lcv);
        } else {
            printf("app_main: multijoin completed thread %d: failed! (%d,%d)\n",
                   lcv, rv, ret.dt_argret_type);
            errcnt++;
        }
    }
    printf("app_main: DONE testing multicreate\n");

    printf("app_main: DONE, errcnt=%d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

dthread_argret_t rem_sleep2(dthread_argret_t *dt_arg) {
    dthread_argret_t ret;
    ret.dt_argret_type = DTHREAD_NODATA;
    printf("rem_sleep2: running\n");
    sleep(2);
    printf("rem_sleep2: done\n");
    return(ret);
}
