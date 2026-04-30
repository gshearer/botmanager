// botmanager — MIT
// Whenmoon trade-history page loop: dispatch, parse, insert, advance.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_trades.h"
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

// Context carried across the async coinbase fetch. The jobtable
// pointer is stable for the lifetime of the plugin; `job_id` re-resolves
// the job under the jobtable lock in case it was cancelled or the
// jobtable was destroyed while the request was in flight.
typedef struct
{
  dl_jobtable_t  *table;
  int64_t         job_id;
} wm_dl_trades_ctx_t;

// Tri-valued insert outcome (WM-DC-3). The pre-WM-DC-3 helper folded
// "all rows duplicate" and "DB error" both into return value 0, so the
// caller silently extended coverage with rows that never landed in the
// table. WM_DL_INSERT_OK now means "rows are confirmed present" — any
// of N>0 written, all-duplicates (already there), or empty page; the
// caller is safe to extend coverage. WM_DL_INSERT_FAIL means the DB
// rejected or dropped the statement; coverage MUST NOT be extended.
typedef enum
{
  WM_DL_INSERT_OK   = 0,
  WM_DL_INSERT_FAIL = 1
} wm_dl_insert_status_t;

static void wm_dl_trades_on_page(const coinbase_trades_result_t *res,
    void *user);
static wm_dl_insert_status_t wm_dl_trades_insert_page(int32_t market_id,
    const coinbase_trades_result_t *res, uint32_t *out_written);
static void wm_dl_us_to_tstz(int64_t time_us, char *out, size_t cap);

// ------------------------------------------------------------------ //
// Timestamp helper                                                    //
// ------------------------------------------------------------------ //

static void
wm_dl_us_to_tstz(int64_t time_us, char *out, size_t cap)
{
  time_t     secs = (time_t)(time_us / 1000000);
  int        usec = (int)(time_us % 1000000);
  struct tm  tm;
  char       buf[64];
  size_t     blen;
  size_t     copy;
  int        year;

  if(out == NULL || cap == 0)
    return;

  if(usec < 0) usec = 0;
  if(usec > 999999) usec = 999999;

  if(gmtime_r(&secs, &tm) == NULL)
  {
    snprintf(out, cap, "1970-01-01 00:00:00+00");
    return;
  }

  year = tm.tm_year + 1900;

  if(year < 0)    year = 0;
  if(year > 9999) year = 9999;

  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d+00",
      year, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec, usec);

  blen = strnlen(buf, sizeof(buf));
  copy = blen < cap - 1 ? blen : cap - 1;
  memcpy(out, buf, copy);
  out[copy] = '\0';
}

// ------------------------------------------------------------------ //
// Dispatch                                                            //
// ------------------------------------------------------------------ //

bool
wm_dl_trades_dispatch_one(dl_jobtable_t *t, dl_job_t *j)
{
  wm_dl_trades_ctx_t *ctx;

  if(t == NULL || j == NULL)
    return(FAIL);

  ctx = mem_alloc("whenmoon.dl", "trade_ctx", sizeof(*ctx));

  if(ctx == NULL)
  {
    // WM-DL-RACE-1: alloc failure short-circuits the async chain so the
    // user callback won't fire — record the error here so the kick
    // doesn't park the job in RUNNING+!in_flight indefinitely.
    wm_dl_record_dispatch_error(t, j, "trade ctx alloc failed");
    return(FAIL);
  }

  ctx->table  = t;
  ctx->job_id = j->id;

  // Ensure the per-pair trade table exists before the first page.
  if(!j->table_ensured)
  {
    if(wm_trade_table_ensure(j->market_id) != SUCCESS)
    {
      mem_free(ctx);

      // WM-DL-RACE-1: same as ctx-alloc — the callback won't fire so
      // we must surface the error through the job's retry budget.
      wm_dl_record_dispatch_error(t, j, "trade table_ensure failed");
      return(FAIL);
    }

    j->table_ensured = true;
  }

  if(coinbase_fetch_trades_async(j->exchange_symbol,
        j->cursor_after, COINBASE_MAX_TRADES, j->priority,
        wm_dl_trades_on_page, ctx) != SUCCESS)
  {
    // The coinbase shim contract says the callback fires with err on
    // FAIL, so `ctx` is owned by the callback even in the failure
    // path — DO NOT free it here. The callback's own error path
    // increments consecutive_errors, so we don't double-record.
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Row insert                                                          //
// ------------------------------------------------------------------ //

static wm_dl_insert_status_t
wm_dl_trades_insert_page(int32_t market_id,
    const coinbase_trades_result_t *res, uint32_t *out_written)
{
  char         table[WM_DL_TABLE_SZ];
  db_result_t *dbres;
  char        *sql;
  size_t       cap = 256 * 1024;
  size_t       len = 0;
  uint32_t     written = 0;
  uint32_t     i;

  if(out_written != NULL)
    *out_written = 0;

  // Empty page is OK: nothing to write, nothing to fail.
  if(res == NULL || res->count == 0)
    return(WM_DL_INSERT_OK);

  if(wm_trade_table_name(market_id, table, sizeof(table)) != SUCCESS)
  {
    clam(CLAM_WARN, WM_DL_CTX,
        "trade insert: cannot resolve table name (market=%" PRId32 ")",
        market_id);
    return(WM_DL_INSERT_FAIL);
  }

  sql = mem_alloc("whenmoon.dl", "trade_insert", cap);

  if(sql == NULL)
    return(WM_DL_INSERT_FAIL);

  len += (size_t)snprintf(sql + len, cap - len,
      "INSERT INTO %s (trade_id, ts, side, price, size, source)"
      " VALUES ", table);

  for(i = 0; i < res->count; i++)
  {
    const coinbase_trade_t *t = &res->rows[i];
    char ts_buf[40];
    char side_ch;
    int  n;

    side_ch = (t->side[0] == 'b' || t->side[0] == 'B') ? 'b' : 's';
    wm_dl_us_to_tstz(t->time_us, ts_buf, sizeof(ts_buf));

    if(cap - len < 512)
    {
      size_t new_cap = cap * 2;
      char  *new_sql = mem_realloc(sql, new_cap);

      if(new_sql == NULL)
      {
        mem_free(sql);
        return(WM_DL_INSERT_FAIL);
      }

      sql = new_sql;
      cap = new_cap;
    }

    // price / size are decimal strings straight from Coinbase. NUMERIC
    // parses them directly — do NOT wrap in quotes.
    n = snprintf(sql + len, cap - len,
        "%s(%" PRId64 ", TIMESTAMPTZ '%s', '%c', %s, %s, 0)",
        i > 0 ? "," : "",
        t->trade_id, ts_buf, side_ch,
        t->price[0] != '\0' ? t->price : "0",
        t->size[0]  != '\0' ? t->size  : "0");

    if(n < 0)
    {
      mem_free(sql);
      return(WM_DL_INSERT_FAIL);
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
      return(WM_DL_INSERT_FAIL);
    }

    sql = new_sql;
    cap = new_cap;
  }

  len += (size_t)snprintf(sql + len, cap - len,
      " ON CONFLICT (trade_id) DO NOTHING");

  dbres = db_result_alloc();

  if(dbres == NULL)
  {
    mem_free(sql);
    return(WM_DL_INSERT_FAIL);
  }

  // PostgreSQL `INSERT ... ON CONFLICT DO NOTHING` is atomic per
  // statement: either the whole batch lands (rows_affected == N) or
  // the whole batch is rejected (ok=false). All-duplicates returns
  // ok=true, rows_affected=0 — the rows are confirmed present so
  // coverage extension is sound.
  if(db_query(sql, dbres) == SUCCESS && dbres->ok)
  {
    written = dbres->rows_affected;
    db_result_free(dbres);
    mem_free(sql);

    if(out_written != NULL)
      *out_written = written;

    return(WM_DL_INSERT_OK);
  }

  clam(CLAM_WARN, WM_DL_CTX,
      "trade insert failed (market=%" PRId32 "): %s",
      market_id,
      dbres->error[0] != '\0' ? dbres->error : "(no driver error)");

  db_result_free(dbres);
  mem_free(sql);
  return(WM_DL_INSERT_FAIL);
}

// ------------------------------------------------------------------ //
// Completion callback                                                 //
// ------------------------------------------------------------------ //

static void
wm_dl_trades_on_page(const coinbase_trades_result_t *res, void *user)
{
  wm_dl_trades_ctx_t *ctx = user;
  dl_jobtable_t      *t;
  dl_job_t           *j;
  uint32_t            inserted = 0;
  int64_t             oldest_id_on_page = 0;
  int64_t             newest_id_on_page = 0;
  int32_t             market_id = 0;
  char                oldest_ts_on_page[40] = {0};
  char                newest_ts_on_page[40] = {0};
  char                oldest_bound[40] = {0};
  bool                empty_page = false;
  bool                hit_err    = false;
  bool                bail       = false;
  char                errmsg[128] = {0};

  if(ctx == NULL)
    return;

  t = ctx->table;

  if(t == NULL)
  {
    mem_free(ctx);
    return;
  }

  // Phase 1: re-resolve the job under the lock. Bail if cancelled,
  // paused (teardown), or missing — but always release the slot and
  // the heap ctx.
  pthread_mutex_lock(&t->lock);

  j = wm_dl_find_job_locked(t, ctx->job_id);

  if(j == NULL || j->state != DL_JOB_RUNNING || t->destroying)
    bail = true;

  else
  {
    market_id = j->market_id;
    snprintf(oldest_bound, sizeof(oldest_bound), "%s", j->oldest_ts);
  }

  pthread_mutex_unlock(&t->lock);

  if(bail)
  {
    // The in_flight guard only exists on live jobs; decrement the
    // jobtable-wide counter so destroy-drain can proceed even when the
    // job itself has vanished from the list.
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

    // Re-kick: this job is paused/cancelled, but other queued jobs may
    // be eligible. wm_dl_kick bails on destroying.
    wm_dl_kick(t);
    return;
  }

  // Phase 2: classify the outcome.
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
    empty_page = true;
  }

  // Phase 3: insert rows (off-lock; DB is the long op). WM-DC-3:
  // distinguish "all rows are present" (OK — extend coverage) from
  // "DB rejected the batch" (FAIL — leave coverage untouched and let
  // the consecutive_errors retry path handle transients).
  if(!hit_err && !empty_page)
  {
    wm_dl_insert_status_t st = wm_dl_trades_insert_page(market_id, res,
        &inserted);

    if(st == WM_DL_INSERT_FAIL)
    {
      hit_err = true;
      snprintf(errmsg, sizeof(errmsg), "db insert failed");
    }

    else
    {
      oldest_id_on_page  = res->rows[res->count - 1].trade_id;
      newest_id_on_page  = res->rows[0].trade_id;
      wm_dl_us_to_tstz(res->rows[res->count - 1].time_us,
          oldest_ts_on_page, sizeof(oldest_ts_on_page));
      wm_dl_us_to_tstz(res->rows[0].time_us,
          newest_ts_on_page, sizeof(newest_ts_on_page));

      // Extend coverage with the range we just stored. Safe whether
      // `inserted` is N or 0 — all-duplicates means rows are already
      // present (a prior write established coverage); a fresh write
      // means we just produced them.
      {
        wm_coverage_t iv;

        memset(&iv, 0, sizeof(iv));
        iv.market_id      = market_id;
        iv.first_trade_id = oldest_id_on_page;
        iv.last_trade_id  = newest_id_on_page;
        snprintf(iv.first_ts, sizeof(iv.first_ts), "%s", oldest_ts_on_page);
        snprintf(iv.last_ts,  sizeof(iv.last_ts),  "%s", newest_ts_on_page);
        snprintf(iv.source,   sizeof(iv.source),   "api");
        wm_coverage_add(WM_COV_TRADES, &iv);
      }
    }
  }

  // Phase 4: update in-memory state under the lock, then persist.
  pthread_mutex_lock(&t->lock);

  j = wm_dl_find_job_locked(t, ctx->job_id);

  if(j == NULL || j->state != DL_JOB_RUNNING || t->destroying)
  {
    // Lost the race — job was cancelled / paused between phase 1 and
    // phase 4. Rows already inserted stay in place; no further state
    // transitions happen here.
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

  else if(empty_page)
  {
    // No more pages older than cursor_after; walked to inception.
    j->state            = DL_JOB_DONE;
    j->last_err[0]      = '\0';
    j->last_progress_ms = wm_dl_now_ms();
  }

  else
  {
    j->pages_fetched++;
    j->rows_written      += inserted;
    j->cursor_after       = oldest_id_on_page;
    j->consecutive_errors = 0;
    j->last_err[0]        = '\0';
    j->last_progress_ms   = wm_dl_now_ms();

    // Terminal if we've walked past the user's oldest bound.
    if(oldest_bound[0] != '\0' &&
       strcmp(oldest_ts_on_page, oldest_bound) <= 0)
    {
      j->state = DL_JOB_DONE;
    }

    clam(CLAM_INFO, WM_DL_CTX,
        "trades %s page=%" PRId32 " rows=+%u cursor=%" PRId64,
        j->exchange_symbol, j->pages_fetched, inserted,
        j->cursor_after);
  }

  pthread_mutex_unlock(&t->lock);

  wm_dl_job_persist(t, j);

  // DL-1: clear_in_flight reaps any DONE / FAILED job whose in-flight
  // guard just dropped, so a job that just transitioned to a terminal
  // state vanishes from the in-memory list here.
  wm_dl_job_clear_in_flight(t, j);
  mem_free(ctx);

  // Chain to the next page. With the rate limiter owned by
  // feature_exchange, throughput is paced by the abstraction's token
  // bucket; we just kick whenever a slot opens.
  wm_dl_kick(t);
}
