#ifndef BM_POOL_H
#define BM_POOL_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration (defined in task.h).
typedef struct task task_t;

// Pool statistics (thread-safe snapshot).
typedef struct
{
  uint16_t total;         // alive elastic worker threads (not counting parent)
  uint16_t idle;          // elastic workers waiting for work
  uint16_t persist;       // dedicated persist task threads
  uint64_t jobs_completed; // lifetime total tasks executed across all workers
  uint16_t peak_workers;  // high-water mark of worker count
} pool_stats_t;

// Override pool limits. Must be called before pool_init().
// Defaults: max=64, min=1, spare=1, max_idle=300s.
// max_threads: maximum worker threads
// min_threads: minimum worker threads
// min_spare: minimum idle workers before scaling up
// max_idle_secs: seconds before an idle worker retires
void pool_set_limits(uint16_t max_threads, uint16_t min_threads,
    uint16_t min_spare, uint32_t max_idle_secs);

// Initialize the thread pool. Creates min_threads workers.
// Must be called after task_init().
void pool_init(void);

// Register KV configuration keys and load values from the database.
// Must be called after kv_init() and kv_load(). Updates pool limits
// from the KV store, overriding compiled defaults.
void pool_register_config(void);

// Enter the parent worker loop. Blocks the calling thread, processing
// TASK_PARENT tasks until shutdown is requested. Must be called from
// the main thread.
void pool_run_parent(void);

// Request pool shutdown. Wakes all workers so they can exit.
void pool_shutdown(void);

// returns: true if pool shutdown has been requested.
// TASK_PERSIST callbacks should poll this in their loop.
bool pool_shutting_down(void);

// Spawn a dedicated thread for a TASK_PERSIST task. The task runs
// immediately on its own thread, outside the elastic worker pool.
// returns: true on success
// t: task with kind == TASK_PERSIST
bool pool_spawn_persist(task_t *t);

// Shut down and clean up the thread pool. Joins all worker threads
// (elastic and persist) and frees resources.
void pool_exit(void);

// Get pool statistics (thread-safe snapshot).
// out: destination for the snapshot
void pool_get_stats(pool_stats_t *out);

#ifdef POOL_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "mem.h"
#include "sig.h"
#include "task.h"

#define POOL_MAX_PERSIST  16      // maximum persistent task threads

// Worker states (internal to pool).
typedef enum
{
  WORKER_UNUSED,              // slot available
  WORKER_RUNNING,             // thread is alive
  WORKER_RETIRING             // thread exited, needs join
} worker_state_t;

typedef struct
{
  uint8_t        id;
  pthread_t      thread;
  uint64_t       jobs;
  time_t         created;
  time_t         last_active;
  worker_state_t wstate;
  bool           idle;        // true while in task_wait
} worker_t;

// Pool configuration.
typedef struct
{
  uint16_t max_threads;
  uint16_t min_threads;
  uint16_t min_spare;
  uint32_t max_idle_secs;
  uint32_t wait_ms;
} pool_cfg_t;

static pool_cfg_t pool_cfg = {
  .max_threads   = 64,
  .min_threads   = 1,
  .min_spare     = 1,
  .max_idle_secs = 300,
  .wait_ms       = 1000,
};

static worker_t        *workers = NULL;
static pthread_mutex_t  pool_mutex;
static uint16_t         pool_size = 0;    // alive elastic workers (not parent)
static uint16_t         pool_idle = 0;    // elastic workers in task_wait
static uint16_t         pool_peak = 0;    // high-water mark of pool_size
static bool             pool_stopping = false;
static bool             pool_ready = false;

// Persist task threads — separate from elastic pool.
static worker_t  persist_workers[POOL_MAX_PERSIST];
static uint16_t  persist_count = 0;

// Forward declarations.
static bool  spawn_worker_locked(void);
static void *worker_entry(void *arg);
static void *persist_entry(void *arg);

#endif // POOL_INTERNAL

#endif // BM_POOL_H
