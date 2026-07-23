#ifndef BM_CMD_GIPHY_H
#define BM_CMD_GIPHY_H

// Command-surface plugin for !giphy. Renders GIF hits from the giphy
// service plugin's public mechanism API (giphy_api.h, resolved by name).
// Pure presentation: every byte the user sees is shaped here; all
// fetching and normalization lives in the service.

#ifdef GIPHY_CMD_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include "giphy_api.h"

#define GIPHY_CMD_CTX       "giphy_cmd"
#define GIPHY_CMD_REPLY_SZ  640
#define GIPHY_CMD_QUERY_SZ  200

// Per-request heap closure: a deep copy of the command context and its
// message so the service completion callback — fired on the curl worker
// thread long after the command body returns — can still reply.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  bool         verbose;                  // -v: full metadata
  char         query[GIPHY_CMD_QUERY_SZ];// echoed in the no-results line
} giphy_cmd_req_t;

// Parsed argument line (stack-only).
typedef struct
{
  giphy_mode_t mode;
  bool         verbose;
  uint32_t     count;                    // 0 = plugin default
  char         query[GIPHY_CMD_QUERY_SZ];
} giphy_cmd_args_t;

static void giphy_cmd(const cmd_ctx_t *);
static bool giphy_cmd_init(void);
static void giphy_cmd_deinit(void);

#endif // GIPHY_CMD_INTERNAL

#endif // BM_CMD_GIPHY_H
