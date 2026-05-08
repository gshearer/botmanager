// live.h — real-mode trade execution skeleton (WM-LT-8-B2).
//
// The whenmoon trade engine has two live sub-modes:
//   PAPER (WM_TRADE_MODE_PAPER) — simulated fills against the cached
//     mark; no orders leave the daemon. Shipped by WM-LT-4.
//   REAL  (WM_TRADE_MODE_LIVE)  — orders submitted to the exchange via
//     coinbase_place_order_async; fills land asynchronously through the
//     `user` WS channel and a /fills REST poll fallback (WM-LT-8-B3).
//
// This module scaffolds the REAL path. The kill-switch KV defaults to
// false, so submission FAILs closed unless an admin explicitly flips
// `plugin.whenmoon.exchange.<exchange>.live=true`. Every gate fails
// closed in the same direction; there is no way to send an order until
// every check passes. The pending-order ring is in-memory only — v1
// hydrates open orders from the gateway on next boot rather than from
// local state.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_LIVE_H
#define BM_WHENMOON_LIVE_H

#ifdef WHENMOON_INTERNAL

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

// Lifecycle: called from whenmoon_init / whenmoon_destroy.
bool wm_live_engine_init(void);
void wm_live_engine_destroy(void);

// Real-mode signal entry point. Caller MUST hold the trade registry
// lock — i.e. the lock that wm_trade_engine_on_signal already takes.
// Reads the kill-switch + risk gates, sizes the trade, mints a fresh
// client_order_id, registers a pending row, and submits the order via
// coinbase_place_order_async. Returns SUCCESS only when an order was
// successfully queued at the exchange abstraction; FAIL on any gate
// trip, sizer-hold, OOM, or submit error. FAIL leaves no pending row
// and no order in flight.
bool wm_live_engine_on_signal_locked(wm_trade_book_t *book,
    double mark_px, int64_t mark_ms, const wm_strategy_signal_t *sig);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_LIVE_H
