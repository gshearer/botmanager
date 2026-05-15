// gemini_pairs.h — Symbols cache.
//
// Gemini's REST surface speaks the symbol concatenated lowercase
// (`btcusd`); WS v2 speaks the same concatenated form uppercase
// (`BTCUSD`); the abstraction-side canonical form is the hyphenated
// uppercase ISO code (`BTC-USD`). The cache holds the four-field row
// {native, abstr, base, quote} so lookups in either direction are
// O(N) over a small table (~150 rows) and are populated from
// GET /v1/symbols + per-symbol GET /v1/symbols/details/<sym>.
//
// Lookups are forgiving: the caller hands in any of the three forms,
// the cache returns the form the target API surface expects, falling
// back to a heuristic split-on-suffix when the cache is cold and the
// pair hasn't been seen (Gemini's gateway responds with a clean 400
// in that case, which the response classifier surfaces).

#ifndef BM_GEMINI_PAIRS_H
#define BM_GEMINI_PAIRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle.
void    gem_pairs_init(void);
void    gem_pairs_deinit(void);

// Replace the cache contents atomically. After clear, repeated
// `gem_pairs_add` calls populate. The pair refresh helper in
// gemini_orders.c clears first, then adds every entry from the parsed
// `/v1/symbols/details` responses.
//
// gem_pairs_add returns SUCCESS when the row was committed, FAIL when
// the cache was full, the inputs were invalid, or a per-currency code
// overflow was detected. Callers use the return value to gate their
// "kept" tally so post-refresh stats reflect only successfully cached
// rows.
void    gem_pairs_clear(void);
bool    gem_pairs_add(const char *native, const char *base, const char *quote);

// Number of rows currently held. Thread-safe.
uint32_t gem_pairs_count(void);

// Lookup native (REST/WS lowercase) form from any of native, abstr, or
// the bare base/quote-concat form. Writes the result into `out`
// (NUL-terminated, truncated to cap-1). On a miss the input is
// normalised through a heuristic (strip non-alpha, lowercase) and
// copied through.
void    gem_pair_to_native(const char *input, char *out, size_t cap);

// Lookup abstraction-side `BASE-QUOTE` (uppercase hyphenated) form
// from any of native, abstr. On a miss, attempt to split on common
// quote currencies (USD/USDT/USDC/EUR/GBP/BTC/ETH/SGD/DAI) — Gemini's
// pair list is short enough that the heuristic almost always lands
// the right split.
void    gem_pair_to_abstr(const char *input, char *out, size_t cap);

#endif // BM_GEMINI_PAIRS_H
