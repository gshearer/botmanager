// botmanager — MIT
// indicators_custom.c — indicator math TA-Lib does not ship.
//
// Pure functions over trailing-window column arrays, called from
// wm_indicators_compute_bar() in indicators.c. See indicators_custom.h
// for the "how to add a custom indicator" contract. The Fisher
// Transform here is a clean-room implementation of John Ehlers'
// public algorithm; no third-party indicator source is copied in.

#define WHENMOON_INTERNAL
#include "indicators_custom.h"

#include <math.h>
#include <stddef.h>

float
wm_calc_vwap(const double *highs, const double *lows,
    const double *closes, const double *vols, int n, int period)
{
  double  pv = 0.0;
  double  v  = 0.0;
  int     i;

  if(n < period || period <= 0)
    return((float)NAN);

  for(i = n - period; i < n; i++)
  {
    double  tp = (highs[i] + lows[i] + closes[i]) / 3.0;

    pv += tp * vols[i];
    v  += vols[i];
  }

  if(v <= 0.0)
    return((float)NAN);

  return((float)(pv / v));
}

float
wm_calc_vpt(const double *closes, const double *vols, int n)
{
  double  vpt = 0.0;
  int     i;

  if(n < 2)
    return((float)NAN);

  for(i = 1; i < n; i++)
  {
    if(closes[i - 1] > 0.0)
      vpt += vols[i] * (closes[i] - closes[i - 1]) / closes[i - 1];
  }

  return((float)vpt);
}

float
wm_calc_fisher(const double *highs, const double *lows, int n, int period)
{
  double  value  = 0.0;     // smoothed median position, clamped to (-1, 1)
  double  fisher = 0.0;     // running fisher line
  int     i;

  if(highs == NULL || lows == NULL || period < 1 || n < period)
    return((float)NAN);

  // Recompute the recursion across the whole window; the newest bar's
  // fisher is the slot value. The inner scan keeps a rolling high/low
  // of the median over the trailing `period` bars — O(n * period),
  // trivial at the window sizes (<= 256) indicators.c supplies.
  for(i = period - 1; i < n; i++)
  {
    double  median = (highs[i] + lows[i]) * 0.5;
    double  hi     = median;
    double  lo     = median;
    double  range;
    double  pos;
    int     j;

    for(j = i - period + 1; j <= i; j++)
    {
      double  m = (highs[j] + lows[j]) * 0.5;

      if(m > hi)
        hi = m;

      if(m < lo)
        lo = m;
    }

    range = hi - lo;

    // Degenerate flat window: floor the range so the transform does not
    // divide by zero (Ehlers' published guard value).
    if(range <= 0.0)
      range = 0.001;

    // Position of the current median in [lo, hi] mapped to [-1, +1],
    // then 0.33 / 0.67-smoothed.
    pos   = (median - lo) / range;
    value = 0.33 * 2.0 * (pos - 0.5) + 0.67 * value;

    // Ehlers' snap-to-rail clamp: keeps the log finite and saturates
    // readings already past the channel edge.
    if(value > 0.99)
      value = 0.999;

    if(value < -0.99)
      value = -0.999;

    fisher = 0.5 * log((1.0 + value) / (1.0 - value)) + 0.5 * fisher;
  }

  return((float)fisher);
}
