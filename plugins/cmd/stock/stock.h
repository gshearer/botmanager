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

// Parsed argument line (stack-only; never heaped).
typedef struct
{
  bool    verbose;
  bool    search;
  char    query[STOCK_QUERY_SZ];
  char    syms[STOCK_MAX_SYMS][STOCK_SYM_SZ];
  uint8_t nsyms;
} stock_args_t;

static void stock_cmd(const cmd_ctx_t *);
static bool stock_init(void);
static void stock_deinit(void);

#endif // STOCK_INTERNAL

#endif // BM_STOCK_H
