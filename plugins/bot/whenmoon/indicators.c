// botmanager — MIT
// indicators.c — TA-Lib-backed indicator computation for closed bars.
//
// One pass per closed bar, called under whenmoon_market_t.lock by the
// aggregator (live and replay paths share this entry point). Reads a
// trailing window of OHLCV from the ring, calls one TA_* per slot,
// writes the latest valid output back into ring[bar_idx].ind[slot].
// Insufficient-history slots stay NaN; strategies (WM-LT-3+) check
// isnan() before consuming.
//
// TA-Lib quirk: TA_* writes its output starting at index 0 even when
// the first valid value is at the input's lookback-th index. Use
// outNBElement to detect "no output", and out[outNBElement - 1] to
// pick the latest. The library is thread-safe on call (independent
// state per call); the per-market lock protects the ring, not TA-Lib.

#define WHENMOON_INTERNAL
#include "indicators.h"

#include <ta-lib/ta_libc.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#define WM_TA_BUF 256

// Helper: scalar indicator returning one output array; pick the last
// valid value or NaN.
static float
wm_ta_pick_last(TA_RetCode rc, int nb, const double *out)
{
  if(rc != TA_SUCCESS || nb <= 0)
    return((float)NAN);

  return((float)out[nb - 1]);
}

// ------------------------------------------------------------------ //
// VWAP / VPT — TA-Lib does not ship these; compute inline.            //
// ------------------------------------------------------------------ //

// VWAP rolling window: sum(typical_price * volume) / sum(volume) over
// the last `period` bars. Returns NaN when the window has zero total
// volume (entirely empty bars).
static float
wm_calc_vwap(const double *highs, const double *lows,
    const double *closes, const double *vols, int n, int period)
{
  double pv = 0.0;
  double v  = 0.0;
  int    i;

  if(n < period || period <= 0)
    return((float)NAN);

  for(i = n - period; i < n; i++)
  {
    double tp = (highs[i] + lows[i] + closes[i]) / 3.0;
    pv += tp * vols[i];
    v  += vols[i];
  }

  if(v <= 0.0)
    return((float)NAN);

  return((float)(pv / v));
}

// VPT cumulative: VPT_t = VPT_{t-1} + V_t * (C_t - C_{t-1}) / C_{t-1}.
// Computed from the start of the window for stability — strategies
// reading the value should care about delta, not the absolute level.
static float
wm_calc_vpt(const double *closes, const double *vols, int n)
{
  double vpt = 0.0;
  int    i;

  if(n < 2)
    return((float)NAN);

  for(i = 1; i < n; i++)
  {
    if(closes[i - 1] > 0.0)
      vpt += vols[i] * (closes[i] - closes[i - 1]) / closes[i - 1];
  }

  return((float)vpt);
}

// ------------------------------------------------------------------ //
// Compute pass                                                        //
// ------------------------------------------------------------------ //

void
wm_indicators_compute_bar(const wm_candle_full_t *ring,
    uint32_t n, uint32_t bar_idx)
{
  double            opens[WM_TA_BUF];
  double            highs[WM_TA_BUF];
  double            lows[WM_TA_BUF];
  double            closes[WM_TA_BUF];
  double            vols[WM_TA_BUF];
  double            out[WM_TA_BUF];
  double            out2[WM_TA_BUF];
  double            out3[WM_TA_BUF];
  int               beg;
  int               nb;
  TA_RetCode        rc;
  uint32_t          take;
  uint32_t          off;
  uint32_t          i;
  wm_candle_full_t *bar;
  int               s;

  if(ring == NULL || bar_idx >= n)
    return;

  // Cast away const — we own the bar at bar_idx for the indicator
  // write-back. The aggregator gives us the ring read-only by
  // contract, but the latest bar is intentionally mutable to land
  // indicators on it.
  bar = (wm_candle_full_t *)&ring[bar_idx];

  for(s = 0; s < 50; s++)
    bar->ind[s] = (float)NAN;

  take = (bar_idx + 1 > WM_TA_BUF) ? WM_TA_BUF : bar_idx + 1;
  off  = bar_idx + 1 - take;

  for(i = 0; i < take; i++)
  {
    opens[i]  = ring[off + i].open;
    highs[i]  = ring[off + i].high;
    lows[i]   = ring[off + i].low;
    closes[i] = ring[off + i].close;
    vols[i]   = ring[off + i].volume;
  }

  (void)opens;   // not needed by TA-Lib calls below, but kept for VWAP-style additions

  // SMA family ------------------------------------------------------
  rc = TA_SMA(0, (int)take - 1, closes, 20,
      &beg, &nb, out);
  bar->ind[WM_IND_SMA_20]  = wm_ta_pick_last(rc, nb, out);

  rc = TA_SMA(0, (int)take - 1, closes, 50,
      &beg, &nb, out);
  bar->ind[WM_IND_SMA_50]  = wm_ta_pick_last(rc, nb, out);

  rc = TA_SMA(0, (int)take - 1, closes, 200,
      &beg, &nb, out);
  bar->ind[WM_IND_SMA_200] = wm_ta_pick_last(rc, nb, out);

  // EMA family ------------------------------------------------------
  rc = TA_EMA(0, (int)take - 1, closes, 9,
      &beg, &nb, out);
  bar->ind[WM_IND_EMA_9]  = wm_ta_pick_last(rc, nb, out);

  rc = TA_EMA(0, (int)take - 1, closes, 12,
      &beg, &nb, out);
  bar->ind[WM_IND_EMA_12] = wm_ta_pick_last(rc, nb, out);

  rc = TA_EMA(0, (int)take - 1, closes, 20,
      &beg, &nb, out);
  bar->ind[WM_IND_EMA_20] = wm_ta_pick_last(rc, nb, out);

  rc = TA_EMA(0, (int)take - 1, closes, 26,
      &beg, &nb, out);
  bar->ind[WM_IND_EMA_26] = wm_ta_pick_last(rc, nb, out);

  rc = TA_EMA(0, (int)take - 1, closes, 50,
      &beg, &nb, out);
  bar->ind[WM_IND_EMA_50] = wm_ta_pick_last(rc, nb, out);

  // MACD (12, 26, 9) ------------------------------------------------
  rc = TA_MACD(0, (int)take - 1, closes, 12, 26, 9,
      &beg, &nb, out, out2, out3);
  bar->ind[WM_IND_MACD]        = wm_ta_pick_last(rc, nb, out);
  bar->ind[WM_IND_MACD_SIGNAL] = wm_ta_pick_last(rc, nb, out2);
  bar->ind[WM_IND_MACD_HIST]   = wm_ta_pick_last(rc, nb, out3);

  // RSI / Stochastic / CCI ------------------------------------------
  rc = TA_RSI(0, (int)take - 1, closes, 14,
      &beg, &nb, out);
  bar->ind[WM_IND_RSI_14] = wm_ta_pick_last(rc, nb, out);

  rc = TA_STOCH(0, (int)take - 1, highs, lows, closes,
      14, 3, TA_MAType_SMA, 3, TA_MAType_SMA,
      &beg, &nb, out, out2);
  bar->ind[WM_IND_STOCH_K] = wm_ta_pick_last(rc, nb, out);
  bar->ind[WM_IND_STOCH_D] = wm_ta_pick_last(rc, nb, out2);

  rc = TA_CCI(0, (int)take - 1, highs, lows, closes, 20,
      &beg, &nb, out);
  bar->ind[WM_IND_CCI_20] = wm_ta_pick_last(rc, nb, out);

  // Bollinger Bands (20, 2, 2, SMA) + %B -----------------------------
  rc = TA_BBANDS(0, (int)take - 1, closes,
      20, 2.0, 2.0, TA_MAType_SMA,
      &beg, &nb, out, out2, out3);
  bar->ind[WM_IND_BB_UPPER]  = wm_ta_pick_last(rc, nb, out);
  bar->ind[WM_IND_BB_MIDDLE] = wm_ta_pick_last(rc, nb, out2);
  bar->ind[WM_IND_BB_LOWER]  = wm_ta_pick_last(rc, nb, out3);

  if(!isnan(bar->ind[WM_IND_BB_UPPER]) &&
     !isnan(bar->ind[WM_IND_BB_LOWER]))
  {
    double up = bar->ind[WM_IND_BB_UPPER];
    double dn = bar->ind[WM_IND_BB_LOWER];
    double rng = up - dn;

    bar->ind[WM_IND_BB_PCTB] = rng > 0
        ? (float)((bar->close - dn) / rng)
        : (float)NAN;
  }

  // Volume-weighted -------------------------------------------------
  rc = TA_OBV(0, (int)take - 1, closes, vols,
      &beg, &nb, out);
  bar->ind[WM_IND_OBV] = wm_ta_pick_last(rc, nb, out);

  rc = TA_MFI(0, (int)take - 1, highs, lows, closes, vols, 14,
      &beg, &nb, out);
  bar->ind[WM_IND_MFI_14] = wm_ta_pick_last(rc, nb, out);

  bar->ind[WM_IND_VWAP] = wm_calc_vwap(highs, lows, closes, vols,
      (int)take, 20);
  bar->ind[WM_IND_VPT]  = wm_calc_vpt(closes, vols, (int)take);

  // Volatility / range ----------------------------------------------
  rc = TA_ATR(0, (int)take - 1, highs, lows, closes, 14,
      &beg, &nb, out);
  bar->ind[WM_IND_ATR_14] = wm_ta_pick_last(rc, nb, out);

  rc = TA_NATR(0, (int)take - 1, highs, lows, closes, 14,
      &beg, &nb, out);
  bar->ind[WM_IND_NATR_14] = wm_ta_pick_last(rc, nb, out);

  rc = TA_TRANGE(0, (int)take - 1, highs, lows, closes,
      &beg, &nb, out);
  bar->ind[WM_IND_TR] = wm_ta_pick_last(rc, nb, out);

  rc = TA_ADX(0, (int)take - 1, highs, lows, closes, 14,
      &beg, &nb, out);
  bar->ind[WM_IND_ADX_14] = wm_ta_pick_last(rc, nb, out);

  // Trend / momentum ------------------------------------------------
  rc = TA_ROC(0, (int)take - 1, closes, 10,
      &beg, &nb, out);
  bar->ind[WM_IND_ROC_10] = wm_ta_pick_last(rc, nb, out);

  rc = TA_MOM(0, (int)take - 1, closes, 10,
      &beg, &nb, out);
  bar->ind[WM_IND_MOM_10] = wm_ta_pick_last(rc, nb, out);

  rc = TA_WILLR(0, (int)take - 1, highs, lows, closes, 14,
      &beg, &nb, out);
  bar->ind[WM_IND_WILLR_14] = wm_ta_pick_last(rc, nb, out);

  rc = TA_SAR(0, (int)take - 1, highs, lows, 0.02, 0.20,
      &beg, &nb, out);
  bar->ind[WM_IND_PSAR] = wm_ta_pick_last(rc, nb, out);

  // Microstructure approximations -----------------------------------
  {
    double rng     = bar->high - bar->low;
    double cls     = bar->close;
    double opn     = bar->open;
    double max_co  = cls > opn ? cls : opn;
    double min_co  = cls < opn ? cls : opn;

    bar->ind[WM_IND_BAR_RANGE_PCT] =
        cls > 0 ? (float)(rng / cls) : (float)NAN;

    bar->ind[WM_IND_BODY_PCT] =
        rng > 0 ? (float)((cls - opn) / rng) : (float)NAN;

    bar->ind[WM_IND_UPPER_WICK_PCT] =
        rng > 0 ? (float)((bar->high - max_co) / rng) : (float)NAN;

    bar->ind[WM_IND_LOWER_WICK_PCT] =
        rng > 0 ? (float)((min_co - bar->low) / rng) : (float)NAN;
  }
}
