#ifndef BM_WHENMOON_WM_BT_METRICS_H
#define BM_WHENMOON_WM_BT_METRICS_H

#ifdef WHENMOON_INTERNAL

#include "market.h"          // wm_market_fill_t

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Derived backtest-report analytics computed from a single config's
// lossless fill record. The fill record is complete for a backtest
// (n_fills == lifetime_fills_count), so every series reconstructed here
// is exact — no engine change required. WM-BT-RPT-2 onward.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

// One equity-curve sample: wall-clock timestamp (ms since epoch), the
// account equity at that point (cash + position marked at the fill price),
// and the running peak-to-trough drawdown as a positive percent (0 at a
// new high-water mark).
typedef struct
{
  int64_t  t_ms;
  double   equity;
  double   dd_pct;
} wm_bt_equity_point_t;

// Reconstruct the per-fill equity curve + running drawdown for one config
// from its fills (oldest-to-newest — the order backtest.c drains them).
// Equity at each fill is `cash_after + position_after * price`; the peak
// tracks the running high-water mark and `dd_pct = (peak-eq)/peak*100`
// (0 when peak <= 0). The series is seeded with `(start_ms, start_cash, 0)`
// so the curve starts flat at the opening balance before the first trade.
//
// Emits at most n+1 points: the seed plus one per fill whose equity is
// finite — non-finite equities are skipped so the downstream
// Lightweight-Charts series never sees a NaN (a single NaN voids a whole
// LWC series). `*out` is a heap buffer the caller frees with mem_free;
// `*out_n` is the populated count. SUCCESS even when n == 0 (seed only).
// FAIL on bad args / null-fills-with-n / alloc failure, with `err`
// populated when non-NULL; `*out`/`*out_n` are zeroed on entry.
bool wm_bt_equity_series_build(const wm_market_fill_t *fills, uint32_t n,
    double start_cash, int64_t start_ms,
    wm_bt_equity_point_t **out, uint32_t *out_n,
    char *err, size_t err_cap);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WM_BT_METRICS_H
