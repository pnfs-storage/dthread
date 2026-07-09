#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dthread/xthread.h>

#define NUM_THREADS 16

typedef struct {
    int tid;
    xthread_ref_t shared_array_ref;
    xthread_ref_t barrier_ref;
} thread_arg_t;

static void *thread_hello(void *args)
{
    thread_arg_t *arg = (thread_arg_t *)args;


    int *shared_array = xthread_ref_to_ptr(&arg->shared_array_ref, sizeof(int) * NUM_THREADS);
    xthread_barrier_t *barrier = xthread_ref_to_ptr(&arg->barrier_ref, sizeof(xthread_barrier_t));
    int tid = arg->tid;

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    printf("[Xthread/%s %d] Hello from %s : PID %d\n",
           xthread_is_using_dthreads() ? "dthread" : "pthread",
           tid,
           hostname,
           getpid());

    shared_array[tid] = tid;

    xthread_barrier_wait(barrier);

    int neighbor_idx = (tid + 1) % NUM_THREADS;
    int neighbor_val = shared_array[neighbor_idx];

    printf("[Xthread/%s %d] Read neighbor index %d, value is %d\n",
           xthread_is_using_dthreads() ? "dthread" : "pthread",
           tid,
           neighbor_idx,
           neighbor_val);

    return NULL;
}

static int application_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf(">>> Starting Xthread Baseline using %s <<<\n",
           xthread_is_using_dthreads() ? "dthreads" : "pthreads");

    int *shared_array =
        xthread_malloc(sizeof(int) * NUM_THREADS);

    if (shared_array == NULL) {
        fprintf(stderr, "xthread_malloc shared_array failed\n");
        return 1;
    }

    for (int i = 0; i < NUM_THREADS; i++)
        shared_array[i] = -1;

    xthread_barrier_t *barrier =
        xthread_malloc(sizeof(xthread_barrier_t));

    if (barrier == NULL) {
        fprintf(stderr, "xthread_malloc barrier failed\n");
        return 1;
    }

    xthread_barrier_init(barrier, NULL, NUM_THREADS);

    xthread_t threads[NUM_THREADS];

    thread_arg_t *thread_args =
        xthread_malloc(sizeof(thread_arg_t) * NUM_THREADS);

    if (thread_args == NULL) {
        fprintf(stderr, "xthread_malloc thread_args failed\n");
        return 1;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
	thread_args[i].tid = i;

	xthread_ptr_to_ref(shared_array,
			   sizeof(int) * NUM_THREADS,
			   &thread_args[i].shared_array_ref);

	xthread_ptr_to_ref(barrier,
			   sizeof(xthread_barrier_t),
			   &thread_args[i].barrier_ref);

	int rv = xthread_create(&threads[i],
				NULL,
				thread_hello,
				&thread_args[i]);

	if (rv != 0) {
	    fprintf(stderr, "xthread_create(%d) failed: %d\n", i, rv);
	    return 1;
	}
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        int rv = xthread_join(threads[i], NULL);

        if (rv != 0) {
            fprintf(stderr, "xthread_join(%d) failed: %d\n", i, rv);
            return 1;
        }
    }

    xthread_barrier_destroy(barrier);

    xthread_free(thread_args);
    xthread_free(barrier);
    xthread_free(shared_array);

    printf("Xthread test execution complete.\n");

    return 0;
}

int main(int argc, char **argv)
{
    xthread_start_t starts[] = {
        { "thread_hello", thread_hello },
    };

    return xthread_run(&argc,
                       &argv,
                       application_main,
                       starts,
                       sizeof(starts) / sizeof(starts[0]),
                       "/tmp/hello_xthread.shm",
                       4 * 1024 * 1024);
}
