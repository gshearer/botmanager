// botmanager — MIT
// numeraire.c — WM-NUMERAIRE-1: a market's books, priced in the office's unit.

#include "numeraire.h"

#include <math.h>
#include <string.h>
#include <strings.h>

static bool wm_numeraire_usable_mark(double);

// A mark we may divide by or multiply through. A market that has not
// ticked carries 0.0 and every caller of this file treats that as a
// refusal rather than a rate of zero.
static bool
wm_numeraire_usable_mark(double mark_px)
{
  return(isfinite(mark_px) && mark_px > 0.0);
}

wm_btc_leg_t
wm_numeraire_leg(const char *product_id)
{
  const char *dash;
  size_t      base_len;

  if(product_id == NULL)
    return(WM_BTC_LEG_NONE);

  dash = strchr(product_id, '-');

  if(dash == NULL)
    return(WM_BTC_LEG_NONE);

  base_len = (size_t)(dash - product_id);

  // Both sides must exist. The symbol is built by
  // wm_market_format_product_id from a validated triple, but this is
  // the boundary that turns a string into a rate, and "-BTC" naming a
  // bitcoin leg is not a mistake worth leaving reachable.
  if(base_len == 0 || dash[1] == '\0')
    return(WM_BTC_LEG_NONE);

  if(base_len == 3 && strncasecmp(product_id, "BTC", 3) == 0)
    return(WM_BTC_LEG_BASE);

  if(strcasecmp(dash + 1, "BTC") == 0)
    return(WM_BTC_LEG_QUOTE);

  return(WM_BTC_LEG_NONE);
}

bool
wm_numeraire_quote_to_sats(wm_btc_leg_t leg, double mark_px,
    double quote_amt, double *out_sats)
{
  if(out_sats == NULL || !isfinite(quote_amt))
    return(false);

  switch(leg)
  {
    case WM_BTC_LEG_BASE:
      if(!wm_numeraire_usable_mark(mark_px))
        return(false);

      *out_sats = quote_amt / mark_px * WM_SATS_PER_BTC;
      return(true);

    case WM_BTC_LEG_QUOTE:
      *out_sats = quote_amt * WM_SATS_PER_BTC;
      return(true);

    case WM_BTC_LEG_NONE:
      break;
  }

  return(false);
}

bool
wm_numeraire_sats_to_quote(wm_btc_leg_t leg, double mark_px, double sats,
    double *out_quote)
{
  if(out_quote == NULL || !isfinite(sats))
    return(false);

  switch(leg)
  {
    case WM_BTC_LEG_BASE:
      if(!wm_numeraire_usable_mark(mark_px))
        return(false);

      *out_quote = sats / WM_SATS_PER_BTC * mark_px;
      return(true);

    case WM_BTC_LEG_QUOTE:
      *out_quote = sats / WM_SATS_PER_BTC;
      return(true);

    case WM_BTC_LEG_NONE:
      break;
  }

  return(false);
}

// The bitcoin leg is carried at face; the other leg is marked through
// `mark_px`. Which is which is the whole content of wm_btc_leg_t, and
// the invariance the header claims falls straight out: on btc-usd a
// book holding only bitcoin has no quote leg to mark, so the price
// cannot move its stack.
bool
wm_numeraire_stack_sats(wm_btc_leg_t leg, double mark_px, double cash,
    double position_qty, double *out_sats)
{
  double btc_leg;
  double other_leg;

  if(out_sats == NULL || !isfinite(cash) || !isfinite(position_qty))
    return(false);

  // A chain rather than a switch: the trailing refusal is what lets the
  // compiler see both legs assigned on every path that reaches the
  // arithmetic, and it makes an enumerator added later fail closed.
  if(leg == WM_BTC_LEG_BASE)
  {
    btc_leg   = position_qty;
    other_leg = cash;
  }

  else if(leg == WM_BTC_LEG_QUOTE)
  {
    btc_leg   = cash;
    other_leg = position_qty;
  }

  else
    return(false);

  if(other_leg != 0.0)
  {
    if(!wm_numeraire_usable_mark(mark_px))
      return(false);

    btc_leg += (leg == WM_BTC_LEG_BASE)
        ? other_leg / mark_px
        : other_leg * mark_px;
  }

  if(!isfinite(btc_leg))
    return(false);

  *out_sats = btc_leg * WM_SATS_PER_BTC;
  return(true);
}
