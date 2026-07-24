// dl_candles.h — candle-history page loop for the whenmoon downloader.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_DL_CANDLES_H
#define BM_WHENMOON_DL_CANDLES_H

#ifdef WHENMOON_INTERNAL

#include "exchange_api.h"
#include "dl_jobtable.h"

#include <stdbool.h>
#include <stdint.h>

#define WM_DL_CANDLE_WINDOW_BUCKETS   300

// WM-DL-EMPTY-RUN: consecutive empty (zero-row) pages the inception walk
// tolerates before concluding it has reached the start of the exchange's
// candle record and marking the job DONE. A single empty page is commonly
// a legitimate no-trade / outage window (a ~12h Coinbase gap on 2026-05-08
// returned an empty page for every market at once); ending the walk on the
// first one truncates the download to whatever sits in front of the gap.
// At WM_DL_CANDLE_WINDOW_BUCKETS=300 (5h per 1m page) this rides out a
// ~60h contiguous hole while costing only this many wasted requests once
// the walk genuinely runs off the end of history.
#define WM_DL_EMPTY_RUN_MAX           12

// WM-S6: soft cap on rows wm_dl_candles_query_aggregated will fill.
// 10k rows * 48 bytes/row = 480 KiB — comfortably sized for a
// heap-allocated scratch buffer on the cmd worker.
#define WM_DL_CANDLES_OUT_CAP      10000

typedef struct
{
  dl_jobtable_t  *table;
  int64_t         job_id;
  int64_t         window_start_s;   // inclusive
  int64_t         window_end_s;     // exclusive
} wm_dl_candles_ctx_t;

// Dispatches one candles request on behalf of `j`. Caller has already
// set j->in_flight=true under the jobtable lock. Returns SUCCESS if the
// request was submitted (the completion callback will eventually clear
// in_flight); FAIL on submit error, in which case the tick caller must
// clear in_flight.
bool     wm_dl_candles_dispatch_one(dl_jobtable_t *t, dl_job_t *j);

// curl-multi worker completion; re-resolves the job under the lock,
// inserts rows off-lock, extends coverage, advances the cursor, decides
// terminal state. Takes ownership of `user` (mem_free on exit).
void     wm_dl_candles_on_page(const exchange_candles_result_t *res,
    void *user);

// Batched INSERT of a 0..300-row page. Returns rows_affected (never the
// page count — ON CONFLICT collapses duplicates).
uint32_t wm_dl_candles_insert_page(int32_t market_id,
    const exchange_candles_result_t *res);

// WM-S6: read the stored 1m-candle table through the
// wm_candle_upsample plpgsql function and fill `out`. Returns the
// number of rows written (0 on bad args, unknown market, empty
// window, or DB error). Truncates to `cap` with a CLAM_WARN.
// `start_ts` / `end_ts` are TIMESTAMPTZ canonical strings
// ("YYYY-MM-DD HH:MM:SS+00"). Ensures the underlying 1m table exists
// before calling the function, so a never-downloaded market replies
// empty rather than failing with "relation does not exist".
uint32_t wm_dl_candles_query_aggregated(int32_t market_id,
    int32_t gran_secs, const char *start_ts, const char *end_ts,
    exchange_candle_t *out, uint32_t cap);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_CANDLES_H
