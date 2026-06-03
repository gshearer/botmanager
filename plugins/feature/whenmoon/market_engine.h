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
#include "exchange_api.h"

#include <stdbool.h>
#include <stdint.h>

// WM-MK-6: risk-adjusted metric helpers. Computed from the shared
// session equity-samples ring + per-mode stats counters. The Sharpe /
// Sortino helpers walk the ring oldest→newest, compute simple returns
// r[i] = (eq[i] - eq[i-1]) / eq[i-1], and return the per-trade ratio
// (unannualized — the legacy wm_pnl_acc_t was per-trade too). Less
// than two returns yields 0.0. Profit factor is gross_profit /
// gross_loss; with no losses we return 0.0 (NOSCORE-friendly) so
// sweep scoring sorts these to the bottom rather than reporting +inf.
double wm_market_stats_sharpe(const wm_market_stats_t *st,
    const wm_market_equity_sample_t *ring, uint64_t total_n,
    uint32_t head);

double wm_market_stats_sortino(const wm_market_stats_t *st,
    const wm_market_equity_sample_t *ring, uint64_t total_n,
    uint32_t head);

double wm_market_stats_profit_factor(const wm_market_stats_t *st);

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

// Operator halt: flip every registered market into MANUAL mode,
// bypassing the flat-position rule (the whole point is "stop new
// orders NOW even on markets mid-trade"). Open positions stay in their
// current ledger and freeze — strategies still log advice but no new
// fills are produced until the operator switches the market back
// (which still requires flat per wm_market_set_mode) or unwinds via
// /whenmoon market force.
//
// Walks `whenmoon_state->markets->arr[]` only (synthetic backtest
// markets allocated by wm_market_create_synthetic are not in this
// list and are not touched). Persists each transition. Writes counts
// into the out-params when non-NULL: total markets visited, count
// that held an open long position at halt time.
void wm_market_halt_all(uint32_t *out_visited, uint32_t *out_with_position);

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

// Read a per-market double knob `plugin.whenmoon.market.<id>.<suffix>`,
// lazy-registering it (with `def_str` + `help`) on first access so the
// operator can `/set kv …` it without a static registration table;
// returns `def_val` if registration fails. Exposed for the real-cash
// reconcile path (live.c), which reads the quote-allocation knobs fresh.
//
// WM-QUOTE-ALLOC-1 knob ordering — the three real-mode sizing knobs
// compose, from bankroll to order:
//   1. quote_alloc_frac / quote_alloc_max  shape the *bankroll*: bound the
//      per-market real `cash` ledger to a fraction of, and/or an absolute
//      ceiling on, the shared quote `available` (most-restrictive wins).
//      Applied inside wm_live_apply_real_cash_locked at reconcile time, to
//      both cash and (on a baseline re-anchor) starting_cash — so the
//      daily-loss cap is relative to allocated capital, not the full
//      balance.
//   2. size_frac    sizes each order as a fraction of that capped cash.
//   3. max_notional caps the resulting per-order notional.
double wm_mk_kv_get_double(const char *market_id_str, const char *suffix,
    const char *def_str, double def_val, const char *help);

// Signal entry point. Idempotent w.r.t. position direction:
// no-op when the strategy advice already matches the current state.
// Risk gates (daily-loss, max-notional, pending-cap) apply only when
// `mode == WM_MARKET_MODE_REAL`.
//
// The id-based wrapper resolves the live market via
// wm_market_lookup_by_id then forwards to the direct-pointer entry.
// Use the _with_mk form when the caller already owns a pointer
// (production strategy dispatch + backtest synthetic markets).
void wm_market_engine_on_signal(const char *market_id_str,
    double mark_px, int64_t mark_ms, const wm_strategy_signal_t *sig);

void wm_market_engine_on_signal_with_mk(whenmoon_market_t *mk,
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
//                     (credentials + daily-loss + pending-cap +
//                     max-notional gates apply).
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

// WM-REAL-CASH-1: reconcile a market's REAL-mode cash ledger against the
// live quote-currency `available` balance on its bound exchange. Real
// order sizing is `size_frac * stats[REAL].cash`; without this the ledger
// is the seeded paper placeholder and real orders bet a fictional
// bankroll. Defined in live.c.
//
// Fetches the account snapshot SYNCHRONOUSLY (blocks up to
// WM_EXCH_QUERY_WAIT_MS) — call only from a command / operator thread,
// NEVER from the aggregator / bar-close path. Acquires `mk->lock`
// internally; the caller must NOT hold it. On SUCCESS writes the resolved
// available balance into stats[REAL].{cash,starting_cash}, stamps
// mk->real_cash_synced_ms, persists, and (if non-NULL) returns the value
// via `out_cash`. FAIL with `errbuf` populated on missing credentials,
// fetch timeout, or the quote currency being absent from the account.
bool wm_market_reconcile_real_cash(whenmoon_market_t *mk, double *out_cash,
    char *errbuf, size_t errbuf_sz);

// Auto-reconcile real cash for every FLAT market bound to `exchange` from
// an already-fetched accounts snapshot — makes NO exchange call (free of
// the WM-PAPER-GATE-1 polling concern; safe on the curl-worker or a
// command thread). For each flat market it resolves the quote currency in
// `rows` and sets stats[REAL].cash to the available balance; starting_cash
// (the daily-loss-cap baseline) is only re-anchored on the market's first
// sync, so periodic auto-reconcile never silently moves an established
// risk baseline. Markets holding an open long are skipped — the quote
// balance understates deployable cash while capital sits in the base
// asset; the market's own fill ledger is authoritative until it closes
// flat. Driven by the balance cache (account.c) so per-market real cash
// auto-syncs to actual funds without a manual `/whenmoon market sync`.
// Defined in live.c.
void wm_live_reconcile_from_accounts(const char *exchange,
    const exchange_account_t *rows, uint32_t n);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_ENGINE_H
