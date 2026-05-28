// indicators_custom.h — custom (non-TA-Lib) indicator math.
//
// TA-Lib (BSD-3) is whenmoon's primary indicator engine; its pass over
// each closed bar lives in indicators.c. This file is the home for
// indicators TA-Lib does not ship — the volume-weighted pair (VWAP,
// VPT) and the Ehlers Fisher Transform. Each function takes trailing-
// window column arrays (oldest at index 0, newest at n-1) and returns
// the latest bar's value as a float, or NaN when there is too little
// history. That mirrors the wm_ta_pick_last() contract in indicators.c
// so a strategy consuming bar->ind[] cannot tell a custom slot from a
// TA-Lib slot: both are float, both are NaN until warm, both are read
// behind the same isnan() guard.
//
// Adding a custom indicator:
//   1. Implement wm_calc_<name>() here + declare it below. Keep it a
//      pure function of its input arrays (no I/O, no logging) — it runs
//      inside the per-bar compute pass under whenmoon_market_t.lock.
//   2. Append a WM_IND_<NAME> slot in whenmoon_strategy.h at the
//      reserved tail (so existing slot ids never shift) and bump
//      WM_INDICATOR_SCHEMA_VERSION.
//   3. Call it from wm_indicators_compute_bar() in indicators.c,
//      storing the result into bar->ind[WM_IND_<NAME>].
//   4. Recompile any .wm backtest snapshots — the schema-version bump
//      invalidates stale files by design (see wm_bt_file.c).
//
// Internal to the whenmoon plugin. Consumers outside this plugin must
// NOT include this header. Gate: WHENMOON_INTERNAL.

#ifndef BM_WHENMOON_INDICATORS_CUSTOM_H
#define BM_WHENMOON_INDICATORS_CUSTOM_H

#ifdef WHENMOON_INTERNAL

// Ehlers Fisher Transform default period. The standard setting (also
// TradingView's default); the over-extension thresholds strategies key
// off (around +-3) are calibrated against it.
#define WM_FISHER_PERIOD  9

// Rolling VWAP: sum(typical_price * volume) / sum(volume) over the last
// `period` bars. NaN when n < period, period <= 0, or the window has
// zero total volume.
float wm_calc_vwap(const double *highs, const double *lows,
    const double *closes, const double *vols, int n, int period);

// Cumulative VPT over the whole window:
// VPT_t = VPT_{t-1} + V_t * (C_t - C_{t-1}) / C_{t-1}. NaN when n < 2.
// Reported as an absolute level — consumers care about its delta, not
// the (window-dependent) absolute value.
float wm_calc_vpt(const double *closes, const double *vols, int n);

// Ehlers Fisher Transform over the trailing window. Reads the median
// price (high+low)/2, normalizes it to its rolling `period`-bar
// high/low range, smooths and clamps, then applies the Fisher
// transform. Returns the newest bar's value, or NaN when n < period or
// period < 1. The recursion is seeded at zero and recomputed over the
// whole window each call (the same windowed-recompute model TA-Lib uses
// in indicators.c); its 0.67 / 0.5 decays converge well inside the
// window so the newest value matches a running computation once warm.
float wm_calc_fisher(const double *highs, const double *lows,
    int n, int period);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_INDICATORS_CUSTOM_H
