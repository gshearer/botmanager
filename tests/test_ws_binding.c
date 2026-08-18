// botmanager — MIT
// Cases for ws_binding.h, the predicate that decides whether a live WS
// subscription may be left alone. Both ways it can be wrong are silent:
// answering "same" when the set changed leaves a market with no feed and
// nothing logged, and answering "differs" when it did not restores
// OBS-41's ~17-second gap on every mutation. The order-insensitivity is
// the specific trap — the desired list is gathered by walking the market
// array, so removing a market reorders it without changing the set.
#include "test.h"
#include "ws_binding.h"

#include <stdio.h>
#include <string.h>

// The recorded side of every comparison case. Deliberately not in
// sorted order: an implementation that sorted one side and not the
// other would pass a sorted fixture.
static const char *const bound[] = { "BTC-USD", "SOL-USD", "ETH-USD" };

static const char *const same_order[]     = { "BTC-USD", "SOL-USD", "ETH-USD" };
static const char *const other_order[]    = { "ETH-USD", "BTC-USD", "SOL-USD" };
static const char *const reversed[]       = { "ETH-USD", "SOL-USD", "BTC-USD" };
static const char *const one_removed[]    = { "BTC-USD", "ETH-USD" };
static const char *const one_added[]      = { "BTC-USD", "SOL-USD", "ETH-USD",
                                              "DOGE-USD" };
static const char *const one_swapped[]    = { "BTC-USD", "SOL-USD", "XRP-USD" };
static const char *const with_dup[]       = { "SOL-USD", "BTC-USD", "ETH-USD",
                                              "BTC-USD" };
static const char *const dup_hides_loss[] = { "BTC-USD", "BTC-USD", "ETH-USD" };
static const char *const with_empty[]     = { "BTC-USD", "", "SOL-USD",
                                              "ETH-USD" };
static const char *const with_null[]      = { "BTC-USD", NULL, "SOL-USD",
                                              "ETH-USD" };
static const char *const prefix_only[]    = { "BTC-USD", "SOL-USD", "ETH-US" };

static const struct
{
  const char        *name;
  const char *const *products;
  uint32_t           n;
  bool               want;
} differ_cases[] = {
  { "same set, same order",              same_order,     3, false },
  { "same set, rotated",                 other_order,    3, false },
  { "same set, reversed",                reversed,       3, false },
  { "same set with a duplicate",         with_dup,       4, false },
  { "same set with an empty entry",      with_empty,     4, false },
  { "same set with a NULL entry",        with_null,      4, false },
  { "one product removed",               one_removed,    2, true  },
  { "one product added",                 one_added,      4, true  },
  { "one product swapped",               one_swapped,    3, true  },
  { "a dup padding a shorter set",       dup_hides_loss, 3, true  },
  { "a product that is only a prefix",   prefix_only,    3, true  },
  { "the empty set",                     NULL,           0, true  },
};

int
main(void)
{
  wm_ws_product_set_t set;
  wm_ws_product_set_t empty;
  wm_ws_product_set_t full;
  const char         *many[WM_WS_BINDING_MAX_PRODUCTS + 1];
  char                many_buf[WM_WS_BINDING_MAX_PRODUCTS + 1][8];
  size_t              i;

  test_check_bool("record", "a set that fits is recorded", true,
      wm_ws_product_set_record(&set, bound,
          sizeof(bound) / sizeof(bound[0])));
  test_check_sz("record", "duplicates are not stored twice", 3,
      set.n_products);
  test_check_bool("record", "a set that fits is not truncated", false,
      set.truncated);

  for(i = 0; i < sizeof(differ_cases) / sizeof(differ_cases[0]); i++)
  {
    test_check_bool("differs", differ_cases[i].name,
        differ_cases[i].want,
        wm_ws_product_set_differs(&set, differ_cases[i].products,
            differ_cases[i].n));
  }

  // An empty recorded set is what a freshly cleared slot holds. It must
  // differ from anything wanted, or a reused tombstone would inherit
  // "nothing to do" and never subscribe.
  wm_ws_product_set_clear(&empty);
  test_check_bool("empty", "clear leaves no products", true,
      empty.n_products == 0 && !empty.truncated);
  test_check_bool("empty", "an empty record differs from a wanted set",
      true, wm_ws_product_set_differs(&empty, bound, 3));
  test_check_bool("empty", "an empty record matches an empty want",
      false, wm_ws_product_set_differs(&empty, NULL, 0));

  test_check_bool("null", "a NULL set always differs", true,
      wm_ws_product_set_differs(NULL, bound, 3));
  test_check_bool("null", "a NULL set cannot be recorded into", false,
      wm_ws_product_set_record(NULL, bound, 3));

  // Overflow: one product past what a binding remembers. The record
  // must report the miss and the comparison must then answer "differs"
  // unconditionally — degrading to the unconditional rebuild, never to
  // a subscription silently left unrenewed.
  for(i = 0; i < WM_WS_BINDING_MAX_PRODUCTS + 1; i++)
  {
    snprintf(many_buf[i], sizeof(many_buf[i]), "P%zu-USD", i);
    many[i] = many_buf[i];
  }

  test_check_bool("truncation", "an oversized set reports the miss",
      false, wm_ws_product_set_record(&full, many,
          WM_WS_BINDING_MAX_PRODUCTS + 1));
  test_check_bool("truncation", "an oversized set is marked truncated",
      true, full.truncated);
  test_check_bool("truncation", "a truncated record differs from itself",
      true, wm_ws_product_set_differs(&full, many,
          WM_WS_BINDING_MAX_PRODUCTS + 1));

  return(test_report("ws_binding"));
}
