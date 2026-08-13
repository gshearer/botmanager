// botmanager — MIT
// Gemini Spot REST: typed wrappers over the gem_submit_* primitives.
//
// GEM-1 ships only the symbols-cache populator:
//
//   gemini_symbols_refresh_async — kicks off
//        GET /v1/symbols → list of native lowercase symbols, then for
//        each symbol GET /v1/symbols/details/<sym> → base + quote.
//        Aggregates results into gem_pairs and fires the caller cb
//        once after the last detail response lands.
//
// Why two stages: as of 2026-05 Gemini's `/v1/symbols/details`
// endpoint accepts a single symbol per call (no batch form). The
// payload structure is `{ "symbol":"btcusd", "base_currency":"BTC",
// "quote_currency":"USD", "tick_size":0.01, ... }`. The populator
// issues N detail requests in parallel through the exchange
// abstraction's priority queue, each fanning into the same batch ctx
// so the user gets exactly one callback regardless of partial
// failure.
//
// GEM-2 lands the other typed wrappers (candles, balances, new order,
// cancel order, order status, active orders, mytrades). Private
// endpoints submit via gem_submit_private directly (no exchange_request
// roundtrip — matches the Kraken pattern); their handlers therefore
// consume curl_response_t. Candles is the lone public endpoint and
// rides gem_submit_public for the same reason.
#define GEM_INTERNAL
#include "gemini.h"

#include "exchange_api.h"
#include "gemini_pairs.h"
#include "gemini_sign.h"
#include "json.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Endpoint paths. /v1/symbols returns the list; /v1/symbols/details/<s>
// returns the per-symbol detail. Both are public GETs — no signing.
#define GEM_PATH_SYMBOLS         "/v1/symbols"
#define GEM_PATH_SYMBOL_DETAILS  "/v1/symbols/details/"

// Cap on the number of in-flight detail requests we'll fire from a
// single refresh. Gemini lists ~150 spot pairs; GEM_SYMS_CAP gives
// headroom for growth, but we add a hard ceiling here so a parser
// glitch can't queue thousands of requests.
#define GEM_SYMBOLS_DETAIL_CAP   512

// ------------------------------------------------------------------
// Batch aggregator
//
// One batch_t per refresh. The first request (the /v1/symbols listing)
// holds a pointer to it; each per-symbol detail request also gets a
// pointer. As detail responses land we decrement `pending` under
// `lock`; when it hits zero we fire the user callback and free the
// batch.
//
// `total` is the number of detail requests we kicked off; it stays
// fixed once the listing parser finishes. `pending` is the number
// still in flight. `kept` tracks how many cache rows successfully
// landed (i.e. detail responses that parsed cleanly).
// ------------------------------------------------------------------

typedef struct
{
  pthread_mutex_t          lock;
  uint32_t                 pending;       // detail requests still in flight
  uint32_t                 kept;          // cache rows successfully added
  bool                     listing_done;  // true once the /v1/symbols handler
                                          // has finished kicking detail
                                          // requests (or aborted). Finalise
                                          // only when pending==0 AND this is
                                          // true — closes the race where
                                          // every detail response lands
                                          // synchronously before the loop
                                          // exits.
  char                     errbuf[GEMINI_ERR_SZ];
  gemini_done_symbols_cb_t cb;
  void                    *user;
} gem_symbols_batch_t;

static gem_symbols_batch_t *
gem_batch_alloc(gemini_done_symbols_cb_t cb, void *user)
{
  gem_symbols_batch_t *b;

  b = mem_alloc(GEM_CTX, "symbols.batch", sizeof(*b));

  memset(b, 0, sizeof(*b));
  pthread_mutex_init(&b->lock, NULL);
  b->cb   = cb;
  b->user = user;

  return(b);
}

static void
gem_batch_free(gem_symbols_batch_t *b)
{
  if(b == NULL)
    return;

  pthread_mutex_destroy(&b->lock);
  mem_free(b);
}

// Deliver the user callback once the batch is fully accounted for.
// Caller must NOT hold b->lock.
static void
gem_batch_finalise(gem_symbols_batch_t *b)
{
  gemini_symbols_result_t res;

  if(b == NULL)
    return;

  memset(&res, 0, sizeof(res));
  res.count = b->kept;
  snprintf(res.err, sizeof(res.err), "%s", b->errbuf);

  clam(CLAM_INFO, GEM_CTX, "symbols: %u row(s) cached", b->kept);

  if(b->cb != NULL)
    b->cb(&res, b->user);

  gem_batch_free(b);
}

// Drop one pending detail; return true iff this call brought us to the
// "all detail responses landed AND the listing handler has finished
// kicking requests" boundary. The caller then finalises.
static bool
gem_batch_dec_pending(gem_symbols_batch_t *b)
{
  bool should_finalise = false;

  pthread_mutex_lock(&b->lock);

  if(b->pending > 0)
    b->pending--;

  if(b->pending == 0 && b->listing_done)
    should_finalise = true;

  pthread_mutex_unlock(&b->lock);

  return(should_finalise);
}

// Called from the listing handler after the kick loop completes (or
// aborts). Marks the batch as no longer accruing new detail requests
// and finalises when pending is already at zero.
static bool
gem_batch_listing_done(gem_symbols_batch_t *b)
{
  bool should_finalise = false;

  pthread_mutex_lock(&b->lock);

  b->listing_done = true;

  if(b->pending == 0)
    should_finalise = true;

  pthread_mutex_unlock(&b->lock);

  return(should_finalise);
}

// ------------------------------------------------------------------
// Per-symbol detail response handler.
// ------------------------------------------------------------------

static void
gem_symbol_details_resp(int http_status, const char *body, size_t body_len,
    const char *err_hint, void *user)
{
  gem_request_t       *r = user;
  gem_symbols_batch_t *b;
  struct json_object  *root;
  char                 errbuf[GEMINI_ERR_SZ];
  char                 base[GEMINI_CURRENCY_SZ];
  char                 quote[GEMINI_CURRENCY_SZ];

  if(r == NULL)
    return;

  b = r->batch;

  switch(gem_classify_exchange(http_status, body, body_len, err_hint,
        errbuf, sizeof(errbuf)))
  {
    case GEM_RESP_OK:
      break;

    case GEM_RESP_RATE_LIMIT:
    case GEM_RESP_TRANSPORT:
    case GEM_RESP_HARD_ERROR:
    default:
      clam(CLAM_WARN, GEM_CTX,
          "symbols details '%s': %s",
          r->detail_symbol, errbuf);
      goto done;
  }

  root = json_parse_buf(body, body_len, GEM_CTX);

  if(root == NULL)
  {
    clam(CLAM_WARN, GEM_CTX,
        "symbols details '%s': malformed JSON",
        r->detail_symbol);
    goto done;
  }

  base[0]  = '\0';
  quote[0] = '\0';

  json_get_str(root, "base_currency",  base,  sizeof(base));
  json_get_str(root, "quote_currency", quote, sizeof(quote));

  if(base[0] == '\0' || quote[0] == '\0')
  {
    clam(CLAM_WARN, GEM_CTX,
        "symbols details '%s': missing base/quote (base='%s' quote='%s')",
        r->detail_symbol, base, quote);
    json_object_put(root);
    goto done;
  }

  if(gem_pairs_add(r->detail_symbol, base, quote) == SUCCESS)
  {
    pthread_mutex_lock(&b->lock);
    b->kept++;
    pthread_mutex_unlock(&b->lock);
  }

  json_object_put(root);

done:
  if(gem_batch_dec_pending(b))
    gem_batch_finalise(b);

  gem_req_release(r);
}

// ------------------------------------------------------------------
// Listing response handler.
//
// Response shape: a plain JSON array of native symbol strings.
//   [ "btcusd", "ethusd", "ltcusd", ... ]
// ------------------------------------------------------------------

static void
gem_symbols_listing_resp(int http_status, const char *body, size_t body_len,
    const char *err_hint, void *user)
{
  gem_request_t        *r       = user;
  gem_symbols_batch_t  *b;
  struct json_object   *root    = NULL;
  size_t                arr_len = 0;
  uint32_t              kicked  = 0;
  uint32_t              i;
  char                  errbuf[GEMINI_ERR_SZ];

  if(r == NULL)
    return;

  b = r->batch;

  switch(gem_classify_exchange(http_status, body, body_len, err_hint,
        errbuf, sizeof(errbuf)))
  {
    case GEM_RESP_OK:
      break;

    case GEM_RESP_RATE_LIMIT:
    case GEM_RESP_TRANSPORT:
    case GEM_RESP_HARD_ERROR:
    default:
      snprintf(b->errbuf, sizeof(b->errbuf), "%s", errbuf);
      goto done;
  }

  root = json_parse_buf(body, body_len, GEM_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    snprintf(b->errbuf, sizeof(b->errbuf),
        "Error: malformed JSON from Gemini /v1/symbols");
    goto done;
  }

  arr_len = (size_t)json_object_array_length(root);

  if(arr_len > GEM_SYMBOLS_DETAIL_CAP)
  {
    clam(CLAM_WARN, GEM_CTX,
        "symbols listing: %zu rows exceeds GEM_SYMBOLS_DETAIL_CAP=%d;"
        " truncating",
        arr_len, GEM_SYMBOLS_DETAIL_CAP);
    arr_len = GEM_SYMBOLS_DETAIL_CAP;
  }

  // Clear existing cache rows so a partially-failed refresh doesn't
  // leave a half-populated table in place. The cache stays empty
  // (pass-through behaviour) for the brief window between this
  // clear and the detail-response landings.
  gem_pairs_clear();

  for(i = 0; i < arr_len; i++)
  {
    struct json_object *elem = json_object_array_get_idx(root, (int)i);
    const char         *sym;
    gem_request_t      *dr;
    char                path[GEM_URL_SZ];
    size_t              sym_len;
    int                 n;

    if(elem == NULL || !json_object_is_type(elem, json_type_string))
      continue;

    sym = json_object_get_string(elem);

    if(sym == NULL || sym[0] == '\0')
      continue;

    sym_len = strlen(sym);

    if(sym_len + sizeof(GEM_PATH_SYMBOL_DETAILS) >= sizeof(path))
    {
      clam(CLAM_WARN, GEM_CTX,
          "symbols listing: detail path overflow for '%s'", sym);
      continue;
    }

    dr = gem_req_alloc();

    dr->type  = GEM_REQ_SYMBOL_DETAILS;
    dr->batch = b;
    snprintf(dr->detail_symbol, sizeof(dr->detail_symbol), "%s", sym);

    n = snprintf(path, sizeof(path), GEM_PATH_SYMBOL_DETAILS "%s", sym);

    if(n < 0 || (size_t)n >= sizeof(path))
    {
      gem_req_release(dr);
      continue;
    }

    // Reserve the pending slot BEFORE submitting so a synchronous
    // SUCCESS-then-async-response cannot race the dec_pending below
    // through `listing_done == false`.
    pthread_mutex_lock(&b->lock);
    b->pending++;
    pthread_mutex_unlock(&b->lock);

    if(exchange_request("gemini", EXCHANGE_PRIO_MARKET_BACKFILL,
          EXCHANGE_OP_REST_GET, path, NULL,
          gem_symbol_details_resp, dr) != SUCCESS)
    {
      gem_req_release(dr);

      pthread_mutex_lock(&b->lock);
      b->pending--;
      pthread_mutex_unlock(&b->lock);

      continue;
    }

    kicked++;
  }

  // arr_len > 0 with kicked == 0 means every per-symbol detail submit
  // failed pre-flight; surface that distinctly. arr_len == 0 is a
  // legitimate empty listing → leave errbuf empty (count=0 success).
  if(kicked == 0 && arr_len > 0 && b->errbuf[0] == '\0')
    snprintf(b->errbuf, sizeof(b->errbuf),
        "Error: Gemini symbols: no detail requests dispatched");

done:
  if(root != NULL)
    json_object_put(root);

  gem_req_release(r);

  if(gem_batch_listing_done(b))
    gem_batch_finalise(b);
}

// ------------------------------------------------------------------
// Public entry point
// ------------------------------------------------------------------

// No-op callback used by the fire-and-forget refresh from gem_start.
static void
gem_symbols_silent_cb(const gemini_symbols_result_t *res, void *user)
{
  (void)user;

  if(res->err[0] != '\0')
    clam(CLAM_WARN, GEM_CTX,
        "symbols refresh: %s (count=%u)", res->err, res->count);
}

bool
gemini_symbols_refresh_async(gemini_done_symbols_cb_t cb, void *user)
{
  gem_symbols_batch_t *b;
  gem_request_t       *r;

  b = gem_batch_alloc((cb != NULL) ? cb : gem_symbols_silent_cb, user);

  if(b == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type  = GEM_REQ_SYMBOLS;
  r->batch = b;

  if(exchange_request("gemini", EXCHANGE_PRIO_MARKET_BACKFILL,
        EXCHANGE_OP_REST_GET, GEM_PATH_SYMBOLS, NULL,
        gem_symbols_listing_resp, r) != SUCCESS)
  {
    gem_req_release(r);

    snprintf(b->errbuf, sizeof(b->errbuf),
        "Error: failed to submit Gemini /v1/symbols request");

    gem_batch_finalise(b);
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// GEM-2 typed wrappers
// ==================================================================

// Endpoint paths.
#define GEM_PATH_CANDLES        "/v2/candles/"     // suffix: <sym>/<tf>
#define GEM_PATH_BALANCES       "/v1/balances"
#define GEM_PATH_ORDER_NEW      "/v1/order/new"
#define GEM_PATH_ORDER_CANCEL   "/v1/order/cancel"
#define GEM_PATH_ORDER_STATUS   "/v1/order/status"
#define GEM_PATH_ORDERS         "/v1/orders"
#define GEM_PATH_MYTRADES       "/v1/mytrades"

#define GEM_ERR_NO_CREDS \
  "Error: Gemini credentials not configured " \
  "(plugin.gemini.creds.{api_key,private_key})"

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

// Tolerant decimal parse. Returns 0.0 on NULL/empty.
static double
gem_strtod(const char *s)
{
  return((s != NULL && s[0] != '\0') ? strtod(s, NULL) : 0.0);
}

// Read a numeric-or-string field as double. Gemini serializes prices,
// sizes, and fees as JSON strings to preserve precision; timestamps and
// flags arrive as bare numbers/bools. Accept both shapes transparently.
static double
gem_json_num(struct json_object *obj, const char *key)
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

// Read an integer-or-string field as int64. Gemini's `timestampms`
// arrives as a JSON number; `tid` and `order_id` arrive as numbers
// too. Returns 0 on miss/parse-fail.
static int64_t
gem_json_int64(struct json_object *obj, const char *key)
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

// Lowercase a buffer in-place.
static void
gem_str_lowercase(char *s)
{
  if(s == NULL)
    return;

  for(char *p = s; *p != '\0'; p++)
    *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
}

// Generic translation: caller-supplied product_id (any form) → Gemini
// native form (lowercase concatenated). Wraps gem_pair_to_native into a
// SUCCESS/FAIL surface so callers can short-circuit cleanly.
static bool
gem_translate_native(const char *product_id, char *out, size_t cap)
{
  if(product_id == NULL || product_id[0] == '\0'
      || out == NULL || cap == 0)
    return(FAIL);

  gem_pair_to_native(product_id, out, cap);

  return(out[0] != '\0' ? SUCCESS : FAIL);
}

// Map the generic granularity to Gemini's `time_frame` token. NULL when
// the tier is unsupported (4h / 1w). Caller surfaces a clean error.
static const char *
gem_gran_to_time_frame(exchange_granularity_t gran)
{
  switch(gran)
  {
    case EXCH_GRAN_1M:   return("1m");
    case EXCH_GRAN_5M:   return("5m");
    case EXCH_GRAN_15M:  return("15m");
    case EXCH_GRAN_30M:  return("30m");
    case EXCH_GRAN_1H:   return("1hr");
    case EXCH_GRAN_4H:   return(NULL);
    case EXCH_GRAN_1D:   return("1day");
    case EXCH_GRAN_1W:   return(NULL);
  }

  return(NULL);
}

// Format a decimal value for Gemini's price/amount strings. Gemini
// accepts trailing zeros and arbitrary precision up to the symbol's
// `tick_size`; `%.8f` strips to the practical 8-decimal floor that
// matches the wider BTC/ETH precision while staying inside Gemini's
// limits for stable pairs. The caller is responsible for honoring
// per-pair tick / min-size separately.
static int
gem_fmt_decimal(char *out, size_t cap, double v)
{
  return(snprintf(out, cap, "%.8f", v));
}

// Mint a client_order_id when the caller didn't supply one. Format
// matches the kraken/coinbase plugins' style: `<prefix>-<unix_ms>-<n>`.
// Static counter survives the process lifetime; collisions across
// daemon restarts within the same millisecond are vanishingly rare and
// would surface as a Gemini-side duplicate-cl_oid error rather than
// silent loss.
static void
gem_mint_client_oid(char *out, size_t cap)
{
  static uint32_t  counter   = 0;
  static pthread_mutex_t mu  = PTHREAD_MUTEX_INITIALIZER;
  struct timespec        ts;
  uint32_t               n;

  if(out == NULL || cap == 0)
    return;

  if(clock_gettime(CLOCK_REALTIME, &ts) != 0)
  {
    ts.tv_sec  = (time_t)time(NULL);
    ts.tv_nsec = 0;
  }

  pthread_mutex_lock(&mu);
  n = ++counter;
  pthread_mutex_unlock(&mu);

  snprintf(out, cap, "gem-%lld%03ld-%u",
      (long long)ts.tv_sec,
      (long)(ts.tv_nsec / 1000000L),
      n);
}

// qsort comparator: ascending by ts_open_ms.
static int
gem_candle_cmp_asc(const void *a, const void *b)
{
  const gemini_candle_t *ca = a;
  const gemini_candle_t *cb = b;

  if(ca->ts_open_ms < cb->ts_open_ms) return(-1);
  if(ca->ts_open_ms > cb->ts_open_ms) return( 1);
  return(0);
}

// ------------------------------------------------------------------
// Delivery helpers (fail path)
// ------------------------------------------------------------------

static void
gem_deliver_candles_fail(gem_request_t *r, const char *err)
{
  gemini_candles_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  gem_req_release(r);
}

static void
gem_deliver_balances_fail(gem_request_t *r, const char *err)
{
  gemini_balances_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.balances != NULL)
    r->cb.balances(&res, r->user);

  gem_req_release(r);
}

static void
gem_deliver_order_fail(gem_request_t *r, const char *err)
{
  gemini_order_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  gem_req_release(r);
}

static void
gem_deliver_orders_fail(gem_request_t *r, const char *err)
{
  gemini_orders_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.orders != NULL)
    r->cb.orders(&res, r->user);

  gem_req_release(r);
}

static void
gem_deliver_fills_fail(gem_request_t *r, const char *err)
{
  gemini_fills_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(r->cb.fills != NULL)
    r->cb.fills(&res, r->user);

  gem_req_release(r);
}

// ------------------------------------------------------------------
// Order object parser
//
// Shared by /v1/order/new, /v1/order/cancel, /v1/order/status and
// /v1/orders (each entry of). Wire shape:
//
//   { "order_id":           "12345",
//     "client_order_id":    "abc-123",
//     "symbol":             "btcusd",
//     "exchange":           "gemini",
//     "price":              "30000.00",
//     "avg_execution_price":"30000.00",
//     "side":               "buy" | "sell",
//     "type":               "exchange limit" | "exchange market" | ...,
//     "timestamp":          "1717000000",
//     "timestampms":         1717000000000,
//     "is_live":            true,
//     "is_cancelled":       false,
//     "executed_amount":    "0.001",
//     "remaining_amount":   "0.000",
//     "original_amount":    "0.001",
//     "options":            [ "maker-or-cancel" ]
//   }
// ------------------------------------------------------------------

static bool
gem_parse_order(struct json_object *obj, gemini_order_t *out)
{
  struct json_object *options_arr;
  bool                is_live      = false;
  bool                is_cancelled = false;
  char                tmp[GEMINI_PRODUCT_ID_SZ];

  if(obj == NULL || out == NULL)
    return(false);

  memset(out, 0, sizeof(*out));

  json_get_str(obj, "order_id",        out->order_id,   sizeof(out->order_id));
  json_get_str(obj, "client_order_id", out->client_oid, sizeof(out->client_oid));

  // `symbol` is the Gemini-native lowercase form; map back to the
  // abstraction's canonical form so the consumer sees uniform ids.
  tmp[0] = '\0';
  json_get_str(obj, "symbol", tmp, sizeof(tmp));

  if(tmp[0] != '\0')
    gem_pair_to_abstr(tmp, out->product_id, sizeof(out->product_id));

  json_get_str(obj, "side", out->side, sizeof(out->side));
  json_get_str(obj, "type", out->type, sizeof(out->type));

  out->price          = gem_json_num(obj, "price");
  out->size           = gem_json_num(obj, "original_amount");
  out->filled_size    = gem_json_num(obj, "executed_amount");
  out->fill_fees      = gem_json_num(obj, "fee_amount");

  // Gemini exposes avg_execution_price for partial / full fills. When
  // non-zero, prefer it over `price` for downstream P&L math; we
  // surface it via `executed_value = avg_execution_price * filled`.
  {
    double avg = gem_json_num(obj, "avg_execution_price");

    if(avg > 0.0 && out->filled_size > 0.0)
      out->executed_value = avg * out->filled_size;
  }

  json_get_bool(obj, "is_live",      &is_live);
  json_get_bool(obj, "is_cancelled", &is_cancelled);

  if(is_cancelled)
    snprintf(out->status, sizeof(out->status), "CANCELLED");
  else if(is_live)
    snprintf(out->status, sizeof(out->status), "OPEN");
  else
    snprintf(out->status, sizeof(out->status), "FILLED");

  out->settled = !is_live;

  // timestampms is a JSON number; fall back to seconds×1000 when absent.
  out->created_at_ms = gem_json_int64(obj, "timestampms");

  if(out->created_at_ms == 0)
  {
    int64_t sec = gem_json_int64(obj, "timestamp");

    if(sec > 0)
      out->created_at_ms = sec * 1000;
  }

  // Gemini's `tif` is implicit: default GTC unless `options` contains
  // `immediate-or-cancel` / `fill-or-kill`. Walk the options array.
  snprintf(out->tif, sizeof(out->tif), "GTC");

  options_arr = json_get_array(obj, "options");

  if(options_arr != NULL)
  {
    int n = (int)json_object_array_length(options_arr);

    for(int i = 0; i < n; i++)
    {
      struct json_object *opt = json_object_array_get_idx(options_arr, i);
      const char         *s   = (opt != NULL)
                                  ? json_object_get_string(opt) : NULL;

      if(s == NULL)
        continue;

      if(strcmp(s, "maker-or-cancel") == 0)
        out->post_only = true;
      else if(strcmp(s, "immediate-or-cancel") == 0)
        snprintf(out->tif, sizeof(out->tif), "IOC");
      else if(strcmp(s, "fill-or-kill") == 0)
        snprintf(out->tif, sizeof(out->tif), "FOK");
    }
  }

  gem_str_lowercase(out->side);

  return(out->order_id[0] != '\0');
}

// ==================================================================
// Candles — public GET /v2/candles/<sym>/<time_frame>
// ==================================================================
//
// Response: JSON array of 6-element arrays:
//   [ [ ts_ms, open, high, low, close, volume ], ... ]
//
// Gemini returns up to 500 most-recent candles per call (no `since`
// param). Order is descending by timestamp; we sort ascending on the
// way out and apply client-side window filtering when since_ms /
// until_ms are non-zero.

typedef struct
{
  int64_t since_ms;
  int64_t until_ms;
} gem_candles_window_t;

static void
gem_candles_done(const curl_response_t *resp)
{
  gem_request_t           *r;
  gemini_candles_result_t  res = { 0 };
  char                     errbuf[GEMINI_ERR_SZ];
  struct json_object      *root;
  int                      len;
  uint32_t                 kept = 0;
  gem_resp_kind_t          kind;
  int64_t                  since_ms;
  int64_t                  until_ms;

  if(resp == NULL || resp->user_data == NULL)
    return;

  r = (gem_request_t *)resp->user_data;

  kind = gem_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != GEM_RESP_OK)
  {
    gem_deliver_candles_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, GEM_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    if(root != NULL)
      json_object_put(root);
    gem_deliver_candles_fail(r,
        "Error: malformed JSON from Gemini /v2/candles");
    return;
  }

  since_ms = r->since_ms;
  until_ms = r->until_ms;

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < GEMINI_MAX_CANDLES; i++)
  {
    struct json_object *row = json_object_array_get_idx(root, i);
    gemini_candle_t    *c   = &res.rows[kept];
    struct json_object *v;
    int64_t             ts;

    if(row == NULL || !json_object_is_type(row, json_type_array))
      continue;

    if(json_object_array_length(row) < 6)
      continue;

    v  = json_object_array_get_idx(row, 0);
    ts = (v != NULL) ? (int64_t)json_object_get_int64(v) : 0;

    if(ts <= 0)
      continue;

    // Client-side window filter. since_ms inclusive, until_ms
    // exclusive — same semantics as the Kraken adapter.
    if(since_ms > 0 && ts < since_ms)
      continue;
    if(until_ms > 0 && ts >= until_ms)
      continue;

    c->ts_open_ms = ts;

    v = json_object_array_get_idx(row, 1);
    c->open = json_object_is_type(v, json_type_string)
                ? gem_strtod(json_object_get_string(v))
                : json_object_get_double(v);

    v = json_object_array_get_idx(row, 2);
    c->high = json_object_is_type(v, json_type_string)
                ? gem_strtod(json_object_get_string(v))
                : json_object_get_double(v);

    v = json_object_array_get_idx(row, 3);
    c->low = json_object_is_type(v, json_type_string)
                ? gem_strtod(json_object_get_string(v))
                : json_object_get_double(v);

    v = json_object_array_get_idx(row, 4);
    c->close = json_object_is_type(v, json_type_string)
                 ? gem_strtod(json_object_get_string(v))
                 : json_object_get_double(v);

    v = json_object_array_get_idx(row, 5);
    c->volume = json_object_is_type(v, json_type_string)
                  ? gem_strtod(json_object_get_string(v))
                  : json_object_get_double(v);

    kept++;
  }

  // Gemini returns most-recent-first; consumers expect ascending order.
  if(kept > 1)
    qsort(res.rows, kept, sizeof(res.rows[0]), gem_candle_cmp_asc);

  res.count = kept;

  clam(CLAM_DEBUG2, GEM_CTX,
       "candles: %s tf=%s -> %u row(s) (raw=%d, window=[%" PRId64 ",%" PRId64 "))",
       r->product_id, r->granularity, kept, len, since_ms, until_ms);

  if(r->cb.candles != NULL)
    r->cb.candles(&res, r->user);

  json_object_put(root);
  gem_req_release(r);
}

bool
gemini_fetch_candles_async(const char *pair, exchange_granularity_t gran,
    int64_t since_ms, int64_t until_ms, uint8_t prio,
    gemini_done_candles_cb_t cb, void *user)
{
  gem_request_t *r;
  const char    *tf;
  char           native[GEMINI_PRODUCT_ID_SZ];
  char           path[GEM_URL_SZ];
  int            n;

  if(pair == NULL || pair[0] == '\0' || cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type       = GEM_REQ_CANDLES;
  r->since_ms   = since_ms;
  r->until_ms   = until_ms;
  r->cb.candles = cb;
  r->user       = user;
  snprintf(r->product_id, sizeof(r->product_id), "%s", pair);

  tf = gem_gran_to_time_frame(gran);

  if(tf == NULL)
  {
    gem_deliver_candles_fail(r, "Error: gemini: granularity unsupported");
    return(FAIL);
  }

  snprintf(r->granularity, sizeof(r->granularity), "%s", tf);

  if(gem_translate_native(pair, native, sizeof(native)) != SUCCESS)
  {
    gem_deliver_candles_fail(r, "Error: gemini: unknown product_id");
    return(FAIL);
  }

  n = snprintf(path, sizeof(path), GEM_PATH_CANDLES "%s/%s", native, tf);

  if(n < 0 || (size_t)n >= sizeof(path))
  {
    gem_deliver_candles_fail(r, "Error: gemini candles path overflow");
    return(FAIL);
  }

  if(gem_submit_public(r, prio, path, gem_candles_done) != SUCCESS)
  {
    gem_deliver_candles_fail(r,
        "Error: failed to submit Gemini /v2/candles request");
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// Balances — private POST /v1/balances
// ==================================================================
//
// Response: array of objects
//   { "currency":"USD",
//     "amount":   "12.34",
//     "available":"10.00",
//     "availableForWithdrawal":"10.00",
//     "type":"exchange" }
//
// hold = amount - available (clamped to zero).

static void
gem_balances_done(const curl_response_t *resp)
{
  gem_request_t            *r;
  gemini_balances_result_t  res = { 0 };
  char                      errbuf[GEMINI_ERR_SZ];
  struct json_object       *root;
  gem_resp_kind_t           kind;
  int                       len;
  uint32_t                  kept = 0;

  if(resp == NULL || resp->user_data == NULL)
    return;

  r = (gem_request_t *)resp->user_data;

  kind = gem_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != GEM_RESP_OK)
  {
    gem_deliver_balances_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, GEM_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    if(root != NULL)
      json_object_put(root);
    gem_deliver_balances_fail(r,
        "Error: malformed JSON from Gemini /v1/balances");
    return;
  }

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < GEMINI_MAX_ACCOUNTS; i++)
  {
    struct json_object *row = json_object_array_get_idx(root, i);
    gemini_account_t   *acc = &res.rows[kept];
    double              amount;
    double              available;

    if(row == NULL || !json_object_is_type(row, json_type_object))
      continue;

    memset(acc, 0, sizeof(*acc));

    json_get_str(row, "currency", acc->currency, sizeof(acc->currency));

    if(acc->currency[0] == '\0')
      continue;

    amount    = gem_json_num(row, "amount");
    available = gem_json_num(row, "available");

    acc->balance   = amount;
    acc->available = available;
    acc->hold      = (amount > available) ? (amount - available) : 0.0;

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, GEM_CTX, "balances: %u currency row(s)", kept);

  if(r->cb.balances != NULL)
    r->cb.balances(&res, r->user);

  json_object_put(root);
  gem_req_release(r);
}

bool
gemini_get_balance_async(gemini_done_balances_cb_t cb, void *user)
{
  gem_request_t *r;
  uint64_t       nonce = 0;
  char           body[GEM_BODY_SZ];
  int            n;

  if(cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type        = GEM_REQ_BALANCE;
  r->cb.balances = cb;
  r->user        = user;

  if(!gem_apikey_configured())
  {
    gem_deliver_balances_fail(r, GEM_ERR_NO_CREDS);
    return(FAIL);
  }

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    gem_deliver_balances_fail(r, "Error: gemini nonce mint failed");
    return(FAIL);
  }

  n = snprintf(body, sizeof(body),
      "{\"request\":\"%s\",\"nonce\":%" PRIu64 "}",
      GEM_PATH_BALANCES, nonce);

  if(n < 0 || (size_t)n >= sizeof(body))
  {
    gem_deliver_balances_fail(r, "Error: gemini balances body overflow");
    return(FAIL);
  }

  if(gem_submit_private(r, EXCHANGE_PRIO_TRANSACTIONAL, GEM_PATH_BALANCES,
        body, (size_t)n, gem_balances_done) != SUCCESS)
  {
    gem_deliver_balances_fail(r,
        "Error: failed to submit Gemini /v1/balances request");
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// New order — private POST /v1/order/new
// ==================================================================
//
// Payload:
//   { "request":         "/v1/order/new",
//     "nonce":           <int>,
//     "client_order_id": "<str>",
//     "symbol":          "btcusd",
//     "amount":          "0.0001",
//     "price":           "30000.00",     // omitted on market orders
//     "side":            "buy" | "sell",
//     "type":            "exchange limit" | "exchange market",
//     "options":         [ "maker-or-cancel" ]  // post_only only
//   }
//
// Response: a single order object (same shape gem_parse_order accepts).

static void
gem_order_new_done(const curl_response_t *resp)
{
  gem_request_t         *r;
  gemini_order_result_t  res = { 0 };
  char                   errbuf[GEMINI_ERR_SZ];
  struct json_object    *root;
  gem_resp_kind_t        kind;

  if(resp == NULL || resp->user_data == NULL)
    return;

  r = (gem_request_t *)resp->user_data;

  kind = gem_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != GEM_RESP_OK)
  {
    gem_deliver_order_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, GEM_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_object))
  {
    if(root != NULL)
      json_object_put(root);
    gem_deliver_order_fail(r,
        "Error: malformed JSON from Gemini /v1/order/new");
    return;
  }

  if(!gem_parse_order(root, &res.order))
  {
    // Server accepted but returned no order_id — surface what we
    // recorded at submit time so the caller can correlate.
    snprintf(res.order.client_oid, sizeof(res.order.client_oid),
        "%s", r->order_id);
    snprintf(res.order.product_id, sizeof(res.order.product_id),
        "%s", r->product_id);
    snprintf(res.order.status, sizeof(res.order.status), "UNKNOWN");
    snprintf(res.err, sizeof(res.err),
        "Error: Gemini /v1/order/new returned no order_id");
  }

  clam(CLAM_DEBUG2, GEM_CTX,
       "order/new: order_id='%s' client_oid='%s' status='%s'",
       res.order.order_id, res.order.client_oid, res.order.status);

  if(r->cb.order != NULL)
    r->cb.order(&res, r->user);

  json_object_put(root);
  gem_req_release(r);
}

bool
gemini_add_order_async(const gemini_place_order_req_t *req,
    gemini_done_order_cb_t cb, void *user)
{
  gem_request_t *r;
  uint64_t       nonce = 0;
  char           body[GEM_BODY_SZ];
  char           native[GEMINI_PRODUCT_ID_SZ];
  char           cl_oid[GEMINI_CLIENT_OID_SZ];
  char           amount_str[40];
  char           price_str[40];
  const char    *type_str;
  bool           is_market;
  bool           is_buy;
  bool           is_sell;
  int            n;

  if(req == NULL || cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type     = GEM_REQ_ADD_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->product_id, sizeof(r->product_id), "%s", req->product_id);
  // Echo client_oid into r->order_id so the response-side fallback
  // can stamp it back even when Gemini's response omits the field.
  snprintf(r->order_id, sizeof(r->order_id), "%s", req->client_oid);

  if(!gem_apikey_configured())
  {
    gem_deliver_order_fail(r, GEM_ERR_NO_CREDS);
    return(FAIL);
  }

  // Gemini's documented type strings start with the literal "exchange ".
  // Accept both the generic and the native form from the caller.
  if(req->type[0] == '\0'
      || strcmp(req->type, "limit") == 0
      || strcmp(req->type, "exchange limit") == 0)
  {
    type_str  = "exchange limit";
    is_market = false;
  }
  else if(strcmp(req->type, "market") == 0
      || strcmp(req->type, "exchange market") == 0)
  {
    type_str  = "exchange market";
    is_market = true;
  }
  else
  {
    gem_deliver_order_fail(r,
        "Error: Gemini supports order types 'limit' and 'market'");
    return(FAIL);
  }

  is_buy  = (strcmp(req->side, "buy")  == 0);
  is_sell = (strcmp(req->side, "sell") == 0);

  if(!is_buy && !is_sell)
  {
    gem_deliver_order_fail(r,
        "Error: Gemini side must be 'buy' or 'sell'");
    return(FAIL);
  }

  if(req->size <= 0.0)
  {
    gem_deliver_order_fail(r, "Error: positive size required");
    return(FAIL);
  }

  if(!is_market && req->price <= 0.0)
  {
    gem_deliver_order_fail(r,
        "Error: positive price required for a limit order");
    return(FAIL);
  }

  if(is_market && req->post_only)
  {
    gem_deliver_order_fail(r,
        "Error: post_only is invalid on market orders");
    return(FAIL);
  }

  if(gem_translate_native(req->product_id, native, sizeof(native)) != SUCCESS)
  {
    gem_deliver_order_fail(r, "Error: gemini: unknown product_id");
    return(FAIL);
  }

  if(req->client_oid[0] != '\0')
    snprintf(cl_oid, sizeof(cl_oid), "%s", req->client_oid);
  else
    gem_mint_client_oid(cl_oid, sizeof(cl_oid));

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    gem_deliver_order_fail(r, "Error: gemini nonce mint failed");
    return(FAIL);
  }

  gem_fmt_decimal(amount_str, sizeof(amount_str), req->size);

  if(is_market)
  {
    // Market orders omit price; Gemini still expects the field on the
    // wire (deliberate book-walk to a quote), but in practice a zero
    // sentinel triggers the documented "execute up to the spread"
    // semantics for IOC-marked orders. We instead emit no price field
    // for market type — Gemini accepts type="exchange market" without
    // price as a normal market order.
    n = snprintf(body, sizeof(body),
        "{"
          "\"request\":\"%s\","
          "\"nonce\":%" PRIu64 ","
          "\"client_order_id\":\"%s\","
          "\"symbol\":\"%s\","
          "\"amount\":\"%s\","
          "\"side\":\"%s\","
          "\"type\":\"%s\""
        "}",
        GEM_PATH_ORDER_NEW, nonce, cl_oid, native,
        amount_str, is_buy ? "buy" : "sell", type_str);
  }
  else
  {
    gem_fmt_decimal(price_str, sizeof(price_str), req->price);

    if(req->post_only)
    {
      n = snprintf(body, sizeof(body),
          "{"
            "\"request\":\"%s\","
            "\"nonce\":%" PRIu64 ","
            "\"client_order_id\":\"%s\","
            "\"symbol\":\"%s\","
            "\"amount\":\"%s\","
            "\"price\":\"%s\","
            "\"side\":\"%s\","
            "\"type\":\"%s\","
            "\"options\":[\"maker-or-cancel\"]"
          "}",
          GEM_PATH_ORDER_NEW, nonce, cl_oid, native,
          amount_str, price_str, is_buy ? "buy" : "sell", type_str);
    }
    else
    {
      n = snprintf(body, sizeof(body),
          "{"
            "\"request\":\"%s\","
            "\"nonce\":%" PRIu64 ","
            "\"client_order_id\":\"%s\","
            "\"symbol\":\"%s\","
            "\"amount\":\"%s\","
            "\"price\":\"%s\","
            "\"side\":\"%s\","
            "\"type\":\"%s\""
          "}",
          GEM_PATH_ORDER_NEW, nonce, cl_oid, native,
          amount_str, price_str, is_buy ? "buy" : "sell", type_str);
    }
  }

  if(n < 0 || (size_t)n >= sizeof(body))
  {
    gem_deliver_order_fail(r, "Error: gemini order/new body overflow");
    return(FAIL);
  }

  if(gem_submit_private(r, EXCHANGE_PRIO_TRANSACTIONAL, GEM_PATH_ORDER_NEW,
        body, (size_t)n, gem_order_new_done) != SUCCESS)
  {
    gem_deliver_order_fail(r,
        "Error: failed to submit Gemini /v1/order/new request");
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// Cancel order — private POST /v1/order/cancel
// ==================================================================
//
// Payload:
//   { "request":"/v1/order/cancel", "nonce":<int>, "order_id":<int> }
//
// Gemini's `order_id` is numeric on the wire. The caller's `order_id`
// is a decimal string (the generic abstraction surface uses strings);
// we convert via strtoll. Response is the same order-object shape new-
// order returns.

static void
gem_order_cancel_done(const curl_response_t *resp)
{
  // Same shape as new-order. Reuse the parser.
  gem_order_new_done(resp);
}

bool
gemini_cancel_order_async(const char *order_id,
    gemini_done_order_cb_t cb, void *user)
{
  gem_request_t *r;
  uint64_t       nonce = 0;
  char           body[GEM_BODY_SZ];
  long long      oid_num;
  char          *end = NULL;
  int            n;

  if(order_id == NULL || order_id[0] == '\0' || cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type     = GEM_REQ_CANCEL_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!gem_apikey_configured())
  {
    gem_deliver_order_fail(r, GEM_ERR_NO_CREDS);
    return(FAIL);
  }

  oid_num = strtoll(order_id, &end, 10);

  if(end == order_id || (end != NULL && *end != '\0') || oid_num <= 0)
  {
    gem_deliver_order_fail(r,
        "Error: gemini cancel: order_id must be a positive integer");
    return(FAIL);
  }

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    gem_deliver_order_fail(r, "Error: gemini nonce mint failed");
    return(FAIL);
  }

  n = snprintf(body, sizeof(body),
      "{"
        "\"request\":\"%s\","
        "\"nonce\":%" PRIu64 ","
        "\"order_id\":%lld"
      "}",
      GEM_PATH_ORDER_CANCEL, nonce, oid_num);

  if(n < 0 || (size_t)n >= sizeof(body))
  {
    gem_deliver_order_fail(r, "Error: gemini cancel body overflow");
    return(FAIL);
  }

  if(gem_submit_private(r, EXCHANGE_PRIO_TRANSACTIONAL, GEM_PATH_ORDER_CANCEL,
        body, (size_t)n, gem_order_cancel_done) != SUCCESS)
  {
    gem_deliver_order_fail(r,
        "Error: failed to submit Gemini /v1/order/cancel request");
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// Order status — private POST /v1/order/status
// ==================================================================
//
// Payload:
//   { "request":"/v1/order/status", "nonce":<int>, "order_id":<int> }
//
// Response is the same order-object shape new-order returns.

static void
gem_order_status_done(const curl_response_t *resp)
{
  gem_order_new_done(resp);
}

bool
gemini_query_order_async(const char *order_id,
    gemini_done_order_cb_t cb, void *user)
{
  gem_request_t *r;
  uint64_t       nonce = 0;
  char           body[GEM_BODY_SZ];
  long long      oid_num;
  char          *end = NULL;
  int            n;

  if(order_id == NULL || order_id[0] == '\0' || cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type     = GEM_REQ_QUERY_ORDER;
  r->cb.order = cb;
  r->user     = user;
  snprintf(r->order_id, sizeof(r->order_id), "%s", order_id);

  if(!gem_apikey_configured())
  {
    gem_deliver_order_fail(r, GEM_ERR_NO_CREDS);
    return(FAIL);
  }

  oid_num = strtoll(order_id, &end, 10);

  if(end == order_id || (end != NULL && *end != '\0') || oid_num <= 0)
  {
    gem_deliver_order_fail(r,
        "Error: gemini status: order_id must be a positive integer");
    return(FAIL);
  }

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    gem_deliver_order_fail(r, "Error: gemini nonce mint failed");
    return(FAIL);
  }

  n = snprintf(body, sizeof(body),
      "{"
        "\"request\":\"%s\","
        "\"nonce\":%" PRIu64 ","
        "\"order_id\":%lld"
      "}",
      GEM_PATH_ORDER_STATUS, nonce, oid_num);

  if(n < 0 || (size_t)n >= sizeof(body))
  {
    gem_deliver_order_fail(r, "Error: gemini status body overflow");
    return(FAIL);
  }

  if(gem_submit_private(r, EXCHANGE_PRIO_TRANSACTIONAL, GEM_PATH_ORDER_STATUS,
        body, (size_t)n, gem_order_status_done) != SUCCESS)
  {
    gem_deliver_order_fail(r,
        "Error: failed to submit Gemini /v1/order/status request");
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// Active orders — private POST /v1/orders
// ==================================================================
//
// Payload: { "request":"/v1/orders", "nonce":<int> }
// Response: array of order objects. Gemini only surfaces OPEN orders
// here — there is no documented closed-orders endpoint, so the vtable
// adapter rejects callers that pass status="closed" upstream.

static void
gem_active_orders_done(const curl_response_t *resp)
{
  gem_request_t          *r;
  gemini_orders_result_t  res = { 0 };
  char                    errbuf[GEMINI_ERR_SZ];
  struct json_object     *root;
  gem_resp_kind_t         kind;
  int                     len;
  uint32_t                kept = 0;

  if(resp == NULL || resp->user_data == NULL)
    return;

  r = (gem_request_t *)resp->user_data;

  kind = gem_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != GEM_RESP_OK)
  {
    gem_deliver_orders_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, GEM_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    if(root != NULL)
      json_object_put(root);
    gem_deliver_orders_fail(r,
        "Error: malformed JSON from Gemini /v1/orders");
    return;
  }

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < GEMINI_MAX_ORDERS_LIST; i++)
  {
    struct json_object *row = json_object_array_get_idx(root, i);

    if(row == NULL || !json_object_is_type(row, json_type_object))
      continue;

    if(gem_parse_order(row, &res.rows[kept]))
      kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, GEM_CTX, "orders/active: %u row(s)", kept);

  if(r->cb.orders != NULL)
    r->cb.orders(&res, r->user);

  json_object_put(root);
  gem_req_release(r);
}

bool
gemini_active_orders_async(gemini_done_orders_cb_t cb, void *user)
{
  gem_request_t *r;
  uint64_t       nonce = 0;
  char           body[GEM_BODY_SZ];
  int            n;

  if(cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type      = GEM_REQ_ACTIVE_ORDERS;
  r->cb.orders = cb;
  r->user      = user;

  if(!gem_apikey_configured())
  {
    gem_deliver_orders_fail(r, GEM_ERR_NO_CREDS);
    return(FAIL);
  }

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    gem_deliver_orders_fail(r, "Error: gemini nonce mint failed");
    return(FAIL);
  }

  n = snprintf(body, sizeof(body),
      "{\"request\":\"%s\",\"nonce\":%" PRIu64 "}",
      GEM_PATH_ORDERS, nonce);

  if(n < 0 || (size_t)n >= sizeof(body))
  {
    gem_deliver_orders_fail(r, "Error: gemini orders body overflow");
    return(FAIL);
  }

  if(gem_submit_private(r, EXCHANGE_PRIO_TRANSACTIONAL, GEM_PATH_ORDERS,
        body, (size_t)n, gem_active_orders_done) != SUCCESS)
  {
    gem_deliver_orders_fail(r,
        "Error: failed to submit Gemini /v1/orders request");
    return(FAIL);
  }

  return(SUCCESS);
}

// ==================================================================
// Past trades — private POST /v1/mytrades
// ==================================================================
//
// Payload:
//   { "request":"/v1/mytrades",
//     "nonce":<int>,
//     "symbol":"<native>",         // required by Gemini
//     "timestamp":<unix-ms>        // optional lower-bound filter
//   }
//
// Response: array of fill objects
//   { "price":"30000.00",
//     "amount":"0.0001",
//     "timestamp":   1717000000,
//     "timestampms": 1717000000000,
//     "type":"Buy" | "Sell",
//     "tid":<int>,
//     "order_id":"12345",
//     "client_order_id":"abc-123",
//     "fee_amount":"0.075",
//     "fee_currency":"USD",
//     "is_auction_fill":false
//   }

static void
gem_mytrades_done(const curl_response_t *resp)
{
  gem_request_t         *r;
  gemini_fills_result_t  res = { 0 };
  char                   errbuf[GEMINI_ERR_SZ];
  struct json_object    *root;
  gem_resp_kind_t        kind;
  int                    len;
  uint32_t               kept = 0;

  if(resp == NULL || resp->user_data == NULL)
    return;

  r = (gem_request_t *)resp->user_data;

  kind = gem_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != GEM_RESP_OK)
  {
    gem_deliver_fills_fail(r, errbuf);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, GEM_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    if(root != NULL)
      json_object_put(root);
    gem_deliver_fills_fail(r,
        "Error: malformed JSON from Gemini /v1/mytrades");
    return;
  }

  len = (int)json_object_array_length(root);

  for(int i = 0; i < len && kept < GEMINI_MAX_FILLS_LIST; i++)
  {
    struct json_object *row = json_object_array_get_idx(root, i);
    gemini_fill_t      *f   = &res.rows[kept];
    int64_t             ts_ms;

    if(row == NULL || !json_object_is_type(row, json_type_object))
      continue;

    memset(f, 0, sizeof(*f));

    json_get_str(row, "order_id",        f->order_id,
        sizeof(f->order_id));
    json_get_str(row, "client_order_id", f->client_oid,
        sizeof(f->client_oid));

    // Echo the caller's product_id (already in the canonical form) so
    // downstream consumers don't need a second lookup. Gemini does not
    // include the symbol in mytrades rows because the request scopes it.
    snprintf(f->product_id, sizeof(f->product_id), "%s", r->product_id);

    json_get_str(row, "type", f->side, sizeof(f->side));
    gem_str_lowercase(f->side);

    f->trade_id = gem_json_int64(row, "tid");
    f->price    = gem_json_num(row, "price");
    f->size     = gem_json_num(row, "amount");
    f->fee      = gem_json_num(row, "fee_amount");

    ts_ms = gem_json_int64(row, "timestampms");

    if(ts_ms == 0)
    {
      int64_t sec = gem_json_int64(row, "timestamp");

      if(sec > 0)
        ts_ms = sec * 1000;
    }

    f->time_ms = ts_ms;

    kept++;
  }

  res.count = kept;

  clam(CLAM_DEBUG2, GEM_CTX,
       "mytrades: %u row(s) for '%s'", kept, r->product_id);

  if(r->cb.fills != NULL)
    r->cb.fills(&res, r->user);

  json_object_put(root);
  gem_req_release(r);
}

bool
gemini_mytrades_async(const char *product_id, int64_t since_ms,
    gemini_done_fills_cb_t cb, void *user)
{
  gem_request_t *r;
  uint64_t       nonce = 0;
  char           body[GEM_BODY_SZ];
  char           native[GEMINI_PRODUCT_ID_SZ];
  int            n;

  if(cb == NULL)
    return(FAIL);

  r = gem_req_alloc();

  r->type     = GEM_REQ_MYTRADES;
  r->cb.fills = cb;
  r->user     = user;

  if(product_id != NULL && product_id[0] != '\0')
    snprintf(r->product_id, sizeof(r->product_id), "%s", product_id);

  if(!gem_apikey_configured())
  {
    gem_deliver_fills_fail(r, GEM_ERR_NO_CREDS);
    return(FAIL);
  }

  // Gemini requires `symbol` on /v1/mytrades; reject NULL/empty rather
  // than silently scoping to every pair. The vtable adapter surfaces a
  // single product at a time.
  if(product_id == NULL || product_id[0] == '\0')
  {
    gem_deliver_fills_fail(r,
        "Error: gemini mytrades requires a product_id");
    return(FAIL);
  }

  if(gem_translate_native(product_id, native, sizeof(native)) != SUCCESS)
  {
    gem_deliver_fills_fail(r, "Error: gemini: unknown product_id");
    return(FAIL);
  }

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    gem_deliver_fills_fail(r, "Error: gemini nonce mint failed");
    return(FAIL);
  }

  if(since_ms > 0)
  {
    n = snprintf(body, sizeof(body),
        "{"
          "\"request\":\"%s\","
          "\"nonce\":%" PRIu64 ","
          "\"symbol\":\"%s\","
          "\"timestamp\":%" PRId64
        "}",
        GEM_PATH_MYTRADES, nonce, native, since_ms);
  }
  else
  {
    n = snprintf(body, sizeof(body),
        "{"
          "\"request\":\"%s\","
          "\"nonce\":%" PRIu64 ","
          "\"symbol\":\"%s\""
        "}",
        GEM_PATH_MYTRADES, nonce, native);
  }

  if(n < 0 || (size_t)n >= sizeof(body))
  {
    gem_deliver_fills_fail(r, "Error: gemini mytrades body overflow");
    return(FAIL);
  }

  if(gem_submit_private(r, EXCHANGE_PRIO_TRANSACTIONAL, GEM_PATH_MYTRADES,
        body, (size_t)n, gem_mytrades_done) != SUCCESS)
  {
    gem_deliver_fills_fail(r,
        "Error: failed to submit Gemini /v1/mytrades request");
    return(FAIL);
  }

  return(SUCCESS);
}
