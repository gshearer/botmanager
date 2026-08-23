#ifndef BM_CMD_IMAGINE_H
#define BM_CMD_IMAGINE_H

// Command-surface plugin exposing a public, stateless text-to-image
// generation: `!imagine <prompt>` (alias `!ig`) runs a prompt against a
// resolved image model through the inference engine, decodes the base64
// the model returns, writes it into a KV-configured web-served directory,
// and replies with a public URL. `-m <model>` selects from a per-bot
// allowlist; `!show imagine` renders the queue and the model menu with the
// default starred.
//
// Nothing on this path blocks. The handler resolves policy, enqueues, and
// returns; the engine's curl worker delivers the bytes to a completion
// that replies and pumps the next request. A FIFO bounds how much work is
// outstanding, and a per-caller quota keeps one talkative user from owning
// it — the bot answers `!ping` instantly with ten renders in the air.

#ifdef IMAGINE_CMD_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"
#include "userns.h"
#include "kv.h"
#include "util.h"
#include "display.h"

#include "inference.h"

#include <pthread.h>
#include <time.h>
#include <uuid/uuid.h>

#define IMG_CMD_CTX        "imagine"
#define IMG_CMD_REPLY_SZ   640
#define IMG_PREPEND_SZ     1024   // cap on style-prepend bytes
#define IMG_PROMPT_SZ      1200   // cap on the assembled prompt sent to the model
#define IMG_PATH_SZ        768    // output_dir / public_base
#define IMG_MODEL_SZ       64     // logical model name (llm registry handle)
#define IMG_MODEL_ID_SZ    128    // a registry model id, at its widest
#define IMG_SIZE_SZ        32     // "1024x1024" and friends
#define IMG_FILENAME_SZ    64     // "imagine-<uuid>.png"
#define IMG_FULLPATH_SZ    (IMG_PATH_SZ + IMG_FILENAME_SZ + 2)  // dir + '/' + file
#define IMG_URL_LINE_SZ    (IMG_FULLPATH_SZ + 32)  // "[#N Ms] " + public URL
#define IMG_PREVIEW_CHARS  30     // prompt chars shown per queue row
#define IMG_SHOW_MAX       24     // queue rows listed before "+N more"

// Every unauthenticated caller shares one queue identity, on every method.
// The quota is a fair-use rule and an anonymous caller is by definition
// not someone we can tell apart, so they share a bucket sized by
// plugin.imagine.max_per_anonymous. A registered user who happens to be
// named this simply shares that bucket — harmless, and cheaper than a
// reserved-name rule nobody would remember.
#define IMG_ANON_OWNER     "anonymous"

// Per-request closure, and a node in exactly one of the two lists below:
// the pending FIFO, or the in-flight set. The command context and message
// are copied in so the reply survives past the originating dispatch frame,
// and every resolved policy value is snapshotted at enqueue so the curl
// worker thread reads no live KV.
typedef struct img_req
{
  struct img_req *next;                    // FIFO / in-flight link
  cmd_ctx_t       ctx;
  method_msg_t    msg;
  char            owner      [USERNS_USER_SZ];  // caller, or IMG_ANON_OWNER
  char            model      [IMG_MODEL_SZ];
  char            size       [IMG_SIZE_SZ];     // "" = provider default
  char            prompt     [IMG_PROMPT_SZ];
  char            output_dir [IMG_PATH_SZ];
  char            public_base[IMG_PATH_SZ];
  uint32_t        id;                      // monotonic within a busy period
  time_t          begin;                   // submit time, for elapsed reporting
} img_req_t;

// One cascading knob, resolved: the winning value together with the full
// KV key that produced it. imagine_first_nonempty returns the value and
// throws the key away, which is why a `show imagine` disagreeing with
// `show kv plugin.imagine.default` has no way to say which tier won.
typedef struct
{
  const char *value;           // NULL when no tier is set
  char        key[KV_KEY_SZ];  // "" when value is NULL
} imagine_tiered_t;

// Resolved per-request scope: the effective default model, the three
// allowlist tiers (each a membership filter), and the effective size.
typedef struct
{
  imagine_tiered_t  def_model;     // most-specific non-empty, or .value NULL
  const char       *allow_plugin;  // absolute list
  const char       *allow_bot;     // narrows plugin (NULL/empty/"*" = no-op)
  const char       *allow_method;  // narrows bot    (NULL/empty/"*" = no-op)
  const char       *size;          // most-specific non-empty, or NULL
} imagine_scope_t;

// One queue entry captured under the lock for display, so `!show imagine`
// never holds the lock across a reply.
//
// `begin` is stamped at SUBMIT, not at enqueue, so a request still waiting
// carries 0 — which is the difference between an age and a dash on screen,
// and the reason the renderer never subtracts from it blindly.
typedef struct
{
  uint32_t id;
  char     owner[USERNS_USER_SZ];
  char     model[IMG_MODEL_SZ];
  time_t   begin;                          // 0 while queued
  char     text[IMG_PREVIEW_CHARS + 4];    // preview + "…" + NUL
} img_snap_t;

// What the card needs out of the registry: the default model's id and the
// service that serves it.
//
// ⛔ This is NOT a row collector any more. `show imagine` listed every
// image model until 2026-08-23 and the operator ruled the list redundant:
// `show llm models image` already draws that table, with per-model
// counters, and both cards now point at it instead. What is left here is
// one by-name lookup, which the registry has no shim for.
//
// The default is captured before any kind or enabled filter, so a default
// pointing at a disabled model — or at a chat model — still reports as
// registered rather than as missing.
typedef struct
{
  const imagine_scope_t *scope;
  bool                   found;
  char                   service [IMG_MODEL_SZ];
  char                   model_id[IMG_MODEL_ID_SZ];
} img_model_lookup_t;

// Column widths for the queue table. `who` fits an IRC nick, `age` a
// three-digit second count, and `prompt` is elastic — it takes whatever is
// left of DISPLAY_COLS.
#define IMG_Q_LEAD    2
#define IMG_Q_ID      4     // "#128"
#define IMG_Q_WHO    12
#define IMG_Q_MODEL   7     // "gpti1m"
#define IMG_Q_AGE     5     // "132s"

// A fitted cell: display_fit bounds it by columns, this bounds it by
// bytes, and 90 columns of UTF-8 fits inside either way.
#define IMG_CELL_SZ      256

// A card line carries its value plus the indent, the label and ": ".
#define IMG_CARD_LINE_SZ (IMG_CMD_REPLY_SZ + 64)

// Card geometry, shared by every `  label   : value` line above it.
#define IMG_CARD_INDENT      2
#define IMG_CARD_LABEL       8
#define IMG_CARD_VALUE_COLS  (DISPLAY_COLS - IMG_CARD_INDENT \
                              - IMG_CARD_LABEL - 2)   // ": "

static void imagine_pump(void);
static void imagine_done(const llm_image_response_t *resp);
static bool imagine_cmd_init(void);
static void imagine_cmd_deinit(void);

#endif // IMAGINE_CMD_INTERNAL

#endif // BM_CMD_IMAGINE_H
