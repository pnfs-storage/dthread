#ifndef XTHREAD_H
#define XTHREAD_H

#include <pthread.h>

// Safely pull in C-based dthreads symbols into C++
extern "C" {
#include <dthread/dthread.h>
}

// Global runtime traffic-cop flag
extern bool use_dthreads;

// Unified Type Definitions
typedef struct {
    pthread_t p_thread;
    dthread_t d_thread; 
} xthread_t;

typedef struct {
    pthread_mutex_t p_mutex;
    dthread_mutex_t d_mutex;
} xthread_mutex_t;

typedef struct {
    pthread_barrier_t p_barrier;
    dthread_barrier_t d_barrier;
} xthread_barrier_t;

// Unified Wrapper API Calls
int xthread_barrier_init(xthread_barrier_t *barrier, const void *attr, unsigned count);
int xthread_barrier_wait(xthread_barrier_t *barrier);

int xthread_mutex_init(xthread_mutex_t *mutex, const void *attr);
int xthread_mutex_lock(xthread_mutex_t *mutex);
int xthread_mutex_unlock(xthread_mutex_t *mutex);

int xthread_create(xthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int xthread_join(xthread_t thread, void **retval);

#endif // XTHREAD_H
