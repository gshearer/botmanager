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

// Per-grain cascading work-bucket. 1m closes feed 5m's work bucket,
// 5m closes feed 15m's, and so on. WM-AGG-1a: a bucket covers exactly
// [bar_start_ms, bar_start_ms + step) and closes when a source bar
// lands on or crosses its end — time boundaries, never input counts.
typedef struct
{
  bool     populated;
  int64_t  bar_start_ms;
  double   open;
  double   high;
  double   low;
  double   close;
  double   volume;
} wm_work_bucket_t;

typedef struct wm_aggregator
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

  // WM-LT-5: when false, wm_aggregator_push_bar skips the live strategy
  // fan-out. Backtest snapshot construction sets this off so warmup
  // bars don't trigger live attachments. Default true for live markets.
  bool  dispatch_strategies;
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
// rows on market add), the DB warm-up task, and the backtest snapshot
// build. `gran` must be WM_GRAN_1M for now — the cascade upgrades from
// 1m bars only. Idempotent on duplicate ts_close_ms (skipped if
// <= last_close_ms). WM-AGG-1b: gaps between the ring tail and `bar`
// are filled with synthetic carry-forward 1m bars exactly as live
// ingest fills them; returns how many were synthesized (callers fold
// the count into their end-of-stream summary logs — per-bar logging
// would flood on gappy history). Caller must hold `mk->lock`.
uint32_t wm_aggregator_replay_bar(struct whenmoon_market *mk,
    wm_gran_t gran, const wm_candle_full_t *bar);

// WM-WARMUP-1: reset grain `gran`'s ring + cursor and replay `bars`
// (ascending by ts_close_ms) into it with strategy dispatch + cascade
// suppressed. Recomputes indicators per bar. Push-only — the warmup
// path fetches every subscribed grain directly, so cascading a
// replayed bar would double-count a grain that gets its own fetch.
// Does not free/realloc grain_arr (synthetic markets share the ring
// pointers); reset-in-place only. Caller holds mk->lock.
void wm_aggregator_warmup_grain(struct whenmoon_market *mk,
    wm_gran_t gran, const wm_candle_full_t *bars, uint32_t n);

// Warm-up loader. Heap-owned context; the task frees it when done.
// WM-MI-1: re-resolves the market by its instance-unique `market_id_str`
// (NOT the shared int32 market_id, which collapses all instances of a
// product onto the first one — the warmup would then pour every instance's
// DB replay into a single ring). A removed market between scheduling and
// run bails cleanly. Reads the shared `wm_candles_<market_id>` table
// chronologically and replays through wm_aggregator_replay_bar into THIS
// instance's rings so the cascade backfills 5m..1d before any live trade.
struct whenmoon_state;
struct task;

typedef struct
{
  struct whenmoon_state *st;
  // Instance-unique canonical id ("<exch>-<base>-<quote>[@<instance>]").
  // 64 mirrors WM_MARKET_ID_STR_SZ (market.h — cannot #include here: it
  // includes aggregator.h, so the dependency is one-way only).
  char                   market_id_str[64];
  uint32_t               limit_override;   // 0 = full 1m ring capacity
} wm_warmup_ctx_t;

// Synchronous DB 1m replay (newest `limit_override` bars, or the full 1m
// ring when 0). Cascades 1m→…→1d. Must run off the cmd thread.
void wm_aggregator_load_history(struct whenmoon_state *st,
    const char *market_id_str, uint32_t limit_override);

void wm_aggregator_load_history_task(struct task *t);

// WM-WARMUP-HERD-1 concurrency gate around the full-ring DB replay.
// try_acquire increments the plugin-global active-warmup counter iff it
// is below WM_WARMUP_MAX_CONCURRENT and returns true; otherwise it makes
// no change and returns false (the caller re-defers). release decrements
// (call once per successful acquire, on every exit path). active_count
// is a live gauge for logging.
bool     wm_warmup_try_acquire(void);
void     wm_warmup_release(void);
uint32_t wm_warmup_active_count(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_AGGREGATOR_H
