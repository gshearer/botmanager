#ifndef BM_COINMARKETCAP_API_H
#define BM_COINMARKETCAP_API_H

// Public mechanism API for the coinmarketcap service plugin. Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym("coinmarketcap", …) — the plugin is loaded RTLD_LOCAL.
//
// Shim shape mirrors plugins/service/openweather/openweather_api.h: an
// atomic-guarded static cache per symbol, union to launder
// void*↔function-pointer conversion, FATAL + abort on lookup miss.
//
// Inside the coinmarketcap plugin itself the static-inline shims below
// would collide with the real definitions, so coinmarketcap.c defines
// CMC_INTERNAL before including this header to skip them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"  // SUCCESS/FAIL

// Size limits used by the result structs. Fixed sizes let callers
// stack-allocate results and avoid lifetime ambiguity on inner strings.

#define COINMARKETCAP_SYMBOL_SZ     16
#define COINMARKETCAP_NAME_SZ       64
#define COINMARKETCAP_DATE_SZ       32
#define COINMARKETCAP_MAX_SELECT    32
#define COINMARKETCAP_MAX_LISTINGS  500

// Sort column indices (bulk accessor).
#define COINMARKETCAP_SORT_RANK     0
#define COINMARKETCAP_SORT_SYMBOL   1
#define COINMARKETCAP_SORT_PRICE    2
#define COINMARKETCAP_SORT_CAP      3
#define COINMARKETCAP_SORT_1H       4
#define COINMARKETCAP_SORT_24H      5
#define COINMARKETCAP_SORT_7D       6
#define COINMARKETCAP_SORT_VOL      7

// Per-coin snapshot parsed from the Listings Latest response.
typedef struct
{
  int32_t   id;
  int32_t   cmc_rank;
  char      name[COINMARKETCAP_NAME_SZ];
  char      symbol[COINMARKETCAP_SYMBOL_SZ];
  double    price;
  double    market_cap;
  double    volume_24h;
  double    pct_1h;
  double    pct_24h;
  double    pct_7d;
  double    circulating_supply;
  double    total_supply;
  double    max_supply;
  int32_t   num_market_pairs;
} coinmarketcap_coin_t;

// Extended detail for verbose mode (Quotes Latest response).
typedef struct
{
  coinmarketcap_coin_t  base;
  double                market_cap_dominance;
  double                fully_diluted_market_cap;
  double                volume_change_24h;
  double                pct_30d;
  double                pct_60d;
  double                pct_90d;
  char                  date_added[COINMARKETCAP_DATE_SZ];
} coinmarketcap_coin_detail_t;

// Global market-metrics snapshot.
typedef struct
{
  int32_t   active_cryptos;
  int32_t   active_exchanges;
  double    btc_dom;
  double    eth_dom;
  double    defi_vol_24h;
  double    defi_cap;
  double    stablecoin_vol;
  double    stablecoin_cap;
  double    derivatives_vol;
  double    total_cap;
  double    total_vol;
  double    total_cap_yest;
  double    total_vol_yest;
} coinmarketcap_global_t;

// Async fetch result payloads. On success, err[0] == '\0' and the inner
// payload is populated. On failure, err carries a human-readable reason
// ready to forward to the user.

typedef struct
{
  char                        err[128];
  coinmarketcap_coin_detail_t detail;
} coinmarketcap_detail_result_t;

typedef struct
{
  char                     err[128];
  coinmarketcap_global_t   global;
} coinmarketcap_global_result_t;

typedef struct
{
  char err[128];
  // On success the listings cache has been refreshed. Consumer uses
  // coinmarketcap_get_listings() to read it.
} coinmarketcap_listings_result_t;

// Callback signatures. Callbacks run on the curl-multi worker thread
// owned by the coinmarketcap plugin — do not block.
typedef void (*coinmarketcap_done_detail_cb_t)(
    const coinmarketcap_detail_result_t *res, void *user);

typedef void (*coinmarketcap_done_global_cb_t)(
    const coinmarketcap_global_result_t *res, void *user);

typedef void (*coinmarketcap_done_listings_cb_t)(
    const coinmarketcap_listings_result_t *res, void *user);

// Real function declarations — visible only inside the coinmarketcap
// plugin (where CMC_INTERNAL is defined). External consumers go through
// the static-inline dlsym shims defined further down.
#ifdef CMC_INTERNAL

// Sync cache reads. Return SUCCESS/FAIL. All routines serialize on the
// plugin's internal rwlock; safe to call concurrently with async fetches.

// Read a coin snapshot from cache by symbol (case-insensitive). Returns
// FAIL if cache is empty or symbol is not present.
bool coinmarketcap_get_coin_by_symbol(const char *symbol,
    coinmarketcap_coin_t *out);

// Read a coin snapshot from cache by rank. Returns FAIL if not present.
bool coinmarketcap_get_coin_by_rank(int32_t rank,
    coinmarketcap_coin_t *out);

// Bulk copy of the cache, sorted by the requested column. Caller
// supplies an output array of capacity *out_cap; on return *out_count
// holds the number of rows populated (min of cache size, out_cap,
// limit). A limit of 0 means "no limit". Returns FAIL on empty cache.
bool coinmarketcap_get_listings(uint32_t limit, uint8_t sort_col,
    bool reverse, coinmarketcap_coin_t *out_arr,
    uint32_t out_cap, uint32_t *out_count);

// Copy cached global metrics. Returns FAIL if no successful fetch has
// occurred yet.
bool coinmarketcap_get_global(coinmarketcap_global_t *out);

// Returns true when the listings cache is populated and within the
// configured cache TTL.
bool coinmarketcap_listings_cache_fresh(void);

// Returns true when the global cache is populated and within the
// configured cache TTL.
bool coinmarketcap_global_cache_fresh(void);

// Force a refresh of the listings cache. On success, done_cb fires with
// res->err == ""; consumer then reads via coinmarketcap_get_listings().
// Returns FAIL if the request could not be queued (e.g. no API key); on
// FAIL the callback is NOT invoked.
bool coinmarketcap_fetch_listings_async(
    coinmarketcap_done_listings_cb_t done_cb, void *user);

// Fetch extended detail for a single coin by symbol (case-insensitive)
// or rank. Exactly one of symbol or rank must be supplied (symbol != NULL
// vs rank > 0).
bool coinmarketcap_fetch_detail_async(const char *symbol, int32_t rank,
    coinmarketcap_done_detail_cb_t done_cb, void *user);

// Force a refresh of the global metrics cache.
bool coinmarketcap_fetch_global_async(
    coinmarketcap_done_global_cb_t done_cb, void *user);

// Returns the current value of plugin.coinmarketcap.default_limit.
uint32_t coinmarketcap_default_limit_kv_value(void);

// Returns true when plugin.coinmarketcap.apikey is configured.
bool coinmarketcap_apikey_configured(void);

#endif // CMC_INTERNAL

// ----------------------------------------------------------------------
// dlsym shim helpers
// ----------------------------------------------------------------------

#ifndef CMC_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
coinmarketcap_get_coin_by_symbol(const char *symbol,
    coinmarketcap_coin_t *out)
{
  typedef bool (*fn_t)(const char *, coinmarketcap_coin_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_get_coin_by_symbol");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_get_coin_by_symbol");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(symbol, out));
}

static inline bool
coinmarketcap_get_coin_by_rank(int32_t rank, coinmarketcap_coin_t *out)
{
  typedef bool (*fn_t)(int32_t, coinmarketcap_coin_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_get_coin_by_rank");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_get_coin_by_rank");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(rank, out));
}

static inline bool
coinmarketcap_get_listings(uint32_t limit, uint8_t sort_col,
    bool reverse, coinmarketcap_coin_t *out_arr,
    uint32_t out_cap, uint32_t *out_count)
{
  typedef bool (*fn_t)(uint32_t, uint8_t, bool,
      coinmarketcap_coin_t *, uint32_t, uint32_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_get_listings");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_get_listings");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(limit, sort_col, reverse, out_arr, out_cap, out_count));
}

static inline bool
coinmarketcap_get_global(coinmarketcap_global_t *out)
{
  typedef bool (*fn_t)(coinmarketcap_global_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap", "coinmarketcap_get_global");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_get_global");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(out));
}

static inline bool
coinmarketcap_listings_cache_fresh(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_listings_cache_fresh");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_listings_cache_fresh");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
coinmarketcap_global_cache_fresh(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_global_cache_fresh");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_global_cache_fresh");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
coinmarketcap_fetch_listings_async(
    coinmarketcap_done_listings_cb_t done_cb, void *user)
{
  typedef bool (*fn_t)(coinmarketcap_done_listings_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_fetch_listings_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_fetch_listings_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(done_cb, user));
}

static inline bool
coinmarketcap_fetch_detail_async(const char *symbol, int32_t rank,
    coinmarketcap_done_detail_cb_t done_cb, void *user)
{
  typedef bool (*fn_t)(const char *, int32_t,
      coinmarketcap_done_detail_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_fetch_detail_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_fetch_detail_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(symbol, rank, done_cb, user));
}

static inline bool
coinmarketcap_fetch_global_async(
    coinmarketcap_done_global_cb_t done_cb, void *user)
{
  typedef bool (*fn_t)(coinmarketcap_done_global_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_fetch_global_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_fetch_global_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(done_cb, user));
}

static inline uint32_t
coinmarketcap_default_limit_kv_value(void)
{
  typedef uint32_t (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_default_limit_kv_value");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_default_limit_kv_value");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
coinmarketcap_apikey_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinmarketcap",
        "coinmarketcap_apikey_configured");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinmarketcap",
          "dlsym failed: coinmarketcap_apikey_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

#endif // !CMC_INTERNAL

#endif // BM_COINMARKETCAP_API_H
