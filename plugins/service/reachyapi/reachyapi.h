#ifndef BM_REACHYAPI_H
#define BM_REACHYAPI_H

// Internal header for the reachyapi service plugin. Public API lives in
// reachyapi_api.h; this file is only consumed by reachyapi.c and is
// gated by REACHYAPI_INTERNAL so the dlsym shims in the public header
// are suppressed inside the service plugin's own TU.

#ifdef REACHYAPI_INTERNAL

#include "reachyapi_api.h"

#include "alloc.h"
#include "clam.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// REACHYAPI_CTX (the plugin name / clam-context root) lives in
// reachyapi_api.h.

#define REACHYAPI_KV_BASE_URL   "plugin.reachyapi.base_url"
#define REACHYAPI_KV_TIMEOUT    "plugin.reachyapi.timeout"

// Declared here and read by the reachy protocol driver, which keeps its
// own copy of the name — the row is registered where the robot's other
// address is, not where its only reader lives.
#define REACHYAPI_KV_BRIDGE_URL "plugin.reachyapi.bridge_url"

// The daemon plays a named move out of a Hugging Face dataset; the
// emotion library is the only one botman addresses, and its id carries a
// slash that has to reach the daemon percent-encoded.
#define REACHY_MOVE_DATASET \
    "pollen-robotics%2Freachy-mini-emotions-library"

#define REACHY_URL_SZ       512
#define REACHY_BODY_SZ      128   // every JSON body we send is tiny
#define REACHY_BOUNDARY_SZ  40

// The daemon's own MAX_SOUND_UPLOAD_BYTES. Refusing oversize payloads
// here keeps a doomed 25 MiB body off the wire.
#define REACHY_UPLOAD_MAX   (25u * 1024u * 1024u)

// A sound filename lands in /tmp/reachy_mini_sounds/ on the robot, so
// it must be a bare basename with an audio extension the daemon's
// allow-list recognises.
#define REACHY_FILE_NAME_SZ 128

// Which shape of reply the completion has to parse, and therefore which
// arm of the callback union it must invoke.
typedef enum
{
  REACHY_REQ_DONE = 0,   // no payload — status only
  REACHY_REQ_DOA,
  REACHY_REQ_STATUS,
  REACHY_REQ_MOVES
} reachy_req_kind_t;

// Per-call state bridging the curl completion back to the caller's cb.
//
// `cb` is a pointer into the CALLER's mapping, held one indirection
// deeper than anything core can range-test — which is why every live
// request is filed in the in-flight list (`next_active`) and why this
// plugin listens for unmaps. See the registry's comment in reachyapi.c.
typedef struct reachy_req
{
  reachy_req_kind_t kind;
  union
  {
    reachy_done_cb_t   done;
    reachy_doa_cb_t    doa;
    reachy_status_cb_t status;
    reachy_moves_cb_t  moves;
  } cb;
  void              *user_data;
  struct reachy_req *next_active;   // guarded by reachy_active_mutex
} reachy_req_t;

// Declared rather than merely defined so the format archetype can be
// attached: every caller splices a path onto the configured base URL,
// and a mistyped conversion there would only surface as a 404.
static bool reachy_url(char *, size_t, const char *, ...)
    __attribute__((format(printf, 3, 4)));

static bool reachyapi_init(void);
static void reachyapi_deinit(void);

#endif // REACHYAPI_INTERNAL

#endif // BM_REACHYAPI_H
