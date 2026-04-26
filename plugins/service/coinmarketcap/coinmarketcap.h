#ifndef BM_COINMARKETCAP_H
#define BM_COINMARKETCAP_H

// Internal header for the coinmarketcap service plugin. Public API is
// in coinmarketcap_api.h; this file is only consumed by coinmarketcap.c
// and is gated by CMC_INTERNAL so the public shims in the api header
// are suppressed.

#ifdef CMC_INTERNAL

#include "clam.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "alloc.h"
#include "json.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"

#include "coinmarketcap_api.h"

#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Constants

#define CMC_CTX             "coinmarketcap"

// API base URLs.
#define CMC_BASE_URL        "https://pro-api.coinmarketcap.com"
#define CMC_LISTINGS_URL    CMC_BASE_URL "/v1/cryptocurrency/listings/latest"
#define CMC_QUOTES_URL      CMC_BASE_URL "/v1/cryptocurrency/quotes/latest"
#define CMC_GLOBAL_URL      CMC_BASE_URL "/v1/global-metrics/quotes/latest"

// Size limits.
#define CMC_APIKEY_SZ       128
#define CMC_URL_SZ          512
#define CMC_HDR_SZ          192
#define CMC_ERR_SZ          128

// Internal aliases — the public types are the authoritative shape.
typedef coinmarketcap_coin_t         cmc_coin_t;
typedef coinmarketcap_coin_detail_t  cmc_coin_detail_t;
typedef coinmarketcap_global_t       cmc_global_t;

// Request type.
typedef enum
{
  CMC_REQ_LISTINGS,
  CMC_REQ_DETAIL,
  CMC_REQ_GLOBAL
} cmc_req_type_t;

// Request context (freelist-managed). Each request carries either a
// listings / detail / global callback; the active callback is chosen
// by r->type.
typedef struct cmc_request
{
  cmc_req_type_t       type;
  char                 apikey[CMC_APIKEY_SZ];

  // Detail-request selectors: one of these is populated.
  char                 symbol[COINMARKETCAP_SYMBOL_SZ];
  int32_t              rank;

  // Typed callback + opaque user pointer. Exactly one member of the
  // union is valid based on `type`.
  union
  {
    coinmarketcap_done_listings_cb_t listings;
    coinmarketcap_done_detail_cb_t   detail;
    coinmarketcap_done_global_cb_t   global;
  } cb;
  void                *user;

  // Background poll: skip the callback dispatch entirely.
  bool                 is_poll;

  // Freelist linkage.
  struct cmc_request  *next;
} cmc_request_t;

// Module state

// Listings cache.
static cmc_coin_t       cmc_cache[COINMARKETCAP_MAX_LISTINGS];
static uint32_t         cmc_cache_count = 0;
static time_t           cmc_cache_time  = 0;
static pthread_rwlock_t cmc_cache_rwl;

// Global stats cache.
static cmc_global_t     cmc_global_cache;
static time_t           cmc_global_cache_time = 0;

// Request freelist.
static cmc_request_t   *cmc_free     = NULL;
static pthread_mutex_t  cmc_free_mu;

// Polling task handle.
static task_handle_t    cmc_poll_task = TASK_HANDLE_NONE;

// KV schema

static const plugin_kv_entry_t cmc_kv_schema[] = {
  { "plugin.coinmarketcap.apikey",        KV_STR,    "",
    "CoinMarketCap API key" },
  { "plugin.coinmarketcap.poll",          KV_BOOL,   "false",
    "Enable background price polling (true/false)" },
  { "plugin.coinmarketcap.cache_ttl",     KV_UINT32, "60",
    "Cache time-to-live in seconds for price data" },
  { "plugin.coinmarketcap.default_limit", KV_UINT32, "12",
    "Default number of results to display" },
};

// Forward declarations

static cmc_request_t   *cmc_req_alloc(void);
static void             cmc_req_release(cmc_request_t *r);

static bool             cmc_cache_valid(void);
static void             cmc_cache_populate(struct json_object *data_arr);
static bool             cmc_global_cache_valid(void);
static void             cmc_global_cache_store(struct json_object *jdata);

static void             cmc_listings_done(const curl_response_t *resp);
static void             cmc_quotes_done(const curl_response_t *resp);
static void             cmc_global_done(const curl_response_t *resp);
static bool             cmc_submit_listings(cmc_request_t *req);
static bool             cmc_submit_quotes(cmc_request_t *req);
static bool             cmc_submit_global(cmc_request_t *req);

static void             cmc_poll_tick(task_t *t);

static bool             cmc_init(void);
static bool             cmc_start(void);
static void             cmc_deinit(void);

#endif // CMC_INTERNAL

#endif // BM_COINMARKETCAP_H
