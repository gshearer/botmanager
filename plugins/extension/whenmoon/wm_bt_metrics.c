// botmanager — MIT
// Whenmoon backtest-report analytics: equity curve + running drawdown
// reconstructed from a config's lossless fills (WM-BT-RPT-2).

#define WHENMOON_INTERNAL
#include "wm_bt_metrics.h"

#include "alloc.h"           // mem_alloc / mem_free
#include "common.h"          // SUCCESS / FAIL

#include <math.h>
#include <stdio.h>

#define WM_BT_METRICS_CTX   "whenmoon.bt.metrics"

bool
wm_bt_equity_series_build(const wm_market_fill_t *fills, uint32_t n,
    double start_cash, int64_t start_ms,
    wm_bt_equity_point_t **out, uint32_t *out_n,
    char *err, size_t err_cap)
{
  wm_bt_equity_point_t *pts;
  double                peak;
  uint32_t              w = 0;
  uint32_t              i;

  if(out == NULL || out_n == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "equity_series: bad args");

    return(FAIL);
  }

  *out   = NULL;
  *out_n = 0;

  if(fills == NULL && n > 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "equity_series: null fills (n=%u)", n);

    return(FAIL);
  }

  // Seed point + one slot per fill. n is bounded by the fill ring cap, so
  // the +1 never overflows uint32_t in practice.
  pts = mem_alloc(WM_BT_METRICS_CTX, "equity_points",
      sizeof(*pts) * ((size_t)n + 1u));

  if(pts == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "equity_points alloc failed (n=%u)", n);

    return(FAIL);
  }

  if(!isfinite(start_cash))
    start_cash = 0.0;

  // Seed: opening balance, flat, no drawdown.
  pts[w].t_ms   = start_ms;
  pts[w].equity = start_cash;
  pts[w].dd_pct = 0.0;
  peak          = start_cash;
  w++;

  for(i = 0; i < n; i++)
  {
    double eq = fills[i].cash_after
        + fills[i].position_after * fills[i].price;

    // Drop any pathological non-finite equity — one NaN voids the whole
    // Lightweight-Charts series downstream.
    if(!isfinite(eq))
      continue;

    if(eq > peak)
      peak = eq;

    pts[w].t_ms   = fills[i].ts_ms;
    pts[w].equity = eq;
    pts[w].dd_pct = (peak > 0.0) ? (peak - eq) / peak * 100.0 : 0.0;
    w++;
  }

  *out   = pts;
  *out_n = w;

  return(SUCCESS);
}
