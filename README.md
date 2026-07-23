# dthread

The dthread library enables applications to use a pthread-like API
to distribute their threads across processes running on a set of
machines.  The intent is to be able to run multithreaded large memory
applications that require more RAM than can be placed in a single node
by leveraging shared memory interconnect technology such as CXL memory.

The dthread system is built from the following components:

 - MPI to launch dthread-based applications across multiple nodes,
   to provide point-to-point messaging for creating and managing threads,
   and to handle process errors (e.g. by shutting down the job in an orderly
   way).

 - pthreads to create and run the underlying application threads in
   each MPI rank's process.

- shared memory to allow application threads running in different
   processes (maybe on different nodes) to communicate without having
   to go through the network stack (or MPI).

The dthread system can be built and tested on a single node system
without a hardware shared memory interconnect using POSIX shared
memory objects or memory mapped files.

Continue reading below for more information about dthread architecture
and usage. You can use the instructions below to write new dthread
applications or to port existing pthread programs to use dthre. 
Additionally, we offer an [xthread](xthread) wrapper which allows
threaded programs to easily switch between pthread and dthread which
can be useful for benchmarking.

## dthread architecture overview

All dthread applications are launched with MPI.   MPI (and its
job scheduler, if any) is used to set number of nodes and number
of processes/ranks on each node to use.   If one of the MPI job's
ranks crashes or unexpectedly exits, we expect MPI to clean up
and shutdown the rest of the job's processes.   We assume the
nodes in a dthread MPI are homogeneous and that once a thread
is created in a rank it will remain on that rank (i.e. threads
do not migrate).

Each MPI rank in a dthread job has two internal threads and
runs zero or more application threads.   The two internal
threads are:

 - manager thread: handles thread lifecycle requests from
   application threads and processes thread lifecycle messages
   received from other ranks.

 - MPI thread: sends/receives point-to-point MPI messages
   to/from other ranks in the dthread MPI job.

The manager thread on rank 0 of the dthread MPI job has additional
responsibilities:

 - it creates the initial application thread in the rank 0 process

 - it bootstraps all the shared memory segments shared by the ranks
   and creates and manages the job's global thread state (e.g. the
   global thread table)

 - it is responsible for selecting which rank will be used when
   a new thread is created (a round robin scheme is currently used)

 - it manages thread life-cycle operations that operate across
   ranks (e.g. thread join operations)

The dthread system provides synchronization objects and APIs
based on pthreads (e.g. spin locks, mutexes, condition variables,
read/write locks, barriers).   The mapping from synchronization
operations on these objects to an underlying implementation
of the operations is set when dthreads is started.

The "pshared" synchronization operations provided with dthreads
provides pthreads-based synchronization that can be used with
POSIX shared memory object or memory mapped files on a single
node (for this to work we must set the PTHREAD_PROCESS_SHARED
attribute for all pthread synchronization objects used).

## building, testing, and installing the dthread package

To build the dthread library you need a posix system with:

 - cmake 3.10 or newer (to build)
 - MPI (where cmake can find it, we have built with MPICH and OpenMPI)

Obtain the dthread source code and choose an installation directory.
The dthread source code's home is: https://github.com/pnfs-storage/dthread

example commands:
```sh
# create a space to build/test
mkdir -p ~/src
cd ~/src
git clone https://github.com/pnfs-storage/dthread

# make build dir, configure and build
#
#   set DTHREAD_TESTING=ON and DTHREAD_MLOG=ON to run the tests
#       CMAKE_INSTALL_PREFIX is the installation directory
#
mkdir dthread-build
cd dthread-build
cmake -DBUILD_SHARED_LIBS=YES -DDTHREAD_TESTING=ON \
     -DDTHREAD_MLOG=ON -DCMAKE_INSTALL_PREFIX=~/dthread-prefix ../dthread
make

# run tests, skip this if DTHREAD_TESTING is OFF
ctest

# install
make install
```

## building dthread-based applications

Building dthread-based applications requires access to the dthread.h
header file and dthread library.  The applications must also be able
to link to the same MPI that was used to build the dthread library.

For cmake-based builds the dthread package can be used.  Here is
an example CMakeLists.txt file:
```cmake
cmake_minimum_required(VERSION 3.10)
project(test-app C)

find_package(dthread REQUIRED)

add_executable(test-app test-app.c)
target_link_libraries(test-app dthread)
```

When running cmake, make sure the correct MPI is in your path.
If the dthread library is installed in a non-standard directory,
add its prefix to the cmake path using the CMAKE_PREFIX_PATH
variable on the cmake command line, like this:
```sh
-DCMAKE_PREFIX_PATH=~/dthread-prefix
```

For non-cmake builds, use the appropriate mpicc and set the
compilers -I and -L values to point to the dthread install
if it is in a non-standard location.

## dthread startup API

Dthread applications are started using two calls: dthread_init()
and dthread_run().

The dthread_init() call should be issued early in the application's
startup.  This calls brings up MPI with MPI_Init() and loads the
MPI state into dthread's private memory:

```c
int dthread_init(int *argcp, char ***argvp);
```

After dthread_init() is called the application can process any
command line flags that are required to call dthread_run().

To call dthread_run() you need the following information:

 - an array of dispatch functions that can be used as the main
   routine of any newly created application threads.  If you
   create a new dthread, its start function must be in this array.
   Note that first entry of the dispatch table is special: it
   contains the function used to start the initial application
   thread on rank 0.

 - an array of all the shared memory sources that the dthread library
   should attach to at startup.

 - an optional array of user-defined shared memory allocators.
   If no user-defined allocators are provided, then only the
   internal allocators provided in the dthread library can be used.

 - the type of synchronization operations to use.
   DTHREAD_SYNCOP_ID_PSHARED should be used for single machine
   pthread/pshared synchronization.  (XXX: there will be other
   options for CXL memory at some point)

 - the maximum number of threads that can run.  This is used
   to size the global thread table in shared memory during bootstrap.

 - the argc/argv from main().  This is only passed to the initial
   app thread (that runs on rank 0) as part of the application
   bootstrap.  The initial app thread can optionally re-parse
   argc/argv for additional options.

The dthread_run prototype is:
```c
void dthread_run(dthread_dispatch_t *dsps, int ndsps,
                 dthread_shmsrc_t *shms, int nshms,
                 dthread_shmalloc_ops_t *usrmalloc, int nusrmalloc,
                 int syncop_id, int maxthreads,
                 int argc, char **argv);
```

Note that the dispatch, shmsrc, and shmalloc arrays must be
the same size and map to the same functions on each rank in
order for dthreads to function properly.   The syncop ID and
max thread value must also match.

The dthread_run() function sets up the dthread data structures
and then creates the internal manager and MPI threads.  On rank 0
it also starts the initial application thread (running the first
function in the dispatch array).   The dthread_run() function
never returns.  Instead, it will exit() the process when it is
done.

## dthread dispatch

The dthread dispatch structure describes a main function to
be used when a new thread is creates.  An array of dispatch
structures is passed into dthread_run() when the application
starts.  The index of a dispatch structure in this array
is used to portably identify main functions across ranks
(thus, the dispatch array passed to dthread_run() must match
across ranks in order for the index to be a valid identification
number).

The dthread library supports two types of call/return APIs for
calling dispatch functions: native and pthread-style.  The native
call/return API takes and returns a dthread_argret_t structure
while the pthread-style API uses void* pointers for call/return.

In order to make void* args work for the pthread-style API,
the user must provide additional "proc" functions that
encode/decode/free void* args to/from the native dthread_argret_t
structures.   (This is similar to the proc functions used
with RPCs under XDR.)

There are 3 types of dispatch functions:

 - dthread_start_app0_t: used to start the initial app thread on rank 0.
   The first entry in the dispatch table must be this type.

 - dthread_start_t: takes and returns dthread_argret_t structures.

 - dthread_start_pth_t: takes and returns void*.  Requires two additional
   proc functions to convert from void* to the native dthread_argret_t.

The typedefs for these are:
```c
typedef int (*dthread_start_app0_t)(int argc, char **argv);

typedef dthread_argret_t (*dthread_start_t)(dthread_argret_t *dt_arg);

typedef void *(*dthread_start_pth_t)(void *arg);
```

The proc functions for dthread_start_pth_t are defined as:
```c
#define DTHREAD_PROC_ENCODE     1    /* encode void* data to an argret */
#define DTHREAD_PROC_DECODE     2    /* decode an argret to void *data */
#define DTHREAD_PROC_FREE       3    /* free argret, void* */

typedef int (*dthread_proc_pth_t)(int procop, dthread_argret_t *arg,
                                  void **pth_arg);
```

The overall dispatch entry structure is:
```c
typedef struct {
    char *dt_dispname;                     /* name */
    union {
        dthread_start_app0_t start0;       /* first entry in table */
        dthread_start_t start;             /* native, NULL proc fns */
        dthread_start_pth_t pthstart;      /* pthread-style w/proc fns */
    } st;
    dthread_proc_pth_t dt_argproc;         /* arg proc for pth-style */
    dthread_proc_pth_t dt_retproc;         /* ret proc for pth-style */
} dthread_dispatch_t;
```

When creating a new thread, the user passes the local address of the
start function to the create API.  The create code searches the dispatch
array for the specified function to determine its index in the table.
The index is passed to the remote node when starting the new thread.

The dthread_argret_t structure is setup to allow small sized
arguments and returns to be passed inline (through MPI and
the global thread table in shared memory).   Larger buffers
can be passed using a shared memory reference (dthread_shmref_t).

```c
#define DTHREAD_INLINE_SIZE    64    /* max inline arg size, >= 16 */

/* type bits */
#define DTHREAD_NODATA          1    /* no data in argret */
#define DTHREAD_INLINE          2    /* data is inline */
#define DTHREAD_SHMREF          3    /* data is dthread_shmref_t */
#define DTHREAD_CANCELED        4    /* no ret, thread was canceled */

typedef struct {
    uint32_t dt_argret_type;               /* type of arg */
    uint32_t dt_inlinelen;                 /* only used if data inline */
    union {                                /* 64 bit alignment, no padding */
        char dt_inline[DTHREAD_INLINE_SIZE];  /* limits max dt_inlinelen */
        dthread_shmref_t dt_shm;
    } u;
} dthread_argret_t;
```

## dthread shared memory sources

The array of dthread_shmsrc_t passed to dthread_run() is used
to attached to shared memory.   Each dthread_shmsrc_t in the
array describes a shared memory segment.  Rank 0 always
attaches each shared memory segment first to set the segment
up for the other ranks to attach to.   Shared memory segments
are identified across ranks by their index in the array of
dthread_shmsrc_t structures (the "shmid").

```c
/* dt_srcflags values */
#define DTHREAD_SRC_DEV    0x1   /* device file */
#define DTHREAD_SRC_FILE   0x2   /* normal file */
#define DTHREAD_SRC_PSHM   0x3   /* posix shm file */
#define DTHREAD_SRC_MASK   0x3   /* source bitmask */

typedef struct {
    char *dt_src;                /* label/filename of src (depends on type) */
    uint64_t dt_srcflags;        /* info on how to interpret dt_src */
    uint64_t dt_mmoffset;        /* offset for mmap */
    uint64_t dt_mmsize;          /* size for mmap */
} dthread_shmsrc_t;
```

All references to shared memory can portably be encoded into
a dthread_shmref_t structure.   This allows references to
be passed between ranks.

```c
typedef struct {
    uint64_t dt_shmid;           /* shm segment id (same across ranks) */
    uint64_t dt_offset;          /* byte offset */
    uint64_t dt_length;          /* length (should not pass bounds) */
} dthread_shmref_t;
```

## dthread user-defined memory allocators

Users can provide their own shared memory malloc functions using
the dthread_shm_alloc_ops_t structure.   Each active instance of
a shared memory malloc allocator is described by the location
of its metadata in shared memory (using dthread_shmref_t).  We
use the term malloc "arena" to refer to the instance's metadata.

The shared memory metadata includes the index of the dthread_shm_alloc_ops_t
ops used to manage the memory:
```c
typedef struct {
    char *name;
    int (*init)(dthread_shmref_t *arena);
    int (*finalize)(dthread_shmref_t *arena);
    void *(*malloc)(dthread_shmref_t *arena, size_t size,
                    dthread_shmref_t *newref);
    void (*free)(dthread_shmref_t *arena, dthread_shmref_t *ref);
    void *(*realloc)(dthread_shmref_t *arena, dthread_shmref_t inref,
                     size_t new_size, dthread_shmref_t *newref);
} dthread_shm_alloc_ops_t;
```

All shared memory malloc metadata starts with a common structure that
is defined as:
```c
typedef struct {
    uint64_t mdmagic;           /* magic number (set when created) */
    dthread_shmref_t self;      /* our arena (including metadata) */
    uint64_t min_uoffset;       /* min user data offset in arena */
    uint64_t max_uoffset;       /* max user data offset in arena */
    uint64_t badalign_bits;     /* bad bits for proper alignment */
    int mopid;                  /* malloc-ops id (of malloc driver) */
    dthread_spinlock_t alock;   /* alloc md spin lock */
} dthread_shm_alloc_md_t;
```
The mopid is used to identify the region's dthread_shm_alloc_ops_t.

## dthread lifecycle functions

The dthread lifecycle functions are:

 - dthread_ncreate() / dthread_create(): create new thread using
   the native / pthread-style APIs
 - dthread_nexit() / dthread_exit(): terminate the current thread
   using the given return value
 - dthread_njoin() / dthread_join(): collect the return value of
   a terminated thread.  If the thread is still running, wait for
   it to exit first.
 - dthread_detach(): do not save the thread return value for a join
   operation, release all resources when the thread terminates.
   Detached threads cannot be joined.
 - dthread_cancel(): kill the specified thread

Note that join, detach, and cancel operations work across ranks.

Threads are referenced using the dthread_t structure.  This
structure can safely be passed between ranks (since it uses
an array index rather than pointers).
```c
typedef struct {
    uint32_t dt_index;           /* index in global thread table */
    uint32_t dt_seq;             /* generation sequence number */
} dthread_t;
```

The args to the lifecycle function are similar to their pthread
counterparts.   Some dthread structures are currently just aliases
for their pthread counterparts:

```c
/* reuse pthread attr structs for dthread API */
typedef pthread_attr_t dthread_attr_t;
typedef pthread_mutexattr_t dthread_mutexattr_t;
typedef pthread_condattr_t dthread_condattr_t;
typedef pthread_rwlockattr_t dthread_rwlockattr_t;
typedef pthread_barrierattr_t dthread_barrierattr_t;
```

## shared memory functions and dynamic memory allocation

As noted above, shared memory is portably referenced using the
dthread_shmref_t structure.   There are dthread APIs to convert
between a dthread_shmref_t and a local memory pointer:

```c
void *dthread_shmref2ptr(dthread_shmref_t *refp, uint64_t len);
int dthread_ptr2shmref(void *ptr, uint64_t len, dthread_shmref_t *refp);
```

Shared memory allocators can be deployed on a block of shared memory.
The dthread library currently contains one simple memory allocator
called "break" that allocates shared memory from a block and only
frees it when all allocations are released.

To create a shared memory allocator, use dthead_shm_new_arena()
to create an arena for the named allocator using the shared memory
segment id number of the block of shared memory.  Note that
the shmid corresponds to the index of the shared memory in the
shmsrc array passed to dthread_run():
```c
int dthead_shm_new_arena(uint64_t shmid, char *shmalloc_name,
                         size_t size, dthread_shmref_t *newarena);
```

The newly established shared memory arena is returned in the
"newarena" shmref.  This shmref can be passed to other threads
(e.g. through the thread args) to allow them to allocate from
it too.   This enables the following allocation functions to
be used:
```c
void *dthread_shm_malloc(dthread_shmref_t *arena, uint64_t len,
                         dthread_shmref_t *newref);
void dthread_shm_free(dthread_shmref_t *arena, void *ptr);
void dthread_shm_free_ref(dthread_shmref_t *arena, dthread_shmref_t *ref);
void *dthread_shm_realloc(dthread_shmref_t *arena, void *ptr, uint64_t len,
                          dthread_shmref_t *newref);
void *dthread_shm_realloc_ref(dthread_shmref_t *arena, dthread_shmref_t *inref,
                              uint64_t len, dthread_shmref_t *newref);
```

Note that the API allows memory to be freed/realloced by local
address or shmref.

Aside: considering added APIs to allow a default shm malloc arena
to be established (where you can just pass NULL in as the arena
and that will cause it to use the default).

## synchronization operations

The intent is to provide pthread-like high-level synchronization
APIs that get mapped down to the appropriate lower-level calls
(as specified by the "syncop_id" arg to dthread_run()).

To do this we define synchronization objects as unions like this:

```c
typedef union {
#ifdef DTHREAD_SYNC_PSHARED
    pthread_spinlock_t psh_spinlock;
#endif
/* other supported types of SYNC objects would go here.. */
} dthread_spinlock_t;
```

The high-level ops for a spin lock are:

```c
int dthread_spin_init(dthread_spinlock_t *lock, int pshared);
int dthread_spin_destroy(dthread_spinlock_t *lock);
int dthread_spin_lock(dthread_spinlock_t *lock);
int dthread_spin_trylock(dthread_spinlock_t *lock);
int dthread_spin_unlock(dthread_spinlock_t *lock);
```

If the syncop_id to dthread_run is DTHREAD_SYNCOP_ID_PSHARED, then
these ops would map directly to the pthread versions of these calls.

Other types of syncop_id (e.g. something CXL-based) will provide
their own implementation (or emulation) of the pthread-style sync ops.
(Or maybe not all ops are supported?)

The remaining high-level sync op API calls are:
```c
int dthread_mutex_init(dthread_mutex_t *mutex, dthread_mutexattr_t *attr);
int dthread_mutex_destroy(dthread_mutex_t *mutex);
int dthread_mutex_lock(dthread_mutex_t *mutex);
int dthread_mutex_trylock(dthread_mutex_t *mutex);
int dthread_mutex_unlock(dthread_mutex_t *mutex);
int dthread_mutex_timedlock(dthread_mutex_t *mutex,
         const struct timespec *timeout);

int dthread_cond_init(dthread_cond_t *cond, dthread_condattr_t *attr);
int dthread_cond_destroy(dthread_cond_t *cond);
int dthread_cond_broadcast(dthread_cond_t *cond);
int dthread_cond_signal(dthread_cond_t *cond);
int dthread_cond_wait(dthread_cond_t *cond, dthread_mutex_t *mutex);
int dthread_cond_timedwait(dthread_cond_t *cond, dthread_mutex_t *mutex,
         const struct timespec *abstime);

int dthread_rwlock_init(dthread_rwlock_t *lock, dthread_rwlockattr_t *attr);
int dthread_rwlock_destroy(dthread_rwlock_t *lock);
int dthread_rwlock_rdlock(dthread_rwlock_t *lock);
int dthread_rwlock_timedrdlock(dthread_rwlock_t *lock,
         const struct timespec *abstime);
int dthread_rwlock_tryrdlock(dthread_rwlock_t *lock);
int dthread_rwlock_wrlock(dthread_rwlock_t *lock);
int dthread_rwlock_timedwrlock(dthread_rwlock_t *lock,
         const struct timespec *abstime);
int dthread_rwlock_trywrlock(dthread_rwlock_t *lock);
int dthread_rwlock_unlock(dthread_rwlock_t *lock);

int dthread_barrier_init(dthread_barrier_t *barrier,
         dthread_barrierattr_t *attr, unsigned int count);
int dthread_barrier_destroy(dthread_barrier_t *barrier);
int dthread_barrier_wait(dthread_barrier_t *barrier);
```
