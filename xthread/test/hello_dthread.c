#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dthread/dthread.h>

#define DTHREAD_SHM_PATH "/tmp/hello_dthread.shm"

typedef struct {
    size_t thread_id;
    size_t num_threads;
    size_t array_size;

    dthread_shmref_t array_ref;
    dthread_shmref_t thread_pids_ref;
    dthread_shmref_t thread_success_ref;
    dthread_shmref_t barrier_ref;
} thread_arg_t;

static size_t num_threads;
static size_t array_size;

static size_t application_shared_bytes(void)
{
    return num_threads * sizeof(dthread_t) +
           num_threads * sizeof(thread_arg_t) +
           array_size * sizeof(pid_t) +
           num_threads * sizeof(pid_t) +
           num_threads * sizeof(bool) +
           sizeof(dthread_barrier_t);
}

static size_t dthread_mapping_bytes(void)
{
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t max_threads = num_threads + 1;

    /*
     * Dthreads also needs space for segment metadata, its global thread
     * table, and shared-allocator metadata.  It has no public sizing helper,
     * so reserve one page for each of those runtime pieces and each thread.
     */
    return application_shared_bytes() + (max_threads + 2) * page_size;
}

static int parse_positive_size(const char *text, size_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || end[0] != '\0' || parsed == 0)
        return -1;

    *value = (size_t)parsed;
    return 0;
}

static int parse_args(int argc, char **argv)
{
    static const struct option options[] = {
        { "numthreads", required_argument, NULL, 'n' },
        { "arraysize", required_argument, NULL, 'a' },
        { NULL, 0, NULL, 0 },
    };
    bool have_num_threads = false;
    bool have_array_size = false;
    int option;

    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'n':
            if (parse_positive_size(optarg, &num_threads) != 0)
                return -1;
            have_num_threads = true;
            break;
        case 'a':
            if (parse_positive_size(optarg, &array_size) != 0)
                return -1;
            have_array_size = true;
            break;
        default:
            return -1;
        }
    }

    if (!have_num_threads || !have_array_size || optind != argc ||
        num_threads >= INT_MAX)
        return -1;

    return 0;
}

static void *thread_hello(void *opaque)
{
    thread_arg_t *arg = opaque;
    char hostname[256];
    size_t segment_size = arg->array_size / arg->num_threads;
    size_t begin = arg->thread_id * segment_size;
    size_t end = begin + segment_size;
    size_t neighbor = (arg->thread_id + 1) % arg->num_threads;
    size_t neighbor_begin = neighbor * segment_size;
    size_t neighbor_end = neighbor_begin + segment_size;
    pid_t pid = getpid();
    int rv;

    /* Each rank converts portable references to its own local addresses. */
    pid_t *array = dthread_shm_ref2ptr(
        &arg->array_ref, arg->array_size * sizeof(*array));
    pid_t *thread_pids = dthread_shm_ref2ptr(
        &arg->thread_pids_ref, arg->num_threads * sizeof(*thread_pids));
    bool *thread_success = dthread_shm_ref2ptr(
        &arg->thread_success_ref, arg->num_threads * sizeof(*thread_success));
    dthread_barrier_t *barrier = dthread_shm_ref2ptr(
        &arg->barrier_ref, sizeof(*barrier));

    if (array == NULL || thread_pids == NULL || thread_success == NULL ||
        barrier == NULL)
        return NULL;

    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");
    hostname[sizeof(hostname) - 1] = '\0';

    printf("[Dthread %zu] Hello from %s : PID %d\n",
           arg->thread_id, hostname, (int)pid);

    thread_pids[arg->thread_id] = pid;
    for (size_t i = begin; i < end; ++i)
        array[i] = pid;

    rv = dthread_barrier_wait(barrier);
    if (rv != 0 && rv != PTHREAD_BARRIER_SERIAL_THREAD) {
        thread_success[arg->thread_id] = false;
        return NULL;
    }

    thread_success[arg->thread_id] = true;
    for (size_t i = neighbor_begin; i < neighbor_end; ++i) {
        if (array[i] != thread_pids[neighbor]) {
            thread_success[arg->thread_id] = false;
            break;
        }
    }

    return NULL;
}

/* Convert pthread-style pointer arguments and returns for dthread dispatch. */
static int pointer_proc(int operation,
                        dthread_argret_t *argret,
                        void **pointer)
{
    switch (operation) {
    case DTHREAD_PROC_ENCODE:
        if (*pointer == NULL) {
            argret->dt_argret_type = DTHREAD_NODATA;
            return 0;
        }
        argret->dt_argret_type = DTHREAD_SHMREF;
        return dthread_shm_ptr2ref(*pointer, 0, &argret->u.dt_shm);

    case DTHREAD_PROC_DECODE:
        if (argret->dt_argret_type == DTHREAD_NODATA) {
            if (pointer != NULL)
                *pointer = NULL;
            return 0;
        }
        if (argret->dt_argret_type != DTHREAD_SHMREF)
            return EINVAL;
        if (pointer == NULL)
            return 0;

        *pointer = dthread_shm_ref2ptr(&argret->u.dt_shm, 0);
        return *pointer == NULL ? EINVAL : 0;

    case DTHREAD_PROC_FREE:
        return 0;

    default:
        return EINVAL;
    }
}

static void *shared_calloc(size_t count, size_t size)
{
    size_t bytes = count * size;
    void *pointer = dthread_shm_malloc(NULL, bytes, NULL);

    if (pointer != NULL)
        memset(pointer, 0, bytes);
    return pointer;
}

/* Dthreads launches this initial application thread only on rank zero. */
static int application_main(int argc, char **argv)
{
    dthread_shmref_t arena;
    dthread_t *threads;
    thread_arg_t *thread_args;
    pid_t *array;
    pid_t *thread_pids;
    bool *thread_success;
    dthread_barrier_t *barrier;
    dthread_shmref_t array_ref;
    dthread_shmref_t thread_pids_ref;
    dthread_shmref_t thread_success_ref;
    dthread_shmref_t barrier_ref;
    bool success = true;

    (void)argc;
    (void)argv;

    if (dthread_alloc_shmarena(0, "break", 0, &arena) != 0 ||
        dthread_default_shmarena(&arena) != 0) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    threads = shared_calloc(num_threads, sizeof(*threads));
    thread_args = shared_calloc(num_threads, sizeof(*thread_args));
    array = shared_calloc(array_size, sizeof(*array));
    thread_pids = shared_calloc(num_threads, sizeof(*thread_pids));
    thread_success = shared_calloc(num_threads, sizeof(*thread_success));
    barrier = shared_calloc(1, sizeof(*barrier));
    if (threads == NULL || thread_args == NULL || array == NULL ||
        thread_pids == NULL || thread_success == NULL || barrier == NULL) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    if (dthread_shm_ptr2ref(array, array_size * sizeof(*array),
                            &array_ref) != 0 ||
        dthread_shm_ptr2ref(thread_pids,
                            num_threads * sizeof(*thread_pids),
                            &thread_pids_ref) != 0 ||
        dthread_shm_ptr2ref(thread_success,
                            num_threads * sizeof(*thread_success),
                            &thread_success_ref) != 0 ||
        dthread_shm_ptr2ref(barrier, sizeof(*barrier), &barrier_ref) != 0) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    if (dthread_barrier_init(barrier, NULL,
                             (unsigned int)num_threads) != 0) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < num_threads; ++i) {
        thread_args[i] = (thread_arg_t) {
            .thread_id = i,
            .num_threads = num_threads,
            .array_size = array_size,
            .array_ref = array_ref,
            .thread_pids_ref = thread_pids_ref,
            .thread_success_ref = thread_success_ref,
            .barrier_ref = barrier_ref,
        };

        if (dthread_create(&threads[i], NULL, thread_hello,
                           &thread_args[i]) != 0) {
            puts("FAILURE");
            return EXIT_FAILURE;
        }
    }

    for (size_t i = 0; i < num_threads; ++i) {
        if (dthread_join(threads[i], NULL) != 0)
            success = false;
    }

    for (size_t i = 0; i < num_threads; ++i)
        success = success && thread_success[i];

    if (dthread_barrier_destroy(barrier) != 0)
        success = false;

    dthread_shm_free(NULL, barrier);
    dthread_shm_free(NULL, thread_success);
    dthread_shm_free(NULL, thread_pids);
    dthread_shm_free(NULL, array);
    dthread_shm_free(NULL, thread_args);
    dthread_shm_free(NULL, threads);

    puts(success ? "SUCCESS" : "FAILURE");
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static dthread_dispatch_t dispatch[] = {
    { "application_main", { .start0 = application_main }, NULL, NULL },
    { "thread_hello", { .pthstart = thread_hello }, pointer_proc, pointer_proc },
};

int main(int argc, char **argv)
{
    dthread_shmsrc_t shmsrcs[1];
    int rv;

    rv = dthread_init(&argc, &argv);
    if (rv != 0 || parse_args(argc, argv) != 0) {
        fprintf(stderr,
                "usage: %s --numthreads N --arraysize N\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (array_size % num_threads != 0) {
        fprintf(stderr, "--arraysize must be a multiple of --numthreads\n");
        return EXIT_FAILURE;
    }

    shmsrcs[0] = (dthread_shmsrc_t) {
        .dt_src = DTHREAD_SHM_PATH,
        .dt_srcflags = DTHREAD_SRC_FILE,
        .dt_mmoffset = 0,
        .dt_mmsize = dthread_mapping_bytes(),
    };

    dthread_run(dispatch,
                sizeof(dispatch) / sizeof(dispatch[0]),
                shmsrcs,
                sizeof(shmsrcs) / sizeof(shmsrcs[0]),
                NULL,
                0,
                DTHREAD_SYNCOP_ID_PSHARED,
                (int)num_threads + 1,
                argc,
                argv);

    return EXIT_FAILURE;
}
