#ifndef BM_TMDB_CMD_H
#define BM_TMDB_CMD_H

// The tmdb plugin's command surface: !tmdb. Renders colorized movie / TV
// / actor info cards, searches, and trending lists over the rows tmdb.c
// normalizes — pure presentation, every byte the user sees is shaped
// here, while all fetching and normalization stays in the service half
// of the same .so.
//
// Both halves ship as one plugin because tmdb is a dependency-graph leaf
// (nothing requires service_tmdb), so the command's upward method_command
// dependency is inherited by nobody. See `PLUGIN.md §Layer Rules` Rule 1,
// leaf exception.

#include <stdbool.h>

// Driven by tmdb.c's lifecycle: the service half owns the plugin
// descriptor, so it raises and lowers the command surface with it.
bool tmdb_cmd_register(void);
void tmdb_cmd_unregister(void);

#ifdef TMDBCMD_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"

#include "tmdb_api.h"

#define TMDBCMD_CTX      TMDB_CTX ".cmd"
#define TMDBCMD_REPLY_SZ 640
#define TMDBCMD_QUERY_SZ 160
#define TMDB_GAUGE_CELLS 10    // rating gauge width
#define TMDB_LIST_MAX    10    // rows printed for -s / trending

typedef enum
{
  TMDB_MODE_LOOKUP,   // search -> best match -> detail card
  TMDB_MODE_LIST,     // search -> ranked list (-s)
  TMDB_MODE_TRENDING  // trending -> ranked list
} tmdb_mode_t;

// Per-request heap closure: a deep copy of the command context + message so
// a service completion callback — fired on the curl worker thread long
// after tmdb_cmd returns — can still reply. Survives the two-hop
// search→detail chain. Mirrors stock_req_t.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  tmdb_mode_t  mode;
  tmdb_media_t forced;                  // UNKNOWN = multi; else MOVIE/TV/PERSON
  bool         verbose;
  bool         weekly;                  // trending window
  char         query[TMDBCMD_QUERY_SZ]; // echoed in list/no-match headers
} tmdb_req_t;

// Parsed argument line (stack-only).
typedef struct
{
  tmdb_mode_t  mode;
  tmdb_media_t forced;
  bool         verbose;
  bool         weekly;
  char         query[TMDBCMD_QUERY_SZ];
} tmdb_args_t;

static void tmdb_cmd(const cmd_ctx_t *);

#endif // TMDBCMD_INTERNAL

#endif // BM_TMDB_CMD_H
