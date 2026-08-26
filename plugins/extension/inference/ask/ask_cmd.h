#ifndef BM_CMD_ASK_H
#define BM_CMD_ASK_H

// Command-surface plugin exposing a public, stateless one-shot LLM
// query: `!ask <query>` (alias `!a`) runs a single prompt against a
// resolved chat model and prints the answer — no conversation memory,
// no context carry (distinct from the `chat` method, which is the
// personality bot). `-m <model>` selects from a per-bot allowlist;
// `!show ask` reports the model, tier, effort and ceilings a call
// would resolve to, and `!show ask queue` what is running and waiting.
//
// Nothing on this path blocks. The handler resolves policy, snapshots
// the whole request, enqueues, and returns; the engine's curl worker
// delivers the answer to a completion that emits it and pumps the next
// request. A FIFO bounds how much is outstanding and a per-caller quota
// keeps one talkative user from owning it — until 2026-08-26 the only
// bound was core/curl's daemon-wide 32 transfers, shared with every
// other HTTP consumer in the process, which is nobody's `!ask` policy.

#ifdef ASK_CMD_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"
#include "userns.h"
#include "kv.h"
#include "display.h"

#include "inference.h"

#include <pthread.h>
#include <time.h>

#define ASK_CMD_CTX        "ask"
#define ASK_CMD_REPLY_SZ   640
#define ASK_PREPEND_SZ     4096   // cap on prepend-file system-prompt bytes
#define ASK_MODEL_SZ       128    // a registry model name, at its widest
#define ASK_PREVIEW_CHARS  36     // query chars shown per queue row
#define ASK_SHOW_MAX       24     // queue rows listed before "+N more"

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

// Every unauthenticated caller shares one queue identity, on every method.
// The quota is a fair-use rule and an anonymous caller is by definition
// not someone we can tell apart, so they share a bucket sized by
// plugin.ask.max_per_anonymous. A registered user who happens to be named
// this simply shares that bucket — harmless, and cheaper than a
// reserved-name rule nobody would remember.
#define ASK_ANON_OWNER     "anonymous"

// Per-request closure, and a node in exactly one of the two lists the
// queue keeps: the pending FIFO, or the in-flight set. The command context
// and message are copied in so the reply survives past the originating
// dispatch frame, and every resolved value — model, effort, ceilings, the
// system prompt read off disk — is snapshotted at ENQUEUE, because the
// submit that consumes them may happen on a curl worker draining behind
// somebody else's completion. max_lines / max_cols are the reply ceilings
// (plugin ceiling narrowed by the bot / protocol tiers).
typedef struct ask_req
{
  struct ask_req *next;                     // FIFO / in-flight link
  cmd_ctx_t       ctx;
  method_msg_t    msg;
  char            owner  [USERNS_USER_SZ];  // caller, or ASK_ANON_OWNER
  char            model  [ASK_MODEL_SZ];
  char            query  [METHOD_TEXT_SZ];
  char            prepend[ASK_PREPEND_SZ];  // "" = no system message
  llm_effort_t    effort;
  uint32_t        max_tokens;
  uint32_t        max_lines;
  uint32_t        max_cols;
  uint32_t        id;                       // monotonic within a busy period
  time_t          begin;                    // submit time; 0 while queued
} ask_req_t;

// One queue entry captured under the lock for display, so `!show ask
// queue` never holds it across a reply.
//
// `begin` is stamped at SUBMIT, not at enqueue, so a request still waiting
// carries 0 — which is the difference between an age and a dash on screen,
// and the reason the renderer never subtracts from it blindly.
typedef struct
{
  uint32_t id;
  char     owner [USERNS_USER_SZ];
  char     model [ASK_MODEL_SZ];
  char     effort[16];                      // wire spelling, "" = unset
  time_t   begin;                           // 0 while queued
  char     text  [ASK_PREVIEW_CHARS + 4];   // preview + "…" + NUL
} ask_snap_t;

// The flag prefix of one !ask invocation: `-m <model>`, `-e <effort>`.
// Each flag is optional and the have_* bit is what separates "not given"
// from "given empty" — an unset effort must fall through to the tier
// resolver, which is not the same as asking for one explicitly.
typedef struct
{
  char model[ASK_MODEL_SZ];
  char effort[16];    // wire spelling; longest is "minimal"
  bool have_model;
  bool have_effort;
} ask_flags_t;

// Column widths for the queue table. `who` fits an IRC nick, `age` a
// three-digit second count, and `query` is elastic — it takes whatever is
// left of DISPLAY_COLS.
#define ASK_Q_LEAD     2
#define ASK_Q_ID       4     // "#128"
#define ASK_Q_WHO     12
#define ASK_Q_MODEL    8     // "gpt56luna" is wider and simply shifts the row
#define ASK_Q_EFFORT   7     // "minimal"
#define ASK_Q_AGE      5     // "132s"

// A fitted cell: display_fit bounds it by columns, this bounds it by
// bytes, and 90 columns of UTF-8 fits inside either way.
#define ASK_CELL_SZ    256

static void ask_pump(void);
static void ask_done(const llm_chat_response_t *resp);
static bool ask_cmd_init(void);
static void ask_cmd_deinit(void);

#endif // ASK_CMD_INTERNAL

#endif // BM_CMD_ASK_H
