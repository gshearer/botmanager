// botmanager — MIT
// Gemini symbols cache. Holds the native / abstr / base / quote
// quadruple for every spot pair Gemini publishes, so REST + WS callers
// can look up the form their target endpoint requires.
//
// Population is two-stage:
//   1. GET /v1/symbols              → array of native symbols
//   2. GET /v1/symbols/details/<s>  → per-symbol base + quote
//
// The chosen layering (native ↔ abstr) keeps lookups O(N) over a
// small table (Gemini lists ~150 spot pairs as of 2026-05). The
// linear walk is simpler than maintaining two sorted lookup
// structures and is bounded by GEM_SYMS_CAP.
#define GEM_INTERNAL
#include "gemini.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct
{
  pthread_mutex_t lock;
  uint32_t        count;
  gemini_pair_t   rows[GEM_SYMS_CAP];
} gem_pairs_t;

static gem_pairs_t gem_pairs;

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------

void
gem_pairs_init(void)
{
  memset(&gem_pairs, 0, sizeof(gem_pairs));
  pthread_mutex_init(&gem_pairs.lock, NULL);
}

void
gem_pairs_deinit(void)
{
  pthread_mutex_lock(&gem_pairs.lock);
  gem_pairs.count = 0;
  pthread_mutex_unlock(&gem_pairs.lock);
  pthread_mutex_destroy(&gem_pairs.lock);
}

// ------------------------------------------------------------------
// Mutators
// ------------------------------------------------------------------

void
gem_pairs_clear(void)
{
  pthread_mutex_lock(&gem_pairs.lock);
  memset(gem_pairs.rows, 0, sizeof(gem_pairs.rows));
  gem_pairs.count = 0;
  pthread_mutex_unlock(&gem_pairs.lock);
}

// Append a row. `native` is the Gemini-native lowercase concatenated
// symbol (`btcusd`); `base` and `quote` are the split currency codes
// (case-insensitive — we upper-case at write time). The abstraction
// form is composed from the uppercase base + "-" + uppercase quote.
//
// Returns SUCCESS on commit, FAIL when validation rejected the inputs
// or the cache was full.
bool
gem_pairs_add(const char *native, const char *base, const char *quote)
{
  gemini_pair_t *row;
  size_t         i;
  size_t         blen;
  size_t         qlen;

  if(native == NULL || native[0] == '\0'
      || base == NULL || base[0] == '\0'
      || quote == NULL || quote[0] == '\0')
    return(FAIL);

  blen = strnlen(base, sizeof(row->base));
  qlen = strnlen(quote, sizeof(row->quote));

  if(blen >= sizeof(row->base) || qlen >= sizeof(row->quote))
  {
    clam(CLAM_WARN, GEM_CTX,
        "symbols cache: currency code overflow base='%s' quote='%s'",
        base, quote);
    return(FAIL);
  }

  pthread_mutex_lock(&gem_pairs.lock);

  if(gem_pairs.count >= GEM_SYMS_CAP)
  {
    pthread_mutex_unlock(&gem_pairs.lock);
    clam(CLAM_WARN, GEM_CTX,
        "symbols cache full; dropping '%s'", native);
    return(FAIL);
  }

  row = &gem_pairs.rows[gem_pairs.count];
  memset(row, 0, sizeof(*row));

  // native form: lowercase concatenated.
  snprintf(row->native, sizeof(row->native), "%s", native);

  for(i = 0; row->native[i] != '\0'; i++)
    row->native[i] = (char)tolower((unsigned char)row->native[i]);

  // base + quote: uppercase.
  for(i = 0; i < blen; i++)
    row->base[i] = (char)toupper((unsigned char)base[i]);
  row->base[blen] = '\0';

  for(i = 0; i < qlen; i++)
    row->quote[i] = (char)toupper((unsigned char)quote[i]);
  row->quote[qlen] = '\0';

  // abstr form: BASE-QUOTE. Build by hand from local-copy buffers so
  // gcc's -Wrestrict cannot warn about format args overlapping the
  // destination through the surrounding struct.
  {
    char tmp_base[sizeof(row->base)];
    char tmp_quote[sizeof(row->quote)];

    memcpy(tmp_base,  row->base,  sizeof(tmp_base));
    memcpy(tmp_quote, row->quote, sizeof(tmp_quote));

    snprintf(row->abstr, sizeof(row->abstr), "%s-%s", tmp_base, tmp_quote);
  }

  gem_pairs.count++;

  pthread_mutex_unlock(&gem_pairs.lock);

  return(SUCCESS);
}

uint32_t
gem_pairs_count(void)
{
  uint32_t n;

  pthread_mutex_lock(&gem_pairs.lock);
  n = gem_pairs.count;
  pthread_mutex_unlock(&gem_pairs.lock);

  return(n);
}

// ------------------------------------------------------------------
// Lookups
// ------------------------------------------------------------------

// Canonicalise a pair token (caller-supplied or cache-row field) for
// equality comparison: lowercase, strip the conventional separators
// (`-`, `/`, `_`, space). Both sides of the comparison go through the
// same transform — the canonical form is an internal matching key,
// never returned to the caller. Output buffer must hold at least 24
// chars (the longest reasonable native form is 12 chars).
static void
gem_pair_canon(const char *src, char *dst, size_t cap)
{
  size_t i;
  size_t j = 0;

  if(dst == NULL || cap == 0)
    return;

  dst[0] = '\0';

  if(src == NULL)
    return;

  for(i = 0; src[i] != '\0' && j + 1 < cap; i++)
  {
    char c = src[i];

    if(c == '-' || c == '/' || c == '_' || c == ' ' || c == '.')
      continue;

    if(c >= 'A' && c <= 'Z')
      c = (char)(c + 32);

    dst[j++] = c;
  }

  dst[j] = '\0';
}

static const gemini_pair_t *
gem_pair_find_locked(const char *input)
{
  uint32_t i;
  char     want[24];
  char     have[24];

  gem_pair_canon(input, want, sizeof(want));

  if(want[0] == '\0')
    return(NULL);

  for(i = 0; i < gem_pairs.count; i++)
  {
    const gemini_pair_t *r = &gem_pairs.rows[i];

    if(strcasecmp(input, r->native) == 0)
      return(r);
    if(r->abstr[0] != '\0' && strcasecmp(input, r->abstr) == 0)
      return(r);

    gem_pair_canon(r->native, have, sizeof(have));
    if(have[0] != '\0' && strcmp(want, have) == 0)
      return(r);

    if(r->abstr[0] != '\0')
    {
      gem_pair_canon(r->abstr, have, sizeof(have));
      if(have[0] != '\0' && strcmp(want, have) == 0)
        return(r);
    }
  }

  return(NULL);
}

static void
gem_pair_copy(const char *src, char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  if(src == NULL || src[0] == '\0')
  {
    out[0] = '\0';
    return;
  }

  snprintf(out, cap, "%s", src);
}

// Heuristic split — try the common quote currencies in descending-
// length order so `usdt` matches before `usd`. Returns SUCCESS with
// base/quote written when one of the candidates matches; FAIL when
// the input does not end with any known quote currency.
static bool
gem_pair_split_heuristic(const char *canon, char *out_base, size_t base_cap,
    char *out_quote, size_t quote_cap)
{
  static const char *const quotes[] = {
    "usdt", "usdc", "busd", "dai",
    "usd", "eur", "gbp", "sgd",
    "btc", "eth"
  };
  size_t n = sizeof(quotes) / sizeof(quotes[0]);
  size_t clen = strlen(canon);
  size_t i;

  if(out_base == NULL || out_quote == NULL || base_cap == 0 || quote_cap == 0)
    return(FAIL);

  for(i = 0; i < n; i++)
  {
    size_t qlen = strlen(quotes[i]);
    size_t k;

    if(qlen >= clen)
      continue;

    if(strncasecmp(canon + clen - qlen, quotes[i], qlen) != 0)
      continue;

    if(clen - qlen >= base_cap || qlen >= quote_cap)
      return(FAIL);

    for(k = 0; k < clen - qlen; k++)
      out_base[k] = (char)toupper((unsigned char)canon[k]);
    out_base[clen - qlen] = '\0';

    for(k = 0; k < qlen; k++)
      out_quote[k] = (char)toupper((unsigned char)quotes[i][k]);
    out_quote[qlen] = '\0';

    return(SUCCESS);
  }

  return(FAIL);
}

void
gem_pair_to_native(const char *input, char *out, size_t cap)
{
  const gemini_pair_t *r;
  char                 canon[24];
  size_t               i;

  if(input == NULL || input[0] == '\0' || out == NULL || cap == 0)
  {
    if(out != NULL && cap > 0)
      out[0] = '\0';
    return;
  }

  pthread_mutex_lock(&gem_pairs.lock);
  r = gem_pair_find_locked(input);

  if(r != NULL)
  {
    gem_pair_copy(r->native, out, cap);
    pthread_mutex_unlock(&gem_pairs.lock);
    return;
  }

  pthread_mutex_unlock(&gem_pairs.lock);

  // Cache miss → canonicalise + lowercase. Gemini's gateway responds
  // with HTTP 400 / 404 for an unknown symbol which surfaces as a
  // hard error through the response classifier.
  gem_pair_canon(input, canon, sizeof(canon));

  if(canon[0] == '\0')
  {
    out[0] = '\0';
    return;
  }

  for(i = 0; canon[i] != '\0'; i++)
    canon[i] = (char)tolower((unsigned char)canon[i]);

  snprintf(out, cap, "%s", canon);
}

void
gem_pair_to_abstr(const char *input, char *out, size_t cap)
{
  const gemini_pair_t *r;
  char                 canon[24];
  char                 base[8];
  char                 quote[8];

  if(input == NULL || input[0] == '\0' || out == NULL || cap == 0)
  {
    if(out != NULL && cap > 0)
      out[0] = '\0';
    return;
  }

  pthread_mutex_lock(&gem_pairs.lock);
  r = gem_pair_find_locked(input);

  if(r != NULL && r->abstr[0] != '\0')
  {
    gem_pair_copy(r->abstr, out, cap);
    pthread_mutex_unlock(&gem_pairs.lock);
    return;
  }

  pthread_mutex_unlock(&gem_pairs.lock);

  // Cache miss → heuristic split.
  gem_pair_canon(input, canon, sizeof(canon));

  if(canon[0] == '\0')
  {
    out[0] = '\0';
    return;
  }

  if(gem_pair_split_heuristic(canon, base, sizeof(base),
        quote, sizeof(quote)) == SUCCESS)
  {
    snprintf(out, cap, "%s-%s", base, quote);
    return;
  }

  // Total miss — pass through unchanged so the caller has a chance to
  // surface a clean Gemini error rather than silently substitute.
  gem_pair_copy(input, out, cap);
}
