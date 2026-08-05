#ifndef BM_SEARXNG_H
#define BM_SEARXNG_H

// Internal header for the searxng service plugin. Public API lives in
// searxng_api.h; this file is only consumed by searxng.c and is gated
// by SEARXNG_INTERNAL so the public shim in the api header is
// suppressed in the service plugin's own TU.

#ifdef SEARXNG_INTERNAL

#include "searxng_api.h"

// Per-call state bridging the curl callback to the caller's cb. `cb`
// and `user_data` belong to another mapping, so every live request is
// filed on the in-flight list through `next_active` — see the registry
// section in searxng.c.
typedef struct sxng_req
{
  sxng_done_cb_t   cb;
  void            *user_data;
  size_t           n_wanted;
  sxng_category_t  category;

  struct sxng_req *next_active;
} sxng_req_t;

#endif // SEARXNG_INTERNAL

#endif // BM_SEARXNG_H
