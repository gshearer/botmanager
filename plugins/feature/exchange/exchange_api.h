#ifndef BM_EXCHANGE_API_H
#define BM_EXCHANGE_API_H

// Public mechanism API for the feature_exchange plugin.
//
// `exchange_request()` is the single entry point for routing a REST
// operation to a registered exchange-protocol plugin (coinbase, kraken,
// …). The abstraction owns:
//
//   * priority queueing (P0 transactional, P50 backfill, P254 user
//     download — see EXCHANGE_PRIO_* constants),
//   * a per-exchange token bucket sized from the protocol plugin's
//     advertised rps (minus a small headroom),
//   * reserved-slot policy so high-priority traffic cannot be drowned
//     by lower-priority backfill (default reserved[0]=1),
//   * 429 / 5xx retry with exponential backoff (250 → 4000 ms cap, 5
//     attempts).
//
// Exchange-protocol plugins implement `exchange_protocol_vtable_t` and
// self-register at init via `exchange_register()`. Consumers (whenmoon,
// future strategy engine) only ever call the four public functions
// declared below.
//
// Shim shape mirrors plugins/service/coinbase/coinbase_api.h: per-
// symbol atomic cache guard, union to launder void*↔function-pointer
// conversion, FATAL + abort on a dlsym miss (which implies a broken
// plugin-dependency graph).
//
// The exchange plugin's own translation units define EXCHANGE_INTERNAL
// before including this header so the static-inline shims below are
// skipped (they would collide with the real definitions).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Priority tiers carried on every request. Lower numeric value = higher
// priority. The token bucket reserves slots for P0 traffic so a flood of
// P254 backfill cannot starve a transactional buy/sell. Reservation
// counts are KV-tunable per exchange.
#define EXCHANGE_PRIO_TRANSACTIONAL    0
#define EXCHANGE_PRIO_MARKET_BACKFILL  50
#define EXCHANGE_PRIO_USER_DOWNLOAD  254

// Op-kind classifier carried into the protocol plugin's `build_request`
// vtable hook. The protocol plugin uses it to pick public vs. signed
// HTTP and the matching curl method.
typedef enum
{
  EXCHANGE_OP_REST_GET,
  EXCHANGE_OP_REST_POST,
  EXCHANGE_OP_REST_DELETE,
  EXCHANGE_OP_PRIVATE_REST_GET,
  EXCHANGE_OP_PRIVATE_REST_POST,
  EXCHANGE_OP_PRIVATE_REST_DELETE
} exchange_op_kind_t;

// Response delivered to the caller after retry / classification.
//
//   http_status — HTTP status code on transport success (>= 0); 0 when
//                 the request never reached the server (transport error
//                 or hard pre-flight failure).
//   body        — response body bytes (UTF-8 in practice); pointer is
//                 only valid for the duration of the callback.
//   body_len    — bytes addressable through `body`. Zero when the
//                 protocol plugin returned no body or on hard error.
//   err         — non-NULL human-readable description on failure (final
//                 retry exhausted, hard pre-flight refusal, …); NULL
//                 on success.
//   user        — verbatim user pointer the caller passed to
//                 exchange_request().
//
// Invoked on the curl worker thread that completed the underlying
// transport. Consumers must not block.
typedef void (*exchange_response_cb_t)(int http_status,
    const char *body, size_t body_len,
    const char *err, void *user);

// ------------------------------------------------------------------ //
// Generic semantic types (WM-OR-1).                                    //
//                                                                      //
// These mirror the protocol-specific types each exchange plugin uses   //
// internally (e.g. coinbase_order_t in coinbase_api.h) but live here   //
// so consumers of the abstraction never pull in protocol headers.     //
// Sizes are deliberately at-or-above every protocol equivalent so a   //
// memcpy at the seam never truncates.                                  //
// ------------------------------------------------------------------ //

#define EXCHANGE_NAME_SZ           32   // matches exchange_t.name[32]
#define EXCHANGE_PRODUCT_ID_SZ     24   // e.g. "BTC-USD" / "1000PEPE-USDC"
#define EXCHANGE_CURRENCY_SZ       16
#define EXCHANGE_ORDER_ID_SZ       64
#define EXCHANGE_CLIENT_OID_SZ     64
#define EXCHANGE_SIDE_SZ            8   // "buy" / "sell"
#define EXCHANGE_TYPE_SZ           16   // "limit" / "market" / "stop" / ...
#define EXCHANGE_STATUS_SZ         16
#define EXCHANGE_TIF_SZ             8   // "GTC" / "GTT" / "IOC" / "FOK"
#define EXCHANGE_ERR_SZ           128

#define EXCHANGE_MAX_ORDERS_LIST  100
#define EXCHANGE_MAX_FILLS_LIST   100
#define EXCHANGE_MAX_ACCOUNTS      64

// KR-2: candle page cap. Sized at 720 to fit Kraken's largest single-page
// reply; Coinbase's 300-row cap fits comfortably underneath. Whenmoon's
// downloader window (WM_DL_CANDLE_WINDOW_BUCKETS = 300) is still the
// constraint that bounds a single dispatch.
#define EXCHANGE_MAX_CANDLES      720

// Capability snapshot returned by exchange_get_capabilities. Reflects
// the protocol vtable's current view: `has_credentials` consults the
// optional `is_authenticated` hook (true when hook is NULL — public-
// only exchanges advertise as "authed" by absence so the auth probe is
// not their gate; auth-gated verbs FAIL on the hook layer instead).
// `sandbox` is false when `is_sandbox` hook is NULL.
typedef struct
{
  char     name[EXCHANGE_NAME_SZ];
  bool     has_credentials;
  bool     sandbox;
  uint32_t advertised_rps;
  uint32_t advertised_burst;
} exchange_capabilities_t;

// Generic order row. Superset of every supported protocol's order
// shape; protocol plugins memcpy/snprintf into these fields at the
// seam. Empty strings + zeroed numerics are valid for fields a given
// protocol doesn't populate (e.g. `executed_value` is coinbase-only).
typedef struct
{
  char    order_id[EXCHANGE_ORDER_ID_SZ];
  char    client_oid[EXCHANGE_CLIENT_OID_SZ];
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  char    side[EXCHANGE_SIDE_SZ];
  char    type[EXCHANGE_TYPE_SZ];
  char    status[EXCHANGE_STATUS_SZ];
  char    tif[EXCHANGE_TIF_SZ];
  double  price;
  double  size;
  double  filled_size;
  double  executed_value;
  double  fill_fees;
  bool    post_only;
  bool    settled;
  int64_t created_at_ms;
} exchange_order_t;

// Generic per-currency balance row.
typedef struct
{
  char    currency[EXCHANGE_CURRENCY_SZ];
  double  balance;
  double  hold;
  double  available;
} exchange_account_t;

// One executed fill — used by the safety-net poll path that the live
// engine eventually moves off direct coinbase_list_fills_async calls.
// `side` lowercased to match the WS user-channel convention.
typedef struct
{
  char    order_id[EXCHANGE_ORDER_ID_SZ];
  char    client_oid[EXCHANGE_CLIENT_OID_SZ];
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  char    side[EXCHANGE_SIDE_SZ];
  int64_t trade_id;
  double  price;
  double  size;
  double  fee;
  int64_t time_ms;
} exchange_fill_t;

// Place-order request body. Caller zero-initialises and fills only the
// fields relevant to the chosen `type`. `client_oid` may be empty —
// the protocol plugin then mints a UUID before the wire request.
// Invalid combinations (e.g. post_only=true with type="market") are
// rejected protocol-side before a signed request leaves.
typedef struct
{
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  char    side[EXCHANGE_SIDE_SZ];        // "buy" or "sell"
  char    type[EXCHANGE_TYPE_SZ];        // "limit" or "market"
  char    tif[EXCHANGE_TIF_SZ];          // "GTC" / "IOC" / ...; empty for market
  double  price;                         // limit only
  double  size;                          // base-ccy amount (limit + market-sell)
  double  funds;                         // optional quote-ccy (market-buy)
  bool    post_only;
  char    client_oid[EXCHANGE_CLIENT_OID_SZ];
} exchange_place_order_req_t;

// Async result payloads. Mirror the coinbase_*_result_t pattern: an
// `err` string populated only on failure (empty on success), plus the
// typed payload. Callbacks see a const pointer; lifetime is the
// callback only — copy out anything that needs to persist.

typedef struct
{
  char              err[EXCHANGE_ERR_SZ];
  exchange_order_t  order;
} exchange_order_result_t;

typedef struct
{
  char              err[EXCHANGE_ERR_SZ];
  uint32_t          count;
  exchange_order_t  rows[EXCHANGE_MAX_ORDERS_LIST];
} exchange_orders_result_t;

typedef struct
{
  char                err[EXCHANGE_ERR_SZ];
  uint32_t            count;
  exchange_account_t  rows[EXCHANGE_MAX_ACCOUNTS];
} exchange_accounts_result_t;

typedef struct
{
  char             err[EXCHANGE_ERR_SZ];
  uint32_t         count;
  exchange_fill_t  rows[EXCHANGE_MAX_FILLS_LIST];
} exchange_fills_result_t;

// ------------------------------------------------------------------ //
// Candle types (KR-2).                                                //
//                                                                      //
// Granularity is a neutral seconds count; protocol adapters map to    //
// their native enum. `ts_open_ms` is the bucket open timestamp in     //
// milliseconds since epoch (uniform with the rest of the abstraction; //
// coinbase's seconds-since-epoch is widened on translation).          //
// ------------------------------------------------------------------ //

typedef enum
{
  EXCH_GRAN_1M  = 60,
  EXCH_GRAN_5M  = 300,
  EXCH_GRAN_15M = 900,
  EXCH_GRAN_30M = 1800,
  EXCH_GRAN_1H  = 3600,
  EXCH_GRAN_4H  = 14400,
  EXCH_GRAN_1D  = 86400,
  EXCH_GRAN_1W  = 604800
} exchange_granularity_t;

typedef struct
{
  int64_t ts_open_ms;   // bucket open, ms since epoch
  double  open;
  double  high;
  double  low;
  double  close;
  double  volume;
} exchange_candle_t;

typedef struct
{
  char               err[EXCHANGE_ERR_SZ];
  uint32_t           count;
  exchange_candle_t  rows[EXCHANGE_MAX_CANDLES];
} exchange_candles_result_t;

// ------------------------------------------------------------------ //
// WebSocket types (KR-2).                                             //
//                                                                      //
// Channel enum is the neutral surface; protocol plugins map to native //
// channel names. Only the channels whenmoon consumes today have       //
// payload types defined — book/ohlc forward-look the Kraken work but  //
// are not exposed yet.                                                 //
// ------------------------------------------------------------------ //

typedef enum
{
  EXCH_WS_TICKER,
  EXCH_WS_TRADES,
  EXCH_WS_BOOK_L2,
  EXCH_WS_OHLC_1M,
  EXCH_WS_USER
} exchange_ws_channel_t;

typedef struct
{
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  double  price;
  double  best_bid;
  double  best_ask;
  double  volume_24h;
  double  low_24h;
  double  high_24h;
  int64_t time_ms;
} exchange_ws_ticker_t;

typedef struct
{
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  char    side[EXCHANGE_SIDE_SZ];      // "buy" / "sell"
  int64_t trade_id;
  double  price;
  double  size;
  int64_t time_ms;
} exchange_ws_match_t;

// Authenticated user-channel sub-payloads. Discriminated by
// exchange_ws_user_event_t::kind below.
typedef enum
{
  EXCH_WS_USER_KIND_ORDER = 0,
  EXCH_WS_USER_KIND_FILL  = 1
} exchange_ws_user_kind_t;

typedef struct
{
  char    order_id[EXCHANGE_ORDER_ID_SZ];
  char    client_order_id[EXCHANGE_CLIENT_OID_SZ];
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  char    side[EXCHANGE_SIDE_SZ];          // "buy" / "sell"
  char    status[EXCHANGE_STATUS_SZ];      // OPEN/FILLED/CANCELLED/EXPIRED/FAILED
  double  limit_price;
  double  cumulative_quantity;
  double  leaves_quantity;
  double  avg_price;
  double  total_fees;
  int64_t creation_time_ms;
  int64_t time_ms;                         // envelope timestamp
} exchange_ws_user_order_t;

typedef struct
{
  char    order_id[EXCHANGE_ORDER_ID_SZ];
  char    client_order_id[EXCHANGE_CLIENT_OID_SZ];
  char    product_id[EXCHANGE_PRODUCT_ID_SZ];
  char    side[EXCHANGE_SIDE_SZ];          // "buy" / "sell"
  int64_t trade_id;
  double  price;
  double  size;
  double  fee;
  int64_t time_ms;
} exchange_ws_user_fill_t;

typedef struct
{
  exchange_ws_user_kind_t kind;
  union
  {
    exchange_ws_user_order_t order;
    exchange_ws_user_fill_t  fill;
  } u;
} exchange_ws_user_event_t;

// Wrapping fanout event. `channel` discriminates the payload union;
// `product_id` mirrors the inner payload's product where applicable so
// fanout subscribers can switch on the wrapper alone.
typedef struct
{
  exchange_ws_channel_t channel;
  char                  product_id[EXCHANGE_PRODUCT_ID_SZ];
  union
  {
    exchange_ws_ticker_t      ticker;     // EXCH_WS_TICKER
    exchange_ws_match_t       match;      // EXCH_WS_TRADES
    exchange_ws_user_event_t  user;       // EXCH_WS_USER
  } payload;
} exchange_ws_event_t;

// Opaque subscription handle. The protocol plugin defines the concrete
// struct internally; the abstraction layer treats it as void*.
typedef struct exchange_ws_sub exchange_ws_sub_t;

// Callback signatures. Same threading rules as
// exchange_response_cb_t — fired on the curl-multi worker thread that
// completed the underlying transport. Consumers must not block.
typedef void (*exchange_done_order_cb_t)(
    const exchange_order_result_t *res, void *user);
typedef void (*exchange_done_orders_cb_t)(
    const exchange_orders_result_t *res, void *user);
typedef void (*exchange_done_accounts_cb_t)(
    const exchange_accounts_result_t *res, void *user);
typedef void (*exchange_done_fills_cb_t)(
    const exchange_fills_result_t *res, void *user);
typedef void (*exchange_done_candles_cb_t)(
    const exchange_candles_result_t *res, void *user);

// WebSocket fanout callback. Fires on the protocol plugin's WS reader
// thread; treat payload pointers as valid only for the call duration.
typedef void (*exchange_ws_event_cb_t)(const exchange_ws_event_t *ev,
    void *user);

// Per-exchange protocol vtable.
//
// `build_request` prepares an opaque protocol-specific request handle
// from the (kind, path, body) triple. The abstraction owns the handle
// for the lifetime of the request and frees it via `free_request` after
// completion (success, exhausted retry, or hard transport refusal).
//
// `submit` hands the prepared handle to the underlying transport. The
// protocol plugin's curl completion routes back into the abstraction
// via the `cb`/`user` pair the abstraction supplies — never via the
// caller's typed callback. The abstraction then classifies the status,
// applies retry policy, and finally invokes the caller's
// exchange_response_cb_t with the raw body.
//
// `prio` is the priority byte from exchange_request(); the protocol
// plugin forwards it to the underlying transport so curl-level priority
// matches exchange-level priority (CURL-PRIO-3). The byte values match
// CURL_PRIO_* by design.
//
// `advertised_rps` / `advertised_burst` are static knobs read once at
// `exchange_register()` time. The abstraction sizes its token bucket
// from these (with a small headroom subtracted from `rps`).
//
// **Capability hooks (WM-OR-1)** sit at the bottom of the struct.
// All are optional — leaving any NULL marks the corresponding verb as
// unsupported, and the public `exchange_*_async` shim FAILs early
// without invoking the typed callback. Public-only exchanges (no API
// keys configured) populate `is_authenticated` returning false; the
// public shim then FAILs with `error: <name>: api keys not configured`
// so the caller sees a stable string regardless of which hook is the
// gate. Hook callbacks fire on the protocol plugin's curl-multi
// worker thread — same threading rules as the existing typed
// wrappers.
typedef struct
{
  bool (*build_request)(exchange_op_kind_t kind,
      const char *path, const char *body_json,
      void **out_handle);

  bool (*submit)(void *handle, uint8_t prio,
      exchange_response_cb_t cb, void *user);

  void (*free_request)(void *handle);

  uint32_t advertised_rps;
  uint32_t advertised_burst;

  // Capability hooks. NULL = unsupported by this exchange.
  bool   (*is_authenticated)(void);
  bool   (*is_sandbox)(void);

  // Async — mirror the protocol's typed wrappers. Fill error reasons
  // into the result's `err` field; never block the caller.
  bool   (*place_order_async)(const exchange_place_order_req_t *req,
                              exchange_done_order_cb_t cb, void *u);
  bool   (*cancel_order_async)(const char *order_id,
                               exchange_done_order_cb_t cb, void *u);
  bool   (*get_order_async)(const char *order_id,
                            exchange_done_order_cb_t cb, void *u);
  bool   (*list_orders_async)(const char *status,
                              const char *product_id,
                              exchange_done_orders_cb_t cb, void *u);
  bool   (*list_fills_async)(const char *order_id,
                             const char *product_id,
                             int64_t start_ms,
                             exchange_done_fills_cb_t cb, void *u);
  bool   (*get_accounts_async)(exchange_done_accounts_cb_t cb,
                               void *u);

  // KR-2 capability hooks. Candles are public market data; WS subscribe
  // is gated per-channel by the protocol plugin (e.g. user channel
  // requires credentials). Both slots may be NULL — the public shim
  // FAILs with a stable error in that case.
  bool   (*fetch_candles_async)(const char *product_id,
                                exchange_granularity_t gran,
                                int64_t since_ms, int64_t until_ms,
                                exchange_done_candles_cb_t cb, void *u);
  bool   (*ws_subscribe)(const exchange_ws_channel_t *channels,
                         uint32_t n_channels,
                         const char *const *product_ids,
                         uint32_t n_products,
                         exchange_ws_event_cb_t cb, void *u,
                         exchange_ws_sub_t **out_handle);
  void   (*ws_unsubscribe)(exchange_ws_sub_t *handle);
} exchange_protocol_vtable_t;

// ------------------------------------------------------------------
// Real function declarations — visible only inside the exchange
// plugin (where EXCHANGE_INTERNAL is defined). External consumers go
// through the static-inline dlsym shims defined further down. Pattern
// matches plugins/service/coinbase/coinbase_api.h.
// ------------------------------------------------------------------

#ifdef EXCHANGE_INTERNAL

// Submit a request to the named exchange. Returns FAIL when the
// exchange is unknown, the abstraction is shutting down, or the
// internal queue could not accept the request; the callback is NOT
// invoked on FAIL. Returns SUCCESS once the request has been queued —
// the callback fires asynchronously on the curl worker thread.
//
// `path` and `body_json` are copied into the abstraction's internal
// buffers; callers may free both as soon as the call returns. `body_json`
// may be NULL for GET / DELETE.
bool exchange_request(const char *exchange, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body_json,
    exchange_response_cb_t cb, void *user);

// Register an exchange-protocol implementation. Called from the
// protocol plugin's `init()` (e.g. coinbase_init) once. The vtable
// pointer must outlive the abstraction (typical: const file-scope).
// Returns FAIL when `name` is empty or already registered.
bool exchange_register(const char *name,
    const exchange_protocol_vtable_t *vt);

// Tear down a registered exchange. Pending requests for `name` are
// surfaced as failures to their callbacks. Safe to call from the
// protocol plugin's `deinit()`.
void exchange_unregister(const char *name);

// ------------------------------------------------------------------ //
// Capability surface (WM-OR-1).                                        //
//                                                                      //
// All `exchange_*_async` capability shims dispatch to the protocol     //
// vtable's matching hook. FAIL when:                                   //
//   * `name` is NULL/empty/unknown,                                    //
//   * the matching vtable hook is NULL,                                //
//   * the auth hook is non-NULL and reports false, AND the verb is    //
//     auth-gated (every order verb + accounts + fills).               //
// On FAIL the typed callback is invoked synchronously with a          //
// populated `err` string; the function then returns FAIL. SUCCESS     //
// means the request was queued and the typed callback fires later.    //
// ------------------------------------------------------------------ //

// Snapshot of capabilities for one named exchange. FAIL when name is
// unknown.
bool exchange_get_capabilities(const char *name,
    exchange_capabilities_t *out);

// Snapshot of every registered exchange name. `out_arr` is an array
// of EXCHANGE_NAME_SZ-byte buffers; up to `out_cap` rows are written
// and the total registered count is written to `*out_count` (so the
// caller can detect truncation when count > cap). Always returns
// SUCCESS unless out_arr/out_count is NULL.
bool exchange_name_list(char (*out_arr)[EXCHANGE_NAME_SZ],
    uint32_t out_cap, uint32_t *out_count);

bool exchange_place_order_async(const char *name,
    const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user);

bool exchange_cancel_order_async(const char *name,
    const char *order_id,
    exchange_done_order_cb_t cb, void *user);

bool exchange_get_order_async(const char *name,
    const char *order_id,
    exchange_done_order_cb_t cb, void *user);

bool exchange_list_orders_async(const char *name,
    const char *status, const char *product_id,
    exchange_done_orders_cb_t cb, void *user);

bool exchange_list_fills_async(const char *name,
    const char *order_id, const char *product_id, int64_t start_ms,
    exchange_done_fills_cb_t cb, void *user);

bool exchange_get_accounts_async(const char *name,
    exchange_done_accounts_cb_t cb, void *user);

// KR-2: candle fetch — public market data; no auth gate. On pre-flight
// FAIL (unknown exchange, missing vtable hook, unsupported granularity),
// the typed callback fires synchronously with `err` populated and the
// function returns FAIL. Otherwise SUCCESS means the request was queued
// and the callback will fire asynchronously.
bool exchange_fetch_candles_async(const char *name, const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user);

// KR-2: WS subscribe. The protocol plugin is responsible for per-channel
// auth gating (user channel requires creds; ticker/trades are public).
// On FAIL `*out_handle` is NULL and the function returns FAIL — the
// fanout callback never fires. On SUCCESS the handle is non-NULL and
// every matching event fires `cb` until exchange_ws_unsubscribe(handle).
bool exchange_ws_subscribe(const char *name,
    const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user,
    exchange_ws_sub_t **out_handle);

// Release a subscription handle. No-op on NULL. Tolerates "plugin
// already unregistered" — outstanding handles may be invalidated by
// exchange_unregister, in which case this call simply returns.
void exchange_ws_unsubscribe(const char *name, exchange_ws_sub_t *handle);

#endif // EXCHANGE_INTERNAL

#ifndef EXCHANGE_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline bool
exchange_request(const char *exchange, uint8_t prio,
    exchange_op_kind_t kind, const char *path, const char *body_json,
    exchange_response_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, uint8_t, exchange_op_kind_t,
      const char *, const char *, exchange_response_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_request");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_request");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(exchange, prio, kind, path, body_json, cb, user));
}

static inline bool
exchange_register(const char *name, const exchange_protocol_vtable_t *vt)
{
  typedef bool (*fn_t)(const char *, const exchange_protocol_vtable_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_register");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_register");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, vt));
}

static inline void
exchange_unregister(const char *name)
{
  typedef void (*fn_t)(const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_unregister");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_unregister");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(name);
}

static inline bool
exchange_get_capabilities(const char *name, exchange_capabilities_t *out)
{
  typedef bool (*fn_t)(const char *, exchange_capabilities_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_get_capabilities");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_get_capabilities");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, out));
}

static inline bool
exchange_name_list(char (*out_arr)[EXCHANGE_NAME_SZ], uint32_t out_cap,
    uint32_t *out_count)
{
  typedef bool (*fn_t)(char (*)[EXCHANGE_NAME_SZ], uint32_t, uint32_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_name_list");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_name_list");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(out_arr, out_cap, out_count));
}

static inline bool
exchange_place_order_async(const char *name,
    const exchange_place_order_req_t *req,
    exchange_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const exchange_place_order_req_t *,
      exchange_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_place_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_place_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, req, cb, user));
}

static inline bool
exchange_cancel_order_async(const char *name, const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *,
      exchange_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_cancel_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_cancel_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, order_id, cb, user));
}

static inline bool
exchange_get_order_async(const char *name, const char *order_id,
    exchange_done_order_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *,
      exchange_done_order_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_get_order_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_get_order_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, order_id, cb, user));
}

static inline bool
exchange_list_orders_async(const char *name, const char *status,
    const char *product_id,
    exchange_done_orders_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *, const char *,
      exchange_done_orders_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_list_orders_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_list_orders_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, status, product_id, cb, user));
}

static inline bool
exchange_list_fills_async(const char *name, const char *order_id,
    const char *product_id, int64_t start_ms,
    exchange_done_fills_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *, const char *,
      int64_t, exchange_done_fills_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_list_fills_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_list_fills_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, order_id, product_id, start_ms, cb, user));
}

static inline bool
exchange_get_accounts_async(const char *name,
    exchange_done_accounts_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, exchange_done_accounts_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_get_accounts_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_get_accounts_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, cb, user));
}

static inline bool
exchange_fetch_candles_async(const char *name, const char *product_id,
    exchange_granularity_t gran, int64_t since_ms, int64_t until_ms,
    exchange_done_candles_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *, exchange_granularity_t,
      int64_t, int64_t, exchange_done_candles_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_fetch_candles_async");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_fetch_candles_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, product_id, gran, since_ms, until_ms, cb, user));
}

static inline bool
exchange_ws_subscribe(const char *name,
    const exchange_ws_channel_t *channels, uint32_t n_channels,
    const char *const *product_ids, uint32_t n_products,
    exchange_ws_event_cb_t cb, void *user,
    exchange_ws_sub_t **out_handle)
{
  typedef bool (*fn_t)(const char *, const exchange_ws_channel_t *,
      uint32_t, const char *const *, uint32_t,
      exchange_ws_event_cb_t, void *, exchange_ws_sub_t **);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_ws_subscribe");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_ws_subscribe");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, channels, n_channels, product_ids, n_products,
      cb, user, out_handle));
}

static inline void
exchange_ws_unsubscribe(const char *name, exchange_ws_sub_t *handle)
{
  typedef void (*fn_t)(const char *, exchange_ws_sub_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("exchange", "exchange_ws_unsubscribe");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "exchange",
          "dlsym failed: exchange_ws_unsubscribe");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(name, handle);
}

#endif // !EXCHANGE_INTERNAL

#endif // BM_EXCHANGE_API_H
