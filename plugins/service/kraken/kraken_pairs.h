// kraken_pairs.h — AssetPairs cache.
//
// Kraken's REST surface speaks three product-id dialects:
//   altname   ("XBTUSD")    — what most modern endpoints accept
//   canonical ("XXBTZUSD")  — older endpoints (legacy ID system)
//   wsname    ("BTC/USD")   — WebSocket v2 canonical form
//
// The cache is populated by GET /0/public/AssetPairs on startup and
// on the assetpairs refresh timer. Lookups are forgiving: the caller
// hands in any of the three forms, the cache returns the form the
// target API surface expects, falling back to the input unchanged on
// a miss (Kraken's gateway responds with a clean EQuery error in that
// case, which the response classifier surfaces).

#ifndef BM_KRAKEN_PAIRS_H
#define BM_KRAKEN_PAIRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle.
void    kr_pairs_init(void);
void    kr_pairs_deinit(void);

// Replace the cache contents atomically. After clear, repeated
// `kr_pairs_add` calls populate. The pair refresh helper in
// kraken_orders.c clears first, then adds every entry from the parsed
// response.
void    kr_pairs_clear(void);
void    kr_pairs_add(const char *altname, const char *canonical,
            const char *wsname);

// Number of rows currently held. Thread-safe.
uint32_t kr_pairs_count(void);

// Lookup by any of altname / canonical / wsname. Writes the result
// form into `out` (NUL-terminated, truncated to cap-1). On a miss the
// input is copied through unchanged.
//
// _rest returns altname (the form most REST endpoints accept).
// _ws   returns wsname  (the form WS v2 subscribes accept).
void    kr_pair_lookup_rest(const char *input, char *out, size_t cap);
void    kr_pair_lookup_ws  (const char *input, char *out, size_t cap);

#endif // BM_KRAKEN_PAIRS_H
