#ifndef BM_COINBASE_API_H
#define BM_COINBASE_API_H

// Public mechanism API for the coinbase service plugin. Consumers
// include this header and resolve the symbols at runtime via
// plugin_dlsym("coinbase", …) — the plugin is loaded RTLD_LOCAL.
//
// The shim shape mirrors plugins/service/coinmarketcap/coinmarketcap_api.h
// and plugins/service/openweather/openweather_api.h: per-symbol atomic
// cache guard, union to launder void*↔function-pointer conversion,
// FATAL + abort on dlsym miss.
//
// Inside the coinbase plugin itself the static-inline shims below
// would collide with the real definitions, so coinbase.c defines
// CB_INTERNAL before including this header to skip them.
//
// Scaffolding: no symbols are exported yet. The type declarations below
// are the stable shapes callers will consume once CB1–CB6 (REST auth,
// products/candles, ticker/orders/accounts, WebSocket transport,
// WebSocket channel multiplexer, consumer convenience APIs) land. Sizes
// are fixed so callers can stack-allocate results without lifetime
// ambiguity.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Real symbols (added by CB2–CB6) return SUCCESS / FAIL from common.h;
// consumers of this header therefore include common.h transitively via
// the shim block once symbols land. Scaffold stays free of the include
// to keep "no unused include" diagnostics quiet until there is a shim
// to consume it.

// Fixed size limits for public result structs.

#define COINBASE_PRODUCT_ID_SZ     16   // e.g. "BTC-USD"
#define COINBASE_CURRENCY_SZ        8   // e.g. "BTC"
#define COINBASE_ORDER_ID_SZ       40   // UUID + NUL
#define COINBASE_CLIENT_OID_SZ     40
#define COINBASE_SIDE_SZ            8   // "buy" / "sell"
#define COINBASE_STATUS_SZ         12   // "open", "pending", "done", …
#define COINBASE_TIF_SZ             8   // "GTC", "GTT", "IOC", "FOK"
#define COINBASE_TYPE_SZ            8   // "limit", "market", "stop"

// Granularity values accepted by GET /products/{id}/candles.
#define COINBASE_GRAN_1M         60
#define COINBASE_GRAN_5M        300
#define COINBASE_GRAN_15M       900
#define COINBASE_GRAN_1H       3600
#define COINBASE_GRAN_6H      21600
#define COINBASE_GRAN_1D      86400

// Max rows returned per candles call (Coinbase limit).
#define COINBASE_MAX_CANDLES      300

// Max rows returned per /orders list call. Coinbase's default page size
// is 100; we do not implement cursor pagination in CB3 — callers who
// need more than the first page should filter on server side via status
// and product_id.
#define COINBASE_MAX_ORDERS_LIST  100

// Public type stubs. Bodies stay thin until CB2 lands the parser.

typedef struct
{
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  char    base_currency[COINBASE_CURRENCY_SZ];
  char    quote_currency[COINBASE_CURRENCY_SZ];
  double  base_min_size;
  double  base_max_size;
  double  quote_increment;
  double  base_increment;
  bool    trading_disabled;
  bool    cancel_only;
  bool    post_only;
  bool    limit_only;
} coinbase_product_t;

typedef struct
{
  int64_t time;        // bucket start, seconds since epoch
  double  low;
  double  high;
  double  open;
  double  close;
  double  volume;
} coinbase_candle_t;

typedef struct
{
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  double  price;
  double  bid;
  double  ask;
  double  volume_24h;
  double  low_24h;
  double  high_24h;
  int64_t time_ms;
} coinbase_ticker_t;

typedef struct
{
  char    order_id[COINBASE_ORDER_ID_SZ];
  char    client_oid[COINBASE_CLIENT_OID_SZ];
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  char    side[COINBASE_SIDE_SZ];
  char    type[COINBASE_TYPE_SZ];
  char    status[COINBASE_STATUS_SZ];
  char    tif[COINBASE_TIF_SZ];
  double  price;
  double  size;
  double  filled_size;
  double  executed_value;
  double  fill_fees;
  bool    post_only;
  bool    settled;
  int64_t created_at_ms;
} coinbase_order_t;

typedef struct
{
  char    currency[COINBASE_CURRENCY_SZ];
  double  balance;
  double  hold;
  double  available;
} coinbase_account_t;

// POST /orders request body. Caller zero-initializes, fills in the
// relevant fields, and passes by const pointer. Invalid combinations
// (e.g. post_only=true with type="market") are rejected client-side
// before a signed request is sent. client_oid may be empty; if non-
// empty it must be a UUID — Coinbase validates.
typedef struct
{
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  char    side[COINBASE_SIDE_SZ];       // "buy" or "sell"
  char    type[COINBASE_TYPE_SZ];       // "limit" or "market"
  char    tif[COINBASE_TIF_SZ];         // "GTC" / "IOC" / "FOK" / "GTT"
  double  price;   // limit only
  double  size;    // base currency amount (market-sell + limit)
  double  funds;   // market-buy quote-currency amount (optional)
  bool    post_only;
  char    client_oid[COINBASE_CLIENT_OID_SZ];
} coinbase_place_order_req_t;

// WebSocket channel identifiers. One channel = one message kind.
typedef enum
{
  COINBASE_CH_HEARTBEAT,
  COINBASE_CH_STATUS,
  COINBASE_CH_TICKER,
  COINBASE_CH_TICKER_BATCH,
  COINBASE_CH_LEVEL2,
  COINBASE_CH_LEVEL2_BATCH,
  COINBASE_CH_MATCHES,
  COINBASE_CH_FULL,
  COINBASE_CH_USER,
  COINBASE_CH__COUNT
} coinbase_ws_channel_t;

// Async-delivery event — dispatched to subscribers on the plugin's WS
// reader thread. Must be treated read-only; the payload pointer is only
// valid for the duration of the callback. Consumers that need to keep
// it must copy. Variant selected by `channel`.
//
// `gap` is true when the channel multiplexer detected a break in the
// per-(channel, product) `sequence` monotonic order. Consumers that
// care about exact state (e.g. a level2 order book) should snapshot-
// refresh when this flips; price-only consumers can ignore it.
typedef struct
{
  coinbase_ws_channel_t channel;
  const char           *product_id;  // NULL for status / server-wide
  int64_t               sequence;    // channel-specific; 0 when n/a
  bool                  gap;         // sequence jumped; payload may be stale
  const void           *payload;     // points at a typed struct below
} coinbase_ws_event_t;

// Payloads carried in `coinbase_ws_event_t.payload`, keyed by channel.
// Every payload copies strings in — pointers into the raw frame do NOT
// escape the dispatcher. `time_ms` is derived from the `time` field and
// is 0 when the string could not be parsed; callers timing on their own
// monotonic clock should ignore it.

typedef struct
{
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  double  price;
  double  best_bid;
  double  best_ask;
  double  volume_24h;
  double  low_24h;
  double  high_24h;
  int64_t sequence;
  int64_t time_ms;
} coinbase_ws_ticker_t;

typedef struct
{
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  int64_t sequence;
  int64_t last_trade_id;
  int64_t time_ms;
} coinbase_ws_heartbeat_t;

typedef struct
{
  char    product_id[COINBASE_PRODUCT_ID_SZ];
  char    side[COINBASE_SIDE_SZ];      // "buy" or "sell"
  int64_t trade_id;
  int64_t sequence;
  double  price;
  double  size;
  int64_t time_ms;
} coinbase_ws_match_t;

// Single delta inside a level2_update. Coinbase emits these as
// ["buy"|"sell", price, size]; size==0 removes the level.
typedef struct
{
  char    side[COINBASE_SIDE_SZ];
  double  price;
  double  size;
} coinbase_ws_l2change_t;

// Level2 update. `n_changes` caps at COINBASE_WS_L2_MAX_CHANGES — any
// overflow increments `n_changes_dropped` so the consumer can detect a
// book that has run away from its local view and refresh.
#define COINBASE_WS_L2_MAX_CHANGES 64

typedef struct
{
  char                    product_id[COINBASE_PRODUCT_ID_SZ];
  int64_t                 time_ms;
  uint32_t                n_changes;
  uint32_t                n_changes_dropped;
  coinbase_ws_l2change_t  changes[COINBASE_WS_L2_MAX_CHANGES];
} coinbase_ws_l2update_t;

// Status broadcast. The full catalogue is too large to propagate — we
// surface only the event-level metadata; consumers that need the list of
// products / currencies should issue a REST fetch triggered off this
// event.
typedef struct
{
  int64_t time_ms;
} coinbase_ws_status_t;

// WebSocket event callback. Invoked on the plugin's WS reader — fast,
// non-blocking, no recursive calls back into coinbase_ws_subscribe().
typedef void (*coinbase_ws_event_cb_t)(const coinbase_ws_event_t *ev,
    void *user);

// Opaque subscription handle (CB5). Returned by coinbase_ws_subscribe;
// consumers pass it back to coinbase_ws_unsubscribe to tear the slot
// down. The struct is defined inside the coinbase plugin.
typedef struct coinbase_ws_sub coinbase_ws_sub_t;

// Async REST result payloads. Pattern mirrors coinmarketcap_*_result_t.

typedef struct
{
  char    err[128];
  // On success: consumer reads via coinbase_get_products().
} coinbase_products_result_t;

typedef struct
{
  char               err[128];
  uint32_t           count;       // number of valid rows in `rows`
  coinbase_candle_t  rows[COINBASE_MAX_CANDLES];
} coinbase_candles_result_t;

typedef struct
{
  char               err[128];
  coinbase_ticker_t  ticker;
} coinbase_ticker_result_t;

typedef struct
{
  char               err[128];
  coinbase_order_t   order;
} coinbase_order_result_t;

typedef struct
{
  char                err[128];
  uint32_t            count;
  coinbase_order_t    rows[COINBASE_MAX_ORDERS_LIST];
} coinbase_orders_result_t;

typedef struct
{
  char                err[128];
  uint32_t            count;
  coinbase_account_t  rows[64];
} coinbase_accounts_result_t;

// Callback signatures. Callbacks run on the curl-multi worker thread
// owned by the plugin — do not block.
typedef void (*coinbase_done_products_cb_t)(
    const coinbase_products_result_t *res, void *user);

typedef void (*coinbase_done_candles_cb_t)(
    const coinbase_candles_result_t *res, void *user);

typedef void (*coinbase_done_ticker_cb_t)(
    const coinbase_ticker_result_t *res, void *user);

typedef void (*coinbase_done_order_cb_t)(
    const coinbase_order_result_t *res, void *user);

typedef void (*coinbase_done_orders_cb_t)(
    const coinbase_orders_result_t *res, void *user);

typedef void (*coinbase_done_accounts_cb_t)(
    const coinbase_accounts_result_t *res, void *user);

// ------------------------------------------------------------------
// Real function declarations — visible only inside the coinbase plugin
// (where CB_INTERNAL is defined). External consumers go through the
// static-inline dlsym shims defined further down. No symbols are
// exported yet; declarations land chunk-by-chunk in CB2–CB6.
// ------------------------------------------------------------------

#ifdef CB_INTERNAL

// Returns true iff apikey, apisecret, and passphrase are all set.
// Public probe consumers use to decide whether private REST endpoints
// and authenticated WebSocket channels are reachable.
bool coinbase_apikey_configured(void);

// Refresh the in-memory product list from GET /products. On success
// res->err is empty and the cache is readable via coinbase_get_products.
// On FAIL the callback is NOT invoked.
bool coinbase_fetch_products_async(coinbase_done_products_cb_t cb,
    void *user);

// Fetch historical candles. `granularity` seconds; `start_ts`/`end_ts`
// in seconds since epoch (0/0 = server default range ending now). The
// 300-bucket cap is enforced client-side — violating ranges fail before
// a request is submitted. Returns FAIL if product_id is invalid, the
// request could not be queued, or the bucket budget is exceeded; on FAIL
// the callback is invoked with a descriptive res->err.
bool coinbase_fetch_candles_async(const char *product_id,
    int32_t granularity, int64_t start_ts, int64_t end_ts,
    coinbase_done_candles_cb_t cb, void *user);

// Fetch the latest ticker snapshot for one product.
bool coinbase_fetch_ticker_async(const char *product_id,
    coinbase_done_ticker_cb_t cb, void *user);

// Bulk cache read. On SUCCESS copies min(cache size, out_cap) rows into
// out_arr and writes the count to *out_count. FAIL when the cache is
// empty (no fetch has landed yet).
bool coinbase_get_products(coinbase_product_t *out_arr, uint32_t out_cap,
    uint32_t *out_count);

// Returns true when the product cache is populated and within the
// configured plugin.coinbase.cache_ttl window.
bool coinbase_products_cache_fresh(void);

// Authenticated endpoints. All five fail fast when
// `cb_apikey_configured()` is false — the callback fires with a
// descriptive err; no HTTP request is sent.

// POST /orders. `req` is copied into the signed body; caller may free
// after the call returns. On success `res->order` is populated with the
// server-assigned order_id + echoed fields.
bool coinbase_place_order_async(const coinbase_place_order_req_t *req,
    coinbase_done_order_cb_t cb, void *user);

// DELETE /orders/{id}. On success `res->order.order_id` echoes the id
// that Coinbase acknowledged; no other fields are populated (the cancel
// ack is a bare string, not an order object).
bool coinbase_cancel_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user);

// GET /orders/{id}. On success `res->order` carries the full server-
// side view (status, filled_size, executed_value, etc.).
bool coinbase_get_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user);

// GET /orders?status=…&product_id=…&limit=100. Either filter may be
// NULL / empty to omit. Returns up to COINBASE_MAX_ORDERS_LIST rows.
bool coinbase_list_orders_async(const char *status, const char *product_id,
    coinbase_done_orders_cb_t cb, void *user);

// GET /accounts. Populates `res->rows` with the caller's per-currency
// balance / hold / available.
bool coinbase_get_accounts_async(coinbase_done_accounts_cb_t cb,
    void *user);

// Subscribe to one or more WebSocket channels on one or more products.
// The channel multiplexer dedups overlapping subscriptions across
// callers: two consumers watching `ticker` on `BTC-USD` share a single
// upstream slot, and a slot is torn down upstream only when the last
// local subscriber drops it. `heartbeat` is implicitly added to the
// upstream set whenever any subscriber is live, so plugin-local
// liveness accounting works regardless of what the caller asked for.
//
// On SUCCESS the callback fires on the plugin's WS reader thread for
// every matching event until coinbase_ws_unsubscribe(). Returns NULL on
// FAIL: cb is NULL, channels[] or product_ids[] is empty when the
// channel set requires products (e.g. ticker / matches / level2), the
// internal capacity is exhausted, or the set includes an authenticated
// channel (`user`, `full`) while credentials are not configured. No
// upstream frame is sent on FAIL.
//
// Safe to call before the WS session is open — subscriptions land in
// the registry and are sent the moment the session reaches OPEN. On
// reconnect, live subscriptions are resent transparently; consumer
// callbacks never fire for the resubscribe ack.
coinbase_ws_sub_t *coinbase_ws_subscribe(
    const coinbase_ws_channel_t *channels, size_t n_channels,
    const char *const *product_ids, size_t n_products,
    coinbase_ws_event_cb_t cb, void *user);

// Release a subscription handle. Decrements refcounts on every slot the
// subscription claimed; slots that reach zero are unsubscribed upstream
// in a single batched frame. No-op on NULL. After this call the handle
// is invalid.
void coinbase_ws_unsubscribe(coinbase_ws_sub_t *sub);

#endif // CB_INTERNAL

// ------------------------------------------------------------------
// dlsym shim helpers
// ------------------------------------------------------------------

#ifndef CB_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
coinbase_apikey_configured(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_apikey_configured");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_apikey_configured");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
coinbase_fetch_products_async(coinbase_done_products_cb_t cb, void *user)
{
  typedef bool (*fn_t)(coinbase_done_products_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_fetch_products_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_fetch_products_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(cb, user));
}

static inline bool
coinbase_fetch_candles_async(const char *product_id, int32_t granularity,
    int64_t start_ts, int64_t end_ts,
    coinbase_done_candles_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, int32_t, int64_t, int64_t,
      coinbase_done_candles_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_fetch_candles_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_fetch_candles_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(product_id, granularity, start_ts, end_ts, cb, user));
}

static inline bool
coinbase_fetch_ticker_async(const char *product_id,
    coinbase_done_ticker_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, coinbase_done_ticker_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_fetch_ticker_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_fetch_ticker_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(product_id, cb, user));
}

static inline bool
coinbase_get_products(coinbase_product_t *out_arr, uint32_t out_cap,
    uint32_t *out_count)
{
  typedef bool (*fn_t)(coinbase_product_t *, uint32_t, uint32_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_get_products");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_get_products");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(out_arr, out_cap, out_count));
}

static inline bool
coinbase_products_cache_fresh(void)
{
  typedef bool (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_products_cache_fresh");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_products_cache_fresh");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

static inline bool
coinbase_place_order_async(const coinbase_place_order_req_t *req,
    coinbase_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const coinbase_place_order_req_t *,
      coinbase_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_place_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_place_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(req, cb, user));
}

static inline bool
coinbase_cancel_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, coinbase_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_cancel_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_cancel_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(order_id, cb, user));
}

static inline bool
coinbase_get_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, coinbase_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_get_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_get_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(order_id, cb, user));
}

static inline bool
coinbase_list_orders_async(const char *status, const char *product_id,
    coinbase_done_orders_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *,
      coinbase_done_orders_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_list_orders_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_list_orders_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(status, product_id, cb, user));
}

static inline bool
coinbase_get_accounts_async(coinbase_done_accounts_cb_t cb, void *user)
{
  typedef bool (*fn_t)(coinbase_done_accounts_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_get_accounts_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_get_accounts_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(cb, user));
}

static inline coinbase_ws_sub_t *
coinbase_ws_subscribe(const coinbase_ws_channel_t *channels,
    size_t n_channels, const char *const *product_ids, size_t n_products,
    coinbase_ws_event_cb_t cb, void *user)
{
  typedef coinbase_ws_sub_t *(*fn_t)(const coinbase_ws_channel_t *, size_t,
      const char *const *, size_t, coinbase_ws_event_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_ws_subscribe");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_ws_subscribe");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(channels, n_channels, product_ids, n_products, cb, user));
}

static inline void
coinbase_ws_unsubscribe(coinbase_ws_sub_t *sub)
{
  typedef void (*fn_t)(coinbase_ws_sub_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("coinbase", "coinbase_ws_unsubscribe");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "coinbase",
          "dlsym failed: coinbase_ws_unsubscribe");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(sub);
}

#endif // !CB_INTERNAL

#endif // BM_COINBASE_API_H
