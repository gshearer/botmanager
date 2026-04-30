// dl_coverage.h — coverage interval store + gap computation for the
// whenmoon downloader. Internal; WHENMOON_INTERNAL-gated.
//
// Coverage is the single source of truth for "do we have this data".
// Every successful page insert in WM-S4/S5 ends with a call to
// wm_coverage_add(); every pre-dispatch job window passes through
// wm_coverage_gaps_* to prune sub-ranges already covered.

#ifndef BM_WHENMOON_DL_COVERAGE_H
#define BM_WHENMOON_DL_COVERAGE_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WM_COV_TS_SZ      40    // "2026-04-22 00:00:00+00" + slack
#define WM_COV_SOURCE_SZ  16    // matches schema VARCHAR(16)

// Coverage-merge gap thresholds. Adjacent paginations are unioned in
// place; non-adjacent intervals stay separate rows. Trades enforce
// touching in BOTH ID space AND timestamp space; a row that touches in
// only one dimension signals a corrupted boundary and rejects the
// merge with CLAM_WARN. Candles have no ID axis so the timestamp-only
// predicate is widened to 2*gran with a 30-day sanity check that no
// touching row's range can pull iv past a multi-year jump.
#define WM_COV_ID_MERGE_GAP          64     // trade-id slack
#define WM_COV_TS_MERGE_GAP_SECONDS  600    // 10 min wall-clock slack
#define WM_COV_TS_SANITY_DAYS        30     // candle out-of-band ceiling

typedef enum
{
  WM_COV_TRADES  = 0,
  WM_COV_CANDLES = 1
} wm_coverage_kind_t;

// One contiguous coverage interval. For trades, first_ts / last_ts are
// advisory (human-readable admin output); first_trade_id /
// last_trade_id are authoritative and drive the merge. For candles,
// first_ts / last_ts are authoritative; id fields are ignored (must be
// 0). Granularity is candles-only (must be 0 for trades). All
// timestamps are Postgres canonical TIMESTAMPTZ strings in UTC form
// ("YYYY-MM-DD HH:MM:SS+00" or "YYYY-MM-DD HH:MM:SS.ffffff+00").
typedef struct
{
  int32_t  market_id;
  int64_t  first_trade_id;
  int64_t  last_trade_id;
  char     first_ts[WM_COV_TS_SZ];
  char     last_ts[WM_COV_TS_SZ];
  int32_t  granularity;
  char     source[WM_COV_SOURCE_SZ];
} wm_coverage_t;

// Merge an interval into the coverage store. Returns SUCCESS on a
// committed write, FAIL on SQL error. Overlapping and touching rows
// are unioned under one transaction (see wm_cov_merge_tx for the exact
// sequence) and serialised against concurrent writers for the same
// (market, kind, granularity) via pg_advisory_xact_lock.
bool wm_coverage_add(wm_coverage_kind_t kind, const wm_coverage_t *iv);

// Complement of coverage rows in [range_start, range_end). Sorted
// ascending. `out` is caller-allocated, capacity `max_out`; returns
// the count actually written. If the complement would exceed max_out,
// the last slot spans the tail (fine for admin display; callers that
// need exact lists must grow max_out).
uint32_t wm_coverage_gaps_trades(int32_t market_id,
    const char *range_start, const char *range_end,
    wm_coverage_t *out, uint32_t max_out);

uint32_t wm_coverage_gaps_candles(int32_t market_id, int32_t gran_secs,
    const char *range_start, const char *range_end,
    wm_coverage_t *out, uint32_t max_out);

typedef void (*wm_coverage_iter_cb_t)(const wm_coverage_t *row,
    void *user);

// Walk existing coverage rows for admin output. `gran_secs` is ignored
// for WM_COV_TRADES. Rows are delivered in ascending start order. Not
// under any lock — callers must be tolerant of rows appearing /
// disappearing mid-iteration (the downloader is transactionally
// merging concurrently).
void wm_coverage_iterate(wm_coverage_kind_t kind, int32_t market_id,
    int32_t gran_secs, wm_coverage_iter_cb_t cb, void *user);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_COVERAGE_H
