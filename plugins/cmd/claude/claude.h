// claude.h -- Claude Code bridge (misc plugin, registers /claude)
//
// Exposes a single user command, /claude <prompt>, gated to the
// owner identity. Spawns the `claude` CLI via the core proc.c
// primitive, captures stdout, and replies on the originating
// method (IRC channel, DM, or botmanctl socket).
//
// Each invocation prepends the contents of plugin.claude.preamble_path
// (default: `prompt.txt` at the project root) to the user's prompt.
// The preamble explains the invocation context to Claude and
// instructs it to call `scripts/botman-restart.sh` when a daemon
// restart is required. The plugin itself no longer matches a
// trailing keyword or runs ninja inline -- that decision lives with
// the CLI session, mediated by the helper script.
//
// Concurrency: one session globally at a time. A second caller
// gets a synchronous "busy" reply; no queue.
//
// This header is private. The plugin interacts with the core
// exclusively through cmd / kv / method / proc / task interfaces.

#ifndef BM_PLUGIN_MISC_CLAUDE_H
#define BM_PLUGIN_MISC_CLAUDE_H

#ifdef CLAUDE_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "method.h"
#include "plugin.h"
#include "proc.h"
#include "task.h"
#include "userns.h"

#include <pthread.h>
#include <sys/types.h>

// Log tag used by every clam() call in this plugin.
#define CLAUDE_CTX "claude"

// Maximum assembled-prompt length (preamble + separator + user
// prompt) passed to the CLI argv. Exceeding this truncates and
// logs a warning.
#define CLAUDE_PROMPT_SZ 16384

// Maximum preamble-file size read from disk. Anything beyond this
// is truncated with a CLAM_WARN at load time.
#define CLAUDE_PREAMBLE_MAX 8192

// Retry schedule length for post-restart delivery. Must equal the
// compile-time length of CLAUDE_DELIVERY_RETRY_DELAYS in claude.c.
#define CLAUDE_DELIVERY_MAX_ATTEMPTS 4

// Heap-allocated context passed to claude_pending_deliver through
// task_add_deferred. Carries only the attempt index; all other
// delivery state lives in KV so it survives the restart.
typedef struct
{
  int attempt;                      // 0-based index into the delays table
} claude_pending_retry_ctx_t;

// Wait rig passed through proc_spec_t.user so the calling thread
// can block on the proc.c waiter thread posting completion.
typedef struct
{
  pthread_mutex_t *m;
  pthread_cond_t  *c;
  bool             done;
  int              status;          // WEXITSTATUS when !signalled; signum otherwise
  int              signalled;
  const char      *buf;             // aliases proc_handle capture (valid until proc_free)
  size_t           len;
} claude_wait_t;

// Plugin-level KV schema. Registered once by the plugin loader.
// Values are read per-invocation so `/set kv plugin.claude.model ...`
// takes effect on the next /claude call without a reload.
static const plugin_kv_entry_t claude_kv_schema[] = {
  { "plugin.claude.cli_path", KV_STR, "claude",
    "Path to the claude CLI executable (PATH lookup if unqualified)",
    NULL },
  { "plugin.claude.cwd", KV_STR, "",
    "Working directory for the claude subprocess (empty = inherit parent)."
    " Must be the project root so the CLI can find prompt.txt, the"
    " scripts/ helpers, and the build tree.",
    NULL },
  { "plugin.claude.preamble_path", KV_STR, "prompt.txt",
    "Path to the preamble file prepended to every /claude prompt."
    " Relative paths resolve against plugin.claude.cwd. Empty value"
    " disables the preamble entirely.",
    NULL },
  { "plugin.claude.model", KV_STR, "claude-opus-4-7",
    "Model name passed as --model to the claude CLI", NULL },
  { "plugin.claude.yolo", KV_INT32, "1",
    "When nonzero, pass --dangerously-skip-permissions to the CLI."
    " Zero will stall on the CLI's permission prompt.",
    NULL },
  { "plugin.claude.timeout_sec", KV_INT32, "1800",
    "Subprocess wall-clock timeout in seconds (0 = no watchdog)", NULL },
  { "plugin.claude.stdout_cap_bytes", KV_INT32, "65536",
    "Maximum captured stdout bytes per invocation", NULL },
  { "plugin.claude.pending_ttl_sec", KV_INT32, "600",
    "Seconds a stashed post-restart reply remains valid. On plugin"
    " start, stashed replies older than this are dropped with a"
    " CLAM_WARN rather than delivered.",
    NULL },
  { "plugin.claude.botmanctl_path", KV_STR, "build/tools/botmanctl",
    "Path to the botmanctl binary, exported as BOTMAN_CTL in the"
    " subprocess environment so scripts/botman-restart.sh knows"
    " how to reach the daemon. Relative paths resolve against"
    " plugin.claude.cwd.",
    NULL },

  // Runtime state (written by scripts/botman-restart.sh, cleared
  // by delivery / TTL).

  { "plugin.claude.pending.network", KV_STR, "",
    "Stashed method instance name (e.g. 'drow') for post-restart"
    " delivery. Empty when no reply is pending.",
    NULL },
  { "plugin.claude.pending.target", KV_STR, "",
    "Stashed delivery target (channel or nick) for a pending post-"
    "restart reply. Empty when no reply is pending.",
    NULL },
  { "plugin.claude.pending.text_path", KV_STR, "",
    "Path to the file holding the stashed post-restart reply body."
    " The helper writes to `.botman-claude-pending-reply` in the"
    " project root by default. File contents are read on delivery"
    " and the file is unlinked after success.",
    NULL },
  { "plugin.claude.pending.ts", KV_INT64, "0",
    "Unix seconds the pending reply was stashed. Zero means no reply"
    " is pending. Compared against plugin.claude.pending_ttl_sec on"
    " plugin start to drop stale stashes.",
    NULL },
};

#endif // CLAUDE_INTERNAL

#endif // BM_PLUGIN_MISC_CLAUDE_H
