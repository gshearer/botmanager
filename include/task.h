#ifndef BM_TASK_H
#define BM_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define TASK_NAME_SZ  40

// Opaque handle for cancellable scheduled tasks. Zero is reserved to
// mean "no task"; live tasks receive a non-zero monotonic id at
// creation time. Handles remain usable across the task's lifetime —
// task_cancel of a stale handle is a harmless no-op.
typedef uint64_t task_handle_t;
#define TASK_HANDLE_NONE ((task_handle_t)0)

typedef enum
{
  TASK_WAITING,     // ready to be picked up by a worker
  TASK_RUNNING,     // currently executing
  TASK_SLEEPING,    // waiting for sleep_until to expire
  TASK_ENDED,       // completed successfully
  TASK_FATAL        // failed — triggers shutdown
} task_state_t;

// Controls which workers may execute the task.
typedef enum
{
  TASK_PARENT,      // parent thread only
  TASK_THREAD,      // worker threads only
  TASK_ANY          // any thread
} task_type_t;

// Which slice of the worker pool a task may occupy. Interactive work —
// a command body, with someone waiting on the reply — may use every
// worker there is. Background work is capped at the pool minus
// `core.pool.reserve_interactive`, so no flood of it can leave a
// command with nowhere to run: the resolver alone submits one task per
// lookup and its own cap equals the whole pool.
//
// Background is the default and the common case. A task declares
// itself interactive by setting `lane` between task_create() and
// task_submit(); cmd.c is the only place that does.
typedef enum
{
  TASK_BACKGROUND = 0,
  TASK_INTERACTIVE
} task_lane_t;

// Controls the lifecycle of a task.
typedef enum
{
  TASK_ONCE,        // run, complete, free (default)
  TASK_PERSIST,     // dedicated thread, callback loops until shutdown
  TASK_PERIODIC,    // runs on interval, auto-reschedules after each execution
  TASK_DEFERRED     // runs once after an initial delay
} task_kind_t;

typedef struct task task_t;

// The callback MUST set t->state before returning.
// Valid next states: TASK_WAITING (re-run), TASK_SLEEPING (with
// sleep_until set), TASK_ENDED (done), TASK_FATAL (error).
// For TASK_PERSIST: callback contains its own loop, sets TASK_ENDED
// when done (typically on shutdown).
// For TASK_PERIODIC: TASK_ENDED means "iteration done, reschedule".
// A callback that returns still in TASK_RUNNING is treated as ENDED and
// named in a WARN — the task is off every list by then, so the only
// alternative to freeing it is leaking it. A one-shot body with several
// returns should set the state once, up top, rather than at each of them.
typedef void (*task_cb_t)(task_t *t);

// Dynamically allocated, lives in a sorted ready queue or timer queue
// managed by the task system.
struct task
{
  // Set at creation, read by callback.
  char            name[TASK_NAME_SZ];
  task_type_t     type;
  task_kind_t     kind;             // lifecycle kind (default TASK_ONCE)
  task_lane_t     lane;             // pool slice (default TASK_BACKGROUND)
  uint8_t         priority;         // 0 = highest, 254 = lowest

  // Set by callback before returning, on whatever thread is running it
  // and under no lock of ours; task_iterate() reads both under
  // task_lock (measured, TSan 2026-08-15). An iteration wants a whole
  // value, not a synchronised one — it is describing a task that is
  // running and will have moved on regardless — so _Atomic is the fix
  // and neither the worker nor the walk takes anything extra.
  _Atomic task_state_t state;
  _Atomic time_t  sleep_until;      // for TASK_SLEEPING

  // Kind-specific fields.
  uint32_t        interval_ms;      // for TASK_PERIODIC (milliseconds)

  // Set at creation, used by callback.
  task_cb_t       cb;
  void           *data;

  // Linked task: promoted to WAITING when this task enters ENDED.
  // Set by the caller before task_submit(). Not valid for TASK_PERIODIC.
  task_t         *link;

  // Managed by the task system — read-only for callers.
  task_handle_t   id;               // non-zero monotonic, assigned at creation
  bool            cancelled;        // set by task_cancel; honoured at task_finish
  time_t          created;
  time_t          last_run;
  uint32_t        run_count;
  task_t         *next;             // internal list pointer
};

typedef struct
{
  uint32_t waiting;
  uint32_t running;
  uint32_t sleeping;
  uint32_t linked;
  uint32_t persist;       // active persistent tasks
  uint32_t periodic;      // total periodic tasks (running + sleeping)
  uint32_t total;
} task_stats_t;

// Returns an allocated task in TASK_WAITING state (not yet submitted).
task_t *task_create(const char *name, task_type_t type, uint8_t priority,
    task_cb_t cb, void *data);

// Submit a task to the ready queue. Any descendants reachable via ->link
// are counted in statistics but not queued until their parent completes.
// TASK_DEFERRED tasks are placed in the timer queue if sleep_until is set.
void task_submit(task_t *t);

// Create and submit in one call. ⚠ The returned pointer is only ever
// safe to test against NULL: the task is queued before this returns, so
// a worker may already have run it and freed the struct. Anything that
// needs to name the task afterwards wants a handle — task_add_deferred
// or task_add_periodic.
task_t *task_add(const char *name, task_type_t type, uint8_t priority,
    task_cb_t cb, void *data);

// Create and spawn a persistent task on a dedicated thread.
// The callback must contain its own loop and poll pool_shutting_down().
// Returns a handle, or TASK_HANDLE_NONE on spawn failure. The task_t is
// freed the instant the callback returns, so the handle — not a
// pointer — is what a caller keeps.
task_handle_t task_add_persist(const char *name, uint8_t priority,
    task_cb_t cb, void *data);

// Block until the thread running persist task `h` has left the callback
// and exited, or until timeout_ms elapses; true if it is gone. A
// plugin's stop() must call this before its own code can be unmapped:
// the callback body lives in the plugin's mapping and signalling it to
// finish says nothing about whether it has actually returned.
bool task_persist_join(task_handle_t h, uint32_t timeout_ms);

// Create and submit a periodic task that runs on an interval.
// Callback sets TASK_ENDED to mean "iteration done, reschedule".
// interval_ms minimum effective value: 1000. Returns a cancellation
// handle, or TASK_HANDLE_NONE on submit failure.
task_handle_t task_add_periodic(const char *name, task_type_t type,
    uint8_t priority, uint32_t interval_ms, task_cb_t cb, void *data);

// Create and submit a deferred task (runs once after delay_ms).
// Returns a cancellation handle, or TASK_HANDLE_NONE on submit failure.
task_handle_t task_add_deferred(const char *name, task_type_t type,
    uint8_t priority, uint32_t delay_ms, task_cb_t cb, void *data);

// Cancel a scheduled periodic or deferred task. Safe to call from any
// thread. TASK_HANDLE_NONE is a no-op. If the task is queued (ready or
// timer), it is unlinked and freed immediately. If the task is
// currently running, a flag is set so task_finish treats the next
// TASK_ENDED return as terminal rather than rescheduling. Does not
// block waiting for a running callback to complete.
//
// Returns true only when the task was dequeued and freed WITHOUT ever
// running — which is the answer a caller needs when the task owns a
// reference on the caller's behalf: true means the callback will never
// run and its reference is the caller's to drop; false means it has run
// or is running and has dropped the reference itself.
bool task_cancel(task_handle_t h);

// Returns a task in RUNNING state, or NULL if none available.
// `allow_background` false narrows the walk to interactive tasks: the
// asking worker holds no background slot, so it may only take work that
// does not need one.
task_t *task_assign(task_type_t type, bool allow_background);

// Block until work may be available or timeout_ms expires.
// Promotes expired sleeping tasks internally.
void task_wait(uint32_t timeout_ms);

// Used during shutdown.
void task_wake_all(void);

// Pointer `t` is invalid after ENDED/FATAL for TASK_ONCE.
task_state_t task_finish(task_t *t);

void task_get_stats(task_stats_t *out);

const char *task_state_name(task_state_t s);
const char *task_type_name(task_type_t t);
const char *task_kind_name(task_kind_t k);

// Snapshot of one task, as seen by task_iterate. `cb` and `data` are
// the pointers the queue retains on the submitter's behalf: for a task
// submitted by a plugin they point into that plugin's mapping, which is
// what makes them the quiescence test (see plugin_owns_ptr).
typedef struct
{
  const char  *name;
  task_state_t state;
  task_kind_t  kind;
  task_type_t  type;
  uint8_t      priority;
  uint32_t     run_count;
  uint32_t     interval_ms;
  time_t       created;
  time_t       last_run;
  time_t       sleep_until;
  task_cb_t    cb;
  void        *data;
} task_iter_info_t;

typedef void (*task_iter_cb_t)(const task_iter_info_t *info, void *data);

// Iterate all tasks (running, waiting, and sleeping). The callback runs
// under task_lock — it must be fast and must not re-enter task_*.
void task_iterate(task_iter_cb_t cb, void *data);

// Must be called after mem_init().
void task_init(void);

// Must be called after admin_init() (requires the "show" parent
// command to exist).
void task_register_commands(void);

// Frees all queued and sleeping tasks.
void task_exit(void);

#ifdef TASK_INTERNAL

#include "common.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "alloc.h"

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} task_show_state_t;

// Ready queue: sorted by priority ascending (0 = highest = head).
// Timer queue: sorted by sleep_until ascending (earliest = head).
// Running list: unordered, tracks tasks currently executing.
// All protected by task_lock. Workers block on task_cond.
static pthread_mutex_t task_lock;
static pthread_cond_t  task_cond;
static task_t         *ready_head   = NULL;
static task_t         *timer_head   = NULL;
static task_t         *running_head = NULL;
static task_stats_t    stats;
static bool            task_ready = false;

#endif // TASK_INTERNAL

#endif // BM_TASK_H
