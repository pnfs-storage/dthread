#include "xthread.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool initialized;
static bool use_dthreads;
static bool dthread_memory_ready;
static xthread_main_fn_t saved_application_main;

static void remove_argument(int *argc, char ***argv, int index)
{
    for (int i = index; i + 1 < *argc; ++i)
        (*argv)[i] = (*argv)[i + 1];

    --(*argc);
    (*argv)[*argc] = NULL;
}

int xthread_init(int *argc, char ***argv)
{
    int rv;

    if (initialized)
        return EALREADY;
    if (argc == NULL || argv == NULL || *argv == NULL)
        return EINVAL;

    for (int i = 1; i < *argc; ++i) {
        if (strcmp((*argv)[i], "--dthread") == 0) {
            use_dthreads = true;
            remove_argument(argc, argv, i);
            --i;
        }
    }

    if (use_dthreads) {
        rv = dthread_init(argc, argv);
        if (rv != 0)
            return rv;
    }

    initialized = true;
    return 0;
}

bool xthread_is_using_dthreads(void)
{
    return use_dthreads;
}

static int xthread_pointer_proc(int operation,
                                dthread_argret_t *argret,
                                void **pointer)
{
    if (argret == NULL)
        return EINVAL;

    switch (operation) {
    case DTHREAD_PROC_ENCODE:
        if (pointer == NULL)
            return EINVAL;
        if (*pointer == NULL) {
            argret->dt_argret_type = DTHREAD_NODATA;
            return 0;
        }

        argret->dt_argret_type = DTHREAD_SHMREF;
        return dthread_shm_ptr2ref(*pointer, 0, &argret->u.dt_shm);

    case DTHREAD_PROC_DECODE:
        if (argret->dt_argret_type == DTHREAD_NODATA) {
            if (pointer != NULL)
                *pointer = NULL;
            return 0;
        }
        if (argret->dt_argret_type != DTHREAD_SHMREF)
            return EINVAL;
        if (pointer == NULL)
            return 0;

        *pointer = dthread_shm_ref2ptr(&argret->u.dt_shm, 0);
        return *pointer == NULL ? EINVAL : 0;

    case DTHREAD_PROC_FREE:
        return 0;

    default:
        return EINVAL;
    }
}

static int xthread_dthread_application_main(int argc, char **argv)
{
    dthread_shmref_t arena;
    int rv;

    rv = dthread_alloc_shmarena(0, "break", 0, &arena);
    if (rv != 0) {
        fprintf(stderr, "xthread: cannot create shared allocator: %s\n",
                strerror(rv));
        return rv;
    }

    rv = dthread_default_shmarena(&arena);
    if (rv != 0) {
        fprintf(stderr, "xthread: cannot select shared allocator: %s\n",
                strerror(rv));
        return rv;
    }

    dthread_memory_ready = true;
    return saved_application_main(argc, argv);
}

static int checked_add_u64(uint64_t *total, uint64_t value)
{
    if (*total > UINT64_MAX - value)
        return EOVERFLOW;

    *total += value;
    return 0;
}

static int dthread_mapping_size(const xthread_config_t *config,
                                size_t index,
                                uint64_t *mapping_size)
{
    long page_size_value = sysconf(_SC_PAGESIZE);
    uint64_t page_size;
    uint64_t total;
    uint64_t remainder;

    if (page_size_value <= 0)
        return EINVAL;

    page_size = (uint64_t)page_size_value;
    total = (uint64_t)config->shmsrcs[index].application_bytes;

    /* dthreads reserves the first page of every mapping for metadata. */
    if (checked_add_u64(&total, page_size) != 0)
        return EOVERFLOW;

    if (index == 0) {
        /* Segment zero also stores dthreads' global thread table. */
        uint64_t thread_entry_size =
            8 * sizeof(uint32_t) + 2 * sizeof(dthread_argret_t);

        if (config->max_threads > UINT64_MAX / thread_entry_size)
            return EOVERFLOW;
        if (checked_add_u64(&total,
                            config->max_threads * thread_entry_size) != 0)
            return EOVERFLOW;

        /* Space for allocator metadata and allocation headers. */
        if (checked_add_u64(&total, page_size) != 0)
            return EOVERFLOW;
    }

    remainder = total % page_size;
    if (remainder != 0 && checked_add_u64(&total, page_size - remainder) != 0)
        return EOVERFLOW;

    *mapping_size = total;
    return 0;
}

static int dthread_source_flags(xthread_shmsrc_type_t type,
                                uint64_t *flags)
{
    switch (type) {
    case XTHREAD_SHMSRC_FILE:
        *flags = DTHREAD_SRC_FILE;
        return 0;
    case XTHREAD_SHMSRC_DEVICE:
        *flags = DTHREAD_SRC_DEV;
        return 0;
    case XTHREAD_SHMSRC_POSIX_SHM:
        *flags = DTHREAD_SRC_PSHM;
        return 0;
    default:
        return EINVAL;
    }
}

int xthread_run(const xthread_config_t *config,
                xthread_main_fn_t application_main,
                int argc,
                char **argv)
{
    dthread_dispatch_t *dispatch;
    dthread_shmsrc_t *shmsrcs;
    int dispatch_count;

    if (!initialized || config == NULL || application_main == NULL ||
        argv == NULL)
        return EINVAL;

    if (!use_dthreads)
        return application_main(argc, argv);

    if (config->entries == NULL || config->entry_count == 0 ||
        config->shmsrcs == NULL || config->shmsrc_count == 0 ||
        config->max_threads == 0 || config->entry_count >= INT_MAX ||
        config->shmsrc_count > INT_MAX || config->max_threads > INT_MAX)
        return EINVAL;

    dispatch_count = (int)config->entry_count + 1;
    dispatch = calloc((size_t)dispatch_count, sizeof(*dispatch));
    shmsrcs = calloc(config->shmsrc_count, sizeof(*shmsrcs));
    if (dispatch == NULL || shmsrcs == NULL) {
        free(shmsrcs);
        free(dispatch);
        return ENOMEM;
    }

    dispatch[0].dt_dispname = "xthread_application_main";
    dispatch[0].st.start0 = xthread_dthread_application_main;

    for (size_t i = 0; i < config->entry_count; ++i) {
        if (config->entries[i].name == NULL ||
            config->entries[i].start == NULL) {
            free(shmsrcs);
            free(dispatch);
            return EINVAL;
        }

        dispatch[i + 1].dt_dispname = (char *)config->entries[i].name;
        dispatch[i + 1].st.pthstart = config->entries[i].start;
        dispatch[i + 1].dt_argproc = xthread_pointer_proc;
        dispatch[i + 1].dt_retproc = xthread_pointer_proc;
    }

    for (size_t i = 0; i < config->shmsrc_count; ++i) {
        uint64_t mapping_size;
        int rv;

        if (config->shmsrcs[i].source == NULL) {
            free(shmsrcs);
            free(dispatch);
            return EINVAL;
        }

        rv = dthread_source_flags(config->shmsrcs[i].type,
                                  &shmsrcs[i].dt_srcflags);
        if (rv == 0)
            rv = dthread_mapping_size(config, i, &mapping_size);
        if (rv != 0) {
            free(shmsrcs);
            free(dispatch);
            return rv;
        }

        shmsrcs[i].dt_src = (char *)config->shmsrcs[i].source;
        shmsrcs[i].dt_mmoffset = config->shmsrcs[i].offset;
        shmsrcs[i].dt_mmsize = mapping_size;
    }

    saved_application_main = application_main;
    dthread_run(dispatch, dispatch_count,
                shmsrcs, (int)config->shmsrc_count,
                NULL, 0, DTHREAD_SYNCOP_ID_PSHARED,
                (int)config->max_threads, argc, argv);

    /* dthread_run() terminates the MPI job instead of returning. */
    return EIO;
}

static int set_shmsrc(xthread_shmsrc_t *shmsrc,
                      const char *source,
                      xthread_shmsrc_type_t type,
                      uint64_t offset,
                      size_t application_bytes)
{
    if (shmsrc == NULL || source == NULL || source[0] == '\0')
        return EINVAL;

    *shmsrc = (xthread_shmsrc_t) {
        .source = source,
        .type = type,
        .offset = offset,
        .application_bytes = application_bytes,
    };
    return 0;
}

int xthread_shmsrc_file(xthread_shmsrc_t *shmsrc,
                        const char *path,
                        size_t application_bytes)
{
    return set_shmsrc(shmsrc, path, XTHREAD_SHMSRC_FILE, 0,
                      application_bytes);
}

int xthread_shmsrc_device(xthread_shmsrc_t *shmsrc,
                          const char *path,
                          uint64_t offset,
                          size_t application_bytes)
{
    return set_shmsrc(shmsrc, path, XTHREAD_SHMSRC_DEVICE, offset,
                      application_bytes);
}

int xthread_shmsrc_posix_shm(xthread_shmsrc_t *shmsrc,
                             const char *name,
                             size_t application_bytes)
{
    return set_shmsrc(shmsrc, name, XTHREAD_SHMSRC_POSIX_SHM, 0,
                      application_bytes);
}

int xthread_create(xthread_t *thread,
                   const xthread_attr_t *attr,
                   xthread_start_fn_t start_routine,
                   void *arg)
{
    if (thread == NULL || start_routine == NULL)
        return EINVAL;

    if (use_dthreads) {
        return dthread_create(&thread->dthread,
                              (dthread_attr_t *)attr,
                              start_routine, arg);
    }

    return pthread_create(&thread->pthread, attr, start_routine, arg);
}

int xthread_join(xthread_t thread, void **retval)
{
    if (use_dthreads)
        return dthread_join(thread.dthread, retval);

    return pthread_join(thread.pthread, retval);
}

int xthread_barrier_init(xthread_barrier_t *barrier,
                         const xthread_barrierattr_t *attr,
                         unsigned int count)
{
    if (barrier == NULL || count == 0)
        return EINVAL;

    if (use_dthreads) {
        return dthread_barrier_init(&barrier->dthread,
                                    (dthread_barrierattr_t *)attr, count);
    }

    return pthread_barrier_init(&barrier->pthread, attr, count);
}

int xthread_barrier_wait(xthread_barrier_t *barrier)
{
    int rv;

    if (barrier == NULL)
        return EINVAL;

    if (use_dthreads)
        rv = dthread_barrier_wait(&barrier->dthread);
    else
        rv = pthread_barrier_wait(&barrier->pthread);

    if (rv == 0 || rv == PTHREAD_BARRIER_SERIAL_THREAD)
        return 0;
    return rv;
}

int xthread_barrier_destroy(xthread_barrier_t *barrier)
{
    if (barrier == NULL)
        return EINVAL;

    if (use_dthreads)
        return dthread_barrier_destroy(&barrier->dthread);

    return pthread_barrier_destroy(&barrier->pthread);
}

int xthread_mutex_init(xthread_mutex_t *mutex,
                       const xthread_mutexattr_t *attr)
{
    if (mutex == NULL)
        return EINVAL;

    if (use_dthreads) {
        return dthread_mutex_init(&mutex->dthread,
                                  (dthread_mutexattr_t *)attr);
    }

    return pthread_mutex_init(&mutex->pthread, attr);
}

int xthread_mutex_lock(xthread_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    if (use_dthreads)
        return dthread_mutex_lock(&mutex->dthread);

    return pthread_mutex_lock(&mutex->pthread);
}

int xthread_mutex_unlock(xthread_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    if (use_dthreads)
        return dthread_mutex_unlock(&mutex->dthread);

    return pthread_mutex_unlock(&mutex->pthread);
}

int xthread_mutex_destroy(xthread_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    if (use_dthreads)
        return dthread_mutex_destroy(&mutex->dthread);

    return pthread_mutex_destroy(&mutex->pthread);
}

void *xthread_malloc(size_t size)
{
    if (!use_dthreads)
        return malloc(size);
    if (!dthread_memory_ready)
        return NULL;

    return dthread_shm_malloc(NULL, size, NULL);
}

void *xthread_calloc(size_t count, size_t size)
{
    size_t total;
    void *pointer;

    if (!use_dthreads)
        return calloc(count, size);
    if (!dthread_memory_ready ||
        (count != 0 && size > SIZE_MAX / count))
        return NULL;

    total = count * size;
    pointer = dthread_shm_malloc(NULL, total, NULL);
    if (pointer != NULL)
        memset(pointer, 0, total);
    return pointer;
}

void xthread_free(void *ptr)
{
    if (ptr == NULL)
        return;

    if (use_dthreads)
        dthread_shm_free(NULL, ptr);
    else
        free(ptr);
}

int xthread_ptr_to_ref(void *ptr, size_t length, xthread_ref_t *ref)
{
    if (ptr == NULL || ref == NULL)
        return EINVAL;

    if (use_dthreads)
        return dthread_shm_ptr2ref(ptr, length, &ref->value.dthread);

    ref->value.pthread.pointer = ptr;
    ref->value.pthread.length = length;
    return 0;
}

void *xthread_ref_to_ptr(const xthread_ref_t *ref, size_t length)
{
    if (ref == NULL)
        return NULL;

    if (use_dthreads) {
        return dthread_shm_ref2ptr((dthread_shmref_t *)&ref->value.dthread,
                                   length);
    }

    if (length > ref->value.pthread.length)
        return NULL;
    return ref->value.pthread.pointer;
}
