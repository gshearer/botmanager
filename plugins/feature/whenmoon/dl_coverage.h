// dl_coverage.h — candle coverage interval store + gap computation
// for the whenmoon downloader. Internal; WHENMOON_INTERNAL-gated.
//
// Coverage is the single source of truth for "do we have this candle
// data". Every successful page insert in dl_candles ends with a call
// to wm_coverage_add(); every pre-dispatch job window passes through
// wm_coverage_gaps_candles to prune sub-ranges already covered.

#ifndef BM_WHENMOON_DL_COVERAGE_H
#define BM_WHENMOON_DL_COVERAGE_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WM_COV_TS_SZ      40    // "2026-04-22 00:00:00+00" + slack

// Coverage-merge gap thresholds. Adjacent paginations are unioned in
// place; non-adjacent intervals stay separate rows. The timestamp
// predicate is widened to 2*gran with a 30-day sanity check so no
// touching row's range can pull iv past a multi-year jump.
#define WM_COV_TS_SANITY_DAYS        30     // candle out-of-band ceiling

// One contiguous coverage interval. `first_ts` / `last_ts` are
// authoritative; `granularity` carries the candles bucket size in
// seconds (must be > 0). All timestamps are Postgres canonical
// TIMESTAMPTZ strings in UTC form ("YYYY-MM-DD HH:MM:SS+00" or
// "YYYY-MM-DD HH:MM:SS.ffffff+00").
typedef struct
{
  int32_t  market_id;
  char     first_ts[WM_COV_TS_SZ];
  char     last_ts[WM_COV_TS_SZ];
  int32_t  granularity;
} wm_coverage_t;

// Merge an interval into the coverage store. Returns SUCCESS on a
// committed write, FAIL on SQL error. Overlapping and touching rows
// are unioned under one transaction (see wm_cov_merge_tx for the
// exact sequence) and serialised against concurrent writers for the
// same (market, granularity) via pg_advisory_xact_lock.
bool wm_coverage_add(const wm_coverage_t *iv);

// Complement of coverage rows in [range_start, range_end). Sorted
// ascending. `out` is caller-allocated, capacity `max_out`; returns
// the count actually written. If the complement would exceed max_out,
// the last slot spans the tail (fine for admin display; callers that
// need exact lists must grow max_out).
uint32_t wm_coverage_gaps_candles(int32_t market_id, int32_t gran_secs,
    const char *range_start, const char *range_end,
    wm_coverage_t *out, uint32_t max_out);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_COVERAGE_H
