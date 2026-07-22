// botmanager — MIT
// Yahoo Finance service plugin: keyless stock/ETF/fund/index/FX/commodity
// quotes. The first provider of the provider-neutral "stock_quotes"
// capability (include/stockquote.h). Pure mechanism — no command surface;
// the `stock` command binds to it via plugin_find_feature. All Yahoo
// nuance is sealed here.
#define STOCKQUOTE_PROVIDER_INTERNAL
#define YF_INTERNAL
#include "yahoofinance.h"

#include <stdio.h>
#include <stdlib.h>

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Percent-encode `in` into `out`, RFC-3986 unreserved set kept verbatim.
// `^` -> %5E and `=` -> %3D (indices, FX pairs); `-` `.` `_` `~` are
// unreserved so "BRK-B" and "EURUSD=X" survive intact. Returns the number
// of bytes the full encoding needs (may exceed cap, like snprintf).
static size_t
yf_urlencode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;

  if(cap == 0)
    return(0);

  for(const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++)
  {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                      || (c >= '0' && c <= '9')
                      || c == '-' || c == '_' || c == '.' || c == '~';

    if(unreserved)
    {
      if(n + 1 < cap)
        out[n] = (char)c;

      n++;
      continue;
    }

    if(n + 3 < cap)
    {
      out[n]     = '%';
      out[n + 1] = hex[c >> 4];
      out[n + 2] = hex[c & 0x0f];
    }

    n += 3;
  }

  out[n < cap ? n : cap - 1] = '\0';
  return(n);
}

// Reset a quote to the contract's "all absent" state: strings "", enums
// _UNKNOWN, every optional numeric NAN (so the command renders a dim "—"
// for anything the provider does not fill). status starts UNAVAILABLE and
// is promoted to a definite value on every code path.
static void
yf_quote_init(quote_t *q)
{
  memset(q, 0, sizeof(*q));

  q->status = QUOTE_UNAVAILABLE;

  q->price      = q->change   = q->change_pct = q->prev_close = NAN;
  q->open       = q->day_high = q->day_low     = q->volume    = NAN;
  q->year_high  = q->year_low = NAN;
  q->bid        = q->ask      = q->bid_size     = q->ask_size  = NAN;
  q->market_cap = q->pe_trailing = q->pe_forward = q->eps_ttm  = NAN;
  q->dividend_yield = q->avg_volume  = q->shares_out = NAN;
  q->ma50       = q->ma200    = q->beta         = NAN;
  q->session_price = q->session_change = q->session_change_pct = NAN;
}

// Trim surrounding whitespace and upper-case into `out` (bounded).
static void
yf_normalize_symbol(const char *in, char *out, size_t cap)
{
  const char *p = in;
  const char *end;
  size_t n = 0;

  if(cap == 0)
    return;

  while(*p == ' ' || *p == '\t')
    p++;

  end = p + strlen(p);

  while(end > p && (end[-1] == ' ' || end[-1] == '\t'))
    end--;

  for(; p < end && n + 1 < cap; p++)
    out[n++] = (char)toupper((unsigned char)*p);

  out[n] = '\0';
}

// Map Yahoo's instrumentType / quoteType string onto the neutral class.
static quote_class_t
yf_map_klass(const char *s)
{
  static const struct { const char *key; quote_class_t klass; } map[] = {
    { "EQUITY",         QUOTE_CLASS_EQUITY },
    { "ETF",            QUOTE_CLASS_ETF    },
    { "MUTUALFUND",     QUOTE_CLASS_FUND   },
    { "INDEX",          QUOTE_CLASS_INDEX  },
    { "CURRENCY",       QUOTE_CLASS_FX     },
    { "CRYPTOCURRENCY", QUOTE_CLASS_CRYPTO },
    { "FUTURE",         QUOTE_CLASS_FUTURE },
  };

  if(s == NULL || s[0] == '\0')
    return(QUOTE_CLASS_UNKNOWN);

  for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if(strcasecmp(s, map[i].key) == 0)
      return(map[i].klass);

  return(QUOTE_CLASS_UNKNOWN);
}

// Apply the browser User-Agent (bot UAs are blocked), per-request timeout,
// and JSON Accept header shared by every Yahoo request.
static void
yf_apply_common(curl_request_t *cr)
{
  const char *ua = kv_get_str("plugin.yahoofinance.user_agent");
  uint32_t    to = (uint32_t)kv_get_uint("plugin.yahoofinance.timeout_secs");

  if(ua != NULL && ua[0] != '\0')
    curl_request_set_user_agent(cr, ua);

  if(to > 0)
    curl_request_set_timeout(cr, to);

  curl_request_add_header(cr, "Accept: application/json");
}

// ----------------------------------------------------------------------
// TTL cache
// ----------------------------------------------------------------------

static bool
yf_cache_lookup(const char *sym, quote_t *out, time_t now, uint32_t ttl)
{
  bool hit = false;

  pthread_rwlock_rdlock(&yf_cache_rwl);

  for(size_t i = 0; i < YF_CACHE_SZ; i++)
  {
    if(yf_cache[i].t != 0
        && (now - yf_cache[i].t) < (time_t)ttl
        && strcmp(yf_cache[i].q.symbol, sym) == 0)
    {
      *out = yf_cache[i].q;
      hit  = true;
      break;
    }
  }

  pthread_rwlock_unlock(&yf_cache_rwl);
  return(hit);
}

static void
yf_cache_store(const quote_t *q)
{
  size_t slot = YF_CACHE_SZ;   // sentinel: no existing entry for this symbol

  pthread_rwlock_wrlock(&yf_cache_rwl);

  for(size_t i = 0; i < YF_CACHE_SZ; i++)
  {
    if(yf_cache[i].t != 0 && strcmp(yf_cache[i].q.symbol, q->symbol) == 0)
    {
      slot = i;
      break;
    }
  }

  if(slot == YF_CACHE_SZ)
  {
    slot            = yf_cache_cursor;
    yf_cache_cursor = (yf_cache_cursor + 1) % YF_CACHE_SZ;
  }

  yf_cache[slot].q = *q;
  yf_cache[slot].t = time(NULL);

  pthread_rwlock_unlock(&yf_cache_rwl);
}

// ----------------------------------------------------------------------
// JSON parse specs (v8 chart meta -> quote_t)
// ----------------------------------------------------------------------

// Direct-offset fields only. prev_close (fallback), name (fallback),
// priceHint (uint8_t narrowing) and instrumentType (enum map) are pulled
// by hand in yf_chart_done because none map to a plain typed offset.
static const json_spec_t yf_meta_spec[] = {
  { JSON_DOUBLE, "regularMarketPrice",   false, offsetof(quote_t, price) },
  { JSON_DOUBLE, "regularMarketDayHigh", false, offsetof(quote_t, day_high) },
  { JSON_DOUBLE, "regularMarketDayLow",  false, offsetof(quote_t, day_low) },
  { JSON_DOUBLE, "fiftyTwoWeekHigh",     false, offsetof(quote_t, year_high) },
  { JSON_DOUBLE, "fiftyTwoWeekLow",      false, offsetof(quote_t, year_low) },
  { JSON_DOUBLE, "regularMarketVolume",  false, offsetof(quote_t, volume) },
  { JSON_INT64,  "regularMarketTime",    false, offsetof(quote_t, as_of) },
  { JSON_STR,    "currency",             false, offsetof(quote_t, currency),
    .len = sizeof(((quote_t *)0)->currency) },
  { JSON_STR,    "fullExchangeName",     false, offsetof(quote_t, exchange),
    .len = sizeof(((quote_t *)0)->exchange) },
  { JSON_END }
};

// Downsample chart.result[0].indicators.quote[0].close[] into spark[],
// dropping JSON nulls. When more than 32 valid points survive, sample 32
// evenly-spaced values spanning both endpoints so the current price and
// the session open both land on the curve.
static void
yf_extract_spark(struct json_object *result0, quote_t *q)
{
  struct json_object *indicators;
  struct json_object *quote_arr;
  struct json_object *quote0;
  struct json_object *close_arr;
  float  vals[YF_SPARK_RAW];
  size_t len;
  size_t v = 0;

  indicators = json_get_obj(result0, "indicators");
  quote_arr  = indicators != NULL ? json_get_array(indicators, "quote") : NULL;
  quote0     = (quote_arr != NULL && json_object_array_length(quote_arr) > 0)
               ? json_object_array_get_idx(quote_arr, 0) : NULL;
  close_arr  = quote0 != NULL ? json_get_array(quote0, "close") : NULL;

  if(close_arr == NULL)
    return;

  len = (size_t)json_object_array_length(close_arr);

  for(size_t i = 0; i < len && v < YF_SPARK_RAW; i++)
  {
    struct json_object *c = json_object_array_get_idx(close_arr, (int)i);

    if(c == NULL || json_object_is_type(c, json_type_null))
      continue;

    vals[v++] = (float)json_object_get_double(c);
  }

  if(v == 0)
    return;

  if(v <= 32)
  {
    for(size_t i = 0; i < v; i++)
      q->spark[i] = vals[i];

    q->spark_n = (uint16_t)v;
    return;
  }

  for(size_t j = 0; j < 32; j++)
    q->spark[j] = vals[(j * (v - 1)) / 31];

  q->spark_n = 32;
}

// ----------------------------------------------------------------------
// Fan-out completion
// ----------------------------------------------------------------------

// Decrement the batch's pending count; the thread that observes the final
// 1 -> 0 transition delivers the assembled batch and frees it.
static void
yf_slot_done(yf_batch_t *b)
{
  if(atomic_fetch_sub(&b->n_pending, 1) == 1)
  {
    quote_batch_t r = {
      .status  = QUOTE_OK,
      .message = "",
      .quotes  = b->out,
      .n       = b->n_total,
    };

    b->cb(&r, b->user);
    mem_free(b);
  }
}

// One chart response -> one quote slot. Runs on the curl worker thread;
// each callback writes only its own out[slot], so no per-slot locking is
// needed (the cache and the pending counter carry their own).
static void
yf_chart_done(const curl_response_t *resp)
{
  yf_sub_t           *sub  = (yf_sub_t *)resp->user_data;
  yf_batch_t         *b    = sub->batch;
  quote_t            *q    = &b->out[sub->slot];
  struct json_object *root = NULL;
  struct json_object *chart;
  struct json_object *rarr;
  struct json_object *r0;
  struct json_object *meta;
  int32_t             ph;
  char                itype[24];

  if(resp->curl_code != 0)
  {
    q->status = QUOTE_TRANSPORT;
    goto done;
  }

  if(resp->status == 429)
  {
    q->status = QUOTE_RATE_LIMITED;
    goto done;
  }

  if(resp->status == 404)
  {
    q->status = QUOTE_NOT_FOUND;
    goto done;
  }

  if(resp->status != 200)
  {
    q->status = QUOTE_TRANSPORT;
    goto done;
  }

  root = json_parse_buf(resp->body, resp->body_len, YF_CTX);

  if(root == NULL)
  {
    q->status = QUOTE_TRANSPORT;
    goto done;
  }

  chart = json_get_obj(root, "chart");
  rarr  = chart != NULL ? json_get_array(chart, "result") : NULL;
  r0    = (rarr != NULL && json_object_array_length(rarr) > 0)
          ? json_object_array_get_idx(rarr, 0) : NULL;
  meta  = r0 != NULL ? json_get_obj(r0, "meta") : NULL;

  if(meta == NULL)
  {
    // chart.result is null and chart.error is set for unknown symbols.
    q->status = QUOTE_NOT_FOUND;
    goto done;
  }

  json_extract(meta, q, yf_meta_spec, YF_CTX ":meta");

  if(!json_get_double(meta, "chartPreviousClose", &q->prev_close))
    json_get_double(meta, "previousClose", &q->prev_close);

  if(!json_get_str(meta, "longName", q->name, sizeof(q->name)))
    json_get_str(meta, "shortName", q->name, sizeof(q->name));

  if(json_get_int(meta, "priceHint", &ph) && ph >= 0 && ph <= 10)
    q->price_decimals = (uint8_t)ph;

  if(json_get_str(meta, "instrumentType", itype, sizeof(itype)))
    q->klass = yf_map_klass(itype);

  // v8/chart never carries change directly; derive it from the close.
  if(isnan(q->change) && !isnan(q->price) && !isnan(q->prev_close)
      && q->prev_close != 0.0)
  {
    q->change     = q->price - q->prev_close;
    q->change_pct = (q->change / q->prev_close) * 100.0;
  }

  yf_extract_spark(r0, q);

  q->status = QUOTE_OK;
  yf_cache_store(q);

done:
  if(root != NULL)
    json_object_put(root);

  yf_slot_done(b);
  mem_free(sub);
}

// ----------------------------------------------------------------------
// Provider API — the four contract symbols
// ----------------------------------------------------------------------

uint32_t
stockquote_provider_caps(void)
{
  // MVP: symbol search only. Series + fundamentals/ext-hours/depth arrive
  // with STOCK-4. The inline spark[] rides every quote regardless.
  return(QUOTE_CAP_SEARCH);
}

bool
stockquote_fetch_async(const char *const *syms, uint8_t n,
    stockquote_batch_cb_t cb, void *user)
{
  const char *range;
  const char *interval;
  uint32_t    ttl;
  time_t      now;
  yf_batch_t *b;

  if(cb == NULL || syms == NULL)
    return(FAIL);

  if(n == 0 || n > YF_MAX_BATCH)
    return(FAIL);

  range    = kv_get_str("plugin.yahoofinance.spark_range");
  interval = kv_get_str("plugin.yahoofinance.spark_interval");
  ttl      = (uint32_t)kv_get_uint("plugin.yahoofinance.cache_ttl");

  if(range == NULL || range[0] == '\0')
    range = "1d";
  if(interval == NULL || interval[0] == '\0')
    interval = "5m";
  if(ttl == 0)
    ttl = 20;

  now = time(NULL);

  b = mem_alloc(YF_CTX, "batch", sizeof(*b));
  b->cb        = cb;
  b->user      = user;
  b->n_total   = n;
  b->n_pending = (uint8_t)(n + 1);   // +1 guard consumed after the loop

  for(uint8_t i = 0; i < n; i++)
  {
    quote_t        *q = &b->out[i];
    yf_sub_t       *sub;
    curl_request_t *cr;
    char            enc[YF_ENC_SZ];
    char            url[YF_URL_SZ];
    int             need;

    yf_quote_init(q);
    yf_normalize_symbol(syms[i] != NULL ? syms[i] : "", q->symbol,
        sizeof(q->symbol));

    if(q->symbol[0] == '\0')
    {
      q->status = QUOTE_NOT_FOUND;
      yf_slot_done(b);
      continue;
    }

    if(yf_cache_lookup(q->symbol, q, now, ttl))
    {
      yf_slot_done(b);   // cache hit already carries QUOTE_OK
      continue;
    }

    if(yf_urlencode(q->symbol, enc, sizeof(enc)) >= sizeof(enc))
    {
      q->status = QUOTE_NOT_FOUND;
      yf_slot_done(b);
      continue;
    }

    need = snprintf(url, sizeof(url), "%s%s?range=%s&interval=%s",
        YF_CHART_URL, enc, range, interval);

    if(need < 0 || (size_t)need >= sizeof(url))
    {
      q->status = QUOTE_TRANSPORT;
      yf_slot_done(b);
      continue;
    }

    sub = mem_alloc(YF_CTX, "sub", sizeof(*sub));
    sub->batch = b;
    sub->slot  = i;

    cr = curl_request_create(CURL_METHOD_GET, url, yf_chart_done, sub);

    if(cr == NULL)
    {
      mem_free(sub);
      q->status = QUOTE_TRANSPORT;
      yf_slot_done(b);
      continue;
    }

    yf_apply_common(cr);

    if(curl_request_submit(cr) != SUCCESS)
    {
      mem_free(sub);
      q->status = QUOTE_TRANSPORT;
      yf_slot_done(b);
      continue;
    }
  }

  yf_slot_done(b);   // consume the +1 guard
  return(SUCCESS);
}

static void
yf_search_done(const curl_response_t *resp)
{
  yf_search_req_t    *r    = (yf_search_req_t *)resp->user_data;
  struct json_object *root = NULL;
  struct json_object *jq;
  quote_hit_t        *hits = NULL;
  quote_search_res_t  res;
  size_t              len;
  uint8_t             kept = 0;

  memset(&res, 0, sizeof(res));
  res.status = QUOTE_OK;

  if(resp->curl_code != 0)
  {
    res.status = QUOTE_TRANSPORT;
    goto emit;
  }

  if(resp->status == 429)
  {
    res.status = QUOTE_RATE_LIMITED;
    goto emit;
  }

  if(resp->status != 200)
  {
    res.status = QUOTE_TRANSPORT;
    goto emit;
  }

  root = json_parse_buf(resp->body, resp->body_len, YF_CTX);

  if(root == NULL)
  {
    res.status = QUOTE_TRANSPORT;
    goto emit;
  }

  jq = json_get_array(root, "quotes");

  if(jq != NULL)
  {
    len = (size_t)json_object_array_length(jq);

    if(len > YF_SEARCH_MAX)
      len = YF_SEARCH_MAX;

    if(len > 0)
    {
      hits = mem_alloc(YF_CTX, "hits", len * sizeof(*hits));
      memset(hits, 0, len * sizeof(*hits));

      for(size_t i = 0; i < len; i++)
      {
        struct json_object *item = json_object_array_get_idx(jq, (int)i);
        char qtype[24];

        if(item == NULL || !json_object_is_type(item, json_type_object))
          continue;

        if(!json_get_str(item, "symbol", hits[kept].symbol,
            sizeof(hits[kept].symbol)))
          continue;   // a hit with no symbol is useless

        if(!json_get_str(item, "shortname", hits[kept].name,
            sizeof(hits[kept].name)))
          json_get_str(item, "longname", hits[kept].name,
              sizeof(hits[kept].name));

        json_get_str(item, "exchDisp", hits[kept].exchange,
            sizeof(hits[kept].exchange));

        if(json_get_str(item, "quoteType", qtype, sizeof(qtype)))
          hits[kept].klass = yf_map_klass(qtype);

        kept++;
      }
    }
  }

  res.hits = hits;
  res.n    = kept;

emit:
  if(r->cb != NULL)
    r->cb(&res, r->user);

  if(hits != NULL)
    mem_free(hits);

  if(root != NULL)
    json_object_put(root);

  mem_free(r);
}

bool
stockquote_search_async(const char *query, stockquote_search_cb_t cb,
    void *user)
{
  char             enc[YF_URL_SZ];
  char             url[YF_URL_SZ];
  int              need;
  curl_request_t  *cr;
  yf_search_req_t *r;

  if(cb == NULL || query == NULL || query[0] == '\0')
    return(FAIL);

  if(yf_urlencode(query, enc, sizeof(enc)) >= sizeof(enc))
    return(FAIL);

  need = snprintf(url, sizeof(url),
      "%s?q=%s&quotesCount=%d&newsCount=0", YF_SEARCH_URL, enc,
      YF_SEARCH_MAX);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  r = mem_alloc(YF_CTX, "search", sizeof(*r));
  r->cb   = cb;
  r->user = user;

  cr = curl_request_create(CURL_METHOD_GET, url, yf_search_done, r);

  if(cr == NULL)
  {
    mem_free(r);
    return(FAIL);
  }

  yf_apply_common(cr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(r);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
stockquote_series_async(const char *sym, quote_range_t range,
    stockquote_series_cb_t cb, void *user)
{
  // Standalone history is deferred to STOCK-4; not advertised in
  // provider_caps(), so a well-behaved consumer never calls this.
  (void)sym;
  (void)range;
  (void)cb;
  (void)user;

  return(FAIL);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
yf_init(void)
{
  pthread_rwlock_init(&yf_cache_rwl, NULL);
  memset(yf_cache, 0, sizeof(yf_cache));
  yf_cache_cursor = 0;

  clam(CLAM_INFO, YF_CTX, "yahoofinance plugin initialized");
  return(SUCCESS);
}

static void
yf_deinit(void)
{
  pthread_rwlock_destroy(&yf_cache_rwl);
  clam(CLAM_INFO, YF_CTX, "yahoofinance plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = YF_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = YF_CTX,
  .provides        = { { .name = "stock_quotes" },
                       { .name = "service_yahoofinance" } },
  .provides_count  = 2,
  .requires_count  = 0,
  .kv_schema       = yf_kv_schema,
  .kv_schema_count = sizeof(yf_kv_schema) / sizeof(yf_kv_schema[0]),
  .init            = yf_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = yf_deinit,
  .ext             = NULL,
};
