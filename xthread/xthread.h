// include/dthread/xthread.h
#ifndef XTHREAD_H
#define XTHREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#include <dthread/dthread.h>
#include <stdbool.h>

// Global flag to control execution path at runtime
extern bool use_dthreads;

typedef struct {
    pthread_t p_thread;
    dthread_t d_thread;
} xthread_t;

typedef union {
    pthread_barrier_t psh_barrier;
    dthread_barrier_t dt_barrier;
} xthread_barrier_t;

typedef union {
    pthread_mutex_t psh_mutex;
    dthread_mutex_t dt_mutex;
} xthread_mutex_t;

int xthread_create(xthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int xthread_join(xthread_t thread, void **retval);
int xthread_barrier_init(xthread_barrier_t *barrier, const void *attr, unsigned int count);
int xthread_barrier_wait(xthread_barrier_t *barrier);

int xthread_mutex_init(xthread_mutex_t *mutex, const void *attr); 
int xthread_mutex_lock(xthread_mutex_t *mutex);
int xthread_mutex_unlock(xthread_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif // XTHREAD_H
