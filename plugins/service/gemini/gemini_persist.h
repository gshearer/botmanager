// gemini_persist.h — Symbol-cache persistence (EXCH-PRIME-1).
//
// Persists the in-memory symbols cache (gemini_pairs) to the
// `gemini_symbols` Postgres table so a daemon restart primes from the
// DB instead of re-running Gemini's N+1 /v1/symbols fan-out on every
// boot. The network refresh fires only when the persisted snapshot is
// missing or older than the staleness window
// (plugin.gemini.symbols_refresh_sec). This keeps the synchronous
// network prime out of plugin start() so the operator control socket is
// no longer hostage to exchange prime latency.

#ifndef BM_GEMINI_PERSIST_H
#define BM_GEMINI_PERSIST_H

#include "gemini_api.h"     // gemini_symbols_result_t, gemini_done_symbols_cb_t

#include <stdbool.h>

// Create the gemini_symbols table if it does not exist. Idempotent
// (CREATE TABLE IF NOT EXISTS); a per-daemon once-latch suppresses log
// noise when many bots start concurrently. Called from gem_init. The
// only synchronous DB touch on the startup path.
bool gem_symbols_ensure_table(void);

// Snapshot the in-memory cache into the gemini_symbols table inside a
// single transaction (DELETE + multi-row INSERT). An empty cache still
// clears the DB rows. Called from gem_symbols_refresh_persist_cb after a
// successful network refresh.
bool gem_symbols_persist(void);

// Async startup prime: load the persisted snapshot from the DB and, if
// it is empty or stale, fire a background network refresh that
// re-persists on completion. Returns immediately. Called from gem_start
// in place of the old synchronous prime barrier.
void gem_symbols_load_or_refresh_async(void);

// Completion callback for a symbols network refresh: re-persists the
// cache on success, logs a warning (leaving any prior cache intact) on
// failure. Wired as the done-cb for both the startup-triggered refresh
// and the periodic refresh task.
void gem_symbols_refresh_persist_cb(const gemini_symbols_result_t *res,
    void *user);

#endif // BM_GEMINI_PERSIST_H
