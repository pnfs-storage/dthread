/*
 * t-exit  basic dthread exit test
 * 17-Jun-2026  chuck@ece.cmu.edu
 */

#include <err.h>
#include <errno.h>
#include <signal.h>
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

    printf("main: exit test\n");
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

/*
 * maybe exit a thread based on type.
 */
void maybe_exit(char *who, char *etype) {
    if (strcmp(etype, "exit") == 0) {
        printf("maybe_exit: %s: calling exit(0)\n", who);
        exit(0);    /* MPI will not be happen w/o MPI_Finalize() */
    }
    if (strcmp(etype, "signal") == 0) {
        printf("maybe_exit: %s: calling SIGUSR1\n", who);
        kill(getpid(), SIGUSR1);
        errx(1, "maybe_exit: not reached (signal)");
    }
    if (strcmp(etype, "ptexit") == 0) {
        printf("maybe_exit: %s: calling pthread_exit\n", who);
        pthread_exit(NULL);
        errx(1, "maybe_exit: not reached (ptexit)");
    }
    if (strcmp(etype, "dtexit") == 0) {
        dthread_argret_t ret;
        ret.dt_argret_type = DTHREAD_NODATA;
        printf("maybe_exit: %s: calling dthread_nexit\n", who);
        dthread_nexit(&ret);
        errx(1, "maybe_exit: not reached (dtexit)");
    }
    if (strcmp(etype, "return") == 0) {
        printf("maybe_exit: %s: returning for exit\n", who);
        return;
    }
    errx(1, "maybe_exit: %s: %s: unknown exit type", who, etype);
}


int app_main(int argc, char **argv) {
    int ch, rv;
    dthread_argret_t arg;
    dthread_t child;

    char *childexit = "return";
    char *mainexit = "return";
    int mainsleep = 4;

    /* can add additional flags here if desired */
    while ((ch = getopt(argc, argv, "c:m:s:t:")) != -1) {
        switch (ch) {
        case 'c':
            childexit = optarg;
            break;
        case 'm':
            mainexit = optarg;
            break;
        case 's':   /* main already handled it */
            break;
        case 't':
            mainsleep = atoi(optarg);
            if (mainsleep < 1 || mainsleep > 20) {
                printf("bad main sleep %d\n", mainsleep);
            }
            break;
        default:
            printf("usage: %s [flags]\n", *argv);
            printf("flags are:\n");
            printf("\t-c etype    child exit type\n");
            printf("\t-m etype    main exit type\n");
            printf("\t-s shmfile  shm filename\n");
            printf("\t-t secs     main sleep time before exit\n");
            printf("\nnotes: child sleep is 2sec\n");
            printf("etype values: exit, signal, ptexit, dtexit, return\n");
            printf("default etype is 'return'\n");
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    printf("app-main: creating child thread\n");
    arg.dt_argret_type = DTHREAD_INLINE;
    snprintf(arg.u.dt_inline, sizeof(arg.u.dt_inline), "%s", childexit);
    arg.dt_inlinelen = strlen(arg.u.dt_inline)+1;
    rv = dthread_ncreate(&child, NULL, rem_sleep2, &arg);
    if (rv) {
        printf("app-main: create failed (%s)\n", strerror(rv));
        return(1);
    }

    printf("app-main: sleeping.  time=%d\n", mainsleep);
    sleep(mainsleep);

    maybe_exit("app-main", mainexit);

    return(0);
}

dthread_argret_t rem_sleep2(dthread_argret_t *dt_arg) {
    dthread_argret_t ret;

    printf("rem_sleep2: running\n");
    if (dt_arg->dt_argret_type != DTHREAD_INLINE) {
        printf("rem_sleep2: ERROR - bad arg type\n");
        exit(1);
    }
    printf("rem_sleep2: sleeping\n");
    sleep(2);
    maybe_exit("child", dt_arg->u.dt_inline);

    ret.dt_argret_type = DTHREAD_NODATA;
    return(ret);
}
