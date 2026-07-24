// dl_coverage.h — candle coverage interval store + gap computation
// for the whenmoon downloader. Internal; WHENMOON_INTERNAL-gated.
//
// The coverage interval store records which windows have been
// attempted: every successful page insert in dl_candles ends with a
// call to wm_coverage_add(). Authoritative "do we actually have this
// minute" questions are answered against the candle rows themselves
// via wm_gap_find_row_gaps / wm_gap_largest_missing, not this store.

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
// authoritative. All timestamps are Postgres canonical TIMESTAMPTZ
// strings in UTC form ("YYYY-MM-DD HH:MM:SS+00" or
// "YYYY-MM-DD HH:MM:SS.ffffff+00").
typedef struct
{
  int32_t  market_id;
  char     first_ts[WM_COV_TS_SZ];
  char     last_ts[WM_COV_TS_SZ];
} wm_coverage_t;

// Format epoch-milliseconds as a Postgres canonical UTC TIMESTAMPTZ
// string ("YYYY-MM-DD HH:MM:SS+00") into `out` (>= WM_COV_TS_SZ). Used
// to build the [now-lookback, now] range passed to wm_gap_find_row_gaps
// and the oldest/newest_ts args of wm_dl_job_enqueue.
void wm_pg_ts_from_ms(int64_t ms, char *out, size_t cap);

// Newest candle timestamp (epoch ms) in wm_candles_<market_id>, or 0 if
// the table is empty/absent. Warmup convergence judges the DB tail
// current when (now - this) < tolerance — old internal holes are
// tolerated; only recency matters for a warm replay.
int64_t wm_candle_newest_ms(int32_t market_id);

// Merge an interval into the coverage store. Returns SUCCESS on a
// committed write, FAIL on SQL error. Overlapping and touching rows
// are unioned under one transaction (see wm_cov_merge_tx for the
// exact sequence) and serialised against concurrent writers for the
// same market via pg_advisory_xact_lock.
bool wm_coverage_add(const wm_coverage_t *iv);

// Row-level gap walker over `wm_candles_<market_id>`. Returns the
// windows where minute-bars are actually missing from the table in
// `[range_start, range_end]`: a backward gap if the table's MIN(ts)
// > range_start, a forward gap if MAX(ts) < range_end, and one entry
// per internal LAG-detected gap. Sorted ascending. This is the
// authoritative "which rows are missing right now" check — it reads
// the candle rows, not the coverage-attempt store.
//
// `out` is caller-allocated, capacity `max_out`; returns count
// written. Truncates silently at max_out (caller should bump cap and
// re-run if it cares about completeness).
uint32_t wm_gap_find_row_gaps(int32_t market_id,
    const char *range_start, const char *range_end,
    wm_coverage_t *out, uint32_t max_out);

// Largest single contiguous missing 1m window over the candle rows in
// `[range_start, range_end]` (same gap classes as wm_gap_find_row_gaps,
// widest one only). Writes it to *out and returns 1; returns 0 when
// the range is fully covered at 1m cadence. Truncation-proof — use
// this when you only need to judge whether a gap is material, so a
// thin market's many small holes can't crowd the biggest one out of a
// fixed array.
uint32_t wm_gap_largest_missing(int32_t market_id,
    const char *range_start, const char *range_end,
    wm_coverage_t *out);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_COVERAGE_H
