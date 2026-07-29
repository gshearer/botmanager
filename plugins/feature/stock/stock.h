#ifndef BM_STOCK_H
#define BM_STOCK_H

// Command-surface plugin for !stock. Renders colorized stock / ETF /
// fund / index / FX / commodity quotes against the provider-neutral
// "stock_quotes" capability (include/stockquote.h). It binds to whoever
// provides that capability at call time via the contract shims and names
// no provider — swapping the data source is a load/unload, never a
// recompile. Pure presentation: it derives nothing the provider did not
// already normalize.

#ifdef STOCK_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"

#include "stockquote.h"

#define STOCK_CTX          "stock"
#define STOCK_REPLY_SZ     640
#define STOCK_MAX_SYMS     24    // per-call symbol cap (STOCK-3 spec)
#define STOCK_SYM_SZ       16    // matches quote_t.symbol
#define STOCK_QUERY_SZ     128   // free-text -s search buffer
#define STOCK_GAUGE_CELLS  20    // 52-week gauge width

// Symbol lists (LIST-1). Per-userns named symbol sets, stored in this
// plugin's own table — a stock list holds only stocks, which is what lets
// `@name` be plain argument expansion into the existing fetch path.
#define STOCK_LIST_TABLE    "stock_lists"
#define STOCK_LIST_NAME_SZ  33    // 32-char list name + NUL
#define STOCK_LIST_MAX      12    // symbols per list; see stock_lists.c
#define STOCK_LIST_CSV_SZ   256   // STOCK_LIST_MAX * (STOCK_SYM_SZ + 1), rounded

typedef enum
{
  STOCK_LIST_OK,
  STOCK_LIST_ERR,      // database failure; already logged
  STOCK_LIST_NOSUCH,   // no such list in this namespace
  STOCK_LIST_FULL,     // would exceed STOCK_LIST_MAX
  STOCK_LIST_BADNAME,  // name rejected by stock_list_name_ok
  STOCK_LIST_BADSYM    // a symbol contains characters we refuse to store
} stock_list_rc_t;

typedef struct
{
  char    sym[STOCK_LIST_MAX][STOCK_SYM_SZ];
  uint8_t n;
} stock_symset_t;

// One whitespace-delimited argument token, in the order the user typed
// it. `is_list` marks a leading '@' reference, resolved to symbols only
// after parsing so the parser itself never touches the database.
typedef struct
{
  bool is_list;
  char text[STOCK_LIST_NAME_SZ];
} stock_item_t;

typedef enum
{
  STOCK_OP_NONE,
  STOCK_OP_SHOW,   // --list
  STOCK_OP_ADD,    // --add <name> <syms…>
  STOCK_OP_DEL     // --del <name> <syms…>
} stock_list_op_t;

// Per-request heap closure: a deep copy of the command context and its
// message so a provider completion callback — fired on the curl worker
// thread long after stock_cmd returns — can still reply. Mirrors
// crypto_req_t.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  bool         verbose;               // single-symbol card vs table
  char         query[STOCK_QUERY_SZ]; // echoed in the -s search header
} stock_req_t;

// Parsed argument line (stack-only; never heaped). `items` holds the bare
// tokens in input order; `syms` is the resolved vector produced by
// stock_lists_expand once any '@' references have been looked up.
typedef struct
{
  bool            verbose;
  bool            search;
  char            query[STOCK_QUERY_SZ];
  stock_list_op_t op;
  char            list[STOCK_LIST_NAME_SZ];
  stock_item_t    items[STOCK_MAX_SYMS];
  uint8_t         nitems;
  char            syms[STOCK_MAX_SYMS][STOCK_SYM_SZ];
  uint8_t         nsyms;
} stock_args_t;

// Command-unit internals: defined only in stock.c. stock_lists.c shares
// this header but not these, so they stay behind their own guard rather
// than reaching a translation unit that can never define them.
#ifdef STOCK_CMD_UNIT
static void stock_cmd(const cmd_ctx_t *);
static bool stock_init(void);
static bool stock_start(void);
static void stock_deinit(void);
#endif

// stock_lists.c — per-userns symbol lists. Every entry point ensures the
// schema first, so a database that arrives late still works. Callers run
// on a command worker thread (cmd.h:163), where blocking is expected.
bool stock_lists_schema_ensure(void);
bool stock_list_name_ok(const char *name);
bool stock_list_sym_ok(const char *sym);

stock_list_rc_t stock_lists_get(uint32_t ns_id, const char *name,
    stock_symset_t *out);

stock_list_rc_t stock_lists_add(uint32_t ns_id, const char *name,
    const stock_item_t *syms, uint8_t n, uint8_t *added, uint8_t *total);

stock_list_rc_t stock_lists_del(uint32_t ns_id, const char *name,
    const stock_item_t *syms, uint8_t n, uint8_t *removed, bool *dropped);

// Renders "tech (3), etf (5)" into out; sets *count to the list total.
stock_list_rc_t stock_lists_names(uint32_t ns_id, char *out, size_t cap,
    uint32_t *count);

#endif // STOCK_INTERNAL

#endif // BM_STOCK_H
