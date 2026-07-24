#ifndef BM_GIPHY_CMD_H
#define BM_GIPHY_CMD_H

// The giphy plugin's command surface: !giphy. Renders GIF hits from the
// rows giphy.c normalizes — pure presentation, every byte the user sees
// is shaped here, while all fetching and normalization stays in the
// service half of the same .so.
//
// Both halves ship as one plugin because giphy is a dependency-graph
// leaf (nothing requires service_giphy), so the command's upward
// method_command dependency is inherited by nobody. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.

#include <stdbool.h>

// Driven by giphy.c's lifecycle: the service half owns the plugin
// descriptor, so it raises and lowers the command surface with it.
bool giphy_cmd_register(void);
void giphy_cmd_unregister(void);

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

#define GIPHY_CMD_CTX       GIPHY_CTX ".cmd"
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

#endif // GIPHY_CMD_INTERNAL

#endif // BM_GIPHY_CMD_H
