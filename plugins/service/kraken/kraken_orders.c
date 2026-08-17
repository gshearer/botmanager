// botmanager — MIT
// Kraken Spot REST: typed wrappers over the kr_submit_* primitives.
//
// Public:   kraken_fetch_candles_async (routes through exchange_request
//                                       so the abstraction's token-bucket
//                                       + retry policy applies),
//           kraken_assetpairs_refresh_async (also via exchange_request).
//
// Private:  balance, add/cancel/query order, open/closed orders, trades
//           history. All call kr_submit_private directly — same pattern
//           as the coinbase typed wrappers — and rely on the exchange-
//           vtable layer (kraken_exchange.c) to compose the abstraction
//           glue for cross-exchange consumers.
#define KR_INTERNAL
#include "kraken.h"

#include "exchange_api.h"
#include "json.h"
#include "kraken_pairs.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Endpoint path tails (after /0/public/ or /0/private/).
#define KR_PATH_OHLC            "OHLC"
#define KR_PATH_ASSETPAIRS      "AssetPairs"
#define KR_PATH_BALANCE_EX      "BalanceEx"
#define KR_PATH_ADD_ORDER       "AddOrder"
#define KR_PATH_CANCEL_ORDER    "CancelOrder"
#define KR_PATH_QUERY_ORDERS    "QueryOrders"
#define KR_PATH_OPEN_ORDERS     "OpenOrders"
#define KR_PATH_CLOSED_ORDERS   "ClosedOrders"
#define KR_PATH_TRADES_HISTORY  "TradesHistory"

#define KR_ERR_NO_CREDS \
  "Error: Kraken credentials not configured " \
  "(plugin.kraken.creds.{api_key,private_key})"

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

// Strip Kraken's currency-class prefix (X/Z) when the remaining code
// is at least 3 chars long. "ZUSD" → "USD", "XXBT" → "XBT".
static void
kr_map_currency(const char *src, char *dst, size_t cap)
{
  size_t slen;

  if(dst == NULL || cap == 0)
    return;

  if(src == NULL || src[0] == '\0')
  {
    dst[0] = '\0';
    return;
  }

  slen = strlen(src);

  if((src[0] == 'X' || src[0] == 'Z') && slen >= 4)
    snprintf(dst, cap, "%s", src + 1);
  else
    snprintf(dst, cap, "%s", src);
}

// Lowercase a buffer in-place.
static void
kr_str_lowercase(char *s)
{
  for(char *p = s; *p != '\0'; p++)
    *p = (char)tolower((unsigned char)*p);
}

// Parse "12345.6789" as a double, tolerant of leading/trailing spaces.
// Returns the parsed value or 0.0 on empty/NULL.
static double
kr_strtod(const char *s)
{
  return((s != NULL && s[0] != '\0') ? strtod(s, NULL) : 0.0);
}

// Read a numeric-or-string field as double. Kraken serializes
// floating-point values as JSON strings to preserve precision; some
// fields (e.g. OHLC tuple elements) are bare numbers. Accept both.
static double
kr_json_num(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *s;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0.0);

  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    return(s != NULL ? strtod(s, NULL) : 0.0);
  }

  return(json_object_get_double(v));
}

// Kraken's order time fields are floats: opentm = 1717000000.1234. The
// integer part is unix seconds, the fractional is sub-second.
static int64_t
kr_json_floatsec_to_ms(struct json_object *obj, const char *key)
{
  double v = kr_json_num(obj, key);

  if(v <= 0.0)
    return(0);

  return((int64_t)(v * 1000.0));
}

// ------------------------------------------------------------------
// Delivery helpers (fail path)
// ------------------------------------------------------------------

static void
kr_deliver_candles_fail(kr_request_t *r, const char *err)
{
  kraken_candles_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  kr_req_release(r);
}

static void
kr_deliver_order_fail(kr_request_t *r, const char *err)
{
  kraken_order_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  kr_req_release(r);
}

static void
kr_deliver_orders_fail(kr_request_t *r, const char *err)
{
  kraken_orders_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.orders != NULL)
    r->cb.orders(&res, r->user);

  kr_req_release(r);
}

static void
kr_deliver_balances_fail(kr_request_t *r, const char *err)
{
  kraken_balances_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.balances != NULL)
    r->cb.balances(&res, r->user);

  kr_req_release(r);
}

static void
kr_deliver_fills_fail(kr_request_t *r, const char *err)
{
  kraken_fills_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.fills != NULL)
    r->cb.fills(&res, r->user);

  kr_req_release(r);
}

static void
kr_deliver_assetpairs_fail(kr_request_t *r, const char *err, uint32_t kept)
{
  kraken_assetpairs_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);
  res.count = kept;

  if(r->cb.assetpairs != NULL)
    r->cb.assetpairs(&res, r->user);

  kr_req_release(r);
}

// ------------------------------------------------------------------
// Order-object parser. The wire shape is the same across QueryOrders,
// OpenOrders, and ClosedOrders — only the wrapping container differs.
// ------------------------------------------------------------------
//
// One Kraken order object (value side of the "ORDER_ID -> {...}" map):
//
//   { "refid":  null,
//     "userref": 0,
//     "cl_ord_id": "abc-...",
//     "status": "open" | "closed" | "canceled" | "expired" | "pending",
//     "opentm":  1717000000.1234,
//     "closetm": 1717000000.4321,
//     "starttm": 0,
//     "expiretm": 0,
//     "descr": {
//       "pair":      "XBTUSD",
//       "type":      "buy" | "sell",
//       "ordertype": "limit" | "market" | "stop-loss" | ...,
//       "price":     "30000.00",
//       "price2":    "0",
//       "leverage":  "none",
//       "order":     "buy 0.001 XBTUSD @ limit 30000.0",
//       "close":     ""
//     },
//     "vol":      "0.00100000",
//     "vol_exec": "0.00100000",
//     "cost":     "30.00000",
//     "fee":      "0.07800",
//     "price":    "30000.00000",
//     "misc":     "",
//     "oflags":   "fciq",
//     "trades":   [ "TKLFC4-NJOTR-..." ]
//   }
//
// `key` is the order id (used to populate out->order_id since the
// object itself doesn't carry it).

static bool
kr_parse_order(const char *order_id, struct json_object *obj,
    kraken_order_t *out)
{
  struct json_object *descr;
  char                tmp[40];

  if(obj == NULL || out == NULL || order_id == NULL)
    return(false);

  memset(out, 0, sizeof(*out));

  snprintf(out->order_id, sizeof(out->order_id), "%s", order_id);

  json_get_str(obj, "cl_ord_id", out->client_oid, sizeof(out->client_oid));
  json_get_str(obj, "status",    out->status,     sizeof(out->status));

  out->created_at_ms = kr_json_floatsec_to_ms(obj, "opentm");

  out->settled = (strcmp(out->status, "closed") == 0
               || strcmp(out->status, "canceled") == 0
               || strcmp(out->status, "expired") == 0);

  if(json_get_str(obj, "vol", tmp, sizeof(tmp)))
    out->size = kr_strtod(tmp);
  if(json_get_str(obj, "vol_exec", tmp, sizeof(tmp)))
    out->filled_size = kr_strtod(tmp);
  if(json_get_str(obj, "cost", tmp, sizeof(tmp)))
    out->executed_value = kr_strtod(tmp);
  if(json_get_str(obj, "fee", tmp, sizeof(tmp)))
    out->fill_fees = kr_strtod(tmp);

  // Average fill price (top-level "price" field on closed/partial).
  if(json_get_str(obj, "price", tmp, sizeof(tmp)))
    out->price = kr_strtod(tmp);

  descr = json_get_obj(obj, "descr");

  if(descr != NULL)
  {
    char ordertype[KRAKEN_TYPE_SZ];

    json_get_str(descr, "pair", out->product_id, sizeof(out->product_id));
    json_get_str(descr, "type", out->side,       sizeof(out->side));

    ordertype[0] = '\0';
    json_get_str(descr, "ordertype", ordertype, sizeof(ordertype));
    snprintf(out->type, sizeof(out->type), "%s", ordertype);

    // descr.price is the limit (or stop) price; prefer it when the
    // top-level price is zero (open order with no fills yet).
    if(json_get_str(descr, "price", tmp, sizeof(tmp)))
    {
      double p = kr_strtod(tmp);

      if(out->price == 0.0 && p > 0.0)
        out->price = p;
    }
  }

  kr_str_lowercase(out->side);
  kr_str_lowercase(out->status);
  kr_str_lowercase(out->type);

  // Kraken's order objects don't carry a tif field directly. Infer:
  // closed-already + opentm==closetm is IOC; otherwise default GTC.
  // This is good enough for the consumers we have today.
  {
    int64_t close_ms = kr_json_floatsec_to_ms(obj, "closetm");

    if(close_ms > 0 && out->created_at_ms > 0
        && close_ms - out->created_at_ms < 2000)
      snprintf(out->tif, sizeof(out->tif), "IOC");
    else
      snprintf(out->tif, sizeof(out->tif), "GTC");
  }

  // Kraken supports post-only via the `post` oflag. Surface when seen.
  {
    char oflags[64] = {0};

    if(json_get_str(obj, "oflags", oflags, sizeof(oflags)))
      out->post_only = (strstr(oflags, "post") != NULL);
  }

  return(out->order_id[0] != '\0');
}

// ------------------------------------------------------------------
// candles — public, via exchange_request
// ------------------------------------------------------------------
//
// Response:
//   { "error": [], "result": { "<pair>": [[time, o, h, l, c, vwap, vol,
//                                          count], ...],
//                              "last": <int> } }
//
// The first key in `result` that isn't "last" carries the candle array.
// Drop the last bar (in-progress).

static void
kr_candles_exchange_resp(int http_status, const char *body, size_t body_len,
    const char *err_hint, void *user)
{
  kr_request_t            *r = user;
  kraken_candles_result_t  res = { 0 };
  struct json_object      *root = NULL;
  struct json_object      *result_obj;
  struct json_object      *arr = NULL;
  char                     errbuf[KRAKEN_ERR_SZ];
  uint32_t                 kept = 0;
  int                      len;
  int                      drop_last;

  if(r == NULL)
    return;

  switch(kr_classify_exchange(http_status, body, body_len, err_hint,
        errbuf, sizeof(errbuf)))
  {
    case KR_RESP_OK:
      break;

    case KR_RESP_RATE_LIMIT:
    case KR_RESP_TRANSPORT:
    case KR_RESP_HARD_ERROR:
    default:
      kr_deliver_candles_fail(r, errbuf);
      return;
  }

  root = json_parse_buf(body, body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_candles_fail(r,
        "Error: malformed JSON from Kraken OHLC");
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_candles_fail(r,
        "Error: missing result from Kraken OHLC");
    return;
  }

  // Find the first non-"last" key whose value is an array.
  json_object_object_foreach(result_obj, key, val)
  {
    if(strcmp(key, "last") == 0)
      continue;

    if(json_object_is_type(val, json_type_array))
    {
      arr = val;
      break;
    }
  }

  if(arr == NULL)
  {
    json_object_put(root);
    kr_deliver_candles_fail(r,
        "Error: no candle array in Kraken OHLC response");
    return;
  }

  len       = (int)json_object_array_length(arr);
  drop_last = (len > 0) ? 1 : 0;

  for(int i = 0; i < len - drop_last && kept < KRAKEN_MAX_CANDLES; i++)
  {
    struct json_object *row = json_object_array_get_idx(arr, i);
    kraken_candle_t    *c   = &res.rows[kept];

    if(row == NULL || !json_object_is_type(row, json_type_array))
      continue;

    if(json_object_array_length(row) < 8)
      continue;

    c->ts_open_sec = (int64_t)json_object_get_int64(
        json_object_array_get_idx(row, 0));

    {
      struct json_object *v;

      v = json_object_array_get_idx(row, 1);
      c->open  = json_object_is_type(v, json_type_string)
                   ? kr_strtod(json_object_get_string(v))
                   : json_object_get_double(v);

      v = json_object_array_get_idx(row, 2);
      c->high  = json_object_is_type(v, json_type_string)
                   ? kr_strtod(json_object_get_string(v))
                   : json_object_get_double(v);

      v = json_object_array_get_idx(row, 3);
      c->low   = json_object_is_type(v, json_type_string)
                   ? kr_strtod(json_object_get_string(v))
                   : json_object_get_double(v);

      v = json_object_array_get_idx(row, 4);
      c->close = json_object_is_type(v, json_type_string)
                   ? kr_strtod(json_object_get_string(v))
                   : json_object_get_double(v);

      v = json_object_array_get_idx(row, 5);
      c->vwap  = json_object_is_type(v, json_type_string)
                   ? kr_strtod(json_object_get_string(v))
                   : json_object_get_double(v);

      v = json_object_array_get_idx(row, 6);
      c->volume = json_object_is_type(v, json_type_string)
                    ? kr_strtod(json_object_get_string(v))
                    : json_object_get_double(v);

      v = json_object_array_get_idx(row, 7);
      c->trade_count = (uint32_t)json_object_get_int(v);
    }

    if(c->ts_open_sec <= 0)
      continue;

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, KR_CTX,
       "candles: %s interval=%u -> %u row(s)",
       r->product_id, r->interval_min, kept);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

async_rc_t
kraken_fetch_candles_async(const char *pair, uint32_t interval_minutes,
    int64_t since_sec, uint8_t prio,
    kraken_done_candles_cb_t cb, void *user)
{
  kr_request_t *r;
  char          rest_pair[KRAKEN_PRODUCT_ID_SZ];
  char          path[KR_URL_SZ];
  int           n;

  if(pair == NULL || pair[0] == '\0' || interval_minutes == 0 || cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type         = KR_REQ_CANDLES;
  r->interval_min = interval_minutes;
  r->since_sec    = since_sec;
  r->cb.candles   = cb;
  r->user         = user;

  snprintf(r->product_id, sizeof(r->product_id), "%s", pair);

  kr_pair_lookup_rest(pair, rest_pair, sizeof(rest_pair));

  if(since_sec > 0)
    n = snprintf(path, sizeof(path),
        KR_PATH_OHLC "?pair=%s&interval=%u&since=%" PRId64,
        rest_pair, interval_minutes, since_sec);
  else
    n = snprintf(path, sizeof(path),
        KR_PATH_OHLC "?pair=%s&interval=%u",
        rest_pair, interval_minutes);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    kr_deliver_candles_fail(r, "Error: Kraken OHLC path overflow");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(exchange_request("kraken", prio, EXCHANGE_OP_REST_GET,
        path, NULL, kr_candles_exchange_resp, r) != SUCCESS)
  {
    kr_deliver_candles_fail(r,
        "Error: failed to submit Kraken OHLC request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// assetpairs — public, via exchange_request
// ------------------------------------------------------------------
//
// Response:
//   { "error": [], "result": {
//       "XXBTZUSD": { "altname":"XBTUSD", "wsname":"XBT/USD",
//                     "aclass_base":"currency", "base":"XXBT", ...,
//                     "status":"online", ... },
//       ... } }
//
// Iterate the result object; for each value, extract altname + wsname
// and feed (altname, canonical=key, wsname) into the cache. The cache
// is cleared first so stale entries from a previous refresh are
// evicted.

static void
kr_assetpairs_exchange_resp(int http_status, const char *body, size_t body_len,
    const char *err_hint, void *user)
{
  kr_request_t       *r = user;
  struct json_object *root;
  struct json_object *result_obj;
  char                errbuf[KRAKEN_ERR_SZ];
  uint32_t            kept = 0;

  if(r == NULL)
    return;

  switch(kr_classify_exchange(http_status, body, body_len, err_hint,
        errbuf, sizeof(errbuf)))
  {
    case KR_RESP_OK:
      break;

    case KR_RESP_RATE_LIMIT:
    case KR_RESP_TRANSPORT:
    case KR_RESP_HARD_ERROR:
    default:
      kr_deliver_assetpairs_fail(r, errbuf, kr_pairs_count());
      return;
  }

  root = json_parse_buf(body, body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_assetpairs_fail(r,
        "Error: malformed JSON from Kraken AssetPairs",
        kr_pairs_count());
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_assetpairs_fail(r,
        "Error: missing result from Kraken AssetPairs",
        kr_pairs_count());
    return;
  }

  kr_pairs_clear();

  json_object_object_foreach(result_obj, key, val)
  {
    char altname[16] = {0};
    char wsname[KRAKEN_PRODUCT_ID_SZ] = {0};
    char status[16]  = {0};

    if(!json_object_is_type(val, json_type_object))
      continue;

    json_get_str(val, "altname", altname, sizeof(altname));
    json_get_str(val, "wsname",  wsname,  sizeof(wsname));
    json_get_str(val, "status",  status,  sizeof(status));

    if(altname[0] == '\0')
      continue;

    // Skip non-online pairs (delisted / cancel_only / post_only) so
    // the cache stays focused on tradable instruments.
    if(status[0] != '\0' && strcmp(status, "online") != 0)
      continue;

    kr_pairs_add(altname, key, wsname);
    kept++;
  }

  clam(CLAM_INFO, KR_CTX, "assetpairs: %u row(s) cached", kept);

  {
    kraken_assetpairs_result_t res = { 0 };

    res.count = kept;

    if(r->cb.assetpairs != NULL)
      r->cb.assetpairs(&res, r->user);
  }

  json_object_put(root);
  kr_req_release(r);
}

// No-op callback used by the fire-and-forget refresh from kr_start.
static void
kr_assetpairs_silent_cb(const kraken_assetpairs_result_t *res, void *user)
{
  (void)user;

  if(res->err[0] != '\0')
    clam(CLAM_WARN, KR_CTX,
        "assetpairs refresh: %s (count=%u)", res->err, res->count);
}

async_rc_t
kraken_assetpairs_refresh_async(kraken_done_assetpairs_cb_t cb, void *user)
{
  kr_request_t *r;

  r = kr_req_alloc();
  r->type          = KR_REQ_ASSETPAIRS;
  r->cb.assetpairs = (cb != NULL) ? cb : kr_assetpairs_silent_cb;
  r->user          = user;

  if(exchange_request("kraken", EXCHANGE_PRIO_MARKET_BACKFILL,
        EXCHANGE_OP_REST_GET, KR_PATH_ASSETPAIRS, NULL,
        kr_assetpairs_exchange_resp, r) != SUCCESS)
  {
    kr_deliver_assetpairs_fail(r,
        "Error: failed to submit Kraken AssetPairs request",
        kr_pairs_count());
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// balance — private POST /0/private/BalanceEx
// ------------------------------------------------------------------
//
// Response (BalanceEx — same shape as Balance but with hold info):
//   { "error": [], "result": {
//       "ZUSD": { "balance":  "5.0000", "hold_trade": "0.5000" },
//       "XXBT": { "balance":  "0.0100", "hold_trade": "0.0000" },
//       ... } }
//
// available = balance - hold_trade. We map the X/Z prefixed currency
// codes to short forms ("XBT" / "USD") for the surface.

static void
kr_balance_done(const curl_response_t *resp)
{
  kr_request_t              *r    = (kr_request_t *)resp->user_data;
  kraken_balances_result_t   res  = { 0 };
  char                       errbuf[KRAKEN_ERR_SZ];
  kr_resp_kind_t             kind;
  struct json_object        *root;
  struct json_object        *result_obj;
  uint32_t                   kept = 0;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    kr_deliver_balances_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_balances_fail(r,
        "Error: malformed JSON from Kraken BalanceEx");
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_balances_fail(r,
        "Error: missing result from Kraken BalanceEx");
    return;
  }

  json_object_object_foreach(result_obj, key, val)
  {
    kraken_account_t *row;
    double            bal  = 0.0;
    double            hold = 0.0;
    char              tmp[40];

    if(kept >= KRAKEN_MAX_ACCOUNTS)
      break;

    row = &res.rows[kept];
    memset(row, 0, sizeof(*row));

    if(json_object_is_type(val, json_type_object))
    {
      if(json_get_str(val, "balance", tmp, sizeof(tmp)))
        bal = kr_strtod(tmp);
      if(json_get_str(val, "hold_trade", tmp, sizeof(tmp)))
        hold = kr_strtod(tmp);
    }
    else if(json_object_is_type(val, json_type_string))
    {
      // Plain Balance shape: { "ZUSD": "12.34" } — fall back if the
      // operator points us at /Balance rather than /BalanceEx.
      bal = kr_strtod(json_object_get_string(val));
    }

    kr_map_currency(key, row->currency, sizeof(row->currency));
    row->balance   = bal;
    row->hold      = hold;
    row->available = bal - hold;

    if(row->currency[0] != '\0')
      kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, KR_CTX, "balances: %u currency row(s)", kept);

  if(r->cb.balances != NULL)
    r->cb.balances(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

async_rc_t
kraken_get_balance_async(kraken_done_balances_cb_t cb, void *user)
{
  kr_request_t *r;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type        = KR_REQ_BALANCE;
  r->cb.balances = cb;
  r->user        = user;

  if(!kr_apikey_configured())
  {
    kr_deliver_balances_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_BALANCE_EX,
        NULL, 0, kr_balance_done) != SUCCESS)
  {
    kr_deliver_balances_fail(r,
        "Error: failed to submit Kraken BalanceEx request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// add order — private POST /0/private/AddOrder
// ------------------------------------------------------------------
//
// Form body:
//   nonce=<n>&pair=<altname>&type=<buy|sell>&ordertype=<limit|market>
//   &volume=<size>[&price=<limit-price>][&timeinforce=<IOC|FOK>]
//   [&oflags=post][&validate=true][&cl_ord_id=<uuid>]
//
// Response:
//   { "error": [], "result": {
//       "descr": { "order": "buy 0.001 XBTUSD @ limit 30000.0" },
//       "txid":  [ "OABCDE-FGHIJ-12345" ] } }
//
// For validate=true the response carries no txid; deliver an order
// with status="validated" + descr-echoed product_id / type / size.

static void
kr_addorder_done(const curl_response_t *resp)
{
  kr_request_t          *r    = (kr_request_t *)resp->user_data;
  kraken_order_result_t  res  = { 0 };
  char                   errbuf[KRAKEN_ERR_SZ];
  kr_resp_kind_t         kind;
  struct json_object    *root;
  struct json_object    *result_obj;
  struct json_object    *txid_arr;
  struct json_object    *descr;
  bool                   validated = false;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    kr_deliver_order_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_order_fail(r,
        "Error: malformed JSON from Kraken AddOrder");
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_order_fail(r,
        "Error: missing result from Kraken AddOrder");
    return;
  }

  txid_arr = json_get_array(result_obj, "txid");

  if(txid_arr != NULL && json_object_array_length(txid_arr) > 0)
  {
    struct json_object *t0   = json_object_array_get_idx(txid_arr, 0);
    const char         *txid = t0 ? json_object_get_string(t0) : NULL;

    if(txid != NULL)
      snprintf(res.order.order_id, sizeof(res.order.order_id), "%s", txid);
  }
  else
  {
    validated = true;
  }

  descr = json_get_obj(result_obj, "descr");

  if(descr != NULL)
  {
    json_get_str(descr, "pair", res.order.product_id,
        sizeof(res.order.product_id));
  }

  // Echo selectors we recorded at submit time so the consumer sees a
  // populated row (Kraken's AddOrder response is sparse).
  if(res.order.product_id[0] == '\0' && r->product_id[0] != '\0')
    snprintf(res.order.product_id, sizeof(res.order.product_id),
        "%s", r->product_id);

  if(res.order.client_oid[0] == '\0' && r->order_id[0] != '\0')
    snprintf(res.order.client_oid, sizeof(res.order.client_oid),
        "%s", r->order_id);

  snprintf(res.order.status, sizeof(res.order.status), "%s",
      validated ? "validated" : "open");

  clam(CLAM_DEBUG2, KR_CTX,
       "AddOrder: txid='%s' validated=%d product='%s'",
       res.order.order_id, (int)validated, res.order.product_id);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

async_rc_t
kraken_add_order_async(const kraken_place_order_req_t *req,
    kraken_done_order_cb_t cb, void *user)
{
  kr_request_t *r;
  kr_form_t     form;
  char          form_buf[KR_BODY_SZ];
  char          rest_pair[KRAKEN_PRODUCT_ID_SZ];
  bool          is_limit;
  bool          is_market;
  bool          is_buy;
  bool          is_sell;

  if(req == NULL || cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type     = KR_REQ_ADD_ORDER;
  r->cb.order = cb;
  r->user     = user;

  snprintf(r->product_id, sizeof(r->product_id), "%s", req->product_id);

  // Echo the caller's cl_ord_id into kr_request_t::order_id so the
  // response delivery can put it in res.order.client_oid even when
  // Kraken's response omits it.
  snprintf(r->order_id, sizeof(r->order_id), "%s", req->client_oid);

  if(!kr_apikey_configured())
  {
    kr_deliver_order_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  is_limit  = (strcmp(req->type, "limit")  == 0);
  is_market = (strcmp(req->type, "market") == 0);
  is_buy    = (strcmp(req->side, "buy")    == 0);
  is_sell   = (strcmp(req->side, "sell")   == 0);

  if(!is_limit && !is_market)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken supports order types 'limit' and 'market'");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(!is_buy && !is_sell)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken side must be 'buy' or 'sell'");
    return(ASYNC_FAILED_DELIVERED);
  }

  // Kraken's AddOrder takes `volume` (base-ccy size); funds-quoted
  // orders aren't supported. Surface a clean error so the trade engine
  // can pre-compute size from funds via last-price.
  if(req->funds > 0.0 && req->size <= 0.0)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken requires size, not funds; "
        "pre-compute via last_price");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(req->size <= 0.0)
  {
    kr_deliver_order_fail(r, "Error: positive size required");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(is_limit && req->price <= 0.0)
  {
    kr_deliver_order_fail(r,
        "Error: positive price required for a limit order");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(is_market && req->post_only)
  {
    kr_deliver_order_fail(r,
        "Error: post_only is invalid on market orders");
    return(ASYNC_FAILED_DELIVERED);
  }

  kr_pair_lookup_rest(req->product_id, rest_pair, sizeof(rest_pair));

  if(rest_pair[0] == '\0')
  {
    kr_deliver_order_fail(r, "Error: unknown product_id");
    return(ASYNC_FAILED_DELIVERED);
  }

  kr_form_init(&form, form_buf, sizeof(form_buf));

  if(kr_form_add(&form, "pair", rest_pair) != SUCCESS
      || kr_form_add(&form, "type", is_buy ? "buy" : "sell") != SUCCESS
      || kr_form_add(&form, "ordertype", req->type) != SUCCESS
      || kr_form_add_double(&form, "volume", req->size) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken AddOrder body overflow");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(is_limit
      && kr_form_add_double(&form, "price", req->price) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken AddOrder body overflow (price)");
    return(ASYNC_FAILED_DELIVERED);
  }

  // TIF — GTC is the implicit default; emit only IOC / FOK.
  if(req->tif[0] != '\0' && strcmp(req->tif, "GTC") != 0)
  {
    if(strcmp(req->tif, "IOC") != 0 && strcmp(req->tif, "FOK") != 0)
    {
      kr_deliver_order_fail(r,
          "Error: Kraken AddOrder supports tif GTC/IOC/FOK only");
      return(ASYNC_FAILED_DELIVERED);
    }

    if(kr_form_add(&form, "timeinforce", req->tif) != SUCCESS)
    {
      kr_deliver_order_fail(r,
          "Error: Kraken AddOrder body overflow (tif)");
      return(ASYNC_FAILED_DELIVERED);
    }
  }

  if(req->post_only
      && kr_form_add(&form, "oflags", "post") != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken AddOrder body overflow (post_only)");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(req->validate
      && kr_form_add_bool(&form, "validate", true) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken AddOrder body overflow (validate)");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(req->client_oid[0] != '\0'
      && kr_form_add(&form, "cl_ord_id", req->client_oid) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: Kraken AddOrder body overflow (cl_ord_id)");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_ADD_ORDER,
        form.buf, form.len, kr_addorder_done) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: failed to submit Kraken AddOrder request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// cancel order — private POST /0/private/CancelOrder
// ------------------------------------------------------------------
//
// Request body: txid=<order_id>
// Response:     { "error": [], "result": { "count": 1, "pending": false } }

static void
kr_cancelorder_done(const curl_response_t *resp)
{
  kr_request_t          *r    = (kr_request_t *)resp->user_data;
  kraken_order_result_t  res  = { 0 };
  char                   errbuf[KRAKEN_ERR_SZ];
  kr_resp_kind_t         kind;
  struct json_object    *root;
  struct json_object    *result_obj;
  int32_t                count = 0;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    kr_deliver_order_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_order_fail(r,
        "Error: malformed JSON from Kraken CancelOrder");
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_order_fail(r,
        "Error: missing result from Kraken CancelOrder");
    return;
  }

  json_get_int(result_obj, "count", &count);

  // Echo the order_id we sent in so the caller can correlate.
  snprintf(res.order.order_id, sizeof(res.order.order_id),
      "%s", r->order_id);
  snprintf(res.order.status, sizeof(res.order.status), "%s",
      count > 0 ? "canceled" : "unknown");

  if(count <= 0)
    snprintf(res.err, sizeof(res.err),
        "Error: Kraken CancelOrder removed nothing");

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

async_rc_t
kraken_cancel_order_async(const char *order_id,
    kraken_done_order_cb_t cb, void *user)
{
  kr_request_t *r;
  kr_form_t     form;
  char          form_buf[KR_BODY_SZ];

  if(order_id == NULL || order_id[0] == '\0' || cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type     = KR_REQ_CANCEL_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!kr_apikey_configured())
  {
    kr_deliver_order_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  kr_form_init(&form, form_buf, sizeof(form_buf));

  if(kr_form_add(&form, "txid", order_id) != SUCCESS)
  {
    kr_deliver_order_fail(r, "Error: Kraken CancelOrder body overflow");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_CANCEL_ORDER,
        form.buf, form.len, kr_cancelorder_done) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: failed to submit Kraken CancelOrder request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// query order — private POST /0/private/QueryOrders
// ------------------------------------------------------------------
//
// Request: txid=<order_id>
// Response:
//   { "error": [], "result": { "ORD1": {<order-object>} } }
//
// We always query exactly one txid; pull the matching key.

static void
kr_queryorder_done(const curl_response_t *resp)
{
  kr_request_t          *r    = (kr_request_t *)resp->user_data;
  kraken_order_result_t  res  = { 0 };
  char                   errbuf[KRAKEN_ERR_SZ];
  kr_resp_kind_t         kind;
  struct json_object    *root;
  struct json_object    *result_obj;
  bool                   parsed = false;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    kr_deliver_order_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_order_fail(r,
        "Error: malformed JSON from Kraken QueryOrders");
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_order_fail(r,
        "Error: missing result from Kraken QueryOrders");
    return;
  }

  json_object_object_foreach(result_obj, key, val)
  {
    if(kr_parse_order(key, val, &res.order))
      parsed = true;
    break;   // exactly one txid requested
  }

  if(!parsed)
  {
    json_object_put(root);
    kr_deliver_order_fail(r,
        "Error: Kraken QueryOrders returned no matching order");
    return;
  }

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

async_rc_t
kraken_query_order_async(const char *order_id,
    kraken_done_order_cb_t cb, void *user)
{
  kr_request_t *r;
  kr_form_t     form;
  char          form_buf[KR_BODY_SZ];

  if(order_id == NULL || order_id[0] == '\0' || cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type     = KR_REQ_QUERY_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!kr_apikey_configured())
  {
    kr_deliver_order_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  kr_form_init(&form, form_buf, sizeof(form_buf));

  if(kr_form_add(&form, "txid", order_id) != SUCCESS)
  {
    kr_deliver_order_fail(r, "Error: Kraken QueryOrders body overflow");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_QUERY_ORDERS,
        form.buf, form.len, kr_queryorder_done) != SUCCESS)
  {
    kr_deliver_order_fail(r,
        "Error: failed to submit Kraken QueryOrders request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// open/closed orders — private POST /0/private/OpenOrders|ClosedOrders
// ------------------------------------------------------------------
//
// OpenOrders   response: { "error":[], "result": { "open": { "ORD":{} } } }
// ClosedOrders response: { "error":[], "result": { "closed": { "ORD":{} },
//                                                  "count": N } }

static void
kr_orderslist_done_inner(kr_request_t *r, const curl_response_t *resp,
    const char *inner_key)
{
  kraken_orders_result_t  res = { 0 };
  char                    errbuf[KRAKEN_ERR_SZ];
  kr_resp_kind_t          kind;
  struct json_object     *root;
  struct json_object     *result_obj;
  struct json_object     *list_obj;
  uint32_t                kept = 0;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    kr_deliver_orders_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_orders_fail(r,
        "Error: malformed JSON from Kraken orders list");
    return;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL)
  {
    json_object_put(root);
    kr_deliver_orders_fail(r,
        "Error: missing result from Kraken orders list");
    return;
  }

  list_obj = json_get_obj(result_obj, inner_key);

  if(list_obj == NULL)
  {
    json_object_put(root);
    res.count = 0;

    if(r->cb.orders != NULL)
      r->cb.orders(&res, r->user);

    kr_req_release(r);
    return;
  }

  json_object_object_foreach(list_obj, key, val)
  {
    if(kept >= KRAKEN_MAX_ORDERS_LIST)
      break;

    if(kr_parse_order(key, val, &res.rows[kept]))
      kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, KR_CTX, "%s: %u row(s)", inner_key, kept);

  if(r->cb.orders != NULL)
    r->cb.orders(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

static void
kr_openorders_done(const curl_response_t *resp)
{
  kr_orderslist_done_inner((kr_request_t *)resp->user_data, resp, "open");
}

static void
kr_closedorders_done(const curl_response_t *resp)
{
  kr_orderslist_done_inner((kr_request_t *)resp->user_data, resp, "closed");
}

async_rc_t
kraken_open_orders_async(kraken_done_orders_cb_t cb, void *user)
{
  kr_request_t *r;

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type      = KR_REQ_OPEN_ORDERS;
  r->cb.orders = cb;
  r->user      = user;

  if(!kr_apikey_configured())
  {
    kr_deliver_orders_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_OPEN_ORDERS,
        NULL, 0, kr_openorders_done) != SUCCESS)
  {
    kr_deliver_orders_fail(r,
        "Error: failed to submit Kraken OpenOrders request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
kraken_closed_orders_async(int64_t start_sec, kraken_done_orders_cb_t cb,
    void *user)
{
  kr_request_t *r;
  kr_form_t     form;
  char          form_buf[KR_BODY_SZ];

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type      = KR_REQ_CLOSED_ORDERS;
  r->cb.orders = cb;
  r->user      = user;
  r->since_sec = start_sec;

  if(!kr_apikey_configured())
  {
    kr_deliver_orders_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  kr_form_init(&form, form_buf, sizeof(form_buf));

  if(start_sec > 0
      && kr_form_add_int(&form, "start", start_sec) != SUCCESS)
  {
    kr_deliver_orders_fail(r, "Error: Kraken ClosedOrders body overflow");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_CLOSED_ORDERS,
        form.buf, form.len, kr_closedorders_done) != SUCCESS)
  {
    kr_deliver_orders_fail(r,
        "Error: failed to submit Kraken ClosedOrders request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ------------------------------------------------------------------
// trades history — private POST /0/private/TradesHistory
// ------------------------------------------------------------------
//
// Request body: [start=<unix-sec>]
// Response:
//   { "error":[], "result": { "trades": { "TID1": {
//                              "ordertxid": "OXXX",
//                              "pair":      "XBTUSD",
//                              "time":      1717000000.123,
//                              "type":      "buy",
//                              "ordertype": "limit",
//                              "price":     "30000.0",
//                              "vol":       "0.001",
//                              "cost":      "30.00",
//                              "fee":       "0.075",
//                              "misc":      ""
//                            }, ... },
//                            "count": N } }
//
// Client-side filters: r->order_id (match `ordertxid`) and
// r->filter_product_id (match `pair`).

static bool
kr_filter_pair_match(const char *want, const char *got)
{
  if(want == NULL || want[0] == '\0')
    return(true);

  // Caller may have passed wsname / canonical / altname. Compare
  // against all three by looking the canonical up in the cache.
  char want_alt[KRAKEN_PRODUCT_ID_SZ];

  kr_pair_lookup_rest(want, want_alt, sizeof(want_alt));

  return(got != NULL && (strcmp(got, want) == 0
                      || strcmp(got, want_alt) == 0));
}

static void
kr_tradeshistory_done(const curl_response_t *resp)
{
  kr_request_t           *r    = (kr_request_t *)resp->user_data;
  kraken_fills_result_t   res  = { 0 };
  char                    errbuf[KRAKEN_ERR_SZ];
  kr_resp_kind_t          kind;
  struct json_object     *root;
  struct json_object     *result_obj;
  struct json_object     *trades_obj;
  uint32_t                kept = 0;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    kr_deliver_fills_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    kr_deliver_fills_fail(r,
        "Error: malformed JSON from Kraken TradesHistory");
    return;
  }

  result_obj = json_get_obj(root, "result");
  trades_obj = result_obj != NULL ? json_get_obj(result_obj, "trades") : NULL;

  if(trades_obj == NULL)
  {
    res.count = 0;
    json_object_put(root);

    if(r->cb.fills != NULL)
      r->cb.fills(&res, r->user);

    kr_req_release(r);
    return;
  }

  json_object_object_foreach(trades_obj, key, val)
  {
    kraken_fill_t *row;
    char           pair[KRAKEN_PRODUCT_ID_SZ] = {0};
    char           tmp[40];

    if(kept >= KRAKEN_MAX_FILLS_LIST)
      break;

    if(!json_object_is_type(val, json_type_object))
      continue;

    row = &res.rows[kept];
    memset(row, 0, sizeof(*row));

    json_get_str(val, "ordertxid", row->order_id,  sizeof(row->order_id));
    json_get_str(val, "pair",      pair,           sizeof(pair));
    json_get_str(val, "type",      row->side,      sizeof(row->side));

    kr_str_lowercase(row->side);
    snprintf(row->product_id, sizeof(row->product_id), "%s", pair);

    // Optional client-side filters from kr_request_t.
    if(r->order_id[0] != '\0'
        && strcmp(row->order_id, r->order_id) != 0)
      continue;

    if(!kr_filter_pair_match(r->filter_product_id, pair))
      continue;

    if(json_get_str(val, "price", tmp, sizeof(tmp)))
      row->price = kr_strtod(tmp);
    if(json_get_str(val, "vol", tmp, sizeof(tmp)))
      row->size = kr_strtod(tmp);
    if(json_get_str(val, "fee", tmp, sizeof(tmp)))
      row->fee = kr_strtod(tmp);

    row->time_ms = kr_json_floatsec_to_ms(val, "time");

    // Kraken's trade key (TID1) is the trade id. Map to numeric — many
    // trade ids are not pure numeric, so we hash the first 8 hex chars
    // when not parseable as a decimal. For now: try strtoll, fall back
    // to a portion of the string treated as hex.
    {
      char *end = NULL;
      long long v = strtoll(key, &end, 10);

      if(end != key && (*end == '\0' || *end == '-'))
        row->trade_id = (int64_t)v;
    }

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, KR_CTX,
       "TradesHistory: %u row(s) order_filter='%s' product_filter='%s'",
       kept, r->order_id, r->filter_product_id);

  if(r->cb.fills != NULL)
    r->cb.fills(&res, r->user);

  json_object_put(root);
  kr_req_release(r);
}

async_rc_t
kraken_trades_history_async(const char *order_id, const char *product_id,
    int64_t start_sec, kraken_done_fills_cb_t cb, void *user)
{
  kr_request_t *r;
  kr_form_t     form;
  char          form_buf[KR_BODY_SZ];

  if(cb == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = kr_req_alloc();
  r->type     = KR_REQ_TRADES_HISTORY;
  r->cb.fills = cb;
  r->user     = user;
  r->since_sec = start_sec;

  if(order_id != NULL && order_id[0] != '\0')
    snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);
  if(product_id != NULL && product_id[0] != '\0')
    snprintf(r->filter_product_id, sizeof(r->filter_product_id),
        "%s", product_id);

  if(!kr_apikey_configured())
  {
    kr_deliver_fills_fail(r, KR_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  kr_form_init(&form, form_buf, sizeof(form_buf));

  if(start_sec > 0
      && kr_form_add_int(&form, "start", start_sec) != SUCCESS)
  {
    kr_deliver_fills_fail(r, "Error: Kraken TradesHistory body overflow");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(kr_submit_private(r, &r->slot, CURL_PRIO_NORMAL, KR_PATH_TRADES_HISTORY,
        form.buf, form.len, kr_tradeshistory_done) != SUCCESS)
  {
    kr_deliver_fills_fail(r,
        "Error: failed to submit Kraken TradesHistory request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}
