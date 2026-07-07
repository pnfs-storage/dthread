/*
 * t-pthargs  basic dthread pthread-style args test
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
void *rem_double(void *arg);

int rem_double_procarg(int op, dthread_argret_t *arg, void **pth_arg);
int rem_double_procret(int op, dthread_argret_t *arg, void **pth_arg);

dthread_dispatch_t disptable[] = {
    { "app_main", { .start0 = app_main }, NULL, NULL },
    { "double", { .pthstart = rem_double },
                   rem_double_procarg, rem_double_procret },
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
    void *ret;
    int *iret;

    printf("app_main: running\n");
    errcnt = 0;

    /* create some threads */
    for (lcv = 0 ; lcv < MSIZE ; lcv++) {
       rvs[lcv] = dthread_create(&dts[lcv], NULL, rem_double, &lcv);
        if (rvs[lcv]) {
            printf("app_main: create %d failed! (%s)\n",
                   lcv, strerror(rvs[lcv]));
            errcnt++;
        }
    }

    /* collect the threads */
    for (lcv = 0 ; lcv < MSIZE ; lcv++) {
        if (rvs[lcv]) continue;   /* failed to launch? */
        rv = dthread_join(dts[lcv], &ret);
        if (rv) {
            printf("app_main: join %d failed! (%s)\n", lcv, strerror(rv));
            errcnt++;
        }
        iret = ret;
        if (iret == NULL) {
            printf("app_main: bad return arg (%d)\n", lcv);
            errcnt++;
         } else if (*iret != lcv*2) {
            printf("app_main: ret no doubled (%d, %d)\n", lcv, *iret);
            errcnt++;
         }  else {
            printf("app_main: ret AOK (%d)\n", lcv);
         }
         if (iret) free(iret);
    }

    printf("app_main: DONE, errcnt=%d\n", errcnt);
    return((errcnt) ? 1 : 0);
}

void *rem_double(void *arg) {
    int *iarg = arg;
    int result;
    /*
     * note: if we return to the caller, we cannot return a pointer
     * that points to a stack value because that part of the stack
     * will be unwound and not valid in the caller's context.
     *
     * we can return a pointer to malloc()'d data on the heap or
     * a pointer to data in the text/data segments.  if we return
     * a pointer to malloc()'d data, someone needs to free it
     * (could be done in return proc function when it encodes
     * the result).
     *
     * on the other hand, it is OK to call dthread_exit() with
     * a return pointer pointing to a stack value since the stack
     * will be valid in the dthread_exit() call (note that the
     * thread will terminate by calling pthread_exit() and going
     * through the cleanup function rather than returning to caller).
     *
     * the bottom line here is that we need to be in sync with
     * our proc functions in order to manage pthread-style args
     * cleanly.
     */

    printf("rem_double: running\n");

    if (iarg == NULL) {
        printf("rem_double: bad arg!\n");
        return(NULL);
    }
    result = *iarg * 2;

    printf("rem_double: done (%d)\n", result);

    dthread_exit(&result);  /* ok to ret stack value w/dthread_exit */

    printf("rem_double: dthread exit didn't exit!!!\n");
    return(NULL);
}

/*
 * double args:
 *
 *   encode: pth_arg is a pointer to an int on the stack.  we can
 *           make it an inline value in arg.
 *
 *   decode: we have the int (or NODATA) inline in arg.
 *           we want to set pth_arg to point to the inline buffer
 *           if it has a value, otherwise set to NULL.
 *
 *   free: we are all inline, so no op required
 */
int rem_double_procarg(int op, dthread_argret_t *arg, void **pth_arg) {

    /*
     * called in thread starting new pthread-style dthread to
     * encode the pthread "void *" arg into a dthread_argret_t to
     * send to <mgr,0> as part of the CREATE msg.  <mgr,0> will
     * stash the dthread_argret_t into the gtab[] entry it allocates
     * for the new thread.
     */
    if (op == DTHREAD_PROC_ENCODE) {         /* encode arg inline */
        int *i = (pth_arg) ? *pth_arg : NULL; 
        if (i) {
            arg->dt_argret_type = DTHREAD_INLINE;
            arg->dt_inlinelen = sizeof(*i);
            memcpy(arg->u.dt_inline, i, sizeof(*i));
            printf("rem_double_procarg: inline encode %d\n", *i);
        } else {
            arg->dt_argret_type = DTHREAD_NODATA;
            printf("rem_double_procarg: inline encode NULL\n");
        }
        return(0);
    }

    /*
     * called on the target rank when it is starting the new thread.
     * we need to decode the arg from gtab[] into a void * pointer to
     * pass to the new proc.  for this we can return a pointer to
     * inline data.
     */
    if (op == DTHREAD_PROC_DECODE) {
        int val;
        if (arg->dt_argret_type == DTHREAD_NODATA) {
            if (pth_arg)
                *pth_arg = NULL;
            printf("rem_double_procarg: decode NULL\n");
            return(0);
        }
        if (arg->dt_argret_type != DTHREAD_INLINE || 
            arg->dt_inlinelen != sizeof(val)) {
            if (pth_arg)
                *pth_arg = NULL;
            printf("rem_double_procarg: decode bad value to NULL\n");
            return(0);
        }
        memcpy(&val, arg->u.dt_inline, sizeof(val));
        if (pth_arg)
            *pth_arg = arg->u.dt_inline;
        printf("rem_double_procarg: decoded %d\n", val);
        return(0);
    }

    printf("rem_double_procarg: free noop called\n");
    return(0);
}

/*
 * double return:
 *
 *   encode: pth_arg is a pointer to an int.  we copy it to
 *           arg (which is the gtab[] return dthread_argret_t)
 *           as an inline for the other side to decode.
 *
 *   decode: the dthread_argret_t return value is placed in
 *           a dthread's zombie gtab[] entry until it is joined.
 *           when a JOIN message arrives, <mgr,0> copies the
 *           dthread_argret_t return from the zombie gtab[] entry
 *           to a JOINED message (and will free the zombie gtab[]).
 *           the pthread-style dthread_join() gets a copy of
 *           the dthread_argret_t return on a stack-allocated
 *           dthread_argret_t before the local mgr frees the JOINED msg.
 *           the decode op must copy out the data from the
 *           stack-allocated dthread_argret_t to a void*.
 *           we cannot return a pointer to inline data in the
 *           source dthread_argret_t because it is stack allocated
 *           and will not be valid after the dthread_join() stack unwinds.
 *           maybe the best option is to malloc something to return
 *           and ether have the caller free it (or have thread return
 *           only happen around shutdown, so we do not need to free it).
 *
 *   free: do not need it, as we are inline
 */
int rem_double_procret(int op, dthread_argret_t *arg, void **pth_arg) {
    /*
     * called by a terminating pthread-style dthread to encode
     * the void* return into the gtab[] dthread_argret_t for
     * later use by a join operation by some other thread.
     */
    if (op == DTHREAD_PROC_ENCODE) {         /* encode ret inline */
        int *i = (pth_arg) ? *pth_arg : NULL; 
        if (i) {
            arg->dt_argret_type = DTHREAD_INLINE;
            arg->dt_inlinelen = sizeof(*i);
            memcpy(arg->u.dt_inline, i, sizeof(*i));
            printf("rem_double_procret: inline encode %d\n", *i);
        } else {
            arg->dt_argret_type = DTHREAD_NODATA;
            printf("rem_double_procret: inline encode NULL\n");
        }
        return(0);
    }

    /*
     * called by a dthread doing a dthread_join() on pthread-style
     * thread that has exited so it can collect the return info.
     */
    if (op == DTHREAD_PROC_DECODE) {
        int *i;

        if (arg->dt_argret_type == DTHREAD_NODATA) {
            if (pth_arg)
                *pth_arg = NULL;
            printf("rem_double_procret: decode NULL\n");
            return(0);
        }
        if (arg->dt_argret_type != DTHREAD_INLINE || 
            arg->dt_inlinelen != sizeof(*i)) {
            if (pth_arg)
                *pth_arg = NULL;
            printf("rem_double_procret: decode bad value to NULL\n");
            return(0);
        }

        i = malloc(sizeof(i));  /* XXX: we cannot free it */
        if (i == NULL) {
            printf("rem_double_procret: decode malloc failed!\n");
            if (pth_arg) *pth_arg = NULL;
            return(0);
        }
        memcpy(i, arg->u.dt_inline, sizeof(*i));
        if (pth_arg)
            *pth_arg = i;
        printf("rem_double_procarg: decoded %d\n", *i);
        return(0);
    }

    printf("rem_double_procret: free noop called\n");
    return(0);
}
