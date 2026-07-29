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

// need_spark rejects an otherwise-live entry that carries no sparkline.
// The two tiers cache quotes of different shapes — v7 has fundamentals
// and no series, v8/chart has a series and no fundamentals — so a plain
// `!stock AAPL` filling the cache from v7 must not then rob a `-v AAPL`
// seconds later of the curve it exists to draw.
static bool
yf_cache_lookup(const char *sym, quote_t *out, time_t now, uint32_t ttl,
    bool need_spark)
{
  bool hit = false;

  pthread_rwlock_rdlock(&yf_cache_rwl);

  for(size_t i = 0; i < YF_CACHE_SZ; i++)
  {
    if(yf_cache[i].t != 0
        && (now - yf_cache[i].t) < (time_t)ttl
        && (!need_spark || yf_cache[i].q.spark_n > 0)
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
// v7 session — the cookie + crumb Yahoo wants before it will answer
// v7/finance/quote. Two hops, minted once for everybody: the state
// machine below guarantees that N concurrent quotes trigger ONE mint and
// all of them wait on its single answer. Nothing here is on a critical
// path — a session that cannot be minted only costs the enrichment, and
// every quote still completes on the keyless v8 tier.
// ----------------------------------------------------------------------

static bool
yf_enrich_enabled(void)
{
  return(kv_get_int("plugin.yahoofinance.enrich") != 0);
}

// A crumb is a short opaque token, and the only thing that makes it one
// is that it parses as one — the endpoint returns bare text with no
// envelope to check. A throttled mint answers "Too Many Requests" as the
// body (429, observed live 2026-07-29), which the status check catches;
// this guard is what stops any similar prose arriving under a status we
// do trust from being installed as a crumb and then blamed on the quote
// endpoint. Whitespace or a control byte means refusal, not token.
static bool
yf_crumb_plausible(const char *s)
{
  size_t n;

  if(s == NULL)
    return(false);

  n = strnlen(s, YF_CRUMB_SZ);

  if(n == 0 || n >= YF_CRUMB_SZ)
    return(false);

  for(size_t i = 0; i < n; i++)
    if(isspace((unsigned char)s[i]) || (unsigned char)s[i] < 0x20)
      return(false);

  return(true);
}

// Park p on the queue; caller must hold yf_sess_lock.
static void
yf_sess_enqueue_locked(yf_pend_t *p)
{
  p->next       = yf_sess_queue;
  yf_sess_queue = p;
}

// Decide what p should do about the session, and take the queue slot or
// start the mint if that is the answer. On YF_ACQ_READY the live cookie
// and crumb are copied out under the lock, so the caller never reads
// module state a re-mint could be rewriting.
static yf_acquire_t
yf_session_acquire(yf_pend_t *p, char *cookie, size_t cookie_cap,
    char *crumb, size_t crumb_cap)
{
  yf_acquire_t act;
  uint32_t     ttl;
  time_t       now   = time(NULL);
  bool         start = false;

  ttl = (uint32_t)kv_get_uint("plugin.yahoofinance.session_ttl");

  if(ttl == 0)
    ttl = 3600;

  pthread_mutex_lock(&yf_sess_lock);

  // An aged-out crumb is no better than none; drop to NONE so the
  // arms below treat it as a fresh mint.
  if(yf_sess_state == YF_SESS_READY && (now - yf_sess_minted) >= (time_t)ttl)
    yf_sess_state = YF_SESS_NONE;

  if(yf_sess_state == YF_SESS_READY)
  {
    snprintf(cookie, cookie_cap, "%s", yf_sess_cookie);
    snprintf(crumb,  crumb_cap,  "%s", yf_sess_crumb);
    act = YF_ACQ_READY;
  }

  else if(yf_sess_state == YF_SESS_MINTING)
  {
    yf_sess_enqueue_locked(p);
    act = YF_ACQ_QUEUED;
  }

  else if(yf_sess_shutdown
      || (yf_sess_state == YF_SESS_FAILED && now < yf_sess_retry_at))
  {
    // Backing off. Do not queue — the answer is "not today".
    act = YF_ACQ_NONE;
  }

  else
  {
    yf_sess_state = YF_SESS_MINTING;
    yf_sess_enqueue_locked(p);
    start = true;
    act   = YF_ACQ_QUEUED;
  }

  pthread_mutex_unlock(&yf_sess_lock);

  if(start)
    yf_session_start_mint();

  return(act);
}

static void
yf_session_publish(const char *cookie, const char *crumb)
{
  pthread_mutex_lock(&yf_sess_lock);

  snprintf(yf_sess_cookie, sizeof(yf_sess_cookie), "%s", cookie);
  snprintf(yf_sess_crumb,  sizeof(yf_sess_crumb),  "%s", crumb);
  yf_sess_minted = time(NULL);
  yf_sess_state  = YF_SESS_READY;

  pthread_mutex_unlock(&yf_sess_lock);

  clam(CLAM_INFO, YF_CTX, "v7 session minted (crumb %zu bytes)",
      strlen(crumb));
}

// Enter the backoff. Quotes keep flowing on v8 the whole time, so this
// is INFO-with-a-reason rather than an error anybody must act on.
static void
yf_session_fail(const char *why)
{
  uint32_t backoff;

  backoff = (uint32_t)kv_get_uint("plugin.yahoofinance.session_backoff");

  // A zero backoff would let every quote restart a mint that just
  // failed; the enrichment is not worth a retry storm.
  if(backoff == 0)
    backoff = 900;

  pthread_mutex_lock(&yf_sess_lock);

  yf_sess_state     = YF_SESS_FAILED;
  yf_sess_retry_at  = time(NULL) + (time_t)backoff;
  yf_sess_cookie[0] = '\0';
  yf_sess_crumb[0]  = '\0';

  pthread_mutex_unlock(&yf_sess_lock);

  clam(CLAM_INFO, YF_CTX,
      "v7 session unavailable (%s); serving keyless v8 for %us", why,
      backoff);

  yf_session_drain();
}

// Drop a session the quote endpoint just refused. Deliberately NOT the
// FAILED backoff: a rejected crumb is the one case where an immediate
// re-mint is the right move, and the single retry is spent by the
// caller's yf_pend_t.retried.
static void
yf_session_invalidate(void)
{
  pthread_mutex_lock(&yf_sess_lock);

  if(yf_sess_state == YF_SESS_READY)
  {
    yf_sess_state     = YF_SESS_NONE;
    yf_sess_cookie[0] = '\0';
    yf_sess_crumb[0]  = '\0';
  }

  pthread_mutex_unlock(&yf_sess_lock);
}

static yf_pend_t *
yf_session_take_queue(void)
{
  yf_pend_t *q;

  pthread_mutex_lock(&yf_sess_lock);
  q = yf_sess_queue;
  yf_sess_queue = NULL;
  pthread_mutex_unlock(&yf_sess_lock);

  return(q);
}

// Hand every parked set of slots back to yf_dispatch, which re-reads the
// (now settled) session state and picks v7 or v8 accordingly.
static void
yf_session_drain(void)
{
  yf_pend_t *q = yf_session_take_queue();

  while(q != NULL)
  {
    yf_pend_t *next = q->next;

    q->next = NULL;
    yf_dispatch(q);
    q = next;
  }
}

static void
yf_session_start_mint(void)
{
  curl_request_t *cr;
  yf_mint_t      *m;

  m = mem_alloc(YF_CTX, "mint", sizeof(*m));
  m->cookie[0] = '\0';

  cr = curl_request_create(CURL_METHOD_GET, YF_COOKIE_URL, yf_cookie_done, m);

  if(cr == NULL)
  {
    mem_free(m);
    yf_session_fail("cookie request not created");
    return;
  }

  yf_apply_common(cr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(m);
    yf_session_fail("cookie request not submitted");
  }
}

// Hop 1. fc.yahoo.com is not an API — it answers 404 by design and the
// Set-Cookie IS the payload, so the status is deliberately not checked.
static void
yf_cookie_done(const curl_response_t *resp)
{
  yf_mint_t      *m = (yf_mint_t *)resp->user_data;
  curl_request_t *cr;
  char            hdr[CURL_COOKIE_SZ + 16];

  if(resp->set_cookie == NULL || resp->set_cookie[0] == '\0')
  {
    mem_free(m);
    yf_session_fail("no Set-Cookie from the cookie endpoint");
    return;
  }

  // core already reduced the response's Set-Cookie lines to their
  // cookie-pairs, joined ready to send straight back.
  snprintf(m->cookie, sizeof(m->cookie), "%s", resp->set_cookie);
  snprintf(hdr, sizeof(hdr), "Cookie: %s", m->cookie);

  cr = curl_request_create(CURL_METHOD_GET, YF_CRUMB_URL, yf_crumb_done, m);

  if(cr == NULL)
  {
    mem_free(m);
    yf_session_fail("crumb request not created");
    return;
  }

  yf_apply_common(cr);
  curl_request_add_header(cr, hdr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(m);
    yf_session_fail("crumb request not submitted");
  }
}

// Hop 2. The body is the bare token — no JSON envelope.
static void
yf_crumb_done(const curl_response_t *resp)
{
  yf_mint_t *m = (yf_mint_t *)resp->user_data;
  char       crumb[YF_CRUMB_SZ];
  char       why[64];
  size_t     n;

  if(resp->curl_code != 0 || resp->status != 200 || resp->body == NULL)
  {
    snprintf(why, sizeof(why), "crumb endpoint HTTP %ld", resp->status);
    mem_free(m);
    yf_session_fail(why);
    return;
  }

  // Truncating would be the wrong kindness here: a body too long to be
  // a crumb is some other thing entirely, and its first 63 bytes would
  // pass every test below while being useless as a token.
  if(resp->body_len >= sizeof(crumb))
  {
    mem_free(m);
    yf_session_fail("crumb endpoint returned an over-long body");
    return;
  }

  snprintf(crumb, sizeof(crumb), "%.*s", (int)resp->body_len, resp->body);

  n = strlen(crumb);

  while(n > 0 && isspace((unsigned char)crumb[n - 1]))
    crumb[--n] = '\0';

  if(!yf_crumb_plausible(crumb))
  {
    mem_free(m);
    yf_session_fail("crumb endpoint returned prose, not a token");
    return;
  }

  yf_session_publish(m->cookie, crumb);
  mem_free(m);
  yf_session_drain();
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
// JSON parse spec (v7 quote item -> quote_t)
// ----------------------------------------------------------------------

// v7 gives the whole quote in one item, so unlike the chart meta this
// spec carries the core block too. Fields with no plain typed offset —
// name, priceHint, quoteType, marketState, the pre/post session block
// and the two dividend spellings — are pulled by hand in yf_apply_v7.
static const json_spec_t yf_v7_spec[] = {
  { JSON_DOUBLE, "regularMarketPrice",         false, offsetof(quote_t, price) },
  { JSON_DOUBLE, "regularMarketChange",        false, offsetof(quote_t, change) },
  { JSON_DOUBLE, "regularMarketChangePercent", false, offsetof(quote_t, change_pct) },
  { JSON_DOUBLE, "regularMarketPreviousClose", false, offsetof(quote_t, prev_close) },
  { JSON_DOUBLE, "regularMarketOpen",          false, offsetof(quote_t, open) },
  { JSON_DOUBLE, "regularMarketDayHigh",       false, offsetof(quote_t, day_high) },
  { JSON_DOUBLE, "regularMarketDayLow",        false, offsetof(quote_t, day_low) },
  { JSON_DOUBLE, "regularMarketVolume",        false, offsetof(quote_t, volume) },
  { JSON_DOUBLE, "fiftyTwoWeekHigh",           false, offsetof(quote_t, year_high) },
  { JSON_DOUBLE, "fiftyTwoWeekLow",            false, offsetof(quote_t, year_low) },
  { JSON_INT64,  "regularMarketTime",          false, offsetof(quote_t, as_of) },

  // depth
  { JSON_DOUBLE, "bid",                        false, offsetof(quote_t, bid) },
  { JSON_DOUBLE, "ask",                        false, offsetof(quote_t, ask) },
  { JSON_DOUBLE, "bidSize",                    false, offsetof(quote_t, bid_size) },
  { JSON_DOUBLE, "askSize",                    false, offsetof(quote_t, ask_size) },

  // fundamentals
  { JSON_DOUBLE, "marketCap",                  false, offsetof(quote_t, market_cap) },
  { JSON_DOUBLE, "trailingPE",                 false, offsetof(quote_t, pe_trailing) },
  { JSON_DOUBLE, "forwardPE",                  false, offsetof(quote_t, pe_forward) },
  { JSON_DOUBLE, "epsTrailingTwelveMonths",    false, offsetof(quote_t, eps_ttm) },
  { JSON_DOUBLE, "averageDailyVolume3Month",   false, offsetof(quote_t, avg_volume) },
  { JSON_DOUBLE, "sharesOutstanding",          false, offsetof(quote_t, shares_out) },
  { JSON_DOUBLE, "fiftyDayAverage",            false, offsetof(quote_t, ma50) },
  { JSON_DOUBLE, "twoHundredDayAverage",       false, offsetof(quote_t, ma200) },

  { JSON_STR,    "currency",                   false, offsetof(quote_t, currency),
    .len = sizeof(((quote_t *)0)->currency) },
  { JSON_STR,    "fullExchangeName",           false, offsetof(quote_t, exchange),
    .len = sizeof(((quote_t *)0)->exchange) },
  { JSON_END }
};

// Map Yahoo's marketState onto the neutral enum. PREPRE/POSTPOST are
// Yahoo's "session exists but has not opened / has closed for good"
// markers; neither carries live extended-hours prices, so both read as
// CLOSED rather than inventing a session the card would then draw.
static market_state_t
yf_map_state(const char *s)
{
  static const struct { const char *key; market_state_t st; } map[] = {
    { "PRE",      MARKET_STATE_PRE    },
    { "REGULAR",  MARKET_STATE_OPEN   },
    { "POST",     MARKET_STATE_POST   },
    { "PREPRE",   MARKET_STATE_CLOSED },
    { "POSTPOST", MARKET_STATE_CLOSED },
    { "CLOSED",   MARKET_STATE_CLOSED },
  };

  if(s == NULL || s[0] == '\0')
    return(MARKET_STATE_UNKNOWN);

  for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if(strcasecmp(s, map[i].key) == 0)
      return(map[i].st);

  return(MARKET_STATE_UNKNOWN);
}

// One v7 result item -> one quote. Leaves q->symbol alone: it is the
// caller's normalized request symbol, and the slot was matched on it.
static void
yf_apply_v7(struct json_object *item, quote_t *q)
{
  char    s[24];
  int32_t ph;
  double  d;

  json_extract(item, q, yf_v7_spec, YF_CTX ":v7");

  if(!json_get_str(item, "longName", q->name, sizeof(q->name)))
    json_get_str(item, "shortName", q->name, sizeof(q->name));

  if(json_get_int(item, "priceHint", &ph) && ph >= 0 && ph <= 10)
    q->price_decimals = (uint8_t)ph;

  if(json_get_str(item, "quoteType", s, sizeof(s)))
    q->klass = yf_map_klass(s);

  // dividendYield is already a percent; the trailing spelling is a
  // fraction of price. Normalize both to percent so the card never has
  // to know which one answered.
  if(!json_get_double(item, "dividendYield", &q->dividend_yield)
      && json_get_double(item, "trailingAnnualDividendYield", &d))
    q->dividend_yield = d * 100.0;

  // Extended hours. Yahoo names the block by session, and only the
  // session named by marketState carries live values.
  if(json_get_str(item, "marketState", s, sizeof(s)))
    q->market_state = yf_map_state(s);

  if(q->market_state == MARKET_STATE_PRE)
  {
    json_get_double(item, "preMarketPrice",         &q->session_price);
    json_get_double(item, "preMarketChange",        &q->session_change);
    json_get_double(item, "preMarketChangePercent", &q->session_change_pct);
  }

  else if(q->market_state == MARKET_STATE_POST)
  {
    json_get_double(item, "postMarketPrice",         &q->session_price);
    json_get_double(item, "postMarketChange",        &q->session_change);
    json_get_double(item, "postMarketChangePercent", &q->session_change_pct);
  }

  // Same derivation the chart path uses, for the rare item that carries
  // a price and a previous close but no change.
  if(isnan(q->change) && !isnan(q->price) && !isnan(q->prev_close)
      && q->prev_close != 0.0)
  {
    q->change     = q->price - q->prev_close;
    q->change_pct = (q->change / q->prev_close) * 100.0;
  }
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
    // Both hops of a merge are now finished — the atomic that got us
    // here orders their writes before this read — so the sparkline can
    // join the quote it belongs to, and the cache can finally hold one
    // entry carrying BOTH tiers.
    if(b->spark_merge && b->spark_n > 0 && b->out[0].status == QUOTE_OK)
    {
      memcpy(b->out[0].spark, b->spark, sizeof(b->out[0].spark));
      b->out[0].spark_n = b->spark_n;
      yf_cache_store(&b->out[0]);
    }

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
  quote_t             scratch;
  quote_t            *q;
  struct json_object *root = NULL;
  struct json_object *chart;
  struct json_object *rarr;
  struct json_object *r0;
  struct json_object *meta;
  int32_t             ph;
  char                itype[24];

  // The spark hop of a merge runs beside a v7 hop that owns out[slot],
  // so it parses into a scratch quote and hands over nothing but the
  // series. Everything below is then identical for both roles.
  if(sub->spark_only)
  {
    yf_quote_init(&scratch);
    snprintf(scratch.symbol, sizeof(scratch.symbol), "%s",
        b->out[sub->slot].symbol);
    q = &scratch;
  }

  else
    q = &b->out[sub->slot];

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

  // A spark hop caches nothing: its scratch quote has no fundamentals,
  // and storing it would evict the richer v7 entry for the same symbol.
  // The merged quote is cached by yf_slot_done instead.
  if(!sub->spark_only)
    yf_cache_store(q);

done:
  if(sub->spark_only)
  {
    memcpy(b->spark, q->spark, sizeof(b->spark));
    b->spark_n = q->spark_n;
  }

  if(root != NULL)
    json_object_put(root);

  yf_slot_done(b);
  mem_free(sub);
}

// ----------------------------------------------------------------------
// Dispatch — which tier answers a set of slots
// ----------------------------------------------------------------------

// One chart request for one slot. spark_only marks the merge hop.
static bool
yf_submit_chart(yf_batch_t *b, uint8_t slot, const char *sym, bool spark_only)
{
  const char     *range;
  const char     *interval;
  yf_sub_t       *sub;
  curl_request_t *cr;
  char            enc[YF_ENC_SZ];
  char            url[YF_URL_SZ];
  int             need;

  range    = kv_get_str("plugin.yahoofinance.spark_range");
  interval = kv_get_str("plugin.yahoofinance.spark_interval");

  if(range == NULL || range[0] == '\0')
    range = "1d";

  if(interval == NULL || interval[0] == '\0')
    interval = "5m";

  if(yf_urlencode(sym, enc, sizeof(enc)) >= sizeof(enc))
    return(FAIL);

  need = snprintf(url, sizeof(url), "%s%s?range=%s&interval=%s",
      YF_CHART_URL, enc, range, interval);

  if(need < 0 || (size_t)need >= sizeof(url))
    return(FAIL);

  sub = mem_alloc(YF_CTX, "sub", sizeof(*sub));
  sub->batch      = b;
  sub->slot       = slot;
  sub->spark_only = spark_only;

  cr = curl_request_create(CURL_METHOD_GET, url, yf_chart_done, sub);

  if(cr == NULL)
  {
    mem_free(sub);
    return(FAIL);
  }

  yf_apply_common(cr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(sub);
    return(FAIL);
  }

  return(SUCCESS);
}

// The keyless tier: one chart request per slot. Always available, and
// the answer to every way the session tier can be unavailable.
static void
yf_dispatch_v8(yf_batch_t *b, const uint8_t *slots, uint8_t n_slots)
{
  for(uint8_t i = 0; i < n_slots; i++)
  {
    uint8_t  s = slots[i];
    quote_t *q = &b->out[s];

    if(yf_submit_chart(b, s, q->symbol, false) != SUCCESS)
    {
      q->status = QUOTE_TRANSPORT;
      yf_slot_done(b);
    }
  }
}

// The session tier: ONE request for every slot in p, and the only
// source of fundamentals, depth and extended hours. Consumes p.
static void
yf_dispatch_v7(yf_pend_t *p, const char *cookie, const char *crumb)
{
  yf_batch_t     *b = p->batch;
  curl_request_t *cr;
  char            syms[YF_SYMLIST_SZ];
  char            enc_crumb[YF_CRUMB_SZ * 3];
  char            url[YF_URL_SZ];
  char            hdr[CURL_COOKIE_SZ + 16];
  uint8_t         keep[YF_MAX_BATCH];
  uint8_t         spill[YF_MAX_BATCH];
  uint8_t         n_keep  = 0;
  uint8_t         n_spill = 0;
  size_t          used    = 0;
  int             need;

  for(uint8_t i = 0; i < p->n_slots; i++)
  {
    uint8_t s = p->slots[i];
    char    enc[YF_ENC_SZ];
    int     wrote;

    if(yf_urlencode(b->out[s].symbol, enc, sizeof(enc)) >= sizeof(enc))
    {
      b->out[s].status = QUOTE_NOT_FOUND;
      yf_slot_done(b);
      continue;
    }

    wrote = snprintf(syms + used, sizeof(syms) - used, "%s%s",
        used > 0 ? "," : "", enc);

    // Out of room in the symbol list — the batch cap makes this
    // unreachable in practice, but a slot that misses this request must
    // ride the keyless tier, not come back as an absent symbol.
    if(wrote < 0 || (size_t)wrote >= sizeof(syms) - used)
    {
      syms[used]      = '\0';
      spill[n_spill++] = s;
      continue;
    }

    used += (size_t)wrote;
    keep[n_keep++] = s;
  }

  if(n_spill > 0)
    yf_dispatch_v8(b, spill, n_spill);

  if(n_keep == 0)
  {
    mem_free(p);
    return;
  }

  yf_urlencode(crumb, enc_crumb, sizeof(enc_crumb));

  need = snprintf(url, sizeof(url), "%s?symbols=%s&crumb=%s",
      YF_QUOTE_URL, syms, enc_crumb);

  if(need < 0 || (size_t)need >= sizeof(url))
  {
    yf_dispatch_v8(b, keep, n_keep);
    mem_free(p);
    return;
  }

  // The merge hop rides alongside, once per batch: v7 has no intraday
  // series and the verbose card is drawn from one.
  if(b->spark_merge && !b->spark_sent)
  {
    b->spark_sent = true;
    atomic_fetch_add(&b->n_pending, 1);

    if(yf_submit_chart(b, keep[0], b->out[keep[0]].symbol, true) != SUCCESS)
      yf_slot_done(b);
  }

  memcpy(p->slots, keep, n_keep);
  p->n_slots = n_keep;

  cr = curl_request_create(CURL_METHOD_GET, url, yf_quote_done, p);

  if(cr == NULL)
  {
    yf_dispatch_v8(b, p->slots, p->n_slots);
    mem_free(p);
    return;
  }

  yf_apply_common(cr);
  snprintf(hdr, sizeof(hdr), "Cookie: %s", cookie);
  curl_request_add_header(cr, hdr);

  if(curl_request_submit(cr) != SUCCESS)
  {
    yf_dispatch_v8(b, p->slots, p->n_slots);
    mem_free(p);
  }
}

// Route a set of slots to whichever tier can answer right now. Consumes
// p on every path except YF_ACQ_QUEUED, where the mint drain owns it.
static void
yf_dispatch(yf_pend_t *p)
{
  char         cookie[CURL_COOKIE_SZ];
  char         crumb[YF_CRUMB_SZ];
  yf_acquire_t act;

  if(!yf_enrich_enabled())
  {
    yf_dispatch_v8(p->batch, p->slots, p->n_slots);
    mem_free(p);
    return;
  }

  act = yf_session_acquire(p, cookie, sizeof(cookie), crumb, sizeof(crumb));

  if(act == YF_ACQ_QUEUED)
    return;

  if(act == YF_ACQ_READY)
  {
    yf_dispatch_v7(p, cookie, crumb);
    return;
  }

  yf_dispatch_v8(p->batch, p->slots, p->n_slots);
  mem_free(p);
}

// One v7 response -> every slot it was asked about. Runs on the curl
// worker thread; each slot is resolved exactly once, here or by the
// fallback this hands off to.
static void
yf_quote_done(const curl_response_t *resp)
{
  yf_pend_t          *p    = (yf_pend_t *)resp->user_data;
  yf_batch_t         *b    = p->batch;
  struct json_object *root = NULL;
  struct json_object *qr;
  struct json_object *arr;
  bool                filled[YF_MAX_BATCH];
  size_t              len;

  memset(filled, 0, sizeof(filled));

  // A refused crumb is the one failure worth one immediate re-mint;
  // p->retried spends it, so a persistently unhappy Yahoo degrades to
  // the keyless tier instead of looping.
  if(resp->curl_code == 0 && (resp->status == 401 || resp->status == 403))
  {
    yf_session_invalidate();

    if(!p->retried)
    {
      p->retried = true;
      yf_dispatch(p);
      return;
    }

    clam(CLAM_INFO, YF_CTX,
        "v7 quote refused the freshly minted crumb; falling back to v8");
    yf_dispatch_v8(b, p->slots, p->n_slots);
    mem_free(p);
    return;
  }

  if(resp->curl_code == 0 && resp->status == 429)
  {
    // v8 would meet the same wall — say so rather than spend N more
    // requests discovering it.
    for(uint8_t i = 0; i < p->n_slots; i++)
    {
      b->out[p->slots[i]].status = QUOTE_RATE_LIMITED;
      yf_slot_done(b);
    }

    mem_free(p);
    return;
  }

  if(resp->curl_code != 0 || resp->status != 200)
  {
    yf_dispatch_v8(b, p->slots, p->n_slots);
    mem_free(p);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, YF_CTX);
  qr   = root != NULL ? json_get_obj(root, "quoteResponse") : NULL;
  arr  = qr   != NULL ? json_get_array(qr, "result") : NULL;

  // A 200 whose shape we do not recognise is Yahoo changing the
  // contract, not the caller asking for absent symbols. Fall back and
  // SAY so — silently reporting every symbol as not-found would look
  // exactly like a bad query and hide the real cause.
  if(arr == NULL)
  {
    clam(CLAM_WARN, YF_CTX,
        "v7 quote returned HTTP 200 with no quoteResponse.result;"
        " falling back to v8");

    if(root != NULL)
      json_object_put(root);

    yf_dispatch_v8(b, p->slots, p->n_slots);
    mem_free(p);
    return;
  }

  len = (size_t)json_object_array_length(arr);

  for(size_t i = 0; i < len; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, (int)i);
    char                sym[16];

    if(item == NULL || !json_object_is_type(item, json_type_object))
      continue;

    if(!json_get_str(item, "symbol", sym, sizeof(sym)))
      continue;

    // Yahoo answers in its own order and may omit symbols entirely, so
    // results are matched back by name, never by position.
    for(uint8_t s = 0; s < p->n_slots; s++)
    {
      quote_t *q = &b->out[p->slots[s]];

      if(filled[s] || strcasecmp(q->symbol, sym) != 0)
        continue;

      yf_apply_v7(item, q);
      q->status = QUOTE_OK;
      yf_cache_store(q);
      filled[s] = true;
      break;
    }
  }

  for(uint8_t s = 0; s < p->n_slots; s++)
  {
    if(!filled[s])
      b->out[p->slots[s]].status = QUOTE_NOT_FOUND;

    yf_slot_done(b);
  }

  json_object_put(root);
  mem_free(p);
}

// ----------------------------------------------------------------------
// Provider API — the four contract symbols
// ----------------------------------------------------------------------

uint32_t
stockquote_provider_caps(void)
{
  // Standalone history (QUOTE_CAP_SERIES) is still unimplemented; the
  // inline spark[] rides every quote regardless. The three v7 blocks are
  // advertised whenever the tier is switched on rather than whenever a
  // session happens to be up: they are per-field-absent by contract
  // (NAN), a mint can come and go between two quotes, and a capability
  // that flickers would make the card's shape flicker with it.
  uint32_t caps = QUOTE_CAP_SEARCH;

  if(yf_enrich_enabled())
    caps |= QUOTE_CAP_FUNDAMENTALS | QUOTE_CAP_EXTHOURS | QUOTE_CAP_DEPTH;

  return(caps);
}

bool
stockquote_fetch_async(const char *const *syms, uint8_t n,
    stockquote_batch_cb_t cb, void *user)
{
  uint32_t    ttl;
  time_t      now;
  yf_batch_t *b;
  yf_pend_t  *p;
  bool        want_spark;

  if(cb == NULL || syms == NULL)
    return(FAIL);

  if(n == 0 || n > YF_MAX_BATCH)
    return(FAIL);

  ttl = (uint32_t)kv_get_uint("plugin.yahoofinance.cache_ttl");

  if(ttl == 0)
    ttl = 20;

  now = time(NULL);

  // A single symbol is what the verbose card is drawn from, and that
  // card wants both an intraday curve (chart only) and fundamentals (v7
  // only) — so a lone symbol is worth two requests. A multi-symbol
  // table draws no sparkline, so it stays at one request for the lot.
  want_spark = (n == 1);

  b = mem_alloc(YF_CTX, "batch", sizeof(*b));
  b->cb          = cb;
  b->user        = user;
  b->n_total     = n;
  b->n_pending   = (uint8_t)(n + 1);   // +1 guard consumed after dispatch
  b->spark_n     = 0;
  b->spark_merge = want_spark && yf_enrich_enabled();
  b->spark_sent  = false;

  p = mem_alloc(YF_CTX, "pend", sizeof(*p));
  p->batch   = b;
  p->n_slots = 0;
  p->retried = false;
  p->next    = NULL;

  for(uint8_t i = 0; i < n; i++)
  {
    quote_t *q = &b->out[i];

    yf_quote_init(q);
    yf_normalize_symbol(syms[i] != NULL ? syms[i] : "", q->symbol,
        sizeof(q->symbol));

    if(q->symbol[0] == '\0')
    {
      q->status = QUOTE_NOT_FOUND;
      yf_slot_done(b);
      continue;
    }

    if(yf_cache_lookup(q->symbol, q, now, ttl, want_spark))
    {
      yf_slot_done(b);   // cache hit already carries QUOTE_OK
      continue;
    }

    p->slots[p->n_slots++] = i;
  }

  if(p->n_slots > 0)
    yf_dispatch(p);

  else
    mem_free(p);

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

  pthread_mutex_init(&yf_sess_lock, NULL);
  yf_sess_state     = YF_SESS_NONE;
  yf_sess_cookie[0] = '\0';
  yf_sess_crumb[0]  = '\0';
  yf_sess_minted    = 0;
  yf_sess_retry_at  = 0;
  yf_sess_queue     = NULL;
  yf_sess_shutdown  = false;

  clam(CLAM_INFO, YF_CTX, "yahoofinance plugin initialized");
  return(SUCCESS);
}

// Refuse further mints and release anyone parked waiting for one. A
// queued slot still owes its yf_slot_done, so answering it — even with
// UNAVAILABLE — is what lets its batch complete and its consumer's
// callback fire; dropping the queue would strand both.
static bool
yf_stop(void)
{
  yf_pend_t *q;

  pthread_mutex_lock(&yf_sess_lock);
  yf_sess_shutdown = true;
  pthread_mutex_unlock(&yf_sess_lock);

  q = yf_session_take_queue();

  while(q != NULL)
  {
    yf_pend_t  *next = q->next;
    yf_batch_t *b    = q->batch;

    for(uint8_t i = 0; i < q->n_slots; i++)
    {
      b->out[q->slots[i]].status = QUOTE_UNAVAILABLE;
      yf_slot_done(b);
    }

    mem_free(q);
    q = next;
  }

  return(SUCCESS);
}

static void
yf_deinit(void)
{
  pthread_rwlock_destroy(&yf_cache_rwl);
  pthread_mutex_destroy(&yf_sess_lock);
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
  .stop            = yf_stop,
  .deinit          = yf_deinit,
  .ext             = NULL,
};
