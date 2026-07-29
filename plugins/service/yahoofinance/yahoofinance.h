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

// Session-gated endpoint. v7/quote answers MANY symbols in ONE request
// and is the only source of the fundamentals / depth / extended-hours
// blocks, but it 401s without a cookie+crumb pair. The two hops that
// mint that pair: fc.yahoo.com hands out the A3 cookie (its own status
// is irrelevant — we want the Set-Cookie), which buys a crumb from
// getcrumb. Still keyless: no API key, no account, no payment.
#define YF_QUOTE_URL    "https://query1.finance.yahoo.com/v7/finance/quote"
#define YF_COOKIE_URL   "https://fc.yahoo.com/"
#define YF_CRUMB_URL    "https://query1.finance.yahoo.com/v1/test/getcrumb"

#define YF_URL_SZ       2048  // assembled request URL
#define YF_ENC_SZ       96    // URL-encoded symbol path segment
#define YF_MAX_BATCH    24    // matches yf_batch_t.out[] capacity
#define YF_SEARCH_MAX   8     // hits returned by a search
#define YF_CACHE_SZ     64    // per-symbol TTL cache slots
#define YF_SPARK_RAW    512   // max raw close[] points sampled per quote
#define YF_CRUMB_SZ     64    // crumb token ("ib1a2Yc3dE" shaped)
#define YF_SYMLIST_SZ   (YF_MAX_BATCH * (YF_ENC_SZ + 1))  // joined ?symbols=

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
//
// INVARIANT: every out[] slot contributes exactly ONE yf_slot_done call,
// whoever finally resolves it — the cache, the v7 hop, the v8 hop, or an
// error path. A slot parked on the session queue still owes its unit, so
// the batch cannot die underneath a pending mint.
typedef struct
{
  stockquote_batch_cb_t cb;
  void                 *user;
  uint8_t               n_total;
  _Atomic uint8_t       n_pending;

  // Single-symbol merge (see yf_dispatch_v7): v7 owns out[0], so the
  // concurrent chart hop parks the sparkline HERE rather than writing
  // into a quote_t another thread is filling. The thread that closes the
  // batch copies it across, ordered by the n_pending release.
  float                 spark[32];
  uint16_t              spark_n;
  bool                  spark_merge;   // this batch merges the two tiers
  bool                  spark_sent;    // its chart hop is already away

  quote_t               out[YF_MAX_BATCH];
} yf_batch_t;

// Per-request sub-context: which batch, which output slot. spark_only
// marks the chart hop of a merge — it must touch nothing but the
// batch-level spark buffer.
typedef struct
{
  yf_batch_t *batch;
  uint8_t     slot;
  bool        spark_only;
} yf_sub_t;

// A set of slots within one batch that still needs fetching: either
// parked on the session queue while a mint is in flight, or in flight
// itself as one v7 request. retried spends the single re-mint a crumb
// rejection is allowed before the slots fall back to keyless v8.
typedef struct yf_pend yf_pend_t;

struct yf_pend
{
  yf_batch_t *batch;
  uint8_t     slots[YF_MAX_BATCH];
  uint8_t     n_slots;
  bool        retried;
  yf_pend_t  *next;
};

// Mint context: carries the cookie from hop 1 into hop 2.
typedef struct
{
  char cookie[CURL_COOKIE_SZ];
} yf_mint_t;

// Session lifecycle. NONE and FAILED both mean "no usable crumb"; they
// differ in whether a mint may start right now (FAILED holds a backoff
// so a rate-limited Yahoo is not hammered once per quote).
typedef enum
{
  YF_SESS_NONE = 0,
  YF_SESS_MINTING,
  YF_SESS_READY,
  YF_SESS_FAILED
} yf_sess_state_t;

// What yf_session_acquire told the caller to do with its slots.
typedef enum
{
  YF_ACQ_READY,     // cookie+crumb copied out; dispatch v7 now
  YF_ACQ_QUEUED,    // parked on the mint queue; the drain will dispatch
  YF_ACQ_NONE       // no session and none coming; use keyless v8
} yf_acquire_t;

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

// Module state: the v7 session. Every field below is owned by
// yf_sess_lock, including the wait queue — a mint is minted ONCE no
// matter how many quotes race for it, and everyone who asked meanwhile
// is on `yf_sess_queue` waiting for the same answer.
static pthread_mutex_t  yf_sess_lock;
static yf_sess_state_t  yf_sess_state    = YF_SESS_NONE;
static char             yf_sess_cookie[CURL_COOKIE_SZ];
static char             yf_sess_crumb[YF_CRUMB_SZ];
static time_t           yf_sess_minted   = 0;   // when the crumb was issued
static time_t           yf_sess_retry_at = 0;   // FAILED: earliest re-mint
static yf_pend_t       *yf_sess_queue    = NULL;
static bool             yf_sess_shutdown = false;

// KV schema

static const plugin_kv_entry_t yf_kv_schema[] = {
  { "plugin.yahoofinance.cache_ttl",     KV_UINT32, "20",
    "Per-symbol quote cache TTL (seconds)" },
  { "plugin.yahoofinance.timeout_secs",  KV_UINT32, "10",
    "Per-request HTTP timeout (seconds)" },
  { "plugin.yahoofinance.user_agent",    KV_STR, "botmanager/1.0",
    "HTTP User-Agent. Yahoo's finance API 429s browser-like *and* empty "
    "UAs; a plain client token is served — do NOT set a Mozilla/Chrome "
    "string here (verified 2026-07-22)" },
  { "plugin.yahoofinance.spark_range",   KV_STR, "1d",
    "Chart range for the inline sparkline" },
  { "plugin.yahoofinance.spark_interval", KV_STR, "5m",
    "Chart interval for the inline sparkline" },
  { "plugin.yahoofinance.enrich",        KV_BOOL, "true",
    "Use the cookie+crumb v7/quote tier: fundamentals, bid/ask and "
    "extended-hours data, and ONE request for a whole batch instead of "
    "one per symbol. Still keyless. Off falls back to v8/chart only" },
  { "plugin.yahoofinance.session_ttl",   KV_UINT32, "3600",
    "Lifetime of a minted cookie+crumb session (seconds) before it is "
    "re-minted; an early crumb rejection re-mints regardless" },
  { "plugin.yahoofinance.session_backoff", KV_UINT32, "900",
    "After a failed mint, seconds before another is attempted. Quotes "
    "keep working on the keyless v8 path throughout" },
};

// Forward declarations

static size_t        yf_urlencode(const char *in, char *out, size_t cap);
static void          yf_quote_init(quote_t *q);
static void          yf_normalize_symbol(const char *in, char *out, size_t cap);
static quote_class_t yf_map_klass(const char *s);
static void          yf_apply_common(curl_request_t *cr);

static bool          yf_cache_lookup(const char *sym, quote_t *out,
                         time_t now, uint32_t ttl, bool need_spark);
static void          yf_cache_store(const quote_t *q);
static void          yf_extract_spark(struct json_object *result0, quote_t *q);

static void          yf_slot_done(yf_batch_t *b);
static void          yf_chart_done(const curl_response_t *resp);
static void          yf_search_done(const curl_response_t *resp);

// v7 session: mint, dispatch, fallback
static bool          yf_enrich_enabled(void);
static yf_acquire_t  yf_session_acquire(yf_pend_t *p, char *cookie,
                         size_t cookie_cap, char *crumb, size_t crumb_cap);
static void          yf_session_start_mint(void);
static void          yf_session_publish(const char *cookie, const char *crumb);
static void          yf_session_fail(const char *why);
static void          yf_session_invalidate(void);
static yf_pend_t    *yf_session_take_queue(void);
static void          yf_session_drain(void);
static void          yf_cookie_done(const curl_response_t *resp);
static void          yf_crumb_done(const curl_response_t *resp);
static bool          yf_crumb_plausible(const char *s);

static void          yf_dispatch(yf_pend_t *p);
static void          yf_dispatch_v7(yf_pend_t *p, const char *cookie,
                         const char *crumb);
static void          yf_dispatch_v8(yf_batch_t *b, const uint8_t *slots,
                         uint8_t n_slots);
static bool          yf_submit_chart(yf_batch_t *b, uint8_t slot,
                         const char *sym, bool spark_only);
static void          yf_quote_done(const curl_response_t *resp);
static void          yf_apply_v7(struct json_object *item, quote_t *q);

static market_state_t yf_map_state(const char *s);
static void          yf_sess_enqueue_locked(yf_pend_t *p);

static bool          yf_init(void);
static bool          yf_stop(void);
static void          yf_deinit(void);

#endif // YF_INTERNAL

#endif // BM_YAHOOFINANCE_H
