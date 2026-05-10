// market_engine.h — WM-MK-2 market position engine.
//
// New per-market state machine: position + paper/real stat ledgers +
// fills rings + pending-order ring + cached params. Lives on
// `whenmoon_market_t.session` (see market.h). Production callers do not
// yet route through these entry points — WM-MK-3 swaps the strategy
// emit path + live external-fill consumer onto this engine, WM-MK-5
// rips the legacy per-(market, strategy) book registry.
//
// Locking: every public entry point takes `mk->lock` internally. The
// `_locked` variants assume the caller already holds it (used by the
// selftest verb and by future WM-MK-3 dispatchers that need to compose
// multiple engine ops atomically).
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_MARKET_ENGINE_H
#define BM_WHENMOON_MARKET_ENGINE_H

#ifdef WHENMOON_INTERNAL

#include "market.h"
#include "whenmoon_strategy.h"

#include <stdbool.h>
#include <stdint.h>

// Apply a fill to the market session under `mk->lock`. Updates
// position (open/extend/close — long-only in WM-MK-2), updates
// stats[mode], appends to fills[mode], records the equity sample, and
// maintains the daily-loss anchor. Caller MUST hold `mk->lock`.
//
// `side` is 'b' (buy) / 's' (sell). Mismatched fills (flat+sell or
// oversell beyond open long qty) drop a CLAM_WARN and no-op — short
// support is a future chunk.
//
// `reason` may be NULL; copied into the fill ring slot's reason field.
void wm_market_apply_fill_locked(whenmoon_market_t *mk,
    wm_market_mode_t mode, char side, double qty, double exec_px,
    double fee, int64_t ts_ms, const char *reason);

// Set the market mode. Returns SUCCESS on a valid value AND a flat
// position; FAIL otherwise (`errbuf` filled, may be NULL).
bool wm_market_set_mode(const char *market_id_str, wm_market_mode_t mode,
    char *errbuf, size_t errbuf_sz);

// Reset one stat ledger + clear that mode's fills ring. If
// `mode_to_reset == mk->session.mode` AND the position is non-flat,
// the position is also flattened (no synthetic fill — the operator is
// asserting the ledger is fresh; flatten silently).
void wm_market_reset(const char *market_id_str,
    wm_market_mode_t mode_to_reset);

// Refresh cached params from `plugin.whenmoon.market.<id>.*`. Lazy-
// register on miss so the operator can `/set kv …` without touching
// any C registration table. Caller may hold `mk->lock` or not — KV
// reads are independent.
void wm_market_session_refresh_kv(whenmoon_market_t *mk);

// New signal entry point. Idempotent w.r.t. position direction:
// no-op when the strategy advice already matches the current state.
// Risk gates (daily-loss, max-notional, pending-cap) apply only when
// `mode == WM_MARKET_MODE_REAL`. **Not yet wired by any production
// caller** — installed for WM-MK-3 to swap the strategy emit path
// onto and for the selftest verb to drive directly.
void wm_market_engine_on_signal(const char *market_id_str,
    double mark_px, int64_t mark_ms, const wm_strategy_signal_t *sig);

// New external-fill entry point (post-confirmation, no synthetic
// slip/fee). Internal lookup, lock, dedup-by-trade_id, dispatch to
// `wm_market_apply_fill_locked` with `mode = WM_MARKET_MODE_REAL`.
// **Not yet wired** — WM-MK-3 swaps live.c WS-user-channel + REST
// `/fills` poll onto this entry point.
void wm_market_engine_record_external_fill(const char *market_id_str,
    int64_t trade_id, char side, double qty, double exec_px,
    double fee, int64_t ts_ms, const char *reason);

// WM-MK-4: operator-issued force trade. Bypasses strategy advisors and
// the MANUAL-mode "no auto-action" rule. Routes by `mk->session.mode`:
//
//   MANUAL / PAPER -> wm_market_apply_fill_locked (synth fee +
//                     paper-only synth slippage when no operator px).
//                     Pre-flighted for sell-against-flat so the verb
//                     can reply meaningfully; oversell beyond open
//                     long is silently clipped by apply_fill_locked.
//   REAL           -> wm_market_engine_real_submit_locked unchanged
//                     (master kill-switch + credentials + daily-loss
//                     + pending-cap + max-notional gates apply).
//
// Resolved exec px (synth) / limit px (real):
//   px_override > 0.0   -> use exactly (no synth slippage)
//   px_override <= 0.0  -> mk->last_px, then mk->session.last_mark_px
//                          (paper applies synth slippage only on this
//                          fallback path)
//
// `reason` may be NULL; truncated into the fill ring's reason slot
// (synth modes) or recorded in the audit line only (real mode —
// coinbase has no operator-reason field on place_order).
//
// Caller MUST hold `mk->lock`. Returns SUCCESS when the fill applied
// (synth) or the order queued at the exchange abstraction (real).
// FAIL on bad args, missing live mark fallback, sell-against-flat
// (synth), oversell-clip no-op (synth), or any real-mode gate trip;
// `errbuf` populated.
bool wm_market_engine_force_trade_locked(whenmoon_market_t *mk,
    char side, double qty, double px_override, int64_t ts_ms,
    const char *reason, char *errbuf, size_t errbuf_sz);

// Selftest body — synthesises a buy + sell against the new path,
// asserts position transitions and stats accumulation, returns
// SUCCESS/FAIL. `errbuf` (may be NULL) carries a one-line summary on
// FAIL. Verb implementation in market_cmds.c calls this; WM-MK-5
// removes both verb and body.
bool wm_market_engine_selftest(const char *market_id_str,
    char *errbuf, size_t errbuf_sz);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_ENGINE_H
