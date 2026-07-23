#ifndef BM_RAWG_CMD_H
#define BM_RAWG_CMD_H

// Command-surface plugin for !rawg. Renders colorized video-game info
// cards, searches, and trending/best/new lists against the rawg service
// plugin's public mechanism API (rawg_api.h, resolved by name). Pure
// presentation: every byte the user sees is shaped here; all fetching and
// normalization lives in the service.

#ifdef RAWGCMD_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"

#include "rawg_api.h"

#define RAWGCMD_CTX      "rawg_cmd"
#define RAWGCMD_REPLY_SZ 640
#define RAWGCMD_QUERY_SZ 160
#define RAWG_GAUGE_CELLS 10    // rating gauge width (over the 0–5 scale)
#define RAWG_LIST_MAX    10    // rows printed for -s / trending

typedef enum
{
  RAWG_MODE_LOOKUP,   // search -> best match -> detail card
  RAWG_MODE_LIST,     // search -> ranked list (-s)
  RAWG_MODE_TRENDING  // popular/best/new -> ranked list
} rawg_mode_t;

// Per-request heap closure: a deep copy of the command context + message so
// a service completion callback — fired on the curl worker thread long
// after rawg_cmd returns — can still reply. Survives the two-hop
// search->detail chain. Mirrors tmdb_req_t.
typedef struct
{
  cmd_ctx_t        ctx;
  method_msg_t     msg;
  rawg_mode_t      mode;
  rawg_list_kind_t list_kind;             // TRENDING mode
  int32_t          year;                  // 0 = default window
  bool             verbose;
  char             query[RAWGCMD_QUERY_SZ]; // echoed in list/no-match headers
} rawg_req_t;

// Parsed argument line (stack-only).
typedef struct
{
  rawg_mode_t      mode;
  rawg_list_kind_t list_kind;
  int32_t          year;
  bool             verbose;
  char             query[RAWGCMD_QUERY_SZ];
} rawg_args_t;

static void rawg_cmd(const cmd_ctx_t *);
static bool rawgcmd_init(void);
static void rawgcmd_deinit(void);

#endif // RAWGCMD_INTERNAL

#endif // BM_RAWG_CMD_H
