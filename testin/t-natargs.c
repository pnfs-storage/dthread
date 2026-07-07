/*
 * t-natargs  basic dthread native args test
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
dthread_argret_t rem_double(dthread_argret_t *dt_arg);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
    { "double", { .start = rem_double }, NULL, NULL },
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

#define MSIZE 8

int app_main(int argc, char **argv) {
    int lcv, errcnt, rv;
    int rvs[MSIZE];
    dthread_t dts[MSIZE];
    dthread_argret_t ret, arg;

    printf("app_main: running\n");
    errcnt = 0;

    arg.dt_argret_type = DTHREAD_INLINE;
    arg.dt_inlinelen = 1;

    /* create some threads */
    for (lcv = 0 ; lcv < MSIZE ; lcv++) {
       arg.u.dt_inline[0] = lcv;
       rvs[lcv] = dthread_ncreate(&dts[lcv], NULL, rem_double, &arg);
        if (rvs[lcv]) {
            printf("app_main: ncreate %d failed! (%s)\n",
                   lcv, strerror(rvs[lcv]));
            errcnt++;
        }
    }

    /* collect the threads */
    for (lcv = 0 ; lcv < MSIZE ; lcv++) {
        ret.dt_argret_type = DTHREAD_NODATA;
        if (rvs[lcv]) continue;   /* failed to launch? */
        rv = dthread_njoin(dts[lcv], &ret);
        if (rv) {
            printf("app_main: join %d failed! (%s)\n", lcv, strerror(rv));
            errcnt++;
        }
        if (ret.dt_argret_type != DTHREAD_INLINE ||
            ret.dt_inlinelen != 1) {
            printf("app_main: bad return arg (%d)\n", lcv);
            errcnt++;
         } else if (ret.u.dt_inline[0] != lcv*2) {
            printf("app_main: ret no doubled (%d, %d)\n",
                   lcv, ret.u.dt_inline[0]);
            errcnt++;
         }  else {
            printf("app_main: ret AOK (%d)\n", lcv);
         }
    }

    printf("app_main: DONE, errcnt=%d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

dthread_argret_t rem_double(dthread_argret_t *dt_arg) {
    dthread_argret_t ret;
    ret.dt_argret_type = DTHREAD_NODATA;

    printf("rem_double: running\n");

    if (dt_arg->dt_argret_type != DTHREAD_INLINE &&
        dt_arg->dt_inlinelen != 1) {
        printf("rem_double: bad arg!\n");
        return(ret);
    }

    ret.dt_argret_type = DTHREAD_INLINE;
    ret.dt_inlinelen = 1;
    ret.u.dt_inline[0] = dt_arg->u.dt_inline[0] * 2;
    printf("rem_double: done (%d)\n", dt_arg->u.dt_inline[0]);
    return(ret);
}
