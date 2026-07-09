#include "xthread.h" 
#include <string.h>

// Confined exclusively to this file's scope
static bool static_use_dthreads = false;

int xthread_init(int *argc, char ***argv) {
    if (argc == NULL || argv == NULL || *argv == NULL) {
        return -1;
    }

    // Inspect command-line arguments for the targeting directive
    for (int i = 0; i < *argc; i++) {
        if (strcmp((*argv)[i], "--dthreads") == 0) {
            static_use_dthreads = true;

            // Seamlessly chain-initialize the underlying dthreads subsystem!
            dthread_init(argc, argv);
            return 0;
        }
    }

    // Default path remains local pthreads
    static_use_dthreads = false;
    return 0;
}

bool xthread_is_using_dthreads(void) {
    return static_use_dthreads;
}

// Update your traffic cop calls to evaluate the internal function state:
int xthread_create(xthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg) {
    if (static_use_dthreads) {
        return dthread_create(&(thread->d_thread), (dthread_attr_t *)attr, start_routine, arg);
    } else {
        return pthread_create(&(thread->p_thread), (const pthread_attr_t *)attr, start_routine, arg);
    }
}

int xthread_join(xthread_t thread, void **retval) {
    if (static_use_dthreads) {
        return dthread_join(thread.d_thread, retval);
    } else {
        return pthread_join(thread.p_thread, retval);
    }
}

int xthread_barrier_init(xthread_barrier_t *barrier, const void *attr, unsigned int count) {
    if (static_use_dthreads) {
        return dthread_barrier_init(&(barrier->dt_barrier), (dthread_barrierattr_t *)attr, count);
    } else {
        return pthread_barrier_init(&(barrier->psh_barrier), (const pthread_barrierattr_t *)attr, count);
    }
}

int xthread_barrier_wait(xthread_barrier_t *barrier) {
    if (static_use_dthreads) {
        return dthread_barrier_wait(&(barrier->dt_barrier));
    } else {
        return pthread_barrier_wait(&(barrier->psh_barrier));
    }
}

int xthread_mutex_init(xthread_mutex_t *mutex, const void *attr) {
    if (static_use_dthreads) {
        return dthread_mutex_init(&(mutex->dt_mutex), (dthread_mutexattr_t *)attr);
    } else {
        return pthread_mutex_init(&(mutex->psh_mutex), (const pthread_mutexattr_t *)attr);
    }
}

int xthread_mutex_lock(xthread_mutex_t *mutex) {
    if (static_use_dthreads) {
        return dthread_mutex_lock(&(mutex->dt_mutex));
    } else {
        return pthread_mutex_lock(&(mutex->psh_mutex));
    }
}

int xthread_mutex_unlock(xthread_mutex_t *mutex) {
    if (static_use_dthreads) {
        return dthread_mutex_unlock(&(mutex->dt_mutex));
    } else {
        return pthread_mutex_unlock(&(mutex->psh_mutex));
    }
}
