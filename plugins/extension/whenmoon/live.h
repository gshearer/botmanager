// live.h — real-mode trade execution skeleton.
//
// Per-market path (the only path post-WM-MK-5):
//   wm_market_engine_on_signal -> wm_market_engine_real_submit_locked
//   (under mk->lock) -> exchange_place_order_async(mk->exchange_name, ...).
//   Fills land asynchronously via the user WS channel + REST /fills poll.
//
// No per-exchange enable switch exists — registration of the exchange
// (creds present + market in REAL mode) is the only gate beyond the
// per-market risk caps (daily_drawdown_bps, max_notional_sats,
// pending_cap, mark_max_age_ms — the first two denominated in satoshis
// per WM-NUMERAIRE-1).
// Operator-side halt is /whenmoon manual, which flips every market into
// MANUAL mode and short-circuits the real-submit path.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_LIVE_H
#define BM_WHENMOON_LIVE_H

#ifdef WHENMOON_INTERNAL

#include "market.h"
#include "whenmoon_strategy.h"

#include <stdbool.h>
#include <stdint.h>

// Lifecycle: called from whenmoon_init / whenmoon_destroy.
bool wm_live_engine_init(void);
void wm_live_engine_destroy(void);

// Cancel the REST /fills poll periodic. Called from whenmoon_stop so
// core's pre-deinit barrier stands between the last tick and the lock
// wm_live_engine_destroy tears down (OBS-44). Idempotent.
void wm_live_engine_stop(void);

// Late-stage start hook. Called from whenmoon_start (after kv_load).
// Schedules the REST /fills safety-net poll periodic and runs the boot
// reconcile (advisory log of any open orders left at the gateway).
// Idempotent.
void wm_live_engine_start(void);

// Hook called from market.c after the active product list mutates.
// Reconciles the user-channel WS bindings against the current running
// set: markets are grouped by exchange and one user-channel WS is held
// per exchange whose credentials are configured. Exchanges without
// credentials are skipped silently — once creds appear the next market
// mutation binds them. Passing a state with zero markets (or NULL)
// drops every binding.
//
// OBS-41: a DIFF, not a rebuild — an exchange whose product set already
// matches its binding is not unsubscribed. `stale_exchange` carries the
// same meaning as in wm_market_resub_ws: NULL for a market mutation,
// otherwise the provider that just (re)registered and is rebuilt
// unconditionally because its handles are dead.
struct whenmoon_state;
void wm_live_ws_resub_all(struct whenmoon_state *st,
    const char *stale_exchange);

// Per-market real-mode market-engine submit. Reads risk gates from
// `mk->session`, mints a client_order_id, registers a pending row in
// `mk->session.pending[]`, and dispatches exchange_place_order_async
// (routed via `mk->exchange_name`) at the transactional priority.
// Caller MUST hold `mk->lock`. Returns SUCCESS only when the order was
// queued at the exchange abstraction; FAIL on gate trip, sizer hold,
// OOM, or submit error (errbuf populated when non-NULL). FAIL leaves
// no pending row. Gate cascade: credentials, daily satoshi drawdown,
// pending-cap, per-order satoshi cap (clips the qty; refuses outright
// when the market has no bitcoin leg to convert against), mark
// staleness.
//
// OBS-62: `px_named` says the price came from the operator rather than
// from a mark this plugin inferred (a force-trade's explicit px), and
// waives the staleness gate alone. `mark_ms` is recorded, never trusted
// for freshness — a strategy sets it for itself; the gate measures
// `mk->last_feed_ms`.
bool wm_market_engine_real_submit_locked(whenmoon_market_t *mk,
    char side, double qty, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig, bool px_named,
    char *errbuf, size_t errbuf_sz);

// WM-NUMERAIRE-1: treasury freeze tripwire (CFO.md sec. 3). Called
// after a fill is recorded and the fill path's locks are released: real
// exchange fills (record_external_fill) and synth-mode operator force
// trades (the market force verb). Reads plugin.whenmoon.treasury.*
// fresh; no-op until the fund is configured (baseline_sats,
// freeze_frac, markets all set) and the filled market is designated.
// The fund's size is summed in SATOSHIS, each designated market's book
// chosen by its mode (PAPER -> paper book, REAL/MANUAL -> real book),
// so a bitcoin price move cannot breach the floor. On breach flips
// every designated market to MANUAL (positions kept) and emits one
// TREASURY-FREEZE CLAM_WARN.
void wm_live_treasury_freeze_check(const char *filled_market_id_str);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_LIVE_H
