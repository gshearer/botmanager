// botmanager — MIT
// Whenmoon download-job table: in-memory list + DB persistence + the
// "kick" surface that hands runnable jobs off to the per-kind
// dispatch_one helpers.
//
// EX-1: the per-bot token bucket retired. Rate limiting + 429/5xx
// retry now live in the feature_exchange plugin
// (plugins/feature/exchange/exchange_request.c). What remains here is
// purely job-table bookkeeping; wm_dl_kick walks the list and submits
// any runnable job's next page through the exchange abstraction.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_jobtable.h"
#include "dl_schema.h"
#include "dl_supervisor.h"
#include "dl_trades.h"
#include "dl_candles.h"
#include "dl_coverage.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"
#include "kv.h"
#include "pool.h"
#include "task.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------------ //
// Forward decls                                                       //
// ------------------------------------------------------------------ //

static bool wm_dl_load_jobs(dl_jobtable_t *t);
static bool wm_dl_insert_job(dl_job_kind_t kind,
    int32_t market_id, int32_t granularity, const char *oldest_ts,
    const char *newest_ts, const char *requested_by, int64_t *out_id);
static void wm_dl_job_list_append_locked(dl_jobtable_t *t, dl_job_t *j);
static bool wm_dl_has_running_trades_for_market_locked(dl_jobtable_t *t,
    int32_t market_id);
static const char *wm_dl_state_str(dl_job_state_t s);
static dl_job_state_t wm_dl_state_from_str(const char *s);
static void wm_dl_free_job_list(dl_job_t *head);

// ------------------------------------------------------------------ //
// Monotonic ms helper                                                 //
// ------------------------------------------------------------------ //

int64_t
wm_dl_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return((int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000));
}

// ------------------------------------------------------------------ //
// State <-> SQL mapping                                               //
// ------------------------------------------------------------------ //

static const char *
wm_dl_state_str(dl_job_state_t s)
{
  switch(s)
  {
    case DL_JOB_QUEUED:  return("queued");
    case DL_JOB_RUNNING: return("running");
    case DL_JOB_PAUSED:  return("paused");
    case DL_JOB_DONE:    return("done");
    case DL_JOB_FAILED:  return("failed");
    default:             return("queued");
  }
}

static dl_job_state_t
wm_dl_state_from_str(const char *s)
{
  if(s == NULL)
    return(DL_JOB_QUEUED);

  if(strcmp(s, "running") == 0) return(DL_JOB_RUNNING);
  if(strcmp(s, "paused")  == 0) return(DL_JOB_PAUSED);
  if(strcmp(s, "done")    == 0) return(DL_JOB_DONE);
  if(strcmp(s, "failed")  == 0) return(DL_JOB_FAILED);

  return(DL_JOB_QUEUED);
}

// ------------------------------------------------------------------ //
// Pick policy                                                         //
// ------------------------------------------------------------------ //

dl_job_t *
wm_dl_jobtable_pick_next(dl_jobtable_t *t)
{
  dl_job_t *j;
  uint32_t  running = 0;

  // Count running slots in use so we honour max_concurrent.
  for(j = t->jobs_head; j != NULL; j = j->next)
    if(j->state == DL_JOB_RUNNING && j->in_flight)
      running++;

  if(running >= t->max_concurrent)
  {
#ifdef WM_DL_TRACE_PICK
    clam(CLAM_DEBUG2, WM_DL_CTX,
        "pick: cap reached running=%u max=%u",
        running, t->max_concurrent);
#endif
    return(NULL);
  }

  // Walk the list once picking the first RUNNING && !in_flight job;
  // promote queued jobs to RUNNING as slots open.
  for(j = t->jobs_head; j != NULL; j = j->next)
  {
    if(j->state == DL_JOB_RUNNING && !j->in_flight)
    {
#ifdef WM_DL_TRACE_PICK
      clam(CLAM_DEBUG2, WM_DL_CTX,
          "pick: running id=%" PRId64 " consecutive_errors=%d",
          j->id, j->consecutive_errors);
#endif
      return(j);
    }
  }

  // No running job ready — promote a queued one if there is slack.
  for(j = t->jobs_head; j != NULL; j = j->next)
  {
    if(j->state == DL_JOB_QUEUED && running < t->max_concurrent)
    {
      j->state = DL_JOB_RUNNING;
#ifdef WM_DL_TRACE_PICK
      clam(CLAM_DEBUG2, WM_DL_CTX,
          "pick: promote id=%" PRId64 " QUEUED->RUNNING running=%u/%u",
          j->id, running, t->max_concurrent);
#endif
      return(j);
    }
  }

#ifdef WM_DL_TRACE_PICK
  clam(CLAM_DEBUG2, WM_DL_CTX,
      "pick: empty (running=%u/%u, n_jobs=%u)",
      running, t->max_concurrent, t->n_jobs);
#endif
  return(NULL);
}

// WM-DL-RACE-1: fold a synchronous-dispatch failure into the same
// retry budget the async error path uses (consecutive_errors +
// last_err + DL_JOB_FAILED at the cap), persist the new state to the
// DB row, and reap the in-memory entry on terminal failure. Without
// this, a silent dispatch_one FAIL leaves the job parked in RUNNING+
// !in_flight with no DB error trail. Manages t->lock internally; the
// snapshot pattern mirrors wm_dl_job_cancel so persist runs outside
// the lock without racing wm_dl_jobtable_destroy.
void
wm_dl_record_dispatch_error(dl_jobtable_t *t, dl_job_t *j,
    const char *errmsg)
{
  dl_job_t snap;
  bool     became_failed = false;

  if(t == NULL || j == NULL)
    return;

  pthread_mutex_lock(&t->lock);

  j->consecutive_errors++;
  snprintf(j->last_err, sizeof(j->last_err), "%s",
      errmsg != NULL ? errmsg : "dispatch_one sync fail");

  if(j->consecutive_errors >= WM_DL_PAGE_RETRY_MAX)
  {
    j->state      = DL_JOB_FAILED;
    became_failed = true;
  }

  snap = *j;
  snap.next = NULL;

  pthread_mutex_unlock(&t->lock);

  clam(CLAM_WARN, WM_DL_CTX,
      "dispatch sync-fail job=%" PRId64 " errs=%d/%d state=%s reason=\"%s\"",
      snap.id, snap.consecutive_errors, WM_DL_PAGE_RETRY_MAX,
      wm_dl_state_str(snap.state), snap.last_err);

  wm_dl_job_persist(t, &snap);

  if(became_failed)
    wm_dl_job_clear_in_flight(t, j);
}

dl_job_t *
wm_dl_find_job_locked(dl_jobtable_t *t, int64_t id)
{
  dl_job_t *j;

  for(j = t->jobs_head; j != NULL; j = j->next)
    if(j->id == id)
      return(j);

  return(NULL);
}

// ------------------------------------------------------------------ //
// Kick: walk job list, submit next page for every runnable job        //
// ------------------------------------------------------------------ //
//
// Replaces the old wm_dl_dispatch_next. There is no token-take here —
// the exchange abstraction owns the rate-limit decision. We just keep
// firing dispatch_one until the concurrency cap is reached or no jobs
// are eligible. The abstraction will queue the requests up beyond its
// burst depth and dispatch them as tokens refill.

void
wm_dl_kick(dl_jobtable_t *t)
{
  dl_job_t *j;
  bool      submitted;

  if(t == NULL || t->destroying || pool_shutting_down())
    return;

  for(;;)
  {
    pthread_mutex_lock(&t->lock);
    j = wm_dl_jobtable_pick_next(t);

    if(j != NULL)
    {
      j->in_flight        = true;
      j->last_progress_ms = wm_dl_now_ms();
      t->in_flight_count++;
      WM_FS_TRACE_INFLIGHT("kick", j->id, +1, t->in_flight_count);
    }

    pthread_mutex_unlock(&t->lock);

    if(j == NULL)
      return;

    // Dispatch off-lock. The async REST submit's completion callback
    // (wm_dl_trades_on_page / wm_dl_candles_on_page) will clear
    // in_flight, commit rows, and re-enter wm_dl_kick to fire the
    // next page in the chain.
    if(j->kind == DL_JOB_TRADES)
      submitted = (wm_dl_trades_dispatch_one(t, j) == SUCCESS);

    else if(j->kind == DL_JOB_CANDLES)
      submitted = (wm_dl_candles_dispatch_one(t, j) == SUCCESS);

    else
    {
      clam(CLAM_WARN, WM_DL_CTX,
          "kick: unknown job kind %d for job %" PRId64,
          (int)j->kind, j->id);
      submitted = false;
    }

    if(!submitted)
    {
      // Synchronous submit failure — clear the slot and stop the
      // walk; a deterministic synchronous failure would otherwise
      // spin without bound on the same job. The dispatch_one's own
      // FAIL paths now advance the job's consecutive_errors via
      // wm_dl_record_dispatch_error (or via the coinbase shim's
      // synchronous cb_deliver_*_fail when applicable), so within
      // WM_DL_PAGE_RETRY_MAX kicks the job FAILs and gets reaped. The
      // next event in the system (a completion callback, the next
      // /whenmoon enqueue, or a supervisor tick spotting a RUNNING+
      // !in_flight zombie) re-enters kick to drive the next attempt.
      wm_dl_job_clear_in_flight(t, j);
      return;
    }
  }
}

void
wm_dl_job_clear_in_flight(dl_jobtable_t *t, dl_job_t *j)
{
  pthread_mutex_lock(&t->lock);

  if(j->in_flight)
  {
    j->in_flight = false;

    if(t->in_flight_count > 0)
      t->in_flight_count--;

    WM_FS_TRACE_INFLIGHT("clear_in_flight", j->id, -1, t->in_flight_count);
  }

  pthread_cond_broadcast(&t->drain);
  pthread_mutex_unlock(&t->lock);

  // DL-1: every completion callback (terminal page or bail path) ends
  // up here. Opportunistically reap any DONE / FAILED jobs that are no
  // longer in flight — including jobs cancelled while a request was
  // outstanding, where this is the moment the in-flight guard finally
  // drops. Cheap when the list is small (cap 32) and a no-op when no
  // job is reapable.
  wm_dl_remove_completed(t);
}

// ------------------------------------------------------------------ //
// Load persisted jobs                                                 //
// ------------------------------------------------------------------ //

static bool
wm_dl_load_jobs(dl_jobtable_t *t)
{
  db_result_t *res = NULL;
  bool         ok = FAIL;
  uint32_t     loaded = 0;

  // Pull core job columns + joined market fields so we can populate
  // exchange_symbol without a second query per job.
  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(
         "SELECT j.id, j.market_id, j.kind, COALESCE(j.granularity, 0),"
         "       COALESCE(to_char(j.oldest_ts AT TIME ZONE 'UTC',"
         "                        'YYYY-MM-DD HH24:MI:SS') || '+00', ''),"
         "       COALESCE(to_char(j.newest_ts AT TIME ZONE 'UTC',"
         "                        'YYYY-MM-DD HH24:MI:SS') || '+00', ''),"
         "       j.state,"
         "       COALESCE(j.cursor_after, 0),"
         "       COALESCE(to_char(j.cursor_end_ts AT TIME ZONE 'UTC',"
         "                        'YYYY-MM-DD HH24:MI:SS') || '+00', ''),"
         "       j.pages_fetched, j.rows_written,"
         "       COALESCE(j.last_err, ''),"
         "       m.exchange, m.exchange_symbol,"
         "       j.last_progress_ms"
         "  FROM wm_download_job j"
         "  JOIN wm_market m ON m.id = j.market_id"
         " WHERE j.state IN ('queued', 'running')"
         " ORDER BY j.id", res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX, "job load failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    goto out;
  }

  pthread_mutex_lock(&t->lock);

  for(uint32_t i = 0; i < res->rows && t->n_jobs < WM_DL_JOBS_MAX; i++)
  {
    dl_job_t   *j;
    const char *v;

    j = mem_alloc("whenmoon.dl", "job", sizeof(*j));

    if(j == NULL)
      break;

    memset(j, 0, sizeof(*j));

    v = db_result_get(res, i, 0);
    if(v != NULL) j->id = (int64_t)strtoll(v, NULL, 10);

    v = db_result_get(res, i, 1);
    if(v != NULL) j->market_id = (int32_t)strtol(v, NULL, 10);

    v = db_result_get(res, i, 2);
    j->kind = (v != NULL && strcmp(v, "candles") == 0)
        ? DL_JOB_CANDLES : DL_JOB_TRADES;

    v = db_result_get(res, i, 3);
    if(v != NULL) j->granularity = (int32_t)strtol(v, NULL, 10);

    v = db_result_get(res, i, 4);
    if(v != NULL) snprintf(j->oldest_ts, sizeof(j->oldest_ts), "%s", v);

    v = db_result_get(res, i, 5);
    if(v != NULL) snprintf(j->newest_ts, sizeof(j->newest_ts), "%s", v);

    v = db_result_get(res, i, 6);
    j->state = wm_dl_state_from_str(v);

    v = db_result_get(res, i, 7);
    if(v != NULL) j->cursor_after = (int64_t)strtoll(v, NULL, 10);

    v = db_result_get(res, i, 8);
    if(v != NULL)
      snprintf(j->cursor_end_ts, sizeof(j->cursor_end_ts), "%s", v);

    v = db_result_get(res, i, 9);
    if(v != NULL) j->pages_fetched = (int32_t)strtol(v, NULL, 10);

    v = db_result_get(res, i, 10);
    if(v != NULL) j->rows_written = (int64_t)strtoll(v, NULL, 10);

    v = db_result_get(res, i, 11);
    if(v != NULL) snprintf(j->last_err, sizeof(j->last_err), "%s", v);

    v = db_result_get(res, i, 12);
    if(v != NULL) snprintf(j->exchange, sizeof(j->exchange), "%s", v);

    v = db_result_get(res, i, 13);
    if(v != NULL)
      snprintf(j->exchange_symbol, sizeof(j->exchange_symbol), "%s", v);

    v = db_result_get(res, i, 14);
    if(v != NULL) j->last_progress_ms = (int64_t)strtoll(v, NULL, 10);

    // Restored jobs default to backfill priority — they survived a
    // restart, the user is not waiting on the originating prompt.
    // /whenmoon download cancel / requeue is the operator's path back
    // to a higher-priority retry.
    j->priority = 50;   // EXCHANGE_PRIO_MARKET_BACKFILL

    // QUEUED rows get promoted by the kick via pick_next. RUNNING rows
    // that survived the orphan reset shouldn't exist — but be forgiving
    // and treat them as queued.
    if(j->state == DL_JOB_RUNNING)
      j->state = DL_JOB_QUEUED;

    wm_dl_job_list_append_locked(t, j);
    loaded++;
  }

  pthread_mutex_unlock(&t->lock);

  ok = SUCCESS;

  if(loaded > 0)
    clam(CLAM_INFO, WM_DL_CTX,
        "resumed %u job%s from persisted state",
        loaded, loaded == 1 ? "" : "s");

out:
  if(res != NULL) db_result_free(res);

  return(ok);
}

// ------------------------------------------------------------------ //
// In-memory job list helpers                                          //
// ------------------------------------------------------------------ //

static void
wm_dl_job_list_append_locked(dl_jobtable_t *t, dl_job_t *j)
{
  dl_job_t **pp = &t->jobs_head;

  while(*pp != NULL)
    pp = &(*pp)->next;

  *pp = j;
  j->next = NULL;
  t->n_jobs++;
}

static bool
wm_dl_has_running_trades_for_market_locked(dl_jobtable_t *t,
    int32_t market_id)
{
  dl_job_t *j;

  for(j = t->jobs_head; j != NULL; j = j->next)
  {
    if(j->market_id != market_id || j->kind != DL_JOB_TRADES)
      continue;

    if(j->state == DL_JOB_QUEUED || j->state == DL_JOB_RUNNING)
      return(true);
  }

  return(false);
}

static void
wm_dl_free_job_list(dl_job_t *head)
{
  while(head != NULL)
  {
    dl_job_t *next = head->next;

    mem_free(head);
    head = next;
  }
}

// DL-1: unlink + free every DONE / FAILED job that isn't currently
// in_flight. The DB row stays so /show whenmoon download history can
// still render terminal jobs after the fact. After removal, we kick the
// supervisor's empty-list check so it auto-disarms when nothing is
// left.
void
wm_dl_remove_completed(dl_jobtable_t *t)
{
  dl_job_t *removed = NULL;

  if(t == NULL)
    return;

  pthread_mutex_lock(&t->lock);

  {
    dl_job_t **pp = &t->jobs_head;

    while(*pp != NULL)
    {
      dl_job_t *j = *pp;

      if((j->state == DL_JOB_DONE || j->state == DL_JOB_FAILED) &&
         !j->in_flight)
      {
        *pp = j->next;

        // Splice onto the local removed list (no order guarantees;
        // we only need to free off-lock).
        j->next = removed;
        removed = j;

        if(t->n_jobs > 0)
          t->n_jobs--;

        continue;
      }

      pp = &(*pp)->next;
    }
  }

  pthread_mutex_unlock(&t->lock);

  wm_dl_free_job_list(removed);

  // Propagate the empty-list edge to the supervisor.
  wm_dl_supervisor_check(t);
}

// ------------------------------------------------------------------ //
// DB persist                                                          //
// ------------------------------------------------------------------ //

void
wm_dl_job_persist(dl_jobtable_t *t, const dl_job_t *j)
{
  db_result_t *res     = NULL;
  char        *e_err   = NULL;
  char         sql[1024];
  int          n;

  (void)t;

  e_err = db_escape(j->last_err);

  if(e_err == NULL)
    return;

  n = snprintf(sql, sizeof(sql),
      "UPDATE wm_download_job"
      "   SET state            = '%s',"
      "       cursor_after     = %" PRId64 ","
      "       pages_fetched    = %" PRId32 ","
      "       rows_written     = %" PRId64 ","
      "       last_err         = %s%s%s,"
      "       last_progress_ms = %" PRId64 ","
      "       updated          = NOW()"
      " WHERE id = %" PRId64,
      wm_dl_state_str(j->state),
      j->cursor_after,
      j->pages_fetched,
      j->rows_written,
      j->last_err[0] == '\0' ? "NULL" : "'",
      j->last_err[0] == '\0' ? "" : e_err,
      j->last_err[0] == '\0' ? "" : "'",
      j->last_progress_ms,
      j->id);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    mem_free(e_err);
    return;
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    mem_free(e_err);
    return;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, WM_DL_CTX,
        "job persist failed for id=%" PRId64 ": %s",
        j->id,
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  mem_free(e_err);
}

// ------------------------------------------------------------------ //
// Job insert (new enqueue)                                            //
// ------------------------------------------------------------------ //

static bool
wm_dl_insert_job(dl_job_kind_t kind,
    int32_t market_id, int32_t granularity, const char *oldest_ts,
    const char *newest_ts, const char *requested_by, int64_t *out_id)
{
  db_result_t *res       = NULL;
  char        *e_old     = NULL;
  char        *e_new     = NULL;
  char        *e_req     = NULL;
  char         sql[2048];
  char         gran_lit[24];
  char         old_lit[96];
  char         new_lit[96];
  bool         ok        = FAIL;
  const char  *kind_str  = (kind == DL_JOB_CANDLES) ? "candles" : "trades";
  int          n;

  e_req = db_escape(requested_by != NULL ? requested_by : "");

  if(oldest_ts != NULL && oldest_ts[0] != '\0')
    e_old = db_escape(oldest_ts);

  if(newest_ts != NULL && newest_ts[0] != '\0')
    e_new = db_escape(newest_ts);

  if(e_req == NULL)
    goto out;

  if(granularity > 0)
    snprintf(gran_lit, sizeof(gran_lit), "%" PRId32, granularity);

  else
    snprintf(gran_lit, sizeof(gran_lit), "NULL");

  if(e_old != NULL)
    snprintf(old_lit, sizeof(old_lit), "TIMESTAMPTZ '%s'", e_old);

  else
    snprintf(old_lit, sizeof(old_lit), "NULL");

  if(e_new != NULL)
    snprintf(new_lit, sizeof(new_lit), "TIMESTAMPTZ '%s'", e_new);

  else
    snprintf(new_lit, sizeof(new_lit), "NULL");

  n = snprintf(sql, sizeof(sql),
      "INSERT INTO wm_download_job"
      " (market_id, kind, granularity,"
      "  oldest_ts, newest_ts, state, cursor_after, pages_fetched,"
      "  rows_written, requested_by)"
      " VALUES (%" PRId32 ", '%s', %s,"
      "         %s, %s, 'queued', 0, 0, 0, '%s')"
      " RETURNING id",
      market_id, kind_str, gran_lit,
      old_lit, new_lit, e_req);

  if(n < 0 || (size_t)n >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    const char *s_id = db_result_get(res, 0, 0);

    if(s_id != NULL)
    {
      *out_id = (int64_t)strtoll(s_id, NULL, 10);
      ok = SUCCESS;
    }
  }

  else
    clam(CLAM_WARN, WM_DL_CTX, "job insert failed: %s",
        res != NULL && res->error[0] != '\0'
            ? res->error : "(no driver error)");

out:
  if(res   != NULL) db_result_free(res);
  if(e_old != NULL) mem_free(e_old);
  if(e_new != NULL) mem_free(e_new);
  if(e_req != NULL) mem_free(e_req);

  return(ok);
}

// ------------------------------------------------------------------ //
// Public enqueue                                                      //
// ------------------------------------------------------------------ //

bool
wm_dl_job_enqueue(whenmoon_state_t *st,
    dl_job_kind_t kind, int32_t market_id, int32_t granularity,
    uint8_t priority,
    const char *exchange, const char *exchange_symbol,
    const char *oldest_ts, const char *newest_ts,
    const char *requested_by, int64_t *out_job_id,
    char *err, size_t err_cap)
{
  dl_jobtable_t *t;
  dl_job_t      *j;
  int64_t        new_id = 0;
  const char    *newest_for_insert = newest_ts;
  char           newest_effective[WM_COV_TS_SZ] = {0};

  if(st == NULL || out_job_id == NULL || err == NULL || err_cap == 0)
    return(FAIL);

  err[0] = '\0';

  if(st->downloader == NULL)
  {
    snprintf(err, err_cap, "downloader not initialised");
    return(FAIL);
  }

  t = st->downloader;

  // Reject a second trades job for the same market while one is live.
  if(kind == DL_JOB_TRADES)
  {
    pthread_mutex_lock(&t->lock);

    if(wm_dl_has_running_trades_for_market_locked(t, market_id))
    {
      pthread_mutex_unlock(&t->lock);
      snprintf(err, err_cap,
          "market %" PRId32 " already has a running trades job",
          market_id);
      return(FAIL);
    }

    if(t->n_jobs >= WM_DL_JOBS_MAX)
    {
      pthread_mutex_unlock(&t->lock);
      snprintf(err, err_cap, "job table full (max %u)", WM_DL_JOBS_MAX);
      return(FAIL);
    }

    pthread_mutex_unlock(&t->lock);
  }

  // Candles pre-flight: clamp future newest_ts to now (logged once per
  // enqueue), default missing newest_ts to now, and short-circuit if
  // coverage is already complete. Gran is authoritative for the gap
  // lookup; cursor_end_ts starts at newest_effective.
  if(kind == DL_JOB_CANDLES)
  {
    time_t          now = time(NULL);
    struct tm       tm;
    uint32_t        n_gaps;
    wm_coverage_t   gaps_buf[4];

    if(gmtime_r(&now, &tm) == NULL)
    {
      snprintf(err, err_cap, "gmtime_r failed");
      return(FAIL);
    }

    snprintf(newest_effective, sizeof(newest_effective),
        "%04d-%02d-%02d %02d:%02d:%02d+00",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    if(newest_ts != NULL && newest_ts[0] != '\0')
    {
      if(strcmp(newest_ts, newest_effective) > 0)
        clam(CLAM_INFO, WM_DL_CTX,
            "candles enqueue: clamping future newest_ts to now");

      else
        snprintf(newest_effective, sizeof(newest_effective),
            "%s", newest_ts);
    }

    n_gaps = wm_coverage_gaps_candles(market_id, granularity,
        (oldest_ts != NULL && oldest_ts[0] != '\0')
            ? oldest_ts : "1970-01-01 00:00:00+00",
        newest_effective, gaps_buf,
        (uint32_t)(sizeof(gaps_buf) / sizeof(gaps_buf[0])));

    if(n_gaps == 0)
    {
      snprintf(err, err_cap, "coverage complete, nothing to fetch");
      return(FAIL);
    }

    newest_for_insert = newest_effective;
  }

  if(wm_dl_insert_job(kind, market_id, granularity,
         oldest_ts, newest_for_insert, requested_by, &new_id) != SUCCESS)
  {
    snprintf(err, err_cap, "db insert failed");
    return(FAIL);
  }

  j = mem_alloc("whenmoon.dl", "job", sizeof(*j));

  if(j == NULL)
  {
    snprintf(err, err_cap, "out of memory");
    return(FAIL);
  }

  memset(j, 0, sizeof(*j));
  j->id               = new_id;
  j->market_id        = market_id;
  j->kind             = kind;
  j->granularity      = granularity;
  j->priority         = priority;
  j->state            = DL_JOB_QUEUED;
  j->last_progress_ms = wm_dl_now_ms();

  if(oldest_ts != NULL)
    snprintf(j->oldest_ts, sizeof(j->oldest_ts), "%s", oldest_ts);

  if(newest_for_insert != NULL)
    snprintf(j->newest_ts, sizeof(j->newest_ts), "%s", newest_for_insert);

  if(exchange != NULL)
    snprintf(j->exchange, sizeof(j->exchange), "%s", exchange);

  if(exchange_symbol != NULL)
    snprintf(j->exchange_symbol, sizeof(j->exchange_symbol),
        "%s", exchange_symbol);

  if(kind == DL_JOB_CANDLES)
  {
    j->cursor_after         = 0;
    j->candle_table_ensured = false;
    snprintf(j->cursor_end_ts, sizeof(j->cursor_end_ts),
        "%s", newest_effective);
  }

  pthread_mutex_lock(&t->lock);
  wm_dl_job_list_append_locked(t, j);
  pthread_mutex_unlock(&t->lock);

  // DL-1: ensure the stall watchdog is armed before we dispatch. Idem-
  // potent — no-op when the supervisor is already running.
  wm_dl_supervisor_kick(t);

  // Kick the dispatcher so the new job's first page fires immediately.
  wm_dl_kick(t);

  *out_job_id = new_id;
  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Cancel                                                              //
// ------------------------------------------------------------------ //

bool
wm_dl_job_cancel(whenmoon_state_t *st, int64_t job_id,
    char *err, size_t err_cap)
{
  dl_jobtable_t *t;
  dl_job_t      *j;
  dl_job_t       snap;
  bool           found = false;

  if(st == NULL || err == NULL || err_cap == 0)
    return(FAIL);

  err[0] = '\0';

  if(st->downloader == NULL)
  {
    snprintf(err, err_cap, "downloader not initialised");
    return(FAIL);
  }

  t = st->downloader;

  pthread_mutex_lock(&t->lock);

  j = wm_dl_find_job_locked(t, job_id);

  if(j != NULL)
  {
    found = true;
    j->state = DL_JOB_FAILED;
    snprintf(j->last_err, sizeof(j->last_err), "cancelled by admin");

    // Snapshot the fields persist reads so we can drop the lock before
    // the DB call without racing wm_dl_jobtable_destroy's drain+free.
    snap = *j;
    snap.next = NULL;
  }

  pthread_mutex_unlock(&t->lock);

  if(!found)
  {
    snprintf(err, err_cap, "job %" PRId64 " not found", job_id);
    return(FAIL);
  }

  wm_dl_job_persist(t, &snap);

  // DL-1: drop the cancelled job from the in-memory list (it is now
  // FAILED). If a request was in-flight when the cancel landed, the
  // completion callback will short-circuit on the missing job. The
  // supervisor disarms itself if the list is now empty.
  wm_dl_remove_completed(t);

  // WM-DL-RACE-1: hand sibling QUEUED/RUNNING+!in_flight jobs a chance
  // to run. Without this kick, cancelling a not-yet-in-flight job
  // leaves siblings sitting until the next external event (sibling
  // completion, supervisor tick, or a new enqueue).
  wm_dl_kick(t);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Iteration                                                           //
// ------------------------------------------------------------------ //

void
wm_dl_job_list_iterate(whenmoon_state_t *st,
    wm_dl_job_iter_cb_t cb, void *user)
{
  dl_jobtable_t *t;
  dl_job_t      *snapshot = NULL;
  dl_job_t      *j;
  dl_job_t     **pp = &snapshot;

  if(st == NULL || st->downloader == NULL || cb == NULL)
    return;

  t = st->downloader;

  // Copy the list under the lock; walk the copy without it.
  pthread_mutex_lock(&t->lock);

  for(j = t->jobs_head; j != NULL; j = j->next)
  {
    dl_job_t *c = mem_alloc("whenmoon.dl", "job_snap", sizeof(*c));

    if(c == NULL)
      break;

    memcpy(c, j, sizeof(*c));
    c->next = NULL;
    *pp = c;
    pp  = &c->next;
  }

  pthread_mutex_unlock(&t->lock);

  for(j = snapshot; j != NULL; j = j->next)
    cb(j, user);

  wm_dl_free_job_list(snapshot);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

bool
wm_dl_jobtable_init(whenmoon_state_t *st)
{
  dl_jobtable_t *t;
  uint64_t       max_u;
  uint32_t       max_concurrent;

  if(st == NULL)
    return(FAIL);

  if(st->downloader != NULL)
    return(SUCCESS);

  max_u = kv_get_uint("plugin.whenmoon.downloader.max_concurrent_jobs");

  if(max_u == 0) max_u = 4;
  if(max_u > WM_DL_JOBS_MAX) max_u = WM_DL_JOBS_MAX;
  max_concurrent = (uint32_t)max_u;

  t = mem_alloc("whenmoon.dl", "jobtable", sizeof(*t));

  if(t == NULL)
    return(FAIL);

  memset(t, 0, sizeof(*t));
  t->st              = st;
  t->max_concurrent  = max_concurrent;

  if(pthread_mutex_init(&t->lock, NULL) != 0)
  {
    mem_free(t);
    return(FAIL);
  }

  if(pthread_cond_init(&t->drain, NULL) != 0)
  {
    pthread_mutex_destroy(&t->lock);
    mem_free(t);
    return(FAIL);
  }

  st->downloader = t;

  // Load persisted queued/running jobs into memory.
  wm_dl_load_jobs(t);

  clam(CLAM_INFO, WM_DL_CTX,
      "downloader ready (max_concurrent=%u, rate-limit owned by"
      " feature_exchange)", max_concurrent);

  // DL-1: if the restore brought any jobs back, arm the supervisor and
  // kick the chain. The exchange abstraction will queue everything
  // beyond its burst depth.
  if(t->n_jobs > 0)
  {
    wm_dl_supervisor_kick(t);
    wm_dl_kick(t);
  }

  return(SUCCESS);
}

void
wm_dl_jobtable_destroy(whenmoon_state_t *st)
{
  dl_jobtable_t *t;
  dl_job_t      *head;
  dl_job_t      *j;

  if(st == NULL || st->downloader == NULL)
    return;

  t = st->downloader;

  // DL-1: cancel the supervisor and wait for any in-flight tick to
  // finish touching the jobtable BEFORE we acquire t->lock. Doing this
  // first avoids any chance of a tick callback dereferencing freed
  // memory after the destroy below frees `t`.
  wm_dl_supervisor_drain();

  // Under the lock: flag destroying, PAUSE every live job so completion
  // callbacks bail out without touching rows, then wait for every
  // in-flight curl request to drain before freeing.
  pthread_mutex_lock(&t->lock);

  t->destroying = true;

  for(j = t->jobs_head; j != NULL; j = j->next)
  {
    if(j->state == DL_JOB_RUNNING || j->state == DL_JOB_QUEUED)
      j->state = DL_JOB_PAUSED;
  }

  WM_FS_TRACE_DRAIN("enter", t->in_flight_count);

  while(t->in_flight_count > 0)
  {
    pthread_cond_wait(&t->drain, &t->lock);
    WM_FS_TRACE_DRAIN("wake", t->in_flight_count);
  }

  WM_FS_TRACE_DRAIN("exit", t->in_flight_count);

  head = t->jobs_head;
  t->jobs_head = NULL;
  t->n_jobs    = 0;

  pthread_mutex_unlock(&t->lock);

  // Persist PAUSED state so a restart resumes from cursor_after.
  for(j = head; j != NULL; j = j->next)
    if(j->state == DL_JOB_PAUSED)
      wm_dl_job_persist(t, j);

  wm_dl_free_job_list(head);

  st->downloader = NULL;
  pthread_cond_destroy(&t->drain);
  pthread_mutex_destroy(&t->lock);
  mem_free(t);

  clam(CLAM_INFO, WM_DL_CTX, "downloader torn down");
}
