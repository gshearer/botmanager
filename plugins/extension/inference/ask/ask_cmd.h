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

// Soft word-wrap width for IRC readability. This is the fallback the emit
// loop uses when resolution yields nothing; the live value is the
// plugin.ask.max_cols ceiling, whose schema default must match.
#define ASK_WRAP_COLS      100

// A wrap width below this emits ragged one-word lines that burn the
// max_lines flood cap for no readability gain, so it is the resolution
// floor. The ceiling is bounded by the emit buffer: a wider column count
// would be silently cut by the snprintf in ask_done, dropping the tail of
// the line instead of wrapping it onto the next one.
#define ASK_WRAP_COLS_MIN  20
#define ASK_WRAP_COLS_MAX  (ASK_CMD_REPLY_SZ - 1)

// A wrapped line carries max_cols visible characters plus the colour
// markers threaded through it, which occupy bytes but no columns. The
// headroom is for those markers: appends are bounds-checked and drop
// markers rather than text, so an answer dense with formatting loses
// colour before it ever loses a character.
#define ASK_LINE_SZ        (ASK_CMD_REPLY_SZ + 64)

// Per-call closure carrying the saved command context through the async
// llm_chat_submit callback. The context's msg pointer is rebound to the
// embedded copy so it survives past the originating dispatch frame.
// max_lines / max_cols are the reply ceilings resolved at request time
// (plugin ceiling narrowed by the bot / protocol tiers) — snapshotted here
// so the async completion doesn't re-resolve on a worker thread.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  uint32_t     max_lines;
  uint32_t     max_cols;
} ask_req_t;

static bool ask_cmd_init(void);
static void ask_cmd_deinit(void);

#endif // ASK_CMD_INTERNAL

#endif // BM_CMD_ASK_H
