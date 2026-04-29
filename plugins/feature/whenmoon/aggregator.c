// botmanager — MIT
// aggregator.c — multi-grain trade -> candle aggregator.
//
// One aggregator per running market. Drives the cascade
//   trade -> 1m bar -> 5m -> 15m -> 1h -> 6h -> 1d
// closing each grain on accumulator-input thresholds (1m feeds 5m on
// every 5 closed 1m bars; 5m feeds 15m every 3; etc.). Every closed
// bar gets pushed onto the corresponding grain ring and triggers a
// TA-Lib indicator pass via wm_indicators_compute_bar.
//
// The aggregator runs entirely under `whenmoon_market_t.lock`, so the
// callers (the WS reader thread for live trades; the warm-up task for
// DB replay) take that lock before invoking these entry points and
// release it when they are done.

#define WHENMOON_INTERNAL
#include "aggregator.h"
#include "dl_schema.h"
#include "indicators.h"
#include "market.h"
#include "strategy.h"
#include "whenmoon.h"

#include "db.h"
#include "task.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

const int32_t wm_gran_seconds[WM_GRAN_MAX] =
{
  60,    // 1m
  300,   // 5m
  900,   // 15m
  3600,  // 1h
  21600, // 6h
  86400, // 1d
};

// Inputs required to close one bar at the given target grain. 1m has
// no upstream and is closed by trade boundaries, not by accumulator.
static const uint32_t wm_inputs_per_grain[WM_GRAN_MAX] =
{
  [WM_GRAN_1M]  = 0,
  [WM_GRAN_5M]  = 5,    // 5  * 1m   = 5m
  [WM_GRAN_15M] = 3,    // 3  * 5m   = 15m
  [WM_GRAN_1H]  = 4,    // 4  * 15m  = 1h
  [WM_GRAN_6H]  = 6,    // 6  * 1h   = 6h
  [WM_GRAN_1D]  = 4,    // 4  * 6h   = 1d
};

// Forward decls.
static void wm_aggregator_close_1m(whenmoon_market_t *mk);
static void wm_aggregator_emit_empty_1m(whenmoon_market_t *mk,
    int64_t bar_start_ms);
static void wm_aggregator_push_bar(whenmoon_market_t *mk,
    wm_gran_t gran, const wm_candle_full_t *bar);
static void wm_aggregator_cascade_to(whenmoon_market_t *mk,
    wm_gran_t target, const wm_candle_full_t *src);

// ------------------------------------------------------------------ //
// Lifecycle                                                          //
// ------------------------------------------------------------------ //

bool
wm_aggregator_init(whenmoon_market_t *mk, uint32_t history_1d_min)
{
  wm_aggregator_t *a;
  uint32_t         g;

  if(mk == NULL || history_1d_min == 0)
    return(FAIL);

  a = mem_alloc("whenmoon", "aggregator", sizeof(*a));

  if(a == NULL)
    return(FAIL);

  memset(a, 0, sizeof(*a));
  a->history_1d           = history_1d_min;
  a->dispatch_strategies  = true;

  // memory: 200 days x 1440 bars x sizeof(wm_candle_full_t) (~280 B)
  // = ~80 MB per market for the 1m grain alone. All six grains together
  // come in around ~110 MB per market. With two markets per bot the
  // working set is ~220 MB — acceptable on a server-class deploy and
  // documented here so future reviewers see the math without grepping.
  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    uint32_t bars = history_1d_min * (86400u /
        (uint32_t)wm_gran_seconds[g]);
    size_t   sz   = sizeof(wm_candle_full_t) * (size_t)bars;

    mk->grain_arr[g] = mem_alloc("whenmoon", "grain", sz);

    if(mk->grain_arr[g] == NULL)
    {
      uint32_t k;

      for(k = 0; k < g; k++)
      {
        mem_free(mk->grain_arr[k]);
        mk->grain_arr[k] = NULL;
        mk->grain_cap[k] = 0;
      }

      mem_free(a);
      return(FAIL);
    }

    memset(mk->grain_arr[g], 0, sz);
    mk->grain_n[g]              = 0;
    mk->grain_cap[g]            = bars;
    a->bars_required[g]         = bars;
    a->work[g].inputs_required  = wm_inputs_per_grain[g];
  }

  mk->aggregator = a;

  clam(CLAM_INFO, WHENMOON_CTX,
      "bot market %s: aggregator init history_1d=%u"
      " (1m=%u 5m=%u 15m=%u 1h=%u 6h=%u 1d=%u)",
      mk->product_id, a->history_1d,
      mk->grain_cap[WM_GRAN_1M], mk->grain_cap[WM_GRAN_5M],
      mk->grain_cap[WM_GRAN_15M], mk->grain_cap[WM_GRAN_1H],
      mk->grain_cap[WM_GRAN_6H], mk->grain_cap[WM_GRAN_1D]);

  return(SUCCESS);
}

void
wm_aggregator_destroy(whenmoon_market_t *mk)
{
  uint32_t g;

  if(mk == NULL)
    return;

  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    if(mk->grain_arr[g] != NULL)
    {
      mem_free(mk->grain_arr[g]);
      mk->grain_arr[g] = NULL;
    }

    mk->grain_n[g]   = 0;
    mk->grain_cap[g] = 0;
  }

  if(mk->aggregator != NULL)
  {
    mem_free(mk->aggregator);
    mk->aggregator = NULL;
  }
}

// ------------------------------------------------------------------ //
// Live trade ingest                                                  //
// ------------------------------------------------------------------ //

void
wm_aggregator_on_trade(whenmoon_market_t *mk, int64_t ts_ms,
    double price, double size)
{
  wm_aggregator_t *a;
  int64_t          bar_ms;

  if(mk == NULL || mk->aggregator == NULL)
    return;

  a = mk->aggregator;

  if(ts_ms <= 0)
  {
    static int once = 0;

    if(!once)
    {
      clam(CLAM_WARN, WHENMOON_CTX,
          "market %s: trade with ts_ms<=0 rejected (%lld)",
          mk->product_id, (long long)ts_ms);
      once = 1;
    }

    return;
  }

  bar_ms = (ts_ms / 60000) * 60000;

  if(!a->pending_1m.populated)
  {
    a->pending_1m.bar_start_ms = bar_ms;
    a->pending_1m.populated    = true;
    a->pending_1m.open         = price;
    a->pending_1m.high         = price;
    a->pending_1m.low          = price;
    a->pending_1m.close        = price;
    a->pending_1m.volume       = size;
    return;
  }

  if(bar_ms == a->pending_1m.bar_start_ms)
  {
    if(price > a->pending_1m.high) a->pending_1m.high = price;
    if(price < a->pending_1m.low)  a->pending_1m.low  = price;
    a->pending_1m.close   = price;
    a->pending_1m.volume += size;
    return;
  }

  if(bar_ms < a->pending_1m.bar_start_ms)
  {
    static int once = 0;

    if(!once)
    {
      clam(CLAM_WARN, WHENMOON_CTX,
          "market %s: out-of-order trade dropped"
          " (ts_ms=%lld pending=%lld)",
          mk->product_id, (long long)ts_ms,
          (long long)a->pending_1m.bar_start_ms);
      once = 1;
    }

    return;
  }

  // Pending bucket is now closed.
  wm_aggregator_close_1m(mk);

  // Skip-bar fill. Any minutes between the closed pending bar and the
  // new bar get a synthetic empty 1m candle so the cascade arithmetic
  // stays correct on low-volume products.
  {
    int64_t expected = a->pending_1m.bar_start_ms + 60000;

    while(expected < bar_ms)
    {
      wm_aggregator_emit_empty_1m(mk, expected);
      expected += 60000;
    }
  }

  a->pending_1m.bar_start_ms = bar_ms;
  a->pending_1m.populated    = true;
  a->pending_1m.open         = price;
  a->pending_1m.high         = price;
  a->pending_1m.low          = price;
  a->pending_1m.close        = price;
  a->pending_1m.volume       = size;
}

// ------------------------------------------------------------------ //
// 1m close + skip-bar synthesis                                      //
// ------------------------------------------------------------------ //

static void
wm_aggregator_close_1m(whenmoon_market_t *mk)
{
  wm_aggregator_t  *a = mk->aggregator;
  wm_candle_full_t  bar;

  if(!a->pending_1m.populated)
    return;

  memset(&bar, 0, sizeof(bar));
  bar.ts_close_ms = a->pending_1m.bar_start_ms + 60000;
  bar.open        = a->pending_1m.open;
  bar.high        = a->pending_1m.high;
  bar.low         = a->pending_1m.low;
  bar.close       = a->pending_1m.close;
  bar.volume      = a->pending_1m.volume;

  wm_aggregator_push_bar(mk, WM_GRAN_1M, &bar);

  // Drive the cascade upward from 1m. The pushed bar in grain_arr[1m]
  // is the one that just closed; we pass it through.
  {
    uint32_t n = mk->grain_n[WM_GRAN_1M];

    if(n > 0)
      wm_aggregator_cascade_to(mk, WM_GRAN_5M,
          &mk->grain_arr[WM_GRAN_1M][n - 1]);
  }

  // pending_1m carries forward to the caller. The caller (live ingest
  // or skip-bar synth) sets it up for the next minute. Mark stale here.
  a->pending_1m.populated = false;
}

static void
wm_aggregator_emit_empty_1m(whenmoon_market_t *mk, int64_t bar_start_ms)
{
  wm_candle_full_t  bar;
  double            prev_close = 0.0;
  uint32_t          n;

  // Carry-forward close from the most-recent 1m bar so synthetic bars
  // do not fabricate a price gap. If there is no prior 1m bar this is
  // a market that just started — emit a zero-priced empty bar; the
  // indicator block is NaN-on-empty regardless.
  n = mk->grain_n[WM_GRAN_1M];

  if(n > 0)
    prev_close = mk->grain_arr[WM_GRAN_1M][n - 1].close;

  memset(&bar, 0, sizeof(bar));
  bar.ts_close_ms = bar_start_ms + 60000;
  bar.open        = prev_close;
  bar.high        = prev_close;
  bar.low         = prev_close;
  bar.close       = prev_close;
  bar.volume      = 0.0;

  wm_aggregator_push_bar(mk, WM_GRAN_1M, &bar);

  if(mk->grain_n[WM_GRAN_1M] > 0)
    wm_aggregator_cascade_to(mk, WM_GRAN_5M,
        &mk->grain_arr[WM_GRAN_1M][mk->grain_n[WM_GRAN_1M] - 1]);
}

// ------------------------------------------------------------------ //
// Ring push + indicator pass                                         //
// ------------------------------------------------------------------ //

// Append `bar` to grain_arr[gran], shifting the ring left by one if at
// capacity (so newest is always at grain_n[gran]-1). Then run the
// TA-Lib indicator pass for that bar and fan the closed bar out to
// every attached strategy whose grains_mask includes `gran`.
//
// The strategy fan-out runs under both mk->lock (already held by the
// caller) and the strategy registry lock. Lock order is enforced
// market_lock -> registry_lock; strategy admin commands take only
// the registry lock so the order is consistent.
static void
wm_aggregator_push_bar(whenmoon_market_t *mk, wm_gran_t gran,
    const wm_candle_full_t *bar)
{
  wm_candle_full_t *ring = mk->grain_arr[gran];
  uint32_t          cap  = mk->grain_cap[gran];
  uint32_t          n    = mk->grain_n[gran];
  whenmoon_state_t *st;

  if(ring == NULL || cap == 0 || bar == NULL)
    return;

  if(n == cap)
  {
    memmove(&ring[0], &ring[1], sizeof(*ring) * ((size_t)cap - 1));
    n = cap - 1;
  }

  ring[n] = *bar;
  mk->grain_n[gran]                  = n + 1;
  mk->aggregator->last_close_ms[gran] = bar->ts_close_ms;

  wm_indicators_compute_bar(ring, n + 1, n);

  // Strategy fan-out — fed the just-pushed bar (now in the ring at
  // index n, with indicators populated). Cheap when no strategies are
  // attached; the registry iterates a tiny list under its lock.
  // WM-LT-5: backtest snapshot construction flips dispatch_strategies
  // off so warmup bars don't fire live attachments.
  if(!mk->aggregator->dispatch_strategies)
    return;

  st = whenmoon_get_state();

  if(st != NULL)
    wm_strategy_dispatch_bar(st, mk, gran, &ring[n]);
}

// ------------------------------------------------------------------ //
// Cascade (1m -> 5m -> 15m -> 1h -> 6h -> 1d)                        //
// ------------------------------------------------------------------ //

static void
wm_aggregator_cascade_to(whenmoon_market_t *mk, wm_gran_t target,
    const wm_candle_full_t *src)
{
  wm_aggregator_t  *a;
  wm_work_bucket_t *w;
  int64_t           step_ms;

  if(target >= WM_GRAN_MAX || mk == NULL || src == NULL)
    return;

  a = mk->aggregator;
  w = &a->work[target];

  step_ms = (int64_t)wm_gran_seconds[target] * 1000;

  if(!w->populated)
  {
    w->bar_start_ms = (src->ts_close_ms - step_ms) / step_ms * step_ms;
    w->populated    = true;
    w->open         = src->open;
    w->high         = src->high;
    w->low          = src->low;
    w->close        = src->close;
    w->volume       = src->volume;
    w->inputs_seen  = 1;
  }

  else
  {
    if(src->high > w->high) w->high = src->high;
    if(src->low  < w->low)  w->low  = src->low;
    w->close   = src->close;
    w->volume += src->volume;
    w->inputs_seen++;
  }

  if(w->inputs_seen >= w->inputs_required)
  {
    wm_candle_full_t  bar;

    memset(&bar, 0, sizeof(bar));
    bar.ts_close_ms = w->bar_start_ms + step_ms;
    bar.open        = w->open;
    bar.high        = w->high;
    bar.low         = w->low;
    bar.close       = w->close;
    bar.volume      = w->volume;

    wm_aggregator_push_bar(mk, target, &bar);

    memset(w, 0, sizeof(*w));
    w->inputs_required = wm_inputs_per_grain[target];

    if(target + 1 < WM_GRAN_MAX)
    {
      uint32_t n = mk->grain_n[target];

      if(n > 0)
        wm_aggregator_cascade_to(mk, (wm_gran_t)(target + 1),
            &mk->grain_arr[target][n - 1]);
    }
  }
}

// ------------------------------------------------------------------ //
// Single-bar replay (warm-up + REST live-ring backfill)              //
// ------------------------------------------------------------------ //

void
wm_aggregator_replay_bar(whenmoon_market_t *mk, wm_gran_t gran,
    const wm_candle_full_t *bar)
{
  if(mk == NULL || mk->aggregator == NULL || bar == NULL)
    return;

  // Replay only at 1m for now. Higher grains are reconstructed via
  // cascade so they share computation with the live path. WM-LT-6
  // backtest replay uses the same entry point.
  if(gran != WM_GRAN_1M)
    return;

  // Idempotency: skip duplicates from overlapping warm-up + REST
  // live-ring backfill.
  if(bar->ts_close_ms <= mk->aggregator->last_close_ms[WM_GRAN_1M])
    return;

  wm_aggregator_push_bar(mk, WM_GRAN_1M, bar);
  wm_aggregator_cascade_to(mk, WM_GRAN_5M,
      &mk->grain_arr[WM_GRAN_1M][mk->grain_n[WM_GRAN_1M] - 1]);
}

// ------------------------------------------------------------------ //
// Warm-up loader                                                     //
// ------------------------------------------------------------------ //

// Look up a market by product_id under its containing whenmoon state.
// Returns NULL if the bot has been torn down or the market removed
// since the warmup task was scheduled.
static whenmoon_market_t *
wm_warmup_find_market(whenmoon_state_t *st, const char *product_id)
{
  whenmoon_markets_t *m;
  uint32_t            i;

  if(st == NULL || st->markets == NULL || product_id == NULL)
    return(NULL);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
  {
    if(strncmp(m->arr[i].product_id, product_id,
           COINBASE_PRODUCT_ID_SZ) == 0)
      return(&m->arr[i]);
  }

  return(NULL);
}

void
wm_aggregator_load_history_task(task_t *t)
{
  wm_warmup_ctx_t   *wctx;
  whenmoon_market_t *mk;
  db_result_t       *res = NULL;
  char               table[WM_DL_TABLE_SZ];
  char               sql[512];
  uint32_t           cap;
  uint32_t           limit;
  uint32_t           replayed = 0;
  uint32_t           i;
  int                n;

  if(t == NULL)
    return;

  wctx = t->data;

  if(wctx == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  mk = wm_warmup_find_market(wctx->st, wctx->product_id);

  if(mk == NULL || mk->aggregator == NULL)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: market gone before run, skipping",
        wctx->product_id);
    mem_free(wctx);
    t->state = TASK_ENDED;
    return;
  }

  // Snapshot capacity before issuing the query. The 1m ring sets the
  // ceiling on what's useful to load — anything older would just shift
  // off the front of the ring on push.
  cap   = mk->grain_cap[WM_GRAN_1M];
  limit = cap > 0 ? cap : 1;

  if(wm_candle_table_name(wctx->market_id, COINBASE_GRAN_1M,
         table, sizeof(table)) != SUCCESS)
  {
    mem_free(wctx);
    t->state = TASK_ENDED;
    return;
  }

  // CREATE IF NOT EXISTS so a market that has never been downloaded
  // queries an empty table cleanly rather than erroring on a missing
  // relation.
  (void)wm_candle_table_ensure(wctx->market_id, COINBASE_GRAN_1M);

  n = snprintf(sql, sizeof(sql),
      "SELECT (EXTRACT(EPOCH FROM ts)::BIGINT * 1000) AS ts_ms,"
      "       low, high, open, close, volume"
      "  FROM %s"
      " ORDER BY ts ASC"
      " LIMIT %u",
      table, limit);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    mem_free(wctx);
    t->state = TASK_ENDED;
    return;
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    mem_free(wctx);
    t->state = TASK_ENDED;
    return;
  }

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: query failed (%s) — bot starts cold",
        wctx->product_id,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    db_result_free(res);
    mem_free(wctx);
    t->state = TASK_ENDED;
    return;
  }

  if(res->rows == 0)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: no rows in %s — bot starts cold",
        wctx->product_id, table);
    db_result_free(res);
    mem_free(wctx);
    t->state = TASK_ENDED;
    return;
  }

  // Re-resolve under the lock — the live set could have been mutated
  // (`/bot ... market stop`) between the find above and acquiring the
  // lock. Holding mk->lock across the replay is the contract for
  // wm_aggregator_replay_bar; live trade ingest queues briefly.
  pthread_mutex_lock(&mk->lock);

  for(i = 0; i < res->rows; i++)
  {
    const char       *s_ts     = db_result_get(res, i, 0);
    const char       *s_low    = db_result_get(res, i, 1);
    const char       *s_high   = db_result_get(res, i, 2);
    const char       *s_open   = db_result_get(res, i, 3);
    const char       *s_close  = db_result_get(res, i, 4);
    const char       *s_volume = db_result_get(res, i, 5);
    wm_candle_full_t  bar;
    int64_t           ts_open_ms;

    if(s_ts == NULL || s_low == NULL || s_high == NULL ||
       s_open == NULL || s_close == NULL || s_volume == NULL)
      continue;

    ts_open_ms = (int64_t)strtoll(s_ts, NULL, 10);

    memset(&bar, 0, sizeof(bar));
    bar.ts_close_ms = ts_open_ms + 60000;
    bar.low         = strtod(s_low, NULL);
    bar.high        = strtod(s_high, NULL);
    bar.open        = strtod(s_open, NULL);
    bar.close       = strtod(s_close, NULL);
    bar.volume      = strtod(s_volume, NULL);

    wm_aggregator_replay_bar(mk, WM_GRAN_1M, &bar);
    replayed++;
  }

  pthread_mutex_unlock(&mk->lock);

  clam(CLAM_INFO, WHENMOON_CTX,
      "warmup %s: replayed %u 1m bars from %s (cap=%u)",
      wctx->product_id, replayed, table, cap);

  db_result_free(res);
  mem_free(wctx);
  t->state = TASK_ENDED;
}
