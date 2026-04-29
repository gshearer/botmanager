// aggregator.h — multi-grain candle types + indicator-block layout.
//
// WM-LT-1 lands the type definitions; WM-LT-2 lands the in-memory
// per-grain rings and the bar-close aggregator that populates them.
// Strategies (WM-LT-3+) read closed bars by indexing `ind[]` via the
// `WM_IND_*` slot enum; the layout is fixed by enum + version, so
// adding indicators must NEVER shift existing slot ids.
//
// Internal to the whenmoon plugin. Consumers outside this plugin must
// NOT include this header. Gate: WHENMOON_INTERNAL.
//
// WM-LT-3 moved the public bar / grain / indicator typedefs into
// whenmoon_strategy.h so strategy plugins (a separate dlopen unit)
// see the same struct layout the aggregator writes.

#ifndef BM_WHENMOON_AGGREGATOR_H
#define BM_WHENMOON_AGGREGATOR_H

#ifdef WHENMOON_INTERNAL

#define WHENMOON_STRATEGY_INTERNAL
#include "whenmoon_strategy.h"
#undef WHENMOON_STRATEGY_INTERNAL

#include "coinbase_api.h"

#include <stdbool.h>
#include <stdint.h>

// Per-bar bucket-second values, indexed by `wm_gran_t`. Defined in
// `aggregator.c`; declared extern here so callers can convert from
// grain enum to seconds without a switch.
extern const int32_t wm_gran_seconds[WM_GRAN_MAX];

// Forward decl — full struct in market.h, also WHENMOON_INTERNAL.
struct whenmoon_market;

// In-flight 1m bucket. Closed and pushed into grain_arr[WM_GRAN_1M] on
// the first trade that crosses the next minute boundary, or synthesized
// empty when the gap is > 1 minute.
typedef struct
{
  int64_t  bar_start_ms;
  bool     populated;
  double   open;
  double   high;
  double   low;
  double   close;
  double   volume;
} wm_pending_bucket_t;

// Per-grain cascading work-bucket. 1m closes feed 5m's work bucket;
// when 5 inputs accumulate, 5m closes and feeds 15m, and so on.
typedef struct
{
  bool     populated;
  int64_t  bar_start_ms;
  double   open;
  double   high;
  double   low;
  double   close;
  double   volume;
  uint32_t inputs_seen;
  uint32_t inputs_required;
} wm_work_bucket_t;

typedef struct
{
  wm_pending_bucket_t  pending_1m;

  // Strategy-declared history requirement (1d-equivalent units). Until
  // WM-LT-3's strategy registry can declare per-strategy requirements,
  // wm_aggregator_init takes this as a constructor argument.
  uint32_t  history_1d;

  // Sized exactly to history_1d * (86400 / wm_gran_seconds[g]).
  uint32_t  bars_required[WM_GRAN_MAX];

  int64_t   last_close_ms[WM_GRAN_MAX];

  // index 0 (1m) has no upstream and is unused.
  wm_work_bucket_t  work[WM_GRAN_MAX];
} wm_aggregator_t;

// Default history requirement until WM-LT-3 lets strategies declare it.
#define WM_AGG_DEFAULT_HISTORY_1D  200

bool wm_aggregator_init(struct whenmoon_market *mk,
    uint32_t history_1d_min);
void wm_aggregator_destroy(struct whenmoon_market *mk);

// Live ingest. `ts_ms` is the event time in milliseconds (Coinbase WS
// `time_ms`). `price` and `size` are the trade fields. Caller must hold
// `mk->lock`.
void wm_aggregator_on_trade(struct whenmoon_market *mk,
    int64_t ts_ms, double price, double size);

// Single-bar replay path. Used by the REST live-ring backfill (300 1m
// rows on market add) and by the DB warm-up task. `gran` must be
// WM_GRAN_1M for now — the cascade upgrades from 1m bars only.
// Idempotent on duplicate ts_close_ms (skipped if <= last_close_ms).
// Caller must hold `mk->lock`.
void wm_aggregator_replay_bar(struct whenmoon_market *mk,
    wm_gran_t gran, const wm_candle_full_t *bar);

// Warm-up loader. Heap-owned context; the task frees it when done.
// Re-resolves the market by product_id under the markets container so
// that a removed market between scheduling and run bails cleanly. Reads
// `wm_candles_<market_id>_60` chronologically and replays through
// wm_aggregator_replay_bar so the cascade backfills 5m..1d before any
// live trade arrives.
struct whenmoon_state;
struct task;

typedef struct
{
  struct whenmoon_state *st;
  int32_t                market_id;
  char                   product_id[COINBASE_PRODUCT_ID_SZ];
} wm_warmup_ctx_t;

void wm_aggregator_load_history_task(struct task *t);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_AGGREGATOR_H
