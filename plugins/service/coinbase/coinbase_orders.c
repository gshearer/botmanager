// botmanager — MIT
// Coinbase Exchange: authenticated REST — orders + accounts.
//
// Builds on CB1's signing primitive and CB2's dispatcher. Every call in
// this TU requires credentials (apikey + apisecret + passphrase); a
// missing credential triggers a fail-fast error delivered through the
// typed callback with no HTTP request sent.
//
// The five public shims wrap `cb_submit_private` from coinbase_rest.c.
// Request bodies (POST /orders) are rendered with json-c and handed to
// the request context; they are freed in cb_req_release once the curl
// pipeline is done with them.
#define CB_INTERNAL
#include "coinbase.h"

#include "curl.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

// Common HTTP error sentinel for missing credentials. Used by every
// public entry point here before touching any HTTP state.
#define CB_ERR_NO_CREDS \
  "Error: Coinbase credentials not configured " \
  "(plugin.coinbase.apikey / apisecret / passphrase)"

// JSON specs

// The server echoes enum-like values as plain strings, numeric fields
// as quoted strings, and booleans as JSON booleans. created_at is an
// ISO 8601 string — left unpopulated for now; callers needing a ms
// timestamp should parse it themselves. Everything non-required: the
// spec tolerates a cancel-ack shape (bare order_id string → we fall
// through and populate just order_id manually).
static const json_spec_t cb_order_spec[] = {
  { JSON_STR,        "id",             false,
      offsetof(coinbase_order_t, order_id),
      .len = sizeof(((coinbase_order_t *)0)->order_id) },
  { JSON_STR,        "client_oid",     false,
      offsetof(coinbase_order_t, client_oid),
      .len = sizeof(((coinbase_order_t *)0)->client_oid) },
  { JSON_STR,        "product_id",     false,
      offsetof(coinbase_order_t, product_id),
      .len = sizeof(((coinbase_order_t *)0)->product_id) },
  { JSON_STR,        "side",           false,
      offsetof(coinbase_order_t, side),
      .len = sizeof(((coinbase_order_t *)0)->side) },
  { JSON_STR,        "type",           false,
      offsetof(coinbase_order_t, type),
      .len = sizeof(((coinbase_order_t *)0)->type) },
  { JSON_STR,        "status",         false,
      offsetof(coinbase_order_t, status),
      .len = sizeof(((coinbase_order_t *)0)->status) },
  { JSON_STR,        "time_in_force",  false,
      offsetof(coinbase_order_t, tif),
      .len = sizeof(((coinbase_order_t *)0)->tif) },
  { JSON_DOUBLE_STR, "price",          false,
      offsetof(coinbase_order_t, price) },
  { JSON_DOUBLE_STR, "size",           false,
      offsetof(coinbase_order_t, size) },
  { JSON_DOUBLE_STR, "filled_size",    false,
      offsetof(coinbase_order_t, filled_size) },
  { JSON_DOUBLE_STR, "executed_value", false,
      offsetof(coinbase_order_t, executed_value) },
  { JSON_DOUBLE_STR, "fill_fees",      false,
      offsetof(coinbase_order_t, fill_fees) },
  { JSON_BOOL,       "post_only",      false,
      offsetof(coinbase_order_t, post_only) },
  { JSON_BOOL,       "settled",        false,
      offsetof(coinbase_order_t, settled) },
  { JSON_END }
};

static const json_spec_t cb_account_spec[] = {
  { JSON_STR,        "currency",  true,
      offsetof(coinbase_account_t, currency),
      .len = sizeof(((coinbase_account_t *)0)->currency) },
  { JSON_DOUBLE_STR, "balance",   false,
      offsetof(coinbase_account_t, balance) },
  { JSON_DOUBLE_STR, "hold",      false,
      offsetof(coinbase_account_t, hold) },
  { JSON_DOUBLE_STR, "available", false,
      offsetof(coinbase_account_t, available) },
  { JSON_END }
};

// Delivery helpers (failure path)

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

// POST /orders body render
//
// Validates the request shape client-side (post_only on a market order,
// market-sell with no size, etc.) and writes a mem_alloc'd JSON string
// to *out_body. Returns NULL on validation failure with a descriptive
// message in err_out.

static char *
cb_render_order_body(const coinbase_place_order_req_t *req,
    size_t *out_len, char *err_out, size_t err_cap)
{
  struct json_object *root;
  const char         *json_text;
  char               *out;
  size_t              len;
  bool                is_market;
  bool                is_limit;

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

  if(is_market && req->post_only)
  {
    snprintf(err_out, err_cap,
        "Error: post_only is invalid on market orders");
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

  if(is_market)
  {
    // Market-buy accepts either `funds` (quote) or `size` (base); market-
    // sell requires `size`. Require at least one for buy, `size` for sell.
    if(strcmp(req->side, "buy") == 0)
    {
      if(req->funds <= 0.0 && req->size <= 0.0)
      {
        snprintf(err_out, err_cap,
            "Error: market-buy requires funds or size");
        return(NULL);
      }
    }
    else if(strcmp(req->side, "sell") == 0)
    {
      if(req->size <= 0.0)
      {
        snprintf(err_out, err_cap,
            "Error: market-sell requires a positive size");
        return(NULL);
      }
    }
    else
    {
      snprintf(err_out, err_cap,
          "Error: unknown side '%s' (use 'buy' or 'sell')", req->side);
      return(NULL);
    }
  }

  root = json_object_new_object();

  if(root == NULL)
  {
    snprintf(err_out, err_cap, "Error: json_object alloc failed");
    return(NULL);
  }

  json_object_object_add(root, "product_id",
      json_object_new_string(req->product_id));
  json_object_object_add(root, "side",
      json_object_new_string(req->side));
  json_object_object_add(root, "type",
      json_object_new_string(req->type));

  if(req->client_oid[0] != '\0')
    json_object_object_add(root, "client_oid",
        json_object_new_string(req->client_oid));

  if(is_limit)
  {
    json_object_object_add(root, "price",
        json_object_new_double(req->price));
    json_object_object_add(root, "size",
        json_object_new_double(req->size));

    if(req->tif[0] != '\0')
      json_object_object_add(root, "time_in_force",
          json_object_new_string(req->tif));

    if(req->post_only)
      json_object_object_add(root, "post_only",
          json_object_new_boolean(1));
  }
  else
  {
    // market: emit exactly one of funds / size. For buy, prefer funds
    // when both are set (quote-currency semantics); for sell, size only.
    if(strcmp(req->side, "buy") == 0 && req->funds > 0.0)
      json_object_object_add(root, "funds",
          json_object_new_double(req->funds));
    else
      json_object_object_add(root, "size",
          json_object_new_double(req->size));
  }

  json_text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

  if(json_text == NULL)
  {
    snprintf(err_out, err_cap, "Error: json_object_to_string failed");
    json_object_put(root);
    return(NULL);
  }

  len = strlen(json_text);
  out = mem_alloc(CB_CTX, "order_body", len + 1);
  memcpy(out, json_text, len + 1);

  json_object_put(root);

  *out_len = len;
  return(out);
}

// Response parsing helpers

static bool
cb_parse_order(struct json_object *obj, coinbase_order_t *out)
{
  if(obj == NULL || out == NULL) return(false);

  memset(out, 0, sizeof(*out));
  json_extract(obj, out, cb_order_spec, CB_CTX ":order");

  return(out->order_id[0] != '\0'
      || out->product_id[0] != '\0'
      || out->status[0] != '\0');
}

// Curl completion: orders (single-order shape — place / get / cancel)
//
// Cancel returns the bare string id rather than an order object; we
// detect that shape up-front and populate only order_id. place / get
// return a full object that the spec walks.

static void
cb_order_done(const curl_response_t *resp)
{
  cb_request_t            *r = (cb_request_t *)resp->user_data;
  coinbase_order_result_t  res = { 0 };
  char                     errbuf[CB_ERR_SZ];
  const char              *err;
  struct json_object      *root;

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

  if(json_object_is_type(root, json_type_string))
  {
    // DELETE /orders/{id} acks with a bare string.
    const char *id = json_object_get_string(root);

    snprintf(res.order.order_id, sizeof(res.order.order_id), "%s",
        id != NULL ? id : "");
  }
  else if(json_object_is_type(root, json_type_object))
  {
    cb_parse_order(root, &res.order);
  }
  else
  {
    json_object_put(root);
    cb_deliver_order_fail(r,
        "Error: unexpected Coinbase order response shape");
    return;
  }

  clam(CLAM_DEBUG2, CB_CTX,
       "order done: id='%s' status='%s' type=%d",
       res.order.order_id, res.order.status, (int)r->type);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Curl completion: list-orders (array of order objects)

static void
cb_orders_list_done(const curl_response_t *resp)
{
  cb_request_t             *r = (cb_request_t *)resp->user_data;
  coinbase_orders_result_t  res = { 0 };
  char                      errbuf[CB_ERR_SZ];
  const char               *err;
  struct json_object       *root;
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

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    cb_deliver_orders_fail(r,
        "Error: unexpected Coinbase orders list shape");
    return;
  }

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < COINBASE_MAX_ORDERS_LIST; i++)
  {
    struct json_object *item = json_object_array_get_idx(root, i);

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

// Curl completion: accounts (array of account objects)

static void
cb_accounts_done(const curl_response_t *resp)
{
  cb_request_t               *r = (cb_request_t *)resp->user_data;
  coinbase_accounts_result_t  res = { 0 };
  char                        errbuf[CB_ERR_SZ];
  const char                 *err;
  struct json_object         *root;
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

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    cb_deliver_accounts_fail(r,
        "Error: unexpected Coinbase accounts response shape");
    return;
  }

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < rows_cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(root, i);

    if(item == NULL)
      continue;

    memset(&res.rows[kept], 0, sizeof(res.rows[kept]));

    if(json_extract(item, &res.rows[kept],
        cb_account_spec, CB_CTX ":account"))
      kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, CB_CTX, "accounts: %u currency row(s)", kept);

  if(r->cb.accounts != NULL)
    r->cb.accounts(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Public API

bool
coinbase_place_order_async(const coinbase_place_order_req_t *req,
    coinbase_done_order_cb_t cb, void *user)
{
  cb_request_t *r;
  char         *body;
  size_t        body_len = 0;
  char          errbuf[CB_ERR_SZ];

  if(req == NULL)
    return(FAIL);

  r = cb_req_alloc();
  r->type     = CB_REQ_PLACE_ORDER;
  r->cb.order = cb;
  r->user     = user;

  if(!cb_apikey_configured())
  {
    cb_deliver_order_fail(r, CB_ERR_NO_CREDS);
    return(FAIL);
  }

  body = cb_render_order_body(req, &body_len, errbuf, sizeof(errbuf));

  if(body == NULL)
  {
    cb_deliver_order_fail(r, errbuf);
    return(FAIL);
  }

  // Transfer body ownership to the request — cb_req_release frees it.
  r->body     = body;
  r->body_len = body_len;

  if(cb_submit_private(r, CURL_METHOD_POST, "/orders",
        body, body_len, cb_order_done) != SUCCESS)
  {
    cb_deliver_order_fail(r,
        "Error: failed to submit Coinbase place-order request");
    return(FAIL);
  }

  return(SUCCESS);
}

bool
coinbase_cancel_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;

  if(order_id == NULL || order_id[0] == '\0')
    return(FAIL);

  r = cb_req_alloc();
  r->type     = CB_REQ_CANCEL_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!cb_apikey_configured())
  {
    cb_deliver_order_fail(r, CB_ERR_NO_CREDS);
    return(FAIL);
  }

  n = snprintf(path, sizeof(path), "/orders/%s", order_id);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    cb_deliver_order_fail(r, "Error: order_id too long");
    return(FAIL);
  }

  if(cb_submit_private(r, CURL_METHOD_DELETE, path,
        NULL, 0, cb_order_done) != SUCCESS)
  {
    cb_deliver_order_fail(r,
        "Error: failed to submit Coinbase cancel-order request");
    return(FAIL);
  }

  return(SUCCESS);
}

bool
coinbase_get_order_async(const char *order_id,
    coinbase_done_order_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;

  if(order_id == NULL || order_id[0] == '\0')
    return(FAIL);

  r = cb_req_alloc();
  r->type     = CB_REQ_GET_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!cb_apikey_configured())
  {
    cb_deliver_order_fail(r, CB_ERR_NO_CREDS);
    return(FAIL);
  }

  n = snprintf(path, sizeof(path), "/orders/%s", order_id);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    cb_deliver_order_fail(r, "Error: order_id too long");
    return(FAIL);
  }

  if(cb_submit_private(r, CURL_METHOD_GET, path,
        NULL, 0, cb_order_done) != SUCCESS)
  {
    cb_deliver_order_fail(r,
        "Error: failed to submit Coinbase get-order request");
    return(FAIL);
  }

  return(SUCCESS);
}

bool
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
    return(FAIL);
  }

  // Build the query string incrementally to keep NULL-selector branches
  // simple. COINBASE_MAX_ORDERS_LIST is also Coinbase's server-side cap
  // for a single unpaginated page.
  n = snprintf(path, sizeof(path), "/orders");

  if(status != NULL && status[0] != '\0')
  {
    int m = snprintf(path + n, sizeof(path) - (size_t)n,
        "%sstatus=%s", sep, status);
    if(m < 0 || (size_t)m >= sizeof(path) - (size_t)n)
    {
      cb_deliver_orders_fail(r, "Error: orders query too long");
      return(FAIL);
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
      return(FAIL);
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
      return(FAIL);
    }
  }

  if(cb_submit_private(r, CURL_METHOD_GET, path,
        NULL, 0, cb_orders_list_done) != SUCCESS)
  {
    cb_deliver_orders_fail(r,
        "Error: failed to submit Coinbase list-orders request");
    return(FAIL);
  }

  return(SUCCESS);
}

bool
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
    return(FAIL);
  }

  if(cb_submit_private(r, CURL_METHOD_GET, "/accounts",
        NULL, 0, cb_accounts_done) != SUCCESS)
  {
    cb_deliver_accounts_fail(r,
        "Error: failed to submit Coinbase accounts request");
    return(FAIL);
  }

  return(SUCCESS);
}
