#ifndef BM_TASK_H
#define BM_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define TASK_NAME_SZ  40

// Task states.
typedef enum
{
  TASK_WAITING,     // ready to be picked up by a worker
  TASK_RUNNING,     // currently executing
  TASK_SLEEPING,    // waiting for sleep_until to expire
  TASK_ENDED,       // completed successfully
  TASK_FATAL        // failed — triggers shutdown
} task_state_t;

// Task types — controls which workers may execute the task.
typedef enum
{
  TASK_PARENT,      // parent thread only
  TASK_THREAD,      // worker threads only
  TASK_ANY          // any thread
} task_type_t;

// Task kinds — controls the lifecycle of a task.
typedef enum
{
  TASK_ONCE,        // run, complete, free (default)
  TASK_PERSIST,     // dedicated thread, callback loops until shutdown
  TASK_PERIODIC,    // runs on interval, auto-reschedules after each execution
  TASK_DEFERRED     // runs once after an initial delay
} task_kind_t;

typedef struct task task_t;

// Task callback. The callback MUST set t->state before returning.
// Valid next states: TASK_WAITING (re-run), TASK_SLEEPING (with
// sleep_until set), TASK_ENDED (done), TASK_FATAL (error).
// For TASK_PERSIST: callback contains its own loop, sets TASK_ENDED
// when done (typically on shutdown).
// For TASK_PERIODIC: TASK_ENDED means "iteration done, reschedule".
typedef void (*task_cb_t)(task_t *t);

// Task structure. Dynamically allocated, lives in a sorted ready queue
// or timer queue managed by the task system.
struct task
{
  // Set at creation, read by callback.
  char            name[TASK_NAME_SZ];
  task_type_t     type;
  task_kind_t     kind;             // lifecycle kind (default TASK_ONCE)
  uint8_t         priority;         // 0 = highest, 254 = lowest

  // Set by callback before returning.
  task_state_t    state;
  time_t          sleep_until;      // for TASK_SLEEPING

  // Kind-specific fields.
  uint32_t        interval_ms;      // for TASK_PERIODIC (milliseconds)

  // Set at creation, used by callback.
  task_cb_t       cb;
  void           *data;

  // Linked task: promoted to WAITING when this task enters ENDED.
  // Set by the caller before task_submit(). Not valid for TASK_PERIODIC.
  task_t         *link;

  // Managed by the task system — read-only for callers.
  time_t          created;
  time_t          last_run;
  uint32_t        run_count;
  task_t         *next;             // internal list pointer
};

// Task statistics (thread-safe snapshot).
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

// returns: allocated task in TASK_WAITING state (not yet submitted)
// name: human-readable task name
// type: which workers may execute this task
// priority: 0 = highest, 254 = lowest
// cb: callback function
// data: opaque user data passed to callback
task_t *task_create(const char *name, task_type_t type, uint8_t priority,
    task_cb_t cb, void *data);

// Submit a task to the ready queue. Any descendants reachable via ->link
// are counted in statistics but not queued until their parent completes.
// TASK_DEFERRED tasks are placed in the timer queue if sleep_until is set.
// t: task to submit
void task_submit(task_t *t);

// returns: submitted task pointer
// name: human-readable task name
// type: which workers may execute this task
// priority: 0 = highest, 254 = lowest
// cb: callback function
// data: opaque user data passed to callback
task_t *task_add(const char *name, task_type_t type, uint8_t priority,
    task_cb_t cb, void *data);

// Create and spawn a persistent task on a dedicated thread.
// The callback must contain its own loop and poll pool_shutting_down().
// returns: task pointer, or NULL on failure
// name: human-readable task name
// priority: 0 = highest, 254 = lowest
// cb: callback (loops internally, sets TASK_ENDED when done)
// data: opaque user data
task_t *task_add_persist(const char *name, uint8_t priority,
    task_cb_t cb, void *data);

// Create and submit a periodic task that runs on an interval.
// Callback sets TASK_ENDED to mean "iteration done, reschedule".
// returns: submitted task pointer
// name: human-readable task name
// type: which workers may execute (TASK_THREAD or TASK_ANY)
// priority: 0 = highest, 254 = lowest
// interval_ms: milliseconds between executions (minimum effective: 1000)
// cb: callback function
// data: opaque user data
task_t *task_add_periodic(const char *name, task_type_t type,
    uint8_t priority, uint32_t interval_ms, task_cb_t cb, void *data);

// Create and submit a deferred task (runs once after a delay).
// returns: submitted task pointer
// name: human-readable task name
// type: which workers may execute this task
// priority: 0 = highest, 254 = lowest
// delay_ms: milliseconds before the task becomes eligible to run
// cb: callback function
// data: opaque user data
task_t *task_add_deferred(const char *name, task_type_t type,
    uint8_t priority, uint32_t delay_ms, task_cb_t cb, void *data);

// returns: task in RUNNING state, or NULL if none available
// type: task type to match against
task_t *task_assign(task_type_t type);

// Block until work may be available or timeout_ms expires.
// Promotes expired sleeping tasks internally.
// timeout_ms: maximum milliseconds to wait
void task_wait(uint32_t timeout_ms);

// Wake all threads waiting in task_wait(). Used during shutdown.
void task_wake_all(void);

// returns: the state set by the callback
// t: task to finish (pointer invalid after ENDED/FATAL for TASK_ONCE)
task_state_t task_finish(task_t *t);

// Get current task statistics (thread-safe snapshot).
// out: destination for the snapshot
void task_get_stats(task_stats_t *out);

// returns: human-readable name of a task state
// s: task state enum value
const char *task_state_name(task_state_t s);

// returns: human-readable name of a task type
// t: task type enum value
const char *task_type_name(task_type_t t);

// returns: human-readable name of a task kind
// k: task kind enum value
const char *task_kind_name(task_kind_t k);

// Callback for task iteration.
// name: task name
// state: current task state
// kind: task kind (once, persist, periodic, deferred)
// type: task type (parent, thread, any)
// priority: task priority (0 = highest)
// run_count: number of times the task has been executed
// interval_ms: interval for periodic tasks (0 otherwise)
// created: task creation timestamp
// last_run: last execution timestamp (0 if never run)
// sleep_until: wakeup time for sleeping tasks (0 otherwise)
// data: opaque user data
typedef void (*task_iter_cb_t)(const char *name, task_state_t state,
    task_kind_t kind, task_type_t type, uint8_t priority,
    uint32_t run_count, uint32_t interval_ms, time_t created,
    time_t last_run, time_t sleep_until, void *data);

// Iterate all tasks (running, waiting, and sleeping).
// cb: callback invoked for each task
// data: opaque user data passed to cb
void task_iterate(task_iter_cb_t cb, void *data);

// Initialize the task system. Must be called after mem_init().
void task_init(void);

// Register task subsystem commands. Must be called after admin_init()
// (requires the "show" parent command to exist).
void task_register_commands(void);

// Shut down the task system. Frees all queued and sleeping tasks.
void task_exit(void);

#ifdef TASK_INTERNAL

#include "common.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "mem.h"

// Iteration state for "show tasks" command.
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
