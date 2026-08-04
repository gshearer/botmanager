#ifndef BM_REACHYCMD_H
#define BM_REACHYCMD_H

// The Reachy Mini's command surface: `!reachy` and `show reachy`. Pure
// presentation over the reachyapi service — every byte the user sees is
// shaped here, while all HTTP and payload normalization stays down in
// plugins/service/reachyapi/.
//
// A separate .so from the service it wraps, because reachyapi is NOT a
// dependency-graph leaf: the reachy protocol driver consumes it too, and
// a command surface welded into the service would push that surface's
// upward method_text dependency onto the driver. See
// `PLUGIN.md §Layer Rules` Rule 1.

#ifdef REACHYCMD_INTERNAL

#include "alloc.h"
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

// `!reachy say` is bounded by the command parser: CMD_ARG_REST captures
// the remainder of the line into one CMD_ARG_SZ token, so nothing longer
// than that can arrive here in the first place.
#define REACHYCMD_SAY_SZ    CMD_ARG_SZ

// One fixed filename on the robot for every spoken line. Uploads
// overwrite by name in /tmp/reachy_mini_sounds/, so a fixed name is what
// keeps a talkative robot from filling its own tmpfs.
#define REACHYCMD_SAY_FILE  "reachy_say.wav"

// Which registered speech model the voice comes out of, and how. These
// are plugin-level because `!reachy say` is a command surface, not a
// bot: per-bot voice lives in the protocol driver's instance KV.
#define REACHYCMD_KV_TTS_MODEL "plugin.reachycmd.tts_model"
#define REACHYCMD_KV_TTS_VOICE "plugin.reachycmd.tts_voice"
#define REACHYCMD_KV_TTS_SPEED "plugin.reachycmd.tts_speed"

// The move list is rendered as a grid. Four to a line keeps the widest
// name in the shipped library (`incomprehensible2`, 17 chars) inside its
// column on every method we emit to.
#define REACHYCMD_LIST_COLS  4
#define REACHYCMD_LIST_WIDTH 20

// Minimum privilege level for every reachy verb, reading ones included
// (operator direction, 2026-08-03). The robot is a physical object in a
// room: a registered user of the namespace at this level or above may
// address it, and nobody else — not even to read its state.
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
// share one renderer and one voice.
typedef struct
{
  reachycmd_hold_t hold;
  char             what[REACHY_MOVE_NAME_SZ + 32];
} reachycmd_act_t;

// `show reachy` runs two calls: the daemon status, then the direction of
// arrival. The first result rides here into the second's completion.
typedef struct
{
  reachycmd_hold_t      hold;
  reachy_robot_status_t robot;
} reachycmd_show_t;

typedef struct
{
  reachycmd_hold_t hold;
  char             filter[REACHYCMD_FILTER_SZ];
} reachycmd_list_t;

// `!reachy say` is three async hops on one heap context — synthesize,
// upload, play — each hop's completion starting the next from the curl
// worker thread. The text rides along only so the final line can quote
// what was actually spoken.
typedef struct
{
  reachycmd_hold_t hold;
  char             text[REACHYCMD_SAY_SZ];
} reachycmd_say_t;

static bool reachycmd_register(void);
static bool reachycmd_init(void);
static void reachycmd_deinit(void);

#endif // REACHYCMD_INTERNAL

#endif // BM_REACHYCMD_H
