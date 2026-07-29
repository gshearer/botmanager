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

// Symbol lists (LIST-2). Per-userns named symbol sets in this plugin's
// own table — a crypto list holds only cryptocurrencies, which is what
// lets `@name` be plain argument expansion into the existing fetch path
// and what makes a bare ETH in a list unambiguous.
#define CRYPTO_LIST_TABLE    "crypto_lists"
#define CRYPTO_LIST_NAME_SZ  33   // 32-char list name + NUL
#define CRYPTO_LIST_MAX      12   // symbols per list; see crypto_lists.c
#define CRYPTO_LIST_CSV_SZ   256  // CRYPTO_LIST_MAX * (SYMBOL_SZ + 1), rounded

typedef enum
{
  CRYPTO_LIST_OK,
  CRYPTO_LIST_ERR,      // database failure; already logged
  CRYPTO_LIST_NOSUCH,   // no such list in this namespace
  CRYPTO_LIST_FULL,     // would exceed CRYPTO_LIST_MAX
  CRYPTO_LIST_BADNAME,  // name rejected by crypto_list_name_ok
  CRYPTO_LIST_BADSYM    // a symbol contains characters we refuse to store
} crypto_list_rc_t;

typedef struct
{
  char    sym[CRYPTO_LIST_MAX][COINMARKETCAP_SYMBOL_SZ];
  uint8_t n;
} crypto_symset_t;

// One whitespace-delimited argument token, in the order the user typed
// it. `is_list` marks a leading '@' reference, resolved to selectors only
// after parsing so the parser itself never touches the database.
typedef struct
{
  bool is_list;
  char text[CRYPTO_LIST_NAME_SZ];
} crypto_item_t;

typedef enum
{
  CRYPTO_OP_NONE,
  CRYPTO_OP_SHOW,   // --list
  CRYPTO_OP_ADD,    // --add <name> <syms…>
  CRYPTO_OP_DEL     // --del <name> <syms…>
} crypto_list_op_t;

// Stack-only parse companion to crypto_req_t: holds the raw tokens and
// any list operation, so the per-request heap closure stays small.
typedef struct
{
  crypto_list_op_t op;
  char             list[CRYPTO_LIST_NAME_SZ];
  crypto_item_t    items[COINMARKETCAP_MAX_SELECT];
  uint8_t          nitems;
} crypto_largs_t;

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

// Command-unit internals: defined only in crypto.c. crypto_lists.c shares
// this header but not these, so they stay behind their own guard rather
// than reaching a translation unit that can never define them.
#ifdef CRYPTO_CMD_UNIT
static void crypto_cmd_crypto(const cmd_ctx_t *ctx);
static bool crypto_init(void);
static bool crypto_start(void);
static void crypto_deinit(void);
#endif

// crypto_lists.c — per-userns symbol lists. Every entry point ensures the
// schema first, so a database that arrives late still works. Callers run
// on a command worker thread (cmd.h:163), where blocking is expected.
bool crypto_lists_schema_ensure(void);
bool crypto_list_name_ok(const char *name);
bool crypto_list_sym_ok(const char *sym);

crypto_list_rc_t crypto_lists_get(uint32_t ns_id, const char *name,
    crypto_symset_t *out);

crypto_list_rc_t crypto_lists_add(uint32_t ns_id, const char *name,
    const crypto_item_t *syms, uint8_t n, uint8_t *added, uint8_t *total);

crypto_list_rc_t crypto_lists_del(uint32_t ns_id, const char *name,
    const crypto_item_t *syms, uint8_t n, uint8_t *removed, bool *dropped);

// Renders "bags (3), alts (5)" into out; sets *count to the list total.
crypto_list_rc_t crypto_lists_names(uint32_t ns_id, char *out, size_t cap,
    uint32_t *count);

#endif // CRYPTO_INTERNAL

#endif // BM_CRYPTO_H
