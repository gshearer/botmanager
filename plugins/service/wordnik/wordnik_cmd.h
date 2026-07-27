#ifndef BM_WORDNIK_CMD_H
#define BM_WORDNIK_CMD_H

// The wordnik plugin's command surface: !wotd. Renders the word of the
// day from the record wordnik.c normalizes — pure presentation, every
// byte the user sees is shaped here, while all fetching and normalization
// stays in the service half of the same .so.
//
// Both halves ship as one plugin because wordnik is a dependency-graph
// leaf (nothing requires service_wordnik), so the command's upward
// method_text dependency is inherited by nobody. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.

#include <stdbool.h>

// Driven by wordnik.c's lifecycle: the service half owns the plugin
// descriptor, so it raises and lowers the command surface with it.
bool wordnik_cmd_register(void);
void wordnik_cmd_unregister(void);

#ifdef WORDNIK_CMD_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include "wordnik_api.h"

#define WORDNIK_CMD_CTX       WORDNIK_CTX ".cmd"
#define WORDNIK_CMD_REPLY_SZ  640

// Per-request heap closure: a deep copy of the command context and its
// message so the service completion callback — fired on the curl worker
// thread long after the command body returns — can still reply.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  bool         verbose;                  // -v: citations and attribution
  char         date[WORDNIK_DATE_SZ];    // echoed in the failure line
} wordnik_cmd_req_t;

// Parsed argument line (stack-only).
typedef struct
{
  bool verbose;
  bool bad_arg;                          // an unparseable token was given
  char date[WORDNIK_DATE_SZ];            // empty = today's word
} wordnik_cmd_args_t;

// Same closure for the dictionary half, carrying the headword so the
// not-found line can name what was looked up.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  bool         verbose;                  // -v: attribution and citations
  char         word[WORDNIK_WORD_SZ];
} wordnik_dict_req_t;

static void wordnik_cmd(const cmd_ctx_t *);
static void wordnik_dict_cmd(const cmd_ctx_t *);

#endif // WORDNIK_CMD_INTERNAL

#endif // BM_WORDNIK_CMD_H
