#ifndef BM_CMD_IMAGINE_H
#define BM_CMD_IMAGINE_H

// Command-surface plugin exposing a public, stateless text-to-image
// generation: `!imagine <prompt>` (alias `!ig`) runs a prompt against a
// resolved image model, decodes the returned bytes, writes them into a
// KV-configured web-served directory, and replies with a public URL.
// `-m <model>` selects from a per-bot allowlist; `!show imagine` renders
// the per-bot model menu with the default starred. Modeled on the ask
// command plugin, gated to image-kind models.

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

#include "inference.h"

#include <uuid/uuid.h>

#define IMG_CMD_CTX        "imagine"
#define IMG_CMD_REPLY_SZ   640
#define IMG_PREPEND_SZ     1024   // cap on style-prepend bytes
#define IMG_PROMPT_SZ      1200   // cap on the assembled prompt sent to the model
#define IMG_PATH_SZ        768    // output_dir / public_base
#define IMG_FILENAME_SZ    64     // "imagine-<uuid>.png"
#define IMG_FULLPATH_SZ    (IMG_PATH_SZ + IMG_FILENAME_SZ + 2)  // dir + '/' + file

// Per-call closure carrying the saved command context through the async
// llm_image_submit callback. The context's msg pointer is rebound to the
// embedded copy so it survives past the originating dispatch frame. The
// output directory and public URL base are snapshotted here so the async
// completion (on a curl worker thread) reads no live KV.
typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  char         output_dir [IMG_PATH_SZ];
  char         public_base[IMG_PATH_SZ];
} img_req_t;

static bool imagine_cmd_init(void);
static void imagine_cmd_deinit(void);

#endif // IMAGINE_CMD_INTERNAL

#endif // BM_CMD_IMAGINE_H
