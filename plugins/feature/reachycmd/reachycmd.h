#ifndef BM_REACHYCMD_H
#define BM_REACHYCMD_H

// The Reachy Mini's command surface: `/bot <name> …` moves the robot a
// bot owns and `/show bot <name> robot|moves` reports on it. Pure
// presentation over the reachyapi service — every byte the user sees is
// shaped here, while all HTTP and payload normalization stays down in
// plugins/service/reachyapi/.
//
// Every verb is scoped to a bot with the `reachy` method bound, because
// the knobs it turns are per-bot KV that the method driver re-applies on
// every connect. A plugin-level command has no bot, which is why this
// surface owns no plugin KV of its own.
//
// A separate .so from the service it wraps, because reachyapi is NOT a
// dependency-graph leaf: the reachy method driver consumes it too, and
// a command surface welded into the service would push that surface's
// upward bot_chat dependency onto the driver. See
// `PLUGIN.md §Layer Rules` Rule 1.

#ifdef REACHYCMD_INTERNAL

#include "alloc.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

// Included WITHOUT REACHYAPI_INTERNAL: this TU lives in a different
// mapping from the service, so every call goes through the header's
// dlsym shims. Same story for the inference engine's ABI.
#include "reachyapi_api.h"
#include "inference.h"
#include "kv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define REACHYCMD_CTX       "reachycmd"
#define REACHYCMD_LINE_SZ   512
#define REACHYCMD_FILTER_SZ 64

// A failure reason, before the "reachy: " prefix that turns it into a
// line. Kept comfortably under REACHYCMD_LINE_SZ so the two compose
// without truncation — the longest reason carries an engine errbuf.
#define REACHYCMD_REASON_SZ 384

// `say` is bounded by the command parser: CMD_ARG_REST captures the
// remainder of the line into one CMD_ARG_SZ token, so nothing longer
// than that can arrive here in the first place.
#define REACHYCMD_SAY_SZ    CMD_ARG_SZ

// "bot.<name>.reachy." — the instance KV prefix every verb works
// through, built once per invocation by reachycmd_bot_prefix().
#define REACHYCMD_PREFIX_SZ (BOT_NAME_SZ + 16)

// One fixed filename on the robot for every spoken line. Uploads
// overwrite by name in /tmp/reachy_mini_sounds/, so a fixed name is what
// keeps a talkative robot from filling its own tmpfs.
#define REACHYCMD_SAY_FILE  "reachy_say.wav"

// The method kind a bot must have bound before any of these verbs will
// touch it, and the instance-KV suffixes they read and write. Every one
// of these rows is registered by the reachy method driver's
// kv_inst_schema — which is what makes kv_set()'s "unregistered key"
// refusal a correctness net rather than an inconvenience.
#define REACHYCMD_METHOD_KIND "reachy"

#define REACHYCMD_KV_TTS_MODEL "tts_model"
#define REACHYCMD_KV_TTS_VOICE "tts_voice"
#define REACHYCMD_KV_TTS_SPEED "tts_speed"
#define REACHYCMD_KV_VOLUME    "volume"
#define REACHYCMD_KV_TRACKING  "tracking_weight"
#define REACHYCMD_KV_WOBBLE    "wobble"

// The move list is rendered as a grid. Four to a line keeps the widest
// name in the shipped library (`incomprehensible2`, 17 chars) inside its
// column on every method we emit to.
#define REACHYCMD_LIST_COLS  4
#define REACHYCMD_LIST_WIDTH 20

// Privilege every reachy verb registers at, reading ones included.
// Under `bot` / `show bot` the effective gate is the PARENT's — the
// name-first dispatch path checks the parent and cmd_invoke() checks
// nothing — so registering anything softer than the parent's
// ADMIN/100 would be a registration that lies about what is enforced.
// The robot becoming admin-only was accepted by the operator on
// 2026-08-05, superseding the earlier `user` gate of the `!reachy`
// surface.
#define REACHYCMD_LEVEL 100

// Default face-tracking strength for `reachy track on`: the weight the
// robot was measured following a walking human at, obviously alive
// without snapping.
#define REACHYCMD_TRACK_WEIGHT 0.6

// A deep copy of the command context and its message, so a completion
// fired on the curl worker thread — long after the command body
// returned — can still reply. Embedded first in every closure below.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
} reachycmd_hold_t;

// One actuating verb in flight. `what` is the present-participle phrase
// the reply is built from ("playing cheerful1"), so success and failure
// share one renderer and one voice. `saved` marks the verbs that wrote
// their intent to KV before poking the robot: for those, a failed hop
// is a delay rather than a loss, and the reply says so.
typedef struct
{
  reachycmd_hold_t hold;
  char             what[REACHY_MOVE_NAME_SZ + 32];
  bool             saved;
} reachycmd_act_t;

// `show reachy` runs two calls: the daemon status, then the direction of
// arrival. The first result rides here into the second's completion.
typedef struct
{
  reachycmd_hold_t      hold;
  reachy_robot_status_t robot;
} reachycmd_show_t;

// The bot name is copied rather than read back off hold.ctx.bot: the
// footer names the verb that plays a move, and a bot destroyed while the
// library is in flight would leave that pointer dangling.
typedef struct
{
  reachycmd_hold_t hold;
  char             filter[REACHYCMD_FILTER_SZ];
  char             bot[BOT_NAME_SZ];
} reachycmd_list_t;

// `say` is three async hops on one heap context — synthesize,
// upload, play — each hop's completion starting the next from the curl
// worker thread. The text rides along only so the final line can quote
// what was actually spoken.
typedef struct
{
  reachycmd_hold_t hold;
  char             text[REACHYCMD_SAY_SZ];
} reachycmd_say_t;

static bool reachycmd_bot_prefix(const cmd_ctx_t *, char *, size_t);
static bool reachycmd_register(void);
static bool reachycmd_init(void);
static void reachycmd_deinit(void);

#endif // REACHYCMD_INTERNAL

#endif // BM_REACHYCMD_H
