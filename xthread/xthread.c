#include "xthread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool use_dthreads = false;
static bool dthread_memory_ready = false;

static xthread_main_fn_t saved_app_main = NULL;

static dthread_dispatch_t *dthread_dispatch = NULL;
static int dthread_dispatch_count = 0;

static dthread_shmsrc_t dthread_shmsrcs[1];

static int strip_arg(int *argc, char ***argv, const char *arg)
{
    int out = 0;

    for (int i = 0; i < *argc; i++) {
        if (strcmp((*argv)[i], arg) == 0)
            continue;

        (*argv)[out++] = (*argv)[i];
    }

    (*argv)[out] = NULL;
    *argc = out;

    return 0;
}

static bool has_arg(int argc, char **argv, const char *arg)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], arg) == 0)
            return true;
    }

    return false;
}

static int xthread_dthread_app_main(int argc, char **argv)
{
    dthread_shmref_t arena;
    int rv;

    rv = dthread_alloc_shmarena(0, "break", 0, &arena);
    if (rv != 0) {
        fprintf(stderr, "dthread_alloc_shmarena failed: %s (%d)\n",
                strerror(rv), rv);
        return 1;
    }

    rv = dthread_default_shmarena(&arena);
    if (rv != 0) {
        fprintf(stderr, "dthread_default_shmarena failed: %s (%d)\n",
                strerror(rv), rv);
        return 1;
    }

    dthread_memory_ready = true;

    return saved_app_main(argc, argv);
}

bool xthread_is_using_dthreads(void)
{
    return use_dthreads;
}

static int xthread_argproc(int procop, dthread_argret_t *arg, void **pth_arg)
{
    switch (procop) {
    case DTHREAD_PROC_ENCODE:
        arg->dt_argret_type = DTHREAD_SHMREF;
        return dthread_shm_ptr2ref(*pth_arg, 0, &arg->u.dt_shm);

    case DTHREAD_PROC_DECODE:
        *pth_arg = dthread_shm_ref2ptr(&arg->u.dt_shm, 0);
        return (*pth_arg == NULL) ? EINVAL : 0;

    case DTHREAD_PROC_FREE:
        return 0;
    }

    return EINVAL;
}

static int xthread_retproc(int procop, dthread_argret_t *arg, void **pth_arg)
{
    switch (procop) {
    case DTHREAD_PROC_ENCODE:
        arg->dt_argret_type = DTHREAD_NODATA;
        return 0;

    case DTHREAD_PROC_DECODE:
        if (pth_arg)
            *pth_arg = NULL;
        return 0;

    case DTHREAD_PROC_FREE:
        return 0;
    }

    return EINVAL;
}

int xthread_run(int *argc,
                char ***argv,
                xthread_main_fn_t app_main,
                xthread_start_t *starts,
                int nstarts,
                const char *dthread_shm_path,
                size_t dthread_shm_size)
{
    int rv;

    if (argc == NULL || argv == NULL || *argv == NULL || app_main == NULL)
        return EINVAL;

    use_dthreads = has_arg(*argc, *argv, "--dthreads");
    strip_arg(argc, argv, "--dthreads");

    if (use_dthreads) {
        if (dthread_shm_path == NULL || dthread_shm_size == 0)
            return EINVAL;

        dthread_shmsrcs[0].dt_src = (char *)dthread_shm_path;
        dthread_shmsrcs[0].dt_srcflags = DTHREAD_SRC_FILE;
        dthread_shmsrcs[0].dt_mmoffset = 0;
        dthread_shmsrcs[0].dt_mmsize = dthread_shm_size;
    }

    saved_app_main = app_main;

    if (!use_dthreads)
        return app_main(*argc, *argv);

    rv = dthread_init(argc, argv);
    if (rv != 0)
        return rv;

    dthread_dispatch_count = nstarts + 1;

    dthread_dispatch =
        calloc((size_t)dthread_dispatch_count, sizeof(dthread_dispatch_t));

    if (dthread_dispatch == NULL)
        return ENOMEM;

    dthread_dispatch[0].dt_dispname = "xthread_app_main";
    dthread_dispatch[0].st.start0 = xthread_dthread_app_main;

    for (int i = 0; i < nstarts; i++) {
        dthread_dispatch[i + 1].dt_dispname = (char *)starts[i].name;
        dthread_dispatch[i + 1].st.pthstart = starts[i].start;
	dthread_dispatch[i + 1].dt_argproc = xthread_argproc;
	dthread_dispatch[i + 1].dt_retproc = xthread_retproc;
    }

    dthread_run(dthread_dispatch,
                dthread_dispatch_count,
                dthread_shmsrcs,
                1,
                NULL,
                0,
                DTHREAD_SYNCOP_ID_PSHARED,
                1024,
                *argc,
                *argv);

    return 1;
}

int xthread_create(xthread_t *thread,
                   const void *attr,
                   xthread_start_fn_t start_routine,
                   void *arg)
{
    if (use_dthreads) {
        return dthread_create(&thread->d_thread,
                              (dthread_attr_t *)attr,
                              start_routine,
                              arg);
    }

    return pthread_create(&thread->p_thread,
                          (const pthread_attr_t *)attr,
                          start_routine,
                          arg);
}

int xthread_join(xthread_t thread, void **retval)
{
    if (use_dthreads)
        return dthread_join(thread.d_thread, retval);

    return pthread_join(thread.p_thread, retval);
}

int xthread_barrier_init(xthread_barrier_t *barrier,
                         const void *attr,
                         unsigned int count)
{
    if (use_dthreads) {
        return dthread_barrier_init(&barrier->d_barrier,
                                    (dthread_barrierattr_t *)attr,
                                    count);
    }

    return pthread_barrier_init(&barrier->p_barrier,
                                (const pthread_barrierattr_t *)attr,
                                count);
}

int xthread_barrier_wait(xthread_barrier_t *barrier)
{
    if (use_dthreads)
        return dthread_barrier_wait(&barrier->d_barrier);

    return pthread_barrier_wait(&barrier->p_barrier);
}

int xthread_barrier_destroy(xthread_barrier_t *barrier)
{
    if (use_dthreads)
        return dthread_barrier_destroy(&barrier->d_barrier);

    return pthread_barrier_destroy(&barrier->p_barrier);
}

int xthread_mutex_init(xthread_mutex_t *mutex, const void *attr)
{
    if (use_dthreads) {
        return dthread_mutex_init(&mutex->d_mutex,
                                  (dthread_mutexattr_t *)attr);
    }

    return pthread_mutex_init(&mutex->p_mutex,
                              (const pthread_mutexattr_t *)attr);
}

int xthread_mutex_lock(xthread_mutex_t *mutex)
{
    if (use_dthreads)
        return dthread_mutex_lock(&mutex->d_mutex);

    return pthread_mutex_lock(&mutex->p_mutex);
}

int xthread_mutex_unlock(xthread_mutex_t *mutex)
{
    if (use_dthreads)
        return dthread_mutex_unlock(&mutex->d_mutex);

    return pthread_mutex_unlock(&mutex->p_mutex);
}

int xthread_mutex_destroy(xthread_mutex_t *mutex)
{
    if (use_dthreads)
        return dthread_mutex_destroy(&mutex->d_mutex);

    return pthread_mutex_destroy(&mutex->p_mutex);
}

void *xthread_malloc(size_t size)
{
    if (use_dthreads) {
        if (!dthread_memory_ready)
            return NULL;

        return dthread_shm_malloc(NULL, size, NULL);
    }

    return malloc(size);
}

void xthread_free(void *ptr)
{
    if (ptr == NULL)
        return;

    if (use_dthreads) {
        dthread_shm_free(NULL, ptr);
        return;
    }

    free(ptr);
}

int xthread_ptr_to_ref(void *ptr,
                       size_t len,
                       xthread_ref_t *ref)
{
    if (use_dthreads) {
        return dthread_shm_ptr2ref(ptr, len, &ref->d_ref);
    }

    /*
     * Pthread mode: fake a shmref by storing the pointer.
     */
    ref->d_ref.dt_shmid  = UINT64_MAX;
    ref->d_ref.dt_offset = (uint64_t)(uintptr_t)ptr;
    ref->d_ref.dt_length = len;

    return 0;
}

void *xthread_ref_to_ptr(xthread_ref_t *ref,
                         size_t len)
{
    if (use_dthreads) {
        return dthread_shm_ref2ptr(&ref->d_ref, len);
    }

    (void)len;

    return (void *)(uintptr_t)ref->d_ref.dt_offset;
}
