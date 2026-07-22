/*
    Distributed Under the MIT license
	Uses vertex coloring to distinguish searches
    Programs by Masab Ahmad (UConn)
*/

#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <dthread/xthread.h>
//#include "carbon_user.h"     /* For the Graphite Simulator*/
#include <time.h>
#include <sys/timeb.h>
#include <unistd.h>

#define INT_MAX        100000000
// #define DEBUG              1
#define BILLION 1E9

static const int VERBOSE=1;

/* enum for all of the various memory allocations
 * using an enum simplifies the various operations
 * we need to perform for memory allocations
 * 1. predetermine maximum memory allocation size
 * 2. actually allocate the memory regions
 * 3. convert pointers to xthread references
 * 4. free the allocations
 */
typedef enum {
   BFS_ALLOC_STATE,
   BFS_ALLOC_THREAD_ARGS,
   BFS_ALLOC_D,
   BFS_ALLOC_Q,
   BFS_ALLOC_EDGES,
   BFS_ALLOC_EXIST,
   BFS_ALLOC_TEMPORARY,
   BFS_ALLOC_W_INDEX,
   BFS_ALLOC_LOCKS,
   BFS_ALLOC_COUNT
} bfs_shared_allocation_id_t;

/* a descriptor for each memory allocation */
typedef struct {
   const char*   name;
   size_t        count;
   size_t        element_size;
   void*         pointer;
   xthread_ref_t reference;
} bfs_shared_allocation_t;

//Shared BFS state
typedef struct
{
  xthread_barrier_t barrier_total;
  int       P;
  int       N;
  int       DEG;
  int       largest;
  int       Total;
  int       terminate;
  xthread_ref_t Q_ref;
  xthread_ref_t D_ref;
  xthread_ref_t W_index_ref;
  xthread_ref_t edges_ref;
  xthread_ref_t exist_ref;
  xthread_ref_t temporary_ref;
  xthread_ref_t locks_ref;
} bfs_shared_state_t;

// the command-line options
typedef struct
{
   int         select;
   int         P;
   int         N;
   int         DEG;
   const char* filename;
   const char* shm_path;
} bfs_options_t;

// what is passed to application_main
typedef struct
{
   bfs_options_t options;
   bfs_shared_allocation_t allocations[BFS_ALLOC_COUNT];
} bfs_application_context_t;

/*
 * This is called once by every rank from main(), because every rank must
 * compute the same shared-memory configuration, and again by rank 0 from
 * application_main(), where the application actually runs.
 */
static int parse_options(int argc, char** argv, bfs_options_t* opts)
{
   if (opts == NULL || argc < 3)
      return EINVAL;

   opts->select = atoi(argv[1]);
   opts->P = atoi(argv[2]);
   opts->shm_path = "/tmp/bfs_xthread.shm";
   if (opts->P <= 0)
      return EINVAL;

   if (opts->select == 1) {
      if (argc != 4 && argc != 5)
         return EINVAL;
      opts->filename = argv[3];
      if (argc == 5)
         opts->shm_path = argv[4];
      opts->N = 2097152;  // default upper limit for file input
      opts->DEG = 16;     // default upper limit for file input
   } else if (opts->select == 0) {
      if (argc != 5 && argc != 6)
         return EINVAL;
      opts->N = atoi(argv[3]);
      opts->DEG = atoi(argv[4]);
      if (argc == 6)
         opts->shm_path = argv[5];
      if (opts->N <= 0 || opts->DEG <= 0)
         return EINVAL;
   } else {
      return EINVAL;
   }

   return 0;
}

static void print_usage(const char* program)
{
   fprintf(stderr,
           "usage: %s [--dthread] 0 THREADS VERTICES DEGREE [SHM_PATH]\n"
           "       %s [--dthread] 1 THREADS INPUT_FILE [SHM_PATH]\n",
           program, program);
}

//Thread Argument Structure
typedef struct
{
   xthread_ref_t state_ref;
   int          tid;
} bfs_thread_arg_t;

//Function Initializers
int initialize_single_source(int* D, int* Q, int source, int N);
void init_weights(int N, int DEG, int* W_index);

//Primary Thread Function
void* do_work(void* args)
{

   bfs_thread_arg_t* arg = (bfs_thread_arg_t*)args;

   bfs_shared_state_t* state = (bfs_shared_state_t*)xthread_ref_to_ptr(
      &arg->state_ref, sizeof(*state));
   if(state == NULL) return NULL;

   if ( VERBOSE ) {
       char hostname[256];
       pid_t pid = getpid();
       if (gethostname(hostname, sizeof(hostname)) != 0)
           snprintf(hostname, sizeof(hostname), "unknown");
       hostname[sizeof(hostname) - 1] = '\0';

       printf("[Xthread %d] Hello from %s : PID %d\n", arg->tid, hostname, (int)pid);
    }

   int tid = arg->tid;
   int P = state->P;

   volatile int* Q = (volatile int*)xthread_ref_to_ptr(
      &state->Q_ref, (size_t)state->N * sizeof(*Q));
   int* D = (int*)xthread_ref_to_ptr(
      &state->D_ref, (size_t)state->N * sizeof(*D));
   int* W_index = (int*)xthread_ref_to_ptr(
      &state->W_index_ref,
      (size_t)state->N * (size_t)state->DEG * sizeof(*W_index));
   int* edges = (int*)xthread_ref_to_ptr(
      &state->edges_ref, (size_t)state->N * sizeof(*edges));
   int* exist = (int*)xthread_ref_to_ptr(
      &state->exist_ref, (size_t)state->N * sizeof(*exist));
   int* temporary = (int*)xthread_ref_to_ptr(
      &state->temporary_ref, (size_t)state->N * sizeof(*temporary));
   xthread_mutex_t* locks = (xthread_mutex_t*)xthread_ref_to_ptr(
      &state->locks_ref, (size_t)state->N * sizeof(*locks));

   if(Q == NULL || D == NULL || W_index == NULL ||
      edges == NULL || exist == NULL ||
      temporary == NULL || locks == NULL)
   {
      return NULL;
   }

   int v = 0;
   int iter = 0;
   //For precision work allocation
   double P_d = P;
   double tid_d = tid;
   double largest_d = state->largest+1.0;

   int start =  0;  //tid    * DEG / (state->P);
   int stop  = 0;   //(tid+1) * DEG / (state->P);

   //Partition data into threads
   double start_d = (tid_d) * (largest_d/P_d);
   double stop_d = (tid_d+1.0) * (largest_d/P_d);
   start = start_d; //tid    *  (largest+1) / (P);
   stop = stop_d; //(tid+1) *  (largest+1) / (P);

   //printf("\n tid:%d %d %d",tid,start,stop);

   xthread_barrier_wait(&state->barrier_total);

   while(state->terminate==0)
   {   
      for(v=start;v<stop;v++)
      {
         if(exist[v]==0)
            continue;                              //if not in graph
         //printf("\nv:%d Q:%d %d",v, Q[v], D[v]);
         if(D[v]==0 || D[v]==2)                    //already colored
            continue;
         //printf("\nuu:%d Q:%d %d",v, Q[v], P); 
         //D[v]=2;

         for(int i = 0; i < edges[v]; i++)
         {   
            int neighbor = W_index[v * state->DEG + i];
            //printf("\n Came in");
            if(Q[neighbor]==1)                       //Uncomment for test and test and set
            {
            xthread_mutex_lock(&locks[neighbor]);
            if(Q[neighbor]==1)                       //if unset then set
               Q[neighbor]=0;                        //Can be set to Parent
            temporary[neighbor] = 1;
            xthread_mutex_unlock(&locks[neighbor]);
            }
         }
      }
      //if(tid==0) printf("\n %d",Q[state->largest]);

      xthread_barrier_wait(&state->barrier_total);
    
      //Update colors	
      for(v=start;v<stop;v++)
      {
         if(D[v]==1)
           D[v] = 2;
         else
           D[v] = temporary[v];
      }
 
      //Termination Condition
      if(Q[state->largest]==0 || iter>=state->Total)
        state->terminate=1;
      iter++;
      xthread_barrier_wait(&state->barrier_total);
   }
   //printf("\n %d %d",tid,state->terminate);
   xthread_barrier_wait(&state->barrier_total);

   return NULL;
}

/* 
 * bfs allocates a lot of memory regions
 * so here we attempt to centralize their descriptions 
 * to make subsequent code easier to read and less error prone
 */
static void create_allocation_descriptors( 
        const bfs_options_t* options, 
        bfs_shared_allocation_t allocations[BFS_ALLOC_COUNT]) 
{
   allocations[BFS_ALLOC_STATE] = {
      "state", 1, sizeof(bfs_shared_state_t), NULL, {}
   };

   allocations[BFS_ALLOC_THREAD_ARGS] = {
      "thread arguments",
      (size_t)options->P,
      sizeof(bfs_thread_arg_t),
      NULL,
      {}
   };

   allocations[BFS_ALLOC_Q] = {
      "Q", (size_t)options->N, sizeof(int), NULL, {}
   };

   allocations[BFS_ALLOC_D] = {
      "D", (size_t)options->N, sizeof(int), NULL, {}
   };

   allocations[BFS_ALLOC_W_INDEX] = {
      "W_index",
      (size_t)options->N * (size_t)options->DEG,
      sizeof(int),
      NULL,
      {}
   };

   allocations[BFS_ALLOC_EDGES] = {
      "edges", (size_t)options->N, sizeof(int), NULL, {}
   };

   allocations[BFS_ALLOC_EXIST] = {
      "exist", (size_t)options->N, sizeof(int), NULL, {}
   };

   allocations[BFS_ALLOC_TEMPORARY] = {
      "temporary", (size_t)options->N, sizeof(int), NULL, {}
   };

   allocations[BFS_ALLOC_LOCKS] = {
      "locks",
      (size_t)options->N,
      sizeof(xthread_mutex_t),
      NULL,
      {}
   };
}

int application_main(void* context_ptr, int argc, char** argv) {
    bfs_application_context_t* context =
        (bfs_application_context_t*)context_ptr;
    bfs_options_t* opts = &context->options;
    bfs_shared_allocation_t* allocations = context->allocations;
    FILE *file0 = NULL;
    bfs_shared_state_t *state;
    bfs_thread_arg_t *thread_arg;
    xthread_t *thread_handle;

    (void)argc;
    (void)argv;

    if (opts->select == 1) {
        printf("\nGraph with Parameters: N:%d DEG:%d\n",opts->N,opts->DEG);
    }

    if (opts->DEG > opts->N) {
      fprintf(stderr, "Degree of graph cannot be grater than number of Vertices\n");
      exit(EXIT_FAILURE);
    }

    // allocate memory and then store the pointer in our allocations structure
    // also create and store xthread_ref_t references which are necessary for dthread mode (no-op for pthread) 
    for(int i = 0; i < BFS_ALLOC_COUNT; ++i) {
       allocations[i].pointer = xthread_calloc(allocations[i].count, allocations[i].element_size);
       if(allocations[i].pointer == NULL) {
          fprintf(stderr, "Allocation of %s failed\n", allocations[i].name);
          return ENOMEM;
       }
       size_t bytes = allocations[i].count * allocations[i].element_size;
       if(xthread_ptr_to_ref(allocations[i].pointer, bytes, &allocations[i].reference) != 0) {
          fprintf(stderr, "Creating reference for %s failed\n", allocations[i].name);
          return EXIT_FAILURE;
       }
    }

    // set up the structures used to store state that is eventually passed to the threads
    state = (bfs_shared_state_t*)allocations[BFS_ALLOC_STATE].pointer;
    thread_arg = (bfs_thread_arg_t*)allocations[BFS_ALLOC_THREAD_ARGS].pointer;

    // thread_handle can just use calloc since it's not a shared structure
    thread_handle = (xthread_t*) calloc(opts->P, sizeof(*thread_handle));
    if(thread_handle == NULL) {
      fprintf(stderr, "Allocation of memory failed\n");
      return ENOMEM;
    }

    // put the necessary refs into the state struct which will be passed to each thread
    // and store the other directly embeddable variables in there as well
    // and initialize our barrier
    state->D_ref         = allocations[BFS_ALLOC_D].reference;
    state->Q_ref         = allocations[BFS_ALLOC_Q].reference;
    state->W_index_ref   = allocations[BFS_ALLOC_W_INDEX].reference;
    state->edges_ref     = allocations[BFS_ALLOC_EDGES].reference;
    state->exist_ref     = allocations[BFS_ALLOC_EXIST].reference;
    state->temporary_ref = allocations[BFS_ALLOC_TEMPORARY].reference;
    state->locks_ref     = allocations[BFS_ALLOC_LOCKS].reference;
    state->P   = opts->P;
    state->N   = opts->N;
    state->DEG = opts->DEG;
    if ( xthread_barrier_init(&state->barrier_total, NULL, opts->P) != 0 ) {
       fprintf(stderr, "Barrier initialization failed\n");
      return EXIT_FAILURE;
    }

    // get pointers to the allocations so the rest of the initialization will work
    int* D = (int*)allocations[BFS_ALLOC_D].pointer;
    int* Q = (int*)allocations[BFS_ALLOC_Q].pointer;
    int* W_index = (int*)allocations[BFS_ALLOC_W_INDEX].pointer;
    int* edges = (int*)allocations[BFS_ALLOC_EDGES].pointer;
    int* exist = (int*)allocations[BFS_ALLOC_EXIST].pointer;
    xthread_mutex_t* locks = (xthread_mutex_t*)allocations[BFS_ALLOC_LOCKS].pointer;
    
    //Memory initialization
    for(size_t i = 0; i < allocations[BFS_ALLOC_W_INDEX].count; ++i) {
      W_index[i] = INT_MAX;
    }

    //If reading from file
    if(opts->select==1)
    {
      int lines_to_check=0;
      char c;
      int number0;
      int number1;
      int inter = -1; 
      file0 = fopen(opts->filename,"r");
      if(file0 == NULL) {
         fprintf(stderr, "Unable to open %s\n", opts->filename);
         return EXIT_FAILURE;
      }
      for(c=getc(file0); c!=EOF; c=getc(file0)) {
         if(c=='\n')
            lines_to_check++;

         if(lines_to_check>3)
         {
            int f0 = fscanf(file0, "%d %d", &number0,&number1);
            if(f0 != 2 && f0 != EOF)
            {
               printf ("Error: Read %d values, expected 2. Parsing failed.\n",f0);
               exit (EXIT_FAILURE);
            }
            //printf("\n%d %d",number0,number1);
            if(number0>state->largest)
               state->largest=number0;
            if(number1>state->largest)
               state->largest=number1;
            inter = edges[number0];

            W_index[number0 * opts->DEG + inter] = number1;
            //previous_node = number0;
            edges[number0]++;
            exist[number0]=1; exist[number1]=1;
         }
      }
      fclose(file0);
      file0 = NULL;
      //printf("\n%d deg:%d",test[0]);
      printf("\nFile Read, Largest Vertex:%d",state->largest);
    }

    //Generate Random graph
    if(opts->select==0)
    {
      init_weights(opts->N, opts->DEG, W_index);
      state->largest = opts->N-1; //largest vertex id
    }


    for(int i=0; i<state->largest+1; i++)
    {
      if(opts->select==0)
      {
         exist[i] = 1;
         edges[i] = opts->DEG;
      }
      if(exist[i]==1)
      {
         state->Total++;
         xthread_mutex_init(&locks[i], NULL);
      }
    }
    //printf("\n %d %d %d",N,state->largest,state->Total);

    //Initialize Data Structures
    initialize_single_source(D, Q, 0, opts->N);

    //Thread arguments
    for(int j = 0; j < opts->P; j++) {
      thread_arg[j].state_ref = allocations[BFS_ALLOC_STATE].reference;
      thread_arg[j].tid = j;
    }

    // Enable Graphite performance and energy models
    //CarbonEnableModels();

    //CPU Time
    struct timespec requestStart, requestEnd;
    clock_gettime(CLOCK_REALTIME, &requestStart);

    //Spawn Threads
    for(int j = 1; j < opts->P; j++) {
      int ret = xthread_create(thread_handle+j, NULL, do_work, (void*)&thread_arg[j]);
      if ( ret != 0 ) {
         fprintf(stderr, "Unable to create thread %d\n", j);
         return EXIT_FAILURE;
      }
    }
    do_work((void*) &thread_arg[0]);  //master thread initializes itself

    //Join threads
    for(int j = 1; j < opts->P; j++) { //mul = mul*2;
      xthread_join(thread_handle[j],NULL);
    }

    printf("\nThreads Joined!");

    clock_gettime(CLOCK_REALTIME, &requestEnd);
    double accum = ( requestEnd.tv_sec - requestStart.tv_sec ) + ( requestEnd.tv_nsec - requestStart.tv_nsec ) / BILLION;
    printf( "\nTime Taken:\n%lf seconds", accum );

    // Disable Graphite performance and energy models
    //CarbonDisableModels();

    //Print Result
    FILE * pfile;
    pfile = fopen("myfile.txt","w");
    for(int j=0;j<=state->largest;j++)
    {
     if(exist[j]==1) //printf("\n %d ",state->Q[j]);
       fprintf(pfile,"\n %d %d ", j,Q[j]);
    }
    printf("\n");
    fclose(pfile);

    for(int i=0; i<state->largest+1; i++) {
      if(exist[i]==1)
         xthread_mutex_destroy(&locks[i]);
    }
    xthread_barrier_destroy(&state->barrier_total);
    free(thread_handle);

    for(int i = BFS_ALLOC_COUNT - 1; i >= 0; --i) {
      xthread_free(allocations[i].pointer);
    }

    return 0;
}

int main(int argc, char** argv) {
    bfs_application_context_t context = {};
    bfs_options_t* opts = &context.options;
    xthread_config_t config = {};
    xthread_shmsrc_t shmsrc;

    /* initialize xthread - if --dthread is passed, will call mpi_init; otherwise no-op */
    if (xthread_init(&argc, &argv) != 0) {
       return EXIT_FAILURE;
    }

    /* Every rank parses these arguments to build an identical configuration. */
    if (parse_options(argc, argv, opts) != 0) {
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }

    /* function pointers for each possible thread function */
    static const xthread_entry_t entries[] = {
        { "do_work", do_work },
    };

    /*
     * In dthreads mode, the maximum allocated memory must be precomputed in order to set up a shared memory region 
     * 1. Create descriptions for each subsequent allocation and then compute the max size
     * 2. Also create a descriptor for the potential shared memory region
     * This happens in pthreads mode as well but this information is just not used. 
     */
    create_allocation_descriptors(opts, context.allocations);
    size_t application_bytes = 0;
    for(int i = 0; i < BFS_ALLOC_COUNT; ++i)
        application_bytes += (context.allocations[i].count * context.allocations[i].element_size);
    if(xthread_shmsrc_file(&shmsrc, opts->shm_path, application_bytes) != 0) {
       fprintf(stderr, "Unable to configure xthread shared memory\n");
       return EXIT_FAILURE;
    }

    /* define the configuration that we need to pass to xthread_run
     * includes the function pointers, the descriptor of how shared memory can be allocated, and the max thread count
     */
    config.entries = entries;
    config.entry_count = sizeof(entries) / sizeof(entries[0]);
    config.shmsrcs = &shmsrc;
    config.shmsrc_count = 1;
    config.max_threads = opts->P;

    return xthread_run(&config, application_main, &context, argc, argv);
}

int initialize_single_source(int*  D,
      int*  Q,
      int   source,
      int   N)
{
   for(int i = 0; i < N; i++)
   {
      D[i] = 0;
      Q[i] = 1;
   }

   D[source] = 1;
   Q[source] = 0;
   return 0;
}


void init_weights(int N, int DEG, int* W_index)
{
   // Initialize to -1
   for(int i = 0; i < N; i++)
      for(int j = 0; j < DEG; j++)
         W_index[i * DEG + j]= -1;

   // Populate Index Array
   for(int i = 0; i < N; i++)
   {
      int last = 0;
      for(int j = 0; j < DEG; j++)
      {
         if(W_index[i * DEG + j] == -1)
         {        
            int neighbor = i+j;
            //W_index[i][j] = i+j;//rand()%(DEG);

            if(neighbor > last)
            {
               W_index[i * DEG + j] = neighbor;
               last = W_index[i * DEG + j];
            }
            else
            {
               if(last < (N-1))
               {
                  W_index[i * DEG + j] = (last + 1);
                  last = W_index[i * DEG + j];
               }
            }
         }
         else
         {
            last = W_index[i * DEG + j];
         }
         if(W_index[i * DEG + j]>=N)
         {
            W_index[i * DEG + j] = N-1;
         }
      }
   }
}
