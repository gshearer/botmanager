// live.h — real-mode trade execution skeleton (WM-LT-8-B2/B3).
//
// The whenmoon trade engine has three per-(market, strategy) modes:
//   MANUAL — book exists, signals do not act. Operator drives via
//            /whenmoon trade buy|sell.
//   PAPER  — simulated fills against the cached mark on every signal.
//   REAL   — orders submitted to the exchange via
//            coinbase_place_order_async; fills land asynchronously
//            through the `user` WS channel + /fills REST poll
//            fallback.
//
// The plugin-wide master KV `plugin.whenmoon.force_manual` (default
// false) overrides every REAL book to behave as MANUAL until cleared.
// Use it as a single-knob "stop all real trading" switch without
// touching per-book mode state. Risk gates (daily loss bps, max
// notional, pending cap) layer on top of the master + book-mode gate
// for any submission that does land.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_LIVE_H
#define BM_WHENMOON_LIVE_H

#ifdef WHENMOON_INTERNAL

#include "market.h"
#include "order.h"
#include "whenmoon_strategy.h"

#include <stdbool.h>
#include <stdint.h>

// Default basis-point cap on realized PnL since UTC midnight before the
// real path FAILs (200 bps = 2% of starting_cash). Per-(market,strategy)
// override at plugin.whenmoon.market.<m>.strategy.<s>.daily_loss_bps.
#define WM_TRADE_RISK_DEFAULT_DAILY_LOSS_BPS  200

// Default cap on |intent.qty * mark_px| for one order. 0 = uncapped
// (operator must explicitly set). When tripped the qty is clipped, not
// the order rejected — degraded sizing is preferable to no trade.
#define WM_TRADE_RISK_DEFAULT_MAX_NOTIONAL    0.0

// Compile-time cap on pending orders per book. Hitting this cap is a
// signal that something upstream is stuck (the user channel is not
// reaping fills); queueing further orders would compound the problem.
#define WM_TRADE_PENDING_CAP                  32

// Mirror COINBASE_CLIENT_OID_SZ / COINBASE_ORDER_ID_SZ without pulling
// in coinbase_api.h here — the live state is keyed by these strings
// regardless of which exchange filled the slot, and v1 only ships
// against coinbase. Sized to UUID-with-NUL.
#define WM_TRADE_COID_SZ      40
#define WM_TRADE_ORDER_ID_SZ  40
#define WM_TRADE_SIDE_SZ       8

// Per-pending-order tracking row. Matched against incoming fill events
// by client_order_id. Reaped when filled_qty >= submitted_qty (within
// epsilon) or status goes CANCELLED / EXPIRED / FAILED.
typedef struct wm_trade_pending
{
  char     coid[WM_TRADE_COID_SZ];
  char     order_id[WM_TRADE_ORDER_ID_SZ];   // populated on gateway ack
  char     side[WM_TRADE_SIDE_SZ];
  double   limit_px;
  double   submitted_qty;
  double   filled_qty;
  int64_t  submitted_ms;
  bool     gateway_accepted;
} wm_trade_pending_t;

// Trade-id dedup ring per (market, strategy). Sized to absorb the
// largest realistic burst (one fill per 100ms over a 6s window).
#define WM_LIVE_TRADE_DEDUP_CAP   64

// Lifecycle: called from whenmoon_init / whenmoon_destroy.
bool wm_live_engine_init(void);
void wm_live_engine_destroy(void);

// Late-stage start hook. Called from whenmoon_start (after kv_load).
// Schedules the REST /fills safety-net poll periodic and runs the boot
// reconcile (advisory log of any open orders left at the gateway).
// Idempotent.
void wm_live_engine_start(void);

// Hook called from market.c after the active product list mutates.
// (Re)subscribes the user-channel WS for the live trader, gated on
// credentials — when no creds are configured the hook is a no-op. Pass
// n_products = 0 to tear the subscription down (e.g. last market
// removed).
struct whenmoon_state;
void wm_live_ws_resub(struct whenmoon_state *st,
    const char *const *product_ids, size_t n_products);

// Real-mode signal entry point. Caller MUST hold the trade registry
// lock — i.e. the lock that wm_trade_engine_on_signal already takes.
// Reads the master force_manual gate + risk caps, sizes the trade,
// mints a fresh client_order_id, registers a pending row, and submits
// the order via coinbase_place_order_async. Returns SUCCESS only when
// an order was successfully queued at the exchange abstraction; FAIL
// on any gate trip, sizer-hold, OOM, or submit error. FAIL leaves no
// pending row and no order in flight.
bool wm_live_engine_on_signal_locked(wm_trade_book_t *book,
    double mark_px, int64_t mark_ms, const wm_strategy_signal_t *sig);

// Operator-issued real submit. Bypasses the sizer (qty + limit_px are
// supplied) and the master force_manual gate (operator action is
// explicit). Daily-loss + max_notional + pending-cap gates still
// apply. `side` is 'b' or 's'. `errbuf` receives a human-readable
// reason on FAIL (may be NULL). Caller MUST hold the trade registry
// lock.
bool wm_live_engine_operator_submit_locked(wm_trade_book_t *book,
    char side, double qty, double limit_px,
    char *errbuf, size_t errbuf_sz);

// WM-MK-3-B: real-mode market-engine submit. The per-market sibling of
// wm_live_engine_on_signal_locked — reads risk gates from
// `mk->session`, mints a client_order_id, registers a pending row in
// `mk->session.pending[]`, and dispatches coinbase_place_order_async at
// the transactional priority. Caller MUST hold `mk->lock`. Returns
// SUCCESS only when the order was queued at the exchange abstraction;
// FAIL on gate trip, sizer hold, OOM, or submit error (errbuf
// populated when non-NULL). FAIL leaves no pending row.
//
// Master kill-switch lives at `plugin.whenmoon.exchange.coinbase.live`
// (KV_BOOL); when missing or false the helper FAILs closed.
bool wm_market_engine_real_submit_locked(whenmoon_market_t *mk,
    char side, double qty, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig,
    char *errbuf, size_t errbuf_sz);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_LIVE_H
