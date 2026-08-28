#ifndef BM_WM_NUMERAIRE_H
#define BM_WM_NUMERAIRE_H

#include <stdbool.h>

// The office's unit of account (WM-NUMERAIRE-1).
//
// whenmoon's books stay in each market's quote currency — that is the
// dollar mirror `CFO.md` §2 requires — but every risk *control* is
// denominated in satoshis, because a cap written in dollars measures
// the wrong thing under a bitcoin mandate. Two ways round, both real:
// a fixed dollar exposure cap shrinks in satoshis as bitcoin rises, so
// staying compliant eventually means SELLING bitcoin; and a dollar
// drawdown breaker trips on a bitcoin dip, which under this mandate is
// a flat result the operator has already accepted (§2 names steering by
// it as the thing the office must not do).
//
// The correction is one line of arithmetic. A book's satoshi stack is
// its bitcoin leg plus its other leg marked into bitcoin, and that
// figure is PRICE-INVARIANT while the book sits in the neutral posture:
// fully in bitcoin, the mark cancels out and the stack does not move.
// It moves only when the book has deviated — which is exactly what §7
// says this office is judged on.
//
// Everything here is pure arithmetic over a product symbol and a mark,
// so this header includes no whenmoon header — the leaf property
// `warm_chain.h` and `ws_binding.h` rest on, and the reason
// `tests/test_numeraire.c` can compile `numeraire.c` on its own.
// `AGENTS.md ## MUST` names all three as the exceptions to the
// `WHENMOON_INTERNAL` gate. The argument for a third is the one the
// other two made and this one makes louder: these conversions decide
// how much real money an armed market may commit, and a wrong answer
// here is silent in every log.

// Satoshis per bitcoin. Every satoshi figure the office can ever hold
// is exact in a double: 21e6 BTC is 2.1e15 sats, well inside 2^53.
#define WM_SATS_PER_BTC  100000000.0

// Which side of a pair is bitcoin, and therefore how a quote-currency
// amount on that market reaches the numeraire.
//
// ⛔ NONE is 0 so a zero-initialised leg REFUSES. Every function below
// answers false for it and no caller may substitute a rate of its own:
// a market with no bitcoin leg (say eth-usd) cannot be priced in
// satoshis from its own mark, and guessing is how a control silently
// stops controlling.
typedef enum
{
  WM_BTC_LEG_NONE = 0,   // no bitcoin leg — unpriceable here
  WM_BTC_LEG_BASE,       // btc-usd: the mark IS the bitcoin price
  WM_BTC_LEG_QUOTE,      // sol-btc: the quote currency IS bitcoin
} wm_btc_leg_t;

// Classify the venue's wire symbol, "BASE-QUOTE". Case-insensitive
// because the case is the venue's to choose, not ours.
wm_btc_leg_t wm_numeraire_leg(const char *product_id);

// Convert between a quote-currency amount on this market and satoshis.
// `mark_px` is that market's own mark and is consulted only where the
// leg needs it; it must be finite and positive when it is. False leaves
// `*out` untouched — the caller must refuse, never fall through.
bool wm_numeraire_quote_to_sats(wm_btc_leg_t leg, double mark_px,
    double quote_amt, double *out_sats);

bool wm_numeraire_sats_to_quote(wm_btc_leg_t leg, double mark_px,
    double sats, double *out_quote);

// A book of (`cash` quote currency + `position_qty` base) expressed in
// satoshis at `mark_px` — the office's score for that book. A leg whose
// quantity is zero contributes nothing and asks nothing of the mark, so
// a market holding only bitcoin is measurable before its first tick.
bool wm_numeraire_stack_sats(wm_btc_leg_t leg, double mark_px,
    double cash, double position_qty, double *out_sats);

#endif // BM_WM_NUMERAIRE_H
