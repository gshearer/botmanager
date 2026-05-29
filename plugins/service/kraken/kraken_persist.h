// kraken_persist.h — AssetPairs-cache persistence (EXCH-PRIME-1).
//
// Persists the in-memory assetpairs cache (kr_pairs) to the
// `kraken_assetpairs` Postgres table so a daemon restart primes from
// the DB instead of re-running GET /0/public/AssetPairs on every boot.
// The network refresh fires only when the persisted snapshot is missing
// or older than the staleness window
// (plugin.kraken.assetpairs_refresh_sec). This keeps the synchronous
// network prime out of plugin start() so the operator control socket is
// no longer hostage to exchange prime latency.

#ifndef BM_KRAKEN_PERSIST_H
#define BM_KRAKEN_PERSIST_H

#include "kraken_api.h"     // kraken_assetpairs_result_t, ..._cb_t

#include <stdbool.h>

// Create the kraken_assetpairs table if it does not exist. Idempotent
// (CREATE TABLE IF NOT EXISTS); a per-daemon once-latch suppresses log
// noise when many bots start concurrently. Called from kr_init. The
// only synchronous DB touch on the startup path.
bool kr_assetpairs_ensure_table(void);

// Snapshot the in-memory cache into the kraken_assetpairs table inside a
// single transaction (DELETE + multi-row INSERT). An empty cache still
// clears the DB rows. Called from kr_assetpairs_refresh_persist_cb after
// a successful network refresh.
bool kr_assetpairs_persist(void);

// Async startup prime: load the persisted snapshot from the DB and, if
// it is empty or stale, fire a background network refresh that
// re-persists on completion. Returns immediately. Called from kr_start
// in place of the old synchronous prime barrier.
void kr_assetpairs_load_or_refresh_async(void);

// Completion callback for an assetpairs network refresh: re-persists the
// cache on success, logs a warning (leaving any prior cache intact) on
// failure. Wired as the done-cb for both the startup-triggered refresh
// and the periodic refresh task.
void kr_assetpairs_refresh_persist_cb(const kraken_assetpairs_result_t *res,
    void *user);

#endif // BM_KRAKEN_PERSIST_H
