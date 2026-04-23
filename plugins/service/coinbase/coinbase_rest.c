// botmanager — MIT
// Coinbase Exchange: unauthenticated REST mechanism.
//
// Lands the public market-data surface — products, candles, ticker —
// on top of the shared curl subsystem. Results flow back to the caller
// through typed completion callbacks; a product cache short-circuits
// repeat reads (pattern mirrors plugins/service/coinmarketcap).
//
// Authenticated endpoints (orders, accounts) arrive in CB3 and reuse
// the request context + freelist declared here.
#define CB_INTERNAL
#include "coinbase.h"

#include "curl.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Endpoint paths (relative to the REST base URL selected by
// cb_rest_base_url). Kept here rather than in coinbase.h because no
// other TU consumes them.
#define CB_PATH_PRODUCTS  "/products"

// Module state

static coinbase_product_t  cb_products_cache[CB_MAX_PRODUCTS];
static uint32_t            cb_products_count = 0;
static time_t              cb_products_time  = 0;
static pthread_rwlock_t    cb_products_rwl;

static cb_request_t       *cb_req_free = NULL;
static pthread_mutex_t     cb_req_mu;

// Freelist helpers. Shared with coinbase_orders.c via coinbase.h.

cb_request_t *
cb_req_alloc(void)
{
  cb_request_t *r = NULL;

  pthread_mutex_lock(&cb_req_mu);

  if(cb_req_free != NULL)
  {
    r = cb_req_free;
    cb_req_free = r->next;
  }

  pthread_mutex_unlock(&cb_req_mu);

  if(r == NULL)
    r = mem_alloc(CB_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

void
cb_req_release(cb_request_t *r)
{
  if(r->body != NULL)
  {
    mem_free(r->body);
    r->body     = NULL;
    r->body_len = 0;
  }

  pthread_mutex_lock(&cb_req_mu);
  r->next = cb_req_free;
  cb_req_free = r;
  pthread_mutex_unlock(&cb_req_mu);
}

// Cache helpers

static bool
cb_products_cache_valid(void)
{
  uint32_t ttl;

  if(cb_products_count == 0)
    return(false);

  ttl = (uint32_t)kv_get_uint("plugin.coinbase.cache_ttl");

  if(ttl == 0)
    ttl = 5;

  return((time(NULL) - cb_products_time) < (time_t)ttl);
}

// JSON specs

// Coinbase returns the numeric fields as quoted strings (e.g.
// "base_min_size": "0.0001"), so JSON_DOUBLE_STR is the right codec.
static const json_spec_t cb_product_spec[] = {
  { JSON_STR,        "id",             true,
      offsetof(coinbase_product_t, product_id),
      .len = sizeof(((coinbase_product_t *)0)->product_id) },
  { JSON_STR,        "base_currency",  false,
      offsetof(coinbase_product_t, base_currency),
      .len = sizeof(((coinbase_product_t *)0)->base_currency) },
  { JSON_STR,        "quote_currency", false,
      offsetof(coinbase_product_t, quote_currency),
      .len = sizeof(((coinbase_product_t *)0)->quote_currency) },
  { JSON_DOUBLE_STR, "base_min_size",   false,
      offsetof(coinbase_product_t, base_min_size) },
  { JSON_DOUBLE_STR, "base_max_size",   false,
      offsetof(coinbase_product_t, base_max_size) },
  { JSON_DOUBLE_STR, "quote_increment", false,
      offsetof(coinbase_product_t, quote_increment) },
  { JSON_DOUBLE_STR, "base_increment",  false,
      offsetof(coinbase_product_t, base_increment) },
  { JSON_BOOL,       "trading_disabled", false,
      offsetof(coinbase_product_t, trading_disabled) },
  { JSON_BOOL,       "cancel_only",      false,
      offsetof(coinbase_product_t, cancel_only) },
  { JSON_BOOL,       "post_only",        false,
      offsetof(coinbase_product_t, post_only) },
  { JSON_BOOL,       "limit_only",       false,
      offsetof(coinbase_product_t, limit_only) },
  { JSON_END }
};

// Ticker response shape (GET /products/{id}/ticker). Returns the latest
// trade and 24h volume. `volume` is the rolling 24h base-currency volume
// — map it into volume_24h. low_24h/high_24h are not included here;
// consumers needing them should fetch /products/{id}/stats. time is an
// ISO 8601 string but the caller only gets time_ms on success below —
// parsed explicitly so JSON_STR avoids an extra scratch buffer.
static const json_spec_t cb_ticker_spec[] = {
  { JSON_DOUBLE_STR, "price",  false,
      offsetof(coinbase_ticker_t, price) },
  { JSON_DOUBLE_STR, "bid",    false,
      offsetof(coinbase_ticker_t, bid) },
  { JSON_DOUBLE_STR, "ask",    false,
      offsetof(coinbase_ticker_t, ask) },
  { JSON_DOUBLE_STR, "volume", false,
      offsetof(coinbase_ticker_t, volume_24h) },
  { JSON_END }
};

// HTTP classification. Shared with coinbase_orders.c via coinbase.h.

const char *
cb_classify_http(const curl_response_t *resp, char *buf, size_t sz)
{
  if(resp->curl_code != 0)
  {
    snprintf(buf, sz, "Coinbase API error: %s",
        resp->error != NULL ? resp->error : "transport error");
    return(buf);
  }

  switch(resp->status)
  {
    case 200:
      return(NULL);

    case 400:
      return("Error: bad request to Coinbase API");

    case 401:
      return("Error: invalid Coinbase API credentials");

    case 403:
      return("Error: Coinbase API key lacks required permission");

    case 404:
      return("Error: Coinbase resource not found");

    case 429:
      return("Error: Coinbase API rate limit exceeded, try again later");

    case 500:
    case 502:
    case 503:
    case 504:
      snprintf(buf, sz,
          "Coinbase API backend error (HTTP %ld), retryable",
          resp->status);
      return(buf);

    default:
      snprintf(buf, sz, "Coinbase API returned HTTP %ld", resp->status);
      return(buf);
  }
}

// Delivery helpers (failure path)

static void
cb_deliver_products_fail(cb_request_t *r, const char *err)
{
  coinbase_products_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.products != NULL)
    r->cb.products(&res, r->user);

  cb_req_release(r);
}

static void
cb_deliver_candles_fail(cb_request_t *r, const char *err)
{
  coinbase_candles_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  cb_req_release(r);
}

static void
cb_deliver_ticker_fail(cb_request_t *r, const char *err)
{
  coinbase_ticker_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.ticker != NULL)
    r->cb.ticker(&res, r->user);

  cb_req_release(r);
}

// HTTP submit helper (public / unauthenticated)
//
// Builds a GET against base + path, attaches the standard Accept header,
// and submits through the curl subsystem. On any build/submit failure
// the request is released and no callback fires (caller invokes the
// matching cb_deliver_*_fail on the FAIL return path below).

static bool
cb_submit_public(cb_request_t *req, const char *path, curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[CB_URL_SZ];
  char            url [CB_URL_SZ];
  int             n;

  if(!cb_rest_base_url(base, sizeof(base)))
  {
    clam(CLAM_WARN, CB_CTX, "submit: REST base URL not configured");
    return(FAIL);
  }

  n = snprintf(url, sizeof(url), "%s%s", base, path);

  if(n < 0 || (size_t)n >= sizeof(url))
  {
    clam(CLAM_WARN, CB_CTX, "submit: URL overflow path='%s'", path);
    return(FAIL);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, done_cb, req);

  if(cr == NULL)
  {
    clam(CLAM_WARN, CB_CTX, "submit: curl_request_create failed url='%s'",
         url);
    return(FAIL);
  }

  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    clam(CLAM_WARN, CB_CTX, "submit: curl_request_submit failed url='%s'",
         url);
    return(FAIL);
  }

  return(SUCCESS);
}

// Build, sign, and submit an authenticated REST request.
//
// Coinbase Exchange signs the string `ts + METHOD + requestPath + body`
// where `requestPath` is the full path including any query string. The
// body is attached unchanged (verbatim bytes) as application/json.
bool
cb_submit_private(cb_request_t *req, curl_method_t method,
    const char *path, const char *body, size_t body_len,
    curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[CB_URL_SZ];
  char            url [CB_URL_SZ];
  char            ts  [CB_TS_SZ];
  char            sig [CB_SIG_SZ];
  char            hdr [CB_URL_SZ];
  const char     *apikey;
  const char     *passphrase;
  int             n;

  if(!cb_apikey_configured())
    return(FAIL);

  if(!cb_rest_base_url(base, sizeof(base)))
  {
    clam(CLAM_WARN, CB_CTX, "private submit: REST base URL not configured");
    return(FAIL);
  }

  n = snprintf(url, sizeof(url), "%s%s", base, path);

  if(n < 0 || (size_t)n >= sizeof(url))
  {
    clam(CLAM_WARN, CB_CTX, "private submit: URL overflow path='%s'",
         path);
    return(FAIL);
  }

  if(cb_timestamp_str(ts, sizeof(ts)) == 0)
    return(FAIL);

  if(!cb_sign_request(curl_method_name(method), path,
        body, body_len, ts, sig, sizeof(sig)))
  {
    clam(CLAM_WARN, CB_CTX,
         "private submit: signing failed method=%s path='%s'",
         curl_method_name(method), path);
    return(FAIL);
  }

  cr = curl_request_create(method, url, done_cb, req);

  if(cr == NULL)
  {
    clam(CLAM_WARN, CB_CTX,
         "private submit: curl_request_create failed url='%s'", url);
    return(FAIL);
  }

  apikey     = kv_get_str("plugin.coinbase.apikey");
  passphrase = kv_get_str("plugin.coinbase.passphrase");

  snprintf(hdr, sizeof(hdr), "CB-ACCESS-KEY: %s",
      apikey != NULL ? apikey : "");
  curl_request_add_header(cr, hdr);

  snprintf(hdr, sizeof(hdr), "CB-ACCESS-SIGN: %s", sig);
  curl_request_add_header(cr, hdr);

  snprintf(hdr, sizeof(hdr), "CB-ACCESS-TIMESTAMP: %s", ts);
  curl_request_add_header(cr, hdr);

  snprintf(hdr, sizeof(hdr), "CB-ACCESS-PASSPHRASE: %s",
      passphrase != NULL ? passphrase : "");
  curl_request_add_header(cr, hdr);

  curl_request_add_header(cr, "Accept: application/json");

  if(body != NULL && body_len > 0)
    curl_request_set_body(cr, "application/json", body, body_len);

  if(curl_request_submit(cr) != SUCCESS)
  {
    clam(CLAM_WARN, CB_CTX,
         "private submit: curl_request_submit failed url='%s'", url);
    return(FAIL);
  }

  return(SUCCESS);
}

// Curl completion: products

static void
cb_products_done(const curl_response_t *resp)
{
  cb_request_t               *r = (cb_request_t *)resp->user_data;
  coinbase_products_result_t  res = { 0 };
  char                        errbuf[CB_ERR_SZ];
  const char                 *err;
  struct json_object         *root;
  int                         len;
  int                         kept = 0;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_products_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_products_fail(r,
        "Error: malformed JSON from Coinbase products");
    return;
  }

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    cb_deliver_products_fail(r,
        "Error: unexpected Coinbase products response format");
    return;
  }

  len = (int)json_object_array_length(root);

  pthread_rwlock_wrlock(&cb_products_rwl);

  memset(cb_products_cache, 0, sizeof(cb_products_cache));

  for(int i = 0; i < len && kept < CB_MAX_PRODUCTS; i++)
  {
    struct json_object *item = json_object_array_get_idx(root, i);

    if(item == NULL)
      continue;

    if(!json_extract(item, &cb_products_cache[kept],
        cb_product_spec, CB_CTX ":product"))
      continue;

    kept++;
  }

  cb_products_count = (uint32_t)kept;
  cb_products_time  = time(NULL);

  pthread_rwlock_unlock(&cb_products_rwl);

  if(len > CB_MAX_PRODUCTS)
    clam(CLAM_WARN, CB_CTX,
         "products: response truncated from %d to %d (raise CB_MAX_PRODUCTS)",
         len, CB_MAX_PRODUCTS);

  clam(CLAM_DEBUG2, CB_CTX, "products: cached %d row(s)", kept);

  if(r->cb.products != NULL)
    r->cb.products(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Curl completion: candles
//
// Coinbase returns a bare array of arrays:
//   [[time, low, high, open, close, volume], …]
// Positional decode by index; we stop at COINBASE_MAX_CANDLES.

static void
cb_candles_done(const curl_response_t *resp)
{
  cb_request_t              *r = (cb_request_t *)resp->user_data;
  coinbase_candles_result_t  res = { 0 };
  char                       errbuf[CB_ERR_SZ];
  const char                *err;
  struct json_object        *root;
  int                        len;
  uint32_t                   kept = 0;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_candles_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_candles_fail(r,
        "Error: malformed JSON from Coinbase candles");
    return;
  }

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    cb_deliver_candles_fail(r,
        "Error: unexpected Coinbase candles response format");
    return;
  }

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < COINBASE_MAX_CANDLES; i++)
  {
    struct json_object *row = json_object_array_get_idx(root, i);
    struct json_object *e;
    coinbase_candle_t  *c   = &res.rows[kept];

    if(row == NULL || !json_object_is_type(row, json_type_array)
        || json_object_array_length(row) < 6)
      continue;

    e = json_object_array_get_idx(row, 0);
    c->time   = (int64_t)json_object_get_int64(e);
    e = json_object_array_get_idx(row, 1);
    c->low    = json_object_get_double(e);
    e = json_object_array_get_idx(row, 2);
    c->high   = json_object_get_double(e);
    e = json_object_array_get_idx(row, 3);
    c->open   = json_object_get_double(e);
    e = json_object_array_get_idx(row, 4);
    c->close  = json_object_get_double(e);
    e = json_object_array_get_idx(row, 5);
    c->volume = json_object_get_double(e);

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, CB_CTX, "candles: %s granularity=%d → %u row(s)",
       r->product_id, (int)r->granularity, kept);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Curl completion: ticker

static void
cb_ticker_done(const curl_response_t *resp)
{
  cb_request_t             *r = (cb_request_t *)resp->user_data;
  coinbase_ticker_result_t  res = { 0 };
  char                      errbuf[CB_ERR_SZ];
  const char               *err;
  struct json_object       *root;

  err = cb_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cb_deliver_ticker_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_ticker_fail(r,
        "Error: malformed JSON from Coinbase ticker");
    return;
  }

  if(!json_object_is_type(root, json_type_object))
  {
    json_object_put(root);
    cb_deliver_ticker_fail(r,
        "Error: unexpected Coinbase ticker response format");
    return;
  }

  snprintf(res.ticker.product_id, sizeof(res.ticker.product_id),
      "%s", r->product_id);

  json_extract(root, &res.ticker, cb_ticker_spec, CB_CTX ":ticker");

  clam(CLAM_DEBUG2, CB_CTX,
       "ticker: %s price=%.8f bid=%.8f ask=%.8f",
       r->product_id, res.ticker.price, res.ticker.bid, res.ticker.ask);

  if(r->cb.ticker != NULL)
    r->cb.ticker(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Public API

bool
coinbase_fetch_products_async(coinbase_done_products_cb_t cb, void *user)
{
  cb_request_t *r;

  r = cb_req_alloc();
  r->type        = CB_REQ_PRODUCTS;
  r->cb.products = cb;
  r->user        = user;

  if(cb_submit_public(r, CB_PATH_PRODUCTS, cb_products_done) != SUCCESS)
  {
    cb_deliver_products_fail(r,
        "Error: failed to submit Coinbase products request");
    return(FAIL);
  }

  return(SUCCESS);
}

bool
coinbase_fetch_candles_async(const char *product_id, int32_t granularity,
    int64_t start_ts, int64_t end_ts,
    coinbase_done_candles_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;

  if(product_id == NULL || product_id[0] == '\0' || granularity <= 0)
    return(FAIL);

  // Bucket budget: Coinbase caps at 300 candles per call. Reject
  // up-front when the implied range would blow the limit — otherwise
  // the server returns 400 and the caller gets a stale error message.
  if(start_ts > 0 && end_ts > 0 && end_ts > start_ts)
  {
    int64_t buckets = (end_ts - start_ts) / (int64_t)granularity;

    if(buckets > (int64_t)COINBASE_MAX_CANDLES)
    {
      r = cb_req_alloc();
      r->type       = CB_REQ_CANDLES;
      r->cb.candles = cb;
      r->user       = user;
      cb_deliver_candles_fail(r,
          "Error: requested candle range exceeds 300-bucket limit");
      return(FAIL);
    }
  }

  if(start_ts > 0 && end_ts > 0)
    n = snprintf(path, sizeof(path),
        "/products/%s/candles?granularity=%d&start=%lld&end=%lld",
        product_id, (int)granularity,
        (long long)start_ts, (long long)end_ts);
  else
    n = snprintf(path, sizeof(path),
        "/products/%s/candles?granularity=%d",
        product_id, (int)granularity);

  if(n < 0 || (size_t)n >= sizeof(path))
    return(FAIL);

  r = cb_req_alloc();
  r->type        = CB_REQ_CANDLES;
  r->granularity = granularity;
  r->start_ts    = start_ts;
  r->end_ts      = end_ts;
  r->cb.candles  = cb;
  r->user        = user;
  snprintf(r->product_id, sizeof(r->product_id), "%s", product_id);

  if(cb_submit_public(r, path, cb_candles_done) != SUCCESS)
  {
    cb_deliver_candles_fail(r,
        "Error: failed to submit Coinbase candles request");
    return(FAIL);
  }

  return(SUCCESS);
}

bool
coinbase_fetch_ticker_async(const char *product_id,
    coinbase_done_ticker_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  int           n;

  if(product_id == NULL || product_id[0] == '\0')
    return(FAIL);

  n = snprintf(path, sizeof(path), "/products/%s/ticker", product_id);

  if(n < 0 || (size_t)n >= sizeof(path))
    return(FAIL);

  r = cb_req_alloc();
  r->type       = CB_REQ_TICKER;
  r->cb.ticker  = cb;
  r->user       = user;
  snprintf(r->product_id, sizeof(r->product_id), "%s", product_id);

  if(cb_submit_public(r, path, cb_ticker_done) != SUCCESS)
  {
    cb_deliver_ticker_fail(r,
        "Error: failed to submit Coinbase ticker request");
    return(FAIL);
  }

  return(SUCCESS);
}

// Sync cache read

bool
coinbase_get_products(coinbase_product_t *out_arr, uint32_t out_cap,
    uint32_t *out_count)
{
  uint32_t n;

  if(out_arr == NULL || out_count == NULL || out_cap == 0)
    return(FAIL);

  *out_count = 0;

  pthread_rwlock_rdlock(&cb_products_rwl);

  if(cb_products_count == 0)
  {
    pthread_rwlock_unlock(&cb_products_rwl);
    return(FAIL);
  }

  n = cb_products_count;

  if(n > out_cap)
    n = out_cap;

  memcpy(out_arr, cb_products_cache, n * sizeof(coinbase_product_t));

  pthread_rwlock_unlock(&cb_products_rwl);

  *out_count = n;

  return(SUCCESS);
}

bool
coinbase_products_cache_fresh(void)
{
  bool fresh;

  pthread_rwlock_rdlock(&cb_products_rwl);
  fresh = cb_products_cache_valid();
  pthread_rwlock_unlock(&cb_products_rwl);

  return(fresh);
}

// Lifecycle

void
cb_rest_init(void)
{
  pthread_mutex_init(&cb_req_mu, NULL);
  pthread_rwlock_init(&cb_products_rwl, NULL);

  memset(cb_products_cache, 0, sizeof(cb_products_cache));
  cb_products_count = 0;
  cb_products_time  = 0;
}

void
cb_rest_deinit(void)
{
  pthread_mutex_lock(&cb_req_mu);

  while(cb_req_free != NULL)
  {
    cb_request_t *r = cb_req_free;

    cb_req_free = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&cb_req_mu);
  pthread_mutex_destroy(&cb_req_mu);

  pthread_rwlock_destroy(&cb_products_rwl);
}
