// botmanager — MIT
// trade_persist.c — buffered async trade-tape persistence (WM-LT-2).
//
// One ring per market; one plugin-global periodic flush task. Every
// tick the task walks a registry of live persist instances, drains a
// batch from each ring under its market lock, releases the lock, and
// issues one batched INSERT against the per-market `wm_trades_<id>`
// table. ON CONFLICT (trade_id) DO NOTHING so a WS reconnect that
// re-delivers recent trades dedups silently by primary key.
//
// The ring is bounded; on overflow we drop the oldest unflushed entry
// so the producer (WS reader thread) can never block. A rate-limited
// CLAM_WARN once per overflow event surfaces sustained backpressure
// without flooding the log under steady drift.

#define WHENMOON_INTERNAL
#include "trade_persist.h"
#include "dl_schema.h"
#include "market.h"
#include "whenmoon.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"
#include "task.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Per-market ring capacity. 4096 entries × 32 B/entry ≈ 128 KB. Sized
// so that a hot product (~50 trades/sec) can absorb ~80 s of DB
// outage at the configured flush cadence before forcing a drop.
#define WM_TP_RING_CAP        4096

// Maximum trades drained per market per flush tick. Sized so the worst
// hot product (~50 trades/sec sustained) stays caught up at 1 s flush
// interval, with headroom for catch-up after a brief DB stall.
#define WM_TP_BATCH_MAX        500

// Flush cadence (ms). Task system rounds sub-second values up to 1 s
// today, so finer cadences are no-ops; documented here so the value
// can be lowered later without re-discovering the floor.
#define WM_TP_FLUSH_INTERVAL_MS  1000

// Overflow warn cooldown — at most one CLAM_WARN per market per minute.
#define WM_TP_OVERFLOW_WARN_COOLDOWN_MS  60000

// One persisted trade. Smaller than coinbase_ws_match_t because the
// match payload's product_id / sequence are not carried into the
// per-market table (the table identity already pins the product, and
// the trade_id PK already orders within the market).
typedef struct
{
  int64_t  trade_id;
  int64_t  time_ms;
  double   price;
  double   size;
  char     side;       // 'b' or 's'
} wm_tp_row_t;

struct wm_trade_persist
{
  whenmoon_market_t *mk;     // back-pointer; resolves market_id + lock

  char               table[WM_DL_TABLE_SZ];

  // Ring under mk->lock. Producer (wm_trade_persist_async) appends at
  // head; consumer (flush task) drains from tail. Both hold mk->lock.
  wm_tp_row_t       *ring;
  uint32_t           cap;
  uint32_t           head;
  uint32_t           tail;
  uint32_t           n;

  // Overflow rate-limiter.
  int64_t            last_overflow_warn_ms;
  uint64_t           overflow_total;

  // Linked into wm_tp_g_head under wm_tp_g_lock.
  struct wm_trade_persist *next;
};

// ------------------------------------------------------------------ //
// Plugin-global registry + flush task                                //
// ------------------------------------------------------------------ //

static pthread_mutex_t       wm_tp_g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wm_trade_persist *wm_tp_g_head = NULL;
static task_handle_t         wm_tp_g_task = TASK_HANDLE_NONE;

static void wm_tp_flush_task(task_t *t);
static void wm_tp_flush_instance(struct wm_trade_persist *tp);
static void wm_tp_ms_to_tstz(int64_t time_ms, char *out, size_t cap);

bool
wm_trade_persist_global_init(void)
{
  if(wm_tp_g_task != TASK_HANDLE_NONE)
    return(SUCCESS);

  wm_tp_g_task = task_add_periodic("wm_trade_flush", TASK_ANY, 200,
      WM_TP_FLUSH_INTERVAL_MS, wm_tp_flush_task, NULL);

  if(wm_tp_g_task == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, WHENMOON_CTX, "trade-persist flush task submit failed");
    return(FAIL);
  }

  return(SUCCESS);
}

void
wm_trade_persist_global_destroy(void)
{
  if(wm_tp_g_task != TASK_HANDLE_NONE)
  {
    task_cancel(wm_tp_g_task);
    wm_tp_g_task = TASK_HANDLE_NONE;
  }
}

// ------------------------------------------------------------------ //
// Per-market lifecycle                                               //
// ------------------------------------------------------------------ //

bool
wm_trade_persist_init(whenmoon_market_t *mk)
{
  struct wm_trade_persist *tp;

  if(mk == NULL || mk->market_id < 0)
    return(FAIL);

  tp = mem_alloc("whenmoon", "trade_persist", sizeof(*tp));

  if(tp == NULL)
    return(FAIL);

  memset(tp, 0, sizeof(*tp));
  tp->mk  = mk;
  tp->cap = WM_TP_RING_CAP;

  if(wm_trade_table_name(mk->market_id, tp->table, sizeof(tp->table))
         != SUCCESS)
  {
    mem_free(tp);
    return(FAIL);
  }

  // Ensure the per-pair trade table exists. wm_trade_table_ensure is
  // idempotent (CREATE … IF NOT EXISTS) so calling per-market on every
  // bot start is cheap.
  if(wm_trade_table_ensure(mk->market_id) != SUCCESS)
  {
    mem_free(tp);
    return(FAIL);
  }

  tp->ring = mem_alloc("whenmoon", "trade_persist_ring",
      sizeof(*tp->ring) * (size_t)tp->cap);

  if(tp->ring == NULL)
  {
    mem_free(tp);
    return(FAIL);
  }

  memset(tp->ring, 0, sizeof(*tp->ring) * (size_t)tp->cap);

  pthread_mutex_lock(&wm_tp_g_lock);
  tp->next     = wm_tp_g_head;
  wm_tp_g_head = tp;
  pthread_mutex_unlock(&wm_tp_g_lock);

  mk->trade_persist = tp;

  return(SUCCESS);
}

void
wm_trade_persist_destroy(whenmoon_market_t *mk)
{
  struct wm_trade_persist  *tp;
  struct wm_trade_persist **pp;

  if(mk == NULL || mk->trade_persist == NULL)
    return;

  tp = mk->trade_persist;

  pthread_mutex_lock(&wm_tp_g_lock);

  for(pp = &wm_tp_g_head; *pp != NULL; pp = &(*pp)->next)
  {
    if(*pp == tp)
    {
      *pp = tp->next;
      break;
    }
  }

  pthread_mutex_unlock(&wm_tp_g_lock);

  if(tp->ring != NULL)
    mem_free(tp->ring);

  mk->trade_persist = NULL;
  mem_free(tp);
}

// ------------------------------------------------------------------ //
// Producer                                                           //
// ------------------------------------------------------------------ //

void
wm_trade_persist_async(whenmoon_market_t *mk, const coinbase_ws_match_t *m)
{
  struct wm_trade_persist *tp;
  wm_tp_row_t             *slot;

  if(mk == NULL || m == NULL || mk->trade_persist == NULL)
    return;

  tp = mk->trade_persist;

  if(tp->ring == NULL || tp->cap == 0)
    return;

  if(tp->n == tp->cap)
  {
    // Overflow: drop oldest unflushed. The flush task is falling
    // behind — emit a rate-limited warn so the operator can spot it.
    struct timespec   ts;
    int64_t           now_ms;

    tp->tail = (tp->tail + 1) % tp->cap;
    tp->n--;
    tp->overflow_total++;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);

    if(now_ms - tp->last_overflow_warn_ms >=
           WM_TP_OVERFLOW_WARN_COOLDOWN_MS)
    {
      clam(CLAM_WARN, WHENMOON_CTX,
          "market %s: trade-persist ring overflow (total=%" PRIu64
          ", oldest dropped)",
          mk->product_id, tp->overflow_total);
      tp->last_overflow_warn_ms = now_ms;
    }
  }

  slot = &tp->ring[tp->head];
  slot->trade_id = m->trade_id;
  slot->time_ms  = m->time_ms;
  slot->price    = m->price;
  slot->size     = m->size;
  slot->side     = (m->side[0] == 'b' || m->side[0] == 'B') ? 'b' : 's';

  tp->head = (tp->head + 1) % tp->cap;
  tp->n++;
}

// ------------------------------------------------------------------ //
// Flush task                                                         //
// ------------------------------------------------------------------ //

static void
wm_tp_flush_task(task_t *t)
{
  struct wm_trade_persist *tp;

  if(t == NULL)
    return;

  pthread_mutex_lock(&wm_tp_g_lock);

  for(tp = wm_tp_g_head; tp != NULL; tp = tp->next)
    wm_tp_flush_instance(tp);

  pthread_mutex_unlock(&wm_tp_g_lock);

  t->state = TASK_ENDED;
}

// Drain up to WM_TP_BATCH_MAX entries from `tp` under its market lock,
// then issue one batched INSERT off-lock. Caller holds wm_tp_g_lock.
static void
wm_tp_flush_instance(struct wm_trade_persist *tp)
{
  whenmoon_market_t *mk;
  wm_tp_row_t        batch[WM_TP_BATCH_MAX];
  uint32_t           drained = 0;
  size_t             cap = 64 * 1024;
  size_t             len = 0;
  char              *sql;
  db_result_t       *res;
  uint32_t           i;

  if(tp == NULL || tp->ring == NULL)
    return;

  mk = tp->mk;

  if(mk == NULL)
    return;

  pthread_mutex_lock(&mk->lock);

  while(drained < WM_TP_BATCH_MAX && tp->n > 0)
  {
    batch[drained++] = tp->ring[tp->tail];
    tp->tail = (tp->tail + 1) % tp->cap;
    tp->n--;
  }

  pthread_mutex_unlock(&mk->lock);

  if(drained == 0)
    return;

  sql = mem_alloc("whenmoon", "tp_insert", cap);

  if(sql == NULL)
    return;

  len += (size_t)snprintf(sql + len, cap - len,
      "INSERT INTO %s (trade_id, ts, side, price, size, source)"
      " VALUES ", tp->table);

  for(i = 0; i < drained; i++)
  {
    char ts_buf[40];
    int  n;

    if(cap - len < 256)
    {
      size_t  new_cap = cap * 2;
      char   *new_sql = mem_realloc(sql, new_cap);

      if(new_sql == NULL)
      {
        mem_free(sql);
        return;
      }

      sql = new_sql;
      cap = new_cap;
    }

    wm_tp_ms_to_tstz(batch[i].time_ms, ts_buf, sizeof(ts_buf));

    n = snprintf(sql + len, cap - len,
        "%s(%" PRId64 ", TIMESTAMPTZ '%s', '%c', %.10g, %.10g, 1)",
        i > 0 ? "," : "",
        batch[i].trade_id, ts_buf, batch[i].side,
        batch[i].price, batch[i].size);

    if(n < 0)
    {
      mem_free(sql);
      return;
    }

    len += (size_t)n;
  }

  if(cap - len < 64)
  {
    char *new_sql = mem_realloc(sql, cap + 64);

    if(new_sql == NULL)
    {
      mem_free(sql);
      return;
    }

    sql  = new_sql;
    cap += 64;
  }

  len += (size_t)snprintf(sql + len, cap - len,
      " ON CONFLICT (trade_id) DO NOTHING");

  res = db_result_alloc();

  if(res == NULL)
  {
    mem_free(sql);
    return;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "trade-persist insert failed (table=%s, drained=%u): %s",
        tp->table, drained,
        res->error[0] != '\0' ? res->error : "(no driver error)");
  }

  else
  {
    clam(CLAM_DEBUG2, WHENMOON_CTX,
        "trade-persist %s flushed %u rows (affected=%u)",
        tp->table, drained, res->rows_affected);
  }

  db_result_free(res);
  mem_free(sql);
}

// ------------------------------------------------------------------ //
// Time helper — int64 ms → TIMESTAMPTZ literal.                       //
// Mirrors dl_trades.c's wm_dl_us_to_tstz pattern but takes ms input.  //
// ------------------------------------------------------------------ //

static void
wm_tp_ms_to_tstz(int64_t time_ms, char *out, size_t cap)
{
  time_t     secs = (time_t)(time_ms / 1000);
  int        msec = (int)(time_ms % 1000);
  struct tm  tm;
  int        year;

  if(out == NULL || cap == 0)
    return;

  if(msec < 0) msec = 0;
  if(msec > 999) msec = 999;

  if(gmtime_r(&secs, &tm) == NULL)
  {
    snprintf(out, cap, "1970-01-01 00:00:00+00");
    return;
  }

  year = tm.tm_year + 1900;

  if(year < 0)    year = 0;
  if(year > 9999) year = 9999;

  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d+00",
      year, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec, msec);
}
