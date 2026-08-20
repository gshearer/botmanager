#ifndef BM_SHORTURL_H
#define BM_SHORTURL_H

// Internal header for the shorturl service plugin. The public API lives in
// shorturl_api.h; this file is consumed only by shorturl.c and is gated by
// SU_INTERNAL, which also suppresses the dlsym shim in the public header.

#ifdef SU_INTERNAL

#include "shorturl_api.h"

// Reached through the daemon's directory rather than by bare name:
// web/shorturl/src also holds a db.h, and botmanager's is the one this
// plugin wants. The src/ prefix makes that impossible to get wrong.
#include "src/token.h"

#include <stdbool.h>
#include <stddef.h>

#define SU_CTX "shorturl"

#define SU_KV_BASE_URL "plugin.shorturl.base_url"
#define SU_KV_ENABLED  "plugin.shorturl.enabled"
#define SU_KV_MIN_LEN  "plugin.shorturl.min_len"

// A collision in a 62^8 keyspace is vanishingly unlikely. This bound is here
// so that a genuinely exhausted keyspace fails loudly rather than spinning;
// it is the admin CLI's INSERT_ATTEMPTS, for the same reason.
#define SU_INSERT_ATTEMPTS 8

static bool su_schema_ensure(void);
static bool su_mint(const char *, token_t *);
static void su_compose(char *, size_t, const char *, const token_t *);

static bool su_init(void);
static bool su_start(void);
static void su_deinit(void);

#endif // SU_INTERNAL

#endif // BM_SHORTURL_H
