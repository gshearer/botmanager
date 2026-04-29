// sizer.h — signal -> trade-intent translator (WM-LT-4 v1: fixed-fraction).
//
// The sizer is a pure function: it consumes a strategy signal + the
// trade book's cached params + current cash/position/mark, and emits a
// trade intent (buy / sell / hold) with a target qty.
//
// V1 model: target_position = clamp(score * size_frac * cash / mark,
//                                   -max_position, +max_position).
//   delta = target_position - current_position.
// A non-zero delta becomes a single fill in the corresponding direction.
//
// Future versions can split into entry / scale / exit sizers, add
// confidence-weighted scaling, integrate volatility-target sizing, etc.
// The interface is intentionally narrow so those substitutions remain
// drop-in.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

#ifndef BM_WHENMOON_SIZER_H
#define BM_WHENMOON_SIZER_H

#ifdef WHENMOON_INTERNAL

#include "order.h"
#include "whenmoon_strategy.h"

#include <stdbool.h>
#include <stdint.h>

// Signed minimum fraction-of-target delta we treat as actionable. Below
// this we report HOLD, so a tiny rounding wobble in (target - current)
// does not churn fills. Tuned for the v1 fixed-fraction sizer; revisit
// if a future sizer produces inherently noisy targets.
#define WM_SIZER_MIN_DELTA_FRAC   0.05   // 5% of |target_position|

typedef enum
{
  WM_SIZER_HOLD  = 0,    // no action
  WM_SIZER_BUY   = 1,    // open / extend long, or close short
  WM_SIZER_SELL  = 2,    // open / extend short, or close long
} wm_sizer_action_t;

typedef struct wm_sizer_intent
{
  wm_sizer_action_t action;
  double            qty;                              // base units, > 0 for non-HOLD
  double            target_position;                  // signed; for /show
  double            target_cash_frac;                 // for /show
  char              reason[WM_STRATEGY_REASON_SZ];
} wm_sizer_intent_t;

// Compute the intent. Caller MUST hold book->lock. mark_px must be
// strictly positive — a zero or negative mark short-circuits to HOLD
// with reason "no mark". sig is read but not retained.
//
// Reads the book's cached params (size_frac, max_position) and current
// cash + position. Does not mutate the book.
void wm_sizer_compute(const wm_trade_book_t *book, double mark_px,
    const wm_strategy_signal_t *sig, wm_sizer_intent_t *out);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_SIZER_H
