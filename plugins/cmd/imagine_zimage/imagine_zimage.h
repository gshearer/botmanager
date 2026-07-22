#ifndef BM_CMD_IMAGINE_ZIMAGE_H
#define BM_CMD_IMAGINE_ZIMAGE_H

// Temporary text-to-image command plugin backed by the standalone
// zimage-service (uvicorn + diffusers on the local GPU). Registers
// `!imagine` / `!ig`, POSTs the prompt to the service's /generate endpoint,
// and replies with a public URL to the image the SERVICE writes into its
// web-served directory. A mutex-guarded single-flight FIFO serializes
// requests (the GPU renders one at a time) and tags each with an id, so a
// busy channel gets "queued #N" instead of clobbering the GPU.
//
// This is a stop-gap. The real implementation is the b64 / LLM-subsystem
// `imagine_cmd` plugin (bot owns hosting), which stays dormant until an
// OpenAI-compatible image backend with a Linux/CUDA runner exists (e.g.
// Ollama). Both plugins register the same command names, so exactly ONE is
// loaded at a time. When the swap comes, this whole directory is deleted.

#ifdef IMAGINE_ZIMAGE_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"
#include "userns.h"
#include "kv.h"
#include "curl.h"
#include "json.h"
#include "colors.h"

#define IZ_CTX          "imagine"
#define IZ_REPLY_SZ     640
#define IZ_PROMPT_SZ    1200   // cap on the prompt sent to the service
#define IZ_URL_SZ       512    // service_url / public_base (KV values are <=255)
#define IZ_HDR_SZ       256    // auth header line "name: value"
#define IZ_FILENAME_SZ  128    // filename returned by the service

// Per-request closure. The command context + message are copied in so the
// reply survives past the originating dispatch frame, and every config value
// is snapshotted so the curl worker thread reads no live KV. Requests link
// into a FIFO when the single in-flight slot is busy.
typedef struct iz_req
{
  cmd_ctx_t      ctx;
  method_msg_t   msg;
  char           prompt[IZ_PROMPT_SZ];
  char           service_url[IZ_URL_SZ];
  char           auth_header[IZ_HDR_SZ];   // "name: value", or "" for none
  char           public_base[IZ_URL_SZ];
  uint32_t       timeout_secs;
  uint32_t       id;
  time_t         begin;                    // submit time, for elapsed reporting
  struct iz_req *next;                     // FIFO link
} iz_req_t;

static bool imagine_zimage_init(void);
static void imagine_zimage_deinit(void);

#endif // IMAGINE_ZIMAGE_INTERNAL

#endif // BM_CMD_IMAGINE_ZIMAGE_H
