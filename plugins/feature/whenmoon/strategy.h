// strategy.h — two-tier KV resolver for whenmoon strategies.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_STRATEGY_H
#define BM_WHENMOON_STRATEGY_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stdint.h>

// Two-tier strategy KV lookup. Resolution order:
//   1. plugin.whenmoon.market.<market_id>.strategy.<strategy>.<key>
//   2. plugin.whenmoon.strategy.<strategy>.<key>
//   3. dflt
//
// market_id is the canonical dash form ("coinbase-btc-usd"). NULL/empty
// market_id skips tier 1 and falls through to the global slot.
//
// In WM-G1 no strategies are registered; the resolver compiles and
// returns dflt. WM-LT-3 wires per-market and global strategy slots into
// runtime kv_register at strategy load time.

uint64_t wm_strategy_kv_get_uint(const char *market_id,
    const char *strategy, const char *key, uint64_t dflt);

int64_t wm_strategy_kv_get_int(const char *market_id,
    const char *strategy, const char *key, int64_t dflt);

const char *wm_strategy_kv_get_str(const char *market_id,
    const char *strategy, const char *key, const char *dflt);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_STRATEGY_H
