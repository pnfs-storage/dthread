#ifndef XTHREAD_H
#define XTHREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <dthread/dthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    pthread_t p_thread;
    dthread_t d_thread;
} xthread_t;

typedef union {
    pthread_barrier_t p_barrier;
    dthread_barrier_t d_barrier;
} xthread_barrier_t;

typedef union {
    pthread_mutex_t p_mutex;
    dthread_mutex_t d_mutex;
} xthread_mutex_t;

typedef int (*xthread_main_fn_t)(int argc, char **argv);
typedef void *(*xthread_start_fn_t)(void *);

typedef struct {
    const char *name;
    xthread_start_fn_t start;
} xthread_start_t;

typedef struct {
    dthread_shmref_t d_ref;
} xthread_ref_t;

bool xthread_is_using_dthreads(void);

int xthread_run(int *argc,
                char ***argv,
                xthread_main_fn_t app_main,
                xthread_start_t *starts,
                int nstarts,
                const char *dthread_shm_path,
                size_t dthread_shm_size);

// LIFECYCLE CALLS
int xthread_create(xthread_t *thread,
                   const void *attr,
                   xthread_start_fn_t start_routine,
                   void *arg);
int xthread_join(xthread_t thread, void **retval);

// BARRIER CALLS
int xthread_barrier_init(xthread_barrier_t *barrier,
                         const void *attr,
                         unsigned int count);
int xthread_barrier_wait(xthread_barrier_t *barrier);
int xthread_barrier_destroy(xthread_barrier_t *barrier);

// MUTEX CALLS
int xthread_mutex_init(xthread_mutex_t *mutex, const void *attr);
int xthread_mutex_lock(xthread_mutex_t *mutex);
int xthread_mutex_unlock(xthread_mutex_t *mutex);
int xthread_mutex_destroy(xthread_mutex_t *mutex);

// SHMEM CALLS
void *xthread_malloc(size_t size);
void xthread_free(void *ptr);
int xthread_ptr_to_ref(void *ptr, size_t len, xthread_ref_t *ref);
void *xthread_ref_to_ptr(xthread_ref_t *ref, size_t len);

#ifdef __cplusplus
}
#endif

#endif
