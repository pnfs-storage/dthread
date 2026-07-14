#ifndef XTHREAD_H
#define XTHREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <dthread/dthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    pthread_t pthread;
    dthread_t dthread;
} xthread_t;

typedef pthread_attr_t xthread_attr_t;

typedef union {
    pthread_barrier_t pthread;
    dthread_barrier_t dthread;
} xthread_barrier_t;

typedef pthread_barrierattr_t xthread_barrierattr_t;

typedef union {
    pthread_mutex_t pthread;
    dthread_mutex_t dthread;
} xthread_mutex_t;

typedef pthread_mutexattr_t xthread_mutexattr_t;

typedef int (*xthread_main_fn_t)(int argc, char **argv);
typedef void *(*xthread_start_fn_t)(void *);

/* Each function that may be passed to xthread_create() has one entry. */
typedef struct {
    const char *name;
    xthread_start_fn_t start;
} xthread_entry_t;

typedef enum {
    XTHREAD_SHMSRC_FILE,
    XTHREAD_SHMSRC_DEVICE,
    XTHREAD_SHMSRC_POSIX_SHM,
} xthread_shmsrc_type_t;

/* application_bytes is just the max number of bytes the app might need.
 * excludes memory used internally by xthreads/dthreads. */
typedef struct {
    const char *source;
    xthread_shmsrc_type_t type;
    uint64_t offset;
    size_t application_bytes;
} xthread_shmsrc_t;

typedef struct {
    const xthread_entry_t *entries;
    size_t entry_count;
    const xthread_shmsrc_t *shmsrcs;
    size_t shmsrc_count;
    size_t max_threads;
} xthread_config_t;

/* A reference is portable between dthread ranks and pointer-like for pthreads. */
typedef struct {
    union {
        struct {
            void *pointer;
            size_t length;
        } pthread;
        dthread_shmref_t dthread;
    } value;
} xthread_ref_t;

/* Selects the backend, removes --dthread, and initializes dthreads if needed. */
int xthread_init(int *argc, char ***argv);
bool xthread_is_using_dthreads(void);

/*
 * Runs application_main directly for pthreads.  For dthreads it builds the
 * dispatch table and uses application_main as dispatch entry zero.
 */
int xthread_run(const xthread_config_t *config,
                xthread_main_fn_t application_main,
                int argc,
                char **argv);

int xthread_shmsrc_file(xthread_shmsrc_t *shmsrc,
                        const char *path,
                        size_t application_bytes);
int xthread_shmsrc_device(xthread_shmsrc_t *shmsrc,
                          const char *path,
                          uint64_t offset,
                          size_t application_bytes);
int xthread_shmsrc_posix_shm(xthread_shmsrc_t *shmsrc,
                             const char *name,
                             size_t application_bytes);

/* In dthread mode, arg and any non-NULL return value must be shared pointers. */
int xthread_create(xthread_t *thread,
                   const xthread_attr_t *attr,
                   xthread_start_fn_t start_routine,
                   void *arg);
int xthread_join(xthread_t thread, void **retval);

int xthread_barrier_init(xthread_barrier_t *barrier,
                         const xthread_barrierattr_t *attr,
                         unsigned int count);
int xthread_barrier_wait(xthread_barrier_t *barrier);
int xthread_barrier_destroy(xthread_barrier_t *barrier);

int xthread_mutex_init(xthread_mutex_t *mutex,
                       const xthread_mutexattr_t *attr);
int xthread_mutex_lock(xthread_mutex_t *mutex);
int xthread_mutex_unlock(xthread_mutex_t *mutex);
int xthread_mutex_destroy(xthread_mutex_t *mutex);

void *xthread_malloc(size_t size);
void *xthread_calloc(size_t count, size_t size);
void xthread_free(void *ptr);

/* in dthread mode, pointers aren't transferable so must be converted to shmem refs
 * in pthread mode, these are no-ops */
int xthread_ptr_to_ref(void *ptr, size_t length, xthread_ref_t *ref);
void *xthread_ref_to_ptr(const xthread_ref_t *ref, size_t length);

#ifdef __cplusplus
}
#endif

#endif
