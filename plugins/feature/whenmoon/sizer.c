// botmanager — MIT
// Whenmoon v1 fixed-fraction sizer (WM-LT-4).
//
// Pure function. Holds no state of its own. Inputs:
//   - book: read-only view of the trade book (cached params, current
//           cash + position). Caller holds book->lock.
//   - mark_px: the price the sizer should trade against (bar->close
//              on a bar-close path, trade->price on a trade-tick path).
//   - sig: the strategy's emission. score in [-1..+1] picks direction
//          and magnitude; confidence in [0..1] scales the target.
//
// Output: a wm_sizer_intent_t — buy / sell / hold + qty.
//
// Convention: in v1 we always trade to a target *signed* position. The
// fill engine in order.c then synthesises one fill of qty = |delta|.
// Reduces churn vs. naive "score change -> buy/sell whole-position"
// designs and lets us extend confidence into a continuous size knob
// without rearchitecting.

#define WHENMOON_INTERNAL
#include "sizer.h"

#include "order.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------- //
// Helpers                                                                 //
// ----------------------------------------------------------------------- //

static double
wm_sizer_clamp(double v, double lo, double hi)
{
  if(v < lo)  return(lo);
  if(v > hi)  return(hi);

  return(v);
}

static double
wm_sizer_signed_position(const wm_position_t *p)
{
  switch(p->side)
  {
    case WM_POS_LONG:   return( p->qty);
    case WM_POS_SHORT:  return(-p->qty);
    case WM_POS_FLAT:
    default:            return( 0.0);
  }
}

// ----------------------------------------------------------------------- //
// Compute                                                                 //
// ----------------------------------------------------------------------- //

void
wm_sizer_compute(const wm_trade_book_t *book, double mark_px,
    const wm_strategy_signal_t *sig, wm_sizer_intent_t *out)
{
  double  score;
  double  conf;
  double  cash_target;
  double  target_position;
  double  current_position;
  double  delta;
  double  abs_delta;
  double  threshold;

  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->action = WM_SIZER_HOLD;

  if(book == NULL || sig == NULL)
  {
    snprintf(out->reason, sizeof(out->reason), "no input");
    return;
  }

  if(!isfinite(mark_px) || mark_px <= 0.0)
  {
    snprintf(out->reason, sizeof(out->reason), "no mark");
    return;
  }

  // Score may be NaN if a degenerate strategy emits one; treat as flat.
  score = sig->score;
  conf  = sig->confidence;

  if(!isfinite(score))  score = 0.0;
  if(!isfinite(conf))   conf  = 0.0;

  score = wm_sizer_clamp(score, -1.0, 1.0);
  conf  = wm_sizer_clamp(conf,   0.0, 1.0);

  // Cash dedicated to *this* signal. size_frac is "what fraction of
  // total equity may a single max-strength signal claim". confidence
  // scales it down further. score sets the side (and, multiplicatively,
  // the depth of allocation between a half-conviction and full-
  // conviction signal).
  cash_target = book->cash * book->size_frac * conf * score;

  out->target_cash_frac = book->size_frac * conf * fabs(score);

  if(book->cash <= 0.0 && cash_target > 0.0)
  {
    snprintf(out->reason, sizeof(out->reason), "no cash");
    return;
  }

  // Translate to base units against the mark.
  target_position = cash_target / mark_px;

  // Per-attachment cap on absolute position size. max_position == 0
  // disables the cap (typical for paper testing).
  if(book->max_position > 0.0)
    target_position = wm_sizer_clamp(target_position,
        -book->max_position, +book->max_position);

  current_position = wm_sizer_signed_position(&book->position);
  delta            = target_position - current_position;
  abs_delta        = fabs(delta);

  out->target_position = target_position;

  if(abs_delta == 0.0)
  {
    snprintf(out->reason, sizeof(out->reason), "at target");
    return;
  }

  // Hysteresis: a delta smaller than 5% of the target position size
  // (or 5% of the threshold-floor when target is zero) is treated as
  // hold. This stops a noisy score wobble from churning fills.
  threshold = WM_SIZER_MIN_DELTA_FRAC *
      (fabs(target_position) > 0.0 ? fabs(target_position)
                                   : fabs(current_position));

  if(threshold > 0.0 && abs_delta < threshold)
  {
    snprintf(out->reason, sizeof(out->reason),
        "delta<%.2f%% of target", WM_SIZER_MIN_DELTA_FRAC * 100.0);
    return;
  }

  out->qty    = abs_delta;
  out->action = delta > 0.0 ? WM_SIZER_BUY : WM_SIZER_SELL;

  snprintf(out->reason, sizeof(out->reason),
      "%s %.6g (target=%.6g cur=%.6g)",
      out->action == WM_SIZER_BUY ? "buy" : "sell",
      out->qty, target_position, current_position);
}
