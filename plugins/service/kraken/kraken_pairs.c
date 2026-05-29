// botmanager — MIT
// Kraken assetpairs cache. Holds the altname / canonical / wsname
// triplet for every spot pair Kraken publishes, so REST callers can
// look up the form their target endpoint requires (most accept altname;
// OHLC accepts both; WS v2 accepts wsname). Population happens via
// kraken_assetpairs_refresh_async (kraken_orders.c) which parses
// GET /0/public/AssetPairs and calls kr_pairs_clear + kr_pairs_add for
// every row.
#define KR_INTERNAL
#include "kraken.h"

#include "kraken_pairs.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct
{
  pthread_mutex_t lock;
  uint32_t        count;
  kraken_pair_t   rows[KRAKEN_PAIRS_CAP];
} kr_pairs_t;

static kr_pairs_t kr_pairs;

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------

void
kr_pairs_init(void)
{
  memset(&kr_pairs, 0, sizeof(kr_pairs));
  pthread_mutex_init(&kr_pairs.lock, NULL);
}

void
kr_pairs_deinit(void)
{
  pthread_mutex_lock(&kr_pairs.lock);
  kr_pairs.count = 0;
  pthread_mutex_unlock(&kr_pairs.lock);
  pthread_mutex_destroy(&kr_pairs.lock);
}

// ------------------------------------------------------------------
// Mutators
// ------------------------------------------------------------------

void
kr_pairs_clear(void)
{
  pthread_mutex_lock(&kr_pairs.lock);
  memset(kr_pairs.rows, 0, sizeof(kr_pairs.rows));
  kr_pairs.count = 0;
  pthread_mutex_unlock(&kr_pairs.lock);
}

void
kr_pairs_add(const char *altname, const char *canonical, const char *wsname)
{
  kraken_pair_t *row;

  if(altname == NULL || altname[0] == '\0')
    return;

  pthread_mutex_lock(&kr_pairs.lock);

  if(kr_pairs.count >= KRAKEN_PAIRS_CAP)
  {
    pthread_mutex_unlock(&kr_pairs.lock);
    clam(CLAM_WARN, KR_CTX,
        "assetpairs cache full; dropping '%s'", altname);
    return;
  }

  row = &kr_pairs.rows[kr_pairs.count];
  memset(row, 0, sizeof(*row));

  snprintf(row->altname, sizeof(row->altname), "%s", altname);

  if(canonical != NULL)
    snprintf(row->canonical, sizeof(row->canonical), "%s", canonical);

  if(wsname != NULL)
  {
    // Kraken's REST AssetPairs publishes the legacy WebSocket v1 form
    // (e.g. "XBT/USD"), but WS v2 — which is the only endpoint this
    // plugin connects to — accepts only the ISO-coded form ("BTC/USD")
    // and rejects "XBT/USD" with "Currency pair not supported". Strip
    // the legacy "XBT/" leading prefix before storing so the cache
    // always returns a v2-compatible wsname.
    if(strncmp(wsname, "XBT/", 4) == 0)
      snprintf(row->wsname, sizeof(row->wsname), "BTC/%s", wsname + 4);
    else
      snprintf(row->wsname, sizeof(row->wsname), "%s", wsname);
  }

  kr_pairs.count++;

  pthread_mutex_unlock(&kr_pairs.lock);
}

uint32_t
kr_pairs_count(void)
{
  uint32_t n;

  pthread_mutex_lock(&kr_pairs.lock);
  n = kr_pairs.count;
  pthread_mutex_unlock(&kr_pairs.lock);

  return(n);
}

uint32_t
kr_pairs_snapshot(kraken_pair_t *out, uint32_t cap)
{
  uint32_t n;

  if(out == NULL || cap == 0)
    return(0);

  pthread_mutex_lock(&kr_pairs.lock);

  n = kr_pairs.count;

  if(n > cap)
    n = cap;

  memcpy(out, kr_pairs.rows, (size_t)n * sizeof(out[0]));

  pthread_mutex_unlock(&kr_pairs.lock);

  return(n);
}

// ------------------------------------------------------------------
// Lookups
//
// Two reads per lookup are acceptable — the cache refreshes once a
// day. The linear scan is bounded by the number of registered Kraken
// pairs (~1500 spot pairs in 2026).
// ------------------------------------------------------------------

// Normalize a pair token (caller-supplied or cache-row field) for
// equality comparison: uppercase, strip the conventional separators
// (`-`, `/`, `_`, space), then rewrite the leading "BTC" → "XBT" so
// abstraction-side ids that follow ISO conventions (`BTC-USD`) match
// Kraken's legacy bitcoin code (altname `XBTUSD`, wsname `XBT/USD`).
//
// Both sides of the comparison go through the same transform — the
// canonical form is an internal matching key, never returned to the
// caller. Output buffer must hold at least 24 chars (Kraken's longest
// altname is ~16 chars; 24 covers stripped/uppercased headroom).
static void
kr_pair_canon(const char *src, char *dst, size_t cap)
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

    if(c >= 'a' && c <= 'z')
      c = (char)(c - 32);

    dst[j++] = c;
  }

  dst[j] = '\0';

  if(j >= 3 && strncmp(dst, "BTC", 3) == 0)
    memcpy(dst, "XBT", 3);
}

static const kraken_pair_t *
kr_pair_find_locked(const char *input)
{
  uint32_t i;
  char     want[24];
  char     have[24];

  kr_pair_canon(input, want, sizeof(want));

  if(want[0] == '\0')
    return(NULL);

  for(i = 0; i < kr_pairs.count; i++)
  {
    const kraken_pair_t *r = &kr_pairs.rows[i];

    if(strcasecmp(input, r->altname) == 0)
      return(r);
    if(r->canonical[0] != '\0' && strcasecmp(input, r->canonical) == 0)
      return(r);
    if(r->wsname[0]    != '\0' && strcasecmp(input, r->wsname)    == 0)
      return(r);

    kr_pair_canon(r->altname, have, sizeof(have));
    if(have[0] != '\0' && strcmp(want, have) == 0)
      return(r);

    if(r->wsname[0] != '\0')
    {
      kr_pair_canon(r->wsname, have, sizeof(have));
      if(have[0] != '\0' && strcmp(want, have) == 0)
        return(r);
    }

    if(r->canonical[0] != '\0')
    {
      kr_pair_canon(r->canonical, have, sizeof(have));
      if(have[0] != '\0' && strcmp(want, have) == 0)
        return(r);
    }
  }

  return(NULL);
}

static void
kr_pair_copy(const char *src, char *out, size_t cap)
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

void
kr_pair_lookup_rest(const char *input, char *out, size_t cap)
{
  const kraken_pair_t *r;

  if(input == NULL || input[0] == '\0' || out == NULL || cap == 0)
  {
    if(out != NULL && cap > 0)
      out[0] = '\0';
    return;
  }

  pthread_mutex_lock(&kr_pairs.lock);
  r = kr_pair_find_locked(input);

  if(r != NULL)
  {
    kr_pair_copy(r->altname, out, cap);
    pthread_mutex_unlock(&kr_pairs.lock);
    return;
  }

  pthread_mutex_unlock(&kr_pairs.lock);

  // Cache miss → pass through. Kraken's gateway will respond with
  // EQuery:Unknown asset pair which surfaces as a hard error through
  // the response classifier.
  kr_pair_copy(input, out, cap);
}

void
kr_pair_lookup_ws(const char *input, char *out, size_t cap)
{
  const kraken_pair_t *r;

  if(input == NULL || input[0] == '\0' || out == NULL || cap == 0)
  {
    if(out != NULL && cap > 0)
      out[0] = '\0';
    return;
  }

  pthread_mutex_lock(&kr_pairs.lock);
  r = kr_pair_find_locked(input);

  if(r != NULL && r->wsname[0] != '\0')
  {
    kr_pair_copy(r->wsname, out, cap);
    pthread_mutex_unlock(&kr_pairs.lock);
    return;
  }

  pthread_mutex_unlock(&kr_pairs.lock);

  kr_pair_copy(input, out, cap);
}

// MW-1: abstraction-canonical hyphenated form. Cache miss → empty
// string (callers drop the row); never pass the wire id through, since
// the abstraction's contract is that snapshot rows carry canonical IDs.
void
kr_pair_lookup_abstr(const char *input, char *out, size_t cap)
{
  const kraken_pair_t *r;
  char                 ws[24];
  size_t               i;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  if(input == NULL || input[0] == '\0')
    return;

  pthread_mutex_lock(&kr_pairs.lock);
  r = kr_pair_find_locked(input);

  if(r == NULL || r->wsname[0] == '\0')
  {
    pthread_mutex_unlock(&kr_pairs.lock);
    return;
  }

  kr_pair_copy(r->wsname, ws, sizeof(ws));
  pthread_mutex_unlock(&kr_pairs.lock);

  // Rewrite '/' → '-' in place; the wsname is already uppercase ISO.
  for(i = 0; ws[i] != '\0'; i++)
  {
    if(ws[i] == '/')
      ws[i] = '-';
  }

  snprintf(out, cap, "%s", ws);
}
