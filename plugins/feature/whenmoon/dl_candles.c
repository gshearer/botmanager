// botmanager — MIT
// Whenmoon candle-history page loop: dispatch, insert, advance.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_candles.h"
#include "dl_jobtable.h"
#include "dl_schema.h"
#include "dl_coverage.h"

#include "coinbase_api.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WM_DL_CANDLE_SQL_CAP      32768

// Coinbase granularity ladder. Kept in ascending order so CSV output
// and validation messages read naturally. 60=1m, 300=5m, 900=15m,
// 3600=1h, 21600=6h, 86400=1d.
static const int32_t WM_DL_CANDLE_GRAN_LADDER[] = {
  60, 300, 900, 3600, 21600, 86400
};

// ------------------------------------------------------------------ //
// Time helpers                                                        //
// ------------------------------------------------------------------ //

// "YYYY-MM-DD HH:MM:SS+00" (or with fractional) -> seconds since epoch.
// Returns 0 on malformed input.
static int64_t
wm_dl_tstz_to_s(const char *in)
{
  unsigned  year;
  unsigned  mon;
  unsigned  day;
  unsigned  hour;
  unsigned  min;
  unsigned  sec;
  struct tm tm;
  time_t    t;

  if(in == NULL || in[0] == '\0')
    return(0);

  if(sscanf(in, "%u-%u-%u %u:%u:%u",
        &year, &mon, &day, &hour, &min, &sec) != 6)
    return(0);

  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)year - 1900;
  tm.tm_mon  = (int)mon  - 1;
  tm.tm_mday = (int)day;
  tm.tm_hour = (int)hour;
  tm.tm_min  = (int)min;
  tm.tm_sec  = (int)sec;

  // timegm is GNU; the project already assumes Linux + POSIX. The
  // incoming string is always UTC ("+00"), so mktime would mis-shift.
  t = timegm(&tm);

  if(t == (time_t)-1)
    return(0);

  return((int64_t)t);
}

// seconds-since-epoch -> "YYYY-MM-DD HH:MM:SS+00".
static void
wm_dl_s_to_tstz(int64_t secs, char *out, size_t cap)
{
  time_t    t = (time_t)secs;
  struct tm tm;
  int       year;

  if(out == NULL || cap == 0)
    return;

  if(secs < 0) t = 0;

  if(gmtime_r(&t, &tm) == NULL)
  {
    snprintf(out, cap, "1970-01-01 00:00:00+00");
    return;
  }

  year = tm.tm_year + 1900;

  if(year < 0)    year = 0;
  if(year > 9999) year = 9999;

  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d+00",
      year, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int64_t
wm_dl_align_down(int64_t v, int64_t step)
{
  if(step <= 0) return(v);

  return((v / step) * step);
}

// ------------------------------------------------------------------ //
// Dispatch                                                            //
// ------------------------------------------------------------------ //

bool
wm_dl_candles_dispatch_one(dl_jobtable_t *t, dl_job_t *j)
{
  wm_dl_candles_ctx_t *ctx;
  int64_t              end_s;
  int64_t              start_s;
  int32_t              gran;

  if(t == NULL || j == NULL)
    return(FAIL);

  gran = j->granularity > 0 ? j->granularity : WM_DL_CANDLE_GRAN_S5;

  end_s = wm_dl_tstz_to_s(j->cursor_end_ts);

  if(end_s <= 0)
    end_s = (int64_t)time(NULL);

  end_s   = wm_dl_align_down(end_s, (int64_t)gran);
  start_s = end_s - (int64_t)WM_DL_CANDLE_WINDOW_BUCKETS * (int64_t)gran;

  if(start_s < 0)
    start_s = 0;

  // Ensure the per-pair candle table exists before the first page.
  // Mirrors wm_dl_trades_dispatch_one's pattern: synchronous DDL on
  // the dispatch thread, idempotent (CREATE TABLE IF NOT EXISTS), then
  // flip the cached marker so subsequent pages skip the round-trip.
  if(!j->candle_table_ensured)
  {
    if(wm_candle_table_ensure(j->market_id, gran) != SUCCESS)
    {
      // WM-DL-RACE-1: callback won't fire so record the error here.
      wm_dl_record_dispatch_error(t, j, "candle table_ensure failed");
      return(FAIL);
    }

    j->candle_table_ensured = true;
  }

  ctx = mem_alloc("whenmoon.dl", "candle_ctx", sizeof(*ctx));

  if(ctx == NULL)
  {
    // WM-DL-RACE-1: same as the table_ensure path.
    wm_dl_record_dispatch_error(t, j, "candle ctx alloc failed");
    return(FAIL);
  }

  ctx->table          = t;
  ctx->job_id         = j->id;
  ctx->window_start_s = start_s;
  ctx->window_end_s   = end_s;

  // Coinbase treats `end` as inclusive; subtract one so the boundary
  // bucket (which starts the next window) doesn't come back twice.
  if(coinbase_fetch_candles_async(j->exchange_symbol, gran,
        start_s, end_s - 1, j->priority,
        wm_dl_candles_on_page, ctx) != SUCCESS)
  {
    // On submit failure the coinbase shim still fires the completion
    // callback synchronously with res->err set, so `ctx` ownership has
    // already transferred — DO NOT free it here. The callback's own
    // error path increments consecutive_errors.
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Row insert                                                          //
// ------------------------------------------------------------------ //

uint32_t
wm_dl_candles_insert_page(int32_t market_id, int32_t gran_secs,
    const coinbase_candles_result_t *res)
{
  char         table[WM_DL_TABLE_SZ];
  db_result_t *dbres;
  char        *sql;
  size_t       cap = WM_DL_CANDLE_SQL_CAP;
  size_t       len = 0;
  uint32_t     written = 0;
  uint32_t     i;

  if(res == NULL || res->count == 0)
    return(0);

  if(wm_candle_table_name(market_id, gran_secs, table, sizeof(table))
      != SUCCESS)
    return(0);

  sql = mem_alloc("whenmoon.dl", "candle_insert", cap);

  if(sql == NULL)
    return(0);

  len += (size_t)snprintf(sql + len, cap - len,
      "INSERT INTO %s (ts, low, high, open, close, volume) VALUES ",
      table);

  for(i = 0; i < res->count; i++)
  {
    const coinbase_candle_t *c = &res->rows[i];
    int                      n;

    if(cap - len < 256)
    {
      size_t new_cap = cap * 2;
      char  *new_sql = mem_realloc(sql, new_cap);

      if(new_sql == NULL)
      {
        mem_free(sql);
        return(0);
      }

      sql = new_sql;
      cap = new_cap;
    }

    n = snprintf(sql + len, cap - len,
        "%s(to_timestamp(%" PRId64 ") AT TIME ZONE 'UTC',"
        " %.17g, %.17g, %.17g, %.17g, %.17g)",
        i > 0 ? "," : "",
        c->time, c->low, c->high, c->open, c->close, c->volume);

    if(n < 0)
    {
      mem_free(sql);
      return(0);
    }

    len += (size_t)n;
  }

  if(cap - len < 64)
  {
    size_t new_cap = cap + 64;
    char  *new_sql = mem_realloc(sql, new_cap);

    if(new_sql == NULL)
    {
      mem_free(sql);
      return(0);
    }

    sql = new_sql;
    cap = new_cap;
  }

  len += (size_t)snprintf(sql + len, cap - len,
      " ON CONFLICT (ts) DO NOTHING");

  dbres = db_result_alloc();

  if(dbres == NULL)
  {
    mem_free(sql);
    return(0);
  }

  if(db_query(sql, dbres) == SUCCESS && dbres->ok)
    written = dbres->rows_affected;

  else
    clam(CLAM_WARN, WM_DL_CTX,
        "candle insert failed (table=%s rows=%u): %s",
        table, res->count,
        dbres->error[0] != '\0' ? dbres->error : "(no driver error)");

  db_result_free(dbres);
  mem_free(sql);
  return(written);
}

// ------------------------------------------------------------------ //
// Completion callback                                                 //
// ------------------------------------------------------------------ //

void
wm_dl_candles_on_page(const coinbase_candles_result_t *res, void *user)
{
  wm_dl_candles_ctx_t *ctx = user;
  dl_jobtable_t       *t;
  dl_job_t            *j;
  int32_t              market_id = 0;
  int32_t              gran      = WM_DL_CANDLE_GRAN_S5;
  uint32_t             inserted  = 0;
  int64_t              oldest_requested_s = 0;
  char                 oldest_bound[40] = {0};
  bool                 hit_err   = false;
  bool                 empty     = false;
  bool                 bail      = false;
  char                 errmsg[128] = {0};

  if(ctx == NULL)
    return;

  t = ctx->table;

  if(t == NULL)
  {
    mem_free(ctx);
    return;
  }

  // Phase 1: re-resolve the job under the lock. Bail if cancelled,
  // paused (teardown), or missing.
  pthread_mutex_lock(&t->lock);

  j = wm_dl_find_job_locked(t, ctx->job_id);

  if(j == NULL || j->state != DL_JOB_RUNNING || t->destroying)
    bail = true;

  else
  {
    market_id            = j->market_id;
    gran                 = j->granularity > 0
                         ? j->granularity : WM_DL_CANDLE_GRAN_S5;
    snprintf(oldest_bound, sizeof(oldest_bound), "%s", j->oldest_ts);
  }

  pthread_mutex_unlock(&t->lock);

  if(bail)
  {
    // Mirror wm_dl_trades_on_page's drain-safe release: decrement the
    // jobtable-wide counter even if the job vanished from the list.
    if(j != NULL)
      wm_dl_job_clear_in_flight(t, j);

    else
    {
      pthread_mutex_lock(&t->lock);

      if(t->in_flight_count > 0)
        t->in_flight_count--;

      pthread_cond_broadcast(&t->drain);
      pthread_mutex_unlock(&t->lock);
    }

    mem_free(ctx);

    // Re-kick: this job is paused/cancelled, but other queued jobs
    // may be eligible. wm_dl_kick bails on destroying.
    wm_dl_kick(t);
    return;
  }

  // Phase 2: classify outcome.
  if(res == NULL)
  {
    hit_err = true;
    snprintf(errmsg, sizeof(errmsg), "null result");
  }

  else if(res->err[0] != '\0')
  {
    hit_err = true;
    snprintf(errmsg, sizeof(errmsg), "%s", res->err);
  }

  else if(res->count == 0)
  {
    empty = true;
  }

  // Phase 3: insert + coverage — all off-lock. The per-pair candle
  // table is ensured synchronously in wm_dl_candles_dispatch_one, so
  // there is no DDL race here.
  if(!hit_err && !empty)
    inserted = wm_dl_candles_insert_page(market_id, gran, res);

  // Coverage extension: use the ATTEMPTED window regardless of
  // res->count. A legitimate zero-tick window would otherwise become a
  // permanent gap. Skip only on hard errors (null result / server err /
  // DDL failure).
  if(!hit_err)
  {
    wm_coverage_t iv;

    memset(&iv, 0, sizeof(iv));
    iv.market_id   = market_id;
    iv.granularity = gran;
    wm_dl_s_to_tstz(ctx->window_start_s, iv.first_ts, sizeof(iv.first_ts));
    wm_dl_s_to_tstz(ctx->window_end_s,   iv.last_ts,  sizeof(iv.last_ts));
    snprintf(iv.source, sizeof(iv.source), "api");
    wm_coverage_add(WM_COV_CANDLES, &iv);
  }

  oldest_requested_s = oldest_bound[0] != '\0'
      ? wm_dl_tstz_to_s(oldest_bound) : 0;

  // Phase 4: update in-memory state under the lock, then persist.
  pthread_mutex_lock(&t->lock);

  j = wm_dl_find_job_locked(t, ctx->job_id);

  if(j == NULL || j->state != DL_JOB_RUNNING || t->destroying)
  {
    if(j != NULL && j->in_flight)
    {
      j->in_flight = false;

      if(t->in_flight_count > 0)
        t->in_flight_count--;
    }

    else if(j == NULL && t->in_flight_count > 0)
      t->in_flight_count--;

    pthread_cond_broadcast(&t->drain);
    pthread_mutex_unlock(&t->lock);
    mem_free(ctx);

    // Re-kick: see bail path above.
    wm_dl_kick(t);
    return;
  }

  if(hit_err)
  {
    j->consecutive_errors++;
    snprintf(j->last_err, sizeof(j->last_err), "%s", errmsg);

    if(j->consecutive_errors >= WM_DL_PAGE_RETRY_MAX)
      j->state = DL_JOB_FAILED;
  }

  else
  {
    char new_cursor[40];

    j->pages_fetched++;
    j->rows_written      += inserted;
    j->consecutive_errors = 0;
    j->last_err[0]        = '\0';
    j->last_progress_ms   = wm_dl_now_ms();

    // Advance cursor: next window ends where this one started.
    wm_dl_s_to_tstz(ctx->window_start_s, new_cursor, sizeof(new_cursor));
    snprintf(j->cursor_end_ts, sizeof(j->cursor_end_ts), "%s", new_cursor);

    // Terminal decisions. Reaching the user's oldest floor ends the job
    // regardless of whether the page was empty; an empty page past the
    // floor also ends it (inception walk).
    if(oldest_requested_s > 0 && ctx->window_start_s <= oldest_requested_s)
      j->state = DL_JOB_DONE;

    else if(empty && ctx->window_start_s <= oldest_requested_s)
      j->state = DL_JOB_DONE;

    else if(oldest_requested_s == 0 && ctx->window_start_s == 0)
      j->state = DL_JOB_DONE;

    clam(CLAM_INFO, WM_DL_CTX,
        "candles %s page=%" PRId32 " rows=+%u cursor=%s%s",
        j->exchange_symbol, j->pages_fetched, inserted,
        j->cursor_end_ts, empty ? " (empty)" : "");
  }

  pthread_mutex_unlock(&t->lock);

  wm_dl_job_persist(t, j);

  // DL-1: clear_in_flight reaps any DONE / FAILED job whose in-flight
  // guard just dropped, so a job that just transitioned to a terminal
  // state vanishes from the in-memory list here.
  wm_dl_job_clear_in_flight(t, j);
  mem_free(ctx);

  // Chain to the next page. The exchange abstraction's token bucket
  // paces sustained throughput; we just kick whenever a slot opens.
  wm_dl_kick(t);
}

// ------------------------------------------------------------------ //
// WM-S6 — aggregated candle query                                     //
// ------------------------------------------------------------------ //

bool
wm_dl_granularity_valid(int32_t gran_secs)
{
  size_t i;

  for(i = 0;
      i < sizeof(WM_DL_CANDLE_GRAN_LADDER)
        / sizeof(WM_DL_CANDLE_GRAN_LADDER[0]);
      i++)
  {
    if(WM_DL_CANDLE_GRAN_LADDER[i] == gran_secs)
      return(true);
  }

  return(false);
}

uint32_t
wm_dl_candles_query_aggregated(int32_t market_id, int32_t gran_secs,
    const char *start_ts, const char *end_ts,
    coinbase_candle_t *out, uint32_t cap)
{
  char         table[WM_DL_TABLE_SZ];
  char        *e_table = NULL;
  char        *e_start = NULL;
  char        *e_end   = NULL;
  db_result_t *res     = NULL;
  char         sql[512];
  uint32_t     n       = 0;
  uint32_t     take;
  uint32_t     i;
  int          written;

  if(out == NULL || cap == 0 ||
     start_ts == NULL || end_ts == NULL ||
     !wm_dl_granularity_valid(gran_secs))
    return(0);

  if(wm_candle_table_name(market_id,
         WM_DL_CANDLE_GRAN_S5, table, sizeof(table)) != SUCCESS)
    return(0);

  // Ensure the 1m table exists so the upsample function doesn't fail
  // on an unknown market with "relation does not exist". The caller
  // sees "no data" instead.
  if(wm_candle_table_ensure(market_id, WM_DL_CANDLE_GRAN_S5) != SUCCESS)
    return(0);

  e_table = db_escape(table);
  e_start = db_escape(start_ts);
  e_end   = db_escape(end_ts);

  if(e_table == NULL || e_start == NULL || e_end == NULL)
    goto out;

  written = snprintf(sql, sizeof(sql),
      "SELECT EXTRACT(EPOCH FROM ts)::BIGINT,"
      "       low, high, open, close, volume"
      "  FROM wm_candle_upsample('%s', %" PRId32 ","
      "                          TIMESTAMPTZ '%s',"
      "                          TIMESTAMPTZ '%s')",
      e_table, gran_secs, e_start, e_end);

  if(written < 0 || (size_t)written >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "candle upsample query failed (market=%" PRId32
        " gran=%" PRId32 "): %s",
        market_id, gran_secs,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    goto out;
  }

  take = res->rows;

  if(take > cap)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "candle upsample market=%" PRId32 " gran=%" PRId32
        ": %u rows, truncating to %u",
        market_id, gran_secs, res->rows, cap);
    take = cap;
  }

  for(i = 0; i < take; i++)
  {
    const char *s;

    memset(&out[i], 0, sizeof(out[i]));

    s = db_result_get(res, i, 0);
    if(s != NULL) out[i].time   = (int64_t)strtoll(s, NULL, 10);
    s = db_result_get(res, i, 1);
    if(s != NULL) out[i].low    = strtod(s, NULL);
    s = db_result_get(res, i, 2);
    if(s != NULL) out[i].high   = strtod(s, NULL);
    s = db_result_get(res, i, 3);
    if(s != NULL) out[i].open   = strtod(s, NULL);
    s = db_result_get(res, i, 4);
    if(s != NULL) out[i].close  = strtod(s, NULL);
    s = db_result_get(res, i, 5);
    if(s != NULL) out[i].volume = strtod(s, NULL);
  }

  n = take;

out:
  if(res     != NULL) db_result_free(res);
  if(e_table != NULL) mem_free(e_table);
  if(e_start != NULL) mem_free(e_start);
  if(e_end   != NULL) mem_free(e_end);

  return(n);
}
