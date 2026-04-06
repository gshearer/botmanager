#ifndef BM_COINMARKETCAP_H
#define BM_COINMARKETCAP_H

// No public API — this plugin is loaded via dlopen and interacts
// with the core exclusively through cmd_register / cmd_reply /
// curl_request_*.  All declarations are internal.

#ifdef CMC_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "curl.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"

#include <json-c/json.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

#define CMC_CTX             "coinmarketcap"

// API base URLs.
#define CMC_BASE_URL        "https://pro-api.coinmarketcap.com"
#define CMC_LISTINGS_URL    CMC_BASE_URL "/v1/cryptocurrency/listings/latest"
#define CMC_QUOTES_URL      CMC_BASE_URL "/v1/cryptocurrency/quotes/latest"
#define CMC_GLOBAL_URL      CMC_BASE_URL "/v1/global-metrics/quotes/latest"

// Size limits.
#define CMC_APIKEY_SZ       128
#define CMC_URL_SZ          512
#define CMC_REPLY_SZ        640
#define CMC_SYMBOL_SZ       16
#define CMC_NAME_SZ         64
#define CMC_HDR_SZ          192
#define CMC_MAX_LISTINGS    500
#define CMC_MAX_SELECT      32

// Sort column indices.
#define CMC_SORT_RANK       0
#define CMC_SORT_SYMBOL     1
#define CMC_SORT_PRICE      2
#define CMC_SORT_CAP        3
#define CMC_SORT_1H         4
#define CMC_SORT_24H        5
#define CMC_SORT_7D         6
#define CMC_SORT_VOL        7

// -----------------------------------------------------------------------
// Data structures
// -----------------------------------------------------------------------

// Per-coin cached data (parsed from Listings Latest response).
typedef struct
{
  int32_t   id;
  int32_t   cmc_rank;
  char      name[CMC_NAME_SZ];
  char      symbol[CMC_SYMBOL_SZ];
  double    price;
  double    market_cap;
  double    volume_24h;
  double    pct_1h;
  double    pct_24h;
  double    pct_7d;
  double    circulating_supply;
  double    total_supply;
  double    max_supply;
  int32_t   num_market_pairs;
} cmc_coin_t;

// Extended detail for verbose mode (Quotes Latest response).
typedef struct
{
  cmc_coin_t  base;
  double      market_cap_dominance;
  double      fully_diluted_market_cap;
  double      volume_change_24h;
  double      pct_30d;
  double      pct_60d;
  double      pct_90d;
  char        date_added[32];
} cmc_coin_detail_t;

// Request type.
typedef enum
{
  CMC_REQ_TABLE,
  CMC_REQ_VERBOSE,
  CMC_REQ_GLOBAL
} cmc_req_type_t;

// Selector kinds for argument parsing.
typedef enum
{
  CMC_SEL_SYMBOL,
  CMC_SEL_RANK,
  CMC_SEL_RANGE
} cmc_sel_kind_t;

// Parsed selection item.
typedef struct
{
  cmc_sel_kind_t kind;
  union
  {
    char    symbol[CMC_SYMBOL_SZ];
    int32_t rank;
    struct { int32_t lo, hi; } range;
  };
} cmc_selector_t;

// Request context (freelist-managed).
typedef struct cmc_request
{
  cmc_req_type_t       type;
  char                 apikey[CMC_APIKEY_SZ];

  // Parsed command arguments.
  cmc_selector_t       selectors[CMC_MAX_SELECT];
  uint8_t              selector_count;
  bool                 verbose;

  // Sort parameters.
  uint8_t              sort_col;
  bool                 sort_reverse;

  uint32_t             limit;

  // Saved command context for async reply.
  cmd_ctx_t            ctx;
  method_msg_t         msg;

  // Whether this is a background poll (no reply target).
  bool                 is_poll;

  // Freelist linkage.
  struct cmc_request  *next;
} cmc_request_t;

// -----------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------

// Listings cache.
static cmc_coin_t       cmc_cache[CMC_MAX_LISTINGS];
static uint32_t         cmc_cache_count = 0;
static time_t           cmc_cache_time  = 0;
static pthread_rwlock_t cmc_cache_rwl;

// Request freelist.
static cmc_request_t   *cmc_free     = NULL;
static pthread_mutex_t  cmc_free_mu;

// Polling task handle.
static task_t          *cmc_poll_task = NULL;

// -----------------------------------------------------------------------
// KV schema
// -----------------------------------------------------------------------

static const plugin_kv_entry_t cmc_kv_schema[] = {
  { "plugin.coinmarketcap.apikey",        KV_STR,    ""   },
  { "plugin.coinmarketcap.poll",          KV_UINT8,  "0"  },
  { "plugin.coinmarketcap.cache_ttl",     KV_UINT32, "60" },
  { "plugin.coinmarketcap.default_limit", KV_UINT32, "12" },
};

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------

static cmc_request_t   *cmc_req_alloc(void);
static void             cmc_req_release(cmc_request_t *r);
static void             cmc_reply(cmc_request_t *r, const char *text);

static int              cmc_fmt_number(double val, char *buf, size_t sz);
static int              cmc_fmt_price(double price, char *buf, size_t sz);
static int              cmc_fmt_pct(double pct, char *buf, size_t sz);

static bool             cmc_parse_args(const char *args,
                            cmc_request_t *req, const cmd_ctx_t *ctx);

static bool             cmc_cache_valid(void);
static void             cmc_cache_populate(struct json_object *data_arr);
static bool             cmc_coin_matches(const cmc_coin_t *c,
                            const cmc_selector_t *sel);

static void             cmc_reply_table(cmc_request_t *req);
static void             cmc_reply_verbose(cmc_request_t *req,
                            struct json_object *root);
static void             cmc_reply_global(cmc_request_t *req,
                            struct json_object *root);

static void             cmc_listings_done(const curl_response_t *resp);
static void             cmc_quotes_done(const curl_response_t *resp);
static void             cmc_global_done(const curl_response_t *resp);
static bool             cmc_submit_listings(cmc_request_t *req);
static bool             cmc_submit_quotes(cmc_request_t *req);
static bool             cmc_submit_global(cmc_request_t *req);

static void             cmc_poll_loop(task_t *t);

static void             cmc_cmd_crypto(const cmd_ctx_t *ctx);
static bool             cmc_init(void);
static void             cmc_deinit(void);

#endif // CMC_INTERNAL

#endif // BM_COINMARKETCAP_H
