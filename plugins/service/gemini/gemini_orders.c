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
// cancel order, order status, active orders, mytrades).
#define GEM_INTERNAL
#include "gemini.h"

#include "exchange_api.h"
#include "gemini_pairs.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

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

  if(b == NULL)
    return(NULL);

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

  gem_pairs_add(r->detail_symbol, base, quote);

  pthread_mutex_lock(&b->lock);
  b->kept++;
  pthread_mutex_unlock(&b->lock);

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

    if(dr == NULL)
      continue;

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

  if(r == NULL)
  {
    gem_batch_free(b);
    return(FAIL);
  }

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
