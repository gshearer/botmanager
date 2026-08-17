// botmanager — MIT
// Coinbase Advanced Trade: authenticated REST — orders + accounts.
//
// Wraps the five private endpoints whenmoon and any future cmd-plugin
// will exercise:
//
//   GET  /api/v3/brokerage/accounts
//   POST /api/v3/brokerage/orders
//   GET  /api/v3/brokerage/orders/historical/{order_id}
//   GET  /api/v3/brokerage/orders/historical/batch?order_status=&product_id=&limit=
//   POST /api/v3/brokerage/orders/batch_cancel
//
// Every call requires CDP credentials; cb_apikey_configured short-
// circuits with CB_ERR_NO_CREDS when either KV is empty. Request
// bodies (place_order, batch_cancel) are rendered with json-c and
// owned by the request struct until cb_req_release runs.
#define CB_INTERNAL
#include "coinbase.h"

#include "curl.h"
#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define CB_ERR_NO_CREDS \
  "Error: Coinbase credentials not configured " \
  "(plugin.coinbase.creds.{key_name,private_key_pem})"

// Endpoint paths.
#define CB_PATH_ACCOUNTS      "/api/v3/brokerage/accounts"
#define CB_PATH_ORDERS        "/api/v3/brokerage/orders"
#define CB_PATH_ORDER_GET     "/api/v3/brokerage/orders/historical/"
#define CB_PATH_ORDERS_LIST   "/api/v3/brokerage/orders/historical/batch"
#define CB_PATH_BATCH_CANCEL  "/api/v3/brokerage/orders/batch_cancel"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// ISO-8601 → milliseconds since epoch. Mirrors cb_ws_parse_iso8601_ms
// in coinbase_ws_channels.c; duplicated rather than promoted so each
// TU keeps its own static. Returns 0 on any parse failure.
static int64_t
cb_parse_iso8601_ms(const char *s)
{
  struct tm    tm = {0};
  int          y, M, d, h, m, sec;
  long         frac_ms = 0;
  const char  *p;
  time_t       t;

  if(s == NULL || s[0] == '\0')
    return(0);

  if(sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &M, &d, &h, &m, &sec) != 6)
    return(0);

  tm.tm_year = y - 1900;
  tm.tm_mon  = M - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min  = m;
  tm.tm_sec  = sec;

  t = timegm(&tm);

  if(t == (time_t)-1)
    return(0);

  p = strchr(s, '.');

  if(p != NULL)
  {
    long ns     = 0;
    int  digits = 0;
    int  i;

    p++;

    while(*p >= '0' && *p <= '9' && digits < 9)
    {
      ns = ns * 10 + (*p - '0');
      p++;
      digits++;
    }

    for(i = digits; i < 9; i++)
      ns *= 10;

    frac_ms = ns / 1000000;
  }

  return((int64_t)t * 1000 + frac_ms);
}

// Generate a v4 UUID string into `out` (caller buffer, ≥37 bytes).
// Returns SUCCESS / FAIL. Used to fill client_order_id when the
// caller didn't supply one — Advanced Trade rejects place-order
// without a client_order_id.
static bool
cb_uuid_v4(char *out, size_t cap)
{
  unsigned char buf[16];
  ssize_t       n;

  if(out == NULL || cap < 37)
    return(FAIL);

  n = getrandom(buf, sizeof(buf), 0);

  if(n != (ssize_t)sizeof(buf))
    return(FAIL);

  // RFC 4122 §4.4 v4: set version (top 4 bits of byte 6) and variant
  // (top 2 bits of byte 8).
  buf[6] = (unsigned char)((buf[6] & 0x0f) | 0x40);
  buf[8] = (unsigned char)((buf[8] & 0x3f) | 0x80);

  snprintf(out, cap,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
      "%02x%02x%02x%02x%02x%02x",
      buf[0], buf[1], buf[2],  buf[3],  buf[4],  buf[5],
      buf[6], buf[7], buf[8],  buf[9],  buf[10], buf[11],
      buf[12], buf[13], buf[14], buf[15]);

  return(SUCCESS);
}

// Uppercase a side token in-place ("buy" → "BUY"). Caller passes a
// NUL-terminated buffer big enough to hold the result.
static void
cb_side_uppercase(char *side)
{
  for(char *p = side; *p != '\0'; p++)
    *p = (char)toupper((unsigned char)*p);
}

// Lowercase a side / status token in-place. Symmetric to the
// uppercaser; used when normalizing parsed AdvTrade values for fields
// whose docstring promised lowercase.
static void
cb_str_lowercase(char *s)
{
  for(char *p = s; *p != '\0'; p++)
    *p = (char)tolower((unsigned char)*p);
}

// ----------------------------------------------------------------------
// Delivery helpers (failure path)
// ----------------------------------------------------------------------

static void
cb_deliver_order_fail(cb_request_t *r, const char *err)
{
  coinbase_order_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  cb_req_release(r);
}

static void
cb_deliver_orders_fail(cb_request_t *r, const char *err)
{
  coinbase_orders_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.orders != NULL)
    r->cb.orders(&res, r->user);

  cb_req_release(r);
}

static void
cb_deliver_accounts_fail(cb_request_t *r, const char *err)
{
  coinbase_accounts_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.accounts != NULL)
    r->cb.accounts(&res, r->user);

  cb_req_release(r);
}

// ----------------------------------------------------------------------
// POST /orders body render
// ----------------------------------------------------------------------
//
// Advanced Trade order shape:
//   {
//     "client_order_id": "<uuid>",
//     "product_id":      "BTC-USD",
//     "side":            "BUY"|"SELL",
//     "order_configuration": {
//       "<config_kind>": { ... config-specific fields ... }
//     }
//   }
//
// config_kind:
//   limit_limit_gtc   { base_size, limit_price, post_only }
//   limit_limit_ioc   { base_size, limit_price }
//   market_market_ioc { quote_size }   for buys with funds set
//                     { base_size  }   for buys with size only, all sells

static char *
cb_render_order_body(const coinbase_place_order_req_t *req,
    size_t *out_len, char *err_out, size_t err_cap)
{
  struct json_object *root;
  struct json_object *config;
  struct json_object *config_inner;
  const char         *json_text;
  char               *out;
  size_t              len;
  bool                is_market;
  bool                is_limit;
  bool                is_buy;
  bool                is_sell;
  bool                tif_gtc;
  bool                tif_ioc;
  char                client_oid[COINBASE_CLIENT_OID_SZ];
  char                side_up[COINBASE_SIDE_SZ];
  char                buf[64];

  *out_len = 0;

  if(req->product_id[0] == '\0' || req->side[0] == '\0'
      || req->type[0] == '\0')
  {
    snprintf(err_out, err_cap,
        "Error: order missing product_id, side, or type");
    return(NULL);
  }

  is_market = (strcmp(req->type, "market") == 0);
  is_limit  = (strcmp(req->type, "limit")  == 0);

  if(!is_market && !is_limit)
  {
    snprintf(err_out, err_cap,
        "Error: unsupported order type '%s' (use 'limit' or 'market')",
        req->type);
    return(NULL);
  }

  is_buy  = (strcmp(req->side, "buy")  == 0);
  is_sell = (strcmp(req->side, "sell") == 0);

  if(!is_buy && !is_sell)
  {
    snprintf(err_out, err_cap,
        "Error: unknown side '%s' (use 'buy' or 'sell')", req->side);
    return(NULL);
  }

  // TIF semantics. Default GTC for limit, IOC for market (Advanced
  // Trade only ships market_market_ioc — no GTC market).
  tif_gtc = (req->tif[0] == '\0' && is_limit) || strcmp(req->tif, "GTC") == 0;
  tif_ioc = (req->tif[0] == '\0' && is_market) || strcmp(req->tif, "IOC") == 0;

  if(is_market && req->post_only)
  {
    snprintf(err_out, err_cap,
        "Error: post_only is invalid on market orders");
    return(NULL);
  }

  if(is_limit && (!tif_gtc && !tif_ioc))
  {
    snprintf(err_out, err_cap,
        "Error: unsupported tif '%s' (use 'GTC' or 'IOC')", req->tif);
    return(NULL);
  }

  if(is_limit && req->price <= 0.0)
  {
    snprintf(err_out, err_cap,
        "Error: limit orders require a positive price");
    return(NULL);
  }

  if(is_limit && req->size <= 0.0)
  {
    snprintf(err_out, err_cap,
        "Error: limit orders require a positive size");
    return(NULL);
  }

  if(is_market && is_buy && req->funds <= 0.0 && req->size <= 0.0)
  {
    snprintf(err_out, err_cap,
        "Error: market-buy requires funds or size");
    return(NULL);
  }

  if(is_market && is_sell && req->size <= 0.0)
  {
    snprintf(err_out, err_cap,
        "Error: market-sell requires a positive size");
    return(NULL);
  }

  // client_order_id: caller-supplied or freshly minted v4 UUID.
  if(req->client_oid[0] != '\0')
  {
    snprintf(client_oid, sizeof(client_oid), "%s", req->client_oid);
  }
  else if(cb_uuid_v4(client_oid, sizeof(client_oid)) != SUCCESS)
  {
    snprintf(err_out, err_cap,
        "Error: failed to mint client_order_id");
    return(NULL);
  }

  // Side uppercase.
  snprintf(side_up, sizeof(side_up), "%s", req->side);
  cb_side_uppercase(side_up);

  // Build JSON.
  root = json_object_new_object();

  if(root == NULL)
  {
    snprintf(err_out, err_cap, "Error: json_object alloc failed");
    return(NULL);
  }

  json_object_object_add(root, "client_order_id",
      json_object_new_string(client_oid));
  json_object_object_add(root, "product_id",
      json_object_new_string(req->product_id));
  json_object_object_add(root, "side",
      json_object_new_string(side_up));

  config       = json_object_new_object();
  config_inner = json_object_new_object();

  if(is_limit)
  {
    snprintf(buf, sizeof(buf), "%.10g", req->size);
    json_object_object_add(config_inner, "base_size",
        json_object_new_string(buf));

    snprintf(buf, sizeof(buf), "%.10g", req->price);
    json_object_object_add(config_inner, "limit_price",
        json_object_new_string(buf));

    if(tif_gtc)
    {
      json_object_object_add(config_inner, "post_only",
          json_object_new_boolean(req->post_only ? 1 : 0));
      json_object_object_add(config, "limit_limit_gtc", config_inner);
    }
    else
    {
      json_object_object_add(config, "limit_limit_ioc", config_inner);
    }
  }
  else
  {
    // market_market_ioc — buy with funds emits quote_size; everything
    // else emits base_size.
    if(is_buy && req->funds > 0.0)
    {
      snprintf(buf, sizeof(buf), "%.10g", req->funds);
      json_object_object_add(config_inner, "quote_size",
          json_object_new_string(buf));
    }
    else
    {
      snprintf(buf, sizeof(buf), "%.10g", req->size);
      json_object_object_add(config_inner, "base_size",
          json_object_new_string(buf));
    }

    json_object_object_add(config, "market_market_ioc", config_inner);
  }

  json_object_object_add(root, "order_configuration", config);

  json_text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

  if(json_text == NULL)
  {
    snprintf(err_out, err_cap, "Error: json_object_to_string failed");
    json_object_put(root);
    return(NULL);
  }

  len = strlen(json_text);

  if(len + 1 > CB_BODY_SZ)
  {
    snprintf(err_out, err_cap, "Error: order body exceeds CB_BODY_SZ");
    json_object_put(root);
    return(NULL);
  }

  out = mem_alloc(CB_CTX, "order_body", len + 1);
  memcpy(out, json_text, len + 1);

  json_object_put(root);

  *out_len = len;
  return(out);
}

// Render `{"order_ids":["<id>"]}` for batch_cancel. Same lifecycle as
// cb_render_order_body — caller transfers ownership to cb_request_t.
static char *
cb_render_cancel_body(const char *order_id, size_t *out_len)
{
  struct json_object *root;
  struct json_object *arr;
  const char         *json_text;
  char               *out;
  size_t              len;

  *out_len = 0;

  root = json_object_new_object();
  arr  = json_object_new_array();

  json_object_array_add(arr, json_object_new_string(order_id));
  json_object_object_add(root, "order_ids", arr);

  json_text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

  if(json_text == NULL)
  {
    json_object_put(root);
    return(NULL);
  }

  len = strlen(json_text);
  out = mem_alloc(CB_CTX, "cancel_body", len + 1);
  memcpy(out, json_text, len + 1);

  json_object_put(root);

  *out_len = len;
  return(out);
}

// ----------------------------------------------------------------------
// Response parsing helpers
// ----------------------------------------------------------------------

// Parse a single Advanced Trade order object into coinbase_order_t.
// The wire shape is:
//   {
//     "order_id":              "<uuid>",
//     "product_id":            "BTC-USD",
//     "user_id":               "...",
//     "side":                  "BUY"|"SELL",
//     "client_order_id":       "<uuid>",
//     "status":                "OPEN"|"FILLED"|"CANCELLED"|"EXPIRED"|...,
//     "time_in_force":         "GOOD_UNTIL_CANCELLED"|"IMMEDIATE_OR_CANCEL"|...,
//     "filled_size":           "0.001",
//     "average_filled_price":  "60000",
//     "filled_value":          "60",
//     "total_fees":            "0.30",
//     "settled":               true|false,
//     "created_time":          "2026-05-08T12:34:56.789Z",
//     "order_configuration": { "<kind>": { ... } }
//   }
//
// `side` and `status` are normalized to lowercase for backward
// compatibility with the `coinbase_order_t.side / .status` doc
// contract ("buy"/"sell"); `tif` keeps Advanced Trade's verbose form
// because there's no clean short form. `type` is derived from the
// order_configuration nested key.
static bool
cb_parse_order(struct json_object *obj, coinbase_order_t *out)
{
  struct json_object *cfg;
  struct json_object *inner;
  char                tif_long[40] = {0};
  char                created_time[40];
  bool                got_filled_price = false;

  if(obj == NULL || out == NULL)
    return(false);

  memset(out, 0, sizeof(*out));

  json_get_str(obj, "order_id",         out->order_id,   sizeof(out->order_id));
  json_get_str(obj, "client_order_id",  out->client_oid, sizeof(out->client_oid));
  json_get_str(obj, "product_id",       out->product_id, sizeof(out->product_id));
  json_get_str(obj, "side",             out->side,       sizeof(out->side));
  json_get_str(obj, "status",           out->status,     sizeof(out->status));
  json_get_str(obj, "time_in_force",    tif_long,        sizeof(tif_long));

  cb_str_lowercase(out->side);
  cb_str_lowercase(out->status);

  // tif: pick a short form. Advanced Trade emits verbose strings; we
  // map the common ones back to Exchange's three-letter shorthand.
  if(strcmp(tif_long, "GOOD_UNTIL_CANCELLED")    == 0
      || strcmp(tif_long, "GOOD_UNTIL_CANCELED") == 0
      || strcmp(tif_long, "GTC")                 == 0)
    snprintf(out->tif, sizeof(out->tif), "GTC");
  else if(strcmp(tif_long, "IMMEDIATE_OR_CANCEL") == 0
      || strcmp(tif_long, "IOC")                  == 0)
    snprintf(out->tif, sizeof(out->tif), "IOC");
  else if(strcmp(tif_long, "FILL_OR_KILL") == 0
      || strcmp(tif_long, "FOK")           == 0)
    snprintf(out->tif, sizeof(out->tif), "FOK");
  else if(strcmp(tif_long, "GOOD_UNTIL_DATE_TIME") == 0
      || strcmp(tif_long, "GTD")                   == 0
      || strcmp(tif_long, "GTT")                   == 0)
    snprintf(out->tif, sizeof(out->tif), "GTT");
  else
    snprintf(out->tif, sizeof(out->tif), "%s", tif_long);

  // Numeric strings.
  {
    char tmp[40];

    if(json_get_str(obj, "filled_size", tmp, sizeof(tmp)))
      out->filled_size = strtod(tmp, NULL);

    if(json_get_str(obj, "average_filled_price", tmp, sizeof(tmp)))
    {
      out->price = strtod(tmp, NULL);
      got_filled_price = (out->price > 0.0);
    }

    if(json_get_str(obj, "filled_value", tmp, sizeof(tmp)))
      out->executed_value = strtod(tmp, NULL);

    if(json_get_str(obj, "total_fees", tmp, sizeof(tmp)))
      out->fill_fees = strtod(tmp, NULL);
  }

  json_get_bool(obj, "settled", &out->settled);

  if(json_get_str(obj, "created_time", created_time, sizeof(created_time)))
    out->created_at_ms = cb_parse_iso8601_ms(created_time);

  // Decode order_configuration → type / size / limit_price / post_only.
  cfg = json_get_obj(obj, "order_configuration");

  if(cfg != NULL)
  {
    if((inner = json_get_obj(cfg, "limit_limit_gtc")) != NULL
        || (inner = json_get_obj(cfg, "limit_limit_ioc")) != NULL
        || (inner = json_get_obj(cfg, "limit_limit_gtd")) != NULL)
    {
      char tmp[40];

      snprintf(out->type, sizeof(out->type), "limit");

      if(json_get_str(inner, "base_size", tmp, sizeof(tmp)))
        out->size = strtod(tmp, NULL);

      // Posted limit price wins over average_filled_price for the
      // .price field — consumers asked for "the limit you set".
      if(json_get_str(inner, "limit_price", tmp, sizeof(tmp)))
        out->price = strtod(tmp, NULL);

      json_get_bool(inner, "post_only", &out->post_only);
    }
    else if((inner = json_get_obj(cfg, "market_market_ioc")) != NULL)
    {
      char tmp[40];

      snprintf(out->type, sizeof(out->type), "market");

      if(json_get_str(inner, "base_size", tmp, sizeof(tmp)))
        out->size = strtod(tmp, NULL);
      else if(json_get_str(inner, "quote_size", tmp, sizeof(tmp)))
        out->size = strtod(tmp, NULL);
    }
  }

  // If no order_configuration but we got an average_filled_price,
  // the field still has a useful value — keep it. Otherwise zero is
  // fine.
  (void)got_filled_price;

  return(out->order_id[0] != '\0'
      || out->product_id[0] != '\0'
      || out->status[0] != '\0');
}

// ----------------------------------------------------------------------
// Curl completion: place / get order
// ----------------------------------------------------------------------
//
// Place-order envelope:
//   { "success": true, "success_response": { "order_id": "...", ... },
//     "error_response": { "error": "...", "message": "..." } }
//
// Get-order envelope:
//   { "order": { ... full order object ... } }
//
// We accept either shape: if a top-level "order" object is present,
// parse it; if a "success_response" with order_id is present, parse
// that; if "error_response" is present and success is false, deliver
// the error string.

static void
cb_order_done(const curl_response_t *resp)
{
  cb_request_t            *r = (cb_request_t *)resp->user_data;
  coinbase_order_result_t  res = { 0 };
  char                     errbuf[CB_ERR_SZ];
  const char              *err;
  struct json_object      *root;
  struct json_object      *order_obj;
  struct json_object      *success_obj;
  bool                     success_flag = false;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_order_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_order_fail(r,
        "Error: malformed JSON from Coinbase order");
    return;
  }

  // Place-order failure envelope.
  if(json_get_bool(root, "success", &success_flag) && !success_flag)
  {
    struct json_object *err_obj = json_get_obj(root, "error_response");
    char                msg[CB_ERR_SZ];

    msg[0] = '\0';

    if(err_obj != NULL)
    {
      char field[64];

      if(json_get_str(err_obj, "message", field, sizeof(field)))
        snprintf(msg, sizeof(msg), "Error: Coinbase: %s", field);
      else if(json_get_str(err_obj, "error", field, sizeof(field)))
        snprintf(msg, sizeof(msg), "Error: Coinbase: %s", field);
    }

    if(msg[0] == '\0')
      snprintf(msg, sizeof(msg), "Error: Coinbase order rejected");

    json_object_put(root);
    cb_deliver_order_fail(r, msg);
    return;
  }

  // Place-order success envelope OR get-order envelope.
  order_obj = json_get_obj(root, "order");

  if(order_obj == NULL)
  {
    success_obj = json_get_obj(root, "success_response");

    if(success_obj != NULL)
      order_obj = success_obj;
  }

  if(order_obj == NULL)
    order_obj = root;

  cb_parse_order(order_obj, &res.order);

  clam(CLAM_DEBUG2, CB_CTX,
       "order done: id='%s' status='%s' type=%d",
       res.order.order_id, res.order.status, (int)r->type);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// ----------------------------------------------------------------------
// Curl completion: cancel order (batch_cancel single-id wrap)
// ----------------------------------------------------------------------
//
// batch_cancel response shape:
//   { "results": [ { "success": bool, "failure_reason": "...",
//                    "order_id": "..." } ] }
//
// We sent exactly one id; pull index [0]. On success, populate
// res.order.order_id; on failure, deliver the failure_reason string.

static void
cb_cancel_done(const curl_response_t *resp)
{
  cb_request_t            *r = (cb_request_t *)resp->user_data;
  coinbase_order_result_t  res = { 0 };
  char                     errbuf[CB_ERR_SZ];
  const char              *err;
  struct json_object      *root;
  struct json_object      *results;
  struct json_object      *first;
  bool                     ok = false;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_order_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_order_fail(r,
        "Error: malformed JSON from Coinbase batch_cancel");
    return;
  }

  results = json_get_array(root, "results");

  if(results == NULL || json_object_array_length(results) < 1)
  {
    json_object_put(root);
    cb_deliver_order_fail(r,
        "Error: empty results from Coinbase batch_cancel");
    return;
  }

  first = json_object_array_get_idx(results, 0);

  if(first == NULL)
  {
    json_object_put(root);
    cb_deliver_order_fail(r,
        "Error: null result from Coinbase batch_cancel");
    return;
  }

  json_get_bool(first, "success", &ok);
  json_get_str (first, "order_id",
      res.order.order_id, sizeof(res.order.order_id));

  if(!ok)
  {
    char reason[64];
    char msg[CB_ERR_SZ];

    if(json_get_str(first, "failure_reason", reason, sizeof(reason)))
      snprintf(msg, sizeof(msg), "Error: Coinbase cancel: %s", reason);
    else
      snprintf(msg, sizeof(msg), "Error: Coinbase cancel rejected");

    json_object_put(root);
    cb_deliver_order_fail(r, msg);
    return;
  }

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// ----------------------------------------------------------------------
// Curl completion: list orders
// ----------------------------------------------------------------------
//
// Advanced Trade list shape:
//   { "orders": [ {...}, ... ], "cursor": "...", "has_next": bool }

static void
cb_orders_list_done(const curl_response_t *resp)
{
  cb_request_t             *r = (cb_request_t *)resp->user_data;
  coinbase_orders_result_t  res = { 0 };
  char                      errbuf[CB_ERR_SZ];
  const char               *err;
  struct json_object       *root;
  struct json_object       *arr;
  int                       len;
  uint32_t                  kept = 0;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_orders_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_orders_fail(r,
        "Error: malformed JSON from Coinbase orders list");
    return;
  }

  arr = json_get_array(root, "orders");

  if(arr == NULL)
  {
    json_object_put(root);
    cb_deliver_orders_fail(r,
        "Error: unexpected Coinbase orders list shape");
    return;
  }

  len = (int)json_object_array_length(arr);

  for(int i = 0; i < len && kept < COINBASE_MAX_ORDERS_LIST; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, i);

    if(item == NULL)
      continue;

    if(cb_parse_order(item, &res.rows[kept]))
      kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, CB_CTX, "orders list: %u row(s) status='%s' product='%s'",
       kept, r->status, r->product_id);

  if(r->cb.orders != NULL)
    r->cb.orders(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// ----------------------------------------------------------------------
// Curl completion: accounts
// ----------------------------------------------------------------------
//
// Advanced Trade accounts shape:
//   { "accounts": [ { "uuid":   "...",
//                     "name":   "...",
//                     "currency": "USD",
//                     "available_balance": { "value": "12.34", "currency": "USD" },
//                     "hold":              { "value": "0",     "currency": "USD" },
//                     ... }, ... ],
//     "has_next": bool, "cursor": "...", "size": N }

static void
cb_accounts_done(const curl_response_t *resp)
{
  cb_request_t               *r = (cb_request_t *)resp->user_data;
  coinbase_accounts_result_t  res = { 0 };
  char                        errbuf[CB_ERR_SZ];
  const char                 *err;
  struct json_object         *root;
  struct json_object         *arr;
  int                         len;
  uint32_t                    kept = 0;
  const size_t                rows_cap =
      sizeof(res.rows) / sizeof(res.rows[0]);

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_accounts_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_accounts_fail(r,
        "Error: malformed JSON from Coinbase accounts");
    return;
  }

  arr = json_get_array(root, "accounts");

  if(arr == NULL)
  {
    json_object_put(root);
    cb_deliver_accounts_fail(r,
        "Error: unexpected Coinbase accounts response shape");
    return;
  }

  len = (int)json_object_array_length(arr);

  for(int i = 0; i < len && kept < rows_cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, i);
    struct json_object *avail_obj;
    struct json_object *hold_obj;
    coinbase_account_t *row;
    char                tmp[40];

    if(item == NULL)
      continue;

    row = &res.rows[kept];
    memset(row, 0, sizeof(*row));

    if(!json_get_str(item, "currency",
        row->currency, sizeof(row->currency)))
      continue;  // currency is the only required field

    avail_obj = json_get_obj(item, "available_balance");
    hold_obj  = json_get_obj(item, "hold");

    if(avail_obj != NULL
        && json_get_str(avail_obj, "value", tmp, sizeof(tmp)))
      row->available = strtod(tmp, NULL);

    if(hold_obj != NULL
        && json_get_str(hold_obj, "value", tmp, sizeof(tmp)))
      row->hold = strtod(tmp, NULL);

    // Exchange semantics: balance = available + hold (total).
    row->balance = row->available + row->hold;

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, CB_CTX, "accounts: %u currency row(s)", kept);

  if(r->cb.accounts != NULL)
    r->cb.accounts(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// ----------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------

async_rc_t
coinbase_place_order_async(const coinbase_place_order_req_t *req,
    coinbase_done_order_cb_t cb, void *user)
{
  cb_request_t *r;
  char         *body;
  size_t        body_len = 0;
  char          errbuf[CB_ERR_SZ];

  if(req == NULL)
    return(ASYNC_FAILED_UNDELIVERED);

  r = cb_req_alloc();
  r->type     = CB_REQ_PLACE_ORDER;
  r->cb.order = cb;
  r->user     = user;

  if(!cb_apikey_configured())
  {
    cb_deliver_order_fail(r, CB_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  body = cb_render_order_body(req, &body_len, errbuf, sizeof(errbuf));

  if(body == NULL)
  {
    cb_deliver_order_fail(r, errbuf);
    return(ASYNC_FAILED_DELIVERED);
  }

  // Transfer body ownership to the request — cb_req_release frees it.
  r->body     = body;
  r->body_len = body_len;

  if(cb_submit_private(r, &r->slot, CURL_PRIO_NORMAL, CURL_METHOD_POST,
        CB_PATH_ORDERS, body, body_len, cb_order_done) != SUCCESS)
  {
    cb_deliver_order_fail(r,
        "Error: failed to submit Coinbase place-order request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
coinbase_cancel_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user)
{
  cb_request_t *r;
  char         *body;
  size_t        body_len = 0;

  if(order_id == NULL || order_id[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  r = cb_req_alloc();
  r->type     = CB_REQ_CANCEL_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!cb_apikey_configured())
  {
    cb_deliver_order_fail(r, CB_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  body = cb_render_cancel_body(order_id, &body_len);

  if(body == NULL)
  {
    cb_deliver_order_fail(r, "Error: failed to render cancel body");
    return(ASYNC_FAILED_DELIVERED);
  }

  r->body     = body;
  r->body_len = body_len;

  if(cb_submit_private(r, &r->slot, CURL_PRIO_NORMAL, CURL_METHOD_POST,
        CB_PATH_BATCH_CANCEL, body, body_len, cb_cancel_done) != SUCCESS)
  {
    cb_deliver_order_fail(r,
        "Error: failed to submit Coinbase cancel-order request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
coinbase_get_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;

  if(order_id == NULL || order_id[0] == '\0')
    return(ASYNC_FAILED_UNDELIVERED);

  r = cb_req_alloc();
  r->type     = CB_REQ_GET_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!cb_apikey_configured())
  {
    cb_deliver_order_fail(r, CB_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  n = snprintf(path, sizeof(path), "%s%s", CB_PATH_ORDER_GET, order_id);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    cb_deliver_order_fail(r, "Error: order_id too long");
    return(ASYNC_FAILED_DELIVERED);
  }

  if(cb_submit_private(r, &r->slot, CURL_PRIO_NORMAL, CURL_METHOD_GET, path,
        NULL, 0, cb_order_done) != SUCCESS)
  {
    cb_deliver_order_fail(r,
        "Error: failed to submit Coinbase get-order request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
coinbase_list_orders_async(const char *status, const char *product_id,
    coinbase_done_orders_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;
  const char   *sep = "?";

  r = cb_req_alloc();
  r->type      = CB_REQ_GET_ORDER;   // list variant — union on orders cb
  r->cb.orders = cb;
  r->user      = user;

  if(status != NULL && status[0] != '\0')
    snprintf(r->status, sizeof(r->status), "%s", status);
  if(product_id != NULL && product_id[0] != '\0')
    snprintf(r->product_id, sizeof(r->product_id), "%s", product_id);

  if(!cb_apikey_configured())
  {
    cb_deliver_orders_fail(r, CB_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  n = snprintf(path, sizeof(path), "%s", CB_PATH_ORDERS_LIST);

  // Advanced Trade renamed `status` → `order_status` and accepts the
  // OPEN / FILLED / CANCELLED enum (uppercase). Caller still passes
  // lowercase per the Exchange-era contract; uppercase here.
  if(status != NULL && status[0] != '\0')
  {
    char up[COINBASE_STATUS_SZ];
    int  m;

    snprintf(up, sizeof(up), "%s", status);
    cb_side_uppercase(up);   // works on any ASCII string

    m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%sorder_status=%s", sep, up);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_orders_fail(r, "Error: orders query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
    n += m;
    sep = "&";
  }

  if(product_id != NULL && product_id[0] != '\0')
  {
    int m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%sproduct_id=%s", sep, product_id);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_orders_fail(r, "Error: orders query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
    n += m;
    sep = "&";
  }

  {
    int m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%slimit=%u", sep, (unsigned)COINBASE_MAX_ORDERS_LIST);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_orders_fail(r, "Error: orders query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
  }

  if(cb_submit_private(r, &r->slot, CURL_PRIO_NORMAL, CURL_METHOD_GET, path,
        NULL, 0, cb_orders_list_done) != SUCCESS)
  {
    cb_deliver_orders_fail(r,
        "Error: failed to submit Coinbase list-orders request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

async_rc_t
coinbase_get_accounts_async(coinbase_done_accounts_cb_t cb, void *user)
{
  cb_request_t *r;

  r = cb_req_alloc();
  r->type        = CB_REQ_GET_ACCOUNTS;
  r->cb.accounts = cb;
  r->user        = user;

  if(!cb_apikey_configured())
  {
    cb_deliver_accounts_fail(r, CB_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  if(cb_submit_private(r, &r->slot, CURL_PRIO_NORMAL, CURL_METHOD_GET,
        CB_PATH_ACCOUNTS, NULL, 0, cb_accounts_done) != SUCCESS)
  {
    cb_deliver_accounts_fail(r,
        "Error: failed to submit Coinbase accounts request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}

// ----------------------------------------------------------------------
// /api/v3/brokerage/orders/historical/fills (WM-LT-8-B3 REST safety net)
// ----------------------------------------------------------------------
//
// AT response shape:
//   { "fills": [ { "entry_id": "...", "trade_id": "...",
//                  "order_id": "...", "client_order_id": "...",
//                  "trade_time": "ISO-8601",
//                  "sequence_timestamp": "ISO-8601",
//                  "trade_type": "FILL",
//                  "price": "decimal-string",
//                  "size":  "decimal-string",
//                  "commission": "decimal-string",
//                  "product_id": "BTC-USD",
//                  "side": "BUY"|"SELL",
//                  ... }, ... ],
//     "cursor": "..." }

#define CB_PATH_FILLS  "/api/v3/brokerage/orders/historical/fills"

static void
cb_deliver_fills_fail(cb_request_t *r, const char *err)
{
  coinbase_fills_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.fills != NULL)
    r->cb.fills(&res, r->user);

  cb_req_release(r);
}

static bool
cb_parse_fill(struct json_object *obj, coinbase_fill_t *out)
{
  char tmp[64];

  if(obj == NULL || out == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));

  json_get_str(obj, "order_id",        out->order_id,
      sizeof(out->order_id));
  json_get_str(obj, "client_order_id", out->client_oid,
      sizeof(out->client_oid));
  json_get_str(obj, "product_id",      out->product_id,
      sizeof(out->product_id));
  json_get_str(obj, "side",            out->side, sizeof(out->side));
  cb_str_lowercase(out->side);

  if(json_get_str(obj, "trade_id", tmp, sizeof(tmp)))
    out->trade_id = strtoll(tmp, NULL, 10);

  if(json_get_str(obj, "price", tmp, sizeof(tmp)))
    out->price = strtod(tmp, NULL);
  if(json_get_str(obj, "size", tmp, sizeof(tmp)))
    out->size = strtod(tmp, NULL);
  if(json_get_str(obj, "commission", tmp, sizeof(tmp)))
    out->fee = strtod(tmp, NULL);

  // sequence_timestamp is the AT-monotone field that paginates fills;
  // trade_time is human-display. Prefer sequence_timestamp; fall back
  // to trade_time when absent.
  if(json_get_str(obj, "sequence_timestamp", tmp, sizeof(tmp)))
    out->time_ms = cb_parse_iso8601_ms(tmp);
  else if(json_get_str(obj, "trade_time", tmp, sizeof(tmp)))
    out->time_ms = cb_parse_iso8601_ms(tmp);

  return(out->trade_id != 0);
}

static void
cb_fills_list_done(const curl_response_t *resp)
{
  cb_request_t            *r = (cb_request_t *)resp->user_data;
  coinbase_fills_result_t  res = { 0 };
  char                     errbuf[CB_ERR_SZ];
  const char              *err;
  struct json_object      *root;
  struct json_object      *arr;
  int                      len;
  uint32_t                 kept = 0;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_fills_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_fills_fail(r,
        "Error: malformed JSON from Coinbase fills list");
    return;
  }

  arr = json_get_array(root, "fills");

  if(arr == NULL)
  {
    json_object_put(root);
    cb_deliver_fills_fail(r,
        "Error: unexpected Coinbase fills list shape");
    return;
  }

  len = (int)json_object_array_length(arr);

  for(int i = 0; i < len && kept < COINBASE_MAX_FILLS_LIST; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, i);

    if(item == NULL)
      continue;

    if(cb_parse_fill(item, &res.rows[kept]))
      kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, CB_CTX, "fills list: %u row(s) order_id='%s' product='%s'",
       kept, r->order_id, r->product_id);

  if(r->cb.fills != NULL)
    r->cb.fills(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Format `epoch_ms` as RFC3339 UTC ("2026-05-08T12:34:56Z"). `out` must
// be at least 21 bytes. Returns SUCCESS / FAIL.
static bool
cb_format_iso8601_z(int64_t epoch_ms, char *out, size_t cap)
{
  time_t    t  = (time_t)(epoch_ms / 1000);
  struct tm tm = {0};

  if(out == NULL || cap < 21) return(FAIL);

  if(gmtime_r(&t, &tm) == NULL) return(FAIL);

  if(strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
    return(FAIL);

  return(SUCCESS);
}

async_rc_t
coinbase_list_fills_async(const char *order_id, const char *product_id,
    int64_t start_ms, coinbase_done_fills_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;
  const char   *sep = "?";

  r = cb_req_alloc();
  r->type     = CB_REQ_LIST_FILLS;
  r->cb.fills = cb;
  r->user     = user;

  if(order_id != NULL && order_id[0] != '\0')
    snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);
  if(product_id != NULL && product_id[0] != '\0')
    snprintf(r->product_id, sizeof(r->product_id), "%s", product_id);

  if(!cb_apikey_configured())
  {
    cb_deliver_fills_fail(r, CB_ERR_NO_CREDS);
    return(ASYNC_FAILED_DELIVERED);
  }

  n = snprintf(path, sizeof(path), "%s", CB_PATH_FILLS);

  if(order_id != NULL && order_id[0] != '\0')
  {
    int m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%sorder_id=%s", sep, order_id);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_fills_fail(r, "Error: fills query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
    n += m;
    sep = "&";
  }

  if(product_id != NULL && product_id[0] != '\0')
  {
    int m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%sproduct_id=%s", sep, product_id);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_fills_fail(r, "Error: fills query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
    n += m;
    sep = "&";
  }

  if(start_ms > 0)
  {
    char ts[32];
    int  m;

    if(cb_format_iso8601_z(start_ms, ts, sizeof(ts)) != SUCCESS)
    {
      cb_deliver_fills_fail(r, "Error: fills start_ms format failed");
      return(ASYNC_FAILED_DELIVERED);
    }

    m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%sstart_sequence_timestamp=%s", sep, ts);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_fills_fail(r, "Error: fills query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
    n += m;
    sep = "&";
  }

  {
    int m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%slimit=%u", sep, (unsigned)COINBASE_MAX_FILLS_LIST);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_fills_fail(r, "Error: fills query too long");
      return(ASYNC_FAILED_DELIVERED);
    }
  }

  if(cb_submit_private(r, &r->slot, CURL_PRIO_NORMAL, CURL_METHOD_GET, path,
        NULL, 0, cb_fills_list_done) != SUCCESS)
  {
    cb_deliver_fills_fail(r,
        "Error: failed to submit Coinbase list-fills request");
    return(ASYNC_FAILED_DELIVERED);
  }

  return(ASYNC_AIRBORNE);
}
