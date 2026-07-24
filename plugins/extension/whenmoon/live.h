// live.h — real-mode trade execution skeleton.
//
// Per-market path (the only path post-WM-MK-5):
//   wm_market_engine_on_signal -> wm_market_engine_real_submit_locked
//   (under mk->lock) -> exchange_place_order_async(mk->exchange_name, ...).
//   Fills land asynchronously via the user WS channel + REST /fills poll.
//
// No per-exchange enable switch exists — registration of the exchange
// (creds present + market in REAL mode) is the only gate beyond the
// per-market risk caps (daily_loss_bps, max_notional, pending_cap).
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

// Late-stage start hook. Called from whenmoon_start (after kv_load).
// Schedules the REST /fills safety-net poll periodic and runs the boot
// reconcile (advisory log of any open orders left at the gateway).
// Idempotent.
void wm_live_engine_start(void);

// Hook called from market.c after the active product list mutates.
// Reconciles the user-channel WS bindings against the current running
// set: tears down all prior bindings, then groups markets by exchange
// and (re)subscribes one user-channel WS per exchange whose
// credentials are configured. Exchanges without credentials are
// skipped silently — once creds appear the next market mutation will
// retry. Passing a state with zero markets tears every binding down.
struct whenmoon_state;
void wm_live_ws_resub_all(struct whenmoon_state *st);

// Per-market real-mode market-engine submit. Reads risk gates from
// `mk->session`, mints a client_order_id, registers a pending row in
// `mk->session.pending[]`, and dispatches exchange_place_order_async
// (routed via `mk->exchange_name`) at the transactional priority.
// Caller MUST hold `mk->lock`. Returns SUCCESS only when the order was
// queued at the exchange abstraction; FAIL on gate trip, sizer hold,
// OOM, or submit error (errbuf populated when non-NULL). FAIL leaves
// no pending row. Gate cascade: credentials, daily_loss_bps,
// pending-cap, max-notional clip.
bool wm_market_engine_real_submit_locked(whenmoon_market_t *mk,
    char side, double qty, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig,
    char *errbuf, size_t errbuf_sz);

// WM-DISC-1: discretionary-treasury freeze tripwire (CFO.md sec. 3).
// Called after a fill is recorded and the fill path's locks are
// released: real exchange fills (record_external_fill) and synth-mode
// operator force trades (the market force verb). Reads
// plugin.whenmoon.disc.* fresh; no-op until the fund is configured
// (deposit_usd, freeze_frac, markets all set) and the filled market is
// designated. Equity reads each designated market's book by its mode
// (PAPER -> paper book, REAL/MANUAL -> real book). On breach flips
// every designated market to MANUAL (positions kept) and emits one
// DISC-FREEZE CLAM_WARN.
void wm_live_disc_freeze_check(const char *filled_market_id_str);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_LIVE_H
