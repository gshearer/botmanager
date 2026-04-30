// trade_persist.h — buffered async trade persistence (WM-LT-2).
//
// One ring per running market. The WS reader thread calls
// wm_trade_persist_async() under mk->lock to append a trade. A
// plugin-global periodic task drains every registered ring on its tick,
// building one batched INSERT per market (ON CONFLICT DO NOTHING so a
// reconnect that re-sends recent trades dedups by primary key).
//
// Architectural rationale: live trade tape extends forward of the
// Coinbase REST trades endpoint's lookback ceiling. With this in place,
// a Tier-2 strategy run for backtests beyond ~30 days can stitch DB +
// live without an externally-sourced trade archive.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_TRADE_PERSIST_H
#define BM_WHENMOON_TRADE_PERSIST_H

#ifdef WHENMOON_INTERNAL

#include "coinbase_api.h"

#include <stdbool.h>
#include <stdint.h>

struct whenmoon_market;

// Plugin-global lifecycle. Called once from whenmoon_init /
// whenmoon_deinit. Spawns the periodic flush task.
bool wm_trade_persist_global_init(void);
void wm_trade_persist_global_destroy(void);

// Per-market lifecycle. Allocated by wm_market_add after the slot's
// product_id and market_id are populated; freed by wm_market_remove
// before the slot's mutex is destroyed.
//
// `mk->market_id` must be valid at init time — the table-name buffer
// is rendered once and cached. Init also registers the instance on the
// global flush registry; destroy unregisters and frees the ring.
bool wm_trade_persist_init(struct whenmoon_market *mk);
void wm_trade_persist_destroy(struct whenmoon_market *mk);

// Append a trade. Caller MUST already hold `mk->lock`. Never blocks
// and never errors; on overflow drops the oldest unflushed entry to
// make room and emits one rate-limited CLAM_WARN per overflow event.
void wm_trade_persist_async(struct whenmoon_market *mk,
    const coinbase_ws_match_t *m);

// ----------------------------------------------------------------------- //
// Book-state queue (WM-PT-3)                                              //
// ----------------------------------------------------------------------- //
//
// Durable trade-book state — one row per (market_id, strategy_name) in
// `wm_trade_book_state`. Producers (paper fill apply, mode-set, reset)
// hand off a fully-formed SQL statement; the flush task drains the
// queue alongside the trade-tape rings on the same 1 s tick.
//
// Coalescing: at most one pending statement per (market_id,
// strategy_name) key — a second enqueue replaces the first and frees
// its prior SQL. This bounds the pending size to the live book count.
//
// `sql_owned` ownership transfers to the persist subsystem on every
// successful enqueue (return SUCCESS). On FAIL the caller still owns
// the buffer and is responsible for freeing it.
bool wm_book_persist_enqueue(int32_t market_id, const char *strategy_name,
    char *sql_owned);

// Drop the (market_id, strategy_name) row. Issued at book-remove. The
// persist subsystem allocates the DELETE SQL itself; caller passes
// keys only.
bool wm_book_persist_drop(int32_t market_id, const char *strategy_name);

// Drain every pending book-state entry synchronously and run each
// statement on the calling thread. Used at SIGTERM (called from
// wm_trade_persist_global_destroy before task_cancel) so no pending
// snapshot is lost across restart.
void wm_book_persist_flush_all(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_TRADE_PERSIST_H
