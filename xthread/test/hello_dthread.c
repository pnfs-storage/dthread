#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <dthread/dthread.h>

#define NUM_THREADS 4

typedef struct {
    int tid;
    dthread_shmref_t shared_array_ref;
    dthread_shmref_t barrier_ref;
} thread_arg_t;

static dthread_argret_t thread_hello(dthread_argret_t *dt_arg)
{
    thread_arg_t arg;
    dthread_argret_t ret;

    memset(&ret, 0, sizeof(ret));
    ret.dt_argret_type = DTHREAD_NODATA;

    memcpy(&arg, dt_arg->u.dt_inline, sizeof(arg));

    int tid = arg.tid;

    int *shared_array =
        dthread_shm_ref2ptr(&arg.shared_array_ref,
                            sizeof(int) * NUM_THREADS);

    dthread_barrier_t *barrier =
        dthread_shm_ref2ptr(&arg.barrier_ref,
                            sizeof(dthread_barrier_t));

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    printf("[Dthread %d] Hello from %s : PID %d\n",
           tid, hostname, getpid());

    shared_array[tid] = tid;

    dthread_barrier_wait(barrier);

    int neighbor_idx = (tid + 1) % NUM_THREADS;
    int neighbor_val = shared_array[neighbor_idx];

    printf("[Dthread %d] Read neighbor index %d, value is %d\n",
           tid, neighbor_idx, neighbor_val);

    return ret;
}

static int application_main(void)
{
    int rv;
    dthread_shmref_t arena;
    dthread_shmref_t shared_array_ref;
    dthread_shmref_t barrier_ref;

    rv = dthread_alloc_shmarena(0, "break", 0, &arena);
    if (rv != 0) {
        fprintf(stderr, "dthread_alloc_shmarena failed: %s (%d)\n",
                strerror(rv), rv);
        return 1;
    }

    rv = dthread_default_shmarena(&arena);
    if (rv != 0) {
        fprintf(stderr, "dthread_default_shmarena failed: %s (%d)\n",
                strerror(rv), rv);
        return 1;
    }

    int *shared_array =
        dthread_shm_malloc(NULL,
                           sizeof(int) * NUM_THREADS,
                           &shared_array_ref);

    if (shared_array == NULL) {
        fprintf(stderr, "dthread_shm_malloc shared_array failed\n");
        return 1;
    }

    for (int i = 0; i < NUM_THREADS; i++)
        shared_array[i] = -1;

    dthread_barrier_t *barrier =
        dthread_shm_malloc(NULL,
                           sizeof(dthread_barrier_t),
                           &barrier_ref);

    if (barrier == NULL) {
        fprintf(stderr, "dthread_shm_malloc barrier failed\n");
        return 1;
    }

    rv = dthread_barrier_init(barrier, NULL, NUM_THREADS);
    if (rv != 0) {
        fprintf(stderr, "dthread_barrier_init failed: %s (%d)\n",
                strerror(rv), rv);
        return 1;
    }

    dthread_t threads[NUM_THREADS];
    dthread_argret_t args[NUM_THREADS];

    for (int i = 1; i < NUM_THREADS; i++) {
        thread_arg_t arg;

        arg.tid = i;
        arg.shared_array_ref = shared_array_ref;
        arg.barrier_ref = barrier_ref;

        memset(&args[i], 0, sizeof(args[i]));
        args[i].dt_argret_type = DTHREAD_INLINE;
        args[i].dt_inlinelen = sizeof(arg);
        memcpy(args[i].u.dt_inline, &arg, sizeof(arg));

        rv = dthread_ncreate(&threads[i],
                             NULL,
                             thread_hello,
                             &args[i]);

        if (rv != 0) {
            fprintf(stderr, "dthread_ncreate(%d) failed: %s (%d)\n",
                    i, strerror(rv), rv);
            return 1;
        }
    }

    thread_arg_t arg0;

    arg0.tid = 0;
    arg0.shared_array_ref = shared_array_ref;
    arg0.barrier_ref = barrier_ref;

    dthread_argret_t argret0;
    memset(&argret0, 0, sizeof(argret0));
    argret0.dt_argret_type = DTHREAD_INLINE;
    argret0.dt_inlinelen = sizeof(arg0);
    memcpy(argret0.u.dt_inline, &arg0, sizeof(arg0));

    thread_hello(&argret0);

    for (int i = 1; i < NUM_THREADS; i++) {
        dthread_argret_t ret;

        rv = dthread_njoin(threads[i], &ret);
        if (rv != 0) {
            fprintf(stderr, "dthread_njoin(%d) failed: %s (%d)\n",
                    i, strerror(rv), rv);
            return 1;
        }
    }

    dthread_barrier_destroy(barrier);

    dthread_shm_free(NULL, barrier);
    dthread_shm_free(NULL, shared_array);

    printf("Dthread test execution complete.\n");

    return 0;
}

int app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return application_main();
}

dthread_dispatch_t disptable[] = {
    { "app_main",     { .start0 = app_main },     NULL, NULL },
    { "thread_hello", { .start = thread_hello }, NULL, NULL },
};

dthread_shmsrc_t shmsrctable[] = {
    { "/tmp/dt.shm", DTHREAD_SRC_FILE, 0, 4 * 1024 * 1024 },
};

int main(int argc, char **argv)
{
    int rv;

    printf(">>> Starting Dthread Baseline <<<\n");

    rv = dthread_init(&argc, &argv);
    if (rv != 0) {
        fprintf(stderr, "dthread_init failed: %s (%d)\n",
                strerror(rv), rv);
        return 1;
    }

    dthread_run(disptable,
                sizeof(disptable) / sizeof(disptable[0]),
                shmsrctable,
                sizeof(shmsrctable) / sizeof(shmsrctable[0]),
                NULL,
                0,
                DTHREAD_SYNCOP_ID_PSHARED,
                NUM_THREADS,
                argc,
                argv);

    fprintf(stderr, "dthread_run unexpectedly returned\n");
    return 1;
}
