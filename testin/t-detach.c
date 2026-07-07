/*
 * t-detach  basic dthread detach test
 * 17-Jun-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dt_internal.h"   /* internal interface for bootstrap alloc */

/*
 * available thread startups and dispatch table.
 */
int app_main(int argc, char **argv);
dthread_argret_t rem_sleep11(dthread_argret_t *dt_arg);
dthread_argret_t rem_sleep2(dthread_argret_t *dt_arg);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
    { "sleep11", { .start = rem_sleep11 }, NULL, NULL },
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

    printf("main: detach test\n");
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
    int ch, rv, errcnt;
    dthread_t me, notme, child;
    dthread_argret_t arg, *xret, ret;
    dthread_shmref_t xref;

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

    /* XXX: begin internal API use (need shm malloc) */
    printf("app_main: use bootstrap allocator to allocate xret\n");
    xret = dthread_shm_segalloc(0, sizeof(*xret), &xref);
    if (xret == NULL) {
        printf("app_main: bootstrap allocator failed!\n");
        return(1);
    }
    /* XXX: end internal API use */

    printf("app_main: xref=%p, <%" PRId64 ",%" PRId64 ",%" PRId64 ">\n",
           xret, xref.dt_shmid, xref.dt_offset, xref.dt_length);

    printf("app_main: checking bad detach request\n");
    notme = me;
    notme.dt_seq++;
    rv = dthread_detach(notme);
    if (rv == ESRCH) {
        printf("app_main: OK - bad detach returned correct error\n");
    } else {
        printf("app_main: ERR - bad detach returned = %s\n", strerror(rv));
        errcnt++;
    }

    printf("app_main: testing self detach child thread\n");

    arg.dt_argret_type = DTHREAD_SHMREF;
    arg.u.dt_shm = xref;
    rv = dthread_ncreate(&child, NULL, rem_sleep11, &arg);
    if (rv != 0) {
        printf("app_main: ncreate failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: created!  sleep 4 to wait for thread to stop\n");
    }
    
    sleep(4);

    if (xret->dt_argret_type != DTHREAD_INLINE || xret->u.dt_inline[0] != 0) {
        printf("app_main: child reports errror!\n");
        errcnt++;
    }

    printf("app_main: testing parent detach child thread\n");
    arg.dt_argret_type = DTHREAD_NODATA;
    rv = dthread_ncreate(&child, NULL, rem_sleep2, &arg);
    if (rv != 0) {
        printf("app_main: ncreate failed! (%s)\n", strerror(rv));
        errcnt++;
    } else {
        printf("app_main: created!  detaching thread.\n");
        rv = dthread_detach(child);
        if (rv != 0) {
            printf("app_main: detach failed! (%s)\n", strerror(rv));
            errcnt++;
        } else {
            rv = dthread_njoin(child, &ret);
            if (rv == EINVAL) {
                printf("app_main: join after detach ret correct err\n");
            } else {
                printf("app_main: join failed! (%s)\n", strerror(rv));
                errcnt++;
            } 
        }
    }

    sleep(4);

    printf("app_main: DONE, errcnt=%d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

dthread_argret_t rem_sleep11(dthread_argret_t *dt_arg) {
    dthread_argret_t ret, *xret;
    dthread_t me;
    int rv;

    ret.dt_argret_type = DTHREAD_INLINE;
    ret.dt_inlinelen = 1;
    ret.u.dt_inline[0] = 0;

    me = dthread_self();
    printf("rem_sleep11: running <%d,%d>\n", me.dt_index, me.dt_seq);

    if (dt_arg->dt_argret_type != DTHREAD_SHMREF)
        errx(1, "rem_sleep11: bad arg!  should not happen\n");
    xret = dthread_shm_ref2ptr(&dt_arg->u.dt_shm, sizeof(dthread_argret_t));
    if (xret == NULL)
        errx(1, "rem_sleep11: bad arg mapping!  should not happen\n");

    sleep(1);

    rv = dthread_detach(me);
    if (rv == 0) {
        printf("rem_sleep11: successful detach!\n");
    } else {
        printf("rem_sleep11: detach failed! (%s)\n", strerror(rv));
        ret.u.dt_inline[0] = 1;
    }

    sleep(1);

    rv = dthread_detach(me);
    if (rv == 0) {
        printf("rem_sleep11: successful detach 2!\n");
    } else {
        printf("rem_sleep11: detach 2 failed! (%s)\n", strerror(rv));
        ret.u.dt_inline[0] = 1;
    }

    /*
     * but since we are detached, ret is thrown away.
     * work around by copying ret to xret in shared memory.
     */
    *xret = ret;
    return(ret);
}

dthread_argret_t rem_sleep2(dthread_argret_t *dt_arg) {
    dthread_argret_t ret;
    ret.dt_argret_type = DTHREAD_NODATA;
    printf("rem_sleep2: running\n");
    sleep(2);
    printf("rem_sleep2: done\n");
    return(ret);
}
