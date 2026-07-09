#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define NUM_THREADS 4

typedef struct {
    int tid;
    int *shared_array;
    pthread_barrier_t *barrier;
} thread_arg_t;

static void *thread_hello(void *args)
{
    thread_arg_t *arg = (thread_arg_t *)args;
    int tid = arg->tid;

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    printf("[Pthread %d] Hello from %s : PID %d\n",
           tid, hostname, getpid());

    arg->shared_array[tid] = tid;

    pthread_barrier_wait(arg->barrier);

    int neighbor_idx = (tid + 1) % NUM_THREADS;
    int neighbor_val = arg->shared_array[neighbor_idx];

    printf("[Pthread %d] Read neighbor index %d, value is %d\n",
           tid, neighbor_idx, neighbor_val);

    return NULL;
}

static int application_main(void)
{
    int *shared_array = malloc(sizeof(int) * NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++)
        shared_array[i] = -1;

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, NUM_THREADS);

    pthread_t threads[NUM_THREADS];
    thread_arg_t thread_args[NUM_THREADS];

    for (int i = 1; i < NUM_THREADS; i++) {
        thread_args[i].tid = i;
        thread_args[i].shared_array = shared_array;
        thread_args[i].barrier = &barrier;

        pthread_create(&threads[i],
                       NULL,
                       thread_hello,
                       &thread_args[i]);
    }

    thread_args[0].tid = 0;
    thread_args[0].shared_array = shared_array;
    thread_args[0].barrier = &barrier;

    thread_hello(&thread_args[0]);

    for (int i = 1; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    pthread_barrier_destroy(&barrier);

    free(shared_array);

    printf("Pthread test execution complete.\n");

    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf(">>> Starting Pure Pthread Baseline <<<\n");

    return application_main();
}
