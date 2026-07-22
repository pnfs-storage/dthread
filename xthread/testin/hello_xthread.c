#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dthread/xthread.h>

#define XTHREAD_SHM_PATH "/tmp/hello_xthread.shm"

typedef struct {
    size_t thread_id;
    size_t num_threads;
    size_t array_size;

    xthread_ref_t array_ref;
    xthread_ref_t thread_pids_ref;
    xthread_ref_t thread_success_ref;
    xthread_ref_t barrier_ref;
} thread_arg_t;

typedef struct {
    size_t num_threads;
    size_t array_size;
} application_options_t;

static size_t application_shared_bytes(const application_options_t *options)
{

    size_t shared_bytes =
    options->num_threads * sizeof(xthread_t) +
    options->num_threads * sizeof(thread_arg_t) +
    options->array_size * sizeof(pid_t) +
    options->num_threads * sizeof(pid_t) +
    options->num_threads * sizeof(bool) +
    sizeof(xthread_barrier_t);
    return shared_bytes;
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

static int parse_args(int argc, char **argv, application_options_t *app_options)
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
            if (parse_positive_size(optarg, &app_options->num_threads) != 0)
                return -1;
            have_num_threads = true;
            break;
        case 'a':
            if (parse_positive_size(optarg, &app_options->array_size) != 0)
                return -1;
            have_array_size = true;
            break;
        default:
            return -1;
        }
    }

    if (!have_num_threads || !have_array_size || optind != argc ||
        app_options->num_threads > UINT_MAX)
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
    pid_t *array = xthread_ref_to_ptr(
        &arg->array_ref, arg->array_size * sizeof(*array));
    pid_t *thread_pids = xthread_ref_to_ptr(
        &arg->thread_pids_ref, arg->num_threads * sizeof(*thread_pids));
    bool *thread_success = xthread_ref_to_ptr(
        &arg->thread_success_ref, arg->num_threads * sizeof(*thread_success));
    xthread_barrier_t *barrier = xthread_ref_to_ptr(
        &arg->barrier_ref, sizeof(*barrier));

    if (array == NULL || thread_pids == NULL || thread_success == NULL ||
        barrier == NULL)
        return NULL;

    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");
    hostname[sizeof(hostname) - 1] = '\0';

    printf("[Xthread %zu] Hello from %s : PID %d\n",
           arg->thread_id, hostname, (int)pid);

    thread_pids[arg->thread_id] = pid;
    for (size_t i = begin; i < end; ++i)
        array[i] = pid;

    rv = xthread_barrier_wait(barrier);
    if (rv != 0) {
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

/*
 * In pthread mode xthread_run() calls this directly.  In dthread mode it
 * launches this as the initial application thread on rank 0.
 */
static int application_main(void *opaque, int argc, char **argv)
{
    application_options_t *options = opaque;
    size_t num_threads = options->num_threads;
    size_t array_size = options->array_size;
    xthread_t *threads;
    thread_arg_t *thread_args;
    pid_t *array;
    pid_t *thread_pids;
    bool *thread_success;
    xthread_barrier_t *barrier;
    xthread_ref_t array_ref;
    xthread_ref_t thread_pids_ref;
    xthread_ref_t thread_success_ref;
    xthread_ref_t barrier_ref;
    bool success = true;

    (void)argc;
    (void)argv;

    threads 		= xthread_calloc(num_threads, sizeof(*threads));
    thread_args 	= xthread_calloc(num_threads, sizeof(*thread_args));
    array 		= xthread_calloc(array_size, sizeof(*array));
    thread_pids 	= xthread_calloc(num_threads, sizeof(*thread_pids));
    thread_success 	= xthread_calloc(num_threads, sizeof(*thread_success));
    barrier 		= xthread_calloc(1, sizeof(*barrier));
    if (threads == NULL || thread_args == NULL || array == NULL ||
        thread_pids == NULL || thread_success == NULL || barrier == NULL) 
    {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    /* Convert local addresses explicitly before placing them in arguments. */
    if (xthread_ptr_to_ref(array, array_size * sizeof(*array),
                           &array_ref) != 0 ||
        xthread_ptr_to_ref(thread_pids,
                           num_threads * sizeof(*thread_pids),
                           &thread_pids_ref) != 0 ||
        xthread_ptr_to_ref(thread_success,
                           num_threads * sizeof(*thread_success),
                           &thread_success_ref) != 0 ||
        xthread_ptr_to_ref(barrier, sizeof(*barrier), &barrier_ref) != 0) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    if (xthread_barrier_init(barrier, NULL,
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

        if (xthread_create(&threads[i], NULL, thread_hello,
                           &thread_args[i]) != 0) {
            puts("FAILURE");
            return EXIT_FAILURE;
        }
    }

    for (size_t i = 0; i < num_threads; ++i) {
        if (xthread_join(threads[i], NULL) != 0)
            success = false;
    }

    for (size_t i = 0; i < num_threads; ++i)
        success = success && thread_success[i];

    if (xthread_barrier_destroy(barrier) != 0)
        success = false;

    xthread_free(barrier);
    xthread_free(thread_success);
    xthread_free(thread_pids);
    xthread_free(array);
    xthread_free(thread_args);
    xthread_free(threads);

    puts(success ? "SUCCESS" : "FAILURE");
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    static const xthread_entry_t entries[] = {
        { "thread_hello", thread_hello },
    };
    xthread_shmsrc_t shmsrcs[1];
    xthread_config_t config;
    application_options_t options = { 0 };
    size_t required_memory_size;
    int rv;

    rv = xthread_init(&argc, &argv);
    if (rv != 0 || parse_args(argc, argv, &options) != 0) {
        fprintf(stderr, "usage: %s [--dthread] --numthreads N --arraysize N\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (options.array_size % options.num_threads != 0) {
        fprintf(stderr, "--arraysize must be a multiple of --numthreads\n");
        return EXIT_FAILURE;
    }

    /*
     * dthread programs allocate from a shared memory region so we must pre-specify the 
     * total potential amount of required memory 
     */
    required_memory_size = application_shared_bytes(&options);
    if (xthread_shmsrc_file(&shmsrcs[0], XTHREAD_SHM_PATH, required_memory_size) != 0) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    config = (xthread_config_t) {
        .entries = entries,
        .entry_count = sizeof(entries) / sizeof(entries[0]),
        .shmsrcs = shmsrcs,
        .shmsrc_count = sizeof(shmsrcs) / sizeof(shmsrcs[0]),
        .max_threads = options.num_threads + 1,
    };

    return xthread_run(&config, application_main, &options, argc, argv);
}
