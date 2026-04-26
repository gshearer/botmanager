// botmanager — MIT
// CoinMarketCap service plugin: JSON lookups against the CMC REST API.
// Pure mechanism — no command surface. Consumers call the public API
// in coinmarketcap_api.h via plugin_dlsym.
#define CMC_INTERNAL
#include "coinmarketcap.h"

#include <stdio.h>
#include <stdlib.h>

// Freelist helpers

static cmc_request_t *
cmc_req_alloc(void)
{
  cmc_request_t *r = NULL;

  pthread_mutex_lock(&cmc_free_mu);

  if(cmc_free != NULL)
  {
    r = cmc_free;
    cmc_free = r->next;
  }

  pthread_mutex_unlock(&cmc_free_mu);

  if(r == NULL)
    r = mem_alloc(CMC_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

static void
cmc_req_release(cmc_request_t *r)
{
  pthread_mutex_lock(&cmc_free_mu);
  r->next = cmc_free;
  cmc_free = r;
  pthread_mutex_unlock(&cmc_free_mu);
}

// Cache helpers

static bool
cmc_cache_valid(void)
{
  uint32_t ttl;
  if(cmc_cache_count == 0)
    return(false);

  ttl = (uint32_t)kv_get_uint("plugin.coinmarketcap.cache_ttl");

  if(ttl == 0)
    ttl = 60;

  return((time(NULL) - cmc_cache_time) < (time_t)ttl);
}

static bool
cmc_global_cache_valid(void)
{
  uint32_t ttl;
  if(cmc_global_cache_time == 0)
    return(false);

  ttl = (uint32_t)kv_get_uint("plugin.coinmarketcap.cache_ttl");

  if(ttl == 0)
    ttl = 60;

  return((time(NULL) - cmc_global_cache_time) < (time_t)ttl);
}

// JSON specs: drive cmc_coin_t and cmc_coin_detail_t population from the
// CoinMarketCap response shape. The USD sub-spec writes into the base
// cmc_coin_t (offset 0 on JSON_OBJ) so one json_extract call fills the
// whole struct.

static const json_spec_t cmc_usd_spec[] = {
  { JSON_DOUBLE, "price",             false, offsetof(cmc_coin_t, price) },
  { JSON_DOUBLE, "market_cap",        false, offsetof(cmc_coin_t, market_cap) },
  { JSON_DOUBLE, "volume_24h",        false, offsetof(cmc_coin_t, volume_24h) },
  { JSON_DOUBLE, "percent_change_1h", false, offsetof(cmc_coin_t, pct_1h) },
  { JSON_DOUBLE, "percent_change_24h",false, offsetof(cmc_coin_t, pct_24h) },
  { JSON_DOUBLE, "percent_change_7d", false, offsetof(cmc_coin_t, pct_7d) },
  { JSON_END }
};

static const json_spec_t cmc_quote_spec[] = {
  { JSON_OBJ, "USD", false, 0, .sub = cmc_usd_spec },
  { JSON_END }
};

static const json_spec_t cmc_coin_spec[] = {
  { JSON_INT,    "id",              true,  offsetof(cmc_coin_t, id) },
  { JSON_INT,    "cmc_rank",        false, offsetof(cmc_coin_t, cmc_rank) },
  { JSON_INT,    "num_market_pairs",false, offsetof(cmc_coin_t, num_market_pairs) },
  { JSON_STR,    "name",            true,  offsetof(cmc_coin_t, name),
    .len = sizeof(((cmc_coin_t *)0)->name) },
  { JSON_STR,    "symbol",          true,  offsetof(cmc_coin_t, symbol),
    .len = sizeof(((cmc_coin_t *)0)->symbol) },
  { JSON_DOUBLE, "circulating_supply", false, offsetof(cmc_coin_t, circulating_supply) },
  { JSON_DOUBLE, "total_supply",       false, offsetof(cmc_coin_t, total_supply) },
  { JSON_DOUBLE, "max_supply",         false, offsetof(cmc_coin_t, max_supply) },
  { JSON_OBJ,    "quote",              false, 0, .sub = cmc_quote_spec },
  { JSON_END }
};

// Verbose-only fields (Quotes Latest returns more detail per coin).
static const json_spec_t cmc_usd_detail_spec[] = {
  { JSON_DOUBLE, "market_cap_dominance",     false,
      offsetof(cmc_coin_detail_t, market_cap_dominance) },
  { JSON_DOUBLE, "fully_diluted_market_cap", false,
      offsetof(cmc_coin_detail_t, fully_diluted_market_cap) },
  { JSON_DOUBLE, "volume_change_24h",        false,
      offsetof(cmc_coin_detail_t, volume_change_24h) },
  { JSON_DOUBLE, "percent_change_30d",       false,
      offsetof(cmc_coin_detail_t, pct_30d) },
  { JSON_DOUBLE, "percent_change_60d",       false,
      offsetof(cmc_coin_detail_t, pct_60d) },
  { JSON_DOUBLE, "percent_change_90d",       false,
      offsetof(cmc_coin_detail_t, pct_90d) },
  { JSON_END }
};

static const json_spec_t cmc_quote_detail_spec[] = {
  { JSON_OBJ, "USD", false, 0, .sub = cmc_usd_detail_spec },
  { JSON_END }
};

static const json_spec_t cmc_coin_detail_extra_spec[] = {
  { JSON_STR, "date_added", false,
    offsetof(cmc_coin_detail_t, date_added),
    .len = sizeof(((cmc_coin_detail_t *)0)->date_added) },
  { JSON_OBJ, "quote", false, 0, .sub = cmc_quote_detail_spec },
  { JSON_END }
};

// Populate the listings cache from a parsed JSON data array.
// Caller must hold the cache write lock.
static void
cmc_cache_populate(struct json_object *data_arr)
{
  int len = (int)json_object_array_length(data_arr);

  if(len > COINMARKETCAP_MAX_LISTINGS)
    len = COINMARKETCAP_MAX_LISTINGS;

  for(int i = 0; i < len; i++)
  {
    struct json_object *item = json_object_array_get_idx(data_arr, i);

    if(item == NULL)
      continue;

    json_extract(item, &cmc_cache[i], cmc_coin_spec, CMC_CTX ":coin");
  }

  cmc_cache_count = (uint32_t)len;
  cmc_cache_time  = time(NULL);

  clam(CLAM_DEBUG2, CMC_CTX, "cache populated: %u coins", cmc_cache_count);
}

// Sort comparator

// Per-thread sort parameters (set before qsort, read by comparator).
static __thread uint8_t cmc_sort_col_tl;
static __thread bool    cmc_sort_rev_tl;

static int
cmc_coin_cmp(const void *a, const void *b)
{
  const cmc_coin_t *ca = (const cmc_coin_t *)a;
  const cmc_coin_t *cb = (const cmc_coin_t *)b;
  int result = 0;

  switch(cmc_sort_col_tl)
  {
    case COINMARKETCAP_SORT_RANK:
      result = (ca->cmc_rank > cb->cmc_rank) - (ca->cmc_rank < cb->cmc_rank);
      break;

    case COINMARKETCAP_SORT_SYMBOL:
      result = strcasecmp(ca->symbol, cb->symbol);
      break;

    case COINMARKETCAP_SORT_PRICE:
      result = (ca->price > cb->price) - (ca->price < cb->price);
      break;

    case COINMARKETCAP_SORT_CAP:
      result = (ca->market_cap > cb->market_cap)
          - (ca->market_cap < cb->market_cap);
      break;

    case COINMARKETCAP_SORT_1H:
      result = (ca->pct_1h > cb->pct_1h) - (ca->pct_1h < cb->pct_1h);
      break;

    case COINMARKETCAP_SORT_24H:
      result = (ca->pct_24h > cb->pct_24h) - (ca->pct_24h < cb->pct_24h);
      break;

    case COINMARKETCAP_SORT_7D:
      result = (ca->pct_7d > cb->pct_7d) - (ca->pct_7d < cb->pct_7d);
      break;

    case COINMARKETCAP_SORT_VOL:
      result = (ca->volume_24h > cb->volume_24h)
          - (ca->volume_24h < cb->volume_24h);
      break;

    default:
      result = (ca->cmc_rank > cb->cmc_rank) - (ca->cmc_rank < cb->cmc_rank);
      break;
  }

  return(cmc_sort_rev_tl ? -result : result);
}

// Global JSON specs (unchanged shape).

static const json_spec_t cmc_global_usd_spec[] = {
  { JSON_DOUBLE, "total_market_cap",           false,
      offsetof(cmc_global_t, total_cap) },
  { JSON_DOUBLE, "total_volume_24h",           false,
      offsetof(cmc_global_t, total_vol) },
  { JSON_DOUBLE, "total_market_cap_yesterday", false,
      offsetof(cmc_global_t, total_cap_yest) },
  { JSON_DOUBLE, "total_volume_24h_yesterday", false,
      offsetof(cmc_global_t, total_vol_yest) },
  { JSON_END }
};

static const json_spec_t cmc_global_quote_spec[] = {
  { JSON_OBJ, "USD", false, 0, .sub = cmc_global_usd_spec },
  { JSON_END }
};

static const json_spec_t cmc_global_spec[] = {
  { JSON_INT,    "active_cryptocurrencies",false,
      offsetof(cmc_global_t, active_cryptos) },
  { JSON_INT,    "active_exchanges",       false,
      offsetof(cmc_global_t, active_exchanges) },
  { JSON_DOUBLE, "btc_dominance",          false,
      offsetof(cmc_global_t, btc_dom) },
  { JSON_DOUBLE, "eth_dominance",          false,
      offsetof(cmc_global_t, eth_dom) },
  { JSON_DOUBLE, "defi_volume_24h",        false,
      offsetof(cmc_global_t, defi_vol_24h) },
  { JSON_DOUBLE, "defi_market_cap",        false,
      offsetof(cmc_global_t, defi_cap) },
  { JSON_DOUBLE, "stablecoin_volume_24h",  false,
      offsetof(cmc_global_t, stablecoin_vol) },
  { JSON_DOUBLE, "stablecoin_market_cap",  false,
      offsetof(cmc_global_t, stablecoin_cap) },
  { JSON_DOUBLE, "derivatives_volume_24h", false,
      offsetof(cmc_global_t, derivatives_vol) },
  { JSON_OBJ,    "quote",                  false, 0,
      .sub = cmc_global_quote_spec },
  { JSON_END }
};

static void
cmc_global_cache_store(struct json_object *jdata)
{
  memset(&cmc_global_cache, 0, sizeof(cmc_global_cache));
  json_extract(jdata, &cmc_global_cache, cmc_global_spec, CMC_CTX ":global");
  cmc_global_cache_time = time(NULL);
}

// Callback-dispatch helpers. On failure these fire the typed callback
// with err set; on poll requests no callback fires.

static void
cmc_deliver_listings_fail(cmc_request_t *r, const char *err)
{
  coinmarketcap_listings_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(!r->is_poll && r->cb.listings != NULL)
    r->cb.listings(&res, r->user);

  cmc_req_release(r);
}

static void
cmc_deliver_detail_fail(cmc_request_t *r, const char *err)
{
  coinmarketcap_detail_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(!r->is_poll && r->cb.detail != NULL)
    r->cb.detail(&res, r->user);

  cmc_req_release(r);
}

static void
cmc_deliver_global_fail(cmc_request_t *r, const char *err)
{
  coinmarketcap_global_result_t res = { 0 };

  snprintf(res.err, sizeof(res.err), "%s", err);

  if(!r->is_poll && r->cb.global != NULL)
    r->cb.global(&res, r->user);

  cmc_req_release(r);
}

// Translate a curl_response_t + HTTP status into a human-readable error
// string suitable for forwarding. Returns NULL if the response is OK.
static const char *
cmc_classify_http(const curl_response_t *resp, char *buf, size_t sz)
{
  if(resp->curl_code != 0)
  {
    snprintf(buf, sz, "CoinMarketCap API error: %s", resp->error);
    return(buf);
  }

  switch(resp->status)
  {
    case 200:
      return(NULL);

    case 400:
      return("Error: cryptocurrency not found or invalid request");

    case 401:
      return("Error: invalid CoinMarketCap API key");

    case 402:
    case 403:
      return("Error: CoinMarketCap plan limit reached or permission denied");

    case 429:
      return("Error: CoinMarketCap API rate limit exceeded, try again later");

    default:
      snprintf(buf, sz, "CoinMarketCap API returned HTTP %ld", resp->status);
      return(buf);
  }
}

// HTTP submit helpers

static bool
cmc_submit_listings(cmc_request_t *req)
{
  curl_request_t *cr;
  char hdr[CMC_HDR_SZ];
  char url[CMC_URL_SZ];

  snprintf(url, sizeof(url),
      CMC_LISTINGS_URL "?limit=%u&sort=market_cap&sort_dir=desc&convert=USD",
      COINMARKETCAP_MAX_LISTINGS);

  cr = curl_request_create(
      CURL_METHOD_GET, url, cmc_listings_done, req);

  if(cr == NULL)
  {
    cmc_deliver_listings_fail(req, "Error: failed to create API request");
    return(FAIL);
  }

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    cmc_deliver_listings_fail(req, "Error: failed to submit API request");
    return(FAIL);
  }

  return(SUCCESS);
}

static bool
cmc_submit_quotes(cmc_request_t *req)
{
  curl_request_t *cr;
  char hdr[CMC_HDR_SZ];
  char url[CMC_URL_SZ];

  if(req->symbol[0] != '\0')
  {
    snprintf(url, sizeof(url),
        CMC_QUOTES_URL "?symbol=%s&convert=USD", req->symbol);
  }
  else
  {
    // Rank path: resolve to an ID from the cache.
    int32_t coin_id = 0;

    pthread_rwlock_rdlock(&cmc_cache_rwl);

    for(uint32_t i = 0; i < cmc_cache_count; i++)
    {
      if(cmc_cache[i].cmc_rank == req->rank)
      {
        coin_id = cmc_cache[i].id;
        break;
      }
    }

    pthread_rwlock_unlock(&cmc_cache_rwl);

    if(coin_id <= 0)
    {
      cmc_deliver_detail_fail(req,
          "Error: rank lookup requires cached data.");
      return(FAIL);
    }

    snprintf(url, sizeof(url),
        CMC_QUOTES_URL "?id=%d&convert=USD", coin_id);
  }

  cr = curl_request_create(
      CURL_METHOD_GET, url, cmc_quotes_done, req);

  if(cr == NULL)
  {
    cmc_deliver_detail_fail(req, "Error: failed to create API request");
    return(FAIL);
  }

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    cmc_deliver_detail_fail(req, "Error: failed to submit API request");
    return(FAIL);
  }

  return(SUCCESS);
}

static bool
cmc_submit_global(cmc_request_t *req)
{
  char hdr[CMC_HDR_SZ];
  curl_request_t *cr = curl_request_create(
      CURL_METHOD_GET,
      CMC_GLOBAL_URL "?convert=USD",
      cmc_global_done, req);

  if(cr == NULL)
  {
    cmc_deliver_global_fail(req, "Error: failed to create API request");
    return(FAIL);
  }

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    cmc_deliver_global_fail(req, "Error: failed to submit API request");
    return(FAIL);
  }

  return(SUCCESS);
}

// Curl callbacks

static void
cmc_listings_done(const curl_response_t *resp)
{
  char errbuf[CMC_ERR_SZ];
  const char *err;
  struct json_object *root;
  struct json_object *jdata;
  coinmarketcap_listings_result_t res = { 0 };
  cmc_request_t *r = (cmc_request_t *)resp->user_data;

  err = cmc_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cmc_deliver_listings_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CMC_CTX);

  if(root == NULL)
  {
    cmc_deliver_listings_fail(r,
        "Error: malformed JSON from CoinMarketCap API");
    return;
  }

  jdata = NULL;

  if(!json_object_object_get_ex(root, "data", &jdata) || jdata == NULL
      || !json_object_is_type(jdata, json_type_array))
  {
    json_object_put(root);
    cmc_deliver_listings_fail(r,
        "Error: unexpected CoinMarketCap API response format");
    return;
  }

  pthread_rwlock_wrlock(&cmc_cache_rwl);
  cmc_cache_populate(jdata);
  pthread_rwlock_unlock(&cmc_cache_rwl);

  if(!r->is_poll && r->cb.listings != NULL)
    r->cb.listings(&res, r->user);

  json_object_put(root);
  cmc_req_release(r);
}

static void
cmc_quotes_done(const curl_response_t *resp)
{
  char errbuf[CMC_ERR_SZ];
  const char *err;
  struct json_object *root;
  struct json_object *jdata = NULL;
  struct json_object *item = NULL;
  coinmarketcap_detail_result_t res = { 0 };
  cmc_request_t *r = (cmc_request_t *)resp->user_data;

  err = cmc_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cmc_deliver_detail_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CMC_CTX);

  if(root == NULL)
  {
    cmc_deliver_detail_fail(r,
        "Error: malformed JSON from CoinMarketCap API");
    return;
  }

  if(!json_object_object_get_ex(root, "data", &jdata) || jdata == NULL)
  {
    json_object_put(root);
    cmc_deliver_detail_fail(r,
        "Error: unexpected API response format");
    return;
  }

  // The data object is keyed by ID or symbol; grab the first value.
  {
    struct json_object_iterator it  = json_object_iter_begin(jdata);
    struct json_object_iterator end = json_object_iter_end(jdata);

    if(!json_object_iter_equal(&it, &end))
      item = json_object_iter_peek_value(&it);
  }

  if(item == NULL)
  {
    json_object_put(root);
    cmc_deliver_detail_fail(r, "Cryptocurrency not found.");
    return;
  }

  // Symbol-keyed responses return an array of matches.
  if(json_object_is_type(item, json_type_array))
  {
    if(json_object_array_length(item) == 0)
    {
      json_object_put(root);
      cmc_deliver_detail_fail(r, "Cryptocurrency not found.");
      return;
    }

    item = json_object_array_get_idx(item, 0);
  }

  json_extract(item, &res.detail.base, cmc_coin_spec, CMC_CTX ":coin");
  json_extract(item, &res.detail,
      cmc_coin_detail_extra_spec, CMC_CTX ":detail");

  if(!r->is_poll && r->cb.detail != NULL)
    r->cb.detail(&res, r->user);

  json_object_put(root);
  cmc_req_release(r);
}

static void
cmc_global_done(const curl_response_t *resp)
{
  char errbuf[CMC_ERR_SZ];
  const char *err;
  struct json_object *root;
  struct json_object *jdata = NULL;
  coinmarketcap_global_result_t res = { 0 };
  cmc_request_t *r = (cmc_request_t *)resp->user_data;

  err = cmc_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    cmc_deliver_global_fail(r, err);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CMC_CTX);

  if(root == NULL)
  {
    cmc_deliver_global_fail(r,
        "Error: malformed JSON from CoinMarketCap API");
    return;
  }

  if(!json_object_object_get_ex(root, "data", &jdata) || jdata == NULL)
  {
    json_object_put(root);
    cmc_deliver_global_fail(r,
        "Error: unexpected API response format");
    return;
  }

  pthread_rwlock_wrlock(&cmc_cache_rwl);
  cmc_global_cache_store(jdata);
  // Snapshot into the result payload while we hold the lock.
  res.global = cmc_global_cache;
  pthread_rwlock_unlock(&cmc_cache_rwl);

  if(!r->is_poll && r->cb.global != NULL)
    r->cb.global(&res, r->user);

  json_object_put(root);
  cmc_req_release(r);
}

// Polling

static void
cmc_poll_tick(task_t *t)
{
  const char *apikey;
  cmc_request_t *r;
  cmc_request_t *g;

  if(!(uint8_t)kv_get_uint("plugin.coinmarketcap.poll"))
  {
    clam(CLAM_INFO, CMC_CTX, "polling disabled, stopping poll task");
    // Cancel self so task_finish treats the TASK_ENDED return below
    // as terminal instead of rescheduling the periodic.
    task_cancel(cmc_poll_task);
    cmc_poll_task = TASK_HANDLE_NONE;
    t->state = TASK_ENDED;
    return;
  }

  apikey = kv_get_str("plugin.coinmarketcap.apikey");

  if(apikey == NULL || apikey[0] == '\0')
  {
    clam(CLAM_DEBUG2, CMC_CTX, "poll: no API key configured, skipping");
    t->state = TASK_ENDED;
    return;
  }

  r = cmc_req_alloc();
  r->type    = CMC_REQ_LISTINGS;
  r->is_poll = true;
  snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

  clam(CLAM_DEBUG2, CMC_CTX, "poll: refreshing listings cache");

  if(cmc_submit_listings(r) != SUCCESS)
    clam(CLAM_WARN, CMC_CTX, "poll: failed to submit listings request");

  g = cmc_req_alloc();
  g->type    = CMC_REQ_GLOBAL;
  g->is_poll = true;
  snprintf(g->apikey, sizeof(g->apikey), "%s", apikey);

  clam(CLAM_DEBUG2, CMC_CTX, "poll: refreshing global stats cache");

  if(cmc_submit_global(g) != SUCCESS)
    clam(CLAM_WARN, CMC_CTX, "poll: failed to submit global request");

  t->state = TASK_ENDED;
}

// Public API implementation

bool
coinmarketcap_get_coin_by_symbol(const char *symbol,
    coinmarketcap_coin_t *out)
{
  bool found = false;

  if(symbol == NULL || symbol[0] == '\0' || out == NULL)
    return(FAIL);

  pthread_rwlock_rdlock(&cmc_cache_rwl);

  for(uint32_t i = 0; i < cmc_cache_count; i++)
  {
    if(strcasecmp(cmc_cache[i].symbol, symbol) == 0)
    {
      *out  = cmc_cache[i];
      found = true;
      break;
    }
  }

  pthread_rwlock_unlock(&cmc_cache_rwl);

  return(found ? SUCCESS : FAIL);
}

bool
coinmarketcap_get_coin_by_rank(int32_t rank, coinmarketcap_coin_t *out)
{
  bool found = false;

  if(out == NULL)
    return(FAIL);

  pthread_rwlock_rdlock(&cmc_cache_rwl);

  for(uint32_t i = 0; i < cmc_cache_count; i++)
  {
    if(cmc_cache[i].cmc_rank == rank)
    {
      *out  = cmc_cache[i];
      found = true;
      break;
    }
  }

  pthread_rwlock_unlock(&cmc_cache_rwl);

  return(found ? SUCCESS : FAIL);
}

bool
coinmarketcap_get_listings(uint32_t limit, uint8_t sort_col, bool reverse,
    coinmarketcap_coin_t *out_arr, uint32_t out_cap, uint32_t *out_count)
{
  uint32_t n;

  if(out_arr == NULL || out_count == NULL || out_cap == 0)
    return(FAIL);

  *out_count = 0;

  pthread_rwlock_rdlock(&cmc_cache_rwl);

  if(cmc_cache_count == 0)
  {
    pthread_rwlock_unlock(&cmc_cache_rwl);
    return(FAIL);
  }

  n = cmc_cache_count;

  if(limit > 0 && n > limit)
    n = limit;
  if(n > out_cap)
    n = out_cap;

  memcpy(out_arr, cmc_cache, n * sizeof(cmc_coin_t));

  pthread_rwlock_unlock(&cmc_cache_rwl);

  cmc_sort_col_tl = sort_col;
  cmc_sort_rev_tl = reverse;
  qsort(out_arr, n, sizeof(cmc_coin_t), cmc_coin_cmp);

  *out_count = n;

  return(SUCCESS);
}

bool
coinmarketcap_get_global(coinmarketcap_global_t *out)
{
  bool fresh = false;

  if(out == NULL)
    return(FAIL);

  pthread_rwlock_rdlock(&cmc_cache_rwl);

  if(cmc_global_cache_time != 0)
  {
    *out  = cmc_global_cache;
    fresh = true;
  }

  pthread_rwlock_unlock(&cmc_cache_rwl);

  return(fresh ? SUCCESS : FAIL);
}

bool
coinmarketcap_listings_cache_fresh(void)
{
  bool fresh;

  pthread_rwlock_rdlock(&cmc_cache_rwl);
  fresh = cmc_cache_valid();
  pthread_rwlock_unlock(&cmc_cache_rwl);

  return(fresh);
}

bool
coinmarketcap_global_cache_fresh(void)
{
  bool fresh;

  pthread_rwlock_rdlock(&cmc_cache_rwl);
  fresh = cmc_global_cache_valid();
  pthread_rwlock_unlock(&cmc_cache_rwl);

  return(fresh);
}

bool
coinmarketcap_fetch_listings_async(
    coinmarketcap_done_listings_cb_t done_cb, void *user)
{
  const char *apikey = kv_get_str("plugin.coinmarketcap.apikey");
  cmc_request_t *r;

  if(apikey == NULL || apikey[0] == '\0')
    return(FAIL);

  r = cmc_req_alloc();
  r->type         = CMC_REQ_LISTINGS;
  r->cb.listings  = done_cb;
  r->user         = user;
  snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

  return(cmc_submit_listings(r));
}

bool
coinmarketcap_fetch_detail_async(const char *symbol, int32_t rank,
    coinmarketcap_done_detail_cb_t done_cb, void *user)
{
  const char *apikey = kv_get_str("plugin.coinmarketcap.apikey");
  cmc_request_t *r;

  if(apikey == NULL || apikey[0] == '\0')
    return(FAIL);

  // Exactly one of symbol/rank must be supplied.
  if((symbol == NULL || symbol[0] == '\0') && rank <= 0)
    return(FAIL);

  r = cmc_req_alloc();
  r->type      = CMC_REQ_DETAIL;
  r->cb.detail = done_cb;
  r->user      = user;
  snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

  if(symbol != NULL && symbol[0] != '\0')
    snprintf(r->symbol, sizeof(r->symbol), "%s", symbol);
  else
    r->rank = rank;

  return(cmc_submit_quotes(r));
}

bool
coinmarketcap_fetch_global_async(
    coinmarketcap_done_global_cb_t done_cb, void *user)
{
  const char *apikey = kv_get_str("plugin.coinmarketcap.apikey");
  cmc_request_t *r;

  if(apikey == NULL || apikey[0] == '\0')
    return(FAIL);

  r = cmc_req_alloc();
  r->type      = CMC_REQ_GLOBAL;
  r->cb.global = done_cb;
  r->user      = user;
  snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

  return(cmc_submit_global(r));
}

uint32_t
coinmarketcap_default_limit_kv_value(void)
{
  uint32_t v = (uint32_t)kv_get_uint("plugin.coinmarketcap.default_limit");

  return(v > 1 ? v : 12);
}

bool
coinmarketcap_apikey_configured(void)
{
  const char *apikey = kv_get_str("plugin.coinmarketcap.apikey");

  return(apikey != NULL && apikey[0] != '\0');
}

// Plugin lifecycle

static bool
cmc_init(void)
{
  pthread_mutex_init(&cmc_free_mu, NULL);
  pthread_rwlock_init(&cmc_cache_rwl, NULL);
  memset(cmc_cache, 0, sizeof(cmc_cache));
  memset(&cmc_global_cache, 0, sizeof(cmc_global_cache));

  clam(CLAM_INFO, CMC_CTX, "coinmarketcap plugin initialized");

  return(SUCCESS);
}

static void
cmc_kv_changed(const char *key, void *data)
{
  uint8_t poll;
  (void)data;

  if(strcmp(key, "plugin.coinmarketcap.poll") != 0)
    return;

  poll = (uint8_t)kv_get_uint("plugin.coinmarketcap.poll");

  if(poll && cmc_poll_task == TASK_HANDLE_NONE)
  {
    uint32_t ttl = (uint32_t)kv_get_uint("plugin.coinmarketcap.cache_ttl");

    if(ttl == 0)
      ttl = 60;

    cmc_poll_task = task_add_periodic("cmc_poll", TASK_THREAD,
        200, ttl * 1000, cmc_poll_tick, NULL);

    clam(CLAM_INFO, CMC_CTX, "polling enabled via KV change");
  }
  else if(!poll && cmc_poll_task != TASK_HANDLE_NONE)
    clam(CLAM_INFO, CMC_CTX, "polling will stop at next tick (KV change)");
}

static bool
cmc_start(void)
{
  kv_set_cb("plugin.coinmarketcap.poll", cmc_kv_changed, NULL);

  if((uint8_t)kv_get_uint("plugin.coinmarketcap.poll"))
  {
    uint32_t ttl = (uint32_t)kv_get_uint("plugin.coinmarketcap.cache_ttl");

    if(ttl == 0)
      ttl = 60;

    cmc_poll_task = task_add_periodic("cmc_poll", TASK_THREAD,
        200, ttl * 1000, cmc_poll_tick, NULL);
  }

  return(SUCCESS);
}

static void
cmc_deinit(void)
{
  kv_set_cb("plugin.coinmarketcap.poll", NULL, NULL);

  pthread_mutex_lock(&cmc_free_mu);

  while(cmc_free != NULL)
  {
    cmc_request_t *r = cmc_free;

    cmc_free = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&cmc_free_mu);
  pthread_mutex_destroy(&cmc_free_mu);

  pthread_rwlock_destroy(&cmc_cache_rwl);

  clam(CLAM_INFO, CMC_CTX, "coinmarketcap plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "coinmarketcap",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "coinmarketcap",
  .provides        = { { .name = "service_coinmarketcap" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = cmc_kv_schema,
  .kv_schema_count = sizeof(cmc_kv_schema) / sizeof(cmc_kv_schema[0]),
  .init            = cmc_init,
  .start           = cmc_start,
  .stop            = NULL,
  .deinit          = cmc_deinit,
  .ext             = NULL,
};
