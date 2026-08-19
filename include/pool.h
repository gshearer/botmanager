#ifndef BM_POOL_H
#define BM_POOL_H

#include <stdbool.h>
#include <stdint.h>

#include "task.h"

typedef struct
{
  uint16_t total;         // alive elastic worker threads (not counting parent)
  uint16_t idle;          // elastic workers waiting for work
  uint16_t persist;       // dedicated persist task threads
  uint64_t jobs_completed; // lifetime total tasks executed across all workers
  uint16_t peak_workers;  // high-water mark of worker count
  uint16_t bg_active;     // workers currently running background tasks
  uint16_t bg_budget;     // most that may do so at once
} pool_stats_t;

// Must be called before pool_init(). Defaults: max=64, min=1, spare=1,
// max_idle=300s.
void pool_set_limits(uint16_t max_threads, uint16_t min_threads,
    uint16_t min_spare, uint32_t max_idle_secs);

// Creates min_threads workers. Must be called after task_init().
void pool_init(void);

// Must be called after kv_init() and kv_load(). Updates pool limits
// from the KV store, overriding compiled defaults.
void pool_register_config(void);

// Blocks the calling thread, processing TASK_PARENT tasks until
// shutdown is requested. Must be called from the main thread.
void pool_run_parent(void);

// Wakes all workers so they can exit.
void pool_shutdown(void);

// TASK_PERSIST callbacks should poll this in their loop.
bool pool_shutting_down(void);

// Spawn a dedicated thread for a TASK_PERSIST task. The task runs
// immediately on its own thread, outside the elastic worker pool.
bool pool_spawn_persist(task_t *t);

// Block until the thread running persist task `id` has left the
// callback and exited, or until `timeout_ms` elapses. Returns true once
// the thread is joined — or immediately, if no live slot holds `id`.
// Returns false on timeout, leaving the thread and its slot alone.
//
// Signalling a persist task is not enough for a plugin: the callback
// body lives in the plugin's mapping, so `dlclose` before the thread
// has actually returned unmaps the code under it. This is the join
// that closes that window.
bool pool_join_persist(task_handle_t id, uint32_t timeout_ms);

// Joins all worker threads (elastic and persist) and frees resources.
void pool_exit(void);

void pool_get_stats(pool_stats_t *out);

#ifdef POOL_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"
#include "sig.h"
#include "task.h"

#define POOL_MAX_PERSIST  16

typedef enum
{
  WORKER_UNUSED,              // slot available
  WORKER_RUNNING,             // thread is alive
  WORKER_RETIRING,            // thread exited, needs join
  WORKER_JOINING              // a joiner owns this slot's thread
} worker_state_t;

typedef struct
// jobs, last_active and wstate are the three a worker updates about
// itself with no lock -- per task completion in worker_entry() and
// pool_run_parent() -- while pool_get_stats() and the persist reapers
// read them under pool_mutex (measured, TSan 2026-08-15). Everything
// else here is written only under that lock.
{
  uint8_t                id;
  pthread_t              thread;
  _Atomic uint64_t       jobs;
  time_t                 created;
  _Atomic time_t         last_active;
  _Atomic worker_state_t wstate;
  bool                   idle;    // true while in task_wait
  task_handle_t          task_id; // persist slots only; what pool_join_persist matches
} worker_t;

// _Atomic for the same reason as sock_cfg_t / curl_cfg_t: pool_load_config()
// runs on a command thread while workers read these in should_retire()
// (measured, TSan 2026-08-15).
typedef struct
{
  _Atomic uint16_t max_threads;
  _Atomic uint16_t min_threads;
  _Atomic uint16_t min_spare;
  _Atomic uint32_t max_idle_secs;
  _Atomic uint32_t wait_ms;
  _Atomic uint16_t reserve;      // workers background work may not occupy
  _Atomic uint16_t bg_budget;    // derived: max_threads - reserve, never 0
} pool_cfg_t;

static pool_cfg_t pool_cfg = {
  .max_threads   = 64,
  .min_threads   = 1,
  .min_spare     = 1,
  .max_idle_secs = 300,
  .wait_ms       = 1000,
  .reserve       = 8,
  .bg_budget     = 56,
};

static worker_t        *workers = NULL;
static pthread_mutex_t  pool_mutex;
static uint16_t         pool_size = 0;    // alive elastic workers (not parent)
static uint16_t         pool_idle = 0;    // elastic workers in task_wait
static uint16_t         pool_peak = 0;    // high-water mark of pool_size
static uint16_t         pool_bg   = 0;    // elastic workers on background work
// _Atomic for the same reason as pool_cfg_t above, and it is the widest
// instance of it in the tree: pool_shutdown() writes this on whichever
// thread asked to stop, while every elastic worker loop, the parent
// loop and every TASK_PERSIST callback in the daemon read it through
// pool_shutting_down() (measured, TSan 2026-08-17). A one-word flag is
// a knob like any other -- the reader gets the old value or the new
// one, and pays at most one more poll interval for the old one.
static _Atomic bool     pool_stopping = false;

// Not atomic on purpose: written by pool_init() / pool_exit() and read
// by pool_exit(), all on the main thread.
static bool             pool_ready = false;

// Separate from elastic pool.
static worker_t  persist_workers[POOL_MAX_PERSIST];
static uint16_t  persist_count = 0;

static bool  spawn_worker_locked(void);
static void *worker_entry(void *arg);
static void *persist_entry(void *arg);
static void  persist_retire_self(void);
static void  reap_persist_locked(void);

#endif // POOL_INTERNAL

#endif // BM_POOL_H
