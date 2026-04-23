#ifndef BM_CMD_SEARXNG_H
#define BM_CMD_SEARXNG_H

// Command-surface plugin exposing /searxng <category> <query> for each
// category supported by the searxng service plugin (general, images,
// news, videos, music). Formats replies with category-appropriate
// metadata (image dimensions, video length, news publication date).

#ifdef SEARXNG_CMD_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"
#include "alloc.h"
#include "method.h"
#include "userns.h"

#include "searxng_api.h"

#define SEARXNG_CMD_CTX        "searxng"
#define SEARXNG_CMD_REPLY_SZ   640

// Per-call closure carrying the saved command context and the search
// category through the async sxng_search callback.
typedef struct
{
  cmd_ctx_t       ctx;
  method_msg_t    msg;
  sxng_category_t category;
} searxng_cmd_req_t;

static bool searxng_cmd_init(void);
static void searxng_cmd_deinit(void);

#endif // SEARXNG_CMD_INTERNAL

#endif // BM_CMD_SEARXNG_H
