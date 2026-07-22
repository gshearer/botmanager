#ifndef BM_YAHOOFINANCE_H
#define BM_YAHOOFINANCE_H

// Internal header for the yahoofinance service plugin — the first
// provider of the "stock_quotes" capability. The *public* API is the
// provider-neutral contract include/stockquote.h; consumers bind to it
// at runtime via plugin_find_feature("stock_quotes"), never to this
// file. Gated by YF_INTERNAL so nothing outside yahoofinance.c pulls the
// internals. yahoofinance.c defines STOCKQUOTE_PROVIDER_INTERNAL before
// including this header, so stockquote.h below exposes the real provider
// prototypes rather than the capability-resolved shims.

#ifdef YF_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"

#include "stockquote.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Constants

#define YF_CTX          "yahoofinance"

// Keyless endpoints. The chart path takes ONE symbol per request and
// carries the meta block we normalize into a quote_t plus the intraday
// close[] we downsample into the inline sparkline. Search is a single
// multi-hit lookup.
#define YF_CHART_URL    "https://query1.finance.yahoo.com/v8/finance/chart/"
#define YF_SEARCH_URL   "https://query1.finance.yahoo.com/v1/finance/search"

#define YF_URL_SZ       2048  // assembled request URL
#define YF_ENC_SZ       96    // URL-encoded symbol path segment
#define YF_MAX_BATCH    24    // matches yf_batch_t.out[] capacity
#define YF_SEARCH_MAX   8     // hits returned by a search
#define YF_CACHE_SZ     64    // per-symbol TTL cache slots
#define YF_SPARK_RAW    512   // max raw close[] points sampled per quote

// Per-symbol TTL cache entry. t == 0 marks an empty slot.
typedef struct
{
  quote_t q;
  time_t  t;
} yf_cache_ent_t;

// Fan-out batch closure: one heap context per stockquote_fetch_async
// call, freed by the last yf_slot_done. n_pending starts at n + 1 — the
// "+1 guard" keeps the batch alive across the whole submit loop even if
// every request completes synchronously before the loop returns.
typedef struct
{
  stockquote_batch_cb_t cb;
  void                 *user;
  uint8_t               n_total;
  _Atomic uint8_t       n_pending;
  quote_t               out[YF_MAX_BATCH];
} yf_batch_t;

// Per-request sub-context: which batch, which output slot.
typedef struct
{
  yf_batch_t *batch;
  uint8_t     slot;
} yf_sub_t;

// Search request context (search returns a single result set, no batch).
typedef struct
{
  stockquote_search_cb_t cb;
  void                  *user;
} yf_search_req_t;

// Module state: the per-symbol TTL cache. cursor gives round-robin
// eviction when no slot already holds the symbol.
static yf_cache_ent_t   yf_cache[YF_CACHE_SZ];
static uint32_t         yf_cache_cursor = 0;
static pthread_rwlock_t yf_cache_rwl;

// KV schema

static const plugin_kv_entry_t yf_kv_schema[] = {
  { "plugin.yahoofinance.cache_ttl",     KV_UINT32, "20",
    "Per-symbol quote cache TTL (seconds)" },
  { "plugin.yahoofinance.timeout_secs",  KV_UINT32, "10",
    "Per-request HTTP timeout (seconds)" },
  { "plugin.yahoofinance.user_agent",    KV_STR,
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36",
    "Browser User-Agent (bot UAs get blocked)" },
  { "plugin.yahoofinance.spark_range",   KV_STR, "1d",
    "Chart range for the inline sparkline" },
  { "plugin.yahoofinance.spark_interval", KV_STR, "5m",
    "Chart interval for the inline sparkline" },
};

// Forward declarations

static size_t        yf_urlencode(const char *in, char *out, size_t cap);
static void          yf_quote_init(quote_t *q);
static void          yf_normalize_symbol(const char *in, char *out, size_t cap);
static quote_class_t yf_map_klass(const char *s);
static void          yf_apply_common(curl_request_t *cr);

static bool          yf_cache_lookup(const char *sym, quote_t *out,
                         time_t now, uint32_t ttl);
static void          yf_cache_store(const quote_t *q);
static void          yf_extract_spark(struct json_object *result0, quote_t *q);

static void          yf_slot_done(yf_batch_t *b);
static void          yf_chart_done(const curl_response_t *resp);
static void          yf_search_done(const curl_response_t *resp);

static bool          yf_init(void);
static void          yf_deinit(void);

#endif // YF_INTERNAL

#endif // BM_YAHOOFINANCE_H
