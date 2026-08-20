#ifndef BM_SHORTURL_API_H
#define BM_SHORTURL_API_H

// Public mechanism API for the shorturl service plugin. Consumers include
// this header and resolve the symbol at runtime via plugin_dlsym_cached —
// the plugin is loaded RTLD_LOCAL.
//
// What sits behind it is unusual for a service plugin and worth knowing: the
// external service is a FastCGI redirector running on the web host, and the
// wire between us and it is not HTTP but a table both sides open. We mint;
// it resolves. web/shorturl/ holds the daemon, and the trust-boundary code
// this plugin mints with is compiled from that same directory so the two
// cannot disagree about what a token is.
//
// Shim shape mirrors plugins/service/openweather/openweather_api.h: an
// atomic-guarded static cache, a union to launder void*-to-function-pointer,
// FATAL + abort on a lookup miss (which implies a broken dependency graph).
// Inside the plugin itself the shim would collide with the real definition,
// so shorturl.c defines SU_INTERNAL before including this header.

#include <stddef.h>

// "<base_url>?<token>" plus the NUL. The live prefix is 22 bytes and a token
// is 8, so this leaves room for a far longer one; su_shorten refuses a base
// that would not fit rather than handing back a truncated link.
#define SU_SHORT_URL_SZ 128

#ifdef SU_INTERNAL

// Mint a short link for each of `n` targets, writing "<base_url>?<token>"
// into the matching slot of `out`. Returns the number of slots filled.
//
// One database round trip per minted link, on the CALLING thread — call it
// from a task or a completion, never under a lock.
//
// A target the table already holds gets that token back rather than a new one,
// so the same URL always prints the same link and its hits accumulate on one
// row. Two callers racing on a target it does NOT hold may each insert; the
// duplicate is harmless and both tokens resolve alike.
//
// A slot is left EMPTY, and the caller must render the original URL, when the
// shortener is disabled, the target is no longer than plugin.shorturl.min_len,
// the target is not a legal redirect destination, or the database refused.
// Shortening is decoration: its failure may never cost the caller its answer.
size_t su_shorten(const char *const *targets, size_t n,
    char (*out)[SU_SHORT_URL_SZ]);

#endif // SU_INTERNAL

#ifndef SU_INTERNAL

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline size_t
su_shorten(const char *const *targets, size_t n, char (*out)[SU_SHORT_URL_SZ])
{
  typedef size_t (*fn_t)(const char *const *, size_t, char (*)[SU_SHORT_URL_SZ]);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("shorturl", "su_shorten", (void **)&cached);

    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "shorturl", "dlsym failed: su_shorten");
      abort();
    }

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }

  return(fn(targets, n, out));
}

#endif // !SU_INTERNAL

#endif // BM_SHORTURL_API_H
