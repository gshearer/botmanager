// pnl.h — per-trade-book PnL accumulators (WM-LT-4).
//
// Accumulators are folded incrementally on every closing fill (realized
// PnL, fees, win/loss tally) and on every equity sample (drawdown). A
// rolling returns ring backs Sharpe/Sortino at compute time.
//
// Returns are per-trade returns (realized PnL of a closing fill divided
// by the *cost basis* of the closed quantity), not periodic returns.
// This is the only formulation that makes sense for an event-driven
// trade engine where the strategy may go flat for hours and then trade
// once. Sharpe is computed as mean / stddev * sqrt(N) where N is the
// trade count — useful as a relative metric across param iterations,
// not directly comparable to periodic-return Sharpe published in the
// literature.
//
// Calmar is intentionally NOT computed here: it requires an annualized
// return, which in turn requires knowing the elapsed wall-clock window
// of the run. WM-LT-5/6 will sit on top and compute it from the
// snapshot's start/end ts pair; for the live trade book we stay out of
// the wall-clock business.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_PNL_H
#define BM_WHENMOON_PNL_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stdint.h>

// Default returns-ring capacity. Each entry is one double; 4 KiB at
// cap=512. A long-running paper trader that overruns the ring loses
// the oldest sample but keeps cumulative tallies (n_trades, gross_*,
// fees, drawdown) intact.
#define WM_PNL_RETURNS_CAP_DEFAULT  512

typedef struct wm_pnl_acc
{
  // Trade tallies (lifetime).
  uint32_t  n_trades;          // closing fills only
  uint32_t  n_wins;
  uint32_t  n_losses;

  // Quote-currency accumulators.
  double    realized_pnl;
  double    gross_profit;      // sum of positive realized fills
  double    gross_loss;        // sum of |negative realized fills|
  double    fees_paid;

  // Drawdown tracker on equity (cash + position*mark).
  double    equity_peak;
  double    max_drawdown;      // peak - trough, >= 0
  bool      have_peak;

  // Returns ring (per-trade closing returns).
  double   *returns;
  uint32_t  returns_cap;
  uint32_t  returns_n;         // populated entries
  uint32_t  returns_head;      // next write slot
} wm_pnl_acc_t;

typedef struct wm_pnl_metrics
{
  uint32_t  n_trades;
  uint32_t  n_wins;
  uint32_t  n_losses;

  double    win_rate;          // n_wins / max(1, n_trades)
  double    realized_pnl;
  double    fees_paid;
  double    profit_factor;     // gross_profit / max(eps, gross_loss)
  double    max_drawdown;
  double    avg_win;           // gross_profit / max(1, n_wins)
  double    avg_loss;          // gross_loss   / max(1, n_losses)
  double    sharpe;            // mean(returns) / stddev(returns) * sqrt(N)
  double    sortino;           // mean(returns) / downside_dev(returns) * sqrt(N)
} wm_pnl_metrics_t;

// Lifecycle. Returns NULL on OOM. cap=0 selects the default capacity.
wm_pnl_acc_t *wm_pnl_acc_create(uint32_t returns_cap);
void          wm_pnl_acc_free(wm_pnl_acc_t *acc);

// Reset all counters / accumulators / ring; keeps the ring allocation.
void wm_pnl_acc_reset(wm_pnl_acc_t *acc);

// Record one closing fill. closed_cost_basis is the quote-currency
// notional of the closed portion (= avg_entry_px * closed_qty); used
// to derive the per-trade return for the Sharpe/Sortino ring. Pass 0
// or negative cost_basis to skip the ring (still folds into the
// realized_pnl / win-loss tallies).
void wm_pnl_acc_record_close(wm_pnl_acc_t *acc, double realized_pnl,
    double closed_cost_basis);

// Record fee paid on any fill (opening or closing). Folded into
// fees_paid; not subtracted from realized_pnl (caller is expected to
// already have done so before passing realized_pnl into record_close).
void wm_pnl_acc_record_fee(wm_pnl_acc_t *acc, double fee);

// Record a fresh equity sample. Updates peak + drawdown. Caller passes
// equity = cash + position * mark.
void wm_pnl_acc_record_equity(wm_pnl_acc_t *acc, double equity);

// Compute derived metrics from the live state. Pure function; safe to
// call without the accumulator being quiescent (caller holds the
// owning book's lock so the accumulator is not racing).
void wm_pnl_compute(const wm_pnl_acc_t *acc, wm_pnl_metrics_t *out);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_PNL_H
