#ifndef BM_SEARXNG_H
#define BM_SEARXNG_H

// Internal header for the searxng service plugin. Public API lives in
// searxng_api.h; this file is only consumed by searxng.c and is gated
// by SEARXNG_INTERNAL so the public shim in the api header is
// suppressed in the service plugin's own TU.

#ifdef SEARXNG_INTERNAL

#include "searxng_api.h"

// Per-call state bridging the curl callback to the caller's cb.
typedef struct
{
  sxng_done_cb_t   cb;
  void            *user_data;
  size_t           n_wanted;
  sxng_category_t  category;
} sxng_req_t;

#endif // SEARXNG_INTERNAL

#endif // BM_SEARXNG_H
