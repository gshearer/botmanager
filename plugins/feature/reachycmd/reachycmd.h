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
// dlsym shims.
#include "reachyapi_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define REACHYCMD_CTX       "reachycmd"
#define REACHYCMD_LINE_SZ   512
#define REACHYCMD_FILTER_SZ 64

// The move list is rendered as a grid. Four to a line keeps the widest
// name in the shipped library (`incomprehensible2`, 17 chars) inside its
// column on every method we emit to.
#define REACHYCMD_LIST_COLS  4
#define REACHYCMD_LIST_WIDTH 20

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

static bool reachycmd_register(void);
static bool reachycmd_init(void);
static void reachycmd_deinit(void);

#endif // REACHYCMD_INTERNAL

#endif // BM_REACHYCMD_H
