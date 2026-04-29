// market.h — per-market state (candles, live px) for whenmoon bots.
//
// Public to strategy plugins. The strategy ABI (whenmoon_strategy.h)
// hands strategies a `const struct whenmoon_market *mkt` on every
// bar / trade callback; that pointer's fields are defined here. The
// per-grain bar rings (`grain_arr[g]`) are the supported way for a
// strategy to read history: oldest bar at index 0, newest at
// `grain_arr[g][grain_n[g] - 1]`. The aggregator shifts the ring
// left on overflow, so the newest slot stays addressable as
// `grain_n[g] - 1` always.
//
// Lifecycle / mutation functions further down in this header are
// whenmoon-internal and must not be called from strategy plugins.
// Strategies treat opaque pointers (aggregator, trade_persist) as
// pointers they neither dereference nor free.

#ifndef BM_WHENMOON_MARKET_H
#define BM_WHENMOON_MARKET_H

#include "whenmoon_strategy.h"   // wm_candle_full_t + wm_gran_t + WM_GRAN_MAX

// Whenmoon-internal translation units pull the full aggregator + coinbase
// types transitively. Strategy plugins don't define WHENMOON_INTERNAL and
// see only the opaque forward decls below.
#ifdef WHENMOON_INTERNAL
#include "aggregator.h"
#include "coinbase_api.h"
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Initial capacity for the plugin-global market array. Grows via
// mem_realloc as `wm_market_add` inserts; there is no hard cap.
#define WM_MARKET_INIT_CAP       8

// Canonical market id buffer. Holds "<exchange>-<base>-<quote>" lowercase
// (e.g. "coinbase-btc-usd"). 64 bytes is generous: longest realistic
// triple is ~31 (e.g. "coinbase-1000pepe-usd"). Used for display and
// for KV path interpolation.
#define WM_MARKET_ID_STR_SZ      64

// Wire-form product id buffer (e.g. "BTC-USD"). Sized to the largest
// exchange convention we support; coinbase uses 16 bytes (see
// COINBASE_PRODUCT_ID_SZ in plugins/service/coinbase/coinbase_api.h).
// Defined here so this header is independent of any exchange plugin.
#define WM_PRODUCT_ID_SZ         16

// Forward decls — opaque to strategy plugins.
struct wm_trade_persist;
struct wm_aggregator;
struct coinbase_ws_sub;

typedef struct whenmoon_market
{
  // Wire-form symbol the exchange uses on the WS / REST APIs. For
  // Coinbase this is the uppercase dash form, e.g. "BTC-USD".
  char                  product_id[WM_PRODUCT_ID_SZ];

  // Canonical id — the user-facing form, lowercase dash-joined with
  // the exchange prefix: "coinbase-btc-usd". This is what shows up in
  // /show whenmoon markets and what /whenmoon market start consumes.
  char                  market_id_str[WM_MARKET_ID_STR_SZ];

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

  // Owned per-market aggregator. Opaque to strategies (forward-decl
  // only) — the bar rings above are the supported strategy surface.
  // NULL until wm_aggregator_init runs in wm_market_add; teardown via
  // wm_aggregator_destroy in wm_market_remove / wm_market_destroy.
  struct wm_aggregator *aggregator;

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
  whenmoon_market_t       *arr;       // n_markets live, `cap` allocated
  uint32_t                 n_markets;
  uint32_t                 cap;

  // One shared WebSocket subscription covering every product_id +
  // {HEARTBEAT, TICKER, MATCHES}. NULL when n_markets == 0 or
  // coinbase_ws_subscribe failed at resub time. Rebuilt on every
  // add/remove via coinbase_ws_unsubscribe + coinbase_ws_subscribe.
  // Opaque to strategy plugins.
  struct coinbase_ws_sub  *ws_sub;
};

// Forward decl to keep this header independent of whenmoon.h.
struct whenmoon_state;

// Init: allocates the empty container. No DB or KV reads here — the
// running set is populated by wm_market_restore on plugin start or
// wm_market_add (user `/whenmoon market start` verb). SUCCESS even
// when no markets have been started; FAIL on allocation failure.
bool wm_market_init(struct whenmoon_state *st);

// Destroy: unsubscribes, destroys per-market mutexes, frees the
// container. Safe to call on a zero-initialised (null `markets`) state.
// Does NOT mutate the `wm_market.enabled` flag — rows survive daemon
// restart so wm_market_restore can pick up where the plugin left off.
void wm_market_destroy(struct whenmoon_state *st);

// Add a market to the running set. Markets are plugin-global.
//
// - `exchange`, `base`, `quote`, `product_id`: normalised by the
//   caller. `product_id` is the Coinbase-style "BASE-QUOTE" (uppercase).
// - `persist`: when true, flips wm_market.enabled = true so the market
//   resumes on next plugin start. Pass false from the restore path
//   (the row is already enabled) and from internal callers who manage
//   persistence themselves.
//
// Effects: (1) resolve/create the wm_market registry row, (2) grow and
// append to `st->markets->arr`, (3) flip wm_market.enabled = true if
// `persist`, (4) rebuild the WS subscription with the updated product
// list, (5) kick a one-shot live-ring backfill for the new product
// (300 rows of 1m candles via REST).
//
// History coverage for trading is the strategy layer's responsibility
// (WM-LT-3) — this function does NOT enqueue any catch-up download.
// Operators who need a deeper history can drive the
// `/whenmoon download candles …` verbs explicitly.
//
// Dedup: silently returns SUCCESS if the product is already in the
// running set. FAIL on DB/alloc errors; writes a terse diagnostic
// into `err` (optional; pass NULL to suppress).
bool wm_market_add(struct whenmoon_state *st,
    const char *exchange, const char *base, const char *quote,
    const char *product_id, bool persist,
    char *err, size_t err_cap);

// Remove a market from the running set. `persist=true` flips
// wm_market.enabled = false so the market does not resume on next
// plugin start. Returns SUCCESS even if the product was not present
// (benign no-op). When `was_present` is non-NULL, it is set to true
// iff the product was found in the running set.
bool wm_market_remove(struct whenmoon_state *st,
    const char *product_id, bool persist, bool *was_present,
    char *err, size_t err_cap);

// Plugin-start restore: enumerate `wm_market` rows with enabled=true
// and call wm_market_add(..., persist=false) for each. SUCCESS even
// when zero markets are enabled; FAIL only on a hard DB error.
bool wm_market_restore(struct whenmoon_state *st);

// Parse "<exchange>-<base>-<quote>" (lowercase dash form). Splits on
// '-', requires exactly 3 non-empty tokens, lowercases all output, and
// validates exchange against the hard-coded allowlist (currently just
// "coinbase"). Returns SUCCESS on well-formed input, FAIL otherwise.
bool wm_market_parse_id(const char *id,
    char *exchange, size_t exch_sz,
    char *base,     size_t base_sz,
    char *quote,    size_t quote_sz);

// Format canonical id from parts. Output is lowercase, dash-joined.
void wm_market_format_id(const char *exchange, const char *base,
    const char *quote, char *out, size_t out_sz);

// Coinbase-callback hooks — whenmoon-internal. Gated so strategy
// plugins don't pull in coinbase types just by including market.h.
#ifdef WHENMOON_INTERNAL

// Async callback invoked by coinbase on backfill completion. `user` is
// a heap-owned wm_market_backfill_ctx_t* that the callback frees.
void wm_market_on_candles(const coinbase_candles_result_t *res,
    void *user);

// WebSocket event fanout — one handler shared across every product.
// `user` is the whenmoon_state_t*. Invoked on the coinbase WS reader
// thread; keep work minimal.
void wm_market_on_event(const coinbase_ws_event_t *ev, void *user);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_H
