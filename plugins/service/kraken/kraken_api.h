#ifndef BM_KRAKEN_API_H
#define BM_KRAKEN_API_H

// Public mechanism API for the kraken service plugin. Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym("kraken", …) — the plugin is loaded RTLD_LOCAL.
//
// The shim shape mirrors plugins/service/coinbase/coinbase_api.h:
// per-symbol atomic cache guard, union to launder void*↔function-
// pointer conversion, FATAL + abort on dlsym miss.
//
// Inside the kraken plugin itself the static-inline shims below
// would collide with the real definitions, so the plugin's TUs
// define KR_INTERNAL before including this header to skip them.
//
// KR-3 shipped the credential-state probe; KR-4 lands the typed REST
// wrappers (candles, balances, orders, fills, assetpairs cache).
// Consumers normally go through the generic exchange abstraction
// (`exchange_api.h`) rather than these symbols directly; the typed
// surface here is for plugin-internal use plus the rare consumer that
// needs Kraken-specific behaviour.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fixed size limits for public result structs.
// Kraken's altname surface is short (`BTCUSD`); canonical/legacy
// surface is also short (`XXBTZUSD`); the wsname is `BTC/USD`. 24 B
// accommodates all three with NUL.
#define KRAKEN_PRODUCT_ID_SZ    24
#define KRAKEN_CURRENCY_SZ       8
#define KRAKEN_ORDER_ID_SZ      64   // matches EXCHANGE_ORDER_ID_SZ
#define KRAKEN_CLIENT_OID_SZ    64   // matches EXCHANGE_CLIENT_OID_SZ
#define KRAKEN_SIDE_SZ           8   // "buy" / "sell"
#define KRAKEN_STATUS_SZ        12
#define KRAKEN_TIF_SZ            8
#define KRAKEN_TYPE_SZ          16

// Granularity values accepted by GET /0/public/OHLC (minutes).
#define KRAKEN_GRAN_1M      1
#define KRAKEN_GRAN_5M      5
#define KRAKEN_GRAN_15M    15
#define KRAKEN_GRAN_30M    30
#define KRAKEN_GRAN_1H     60
#define KRAKEN_GRAN_4H    240
#define KRAKEN_GRAN_1D   1440
#define KRAKEN_GRAN_1W  10080

// Max rows the typed wrappers will surface to the caller. Kraken's
// OHLC endpoint returns up to 720 bars per call; orders/fills/accounts
// match the generic exchange caps so translation at the vtable seam
// never truncates.
#define KRAKEN_MAX_CANDLES        720
#define KRAKEN_MAX_ORDERS_LIST    100
#define KRAKEN_MAX_FILLS_LIST     100
#define KRAKEN_MAX_ACCOUNTS        64

// AssetPairs cache capacity. Kraken lists ~700 pairs in 2026; 512 is
// a soft bound — overflow is logged + the trailing pairs are dropped.
#define KRAKEN_PAIRS_CAP          512

// Buffer for the err string on every *_result_t. 128 B is enough for
// the longest Kraken error envelope we surface today.
#define KRAKEN_ERR_SZ             128

// Public type stubs. Plugin-internal callers consume the typed
// callbacks; the exchange-vtable adapter translates these to the
// generic exchange_*_t shapes at the seam.

typedef struct
{
  int64_t  ts_open_sec;      // bucket open, seconds since epoch
  double   open;
  double   high;
  double   low;
  double   close;
  double   vwap;
  double   volume;
  uint32_t trade_count;
} kraken_candle_t;

typedef struct
{
  char     err[KRAKEN_ERR_SZ];
  uint32_t count;
  kraken_candle_t rows[KRAKEN_MAX_CANDLES];
} kraken_candles_result_t;

typedef struct
{
  char    order_id[KRAKEN_ORDER_ID_SZ];
  char    client_oid[KRAKEN_CLIENT_OID_SZ];
  char    product_id[KRAKEN_PRODUCT_ID_SZ];
  char    side[KRAKEN_SIDE_SZ];
  char    type[KRAKEN_TYPE_SZ];
  char    status[KRAKEN_STATUS_SZ];
  char    tif[KRAKEN_TIF_SZ];
  double  price;
  double  size;
  double  filled_size;
  double  executed_value;
  double  fill_fees;
  bool    post_only;
  bool    settled;
  int64_t created_at_ms;
} kraken_order_t;

typedef struct
{
  char           err[KRAKEN_ERR_SZ];
  kraken_order_t order;
} kraken_order_result_t;

typedef struct
{
  char           err[KRAKEN_ERR_SZ];
  uint32_t       count;
  kraken_order_t rows[KRAKEN_MAX_ORDERS_LIST];
} kraken_orders_result_t;

typedef struct
{
  char    currency[KRAKEN_CURRENCY_SZ];   // short form: "XBT", "USD"
  double  balance;                        // total = available + hold
  double  hold;                           // reserved by open orders
  double  available;                      // free for trading
} kraken_account_t;

typedef struct
{
  char             err[KRAKEN_ERR_SZ];
  uint32_t         count;
  kraken_account_t rows[KRAKEN_MAX_ACCOUNTS];
} kraken_balances_result_t;

typedef struct
{
  char    order_id[KRAKEN_ORDER_ID_SZ];
  char    client_oid[KRAKEN_CLIENT_OID_SZ];
  char    product_id[KRAKEN_PRODUCT_ID_SZ];
  char    side[KRAKEN_SIDE_SZ];
  int64_t trade_id;
  double  price;
  double  size;
  double  fee;
  int64_t time_ms;
} kraken_fill_t;

typedef struct
{
  char          err[KRAKEN_ERR_SZ];
  uint32_t      count;
  kraken_fill_t rows[KRAKEN_MAX_FILLS_LIST];
} kraken_fills_result_t;

// POST AddOrder request body. Caller zero-initializes, fills in the
// fields, and passes by const pointer. Validation happens client-side
// against Kraken's documented ordertype/timeinforce combinations
// before any signed request leaves the daemon.
//
// `product_id` accepts altname (`BTCUSD`), canonical (`XXBTZUSD`), or
// wsname (`BTC/USD`); the typed wrapper looks the canonical altname
// up via the assetpairs cache before signing. `validate=true` asks
// Kraken to echo the validated descriptor without committing — used
// by WM-LT-8 step 5 to verify the signing path on a real key.
typedef struct
{
  char    product_id[KRAKEN_PRODUCT_ID_SZ];
  char    side[KRAKEN_SIDE_SZ];         // "buy" / "sell"
  char    type[KRAKEN_TYPE_SZ];         // "limit" / "market"
  char    tif[KRAKEN_TIF_SZ];           // "GTC" / "IOC" / "FOK"; "" = default GTC
  double  price;                        // limit only
  double  size;                         // base-ccy amount
  double  funds;                        // unused on Kraken; surfaces an error
  bool    post_only;
  bool    validate;                     // true → AddOrder validate-only
  char    client_oid[KRAKEN_CLIENT_OID_SZ];
} kraken_place_order_req_t;

// One row in the assetpairs cache. `altname` is what most legacy
// endpoints accept; `canonical` is the older `XXBTZUSD`-style code
// some endpoints prefer; `wsname` is the slash form (`BTC/USD`) used
// over WS v2. Populated by kraken_assetpairs_refresh_async.
typedef struct
{
  char    altname[16];
  char    canonical[16];
  char    wsname[KRAKEN_PRODUCT_ID_SZ];
} kraken_pair_t;

// Asset-pairs refresh result. The cache is populated by side-effect;
// the result struct just carries the refreshed row count + an error
// string so the caller can react to a failed refresh.
typedef struct
{
  char     err[KRAKEN_ERR_SZ];
  uint32_t count;
} kraken_assetpairs_result_t;

// Callback signatures. Run on the curl-multi worker thread owned by
// the plugin — do not block.
typedef void (*kraken_done_candles_cb_t)(
    const kraken_candles_result_t *res, void *user);

typedef void (*kraken_done_order_cb_t)(
    const kraken_order_result_t *res, void *user);

typedef void (*kraken_done_orders_cb_t)(
    const kraken_orders_result_t *res, void *user);

typedef void (*kraken_done_balances_cb_t)(
    const kraken_balances_result_t *res, void *user);

typedef void (*kraken_done_fills_cb_t)(
    const kraken_fills_result_t *res, void *user);

typedef void (*kraken_done_assetpairs_cb_t)(
    const kraken_assetpairs_result_t *res, void *user);

// ------------------------------------------------------------------
// Real function declarations — visible only inside the kraken plugin
// (where KR_INTERNAL is defined). External consumers go through the
// static-inline dlsym shims defined further down.
// ------------------------------------------------------------------

#ifdef KR_INTERNAL

// Returns true iff both plugin.kraken.creds.api_key and
// plugin.kraken.creds.private_key are set (non-empty) AND
// creds.private_key base64-decodes cleanly.
bool kraken_apikey_configured(void);

// Public REST. `pair` accepts any of altname/canonical/wsname; the
// assetpairs cache translates to the form the OHLC endpoint expects.
// `interval_minutes` must be one of the KRAKEN_GRAN_* constants. The
// last (in-progress) candle is dropped from the result.
// `since_sec=0` returns the latest 720 bars.
//
// `prio` is one of EXCHANGE_PRIO_* (the abstraction's byte values
// match curl_prio_t on purpose — pass-through through the vtable).
//
// On synchronous FAIL the callback fires with `res->err` populated;
// on SUCCESS the callback fires later from the curl-multi thread.
bool kraken_fetch_candles_async(const char *pair,
    uint32_t interval_minutes, int64_t since_sec, uint8_t prio,
    kraken_done_candles_cb_t cb, void *user);

// Private REST. Every call short-circuits with a credential-error
// callback when kraken_apikey_configured() returns false. No retry
// happens here; the abstraction's classifier retries on HTTP 5xx /
// 429, but Kraken usually delivers rate-limit hits in the JSON body
// (`error: ["EAPI:Rate limit exceeded"]`) with HTTP 200 — those land
// as hard errors in `res->err`.

bool kraken_get_balance_async(kraken_done_balances_cb_t cb, void *user);

bool kraken_add_order_async(const kraken_place_order_req_t *req,
    kraken_done_order_cb_t cb, void *user);

bool kraken_cancel_order_async(const char *order_id,
    kraken_done_order_cb_t cb, void *user);

bool kraken_query_order_async(const char *order_id,
    kraken_done_order_cb_t cb, void *user);

bool kraken_open_orders_async(kraken_done_orders_cb_t cb, void *user);

bool kraken_closed_orders_async(int64_t start_sec,
    kraken_done_orders_cb_t cb, void *user);

// `order_id` and `product_id` are optional client-side filters
// (Kraken's TradesHistory endpoint does not support them server-
// side). When both are NULL/empty every fill since `start_sec` is
// surfaced.
bool kraken_trades_history_async(const char *order_id,
    const char *product_id, int64_t start_sec,
    kraken_done_fills_cb_t cb, void *user);

// Refresh the assetpairs cache via GET /0/public/AssetPairs. Safe to
// call with cb=NULL for the fire-and-forget pattern. The cache is
// populated by side-effect via kraken_pairs.c regardless of whether
// `cb` is supplied.
bool kraken_assetpairs_refresh_async(kraken_done_assetpairs_cb_t cb,
    void *user);

#endif // KR_INTERNAL

// ------------------------------------------------------------------
// dlsym shim helpers
// ------------------------------------------------------------------

#ifndef KR_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
kraken_apikey_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("kraken", "kraken_apikey_configured");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "kraken",
          "dlsym failed: kraken_apikey_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

#endif // !KR_INTERNAL

#endif // BM_KRAKEN_API_H
