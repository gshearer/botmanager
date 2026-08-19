// market_persist.h — durable per-market session queue (WM-MK-2).
//
// One row per `market_id` in `wm_market_state` carrying the new
// position + paper/real ledgers + fills rings + pending ring + cached
// params. Producers (`wm_market_apply_fill_locked`, `wm_market_set_mode`,
// `wm_market_reset`) enqueue UPSERT statements; the persist subsystem
// coalesces by `market_id`, drains on a 1 s tick, and runs each
// statement off-lock so DB latency never blocks the on-fill path.
//
// Mirrors `book_persist.{c,h}` (WM-PT-3) by design — the patterns are
// identical because the constraints are identical (debounce a hot
// writer, never block on DB latency, drain at SIGTERM).
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_MARKET_PERSIST_H
#define BM_WHENMOON_MARKET_PERSIST_H

#ifdef WHENMOON_INTERNAL

#include "market.h"

#include <stdbool.h>
#include <stdint.h>

// Plugin-global lifecycle. Called once from whenmoon_init /
// whenmoon_deinit. Spawns the periodic flush task that drains the
// market-state queue every WM_MP_FLUSH_INTERVAL_MS.
bool wm_market_persist_global_init(void);
void wm_market_persist_global_destroy(void);

// Cancel the flush periodic. Called from whenmoon_stop so the task is
// off the queues before the unload, rather than one tick ahead of a
// mapping that is about to go (OBS-44). The final flush stays in
// wm_market_persist_global_destroy. Idempotent.
void wm_market_persist_global_stop(void);

// Snapshot one market's session under `mk->lock` and enqueue the
// UPSERT. Caller MUST hold `mk->lock`. Allocations happen on the hot
// path, but the SQL is built into a single heap buffer per call so
// ring serialization stays tight (~32 KiB / market worst case).
bool wm_market_persist_locked(whenmoon_market_t *mk);

// WM-MI-2: mark the (market_id, instance) session out of the running
// set (enabled=FALSE) while keeping its row. Issued at
// `wm_market_remove` so a stopped instance no longer restores on the
// next daemon start, yet its final paper P&L stays inspectable. A later
// re-start of the same instance overwrites the row from a fresh session.
bool wm_market_persist_disable(int32_t market_id, const char *instance);

// Drain every pending entry synchronously and run each statement on
// the calling thread. Used at SIGTERM (the first act of
// `wm_market_persist_global_destroy`) so the final snapshot survives a
// restart.
void wm_market_persist_flush_all(void);

// Plugin-start restore: enumerate `wm_market_state` rows + hydrate
// the running market's session for each match. Called from
// whenmoon_start AFTER `wm_market_restore` so the running set is
// non-empty. Rows whose market_id is not in the running set are left
// untouched (operator may re-enable the market later).
bool wm_market_persist_restore_all(struct whenmoon_state *st);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_MARKET_PERSIST_H
