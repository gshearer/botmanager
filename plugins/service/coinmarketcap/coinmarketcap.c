// botmanager — MIT
// CoinMarketCap service plugin: JSON lookups against the CMC REST API.
// Pure mechanism — no command surface. Consumers call the public API
// in coinmarketcap_api.h via plugin_dlsym.
#define _GNU_SOURCE
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

// Metadata cache. Slots are matched case-insensitively by symbol; a
// write reuses the matching slot, then the first empty one, and only
// then evicts the least recently fetched.

static bool
cmc_info_lookup(const char *symbol, cmc_info_t *out)
{
  uint32_t ttl   = (uint32_t)kv_get_uint("plugin.coinmarketcap.info_ttl");
  bool     found = false;

  if(symbol == NULL || symbol[0] == '\0')
    return(false);

  if(ttl == 0)
    ttl = 86400;

  pthread_mutex_lock(&cmc_info_mu);

  for(size_t i = 0; i < CMC_INFO_CACHE_N; i++)
  {
    const cmc_info_slot_t *s = &cmc_info_cache[i];

    if(s->symbol[0] == '\0' || strcasecmp(s->symbol, symbol) != 0)
      continue;

    if((time(NULL) - s->fetched) < (time_t)ttl)
    {
      *out  = s->info;
      found = true;
    }

    break;
  }

  pthread_mutex_unlock(&cmc_info_mu);

  return(found);
}

static void
cmc_info_store(const char *symbol, const cmc_info_t *info)
{
  size_t victim = 0;
  time_t oldest;

  if(symbol == NULL || symbol[0] == '\0')
    return;

  pthread_mutex_lock(&cmc_info_mu);

  oldest = cmc_info_cache[0].fetched;

  for(size_t i = 0; i < CMC_INFO_CACHE_N; i++)
  {
    if(cmc_info_cache[i].symbol[0] == '\0' ||
       strcasecmp(cmc_info_cache[i].symbol, symbol) == 0)
    {
      victim = i;
      break;
    }

    if(cmc_info_cache[i].fetched < oldest)
    {
      oldest = cmc_info_cache[i].fetched;
      victim = i;
    }
  }

  snprintf(cmc_info_cache[victim].symbol,
      sizeof(cmc_info_cache[victim].symbol), "%s", symbol);
  cmc_info_cache[victim].info    = *info;
  cmc_info_cache[victim].fetched = time(NULL);

  pthread_mutex_unlock(&cmc_info_mu);
}

// Tidies a URL for a chat line without breaking it: the scheme stays
// (an IRC client only linkifies what it can open), a bare host gains
// https://, and the trailing slash goes.
// "https://bitcoin.org/" -> "https://bitcoin.org".
static void
cmc_url_clean(const char *url, char *out, size_t cap)
{
  const bool schemed = (url != NULL &&
      (strncasecmp(url, "https://", 8) == 0 ||
       strncasecmp(url, "http://",  7) == 0));
  size_t     len;

  if(url == NULL || url[0] == '\0' || cap == 0)
    return;

  snprintf(out, cap, "%s%s", schemed ? "" : "https://", url);

  len = strlen(out);

  // Never strip past the scheme's own "//".
  while(len > 0 && out[len - 1] == '/' && out[len - 2] != '/')
    out[--len] = '\0';
}

// Turns one element of the Info response into a cmc_info_t. The shape
// mixes scalars, a string array (tag-names) and an array of nested
// objects (contract_address), so it is walked by hand rather than
// described as a json_spec_t.
static void
cmc_info_parse(struct json_object *item, cmc_info_t *out)
{
  struct json_object *tags;
  struct json_object *urls;
  struct json_object *contracts;

  memset(out, 0, sizeof(*out));

  json_get_str(item, "category", out->category, sizeof(out->category));
  json_get_str(item, "subreddit", out->subreddit, sizeof(out->subreddit));
  json_get_str(item, "twitter_username", out->twitter, sizeof(out->twitter));
  json_get_bool(item, "infinite_supply", &out->infinite_supply);

  tags = json_get_array(item, "tag-names");

  if(tags != NULL)
  {
    size_t n = json_object_array_length(tags);

    for(size_t i = 0; i < n && out->tag_count < COINMARKETCAP_MAX_TAGS; i++)
    {
      const char *t = json_object_get_string(
          json_object_array_get_idx(tags, i));

      if(t == NULL || t[0] == '\0')
        continue;

      snprintf(out->tags[out->tag_count], COINMARKETCAP_TAG_SZ, "%s", t);
      out->tag_count++;
    }
  }

  urls = json_get_obj(item, "urls");

  if(urls != NULL)
  {
    struct json_object *web = json_get_array(urls, "website");

    // The list is not ordered by usefulness — Uniswap leads with a blog
    // post — and the shortest entry is reliably the project's root.
    if(web != NULL)
    {
      size_t      n    = json_object_array_length(web);
      const char *best = NULL;

      for(size_t i = 0; i < n; i++)
      {
        const char *u = json_object_get_string(
            json_object_array_get_idx(web, i));

        if(u != NULL && u[0] != '\0' &&
            (best == NULL || strlen(u) < strlen(best)))
          best = u;
      }

      cmc_url_clean(best, out->website, sizeof(out->website));
    }
  }

  // Every chain the token is deployed on. The count is kept whole even
  // though only the first few names are stored, so a reply can say how
  // many were left out.
  contracts = json_get_array(item, "contract_address");

  if(contracts != NULL)
  {
    size_t n = json_object_array_length(contracts);

    out->chain_total = (uint8_t)(n > 255 ? 255 : n);

    for(size_t i = 0; i < n && out->chain_count < COINMARKETCAP_MAX_CHAINS;
        i++)
    {
      struct json_object *plat = json_get_obj(
          json_object_array_get_idx(contracts, i), "platform");
      char                name[COINMARKETCAP_CHAIN_SZ] = { 0 };

      if(plat == NULL || !json_get_str(plat, "name", name, sizeof(name)))
        continue;

      snprintf(out->chains[out->chain_count], COINMARKETCAP_CHAIN_SZ,
          "%s", name);
      out->chain_count++;
    }
  }

  out->valid = true;
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

// PLIFE-5: these were two `__thread` scalars — the only thread-local
// storage anywhere in the tree, and the one thing that could have made
// this mapping unsafe to drop. qsort_r carries the same two values on
// the stack, so the plugin now holds nothing that outlives a call.
typedef struct
{
  uint8_t col;
  bool    rev;
} cmc_sort_ctx_t;

static int
cmc_coin_cmp(const void *a, const void *b, void *arg)
{
  const cmc_coin_t     *ca  = (const cmc_coin_t *)a;
  const cmc_coin_t     *cb  = (const cmc_coin_t *)b;
  const cmc_sort_ctx_t *ctx = arg;
  int result = 0;

  switch(ctx->col)
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

  return(ctx->rev ? -result : result);
}

// Global JSON specs (unchanged shape).

static const json_spec_t cmc_global_usd_spec[] = {
  { JSON_DOUBLE, "total_market_cap",           false,
      offsetof(cmc_global_t, total_cap) },
  { JSON_DOUBLE, "total_volume_24h",           false,
      offsetof(cmc_global_t, total_vol) },
  { JSON_DOUBLE, "total_volume_24h_reported",  false,
      offsetof(cmc_global_t, total_vol_reported) },
  { JSON_DOUBLE, "total_market_cap_yesterday", false,
      offsetof(cmc_global_t, total_cap_yest) },
  { JSON_DOUBLE, "total_volume_24h_yesterday", false,
      offsetof(cmc_global_t, total_vol_yest) },
  { JSON_DOUBLE, "total_market_cap_yesterday_percentage_change", false,
      offsetof(cmc_global_t, total_cap_chg_24h) },
  { JSON_DOUBLE, "total_volume_24h_yesterday_percentage_change", false,
      offsetof(cmc_global_t, total_vol_chg_24h) },
  { JSON_DOUBLE, "altcoin_market_cap",         false,
      offsetof(cmc_global_t, altcoin_cap) },
  { JSON_DOUBLE, "altcoin_volume_24h",         false,
      offsetof(cmc_global_t, altcoin_vol_24h) },
  { JSON_END }
};

static const json_spec_t cmc_global_quote_spec[] = {
  { JSON_OBJ, "USD", false, 0, .sub = cmc_global_usd_spec },
  { JSON_END }
};

static const json_spec_t cmc_global_spec[] = {
  { JSON_INT,    "active_cryptocurrencies",false,
      offsetof(cmc_global_t, active_cryptos) },
  { JSON_INT,    "total_cryptocurrencies", false,
      offsetof(cmc_global_t, total_cryptos) },
  { JSON_INT,    "active_exchanges",       false,
      offsetof(cmc_global_t, active_exchanges) },
  { JSON_INT,    "total_exchanges",        false,
      offsetof(cmc_global_t, total_exchanges) },
  { JSON_INT,    "active_market_pairs",    false,
      offsetof(cmc_global_t, active_market_pairs) },
  { JSON_INT,    "past_24h_incremental_crypto_number", false,
      offsetof(cmc_global_t, new_cryptos_24h) },
  { JSON_DOUBLE, "btc_dominance",          false,
      offsetof(cmc_global_t, btc_dom) },
  { JSON_DOUBLE, "eth_dominance",          false,
      offsetof(cmc_global_t, eth_dom) },
  { JSON_DOUBLE, "btc_dominance_24h_percentage_change", false,
      offsetof(cmc_global_t, btc_dom_chg_24h) },
  { JSON_DOUBLE, "eth_dominance_24h_percentage_change", false,
      offsetof(cmc_global_t, eth_dom_chg_24h) },
  { JSON_DOUBLE, "defi_volume_24h",        false,
      offsetof(cmc_global_t, defi_vol_24h) },
  { JSON_DOUBLE, "defi_market_cap",        false,
      offsetof(cmc_global_t, defi_cap) },
  { JSON_DOUBLE, "defi_24h_percentage_change", false,
      offsetof(cmc_global_t, defi_chg_24h) },
  { JSON_DOUBLE, "stablecoin_volume_24h",  false,
      offsetof(cmc_global_t, stablecoin_vol) },
  { JSON_DOUBLE, "stablecoin_market_cap",  false,
      offsetof(cmc_global_t, stablecoin_cap) },
  { JSON_DOUBLE, "stablecoin_24h_percentage_change", false,
      offsetof(cmc_global_t, stablecoin_chg_24h) },
  { JSON_DOUBLE, "derivatives_volume_24h", false,
      offsetof(cmc_global_t, derivatives_vol) },
  { JSON_DOUBLE, "derivatives_24h_percentage_change", false,
      offsetof(cmc_global_t, derivatives_chg_24h) },
  { JSON_OBJ,    "quote",                  false, 0,
      .sub = cmc_global_quote_spec },
  { JSON_END }
};

static void
cmc_global_cache_store(struct json_object *jdata)
{
  memset(&cmc_global_cache, 0, sizeof(cmc_global_cache));
  json_extract(jdata, &cmc_global_cache, cmc_global_spec, CMC_CTX ":global");
  cmc_global_cache_time         = time(NULL);
  cmc_global_cache.fetched_at   = (int64_t)cmc_global_cache_time;
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
    return(cmc_abort_unsent(req,
        "Error: failed to create API request"));

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
    return(cmc_abort_unsent(req,
        "Error: failed to submit API request"));

  return(SUCCESS);
}

// Metadata leg of a detail request. Whatever happens here the request
// continues on to the quote — a card without tags is still a card — so
// every path ends in cmc_submit_quotes and none of them delivers a
// failure to the consumer.
static void
cmc_info_done(const curl_response_t *resp)
{
  char                errbuf[CMC_ERR_SZ];
  const char         *err;
  struct json_object *root;
  struct json_object *jdata = NULL;
  struct json_object *item  = NULL;
  cmc_request_t      *r     = (cmc_request_t *)resp->user_data;

  err = cmc_classify_http(resp, errbuf, sizeof(errbuf));

  if(err != NULL)
  {
    clam(CLAM_DEBUG, CMC_CTX, "info fetch failed for %s: %s", r->symbol, err);
    cmc_submit_quotes(r, true);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, CMC_CTX);

  if(root == NULL || !json_object_object_get_ex(root, "data", &jdata) ||
      jdata == NULL)
  {
    clam(CLAM_DEBUG, CMC_CTX, "info response unusable for %s", r->symbol);

    if(root != NULL)
      json_object_put(root);

    cmc_submit_quotes(r, true);
    return;
  }

  // data is keyed by the requested symbol and holds an array of matches.
  {
    struct json_object_iterator it  = json_object_iter_begin(jdata);
    struct json_object_iterator end = json_object_iter_end(jdata);

    if(!json_object_iter_equal(&it, &end))
      item = json_object_iter_peek_value(&it);
  }

  if(item != NULL && json_object_is_type(item, json_type_array))
    item = json_object_array_length(item) > 0
        ? json_object_array_get_idx(item, 0) : NULL;

  if(item != NULL)
  {
    cmc_info_t info;

    cmc_info_parse(item, &info);
    cmc_info_store(r->symbol, &info);
  }

  json_object_put(root);
  cmc_submit_quotes(r, true);
}

static bool
cmc_submit_info(cmc_request_t *req)
{
  curl_request_t *cr;
  char            hdr[CMC_HDR_SZ];
  char            url[CMC_URL_SZ];

  snprintf(url, sizeof(url), CMC_INFO_URL "?symbol=%s", req->symbol);

  cr = curl_request_create(CURL_METHOD_GET, url, cmc_info_done, req);

  if(cr == NULL)
    return(cmc_submit_quotes(req, false));

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
    return(cmc_submit_quotes(req, false));

  return(SUCCESS);
}

// A request that never reached the wire, dropped while its caller is
// still on the stack. The header promises that on FAIL the callback is
// NOT invoked — the caller owns the user-facing message — so firing it
// here as well is what makes a consumer reply twice and free its
// closure twice. The reason survives in the log instead.
static bool
cmc_abort_unsent(cmc_request_t *req, const char *err)
{
  clam(CLAM_WARN, CMC_CTX, "request dropped before submit: %s", err);
  cmc_req_release(req);

  return(FAIL);
}

// Detail requests have a second leg: once the metadata fetch has
// returned, the caller is long gone and the consumer is reachable only
// through its callback. `deliver` distinguishes the two cases.
static bool
cmc_detail_abort(cmc_request_t *req, bool deliver, const char *err)
{
  if(deliver)
  {
    cmc_deliver_detail_fail(req, err);
    return(FAIL);
  }

  return(cmc_abort_unsent(req, err));
}

static bool
cmc_submit_quotes(cmc_request_t *req, bool deliver_on_fail)
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
      return(cmc_detail_abort(req, deliver_on_fail,
          "Error: rank lookup requires cached data."));

    snprintf(url, sizeof(url),
        CMC_QUOTES_URL "?id=%d&convert=USD", coin_id);
  }

  cr = curl_request_create(
      CURL_METHOD_GET, url, cmc_quotes_done, req);

  if(cr == NULL)
    return(cmc_detail_abort(req, deliver_on_fail,
        "Error: failed to create API request"));

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
    return(cmc_abort_unsent(req,
        "Error: failed to create API request"));

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
    return(cmc_abort_unsent(req,
        "Error: failed to submit API request"));

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

  // Metadata is decoration: whatever is cached by now rides along, and
  // a miss simply leaves res.info.valid false. A rank lookup only ever
  // hits here if some earlier request warmed the same symbol.
  cmc_info_lookup(res.detail.base.symbol, &res.info);

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

  apikey = kv_get_creds("plugin.coinmarketcap.creds.apikey");

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

  {
    cmc_sort_ctx_t ctx = { .col = sort_col, .rev = reverse };

    qsort_r(out_arr, n, sizeof(cmc_coin_t), cmc_coin_cmp, &ctx);
  }

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
  const char *apikey = kv_get_creds("plugin.coinmarketcap.creds.apikey");
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
  const char *apikey = kv_get_creds("plugin.coinmarketcap.creds.apikey");
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

  // A symbol we have no metadata for takes the long way round: fetch
  // the description first, then the quote. Metadata barely changes, so
  // this second call is paid roughly once a day per coin.
  {
    cmc_info_t cached;

    if(r->symbol[0] != '\0' && !cmc_info_lookup(r->symbol, &cached))
      return(cmc_submit_info(r));
  }

  return(cmc_submit_quotes(r, false));
}

bool
coinmarketcap_fetch_global_async(
    coinmarketcap_done_global_cb_t done_cb, void *user)
{
  const char *apikey = kv_get_creds("plugin.coinmarketcap.creds.apikey");
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
  const char *apikey = kv_get_creds("plugin.coinmarketcap.creds.apikey");

  return(apikey != NULL && apikey[0] != '\0');
}

// Plugin lifecycle

static bool
cmc_init(void)
{
  pthread_mutex_init(&cmc_free_mu, NULL);
  pthread_mutex_init(&cmc_info_mu, NULL);
  pthread_rwlock_init(&cmc_cache_rwl, NULL);
  memset(cmc_cache, 0, sizeof(cmc_cache));
  memset(cmc_info_cache, 0, sizeof(cmc_info_cache));
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
  pthread_mutex_destroy(&cmc_info_mu);

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
