#ifndef BM_GIPHY_H
#define BM_GIPHY_H

// Internal header for the giphy service plugin. Public API lives in
// giphy_api.h; this file is only consumed by giphy.c and is gated by
// GIPHY_INTERNAL so the dlsym shims in the public header are suppressed
// inside the service plugin's own TU.

#ifdef GIPHY_INTERNAL

#include "giphy_api.h"

#include "alloc.h"
#include "clam.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIPHY_CTX          "giphy"

#define GIPHY_API_BASE     "https://api.giphy.com/v1/gifs"
#define GIPHY_MEDIA_BASE   "https://media.giphy.com/media"

#define GIPHY_URL_BUF_SZ   1024
#define GIPHY_ENC_SZ       512   // URL-encoded query
#define GIPHY_KEY_SZ       KV_STR_SZ

#define GIPHY_KV_API_KEY   "plugin.giphy.creds.apikey"
#define GIPHY_KV_RATING    "plugin.giphy.rating"
#define GIPHY_KV_LANG      "plugin.giphy.lang"
#define GIPHY_KV_MAX       "plugin.giphy.max_results"
#define GIPHY_KV_TIMEOUT   "plugin.giphy.timeout_secs"

// Per-call state bridging the curl completion back to the caller's cb.
typedef struct
{
  giphy_done_cb_t  cb;
  void            *user_data;
  giphy_mode_t     mode;
  size_t           n_wanted;
} giphy_req_t;

static bool giphy_init(void);
static void giphy_deinit(void);

#endif // GIPHY_INTERNAL

#endif // BM_GIPHY_H
