#ifndef BM_STOCKQUOTE_H
#define BM_STOCKQUOTE_H

// Provider-neutral stock/equity/fund/index/FX/commodity quote contract.
//
// This header is the spine shared by two decoupled plugins:
//
//   * a *provider* — any service plugin that fetches quotes from some
//     data source (yahoofinance now; finnhub/etc. later). It declares
//     `.provides = { "stock_quotes" }`, defines STOCKQUOTE_PROVIDER_INTERNAL
//     before including this header to get the real prototypes, and exports
//     all four `stockquote_*` symbols. All source-specific nuance is sealed
//     inside the provider.
//
//   * a *consumer* — the `stock` command plugin (and anything else wanting a
//     quote). It declares `.requires = { "stock_quotes" }` and calls the
//     `stockquote_*` functions below. It never names a provider: the
//     capability-resolved shims here bind to whoever currently `provides`
//     "stock_quotes" via plugin_find_feature(), so swapping the data source
//     is a load/unload, not a recompile.
//
// Shim shape mirrors plugins/service/coinmarketcap/coinmarketcap_api.h — an
// atomic-guarded static cache per symbol, a union to launder
// void*↔function-pointer, FATAL + abort when a loaded provider is missing a
// contract symbol — with one delta: the provider name is resolved at runtime
// via plugin_find_feature("stock_quotes")->name rather than hardcoded. The
// cached slot is registered with plugin_dlsym_cached() so it is NULLed on
// provider unload; the next call then re-resolves, giving a clean hot-swap.
//
// Normalization conventions every provider MUST honor (the service
// normalizes; the consumer only presents — it derives nothing, guesses
// nothing): change_pct / session_change_pct / dividend_yield are in *percent*
// (1.23 means 1.23%); volume / avg_volume are in *shares*; market_cap and all
// prices are in the quote's `currency`; `as_of` is epoch *seconds* UTC.
// Optional numerics carry NAN when absent (test with isnan() at render time),
// strings are "", enums are the _UNKNOWN member.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"  // SUCCESS/FAIL

// ----------------------------------------------------------------------
// Enumerations (every one has UNKNOWN = 0 so a zeroed struct is "unknown")
// ----------------------------------------------------------------------

typedef enum
{
  QUOTE_CLASS_UNKNOWN = 0,
  QUOTE_CLASS_EQUITY,
  QUOTE_CLASS_ETF,
  QUOTE_CLASS_FUND,
  QUOTE_CLASS_INDEX,
  QUOTE_CLASS_FX,
  QUOTE_CLASS_CRYPTO,
  QUOTE_CLASS_FUTURE
} quote_class_t;

typedef enum
{
  MARKET_STATE_UNKNOWN = 0,
  MARKET_STATE_PRE,
  MARKET_STATE_OPEN,
  MARKET_STATE_POST,
  MARKET_STATE_CLOSED
} market_state_t;

typedef enum
{
  QUOTE_OK = 0,
  QUOTE_NOT_FOUND,
  QUOTE_RATE_LIMITED,
  QUOTE_AUTH,
  QUOTE_TRANSPORT,
  QUOTE_UNAVAILABLE
} quote_status_t;

typedef enum
{
  QUOTE_RANGE_1D = 0,
  QUOTE_RANGE_5D,
  QUOTE_RANGE_1M,
  QUOTE_RANGE_3M,
  QUOTE_RANGE_6M,
  QUOTE_RANGE_1Y,
  QUOTE_RANGE_5Y
} quote_range_t;

// ----------------------------------------------------------------------
// Capability bits — a provider advertises what it really supports via
// stockquote_provider_caps(); consumers degrade silently on the rest.
// ----------------------------------------------------------------------

#define QUOTE_CAP_SEARCH        0x01u  // stockquote_search_async is real
#define QUOTE_CAP_SERIES        0x02u  // stockquote_series_async is real
#define QUOTE_CAP_FUNDAMENTALS  0x04u  // market_cap/pe/eps/dividend/... populated
#define QUOTE_CAP_EXTHOURS      0x08u  // market_state + session_* populated
#define QUOTE_CAP_DEPTH         0x10u  // bid/ask populated

// ----------------------------------------------------------------------
// The neutral quote. Fixed-size strings let consumers stack-allocate and
// avoid inner-string lifetime ambiguity. Optional numerics are NAN when
// absent, strings "", enums _UNKNOWN.
// ----------------------------------------------------------------------

typedef struct
{
  // classification (set on success)
  char           symbol[16];      // normalized upper-case: "AAPL","^GSPC","BTC-USD"
  char           name[64];        // "" if the provider omits it
  char           currency[8];     // ISO-4217 "USD"; "" if unknown
  char           exchange[32];    // display venue; "" if unknown
  quote_class_t  klass;
  int64_t        as_of;           // epoch-seconds of the price; 0 if unknown
  quote_status_t status;          // per-symbol (a batch may partially fail)

  // core market data (universal block)
  double         price, change, change_pct, prev_close;  // change/pct derived if omitted
  double         open, day_high, day_low, volume;        // NAN if absent
  uint8_t        price_decimals;  // display hint; 0 = consumer decides by magnitude

  // range context
  double         year_high, year_low;                    // 52-week; NAN if absent

  // depth (QUOTE_CAP_DEPTH)
  double         bid, ask, bid_size, ask_size;           // NAN if absent

  // fundamentals (QUOTE_CAP_FUNDAMENTALS)
  double         market_cap, pe_trailing, pe_forward, eps_ttm;
  double         dividend_yield, avg_volume, shares_out, ma50, ma200, beta;

  // extended hours (QUOTE_CAP_EXTHOURS)
  market_state_t market_state;
  double         session_price, session_change, session_change_pct;

  // inline intraday sparkline (bundled with the quote when available)
  float          spark[32];
  uint16_t       spark_n;
} quote_t;

// ----------------------------------------------------------------------
// Result envelopes + callbacks. Every payload — the envelope and any array
// it points at — is valid only for the duration of the callback; consumers
// that need to outlive it must deep-copy.
// ----------------------------------------------------------------------

typedef struct
{
  // Dispatch-level status, NOT an aggregate of the per-symbol results.
  // Once the callback fires it means "the batch was dispatched"; a
  // provider that issues one request per symbol reports every real
  // outcome — including a total failure — in quotes[i].status. Consumers
  // MUST iterate quotes[i].status per symbol and never shortcut on this
  // field. (A pre-dispatch failure returns FAIL from the submit call and
  // never fires the callback at all.)
  quote_status_t status;
  char           message[128]; // human-readable detail; "" on success
  quote_t       *quotes;       // n entries, one per requested symbol (order preserved)
  uint8_t        n;
} quote_batch_t;

typedef struct
{
  char          symbol[16];
  char          name[64];
  char          exchange[32];
  quote_class_t klass;
} quote_hit_t;

typedef struct
{
  quote_status_t status;
  char           message[128];
  quote_hit_t   *hits;
  uint8_t        n;
} quote_search_res_t;

typedef struct
{
  int64_t ts;     // epoch-seconds UTC
  double  close;
} quote_point_t;

typedef struct
{
  quote_status_t status;
  char           message[128];
  quote_point_t *pts;
  uint16_t       n;
} quote_series_res_t;

typedef void (*stockquote_batch_cb_t)(const quote_batch_t *, void *user);
typedef void (*stockquote_search_cb_t)(const quote_search_res_t *, void *user);
typedef void (*stockquote_series_cb_t)(const quote_series_res_t *, void *user);

// ----------------------------------------------------------------------
// Provider API. A provider MUST export all four symbols; ones it does not
// implement return FAIL and are omitted from the caps bitmask. These
// prototypes are visible only inside the provider (which defines
// STOCKQUOTE_PROVIDER_INTERNAL before including this header). Everyone else
// gets the capability-resolved shims below instead.
// ----------------------------------------------------------------------

#ifdef STOCKQUOTE_PROVIDER_INTERNAL

uint32_t stockquote_provider_caps(void);
bool     stockquote_fetch_async(const char *const *syms, uint8_t n,
             stockquote_batch_cb_t cb, void *user);
bool     stockquote_search_async(const char *query,
             stockquote_search_cb_t cb, void *user);
bool     stockquote_series_async(const char *sym, quote_range_t range,
             stockquote_series_cb_t cb, void *user);

#endif // STOCKQUOTE_PROVIDER_INTERNAL

// ----------------------------------------------------------------------
// Capability-resolved dlsym shims (consumer side)
// ----------------------------------------------------------------------

#ifndef STOCKQUOTE_PROVIDER_INTERNAL

#include <stdlib.h>  // abort

#include "clam.h"
#include "plugin.h"

// Returns 0 (no capabilities) when no provider is loaded — consumers then
// simply offer nothing. A provider that is loaded but missing this symbol is
// a contract violation and aborts.
static inline uint32_t
stockquote_provider_caps(void)
{
  typedef uint32_t (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    const plugin_desc_t *p = plugin_find_feature("stock_quotes");
    union { void *obj; fn_t fn; } u;

    if(p == NULL)
    {
      clam(CLAM_WARN, "stockquote", "no stock_quotes provider loaded");
      return(0u);
    }
    u.obj = plugin_dlsym_cached(p->name, "stockquote_provider_caps",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "stockquote",
          "provider %s lacks stockquote_provider_caps", p->name);
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
stockquote_fetch_async(const char *const *syms, uint8_t n,
    stockquote_batch_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *const *, uint8_t,
      stockquote_batch_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    const plugin_desc_t *p = plugin_find_feature("stock_quotes");
    union { void *obj; fn_t fn; } u;

    if(p == NULL)
    {
      clam(CLAM_WARN, "stockquote", "no stock_quotes provider loaded");
      return(FAIL);
    }
    u.obj = plugin_dlsym_cached(p->name, "stockquote_fetch_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "stockquote",
          "provider %s lacks stockquote_fetch_async", p->name);
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(syms, n, cb, user));
}

static inline bool
stockquote_search_async(const char *query,
    stockquote_search_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, stockquote_search_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    const plugin_desc_t *p = plugin_find_feature("stock_quotes");
    union { void *obj; fn_t fn; } u;

    if(p == NULL)
    {
      clam(CLAM_WARN, "stockquote", "no stock_quotes provider loaded");
      return(FAIL);
    }
    u.obj = plugin_dlsym_cached(p->name, "stockquote_search_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "stockquote",
          "provider %s lacks stockquote_search_async", p->name);
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(query, cb, user));
}

static inline bool
stockquote_series_async(const char *sym, quote_range_t range,
    stockquote_series_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, quote_range_t,
      stockquote_series_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    const plugin_desc_t *p = plugin_find_feature("stock_quotes");
    union { void *obj; fn_t fn; } u;

    if(p == NULL)
    {
      clam(CLAM_WARN, "stockquote", "no stock_quotes provider loaded");
      return(FAIL);
    }
    u.obj = plugin_dlsym_cached(p->name, "stockquote_series_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "stockquote",
          "provider %s lacks stockquote_series_async", p->name);
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(sym, range, cb, user));
}

#endif // !STOCKQUOTE_PROVIDER_INTERNAL

#endif // BM_STOCKQUOTE_H
