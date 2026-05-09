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

// ------------------------------------------------------------------ //
// WM-MK-2: market position model — installed but not yet wired       //
// ------------------------------------------------------------------ //
//
// `wm_market_session_t` lives on every running market and carries the
// position, two independent stat ledgers (paper + real), per-mode fills
// rings, the pending-order ring, the per-market mode, the cached
// risk/economic params, and the last-acted strategy signal. New
// signal/fill entry points (`wm_market_engine_*`) operate on this struct
// but no production caller wires them yet — WM-MK-3 swaps consumers
// onto them, WM-MK-5 rips the legacy per-(market, strategy) book path.

typedef enum
{
  WM_MARKET_MODE_MANUAL = 0,
  WM_MARKET_MODE_PAPER  = 1,
  WM_MARKET_MODE_REAL   = 2,
} wm_market_mode_t;

#define WM_MARKET_MODE_COUNT     3   // for stats[] / fills[] sizing only

typedef enum
{
  WM_MARKET_POS_FLAT = 0,
  WM_MARKET_POS_LONG = 1,
} wm_market_position_side_t;

typedef struct
{
  wm_market_position_side_t side;
  double                    qty;
  double                    avg_entry_px;
  int64_t                   opened_at_ms;
} wm_market_position_t;

typedef struct
{
  double   starting_cash;
  double   cash;
  double   realized_pnl_lifetime;
  double   realized_pnl_today;
  int64_t  daily_anchor_ms;
  double   lifetime_fees;
  uint64_t lifetime_fills_count;
  int64_t  last_fill_ms;
} wm_market_stats_t;

#define WM_MARKET_FILL_RING_CAP  256
#define WM_MARKET_PENDING_CAP     32
#define WM_MARKET_TRADE_DEDUP     64

// Mirror of wm_fill_t (order.h:95) — same shape so a future verifier
// can memcmp rings as a sanity check, but declared here so callers do
// not pull order.h.
typedef struct
{
  int64_t  ts_ms;
  char     side;             // 'b' (buy) / 's' (sell)
  double   qty;
  double   price;
  double   fee;
  double   slippage;
  double   realized_pnl;
  double   cash_after;
  double   position_after;   // signed
  char     reason[64];
} wm_market_fill_t;

typedef struct
{
  char     coid[64];
  char     order_id[64];
  char     side[8];
  double   limit_px;
  double   submitted_qty;
  double   filled_qty;
  int64_t  submitted_ms;
  bool     gateway_accepted;
  int64_t  recorded_trade_ids[WM_MARKET_TRADE_DEDUP];
  uint8_t  n_recorded_trades;
} wm_market_pending_t;

typedef struct
{
  wm_market_mode_t      mode;
  wm_market_position_t  position;

  // Indexed by mode = paper / real. Manual-mode slot is sized for
  // consistency; force-trades land in WM-MK-4 and route to whichever
  // ledger the caller selects.
  wm_market_stats_t     stats[WM_MARKET_MODE_COUNT];

  wm_market_fill_t      fills[WM_MARKET_MODE_COUNT][WM_MARKET_FILL_RING_CAP];
  uint64_t              fills_n[WM_MARKET_MODE_COUNT];
  uint32_t              fills_head[WM_MARKET_MODE_COUNT];

  wm_market_pending_t   pending[WM_MARKET_PENDING_CAP];
  uint32_t              pending_n;

  double                last_mark_px;
  int64_t               last_mark_ms;

  wm_strategy_signal_t  last_acted_signal;
  bool                  has_last_acted_signal;

  // Cached params — lazy-loaded from plugin.whenmoon.market.<id>.*
  // on session init / explicit refresh.
  double                fee_bps;
  double                slip_bps;
  double                size_frac;
  double                max_notional;
  double                daily_loss_bps;
  uint32_t              pending_cap;
} wm_market_session_t;

#define WM_MARKET_DEFAULT_STARTING_CASH    10000.0
#define WM_MARKET_DEFAULT_FEE_BPS              5.0
#define WM_MARKET_DEFAULT_SLIP_BPS             5.0
#define WM_MARKET_DEFAULT_SIZE_FRAC            0.25
#define WM_MARKET_DEFAULT_DAILY_LOSS_BPS     200.0
#define WM_MARKET_DEFAULT_MAX_NOTIONAL         0.0
#define WM_MARKET_DEFAULT_PENDING_CAP            8u

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

  // Last observed ticker price (0.0 until the first ticker event
  // lands). last_tick_ms is the event timestamp coinbase_ws_ticker_t
  // reports; 0 when unparseable.
  double                last_px;
  int64_t               last_tick_ms;

  pthread_mutex_t       lock;

  // WM-MK-2: market position model. Append-only — existing field
  // offsets above stay stable so any external consumer mirroring early
  // offsets is unaffected. Defaults installed in wm_market_add via
  // wm_market_session_init. Production callers do not yet route here;
  // WM-MK-3 swaps strategy + live-fill consumers onto it.
  wm_market_session_t   session;
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

// Build the exchange wire-form symbol "BASE-QUOTE" (uppercase) from
// already-parsed lowercase base/quote tokens. Output is empty on
// truncation. WM-OR-1: shared between market_cmds.c and the new
// /whenmoon order verbs.
void wm_market_wire_symbol(const char *base, const char *quote,
    char *out, size_t out_sz);

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

// ------------------------------------------------------------------ //
// WM-MK-2: market session helpers                                     //
// ------------------------------------------------------------------ //
//
// Parse / print the market mode token. Strings: "manual", "paper",
// "real". Parser is case-insensitive.
bool        wm_market_mode_parse(const char *tok, wm_market_mode_t *out);
const char *wm_market_mode_name(wm_market_mode_t m);

// Default-init a freshly-zeroed session. Called from wm_market_add
// after the slot mutex is in place. Sets mode = PAPER, position = flat,
// stats[].starting_cash + cash = WM_MARKET_DEFAULT_STARTING_CASH for
// every mode slot, both fills rings empty, no pending orders. Cached
// params are seeded with the WM_MARKET_DEFAULT_* macros; the lazy
// KV-refresh helper (WM-MK-2 Phase C) replaces them on first access.
void        wm_market_session_init(wm_market_session_t *s);

// Lookup helper used by the new engine entry points + the selftest.
// Returns the live `whenmoon_market_t *` whose canonical id matches
// `market_id_str`, or NULL if absent. Caller does NOT hold any lock;
// the search walks `st->markets->arr` linearly. The returned pointer
// is valid until the next add/remove (i.e. for the duration of a
// single dispatch) — callers should grab `mk->lock` immediately and
// not retain across re-entries.
whenmoon_market_t *wm_market_lookup_by_id(struct whenmoon_state *st,
    const char *market_id_str);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_H
