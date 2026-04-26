// market.h — per-market state (candles, live px) for whenmoon bots.
//
// Internal to the whenmoon plugin. Consumers outside this plugin must
// NOT include this header; cross-plugin access would have to go through
// a future public `whenmoon_api.h` dlsym shim, which does not yet
// exist. Gate: WHENMOON_INTERNAL.

#ifndef BM_WHENMOON_MARKET_H
#define BM_WHENMOON_MARKET_H

#ifdef WHENMOON_INTERNAL

#include "aggregator.h"
#include "coinbase_api.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Initial capacity for the per-bot market array. Grows via
// mem_realloc as `wm_market_add` inserts; there is no hard cap.
#define WM_MARKET_INIT_CAP       8

// Forward decl — defined in trade_persist.h, opaque to most callers.
struct wm_trade_persist;

typedef struct whenmoon_market
{
  char                  product_id[COINBASE_PRODUCT_ID_SZ];

  // Registry id from wm_market (Postgres). Cached at add-time so the
  // market verbs and ad-hoc download verbs don't re-resolve on every
  // call. -1 means "not resolved yet" (should not happen once
  // wm_market_add returns SUCCESS).
  int32_t               market_id;

  // Multi-grain candle rings. `grain_cap[g]` slots of
  // `wm_candle_full_t`; `grain_n[g]` populated, oldest at index 0,
  // newest at `grain_n[g] - 1`. Once full, the aggregator shifts the
  // ring left by one to make room for the new bar (memmove cost is
  // small at 200-day capacities and avoids ring-buffer wrap-around in
  // the indicator computation hot path).
  wm_candle_full_t     *grain_arr[WM_GRAN_MAX];
  uint32_t              grain_n[WM_GRAN_MAX];
  uint32_t              grain_cap[WM_GRAN_MAX];

  // Owned per-market aggregator. NULL until wm_aggregator_init runs in
  // wm_market_add; teardown via wm_aggregator_destroy in
  // wm_market_remove / wm_market_destroy.
  wm_aggregator_t      *aggregator;

  // Buffered trade-persist ring. Allocated by wm_trade_persist_init
  // from wm_market_add; freed by wm_trade_persist_destroy.
  struct wm_trade_persist *trade_persist;

  // Last observed ticker price (0.0 until the first ticker event
  // lands). last_tick_ms is the event timestamp coinbase_ws_ticker_t
  // reports; 0 when unparseable.
  double                last_px;
  int64_t               last_tick_ms;

  pthread_mutex_t       lock;
} whenmoon_market_t;

struct whenmoon_markets
{
  whenmoon_market_t    *arr;       // n_markets live, `cap` allocated
  uint32_t              n_markets;
  uint32_t              cap;

  // One shared WebSocket subscription covering every product_id +
  // {HEARTBEAT, TICKER, MATCHES}. NULL when n_markets == 0 or
  // coinbase_ws_subscribe failed at resub time. Rebuilt on every
  // add/remove via coinbase_ws_unsubscribe + coinbase_ws_subscribe.
  coinbase_ws_sub_t    *ws_sub;
};

// Forward decl to keep this header independent of whenmoon.h.
struct whenmoon_state;

// Init: allocates the empty container. No DB or KV reads here — the
// running set is populated by wm_market_restore (bot-start path) or
// wm_market_add (user `/bot … market start` verb). SUCCESS even when
// the bot has never started any markets; FAIL on allocation failure.
bool wm_market_init(struct whenmoon_state *st);

// Destroy: unsubscribes, destroys per-market mutexes, frees the
// container. Safe to call on a zero-initialised (null `markets`) state.
// Does NOT mutate the `wm_bot_market` table — the rows survive daemon
// restart so wm_market_restore can pick up where the bot left off.
void wm_market_destroy(struct whenmoon_state *st);

// Add a market to the running set.
//
// - `exchange`, `base`, `quote`, `product_id`: normalised by the
//   caller. `product_id` is the Coinbase-style "BASE-QUOTE" (uppercase).
// - `persist`: when true, idempotently INSERT into wm_bot_market so the
//   assignment survives a daemon restart. Pass false from the restore
//   path (the row already exists) and from future internal callers who
//   manage persistence themselves.
//
// Effects: (1) resolve/create the wm_market registry row, (2) grow and
// append to `st->markets->arr`, (3) INSERT into wm_bot_market if
// `persist`, (4) rebuild the WS subscription with the updated product
// list, (5) kick a one-shot live-ring backfill for the new product
// (300 rows of 1m candles via REST).
//
// History coverage for trading is the strategy layer's responsibility
// (WM-LT-3) — this function does NOT enqueue any catch-up download.
// Operators who need a deeper history can drive the
// `/bot <name> download candles …` verbs explicitly.
//
// Dedup: silently returns SUCCESS if the product is already in the
// running set. FAIL on DB/alloc errors; writes a terse diagnostic
// into `err` (optional; pass NULL to suppress).
bool wm_market_add(struct whenmoon_state *st,
    const char *exchange, const char *base, const char *quote,
    const char *product_id, bool persist,
    char *err, size_t err_cap);

// Remove a market from the running set. `persist=true` also DELETEs
// the wm_bot_market row so the market does not resume on next daemon
// start. Returns SUCCESS even if the product was not present (benign
// no-op). When `was_present` is non-NULL, it is set to true iff the
// product was found in the running set; callers can use this to
// distinguish "stopped" from "not running" in user-facing replies.
bool wm_market_remove(struct whenmoon_state *st,
    const char *product_id, bool persist, bool *was_present,
    char *err, size_t err_cap);

// Bot-start restore: enumerate the bot's rows in wm_bot_market and
// call wm_market_add(..., persist=false) for each. SUCCESS even when
// the bot has zero running markets; FAIL only on a hard DB error.
bool wm_market_restore(struct whenmoon_state *st);

// Async callback invoked by coinbase on backfill completion. `user` is
// a heap-owned wm_market_backfill_ctx_t* that the callback frees.
void wm_market_on_candles(const coinbase_candles_result_t *res, void *user);

// WebSocket event fanout — one handler shared across every product.
// `user` is the whenmoon_state_t*. Invoked on the coinbase WS reader
// thread; keep work minimal.
void wm_market_on_event(const coinbase_ws_event_t *ev, void *user);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_H
