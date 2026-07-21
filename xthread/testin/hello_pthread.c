#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_NUM_THREADS 4
#define DEFAULT_ARRAY_SIZE 1024

typedef struct {
    size_t thread_id;
    size_t num_threads;
    size_t array_size;
    pid_t *array;
    pid_t *thread_pids;
    bool *thread_success;
    pthread_barrier_t *barrier;
} thread_arg_t;

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

static int parse_args(int argc, char **argv,
                      size_t *num_threads, size_t *array_size)
{
    static const struct option options[] = {
        { "numthreads", required_argument, NULL, 'n' },
        { "arraysize", required_argument, NULL, 'a' },
        { NULL, 0, NULL, 0 },
    };
    int option;

    *num_threads = DEFAULT_NUM_THREADS;
    *array_size = DEFAULT_ARRAY_SIZE;

    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'n':
            if (parse_positive_size(optarg, num_threads) != 0)
                return -1;
            break;
        case 'a':
            if (parse_positive_size(optarg, array_size) != 0)
                return -1;
            break;
        default:
            return -1;
        }
    }

    if (optind != argc || *num_threads > UINT_MAX)
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

    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");
    hostname[sizeof(hostname) - 1] = '\0';

    printf("[Pthread %zu] Hello from %s : PID %d\n",
           arg->thread_id, hostname, (int)pid);

    arg->thread_pids[arg->thread_id] = pid;
    for (size_t i = begin; i < end; ++i)
        arg->array[i] = pid;

    rv = pthread_barrier_wait(arg->barrier);
    if (rv != 0 && rv != PTHREAD_BARRIER_SERIAL_THREAD) {
        arg->thread_success[arg->thread_id] = false;
        return NULL;
    }

    arg->thread_success[arg->thread_id] = true;
    for (size_t i = neighbor_begin; i < neighbor_end; ++i) {
        if (arg->array[i] != arg->thread_pids[neighbor]) {
            arg->thread_success[arg->thread_id] = false;
            break;
        }
    }

    return NULL;
}

int main(int argc, char **argv)
{
    size_t num_threads;
    size_t array_size;
    pthread_t *threads;
    thread_arg_t *thread_args;
    pid_t *array;
    pid_t *thread_pids;
    bool *thread_success;
    pthread_barrier_t barrier;
    bool success = true;

    if (parse_args(argc, argv, &num_threads, &array_size) != 0) {
        fprintf(stderr,
                "usage: %s [--numthreads N] [--arraysize N]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (array_size % num_threads != 0) {
        fprintf(stderr, "--arraysize must be a multiple of --numthreads\n");
        return EXIT_FAILURE;
    }

    threads = calloc(num_threads, sizeof(*threads));
    thread_args = calloc(num_threads, sizeof(*thread_args));
    array = calloc(array_size, sizeof(*array));
    thread_pids = calloc(num_threads, sizeof(*thread_pids));
    thread_success = calloc(num_threads, sizeof(*thread_success));
    if (threads == NULL || thread_args == NULL || array == NULL ||
        thread_pids == NULL || thread_success == NULL) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    if (pthread_barrier_init(&barrier, NULL, (unsigned int)num_threads) != 0) {
        puts("FAILURE");
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < num_threads; ++i) {
        thread_args[i] = (thread_arg_t) {
            .thread_id = i,
            .num_threads = num_threads,
            .array_size = array_size,
            .array = array,
            .thread_pids = thread_pids,
            .thread_success = thread_success,
            .barrier = &barrier,
        };

        if (pthread_create(&threads[i], NULL, thread_hello,
                           &thread_args[i]) != 0) {
            puts("FAILURE");
            return EXIT_FAILURE;
        }
    }

    for (size_t i = 0; i < num_threads; ++i) {
        if (pthread_join(threads[i], NULL) != 0)
            success = false;
    }

    for (size_t i = 0; i < num_threads; ++i)
        success = success && thread_success[i];

    if (pthread_barrier_destroy(&barrier) != 0)
        success = false;

    free(thread_success);
    free(thread_pids);
    free(array);
    free(thread_args);
    free(threads);

    puts(success ? "SUCCESS" : "FAILURE");
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
