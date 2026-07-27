#ifndef BM_WORDNIK_H
#define BM_WORDNIK_H

// Internal header for the wordnik service plugin. Public API lives in
// wordnik_api.h; this file is only consumed by wordnik.c and is gated by
// WORDNIK_INTERNAL so the dlsym shims in the public header are suppressed
// inside the service plugin's own TU.

#ifdef WORDNIK_INTERNAL

#include "wordnik_api.h"

#include "alloc.h"
#include "clam.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WORDNIK_CTX (the plugin name / clam-context root) lives in
// wordnik_api.h.

#define WORDNIK_API_WOTD    "https://api.wordnik.com/v4/words.json/wordOfTheDay"

#define WORDNIK_URL_BUF_SZ  512
#define WORDNIK_KEY_SZ      KV_STR_SZ

#define WORDNIK_KV_API_KEY  "plugin.wordnik.creds.apikey"
#define WORDNIK_KV_MAX_DEFS "plugin.wordnik.max_definitions"
#define WORDNIK_KV_MAX_EX   "plugin.wordnik.max_examples"
#define WORDNIK_KV_TIMEOUT  "plugin.wordnik.timeout_secs"

// Per-call state bridging the curl completion back to the caller's cb.
// `date` is the requested day, empty for today — carried for the log line
// only, since the parsed word reports its own publication date.
typedef struct
{
  wordnik_done_cb_t  cb;
  void              *user_data;
  char               date[WORDNIK_DATE_SZ];
} wordnik_req_t;

static bool wordnik_init(void);
static void wordnik_deinit(void);

#endif // WORDNIK_INTERNAL

#endif // BM_WORDNIK_H
