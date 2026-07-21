#ifndef BM_CMD_ASK_H
#define BM_CMD_ASK_H

// Command-surface plugin exposing a public, stateless one-shot LLM
// query: `!ask <query>` (alias `!a`) runs a single prompt against a
// resolved chat model and prints the answer — no conversation memory,
// no context carry (distinct from the `chat` method, which is the
// personality bot). `-m <model>` selects from a per-bot allowlist;
// `!show ask` renders the per-bot model menu with the default starred.

#ifdef ASK_CMD_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"
#include "userns.h"
#include "kv.h"

#include "inference.h"

#define ASK_CMD_CTX        "ask"
#define ASK_CMD_REPLY_SZ   640
#define ASK_PREPEND_SZ     4096   // cap on prepend-file system-prompt bytes
#define ASK_WRAP_COLS      100    // soft word-wrap width for IRC readability

// Per-call closure carrying the saved command context through the async
// llm_chat_submit callback. The context's msg pointer is rebound to the
// embedded copy so it survives past the originating dispatch frame.
// max_lines is the reply-line cap resolved at request time (plugin ceiling
// narrowed by the bot / protocol tiers) — snapshotted here so the async
// completion doesn't re-resolve on a worker thread.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  uint32_t     max_lines;
} ask_req_t;

static bool ask_cmd_init(void);
static void ask_cmd_deinit(void);

#endif // ASK_CMD_INTERNAL

#endif // BM_CMD_ASK_H
