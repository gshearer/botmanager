// book_persist.h — durable trade-book state queue (WM-PT-3).
//
// One row per (market_id, strategy_name) in `wm_trade_book_state` —
// the authoritative restart record for the live + paper trade engine.
// Producers (paper fill apply, mode-set, reset, remove) hand off a
// fully-formed SQL statement; the persist subsystem coalesces by
// (market_id, strategy_name) key, drains on a 1 s tick, and runs each
// statement off-lock so DB latency never blocks the on-fill path.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_BOOK_PERSIST_H
#define BM_WHENMOON_BOOK_PERSIST_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stdint.h>

// Plugin-global lifecycle. Called once from whenmoon_init /
// whenmoon_deinit. Spawns the periodic flush task that drains the
// book-state queue every WM_BP_FLUSH_INTERVAL_MS.
bool wm_book_persist_global_init(void);
void wm_book_persist_global_destroy(void);

// Enqueue a fully-formed UPSERT statement keyed by (market_id,
// strategy_name). Coalescing: at most one pending statement per key —
// a second enqueue replaces the first and frees its prior SQL,
// bounding the pending-list size to the live book count.
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

// Drain every pending entry synchronously and run each statement on
// the calling thread. Used at SIGTERM (called from
// wm_book_persist_global_destroy before task_cancel) so no pending
// snapshot is lost across restart.
void wm_book_persist_flush_all(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_BOOK_PERSIST_H
