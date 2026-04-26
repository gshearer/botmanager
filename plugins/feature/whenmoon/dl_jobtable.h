// dl_jobtable.h — in-memory whenmoon download-job table.
// Internal; WHENMOON_INTERNAL-gated.
//
// EX-1: this file used to be dl_scheduler.{c,h} and owned a per-bot
// token bucket + dispatch loop. Rate-limited dispatch moved to the
// feature_exchange plugin (plugins/feature/exchange) — what remains
// here is the in-memory job list, persistence, and the
// "kick the dispatch" surface that hands runnable jobs off to
// dl_trades_dispatch_one / dl_candles_dispatch_one. Those, in turn,
// route through coinbase_fetch_*_async, which routes through
// exchange_request().

#ifndef BM_WHENMOON_DL_JOBTABLE_H
#define BM_WHENMOON_DL_JOBTABLE_H

#ifdef WHENMOON_INTERNAL

#include "task.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WM_DL_PAGE_RETRY_MAX      5
#define WM_DL_JOBS_MAX           32

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

  // EX-1: priority for the request queue. Set by the enqueue path
  // (EXCHANGE_PRIO_USER_DOWNLOAD for /whenmoon download …,
  // EXCHANGE_PRIO_MARKET_BACKFILL for system catchup).
  uint8_t          priority;

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
  // coinbase_fetch_*_async at a time. Set true when wm_dl_kick submits,
  // cleared by the completion callback.
  bool             in_flight;

  // DL-1 stall watchdog: wall-clock ms (CLOCK_MONOTONIC-derived) of the
  // last forward-progress event. Updated on dispatch (in wm_dl_kick) and
  // on each successful page in the completion callbacks. The supervisor
  // (dl_supervisor.c) walks RUNNING + in_flight jobs and recovers any
  // whose value is older than WM_DL_STALL_THRESHOLD_MS.
  int64_t          last_progress_ms;

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

  // List linkage (insertion order).
  dl_job_t        *next;
};

// In-memory job table — the post-EX-1 successor of dl_scheduler_t.
// Concurrency cap is still useful so a high-priority transactional flow
// can preempt backfill at the exchange layer; we just don't gate
// dispatch on a token bucket here anymore.
typedef struct dl_jobtable
{
  struct whenmoon_state *st;            // owning plugin
  pthread_mutex_t        lock;
  pthread_cond_t         drain;         // signalled when in_flight drops

  dl_job_t              *jobs_head;
  uint32_t               n_jobs;
  uint32_t               in_flight_count;

  bool                   destroying;    // set by wm_dl_jobtable_destroy
  uint32_t               max_concurrent;
} dl_jobtable_t;

bool wm_dl_jobtable_init(struct whenmoon_state *st);
void wm_dl_jobtable_destroy(struct whenmoon_state *st);

// Command-handler entry points (called on a cmd worker thread).
// On success returns SUCCESS and writes the new job id to *out_job_id;
// on FAIL writes a terse reason into `err`.
//
// `priority` is one of EXCHANGE_PRIO_* (exchange_api.h); it travels
// with the job and is passed to the underlying coinbase_fetch_*_async
// calls so the exchange abstraction routes accordingly.
bool wm_dl_job_enqueue(struct whenmoon_state *st,
    dl_job_kind_t kind, int32_t market_id, int32_t granularity,
    uint8_t priority,
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
// max_concurrent. Caller must hold table->lock.
dl_job_t *wm_dl_jobtable_pick_next(dl_jobtable_t *t);

// Find a job by id. Caller must hold table->lock.
dl_job_t *wm_dl_find_job_locked(dl_jobtable_t *t, int64_t id);

// Walk the job list and submit every runnable job's next page through
// the exchange abstraction. Idempotent; safe to call from any thread.
// Replaces the old wm_dl_dispatch_next: there is no token-take here,
// the abstraction owns the rate-limit decision.
void wm_dl_kick(dl_jobtable_t *t);

// Persist an in-memory job's state + cursor + progress to the DB row.
// Called from completion callbacks on the curl worker thread; uses
// db_query (sync). The completion callback re-enters wm_dl_kick
// afterward to chain to the next page.
void wm_dl_job_persist(dl_jobtable_t *t, const dl_job_t *j);

// Called by a completion callback to release the in-flight slot for
// a job. Signals the drain condvar so wm_dl_jobtable_destroy can
// safely free the list after all outstanding requests complete.
void wm_dl_job_clear_in_flight(dl_jobtable_t *t, dl_job_t *j);

// DL-1: remove DONE / FAILED jobs from the in-memory list and free the
// nodes. Persistence is unchanged — the wm_download_job rows stay so
// /show whenmoon download history can render terminal jobs after the
// fact. Walks the list under t->lock; safe to call from any thread.
// Calls wm_dl_supervisor_check(t) on its way out so the supervisor
// task auto-cancels when the list empties.
void wm_dl_remove_completed(dl_jobtable_t *t);

// DL-1: monotonic millisecond clock used by the supervisor to age
// last_progress_ms. Exposed so dl_supervisor.c can compare against the
// same source the dispatch / completion paths use without each module
// open-coding clock_gettime.
int64_t wm_dl_now_ms(void);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_JOBTABLE_H
