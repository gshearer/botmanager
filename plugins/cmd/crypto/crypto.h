#ifndef BM_CRYPTO_H
#define BM_CRYPTO_H

// Command-surface plugin for /crypto. Formats replies against the
// coinmarketcap service plugin's public API; owns no mechanism state.

#ifdef CRYPTO_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "kv.h"
#include "method.h"

#include "coinmarketcap_api.h"

#define CRYPTO_CTX       "crypto"
#define CRYPTO_REPLY_SZ  640

// Request kind — chosen by the command-line flags at dispatch.
typedef enum
{
  CRYPTO_REQ_TABLE,
  CRYPTO_REQ_VERBOSE,
  CRYPTO_REQ_GLOBAL
} crypto_req_kind_t;

// Selector kinds for argument parsing.
typedef enum
{
  CRYPTO_SEL_SYMBOL,
  CRYPTO_SEL_RANK,
  CRYPTO_SEL_RANGE
} crypto_sel_kind_t;

typedef struct
{
  crypto_sel_kind_t kind;
  union
  {
    char    symbol[COINMARKETCAP_SYMBOL_SZ];
    int32_t rank;
    struct { int32_t lo, hi; } range;
  };
} crypto_selector_t;

// Per-request heap closure that survives the async round-trip.
typedef struct
{
  cmd_ctx_t            ctx;
  method_msg_t         msg;
  crypto_req_kind_t    kind;
  crypto_selector_t    selectors[COINMARKETCAP_MAX_SELECT];
  uint8_t              selector_count;
  uint8_t              sort_col;
  bool                 sort_reverse;
  bool                 verbose;
  uint32_t             limit;
} crypto_req_t;

static void crypto_cmd_crypto(const cmd_ctx_t *ctx);
static bool crypto_init(void);
static void crypto_deinit(void);

#endif // CRYPTO_INTERNAL

#endif // BM_CRYPTO_H
