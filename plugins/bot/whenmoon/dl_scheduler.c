// botmanager — MIT
// Per-bot download scheduler: token bucket + event-driven dispatcher.
//
// No periodic tick. The dispatch chain is driven by:
//   1. wm_dl_job_enqueue — kicks a freshly-queued job.
//   2. wm_dl_scheduler_init — kicks any persisted queued/running rows
//      restored on bot start.
//   3. completion callbacks (wm_dl_trades_on_page,
//      wm_dl_candles_on_page) — chain to the next page after the prior
//      one returns.
//   4. wm_dl_retry_task_cb — a one-shot deferred re-entry when the
//      bucket was empty at the time of an earlier dispatch attempt.
//
// All four routes call wm_dl_dispatch_next, which is the single
// take-token + pick-job + submit primitive. On bucket-empty (or a rare
// synchronous submit failure) it arms `retry_task` for a delayed
// re-entry; once the bucket refills the chain resumes naturally.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_scheduler.h"
#include "dl_schema.h"
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

static void wm_dl_bucket_refill_locked(dl_scheduler_t *s);
static bool wm_dl_load_jobs(dl_scheduler_t *s);
static bool wm_dl_insert_job(const char *bot_name, dl_job_kind_t kind,
    int32_t market_id, int32_t granularity, const char *oldest_ts,
    const char *newest_ts, const char *requested_by, int64_t *out_id);
static void wm_dl_job_list_append_locked(dl_scheduler_t *s, dl_job_t *j);
static bool wm_dl_has_running_trades_for_market_locked(dl_scheduler_t *s,
    int32_t market_id);
static const char *wm_dl_state_str(dl_job_state_t s);
static dl_job_state_t wm_dl_state_from_str(const char *s);
static void wm_dl_free_job_list(dl_job_t *head);
static void wm_dl_arm_retry(dl_scheduler_t *s);
static void wm_dl_retry_task_cb(task_t *t);

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
// Token bucket                                                        //
// ------------------------------------------------------------------ //

static void
wm_dl_bucket_refill_locked(dl_scheduler_t *s)
{
  struct timespec now;
  double          elapsed;

  clock_gettime(CLOCK_MONOTONIC, &now);
  elapsed = (double)(now.tv_sec - s->last_refill.tv_sec)
          + (double)(now.tv_nsec - s->last_refill.tv_nsec) / 1e9;

  if(elapsed > 0.0)
  {
    s->tokens += elapsed * s->tokens_per_sec;

    if(s->tokens > s->tokens_cap)
      s->tokens = s->tokens_cap;

    s->last_refill = now;
  }
}

void
wm_dl_bucket_refill(dl_scheduler_t *s)
{
  pthread_mutex_lock(&s->lock);
  wm_dl_bucket_refill_locked(s);
  pthread_mutex_unlock(&s->lock);
}

bool
wm_dl_bucket_take(dl_scheduler_t *s)
{
  bool ok;

  pthread_mutex_lock(&s->lock);
  wm_dl_bucket_refill_locked(s);

  if(s->tokens >= 1.0)
  {
    s->tokens -= 1.0;
    ok = true;
  }

  else
    ok = false;

  pthread_mutex_unlock(&s->lock);
  return(ok);
}

// ------------------------------------------------------------------ //
// Pick policy                                                         //
// ------------------------------------------------------------------ //

dl_job_t *
wm_dl_sched_pick_next(dl_scheduler_t *s)
{
  dl_job_t *j;
  uint32_t  running = 0;

  // Count running slots in use so we honour max_concurrent.
  for(j = s->jobs_head; j != NULL; j = j->next)
    if(j->state == DL_JOB_RUNNING && j->in_flight)
      running++;

  if(running >= s->max_concurrent)
    return(NULL);

  // Walk the list once picking the first RUNNING && !in_flight job;
  // promote queued jobs to RUNNING as slots open.
  for(j = s->jobs_head; j != NULL; j = j->next)
  {
    if(j->state == DL_JOB_RUNNING && !j->in_flight)
      return(j);
  }

  // No running job ready — promote a queued one if there is slack.
  for(j = s->jobs_head; j != NULL; j = j->next)
  {
    if(j->state == DL_JOB_QUEUED && running < s->max_concurrent)
    {
      j->state = DL_JOB_RUNNING;
      return(j);
    }
  }

  return(NULL);
}

dl_job_t *
wm_dl_find_job_locked(dl_scheduler_t *s, int64_t id)
{
  dl_job_t *j;

  for(j = s->jobs_head; j != NULL; j = j->next)
    if(j->id == id)
      return(j);

  return(NULL);
}

// ------------------------------------------------------------------ //
// Event-driven dispatcher                                             //
// ------------------------------------------------------------------ //

// Arm the deferred retry. At-most-one-outstanding semantics: callers
// race-safely no-op when retry_task is already armed. Cancellation is
// the destroy path's responsibility.
static void
wm_dl_arm_retry(dl_scheduler_t *s)
{
  pthread_mutex_lock(&s->lock);

  if(s->destroying || !s->enabled || s->retry_task != TASK_HANDLE_NONE)
  {
    pthread_mutex_unlock(&s->lock);
    return;
  }

  s->retry_task = task_add_deferred("wm_dl_retry", TASK_ANY, 120,
      WM_DL_RETRY_DELAY_MS, wm_dl_retry_task_cb, s);

  pthread_mutex_unlock(&s->lock);
}

static void
wm_dl_retry_task_cb(task_t *t)
{
  dl_scheduler_t *s = t->data;

  if(s == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  pthread_mutex_lock(&s->lock);
  s->retry_task = TASK_HANDLE_NONE;
  pthread_mutex_unlock(&s->lock);

  // wm_dl_dispatch_next short-circuits on destroying / pool_shutting_down,
  // so no extra guard needed here.
  wm_dl_dispatch_next(s);

  t->state = TASK_ENDED;
}

void
wm_dl_dispatch_next(dl_scheduler_t *s)
{
  dl_job_t *j;
  bool      submitted = false;

  if(s == NULL || s->destroying || pool_shutting_down())
    return;

  if(!s->enabled)
    return;

  if(!wm_dl_bucket_take(s))
  {
    // Bucket empty: arm a retry so the chain resumes once tokens
    // refill. Completion callbacks may also re-enter this function in
    // the meantime — first-come-first-served.
    wm_dl_arm_retry(s);
    return;
  }

  pthread_mutex_lock(&s->lock);
  j = wm_dl_sched_pick_next(s);

  if(j != NULL)
  {
    j->in_flight = true;
    s->in_flight_count++;
  }

  pthread_mutex_unlock(&s->lock);

  if(j == NULL)
  {
    // No eligible job — refund the token. Caller (a completion
    // callback or enqueue path) has nothing to chain; the next event
    // in the system will re-enter this function.
    pthread_mutex_lock(&s->lock);
    s->tokens += 1.0;

    if(s->tokens > s->tokens_cap)
      s->tokens = s->tokens_cap;

    pthread_mutex_unlock(&s->lock);
    return;
  }

  // Dispatch off-lock. The async REST submit's completion callback
  // (wm_dl_trades_on_page / wm_dl_candles_on_page) will clear
  // in_flight, commit rows, and re-enter wm_dl_dispatch_next to fire
  // the next page in the chain.
  if(j->kind == DL_JOB_TRADES)
    submitted = (wm_dl_trades_dispatch_one(s, j) == SUCCESS);

  else if(j->kind == DL_JOB_CANDLES)
    submitted = (wm_dl_candles_dispatch_one(s, j) == SUCCESS);

  else
    clam(CLAM_WARN, WM_DL_CTX,
        "dispatch_next: unknown job kind %d for job %" PRId64,
        (int)j->kind, j->id);

  if(!submitted)
  {
    // Synchronous submit failure — clear the slot and arm a retry so
    // the chain doesn't stall. Don't recurse: a deterministic
    // synchronous failure would otherwise spin without bound.
    wm_dl_job_clear_in_flight(s, j);
    wm_dl_arm_retry(s);
  }
}

void
wm_dl_job_clear_in_flight(dl_scheduler_t *s, dl_job_t *j)
{
  pthread_mutex_lock(&s->lock);

  if(j->in_flight)
  {
    j->in_flight = false;

    if(s->in_flight_count > 0)
      s->in_flight_count--;
  }

  pthread_cond_broadcast(&s->drain);
  pthread_mutex_unlock(&s->lock);
}

// ------------------------------------------------------------------ //
// Load persisted jobs for this bot                                    //
// ------------------------------------------------------------------ //

static bool
wm_dl_load_jobs(dl_scheduler_t *s)
{
  db_result_t *res   = NULL;
  char        *e_bot = NULL;
  char         sql[1024];
  bool         ok = FAIL;
  uint32_t     loaded = 0;

  e_bot = db_escape(s->st->bot_name);

  if(e_bot == NULL)
    goto out;

  // Pull core job columns + joined market fields so we can populate
  // exchange_symbol without a second query per job.
  snprintf(sql, sizeof(sql),
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
      "       m.exchange, m.exchange_symbol"
      "  FROM wm_download_job j"
      "  JOIN wm_market m ON m.id = j.market_id"
      " WHERE j.bot_name = '%s'"
      "   AND j.state IN ('queued', 'running')"
      " ORDER BY j.id",
      e_bot);

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "job load failed for bot %s: %s", s->st->bot_name,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    goto out;
  }

  pthread_mutex_lock(&s->lock);

  for(uint32_t i = 0; i < res->rows && s->n_jobs < WM_DL_JOBS_MAX; i++)
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

    // QUEUED rows get promoted by the tick via pick_next. RUNNING rows
    // that survived the orphan reset shouldn't exist — but be forgiving
    // and treat them as queued.
    if(j->state == DL_JOB_RUNNING)
      j->state = DL_JOB_QUEUED;

    wm_dl_job_list_append_locked(s, j);
    loaded++;
  }

  pthread_mutex_unlock(&s->lock);

  ok = SUCCESS;

  if(loaded > 0)
    clam(CLAM_INFO, WM_DL_CTX,
        "bot %s: resumed %u job%s from persisted state",
        s->st->bot_name, loaded, loaded == 1 ? "" : "s");

out:
  if(res   != NULL) db_result_free(res);
  if(e_bot != NULL) mem_free(e_bot);

  return(ok);
}

// ------------------------------------------------------------------ //
// In-memory job list helpers                                          //
// ------------------------------------------------------------------ //

static void
wm_dl_job_list_append_locked(dl_scheduler_t *s, dl_job_t *j)
{
  dl_job_t **pp = &s->jobs_head;

  while(*pp != NULL)
    pp = &(*pp)->next;

  *pp = j;
  j->next = NULL;
  s->n_jobs++;
}

static bool
wm_dl_has_running_trades_for_market_locked(dl_scheduler_t *s,
    int32_t market_id)
{
  dl_job_t *j;

  for(j = s->jobs_head; j != NULL; j = j->next)
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

// ------------------------------------------------------------------ //
// DB persist                                                          //
// ------------------------------------------------------------------ //

void
wm_dl_job_persist(dl_scheduler_t *s, const dl_job_t *j)
{
  db_result_t *res     = NULL;
  char        *e_err   = NULL;
  char         sql[1024];
  int          n;

  (void)s;

  e_err = db_escape(j->last_err);

  if(e_err == NULL)
    return;

  n = snprintf(sql, sizeof(sql),
      "UPDATE wm_download_job"
      "   SET state         = '%s',"
      "       cursor_after  = %" PRId64 ","
      "       pages_fetched = %" PRId32 ","
      "       rows_written  = %" PRId64 ","
      "       last_err      = %s%s%s,"
      "       updated       = NOW()"
      " WHERE id = %" PRId64,
      wm_dl_state_str(j->state),
      j->cursor_after,
      j->pages_fetched,
      j->rows_written,
      j->last_err[0] == '\0' ? "NULL" : "'",
      j->last_err[0] == '\0' ? "" : e_err,
      j->last_err[0] == '\0' ? "" : "'",
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
wm_dl_insert_job(const char *bot_name, dl_job_kind_t kind,
    int32_t market_id, int32_t granularity, const char *oldest_ts,
    const char *newest_ts, const char *requested_by, int64_t *out_id)
{
  db_result_t *res       = NULL;
  char        *e_bot     = NULL;
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

  e_bot = db_escape(bot_name);
  e_req = db_escape(requested_by != NULL ? requested_by : "");

  if(oldest_ts != NULL && oldest_ts[0] != '\0')
    e_old = db_escape(oldest_ts);

  if(newest_ts != NULL && newest_ts[0] != '\0')
    e_new = db_escape(newest_ts);

  if(e_bot == NULL || e_req == NULL)
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
      " (bot_name, market_id, kind, granularity,"
      "  oldest_ts, newest_ts, state, cursor_after, pages_fetched,"
      "  rows_written, requested_by)"
      " VALUES ('%s', %" PRId32 ", '%s', %s,"
      "         %s, %s, 'queued', 0, 0, 0, '%s')"
      " RETURNING id",
      e_bot, market_id, kind_str, gran_lit,
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
    clam(CLAM_WARN, WM_DL_CTX,
        "job insert failed for bot %s: %s", bot_name,
        res != NULL && res->error[0] != '\0'
            ? res->error : "(no driver error)");

out:
  if(res   != NULL) db_result_free(res);
  if(e_bot != NULL) mem_free(e_bot);
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
    const char *exchange, const char *exchange_symbol,
    const char *oldest_ts, const char *newest_ts,
    const char *requested_by, int64_t *out_job_id,
    char *err, size_t err_cap)
{
  dl_scheduler_t *s;
  dl_job_t       *j;
  int64_t         new_id = 0;
  const char     *newest_for_insert = newest_ts;
  char            newest_effective[WM_COV_TS_SZ] = {0};

  if(st == NULL || out_job_id == NULL || err == NULL || err_cap == 0)
    return(FAIL);

  err[0] = '\0';

  if(st->downloader == NULL)
  {
    snprintf(err, err_cap, "downloader not initialised");
    return(FAIL);
  }

  s = st->downloader;

  // Reject a second trades job for the same market while one is live.
  if(kind == DL_JOB_TRADES)
  {
    pthread_mutex_lock(&s->lock);

    if(wm_dl_has_running_trades_for_market_locked(s, market_id))
    {
      pthread_mutex_unlock(&s->lock);
      snprintf(err, err_cap,
          "market %" PRId32 " already has a running trades job",
          market_id);
      return(FAIL);
    }

    if(s->n_jobs >= WM_DL_JOBS_MAX)
    {
      pthread_mutex_unlock(&s->lock);
      snprintf(err, err_cap, "job table full (max %u)", WM_DL_JOBS_MAX);
      return(FAIL);
    }

    pthread_mutex_unlock(&s->lock);
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

  if(wm_dl_insert_job(st->bot_name, kind, market_id, granularity,
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
  j->id          = new_id;
  j->market_id   = market_id;
  j->kind        = kind;
  j->granularity = granularity;
  j->state       = DL_JOB_QUEUED;

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

  pthread_mutex_lock(&s->lock);
  wm_dl_job_list_append_locked(s, j);
  pthread_mutex_unlock(&s->lock);

  // Kick the dispatcher so the new job's first page fires immediately
  // rather than waiting on a tick or a completion of some other job.
  wm_dl_dispatch_next(s);

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
  dl_scheduler_t *s;
  dl_job_t       *j;
  dl_job_t        snap;
  bool            found = false;

  if(st == NULL || err == NULL || err_cap == 0)
    return(FAIL);

  err[0] = '\0';

  if(st->downloader == NULL)
  {
    snprintf(err, err_cap, "downloader not initialised");
    return(FAIL);
  }

  s = st->downloader;

  pthread_mutex_lock(&s->lock);

  j = wm_dl_find_job_locked(s, job_id);

  if(j != NULL)
  {
    found = true;
    j->state = DL_JOB_FAILED;
    snprintf(j->last_err, sizeof(j->last_err), "cancelled by admin");

    // Snapshot the fields persist reads so we can drop the lock before
    // the DB call without racing wm_dl_scheduler_destroy's drain+free.
    snap = *j;
    snap.next = NULL;
  }

  pthread_mutex_unlock(&s->lock);

  if(!found)
  {
    snprintf(err, err_cap, "job %" PRId64 " not found", job_id);
    return(FAIL);
  }

  wm_dl_job_persist(s, &snap);
  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Iteration                                                           //
// ------------------------------------------------------------------ //

void
wm_dl_job_list_iterate(whenmoon_state_t *st,
    wm_dl_job_iter_cb_t cb, void *user)
{
  dl_scheduler_t *s;
  dl_job_t       *snapshot = NULL;
  dl_job_t       *j;
  dl_job_t      **pp = &snapshot;

  if(st == NULL || st->downloader == NULL || cb == NULL)
    return;

  s = st->downloader;

  // Copy the list under the lock; walk the copy without it.
  pthread_mutex_lock(&s->lock);

  for(j = s->jobs_head; j != NULL; j = j->next)
  {
    dl_job_t *c = mem_alloc("whenmoon.dl", "job_snap", sizeof(*c));

    if(c == NULL)
      break;

    memcpy(c, j, sizeof(*c));
    c->next = NULL;
    *pp = c;
    pp  = &c->next;
  }

  pthread_mutex_unlock(&s->lock);

  for(j = snapshot; j != NULL; j = j->next)
    cb(j, user);

  wm_dl_free_job_list(snapshot);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

bool
wm_dl_scheduler_init(whenmoon_state_t *st)
{
  dl_scheduler_t *s;
  char            key[KV_KEY_SZ];
  uint64_t        rps_u;
  uint64_t        max_u;
  uint32_t        rps;
  uint32_t        max_concurrent;
  bool            enabled;
  bool            rps_clamped = false;

  if(st == NULL)
    return(FAIL);

  if(st->downloader != NULL)
    return(SUCCESS);

  // Read KV knobs. Clamp rps to the Coinbase public cap.
  snprintf(key, sizeof(key),
      "bot.%s.whenmoon.downloader.enabled", st->bot_name);
  enabled = (kv_get_uint(key) != 0);

  snprintf(key, sizeof(key),
      "bot.%s.whenmoon.downloader.rate_limit_rps", st->bot_name);
  rps_u = kv_get_uint(key);

  if(rps_u == 0) rps_u = WM_DL_DEFAULT_RPS;
  if(rps_u < WM_DL_RPS_MIN) { rps_u = WM_DL_RPS_MIN; rps_clamped = true; }
  if(rps_u > WM_DL_RPS_MAX) { rps_u = WM_DL_RPS_MAX; rps_clamped = true; }
  rps = (uint32_t)rps_u;

  snprintf(key, sizeof(key),
      "bot.%s.whenmoon.downloader.max_concurrent_jobs", st->bot_name);
  max_u = kv_get_uint(key);

  if(max_u == 0) max_u = 4;
  if(max_u > WM_DL_JOBS_MAX) max_u = WM_DL_JOBS_MAX;
  max_concurrent = (uint32_t)max_u;

  s = mem_alloc("whenmoon.dl", "sched", sizeof(*s));

  if(s == NULL)
    return(FAIL);

  memset(s, 0, sizeof(*s));
  s->st              = st;
  s->tokens_per_sec  = (double)rps;
  s->tokens_cap      = (double)rps * WM_DL_BURST_MULT;
  s->tokens          = s->tokens_cap;
  s->enabled         = enabled;
  s->max_concurrent  = max_concurrent;
  s->retry_task      = TASK_HANDLE_NONE;

  clock_gettime(CLOCK_MONOTONIC, &s->last_refill);

  if(pthread_mutex_init(&s->lock, NULL) != 0)
  {
    mem_free(s);
    return(FAIL);
  }

  if(pthread_cond_init(&s->drain, NULL) != 0)
  {
    pthread_mutex_destroy(&s->lock);
    mem_free(s);
    return(FAIL);
  }

  st->downloader = s;

  // Load persisted queued/running jobs into memory.
  wm_dl_load_jobs(s);

  if(rps_clamped)
    clam(CLAM_INFO, WM_DL_CTX,
        "bot %s: rate_limit_rps clamped to [%d,%d]",
        st->bot_name, WM_DL_RPS_MIN, WM_DL_RPS_MAX);

  clam(CLAM_INFO, WM_DL_CTX,
      "bot %s: downloader ready (rps=%u burst=%u max_concurrent=%u%s)",
      st->bot_name, rps, (unsigned)s->tokens_cap, max_concurrent,
      enabled ? "" : " [disabled]");

  // Kick the chain for any restored queued jobs. Disabled scheduler
  // bails inside wm_dl_dispatch_next; queued jobs sit dormant until a
  // future enable+enqueue (current behaviour: enabled is read at init
  // only).
  wm_dl_dispatch_next(s);

  return(SUCCESS);
}

void
wm_dl_scheduler_destroy(whenmoon_state_t *st)
{
  dl_scheduler_t *s;
  dl_job_t       *head;
  dl_job_t       *j;

  if(st == NULL || st->downloader == NULL)
    return;

  s = st->downloader;

  if(s->retry_task != TASK_HANDLE_NONE)
  {
    task_cancel(s->retry_task);
    s->retry_task = TASK_HANDLE_NONE;
  }

  // Under the lock: flag destroying, PAUSE every live job so completion
  // callbacks bail out without touching rows, then wait for every
  // in-flight curl request to drain before freeing.
  pthread_mutex_lock(&s->lock);

  s->destroying = true;

  for(j = s->jobs_head; j != NULL; j = j->next)
  {
    if(j->state == DL_JOB_RUNNING || j->state == DL_JOB_QUEUED)
      j->state = DL_JOB_PAUSED;
  }

  while(s->in_flight_count > 0)
    pthread_cond_wait(&s->drain, &s->lock);

  head = s->jobs_head;
  s->jobs_head = NULL;
  s->n_jobs    = 0;

  pthread_mutex_unlock(&s->lock);

  // Persist PAUSED state so a restart resumes from cursor_after.
  for(j = head; j != NULL; j = j->next)
    if(j->state == DL_JOB_PAUSED)
      wm_dl_job_persist(s, j);

  wm_dl_free_job_list(head);

  st->downloader = NULL;
  pthread_cond_destroy(&s->drain);
  pthread_mutex_destroy(&s->lock);
  mem_free(s);

  clam(CLAM_INFO, WM_DL_CTX,
      "bot %s: downloader torn down", st->bot_name);
}
