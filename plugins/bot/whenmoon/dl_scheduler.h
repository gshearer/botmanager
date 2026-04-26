// dl_scheduler.h — per-bot download scheduler for whenmoon.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_DL_SCHEDULER_H
#define BM_WHENMOON_DL_SCHEDULER_H

#ifdef WHENMOON_INTERNAL

#include "task.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WM_DL_TICK_MS          1000
#define WM_DL_DEFAULT_RPS         8
#define WM_DL_BURST_MULT        1.5
#define WM_DL_PAGE_RETRY_MAX      5
#define WM_DL_JOBS_MAX           32

// Max rps the scheduler accepts; Coinbase public API cap.
#define WM_DL_RPS_MIN             1
#define WM_DL_RPS_MAX            10

typedef enum
{
  DL_JOB_QUEUED,
  DL_JOB_RUNNING,
  DL_JOB_PAUSED,
  DL_JOB_DONE,
  DL_JOB_FAILED
} dl_job_state_t;

typedef enum
{
  DL_JOB_TRADES,
  DL_JOB_CANDLES
} dl_job_kind_t;

struct whenmoon_state;

typedef struct dl_job dl_job_t;
struct dl_job
{
  int64_t          id;                  // wm_download_job.id
  int32_t          market_id;
  dl_job_kind_t    kind;
  int32_t          granularity;         // candles only; 0 for trades

  // User-provided window. Empty strings mean "no bound".
  //   trades oldest_ts  = "" -> walk to exchange inception
  //   trades newest_ts  = "" -> start at "now"
  char             oldest_ts[40];
  char             newest_ts[40];

  dl_job_state_t   state;

  // Pagination cursor:
  //   trades  — cursor_after is the trade_id for the NEXT ?after= call
  //             (0 = first page).
  //   candles — cursor_end_ts is the exclusive end bound for the next
  //             300-bucket window (initial = newest_ts).
  int64_t          cursor_after;
  char             cursor_end_ts[40];

  // Progress counters. Persisted on each page.
  int32_t          pages_fetched;
  int64_t          rows_written;

  // Failure accounting.
  int              consecutive_errors;
  char             last_err[256];

  // Outstanding-request guard: a job may have at most one in-flight
  // coinbase_fetch_*_async at a time. Set true when dispatch submits,
  // cleared by the completion callback. Prevents the tick from
  // double-dispatching while a prior page is still in transit.
  bool             in_flight;

  // Lazy per-pair DDL marker for candles. Parallel to `table_ensured`
  // but tracked separately because the candle table name includes the
  // granularity suffix.
  bool             candle_table_ensured;

  // Cached exchange symbol used in the hot loop (e.g. "BTC-USD").
  char             exchange_symbol[32];
  char             exchange[32];

  // Lazy per-pair DDL marker — the trade table is ensured once per
  // job lifetime instead of on every page.
  bool             table_ensured;

  // Round-robin cursor for wm_dl_sched_pick_next; caller rotates after
  // picking.
  dl_job_t        *next;
};

typedef struct dl_scheduler
{
  struct whenmoon_state *st;            // owning bot
  pthread_mutex_t        lock;
  pthread_cond_t         drain;         // signalled when in_flight drops

  dl_job_t              *jobs_head;
  uint32_t               n_jobs;
  uint32_t               in_flight_count;

  // Token bucket.
  double                 tokens;
  double                 tokens_cap;
  double                 tokens_per_sec;
  struct timespec        last_refill;

  // Scheduler state.
  task_handle_t          tick_task;     // TASK_HANDLE_NONE when idle
  bool                   enabled;
  bool                   destroying;    // set by wm_dl_scheduler_destroy
  uint32_t               max_concurrent;
} dl_scheduler_t;

bool wm_dl_scheduler_init(struct whenmoon_state *st);
void wm_dl_scheduler_destroy(struct whenmoon_state *st);

// Command-handler entry points (called on a cmd worker thread).
// On success returns SUCCESS and writes the new job id to *out_job_id;
// on FAIL writes a terse reason into `err`.
bool wm_dl_job_enqueue(struct whenmoon_state *st,
    dl_job_kind_t kind, int32_t market_id, int32_t granularity,
    const char *exchange, const char *exchange_symbol,
    const char *oldest_ts, const char *newest_ts,
    const char *requested_by, int64_t *out_job_id,
    char *err, size_t err_cap);

bool wm_dl_job_cancel(struct whenmoon_state *st, int64_t job_id,
    char *err, size_t err_cap);

typedef void (*wm_dl_job_iter_cb_t)(const dl_job_t *j, void *user);

// Walks a snapshot copy of the in-memory job list with no lock held
// across the callback.
void wm_dl_job_list_iterate(struct whenmoon_state *st,
    wm_dl_job_iter_cb_t cb, void *user);

// Picks the next RUNNING job not currently in flight, honouring
// max_concurrent. Caller must hold sched->lock.
dl_job_t *wm_dl_sched_pick_next(dl_scheduler_t *s);

// Find a job by id. Caller must hold sched->lock.
dl_job_t *wm_dl_find_job_locked(dl_scheduler_t *s, int64_t id);

// Token-bucket primitives.
void wm_dl_bucket_refill(dl_scheduler_t *s);
bool wm_dl_bucket_take(dl_scheduler_t *s);

// Persist an in-memory job's state + cursor + progress to the DB row.
// Called from completion callbacks on the curl worker thread; uses
// db_query (sync) — the worker already ate a token for this cycle and
// the next page dispatch goes through the tick, not this thread.
void wm_dl_job_persist(dl_scheduler_t *s, const dl_job_t *j);

// Called by a completion callback to release the in-flight slot for
// a job. Signals the drain condvar so wm_dl_scheduler_destroy can
// safely free the list after all outstanding requests complete.
void wm_dl_job_clear_in_flight(dl_scheduler_t *s, dl_job_t *j);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_SCHEDULER_H
