// botmanager — MIT
// aggregator.c — multi-grain trade -> candle aggregator.
//
// One aggregator per running market. Drives the cascade
//   trade -> 1m bar -> 5m -> 15m -> 1h -> 4h -> 1d
// rolling each grain's work bucket on TIME boundaries (WM-AGG-1a): a
// bucket covers exactly [bar_start, bar_start + step) and closes the
// instant a source bar lands on or crosses its end, so a bar's label
// always bounds its content. Every closed bar gets pushed onto the
// corresponding grain ring and triggers a TA-Lib indicator pass via
// wm_indicators_compute_bar.
//
// The aggregator runs entirely under `whenmoon_market_t.lock`, so the
// callers (the WS reader thread for live trades; the warm-up task for
// DB replay) take that lock before invoking these entry points and
// release it when they are done.

#define WHENMOON_INTERNAL
#include "aggregator.h"
#include "dl_jobtable.h"
#include "dl_schema.h"
#include "indicators.h"
#include "market.h"
#include "strategy.h"
#include "warmup.h"
#include "whenmoon.h"

#include "db.h"
#include "task.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

const int32_t wm_gran_seconds[WM_GRAN_MAX] =
{
  60,    // 1m
  300,   // 5m
  900,   // 15m
  3600,  // 1h
  14400, // 4h
  86400, // 1d
};

// Forward decls.
static void wm_aggregator_close_1m(whenmoon_market_t *mk);
static void wm_aggregator_emit_empty_1m(whenmoon_market_t *mk,
    int64_t bar_start_ms);
static void wm_aggregator_push_bar(whenmoon_market_t *mk,
    wm_gran_t gran, const wm_candle_full_t *bar);
static void wm_aggregator_bucket_emit(whenmoon_market_t *mk,
    wm_gran_t target, wm_work_bucket_t *w);
static void wm_aggregator_cascade_to(whenmoon_market_t *mk,
    wm_gran_t target, const wm_candle_full_t *src, wm_gran_t src_gran);

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

    memset(mk->grain_arr[g], 0, sz);
    mk->grain_n[g]      = 0;
    mk->grain_cap[g]    = bars;
    a->bars_required[g] = bars;
  }

  mk->aggregator = a;

  clam(CLAM_INFO, WHENMOON_CTX,
      "bot market %s: aggregator init history_1d=%u"
      " (1m=%u 5m=%u 15m=%u 1h=%u 4h=%u 1d=%u)",
      mk->product_id, a->history_1d,
      mk->grain_cap[WM_GRAN_1M], mk->grain_cap[WM_GRAN_5M],
      mk->grain_cap[WM_GRAN_15M], mk->grain_cap[WM_GRAN_1H],
      mk->grain_cap[WM_GRAN_4H], mk->grain_cap[WM_GRAN_1D]);

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
          &mk->grain_arr[WM_GRAN_1M][n - 1], WM_GRAN_1M);
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
        &mk->grain_arr[WM_GRAN_1M][mk->grain_n[WM_GRAN_1M] - 1],
        WM_GRAN_1M);
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
//
// SAN-9: mk->lock is held across the strategy callback and stays held
// — the emit path's re-entry into wm_market_engine_on_signal takes it
// recursively on the same thread. Dropping it there instead is what
// used to invert the order against this very function.
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
// Cascade (1m -> 5m -> 15m -> 1h -> 4h -> 1d)                        //
// ------------------------------------------------------------------ //

// Close `target`'s work bucket: label it bucket-start + step (the
// window it covers), push it, reset the bucket, and feed the closed
// bar into the next grain up. Partial buckets (fewer source bars than
// a full window) are legitimate bars — the label stays honest, the
// content just stops early, exactly what live produces on a quiet
// feed.
static void
wm_aggregator_bucket_emit(whenmoon_market_t *mk, wm_gran_t target,
    wm_work_bucket_t *w)
{
  wm_candle_full_t bar;
  int64_t          step_ms = (int64_t)wm_gran_seconds[target] * 1000;

  memset(&bar, 0, sizeof(bar));
  bar.ts_close_ms = w->bar_start_ms + step_ms;
  bar.open        = w->open;
  bar.high        = w->high;
  bar.low         = w->low;
  bar.close       = w->close;
  bar.volume      = w->volume;

  wm_aggregator_push_bar(mk, target, &bar);

  memset(w, 0, sizeof(*w));

  if(target + 1 < WM_GRAN_MAX)
  {
    uint32_t n = mk->grain_n[target];

    if(n > 0)
      wm_aggregator_cascade_to(mk, (wm_gran_t)(target + 1),
          &mk->grain_arr[target][n - 1], target);
  }
}

// WM-AGG-1a: buckets roll on TIME boundaries, never on input counts.
// The old count-roll (N source bars close one target bar) floored a
// fresh bucket one full target-step early and let every feed gap
// ratchet bucket content past the bar's label — both legs handed
// backtests future prices under the bar's stated close.
//
// `src_gran` is the grain `src` was aggregated at (the cascade always
// feeds `target` from `target - 1`); its step locates the source bar's
// window-open instant for the boundary-crossing test.
static void
wm_aggregator_cascade_to(whenmoon_market_t *mk, wm_gran_t target,
    const wm_candle_full_t *src, wm_gran_t src_gran)
{
  wm_aggregator_t  *a;
  wm_work_bucket_t *w;
  int64_t           step_ms;
  int64_t           src_open_ms;

  if(target >= WM_GRAN_MAX || mk == NULL || src == NULL)
    return;

  a = mk->aggregator;
  w = &a->work[target];

  step_ms     = (int64_t)wm_gran_seconds[target] * 1000;
  src_open_ms = src->ts_close_ms
              - (int64_t)wm_gran_seconds[src_gran] * 1000;

  // A source bar that BEGINS at/after the current bucket's end belongs
  // to a later window: close the bucket as a partial first (its label
  // is already correct — the content just stops early). A bar whose
  // close merely touches the end stays inside the closing bucket.
  if(w->populated && src_open_ms >= w->bar_start_ms + step_ms)
    wm_aggregator_bucket_emit(mk, target, w);

  if(!w->populated)
  {
    // Bucket containing the bar's LAST instant — never the target-step
    // subtraction (that was leg 1 of the lookahead bug).
    w->bar_start_ms = ((src->ts_close_ms - 1) / step_ms) * step_ms;
    w->populated    = true;
    w->open         = src->open;
    w->high         = src->high;
    w->low          = src->low;
    w->close        = src->close;
    w->volume       = src->volume;
  }

  else
  {
    if(src->high > w->high) w->high = src->high;
    if(src->low  < w->low)  w->low  = src->low;
    w->close   = src->close;
    w->volume += src->volume;
  }

  // Complete exactly at the boundary: the 1h bar fires the moment its
  // last source bar closes — the same instant the old count path fired
  // on a gapless stream.
  if(src->ts_close_ms >= w->bar_start_ms + step_ms)
    wm_aggregator_bucket_emit(mk, target, w);
}

// ------------------------------------------------------------------ //
// Single-bar replay (warm-up + REST live-ring backfill)              //
// ------------------------------------------------------------------ //

uint32_t
wm_aggregator_replay_bar(whenmoon_market_t *mk, wm_gran_t gran,
    const wm_candle_full_t *bar)
{
  uint32_t synthesized = 0;

  if(mk == NULL || mk->aggregator == NULL || bar == NULL)
    return(0);

  // Replay only at 1m for now. Higher grains are reconstructed via
  // cascade so they share computation with the live path. WM-LT-6
  // backtest replay uses the same entry point.
  if(gran != WM_GRAN_1M)
    return(0);

  // Idempotency: skip duplicates from overlapping warm-up + REST
  // live-ring backfill.
  if(bar->ts_close_ms <= mk->aggregator->last_close_ms[WM_GRAN_1M])
    return(0);

  // WM-AGG-1b: synthesize the missing minutes between the ring's tail
  // and this bar, mirroring live ingest's skip-bar loop — a replayed
  // cascade must aggregate the same bars a live feed would have
  // produced, or gaps ratchet higher-grain content past its labels.
  // First bar of a stream (last_close == 0) starts cold, like live.
  if(mk->aggregator->last_close_ms[WM_GRAN_1M] != 0)
  {
    int64_t expected = mk->aggregator->last_close_ms[WM_GRAN_1M];

    while(expected < bar->ts_close_ms - 60000)
    {
      wm_aggregator_emit_empty_1m(mk, expected);
      expected += 60000;
      synthesized++;
    }
  }

  // WM-AGG-2: poison-print guard. A bar whose open AND close BOTH sit
  // more than 5x from the last accepted close is a bad exchange print
  // (the 2017-04-15 btc $0.06 flash row bought one backtest 443k BTC),
  // not a market move — drop it and synthesize a carry-forward minute
  // in its place. Intrabar wicks pass untouched: high/low never price
  // a fill, so real flash-crash wicks survive. Synthetic bars carry
  // the last real close forward, so the ring tail is always the last
  // ACCEPTED real close and the recovery bar after a dropped print is
  // judged against sane prices.
  {
    uint32_t n = mk->grain_n[WM_GRAN_1M];

    if(n > 0)
    {
      double prev = mk->grain_arr[WM_GRAN_1M][n - 1].close;

      if(prev > 0.0 &&
         (bar->open  > prev * 5.0 || bar->open  < prev * 0.2) &&
         (bar->close > prev * 5.0 || bar->close < prev * 0.2))
      {
        clam(CLAM_WARN, WHENMOON_CTX,
            "market %s: poison 1m bar at %lld dropped"
            " (o=%.8f c=%.8f prev=%.8f)",
            mk->product_id, (long long)bar->ts_close_ms,
            bar->open, bar->close, prev);

        wm_aggregator_emit_empty_1m(mk, bar->ts_close_ms - 60000);
        return(synthesized + 1);
      }
    }
  }

  wm_aggregator_push_bar(mk, WM_GRAN_1M, bar);
  wm_aggregator_cascade_to(mk, WM_GRAN_5M,
      &mk->grain_arr[WM_GRAN_1M][mk->grain_n[WM_GRAN_1M] - 1],
      WM_GRAN_1M);

  return(synthesized);
}

// Reset grain `gran`'s ring + cursor, then replay `n` bars (which MUST
// be ascending by ts_close_ms) into it with strategy dispatch
// suppressed. Recomputes indicators per bar via push_bar. Push-only:
// does NOT cascade — warmup fetches every subscribed grain directly,
// so cascading a replayed 5m bar into 15m would double-count a grain
// that gets its own fetch. Does not free/realloc grain_arr — synthetic
// backtest markets share the ring pointers; reset-in-place only.
// Caller holds mk->lock.
void
wm_aggregator_warmup_grain(whenmoon_market_t *mk, wm_gran_t gran,
    const wm_candle_full_t *bars, uint32_t n)
{
  wm_aggregator_t *a;
  bool             saved;
  uint32_t         i;

  if(mk == NULL || mk->aggregator == NULL || bars == NULL)
    return;

  if((unsigned)gran >= WM_GRAN_MAX || mk->grain_arr[gran] == NULL)
    return;

  a = mk->aggregator;

  // Reset ring length + cursor so the replay overwrites from index 0.
  // WM-AGG-1: clear the grain's cascade bucket too — the direct fetch
  // supersedes whatever was in flight, and a stale bucket would
  // partial-emit old content into the warmed ring on the next source
  // bar.
  mk->grain_n[gran]      = 0;
  a->last_close_ms[gran] = 0;
  memset(&a->work[gran], 0, sizeof(a->work[gran]));

  // Suppress fan-out across the replay so historical bars do not fire
  // wm_strategy_dispatch_bar; restore the prior flag after.
  saved                  = a->dispatch_strategies;
  a->dispatch_strategies = false;

  for(i = 0; i < n; i++)
    wm_aggregator_push_bar(mk, gran, &bars[i]);

  a->dispatch_strategies = saved;
}

// ------------------------------------------------------------------ //
// Warm-up loader                                                     //
// ------------------------------------------------------------------ //

// WM-WARMUP-HERD-1: plugin-global count of in-flight full-ring DB
// replays. Bounds the remote-Postgres fetch + replay-lock herd on a bulk
// restore so the control plane keeps a worker + DB slot free.
static atomic_uint_fast32_t g_wm_warmup_active = 0;

// Reserve a warmup slot if we're under the cap. Soft cap: momentarily
// oversubscribes by one under contention (fetch_add then back off), which
// is harmless — a single extra replay never reconstitutes the herd.
bool
wm_warmup_try_acquire(void)
{
  uint32_t prev = (uint32_t)atomic_fetch_add(&g_wm_warmup_active, 1);

  if(prev >= WM_WARMUP_MAX_CONCURRENT)
  {
    atomic_fetch_sub(&g_wm_warmup_active, 1);
    return(false);
  }

  return(true);
}

void
wm_warmup_release(void)
{
  atomic_fetch_sub(&g_wm_warmup_active, 1);
}

uint32_t
wm_warmup_active_count(void)
{
  return((uint32_t)atomic_load(&g_wm_warmup_active));
}

// Synchronous DB 1m replay. `limit_override` caps the replay at the
// newest N 1m bars (0 = the full 1m ring capacity). Cascades 1m→…→1d via
// wm_aggregator_replay_bar so every grain warms from one source. MUST be
// called off the cmd thread (issues a DB query + a potentially long
// replay under mk->lock); WM-WARMUP-2 calls it from the warmup task and
// from the convergence re-check (both task threads).
void
wm_aggregator_load_history(whenmoon_state_t *st, const char *market_id_str,
    uint32_t limit_override)
{
  whenmoon_market_t *mk;
  int32_t            market_id;
  db_result_t       *res = NULL;
  char               table[WM_DL_TABLE_SZ];
  char               sql[512];
  uint32_t           cap;
  uint32_t           limit;
  uint32_t           replayed = 0;
  uint32_t           synthesized = 0;
  uint32_t           i;
  uint32_t           g;
  int                n;
  bool               saved_dispatch;
  int64_t            t0;             // WM-WARMUP-HERD-1 STEP 0: timing
  int64_t            fetch_ms = 0;
  int64_t            replay_ms = 0;

  if(st == NULL || st->markets == NULL || market_id_str == NULL)
    return;

  // WM-MKT-ARR-UAF-1: hold rdlock across the whole load — mk is dereferenced
  // throughout the DB query + replay. A concurrent remove of THIS market
  // waits out the replay (rare; warmup runs right after add). Multiple
  // warmups are readers and still run concurrently.
  pthread_rwlock_rdlock(&st->markets->arr_lock);

  // WM-MI-1: resolve the INSTANCE (by its unique market_id_str), not the
  // shared int32 market_id — otherwise every instance of a product warms the
  // first instance's ring and the rest stay at their REST-backfill depth.
  mk = wm_market_lookup_by_id(st, market_id_str);

  if(mk == NULL || mk->aggregator == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: market gone before run, skipping", market_id_str);
    return;
  }

  // Candle history is shared per product, keyed by the int32 registry id.
  market_id = mk->market_id;

  // The 1m ring sets the ceiling on what's useful to load — anything
  // older would just shift off the front on push. A caller-supplied
  // override (the deepest strategy's lookback in bars) narrows it
  // further so we don't replay 200 days to warm a 30-day strategy.
  cap   = mk->grain_cap[WM_GRAN_1M];
  limit = cap > 0 ? cap : 1;

  if(limit_override > 0 && limit_override < limit)
    limit = limit_override;

  if(wm_candle_table_name(market_id, table, sizeof(table)) != SUCCESS)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  // CREATE IF NOT EXISTS so a market that has never been downloaded
  // queries an empty table cleanly rather than erroring on a missing
  // relation.
  (void)wm_candle_table_ensure(market_id);

  // Warm-up must prime the indicators on the MOST RECENT bars so the
  // live grains are contiguous with incoming ticks. Take the newest
  // `limit` rows (ORDER BY ts DESC LIMIT n) in a subquery, then re-sort
  // ascending so the replay loop below pushes them chronologically
  // (oldest -> newest). A bare "ORDER BY ts ASC LIMIT n" would load the
  // OLDEST n bars — for a full-history table that is years-stale data
  // and seeds the indicators on prices unrelated to the live tape.
  n = snprintf(sql, sizeof(sql),
      "SELECT ts_ms, low, high, open, close, volume FROM ("
      "  SELECT (EXTRACT(EPOCH FROM ts)::BIGINT * 1000) AS ts_ms,"
      "         low, high, open, close, volume"
      "    FROM %s"
      "   ORDER BY ts DESC"
      "   LIMIT %u"
      ") sub ORDER BY ts_ms ASC",
      table, limit);

  if(n < 0 || (size_t)n >= sizeof(sql))
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  res = db_result_alloc();

  // STEP 0: bracket the (remote) fetch. wm_dl_now_ms() is monotonic
  // (memory wm_dl_now_ms_is_monotonic) — never wm_now_ms() here.
  t0 = wm_dl_now_ms();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: query failed (%s) — bot starts cold",
        mk->market_id_str,
        res->error[0] != '\0' ? res->error : "(no driver error)");
    db_result_free(res);
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  fetch_ms = wm_dl_now_ms() - t0;

  if(res->rows == 0)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "warmup %s: no rows in %s — bot starts cold",
        mk->market_id_str, table);
    db_result_free(res);
    pthread_rwlock_unlock(&st->markets->arr_lock);
    return;
  }

  // Holding mk->lock across the replay is the contract for
  // wm_aggregator_replay_bar; live trade ingest queues briefly.
  pthread_mutex_lock(&mk->lock);

  // Suppress strategy fan-out across the replay. The grains (and their
  // indicator blocks) still warm via push_bar; we just must not fire
  // on_bar for thousands of historical closes — that would replay stale
  // advice into every attached strategy. (Roster strategies are already
  // attached by the time WM-WARMUP-2 calls this from the convergence
  // re-check.) Restored after the loop.
  saved_dispatch = mk->aggregator->dispatch_strategies;
  mk->aggregator->dispatch_strategies = false;

  // Reset every grain ring + cursor so the replay fully repopulates from
  // the DB. The live WS may have already advanced the 1m cursor past the
  // DB's newest bar (replay_bar dedups on last_close_ms) — without this
  // reset the replay would be a no-op for an already-live market. Live
  // bars are re-established going forward once warmup hands back.
  // WM-AGG-1: clear the cascade buckets too — a live market's in-flight
  // buckets would otherwise partial-emit stale content into the freshly
  // reset rings on the first replayed bar.
  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    mk->grain_n[g]                   = 0;
    mk->aggregator->last_close_ms[g] = 0;
    memset(&mk->aggregator->work[g], 0,
        sizeof(mk->aggregator->work[g]));
  }

  t0 = wm_dl_now_ms();   // STEP 0: bracket the in-memory replay loop

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

    synthesized += wm_aggregator_replay_bar(mk, WM_GRAN_1M, &bar);
    replayed++;
  }

  mk->aggregator->dispatch_strategies = saved_dispatch;

  replay_ms = wm_dl_now_ms() - t0;

  pthread_mutex_unlock(&mk->lock);

  // STEP 0 (WM-WARMUP-HERD-1): one line per run splitting remote-fetch vs
  // replay wall-time + the live concurrent-warmup gauge, so a restart is a
  // measurement (which of fetch/replay dominates, and that the cap holds).
  clam(CLAM_INFO, WHENMOON_CTX,
      "warmup %s: replayed %u 1m bars from %s (limit=%u) synth=%u "
      "fetch=%lldms replay=%lldms rows=%u active=%u",
      mk->market_id_str, replayed, table, limit, synthesized,
      (long long)fetch_ms, (long long)replay_ms, res->rows,
      wm_warmup_active_count());

  db_result_free(res);
  pthread_rwlock_unlock(&st->markets->arr_lock);
}

// Thin task wrapper: replays then frees its heap ctx. Used for the
// feed-only / no-strategy warm (full ring); the strategy path calls
// wm_aggregator_load_history directly from the convergence re-check.
void
wm_aggregator_load_history_task(task_t *t)
{
  wm_warmup_ctx_t *wctx;

  if(t == NULL)
    return;

  wctx = t->data;

  if(wctx == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  // STEP 1 (WM-WARMUP-HERD-1): bound concurrent full-ring replays. Over
  // the cap, re-defer this same task (ctx stays heap-owned) rather than
  // landing another remote-DB fetch on the herd.
  if(!wm_warmup_try_acquire())
  {
    if(task_add_deferred("wm_warmup", TASK_ANY, 200, WM_WARMUP_DEFER_MS,
           wm_aggregator_load_history_task, wctx) == TASK_HANDLE_NONE)
      mem_free(wctx);   // couldn't reschedule — drop rather than leak

    t->state = TASK_ENDED;
    return;
  }

  wm_aggregator_load_history(wctx->st, wctx->market_id_str,
      wctx->limit_override);

  wm_warmup_release();

  mem_free(wctx);
  t->state = TASK_ENDED;
}
