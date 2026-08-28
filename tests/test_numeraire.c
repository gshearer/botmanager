// botmanager — MIT
// Cases for numeraire.h, the arithmetic that decides how much real money
// an armed whenmoon market may commit (WM-NUMERAIRE-1). Every way it can
// be wrong is silent: a leg misread as bitcoin prices a cap off an
// unrelated pair, an inverted conversion turns a 12,000-sat ceiling into
// a 12,000-dollar one, and a stack that is not price-invariant hands the
// drawdown breaker a bitcoin dip to trip on — the one behaviour CFO.md
// §2 forbids outright.
#include "test.h"
#include "numeraire.h"

#include <math.h>
#include <stddef.h>

// Round to the nearest satoshi so the harness can compare exactly:
// satoshis are integral, and every figure this office can hold is exact
// in a double (WM_SATS_PER_BTC's comment carries the arithmetic).
static size_t
sats_of(double v)
{
  return((size_t)llround(v));
}

static const struct
{
  const char   *product_id;
  wm_btc_leg_t  want;
} leg_cases[] = {
  { "BTC-USD",   WM_BTC_LEG_BASE  },
  { "btc-usd",   WM_BTC_LEG_BASE  },
  { "BTC-USDC",  WM_BTC_LEG_BASE  },
  { "SOL-BTC",   WM_BTC_LEG_QUOTE },
  { "sol-btc",   WM_BTC_LEG_QUOTE },
  { "ETH-USD",   WM_BTC_LEG_NONE  },
  { "WBTC-USD",  WM_BTC_LEG_NONE  },   // base is a superstring, not BTC
  { "USD-WBTC",  WM_BTC_LEG_NONE  },   // quote is a superstring, not BTC
  { "BT-USD",    WM_BTC_LEG_NONE  },
  { "BTCUSD",    WM_BTC_LEG_NONE  },   // no separator at all
  { "BTC-",      WM_BTC_LEG_NONE  },
  { "-BTC",      WM_BTC_LEG_NONE  },
  { "",          WM_BTC_LEG_NONE  },
  { NULL,        WM_BTC_LEG_NONE  },
};

int
main(void)
{
  double   sats;
  double   quote;
  double   flat;
  double   risen;
  size_t   i;

  for(i = 0; i < sizeof(leg_cases) / sizeof(leg_cases[0]); i++)
  {
    test_check_sz("leg",
        leg_cases[i].product_id != NULL ? leg_cases[i].product_id : "(null)",
        (size_t)leg_cases[i].want,
        (size_t)wm_numeraire_leg(leg_cases[i].product_id));
  }

  // btc-usd: the mark is the bitcoin price, so a quote amount buys
  // quote/px bitcoin. $100 at $80,000 is 125,000 sats.
  test_check_bool("quote_to_sats", "btc-usd converts at the mark", true,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_BASE, 80000.0, 100.0, &sats));
  test_check_sz("quote_to_sats", "$100 at $80k is 125,000 sats",
      125000, sats_of(sats));

  // sol-btc: the quote currency IS bitcoin, so the mark is not consulted
  // and a zero mark must not refuse.
  test_check_bool("quote_to_sats", "sol-btc needs no mark", true,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_QUOTE, 0.0, 0.5, &sats));
  test_check_sz("quote_to_sats", "0.5 BTC of quote is 50,000,000 sats",
      50000000, sats_of(sats));

  test_check_bool("quote_to_sats", "an unpriced leg refuses", false,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_NONE, 80000.0, 100.0, &sats));
  test_check_bool("quote_to_sats", "a zero mark refuses on btc-usd", false,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_BASE, 0.0, 100.0, &sats));
  test_check_bool("quote_to_sats", "a negative mark refuses", false,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_BASE, -80000.0, 100.0, &sats));
  test_check_bool("quote_to_sats", "a NaN mark refuses", false,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_BASE, NAN, 100.0, &sats));
  test_check_bool("quote_to_sats", "a NaN amount refuses", false,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_BASE, 80000.0, NAN, &sats));
  test_check_bool("quote_to_sats", "a NULL out refuses", false,
      wm_numeraire_quote_to_sats(WM_BTC_LEG_BASE, 80000.0, 100.0, NULL));

  // The round trip is what an armed cap actually walks: an operator sets
  // a satoshi ceiling and gate 4 turns it back into a quote notional.
  test_check_bool("sats_to_quote", "btc-usd converts at the mark", true,
      wm_numeraire_sats_to_quote(WM_BTC_LEG_BASE, 80000.0, 125000.0,
          &quote));
  test_check_sz("sats_to_quote", "125,000 sats at $80k is $100",
      100, (size_t)llround(quote));
  test_check_bool("sats_to_quote", "sol-btc needs no mark", true,
      wm_numeraire_sats_to_quote(WM_BTC_LEG_QUOTE, 0.0, 50000000.0,
          &quote));
  test_check_sz("sats_to_quote", "50,000,000 sats is 0.5 quote BTC",
      50000000, sats_of(quote * WM_SATS_PER_BTC));
  test_check_bool("sats_to_quote", "an unpriced leg refuses", false,
      wm_numeraire_sats_to_quote(WM_BTC_LEG_NONE, 80000.0, 1.0, &quote));

  // The stack. A btc-usd book holding only bitcoin has no quote leg to
  // mark, so its score is the position itself — and no mark at all is
  // needed to say so. This is the case a restored market is in before
  // its first tick.
  test_check_bool("stack", "an all-bitcoin book needs no mark", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 0.0, 0.0, 0.01216177,
          &sats));
  test_check_sz("stack", "0.01216177 BTC is the opening stack",
      1216177, sats_of(sats));

  // Price invariance — the property the whole chunk rests on. The same
  // all-bitcoin book scored at two very different marks must give the
  // same answer, or the drawdown breaker trips on the dip.
  test_check_bool("stack", "invariance: scored at $40k", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 40000.0, 0.0, 0.01216177,
          &flat));
  test_check_bool("stack", "invariance: scored at $120k", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 120000.0, 0.0, 0.01216177,
          &risen));
  test_check_sz("stack", "a bitcoin move does not move the stack",
      sats_of(flat), sats_of(risen));

  // Cash, by contrast, IS a position under this mandate: the same
  // dollars are worth fewer sats as bitcoin rises, and the stack says so.
  test_check_bool("stack", "an all-cash book at $80k", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 80000.0, 8000.0, 0.0,
          &flat));
  test_check_sz("stack", "$8,000 at $80k is 0.1 BTC",
      10000000, sats_of(flat));
  test_check_bool("stack", "an all-cash book at $160k", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 160000.0, 8000.0, 0.0,
          &risen));
  test_check_sz("stack", "the same cash halves when bitcoin doubles",
      5000000, sats_of(risen));

  test_check_bool("stack", "a mixed btc-usd book", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 80000.0, 8000.0, 0.01,
          &sats));
  test_check_sz("stack", "0.01 BTC plus $8,000 at $80k is 0.11 BTC",
      11000000, sats_of(sats));

  // sol-btc inverts which leg is which: cash is bitcoin at face and the
  // position is marked through. 0.5 BTC + 10 SOL at 0.002 = 0.52 BTC.
  test_check_bool("stack", "a mixed sol-btc book", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_QUOTE, 0.002, 0.5, 10.0, &sats));
  test_check_sz("stack", "0.5 BTC plus 10 SOL at 0.002 is 0.52 BTC",
      52000000, sats_of(sats));
  test_check_bool("stack", "an all-cash sol-btc book needs no mark", true,
      wm_numeraire_stack_sats(WM_BTC_LEG_QUOTE, 0.0, 0.5, 0.0, &sats));
  test_check_sz("stack", "sol-btc cash is bitcoin at face",
      50000000, sats_of(sats));

  // Refusals. A book with a leg to mark and no mark to do it with is
  // unmeasurable, and the caller must not receive a number.
  test_check_bool("stack", "cash with no mark refuses", false,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 0.0, 8000.0, 0.0, &sats));
  test_check_bool("stack", "a position with no mark refuses on sol-btc",
      false,
      wm_numeraire_stack_sats(WM_BTC_LEG_QUOTE, 0.0, 0.5, 10.0, &sats));
  test_check_bool("stack", "an unpriced leg refuses", false,
      wm_numeraire_stack_sats(WM_BTC_LEG_NONE, 80000.0, 0.0, 1.0, &sats));
  test_check_bool("stack", "a NaN cash refuses", false,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 80000.0, NAN, 0.0, &sats));
  test_check_bool("stack", "an infinite position refuses", false,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 80000.0, 0.0, INFINITY,
          &sats));
  test_check_bool("stack", "a NULL out refuses", false,
      wm_numeraire_stack_sats(WM_BTC_LEG_BASE, 80000.0, 0.0, 1.0, NULL));

  return(test_report("numeraire"));
}
