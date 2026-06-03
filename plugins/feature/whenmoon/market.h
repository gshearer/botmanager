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

// Whenmoon-internal translation units pull the full aggregator +
// exchange-abstraction types transitively. Strategy plugins don't
// define WHENMOON_INTERNAL and see only the opaque forward decls below.
#ifdef WHENMOON_INTERNAL
#include "aggregator.h"
#include "exchange_api.h"
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

// WM-WARMUP-2: per-market warmup lifecycle. Transient — recomputed on
// every (re)start, never persisted. The trade engine acts on strategy
// advice only in WM_WARM_READY (see wm_market_engine_on_signal_with_mk);
// synthetic backtest markets are forced READY at creation since they
// carry a full pre-loaded ring.
typedef enum
{
  WM_WARM_COLD = 0,   // not started
  WM_WARM_WARMING,    // bulk DB gap-fill in flight
  WM_WARM_FINAL,      // backward gap closed; closing the tail to now
  WM_WARM_READY,      // warm; cleared to act (subject to mode + roster)
} wm_warmup_state_t;

const char *wm_warmup_state_name(wm_warmup_state_t s);

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

  // WM-MK-6: per-mode trade outcome counters + drawdown tracker.
  // Incremented inside wm_market_apply_fill_locked on every closing
  // fill (sell against open long that realized PnL). gross_profit /
  // gross_loss are lifetime running totals over realized PnL,
  // partitioned by sign; profit_factor derives as
  // gross_profit / gross_loss without walking the fills ring.
  // max_drawdown is the worst observed (equity_peak - equity) /
  // equity_peak as a positive fraction; equity_peak is the internal
  // anchor (running max).
  uint32_t n_trades;
  uint32_t n_wins;
  uint32_t n_losses;
  double   gross_profit;
  double   gross_loss;
  double   max_drawdown;
  double   equity_peak;
} wm_market_stats_t;

#define WM_MARKET_FILL_RING_CAP    256
#define WM_MARKET_PENDING_CAP       32
#define WM_MARKET_TRADE_DEDUP       64

// WM-MK-6: equity samples ring (shared across modes — see
// wm_market_session_t below). 8192 entries supports ~5-6 days of
// 1-fill-per-bar at 1-minute granularity; the ring slides when full
// so older samples drop out of the Sharpe/Sortino window. Sized to
// stay inside 128 KB per session, dwarfed by the fills+pending rings.
#define WM_MARKET_EQUITY_RING_CAP  8192

typedef struct
{
  int64_t  ts_ms;
  double   equity;
} wm_market_equity_sample_t;

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

  // WM-MK-6: equity-samples ring, shared across modes (the chunk's
  // simplifying scope — a mode-aware ring is future work). Each
  // wm_market_apply_fill_locked invocation appends one sample with
  // (ts_ms, equity_after_fill); the ring slides when full so the
  // Sharpe/Sortino window walks the most recent
  // WM_MARKET_EQUITY_RING_CAP samples. `equity_n` is the lifetime
  // append count; the populated length to read is
  // min(equity_n, WM_MARKET_EQUITY_RING_CAP). `equity_head` indexes
  // the next write slot (i.e. the oldest sample lives at
  // equity_head when the ring is full). Not persisted — restored
  // markets begin with an empty ring; backtest synth markets carry
  // their own per-iteration ring so determinism is automatic.
  wm_market_equity_sample_t  equity_samples[WM_MARKET_EQUITY_RING_CAP];
  uint64_t                   equity_n;
  uint32_t                   equity_head;
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
// exchange convention we support; the generic
// EXCHANGE_PRODUCT_ID_SZ (24) bounds Kraken's longest pair strings,
// Coinbase fits underneath. Strategies see this constant directly.
#define WM_PRODUCT_ID_SZ         24

// Forward decls — opaque to strategy plugins.
struct wm_aggregator;
struct exchange_ws_sub;

// Per-exchange WebSocket subscription binding. One slot per distinct
// exchange in the running set; whenmoon_markets carries an array.
// `exchange_name` mirrors EXCHANGE_NAME_SZ (32) so strategies that
// include market.h don't need exchange_api.h on the path.
typedef struct
{
#ifdef WHENMOON_INTERNAL
  char                     exchange_name[EXCHANGE_NAME_SZ];
#else
  char                     exchange_name[32];  // mirror EXCHANGE_NAME_SZ
#endif
  struct exchange_ws_sub  *ws_sub;
} wm_market_ws_binding_t;

// Compile-time ceiling on the number of distinct exchanges that can
// hold an active WS binding. Mirrors WM_LIVE_MAX_EXCHANGES; lifting
// either is a coordinated change.
#define WM_MARKET_MAX_WS_BINDINGS  8

typedef struct whenmoon_market
{
  // Bound exchange's registered name (e.g. "coinbase", "kraken"). Set
  // at add-time; used by the live trader to route per-market verbs
  // through the exchange abstraction. Strategies can read it to gate
  // venue-specific logic but must not mutate.
#ifdef WHENMOON_INTERNAL
  char                  exchange_name[EXCHANGE_NAME_SZ];
#else
  char                  exchange_name[32];  // mirror EXCHANGE_NAME_SZ
#endif

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
  // lands). last_tick_ms is the event timestamp exchange_ws_ticker_t
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

  // WM-WARMUP-2: warmup lifecycle (transient; not persisted). Zeroed by
  // the add/restore memset, so a fresh slot is COLD with warmup_gen 0.
  // The re-check / tail-fill timers are self-rescheduling deferred tasks
  // guarded by `warmup_gen`: wm_market_warmup_begin bumps it, so any
  // in-flight timer from a prior generation (or a stopped market) sees
  // the mismatch on its next tick, frees its ctx, and stops — no
  // task_cancel needed (which would leave the heap ctx leaked).
  wm_warmup_state_t     warmup_state;
  uint32_t              warmup_gen;

  // WM-REAL-CASH-1: wall-clock ms of the last successful real-mode cash
  // reconciliation against the bound exchange balance (0 = never synced;
  // real cash is still the seeded placeholder). Transient — zeroed by the
  // add/restore memset, so a restored real market reads "unsynced" until
  // the operator re-reconciles. The reconciled cash value itself IS
  // persisted (it lives in session.stats[REAL]).
  int64_t               real_cash_synced_ms;
} whenmoon_market_t;

struct whenmoon_markets
{
  whenmoon_market_t       *arr;       // n_markets live, `cap` allocated
  uint32_t                 n_markets;
  uint32_t                 cap;

  // Per-exchange WebSocket bindings covering {TICKER, TRADES}. One
  // entry per distinct exchange in the running set; the binding's
  // ws_sub aggregates every product_id from markets bound to that
  // exchange. Rebuilt wholesale on every add/remove via
  // wm_market_resub_ws. NULL ws_sub means the most recent subscribe
  // attempt for that exchange failed; the slot stays so a follow-up
  // resub retries.
  wm_market_ws_binding_t   ws_bindings[WM_MARKET_MAX_WS_BINDINGS];
  uint32_t                 n_ws_bindings;
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
// `/whenmoon download <market> [start] [end]` verb explicitly.
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
// plugin start. Returns SUCCESS even if the (exchange, product) pair
// was not present (benign no-op). When `was_present` is non-NULL, it
// is set to true iff the pair was found in the running set.
// `exchange` disambiguates same-product entries from different
// exchanges (e.g. coinbase BTC-USD vs kraken BTC-USD).
bool wm_market_remove(struct whenmoon_state *st,
    const char *exchange, const char *product_id,
    bool persist, bool *was_present,
    char *err, size_t err_cap);

// Plugin-start restore: enumerate `wm_market` rows with enabled=true
// and call wm_market_add(..., persist=false) for each. SUCCESS even
// when zero markets are enabled; FAIL only on a hard DB error.
bool wm_market_restore(struct whenmoon_state *st);

// Parse "<exchange>-<base>-<quote>" (lowercase dash form). Splits on
// '-', requires exactly 3 non-empty tokens, lowercases all output, and
// validates the exchange against the live exchange registry — so any
// registered service plugin (coinbase, kraken, …) is accepted without
// further code changes here. Returns SUCCESS on well-formed input,
// FAIL otherwise.
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

// Exchange-callback hooks — whenmoon-internal. Gated so strategy
// plugins don't pull in exchange types just by including market.h.
#ifdef WHENMOON_INTERNAL

// Async callback invoked by feature_exchange on backfill completion.
// `user` is a heap-owned wm_market_backfill_ctx_t* that the callback
// frees.
void wm_market_on_candles(const exchange_candles_result_t *res,
    void *user);

// WebSocket event fanout — one handler shared across every product.
// `user` is the whenmoon_state_t*. Invoked on the protocol plugin's WS
// reader thread; keep work minimal.
void wm_market_on_event(const exchange_ws_event_t *ev, void *user);

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

// True when at least one market on `exchange_name` is in REAL mode.
// Gates authenticated background polling (live-fills reconcile, account
// balance refresh): paper / manual exchanges must never emit
// authenticated exchange traffic — even when stale credential KVs make
// is_authenticated() report true — because needless private-endpoint
// calls against a non-trading account read as anomalous to an
// exchange's fraud tooling. Caller holds no lock; walks st->markets.
bool wm_market_exchange_has_real_mode(struct whenmoon_state *st,
    const char *exchange_name);

// ------------------------------------------------------------------ //
// WM-MK-OBS-1: per-market session snapshot for /show whenmoon market //
// ------------------------------------------------------------------ //

#define WM_MK_OBS_RECENT_FILLS  16

typedef struct
{
  char                  market_id_str[WM_MARKET_ID_STR_SZ];
  char                  product_id[WM_PRODUCT_ID_SZ];

  wm_market_mode_t      mode;
  wm_warmup_state_t     warmup_state;
  wm_market_position_t  position;
  wm_market_stats_t     stats[WM_MARKET_MODE_COUNT];

  // Most recent fills, oldest at index 0, newest at recent_fills_n-1.
  // Capped at WM_MK_OBS_RECENT_FILLS.
  wm_market_fill_t      recent_fills[WM_MARKET_MODE_COUNT][WM_MK_OBS_RECENT_FILLS];
  uint32_t              recent_fills_n[WM_MARKET_MODE_COUNT];

  uint32_t              pending_n;

  double                last_mark_px;
  int64_t               last_mark_ms;
  double                last_ticker_px;
  int64_t               last_ticker_ms;

  double                fee_bps;
  double                slip_bps;
  double                size_frac;
  double                max_notional;
  double                daily_loss_bps;
  uint32_t              pending_cap;

  // WM-MK-6: pre-computed risk-adjusted metrics. Computed under
  // `mk->lock` in wm_market_session_snapshot from the shared equity
  // ring so off-lock renderers + sweep scoring read scalars rather
  // than re-walking. Per-mode profit factor is derived in callers
  // from stats[mode].gross_profit / gross_loss (see
  // wm_market_stats_profit_factor).
  double                sharpe;
  double                sortino;

  // WM-REAL-CASH-1: 0 = real cash never reconciled with the exchange
  // (stats[REAL].cash is the seeded placeholder, not actual funds).
  int64_t               real_cash_synced_ms;
} wm_market_session_snapshot_t;

// Take a deep copy of `mk->session` (and a few mk-level fields) under
// `mk->lock`, releasing before return. Recent fills tails are extracted
// from the per-mode rings into linear oldest→newest arrays. `mk` must
// be a live pointer from `wm_market_lookup_by_id`. SUCCESS always; the
// helper's only failure mode is OS-level mutex breakage which is fatal
// elsewhere.
bool wm_market_session_snapshot(whenmoon_market_t *mk,
    wm_market_session_snapshot_t *out);

// Register the /show whenmoon market verbs (no-arg list + `<id>` detail
// + `mk` alias). Called from `whenmoon_init` after the existing market
// verb registration. Returns SUCCESS on full success, FAIL if any
// cmd_register call fails (matches sibling `wm_market_register_verbs`
// shape).
bool wm_show_market_register_verbs(void);

// ------------------------------------------------------------------ //
// WM-MK-5: synthetic markets for backtest iterations                  //
// ------------------------------------------------------------------ //
//
// Heap-owned `whenmoon_market_t` instances that share their grain
// rings with a source live market but carry an independent session
// (mode, position, ledgers, pending ring). NOT registered in
// `st->markets->arr` — bypasses WS resub, persistence, and the
// live-tick fanout. Allocated per-iteration; destroyed by the same
// worker after snapshotting.
//
// Lifetime: `src->grain_arr` pointers are shared (read-only during
// iteration). The source market's lifetime spans the entire sweep;
// each iteration is a strict subset. No grain ring is freed before
// the synth market goes away.

// SUCCESS on alloc + init; FAIL on bad args / OOM (errbuf populated
// when non-NULL).
bool wm_market_create_synthetic(const char *market_id_str,
    const whenmoon_market_t *src, whenmoon_market_t **out_mk,
    char *errbuf, size_t errbuf_sz);

// Destroy a synthetic market. Caller asserts no other thread holds
// `mk->lock`. Frees the struct.
void wm_market_destroy_synthetic(whenmoon_market_t *mk);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_H
