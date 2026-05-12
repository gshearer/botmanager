// botmanager — MIT
// Coinbase Advanced Trade: REST mechanism + freelist.
//
// Public Market Data (candles) lands here on top of the shared curl
// subsystem; results flow back through typed completion callbacks.
// Authenticated endpoints (orders, accounts, fills) live in
// coinbase_orders.c and reuse the request context + freelist declared
// here. Legacy Exchange (`api.exchange.coinbase.com`,
// `/products/{id}/...`) is gone — `feedback_no_deprecated_code.md`
// applies; recovery path is git log.
//
// EX-1 routes candle traffic through the feature_exchange abstraction:
// coinbase_fetch_candles_async builds the path, allocates a typed
// completion context, and dispatches through exchange_request. The
// abstraction's response callback (cb_candles_exchange_resp below)
// parses the body and fires the user's typed cb.
#define CB_INTERNAL
#include "coinbase.h"
#include "exchange_api.h"

#include "curl.h"
#include "json.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// Endpoint paths (relative to the REST base URL selected by
// cb_rest_base_url). Kept here rather than in coinbase.h because no
// other TU consumes them.
#define CB_PATH_CANDLES_FMT      "/api/v3/brokerage/market/products/%s/candles"

// Coinbase Advanced Trade granularity is a named enum rather than a
// raw second count. Map the seconds the rest of the codebase uses
// (dl_candles.c's ladder: 60 / 300 / 900 / 3600 / 21600 / 86400) onto
// the wire string. Returns SUCCESS on a known value + FAIL otherwise;
// caller surfaces a clear error rather than letting the server 400.
static bool
cb_gran_seconds_to_at_name(int32_t seconds, char *out, size_t cap)
{
  const char *name;

  if(out == NULL || cap == 0)
    return(FAIL);

  switch(seconds)
  {
    case 60:    name = "ONE_MINUTE";     break;
    case 300:   name = "FIVE_MINUTE";    break;
    case 900:   name = "FIFTEEN_MINUTE"; break;
    case 1800:  name = "THIRTY_MINUTE";  break;
    case 3600:  name = "ONE_HOUR";       break;
    case 7200:  name = "TWO_HOUR";       break;
    case 21600: name = "SIX_HOUR";       break;
    case 86400: name = "ONE_DAY";        break;
    default:    return(FAIL);
  }

  if(strlen(name) >= cap)
    return(FAIL);

  snprintf(out, cap, "%s", name);
  return(SUCCESS);
}

// Module state

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
cb_deliver_candles_fail(cb_request_t *r, const char *err)
{
  coinbase_candles_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  cb_req_release(r);
}

// HTTP submit helper (public / unauthenticated). Promoted from a file-
// scope static in EX-1 because the exchange-vtable build_request /
// submit handlers in coinbase_exchange.c also need it. The candles path
// routes through the exchange abstraction directly (see
// coinbase_fetch_candles_async below); cb_submit_public exists for the
// abstraction's own use.
//
// Builds a GET against base + path, attaches the standard Accept header,
// and submits through the curl subsystem. On any build/submit failure
// the request is released and no callback fires.

bool
cb_submit_public(void *user_data, uint8_t prio, const char *path,
    curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[CB_URL_SZ];
  char            url [CB_URL_SZ];
  int             n;

  if(cb_rest_base_url(base, sizeof(base)) != SUCCESS)
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

  cr = curl_request_create(CURL_METHOD_GET, url, done_cb, user_data);

  if(cr == NULL)
  {
    clam(CLAM_WARN, CB_CTX, "submit: curl_request_create failed url='%s'",
         url);
    return(FAIL);
  }

  curl_request_add_header(cr, "Accept: application/json");
  (void)curl_request_set_prio(cr, (curl_prio_t)prio);

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
// Coinbase Advanced Trade signs requests with a CDP JWT carried in the
// `Authorization: Bearer …` header; the body is attached unchanged
// (verbatim bytes) as application/json.
//
// WM-LT-8-A: `user_data` is echoed back to `done_cb` via
// `curl_response_t::user_data`. Legacy typed callers pass their
// `cb_request_t *`; the exchange-vtable path passes its own handle.
bool
cb_submit_private(void *user_data, uint8_t prio,
    curl_method_t method, const char *path,
    const char *body, size_t body_len,
    curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[CB_URL_SZ];
  char            url [CB_URL_SZ];
  char            jwt [CB_JWT_SZ];
  char            auth_hdr[CB_JWT_SZ + 32];
  int             n;

  if(!cb_apikey_configured())
    return(FAIL);

  if(cb_rest_base_url(base, sizeof(base)) != SUCCESS)
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

  if(cb_sign_jwt(curl_method_name(method), path,
        jwt, sizeof(jwt)) != SUCCESS)
  {
    clam(CLAM_WARN, CB_CTX,
         "private submit: cdp sign failed method=%s path='%s'",
         curl_method_name(method), path);
    return(FAIL);
  }

  cr = curl_request_create(method, url, done_cb, user_data);

  if(cr == NULL)
  {
    clam(CLAM_WARN, CB_CTX,
         "private submit: curl_request_create failed url='%s'", url);
    return(FAIL);
  }

  (void)curl_request_set_prio(cr, (curl_prio_t)prio);

  snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", jwt);
  curl_request_add_header(cr, auth_hdr);
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

// Exchange-routed completion: candles
//
// Called by the feature_exchange abstraction once the underlying curl
// transport has succeeded (2xx) or has exhausted retry. The abstraction
// has already applied 429/5xx retry policy and translated transport
// errors into a non-NULL `err` string with http_status==0.
//
// Coinbase Advanced Trade returns:
//   { "candles": [
//       { "start":"<unix-secs>", "low":"...", "high":"...",
//         "open":"...", "close":"...", "volume":"..." }, ...
//     ] }
// Every field is a string. Newest-first by `start`.
//
// One quirk we paper over for downstream consumers: Advanced Trade
// drops candle objects whose bucket has zero matched volume rather
// than returning them with volume=0. The dl_candles page loop checks
// for sparse results and treats them as honest coverage; the bar
// aggregator already tolerates absent buckets via its grain rings.

static double
cb_json_str_double(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *s;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0.0);

  // Advanced Trade serializes numerics as JSON strings to preserve
  // precision; tolerate the legacy bare-number form too in case the
  // server toggles representation.
  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    return(s != NULL ? strtod(s, NULL) : 0.0);
  }

  return(json_object_get_double(v));
}

static int64_t
cb_json_str_int64(struct json_object *obj, const char *key)
{
  struct json_object *v;
  const char         *s;

  if(obj == NULL || !json_object_object_get_ex(obj, key, &v))
    return(0);

  if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);
    return(s != NULL ? (int64_t)strtoll(s, NULL, 10) : 0);
  }

  return((int64_t)json_object_get_int64(v));
}

static void
cb_candles_exchange_resp(int http_status, const char *body, size_t body_len,
    const char *err, void *user)
{
  cb_request_t              *r = user;
  coinbase_candles_result_t  res = { 0 };
  struct json_object        *root;
  struct json_object        *candles;
  int                        len;
  uint32_t                   kept = 0;

  (void)http_status;

  if(r == NULL)
    return;

  if(err != NULL)
  {
    cb_deliver_candles_fail(r, err);
    return;
  }

  root = json_parse_buf(body, body_len, CB_CTX);

  if(root == NULL)
  {
    cb_deliver_candles_fail(r,
        "Error: malformed JSON from Coinbase candles");
    return;
  }

  if(!json_object_is_type(root, json_type_object) ||
     !json_object_object_get_ex(root, "candles", &candles) ||
     !json_object_is_type(candles, json_type_array))
  {
    json_object_put(root);
    cb_deliver_candles_fail(r,
        "Error: unexpected Coinbase candles response format");
    return;
  }

  len = (int)json_object_array_length(candles);

  for(int i = 0; i < len && kept < COINBASE_MAX_CANDLES; i++)
  {
    struct json_object *row = json_object_array_get_idx(candles, i);
    coinbase_candle_t  *c   = &res.rows[kept];

    if(row == NULL || !json_object_is_type(row, json_type_object))
      continue;

    c->time   = cb_json_str_int64 (row, "start");
    c->low    = cb_json_str_double(row, "low");
    c->high   = cb_json_str_double(row, "high");
    c->open   = cb_json_str_double(row, "open");
    c->close  = cb_json_str_double(row, "close");
    c->volume = cb_json_str_double(row, "volume");

    if(c->time <= 0)
      continue;     // skip malformed rows

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, CB_CTX, "candles: %s granularity=%d -> %u row(s)",
       r->product_id, (int)r->granularity, kept);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  json_object_put(root);
  cb_req_release(r);
}

// Public API

bool
coinbase_fetch_candles_async(const char *product_id, int32_t granularity,
    int64_t start_ts, int64_t end_ts, uint8_t prio,
    coinbase_done_candles_cb_t cb, void *user)
{
  cb_request_t *r;
  char          path[CB_URL_SZ];
  char          gran_name[24];
  int           n;

  if(product_id == NULL || product_id[0] == '\0' || granularity <= 0)
    return(FAIL);

  if(cb_gran_seconds_to_at_name(granularity, gran_name,
        sizeof(gran_name)) != SUCCESS)
  {
    r = cb_req_alloc();
    r->type       = CB_REQ_CANDLES;
    r->cb.candles = cb;
    r->user       = user;
    cb_deliver_candles_fail(r,
        "Error: unsupported granularity for Coinbase Advanced Trade");
    return(FAIL);
  }

  // Bucket budget: Coinbase Advanced Trade caps at 350 candles per
  // call (was 300 on the retired Exchange API). Stay at the legacy
  // cap so existing callers' page-walk arithmetic
  // (WM_DL_CANDLE_WINDOW_BUCKETS) stays exactly on the boundary;
  // bumping is a separate change.
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
        CB_PATH_CANDLES_FMT "?start=%lld&end=%lld&granularity=%s",
        product_id, (long long)start_ts, (long long)end_ts, gran_name);
  else
    n = snprintf(path, sizeof(path),
        CB_PATH_CANDLES_FMT "?granularity=%s",
        product_id, gran_name);

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

  // Route through the exchange abstraction. The vtable's build_request
  // / submit owns the curl-level handle; cb_candles_exchange_resp
  // parses the body and fires r->cb.candles.
  if(exchange_request("coinbase", prio,
        EXCHANGE_OP_REST_GET, path, NULL,
        cb_candles_exchange_resp, r) != SUCCESS)
  {
    cb_deliver_candles_fail(r,
        "Error: failed to submit Coinbase candles request");
    return(FAIL);
  }

  return(SUCCESS);
}

// Lifecycle

void
cb_rest_init(void)
{
  pthread_mutex_init(&cb_req_mu, NULL);
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
}
