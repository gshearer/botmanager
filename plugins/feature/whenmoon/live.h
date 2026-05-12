// live.h — real-mode trade execution skeleton.
//
// Per-market path (the only path post-WM-MK-5):
//   wm_market_engine_on_signal -> wm_market_engine_real_submit_locked
//   (under mk->lock) -> exchange_place_order_async(mk->exchange_name, ...).
//   Fills land asynchronously via the user WS channel + REST /fills poll.
//
// Master kill-switch lives at
// `plugin.whenmoon.exchange.<exchange>.live` (KV_BOOL, default false)
// — one per registered exchange. Per-market risk caps
// (daily_loss_bps, max_notional, pending_cap) layer on top once the
// master is enabled for that exchange.
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
// (Re)subscribes the user-channel WS for the live trader against the
// supplied `exchange_name`, gated on credentials — when no creds are
// configured the hook is a no-op. Pass n_products = 0 to tear the
// subscription down (e.g. last market removed). KR-2 single-exchange
// limitation: only one bound exchange at a time; KR-5 will partition.
struct whenmoon_state;
void wm_live_ws_resub(struct whenmoon_state *st,
    const char *exchange_name,
    const char *const *product_ids, size_t n_products);

// Per-market real-mode market-engine submit. Reads risk gates from
// `mk->session`, mints a client_order_id, registers a pending row in
// `mk->session.pending[]`, and dispatches exchange_place_order_async
// (routed via `mk->exchange_name`) at the transactional priority.
// Caller MUST hold `mk->lock`. Returns SUCCESS only when the order was
// queued at the exchange abstraction; FAIL on gate trip, sizer hold,
// OOM, or submit error (errbuf populated when non-NULL). FAIL leaves
// no pending row.
//
// Master kill-switch lives at
// `plugin.whenmoon.exchange.<mk->exchange_name>.live` (KV_BOOL); when
// missing or false the helper FAILs closed.
bool wm_market_engine_real_submit_locked(whenmoon_market_t *mk,
    char side, double qty, double mark_px, int64_t mark_ms,
    const wm_strategy_signal_t *sig,
    char *errbuf, size_t errbuf_sz);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_LIVE_H
