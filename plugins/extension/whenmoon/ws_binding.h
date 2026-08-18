#ifndef BM_WM_WS_BINDING_H
#define BM_WM_WS_BINDING_H

#include <stdbool.h>
#include <stdint.h>

// The product set a WebSocket binding actually subscribed with, and the
// order-insensitive comparison that decides whether it still matches
// what the running market set wants (§SC-OBSERVED OBS-41).
//
// Before this type existed there was nothing to compare against: the
// bound set was an argument handed to exchange_ws_subscribe and then
// discarded, so a market mutation could only do the maximal thing —
// unsubscribe every exchange and resubscribe every exchange, including
// the ones the mutation never touched. Measured three times on
// 2026-08-17, that cost the live Coinbase feed ~17 s with no ticks at
// all, on mutations that concerned a different venue entirely.
//
// This header includes no whenmoon header on purpose — the same leaf
// property `warm_chain.h` rests on — so `tests/test_ws_binding.c` can
// compile `ws_binding.c` on its own. `AGENTS.md ## MUST` names both
// files as the exceptions to the `WHENMOON_INTERNAL` gate.

// Mirrors WM_PRODUCT_ID_SZ (`market.h`). Copied rather than included:
// including `market.h` would make this a non-leaf and cost the suite.
// `market.h` _Static_asserts the two agree, so the copy cannot drift.
#define WM_WS_PRODUCT_ID_SZ  24

// Ceiling on the products one binding remembers.
//
// ⛔ There is no compile-time bound to size this from. The running
// market set grows without limit (`wm_market_grow` doubles from
// WM_MARKET_INIT_CAP), so no value here is provably sufficient, and the
// overflow is therefore HANDLED rather than assumed away: a set that
// does not fit is recorded `truncated`, and a truncated record always
// compares as "differs". That exchange then falls back to exactly the
// unconditional rebuild this module exists to avoid — degraded, never a
// subscription silently left unrenewed.
//
// 32 is 4x WM_MARKET_INIT_CAP; `market.h` asserts that relationship so
// raising the market array's own constant is felt here.
// ⛔ NOT sized from a driver's per-subscribe cap
// (CB_WS_CH_MAX_PRODUCTS_PER_SUB) — that bounds one frame on one
// gateway and is a different question.
#define WM_WS_BINDING_MAX_PRODUCTS  32

typedef struct
{
  char     products[WM_WS_BINDING_MAX_PRODUCTS][WM_WS_PRODUCT_ID_SZ];
  uint32_t n_products;

  // The last record did not fit. A field rather than a folded-in
  // sentinel so the caller can warn once at the record site and the
  // comparison stays a pure function of what was stored.
  bool     truncated;
} wm_ws_product_set_t;

// Record what was just subscribed with. Duplicate and empty entries are
// collapsed, so the stored form is always a set. Returns false when the
// products did not fit — the caller may warn; the set is left
// `truncated` either way.
bool wm_ws_product_set_record(wm_ws_product_set_t *set,
    const char *const *products, uint32_t n_products);

// True when `products` is a different SET from the one recorded.
// Order-insensitive, and tested by containment in BOTH directions, so
// duplicates on either side cannot make two different sets compare
// equal. A truncated record and a NULL set always differ.
//
// ⚠ Order-insensitivity is the whole point: the desired list is
// gathered by walking the market array, and removing a market changes
// that order without changing the set. An order-sensitive compare would
// rebuild on a no-op remove and quietly deliver nothing.
bool wm_ws_product_set_differs(const wm_ws_product_set_t *set,
    const char *const *products, uint32_t n_products);

// Forget the recorded set, so a slot that is reused cannot inherit the
// previous occupant's answer.
void wm_ws_product_set_clear(wm_ws_product_set_t *set);

#endif // BM_WM_WS_BINDING_H
