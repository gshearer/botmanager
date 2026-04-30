// dl_schema.h — DDL + market registry resolver for whenmoon downloader.
//
// Internal. WHENMOON_INTERNAL-gated like siblings market.h / account.h.
// The downloader proper (scheduler, worker, coverage tracker) lands in
// WM-S2+. S1 exposes only the metadata-schema bootstrap and the
// (exchange, base, quote, symbol) -> wm_market.id resolver.

#ifndef BM_WHENMOON_DL_SCHEMA_H
#define BM_WHENMOON_DL_SCHEMA_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WM_DL_CTX         "whenmoon.dl"

// Buffer size for per-pair table names. The widest name we render is
// "wm_candles_<int32_t>_<int32_t>" where both int32_ts max out at 10
// decimal digits: 11 ("wm_candles_") + 10 + 1 ("_") + 10 + NUL = 33.
// 40 gives us comfortable slack and aligns nicely.
#define WM_DL_TABLE_SZ    40

struct whenmoon_state;

// Lifecycle — called from whenmoon_start_cb / whenmoon_destroy.
// wm_dl_init runs the core DDL (metadata tables) once per daemon and
// flips st->dl_ready on success. Subsequent calls on a second bot
// observe the per-process guard and no-op the DDL pass, still flipping
// st->dl_ready. Returns SUCCESS on success, FAIL on DDL error.
bool wm_dl_init(struct whenmoon_state *st);

// S1 stub: clears st->dl_ready. Scheduler / worker teardown lands in
// WM-S4. No-op on NULL.
void wm_dl_destroy(struct whenmoon_state *st);

// Market resolution. Returns -1 on failure (NULL args, OOM on escape,
// SQL error, or CB cache miss on a create path for a non-coinbase
// exchange); writes the registry id on success. Idempotent: lookup
// hits an existing row, the create path runs only on first encounter.
// For "coinbase" entries the create path consults the product cache to
// fill in base_increment / quote_increment; other exchanges INSERT
// with NULL increments.
//
// WM-DC-1: when `exchange == "coinbase"` and coinbase_sandbox_active()
// is true, the resolver stores `wm_market.exchange = "coinbase-sb"`
// so sandbox markets occupy a distinct registry slot (and therefore a
// distinct wm_trades_<id> table) from prod. Already-qualified inputs
// ("coinbase-sb") pass through unchanged so wm_market_restore can
// re-add rows without double-suffixing.
int32_t wm_market_lookup_or_create(const char *exchange,
    const char *base_asset, const char *quote_asset,
    const char *exchange_symbol);

// Table-name rendering. Writes "wm_trades_<id>" / "wm_candles_<id>_<g>"
// into `out`. Returns SUCCESS on success, FAIL when `out`/`cap` cannot
// hold the result or `market_id` / `gran_secs` are negative.
bool wm_trade_table_name(int32_t market_id, char *out, size_t cap);
bool wm_candle_table_name(int32_t market_id, int32_t gran_secs,
    char *out, size_t cap);

// Lazy per-pair DDL. Idempotent (CREATE TABLE IF NOT EXISTS + CREATE
// INDEX IF NOT EXISTS); safe to call on every page insert. Callers
// SHOULD cache the "ensured" bit per-job to avoid hammering Postgres
// with redundant DDL runs in the hot path.
bool wm_trade_table_ensure(int32_t market_id);
bool wm_candle_table_ensure(int32_t market_id, int32_t gran_secs);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_DL_SCHEMA_H
