// botmanager — MIT
// Whenmoon PnL accumulators (WM-LT-4).
//
// All entry points assume the caller serialises access (typically via
// the owning trade book's lock). The ring + cumulative tallies are
// updated in lock-step on every closing fill; metrics derive on demand.

#define WHENMOON_INTERNAL
#include "pnl.h"

#include "alloc.h"

#include <math.h>
#include <string.h>

// Floor used to keep ratios finite when the denominator is zero. Picked
// to be small enough that "no losses, $1k profit" produces a
// recognisably huge profit factor (1e9-ish) rather than overflowing.
#define WM_PNL_EPS  1.0e-9

// ----------------------------------------------------------------------- //
// Lifecycle                                                               //
// ----------------------------------------------------------------------- //

wm_pnl_acc_t *
wm_pnl_acc_create(uint32_t returns_cap)
{
  wm_pnl_acc_t *acc;
  uint32_t      cap;

  cap = returns_cap > 0 ? returns_cap : WM_PNL_RETURNS_CAP_DEFAULT;

  acc = mem_alloc("whenmoon", "pnl_acc", sizeof(*acc));

  if(acc == NULL)
    return(NULL);

  memset(acc, 0, sizeof(*acc));

  acc->returns = mem_alloc("whenmoon", "pnl_ring", sizeof(double) * cap);

  if(acc->returns == NULL)
  {
    mem_free(acc);
    return(NULL);
  }

  acc->returns_cap = cap;
  return(acc);
}

void
wm_pnl_acc_free(wm_pnl_acc_t *acc)
{
  if(acc == NULL)
    return;

  if(acc->returns != NULL)
    mem_free(acc->returns);

  mem_free(acc);
}

void
wm_pnl_acc_reset(wm_pnl_acc_t *acc)
{
  uint32_t cap;
  double  *ring;

  if(acc == NULL)
    return;

  // Preserve the ring allocation across reset; the cap and pointer
  // survive the memset.
  cap  = acc->returns_cap;
  ring = acc->returns;

  memset(acc, 0, sizeof(*acc));

  acc->returns_cap = cap;
  acc->returns     = ring;
}

// ----------------------------------------------------------------------- //
// Record                                                                  //
// ----------------------------------------------------------------------- //

void
wm_pnl_acc_record_close(wm_pnl_acc_t *acc, double realized_pnl,
    double closed_cost_basis)
{
  double ret;

  if(acc == NULL)
    return;

  acc->n_trades++;
  acc->realized_pnl += realized_pnl;

  if(realized_pnl > 0.0)
  {
    acc->n_wins++;
    acc->gross_profit += realized_pnl;
  }
  else if(realized_pnl < 0.0)
  {
    acc->n_losses++;
    acc->gross_loss += -realized_pnl;
  }

  // Push the per-trade return onto the ring. Skip when we lack a
  // meaningful cost basis (e.g. closing a synthetic flatten with zero
  // notional — which should not happen, but is defensive).
  if(closed_cost_basis > WM_PNL_EPS && acc->returns != NULL &&
     acc->returns_cap > 0)
  {
    ret = realized_pnl / closed_cost_basis;

    acc->returns[acc->returns_head] = ret;
    acc->returns_head = (acc->returns_head + 1) % acc->returns_cap;

    if(acc->returns_n < acc->returns_cap)
      acc->returns_n++;
  }
}

void
wm_pnl_acc_record_fee(wm_pnl_acc_t *acc, double fee)
{
  if(acc == NULL || fee == 0.0)
    return;

  acc->fees_paid += fee;
}

void
wm_pnl_acc_record_equity(wm_pnl_acc_t *acc, double equity)
{
  double dd;

  if(acc == NULL)
    return;

  if(!acc->have_peak)
  {
    acc->equity_peak = equity;
    acc->have_peak   = true;
    return;
  }

  if(equity > acc->equity_peak)
  {
    acc->equity_peak = equity;
    return;
  }

  dd = acc->equity_peak - equity;

  if(dd > acc->max_drawdown)
    acc->max_drawdown = dd;
}

// ----------------------------------------------------------------------- //
// Compute                                                                 //
// ----------------------------------------------------------------------- //

// Iterate the populated portion of the returns ring in append order.
// The ring writes at returns_head and grows up to returns_cap; once
// full, returns_head is the oldest. fn(ret, user) per element.
typedef void (*wm_pnl_ring_iter_fn)(double ret, void *user);

static void
wm_pnl_ring_iterate(const wm_pnl_acc_t *acc, wm_pnl_ring_iter_fn fn,
    void *user)
{
  uint32_t i;
  uint32_t idx;
  uint32_t start;

  if(acc == NULL || acc->returns == NULL || acc->returns_n == 0)
    return;

  if(acc->returns_n < acc->returns_cap)
    start = 0;
  else
    start = acc->returns_head;

  for(i = 0; i < acc->returns_n; i++)
  {
    idx = (start + i) % acc->returns_cap;
    fn(acc->returns[idx], user);
  }
}

typedef struct
{
  double sum;
  double sum_neg_sq;     // sum of squared *negative* returns (Sortino)
  double sum_sq_dev;     // sum of (r - mean)^2 (Sharpe stddev)
  double mean;           // populated on second pass
} wm_pnl_stats_t;

static void
wm_pnl_sum_cb(double r, void *user)
{
  wm_pnl_stats_t *s = user;

  s->sum += r;

  if(r < 0.0)
    s->sum_neg_sq += r * r;
}

static void
wm_pnl_dev_cb(double r, void *user)
{
  wm_pnl_stats_t *s = user;
  double          d = r - s->mean;

  s->sum_sq_dev += d * d;
}

void
wm_pnl_compute(const wm_pnl_acc_t *acc, wm_pnl_metrics_t *out)
{
  wm_pnl_stats_t stats;
  double         denom_w;
  double         denom_l;
  double         stddev;
  double         downside_dev;
  double         n;

  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));

  if(acc == NULL)
    return;

  out->n_trades     = acc->n_trades;
  out->n_wins       = acc->n_wins;
  out->n_losses     = acc->n_losses;
  out->realized_pnl = acc->realized_pnl;
  out->fees_paid    = acc->fees_paid;
  out->max_drawdown = acc->max_drawdown;

  if(acc->n_trades > 0)
    out->win_rate = (double)acc->n_wins / (double)acc->n_trades;

  // Profit factor — bounded by epsilon so a no-loss run still produces
  // a meaningful number rather than a divide-by-zero.
  out->profit_factor =
      acc->gross_profit / (acc->gross_loss > WM_PNL_EPS
                              ? acc->gross_loss : WM_PNL_EPS);

  denom_w = acc->n_wins   > 0 ? (double)acc->n_wins   : 1.0;
  denom_l = acc->n_losses > 0 ? (double)acc->n_losses : 1.0;
  out->avg_win  = acc->gross_profit / denom_w;
  out->avg_loss = acc->gross_loss   / denom_l;

  if(acc->returns_n < 2)
    return;

  memset(&stats, 0, sizeof(stats));

  wm_pnl_ring_iterate(acc, wm_pnl_sum_cb, &stats);

  n          = (double)acc->returns_n;
  stats.mean = stats.sum / n;

  wm_pnl_ring_iterate(acc, wm_pnl_dev_cb, &stats);

  // Sample stddev (denominator n-1). Using n instead would inflate the
  // ratio for small samples; n-1 is the standard convention for the
  // financial Sharpe.
  stddev = sqrt(stats.sum_sq_dev / (n - 1.0));

  if(stddev > WM_PNL_EPS)
    out->sharpe = stats.mean / stddev * sqrt(n);

  // Sortino: substitutes downside semi-deviation for stddev. Uses the
  // raw count n in the denominator (not the negative count) — the
  // standard formulation treats the downside variance as an estimate
  // over the full sample.
  downside_dev = sqrt(stats.sum_neg_sq / n);

  if(downside_dev > WM_PNL_EPS)
    out->sortino = stats.mean / downside_dev * sqrt(n);
}
