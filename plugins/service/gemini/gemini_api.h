#ifndef BM_GEMINI_API_H
#define BM_GEMINI_API_H

// Public mechanism API for the gemini service plugin. Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym("gemini", …) — the plugin is loaded RTLD_LOCAL.
//
// The shim shape mirrors plugins/service/kraken/kraken_api.h:
// per-symbol atomic cache guard, union to launder void*↔function-
// pointer conversion, FATAL + abort on dlsym miss.
//
// Inside the gemini plugin itself the static-inline shims below
// would collide with the real definitions, so the plugin's TUs
// define GEM_INTERNAL before including this header to skip them.
//
// GEM-1 ships the credential-state probe + the symbols-refresh
// public entry point. GEM-2 lands the typed REST wrappers (candles,
// balances, orders, fills). Consumers normally go through the
// generic exchange abstraction (`exchange_api.h`) rather than these
// symbols directly; the typed surface here is for plugin-internal
// use plus the rare consumer that needs Gemini-specific behaviour.
//
// `exchange_api.h` is pulled in for `exchange_granularity_t` which the
// candles wrapper signature consumes — this keeps the dlsym shim self-
// contained for any external consumer that does want the typed path.

#include "exchange_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fixed size limits for public result structs.
// Gemini's native symbol is the concatenated lowercase form
// (`btcusd`, `ethusd`); the abstraction surface uses the hyphenated
// uppercase form (`BTC-USD`). 24 B accommodates both with NUL.
#define GEMINI_PRODUCT_ID_SZ    24
#define GEMINI_CURRENCY_SZ       8
#define GEMINI_ORDER_ID_SZ      64   // matches EXCHANGE_ORDER_ID_SZ
#define GEMINI_CLIENT_OID_SZ    64   // matches EXCHANGE_CLIENT_OID_SZ
#define GEMINI_SIDE_SZ           8   // "buy" / "sell"
#define GEMINI_STATUS_SZ        16
#define GEMINI_TIF_SZ            8
#define GEMINI_TYPE_SZ          24   // Gemini types: "exchange limit", ...

// Granularity strings accepted by GET /v2/candles/<sym>/<gran>.
// Note: Gemini does NOT support 4hr or 1week tiers — the candle
// fetcher in GEM-2 FAILs on EXCH_GRAN_4H / EXCH_GRAN_1W.
#define GEMINI_GRAN_1M    "1m"
#define GEMINI_GRAN_5M    "5m"
#define GEMINI_GRAN_15M   "15m"
#define GEMINI_GRAN_30M   "30m"
#define GEMINI_GRAN_1H    "1hr"
#define GEMINI_GRAN_6H    "6hr"
#define GEMINI_GRAN_1D    "1day"

// Max rows the typed wrappers will surface to the caller. Gemini's
// candles endpoint returns up to 500 rows per call (no `since`
// parameter — client-side filtering applies at the vtable seam).
#define GEMINI_MAX_CANDLES        500
#define GEMINI_MAX_ORDERS_LIST    100
#define GEMINI_MAX_FILLS_LIST     100
#define GEMINI_MAX_ACCOUNTS        64

// Symbols cache capacity. Mirror of GEM_SYMS_CAP. Defined here too
// so external consumers (none today; future Gemini-specific cmd
// plugin) can size their iteration buffer without pulling gemini.h.
#define GEMINI_SYMBOLS_CAP        1024

// Buffer for the err string on every *_result_t. 128 B is enough for
// the longest Gemini error envelope we surface today.
#define GEMINI_ERR_SZ             128

// Public type stubs. Plugin-internal callers consume the typed
// callbacks; the exchange-vtable adapter (GEM-2) translates these
// to the generic exchange_*_t shapes at the seam.

typedef struct
{
  int64_t  ts_open_ms;       // bucket open, ms since epoch
  double   open;
  double   high;
  double   low;
  double   close;
  double   volume;
} gemini_candle_t;

typedef struct
{
  char            err[GEMINI_ERR_SZ];
  uint32_t        count;
  gemini_candle_t rows[GEMINI_MAX_CANDLES];
} gemini_candles_result_t;

typedef struct
{
  char    order_id[GEMINI_ORDER_ID_SZ];
  char    client_oid[GEMINI_CLIENT_OID_SZ];
  char    product_id[GEMINI_PRODUCT_ID_SZ];
  char    side[GEMINI_SIDE_SZ];
  char    type[GEMINI_TYPE_SZ];
  char    status[GEMINI_STATUS_SZ];
  char    tif[GEMINI_TIF_SZ];
  double  price;
  double  size;
  double  filled_size;
  double  executed_value;
  double  fill_fees;
  bool    post_only;
  bool    settled;
  int64_t created_at_ms;
} gemini_order_t;

typedef struct
{
  char           err[GEMINI_ERR_SZ];
  gemini_order_t order;
} gemini_order_result_t;

typedef struct
{
  char           err[GEMINI_ERR_SZ];
  uint32_t       count;
  gemini_order_t rows[GEMINI_MAX_ORDERS_LIST];
} gemini_orders_result_t;

typedef struct
{
  char    currency[GEMINI_CURRENCY_SZ];   // "BTC", "USD", "ETH"
  double  balance;                        // total = available + hold
  double  hold;                           // reserved by open orders
  double  available;                      // free for trading
} gemini_account_t;

typedef struct
{
  char             err[GEMINI_ERR_SZ];
  uint32_t         count;
  gemini_account_t rows[GEMINI_MAX_ACCOUNTS];
} gemini_balances_result_t;

typedef struct
{
  char    order_id[GEMINI_ORDER_ID_SZ];
  char    client_oid[GEMINI_CLIENT_OID_SZ];
  char    product_id[GEMINI_PRODUCT_ID_SZ];
  char    side[GEMINI_SIDE_SZ];
  int64_t trade_id;
  double  price;
  double  size;
  double  fee;
  int64_t time_ms;
} gemini_fill_t;

typedef struct
{
  char          err[GEMINI_ERR_SZ];
  uint32_t      count;
  gemini_fill_t rows[GEMINI_MAX_FILLS_LIST];
} gemini_fills_result_t;

// POST new-order request body. Caller zero-initializes, fills in the
// fields, and passes by const pointer. Validation happens client-side
// against Gemini's documented type/timeinforce combinations before
// any signed request leaves the daemon.
//
// `product_id` accepts the canonical hyphenated form (`BTC-USD`) or
// the Gemini-native form (`btcusd`); the symbols cache normalises.
typedef struct
{
  char    product_id[GEMINI_PRODUCT_ID_SZ];
  char    side[GEMINI_SIDE_SZ];           // "buy" / "sell"
  char    type[GEMINI_TYPE_SZ];           // "exchange limit" / "market"
  char    tif[GEMINI_TIF_SZ];             // "GTC" / "IOC" / "FOK"
  double  price;                          // limit only
  double  size;                           // base-ccy amount
  bool    post_only;
  char    client_oid[GEMINI_CLIENT_OID_SZ];
} gemini_place_order_req_t;

// One row in the symbols cache. `native` is the Gemini-native form
// (lowercase concatenated: `btcusd`); `abstr` is the abstraction-side
// canonical form (uppercase hyphenated: `BTC-USD`); `base` and `quote`
// are split for callers that need to address the currencies directly.
typedef struct
{
  char    native[16];     // "btcusd"
  char    abstr [16];     // "BTC-USD"
  char    base  [8];      // "BTC"
  char    quote [8];      // "USD"
} gemini_pair_t;

// Symbols refresh result. The cache is populated by side-effect; the
// result struct just carries the refreshed row count + an error string
// so the caller can react to a failed refresh.
typedef struct
{
  char     err[GEMINI_ERR_SZ];
  uint32_t count;
} gemini_symbols_result_t;

// Callback signatures. Run on the curl-multi worker thread owned by
// the plugin — do not block.
typedef void (*gemini_done_candles_cb_t)(
    const gemini_candles_result_t *res, void *user);

typedef void (*gemini_done_order_cb_t)(
    const gemini_order_result_t *res, void *user);

typedef void (*gemini_done_orders_cb_t)(
    const gemini_orders_result_t *res, void *user);

typedef void (*gemini_done_balances_cb_t)(
    const gemini_balances_result_t *res, void *user);

typedef void (*gemini_done_fills_cb_t)(
    const gemini_fills_result_t *res, void *user);

typedef void (*gemini_done_symbols_cb_t)(
    const gemini_symbols_result_t *res, void *user);

// ------------------------------------------------------------------
// Real function declarations — visible only inside the gemini plugin
// (where GEM_INTERNAL is defined). External consumers go through the
// static-inline dlsym shims defined further down.
// ------------------------------------------------------------------

#ifdef GEM_INTERNAL

// Returns true iff both plugin.gemini.creds.api_key and
// plugin.gemini.creds.private_key are set (non-empty) AND
// creds.private_key base64-decodes cleanly.
bool gemini_apikey_configured(void);

// Refresh the symbols cache via GET /v1/symbols followed by per-symbol
// GET /v1/symbols/details/<sym>. Safe to call with cb=NULL for the
// fire-and-forget pattern. The cache is populated by side-effect via
// gemini_pairs.c regardless of whether `cb` is supplied.
//
// Implementation note: as of 2026-05 Gemini's `/v1/symbols/details`
// endpoint accepts a single symbol per call (no batch form), so the
// populator issues one detail request per listed symbol, completing
// asynchronously. The callback fires once after the last detail
// response lands; partial-failure rows are skipped silently.
bool gemini_symbols_refresh_async(gemini_done_symbols_cb_t cb, void *user);

// ------------------------------------------------------------------
// GEM-2 typed REST wrappers (forwarded to by gemini_exchange.c via the
// vtable capability hooks; also callable directly from other gemini-
// internal TUs). Each runs the corresponding gem_submit_{public,
// private} call, parses the response, and fires the typed callback on
// the curl worker thread.
// ------------------------------------------------------------------

// Public GET /v2/candles/<native>/<time_frame>. since_ms / until_ms are
// client-side window bounds applied after parse (Gemini's endpoint has
// no `since` parameter). `prio` is forwarded to the curl scheduler.
bool gemini_fetch_candles_async(const char *pair,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    uint8_t prio, gemini_done_candles_cb_t cb, void *user);

// Private POST /v1/balances.
bool gemini_get_balance_async(gemini_done_balances_cb_t cb, void *user);

// Private POST /v1/order/new.
bool gemini_add_order_async(const gemini_place_order_req_t *req,
    gemini_done_order_cb_t cb, void *user);

// Private POST /v1/order/cancel. `order_id` must be a positive decimal
// integer in string form (Gemini's wire type is numeric).
bool gemini_cancel_order_async(const char *order_id,
    gemini_done_order_cb_t cb, void *user);

// Private POST /v1/order/status.
bool gemini_query_order_async(const char *order_id,
    gemini_done_order_cb_t cb, void *user);

// Private POST /v1/orders — Gemini only surfaces OPEN orders here.
bool gemini_active_orders_async(gemini_done_orders_cb_t cb, void *user);

// Private POST /v1/mytrades. `product_id` is required (Gemini scopes
// the endpoint per symbol); since_ms is an optional lower bound.
bool gemini_mytrades_async(const char *product_id, int64_t since_ms,
    gemini_done_fills_cb_t cb, void *user);

#endif // GEM_INTERNAL

// ------------------------------------------------------------------
// dlsym shim helpers
// ------------------------------------------------------------------

#ifndef GEM_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
gemini_apikey_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_apikey_configured");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_apikey_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
gemini_symbols_refresh_async(gemini_done_symbols_cb_t cb, void *user)
{
  typedef bool (*fn_t)(gemini_done_symbols_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_symbols_refresh_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_symbols_refresh_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(cb, user));
}

static inline bool
gemini_fetch_candles_async(const char *pair, exchange_granularity_t gran,
    int64_t since_ms, int64_t until_ms, uint8_t prio,
    gemini_done_candles_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, exchange_granularity_t,
      int64_t, int64_t, uint8_t,
      gemini_done_candles_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_fetch_candles_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_fetch_candles_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(pair, gran, since_ms, until_ms, prio, cb, user));
}

static inline bool
gemini_get_balance_async(gemini_done_balances_cb_t cb, void *user)
{
  typedef bool (*fn_t)(gemini_done_balances_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_get_balance_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_get_balance_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(cb, user));
}

static inline bool
gemini_add_order_async(const gemini_place_order_req_t *req,
    gemini_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const gemini_place_order_req_t *,
      gemini_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_add_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_add_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(req, cb, user));
}

static inline bool
gemini_cancel_order_async(const char *order_id,
    gemini_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, gemini_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_cancel_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_cancel_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(order_id, cb, user));
}

static inline bool
gemini_query_order_async(const char *order_id,
    gemini_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, gemini_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_query_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_query_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(order_id, cb, user));
}

static inline bool
gemini_active_orders_async(gemini_done_orders_cb_t cb, void *user)
{
  typedef bool (*fn_t)(gemini_done_orders_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_active_orders_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_active_orders_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(cb, user));
}

static inline bool
gemini_mytrades_async(const char *product_id, int64_t since_ms,
    gemini_done_fills_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, int64_t,
      gemini_done_fills_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("gemini", "gemini_mytrades_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "gemini",
          "dlsym failed: gemini_mytrades_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(product_id, since_ms, cb, user));
}

#endif // !GEM_INTERNAL

#endif // BM_GEMINI_API_H
