// botmanager — MIT
// Small string, path, and time helpers shared across the daemon.
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "clam.h"
#include "common.h"

// Initialization

void
util_init(void)
{
  unsigned int seed;

  if(getrandom(&seed, sizeof(seed), 0) == sizeof(seed))
  {
    srand(seed);
    clam(CLAM_INFO, "util", "PRNG seeded via getrandom()");
  }

  else
  {
    seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    srand(seed);
    clam(CLAM_WARN, "util",
        "getrandom() unavailable, PRNG seeded from time/pid");
  }
}

// Random number generation

int
util_rand(int upper)
{
  return(rand() % upper);
}

// Formatting helpers

void
util_fmt_bytes(size_t bytes, char *buf, size_t sz)
{
  if(bytes < 1024)
    snprintf(buf, sz, "%zuB", bytes);
  else if(bytes < 1024 * 1024)
    snprintf(buf, sz, "%.1fK", (double)bytes / 1024.0);
  else if(bytes < 1024 * 1024 * 1024)
    snprintf(buf, sz, "%.1fM", (double)bytes / (1024.0 * 1024.0));
  else
    snprintf(buf, sz, "%.1fG", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

void
util_fmt_duration(time_t secs, char *buf, size_t sz)
{
  if(secs < 0)      secs = 0;

  if(secs < 60)
    snprintf(buf, sz, "%lds", (long)secs);
  else if(secs < 3600)
    snprintf(buf, sz, "%ldm%lds", (long)(secs / 60), (long)(secs % 60));
  else if(secs < 86400)
    snprintf(buf, sz, "%ldh%ldm",
        (long)(secs / 3600), (long)((secs % 3600) / 60));
  else
    snprintf(buf, sz, "%ldd%ldh",
        (long)(secs / 86400), (long)((secs % 86400) / 3600));
}

// Hashing

uint32_t
util_fnv1a(const char *s)
{
  uint32_t h = 2166136261u;

  for(; *s != '\0'; s++)
    h = (h ^ (uint8_t)*s) * 16777619u;

  return(h);
}

uint32_t
util_fnv1a_ci(const char *s)
{
  uint32_t h = 2166136261u;

  for(; *s != '\0'; s++)
    h = (h ^ (uint8_t)tolower((unsigned char)*s)) * 16777619u;

  return(h);
}

uint32_t
util_djb2(const char *s)
{
  uint32_t h = 5381;

  for(; *s != '\0'; s++)
    h = ((h << 5) + h) + (uint8_t)*s;

  return(h);
}

// Bounded string scanning

const char *
util_memstr(const char *hay, size_t haylen, const char *needle)
{
  size_t nlen = strlen(needle);

  if(nlen == 0 || nlen > haylen)
    return(NULL);

  for(size_t i = 0; i + nlen <= haylen; i++)
    if(memcmp(hay + i, needle, nlen) == 0)
      return(hay + i);

  return(NULL);
}

const char *
util_read_int(const char *p, const char *end, long *out)
{
  char *endp = NULL;
  long v = strtol(p, &endp, 10);

  if(endp == p || endp > end)
    return(NULL);

  *out = v;
  return(endp);
}

const char *
util_skip_to_value(const char *p, const char *end)
{
  while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;

  if(p >= end || *p != ':')
    return(NULL);

  p++;

  while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;

  return(p);
}

// Time helpers

uint64_t
util_ms_since(const struct timespec *start)
{
  struct timespec now;
  int64_t ms;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ms = (int64_t)(now.tv_sec - start->tv_sec) * 1000
      + (int64_t)(now.tv_nsec - start->tv_nsec) / 1000000;

  return(ms < 0 ? 0 : (uint64_t)ms);
}

// Encoders

size_t
util_b64_encode(const void *in, size_t in_len, char *out, size_t out_cap)
{
  static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  const uint8_t *p = in;
  size_t need = ((in_len + 2) / 3) * 4 + 1;
  size_t o = 0;

  if(out == NULL || out_cap < need) return(0);

  for(size_t i = 0; i < in_len; i += 3)
  {
    uint32_t v = ((uint32_t)p[i]) << 16;

    if(i + 1 < in_len) v |= ((uint32_t)p[i + 1]) << 8;
    if(i + 2 < in_len) v |= (uint32_t)p[i + 2];

    out[o++] = tbl[(v >> 18) & 0x3f];
    out[o++] = tbl[(v >> 12) & 0x3f];
    out[o++] = (i + 1 < in_len) ? tbl[(v >> 6) & 0x3f] : '=';
    out[o++] = (i + 2 < in_len) ? tbl[v & 0x3f]        : '=';
  }

  out[o] = '\0';
  return(o);
}

size_t
util_b64url_encode(const void *in, size_t in_len, char *out, size_t out_cap)
{
  size_t n = util_b64_encode(in, in_len, out, out_cap);
  size_t i;

  if(n == 0) return(0);

  for(i = 0; i < n; i++)
  {
    if(out[i] == '+')      out[i] = '-';
    else if(out[i] == '/') out[i] = '_';
  }

  // Strip RFC 4648 §5 padding — JWT b64url is unpadded.
  while(n > 0 && out[n - 1] == '=')
    out[--n] = '\0';

  return(n);
}

// Decode one RFC 4648 standard-alphabet character to its 6-bit value,
// or -1 if the character is not part of the alphabet.
static int
util_b64_val(unsigned char c)
{
  if(c >= 'A' && c <= 'Z') return(c - 'A');
  if(c >= 'a' && c <= 'z') return(c - 'a' + 26);
  if(c >= '0' && c <= '9') return(c - '0' + 52);
  if(c == '+') return(62);
  if(c == '/') return(63);
  return(-1);
}

bool
util_b64_decode(const char *in, size_t in_len, void *out, size_t out_cap,
    size_t *written)
{
  const unsigned char *p = (const unsigned char *)in;
  unsigned char       *o = (unsigned char *)out;
  uint32_t             acc = 0;
  int                  nbits = 0;
  size_t               w = 0;

  if(in == NULL || out == NULL)
    return(FAIL);

  for(size_t i = 0; i < in_len; i++)
  {
    unsigned char c = p[i];
    int           v;

    if(c == '=')                 // padding — data ends here
      break;

    if(c == ' ' || c == '\t' || c == '\n' || c == '\r')
      continue;

    v = util_b64_val(c);

    if(v < 0)
      return(FAIL);

    acc = (acc << 6) | (uint32_t)v;
    nbits += 6;

    if(nbits >= 8)
    {
      nbits -= 8;

      if(w >= out_cap)
        return(FAIL);

      o[w++] = (unsigned char)((acc >> nbits) & 0xff);
    }
  }

  if(written != NULL)
    *written = w;

  return(SUCCESS);
}

// URL helpers

// True if c is a URL-body character (non-delimiter, non-whitespace).
static bool
util_is_url_char(unsigned char c)
{
  if(c <= 0x20 || c == 0x7f) return(false);
  if(c == '<' || c == '>' || c == '"' || c == '\'') return(false);

  return(true);
}

// Lowercase ASCII compare of n bytes. Returns 0 on match.
static int
util_strncasecmp_ascii(const char *a, const char *b, size_t n)
{
  for(size_t i = 0; i < n; i++)
  {
    int ca = tolower((unsigned char)a[i]);
    int cb = tolower((unsigned char)b[i]);

    if(ca != cb) return(ca - cb);
    if(ca == '\0') return(0);
  }

  return(0);
}

// Check whether the URL body [start, end) ends in a known image
// extension, possibly followed by a `?…` query string. Returns true on
// match.
static bool
util_url_has_image_ext(const char *start, const char *end)
{
  static const char *exts[] = { "jpeg", "jpg", "png", "gif", "webp", NULL };
  const char *q = NULL;
  const char *tail;
  const char *dot;
  size_t ext_len;

  // Locate query separator (if any) so the extension test runs on the
  // path portion only.
  for(const char *p = start; p < end; p++)
    if(*p == '?')
    {
      q = p;
      break;
    }

  tail = (q != NULL) ? q : end;

  // Find last '.' in the path portion.
  dot = NULL;
  for(const char *p = tail - 1; p >= start; p--)
    if(*p == '.')
    {
      dot = p;
      break;
    }

  if(dot == NULL || dot + 1 >= tail) return(false);

  ext_len = (size_t)(tail - (dot + 1));

  for(size_t i = 0; exts[i] != NULL; i++)
  {
    if(strlen(exts[i]) != ext_len) continue;
    if(util_strncasecmp_ascii(dot + 1, exts[i], ext_len) == 0)
      return(true);
  }

  return(false);
}

bool
util_find_image_url(const char *text, char *out, size_t out_cap)
{
  if(text == NULL || out == NULL || out_cap == 0) return(false);

  for(const char *p = text; *p != '\0'; p++)
  {
    const char *u = p;
    const char *end;
    size_t ulen;

    // Case-insensitive "http" match.
    if(util_strncasecmp_ascii(u, "http", 4) != 0) continue;
    u += 4;

    if(*u == 's' || *u == 'S') u++;

    if(u[0] != ':' || u[1] != '/' || u[2] != '/') continue;
    u += 3;

    // Consume URL body.
    end = u;
    while(*end != '\0' && util_is_url_char((unsigned char)*end))
      end++;

    if(end == u) continue;

    if(!util_url_has_image_ext(u, end)) continue;

    ulen = (size_t)(end - p);

    // Fail closed when the URL would not fit — refuse to truncate.
    if(ulen + 1 > out_cap) return(false);

    memcpy(out, p, ulen);
    out[ulen] = '\0';
    return(true);
  }

  return(false);
}

// Query-parameter name fragments that mark a value as a credential. Matched
// case-insensitively as substrings, so this stays short and still covers the
// hyphenated and prefixed spellings services actually use.
static const char *const util_secret_frags[] = {
  "key", "token", "secret", "password", "passwd", "pass",
  "auth", "signature", "sig", "crumb", "credential", "session", "cookie",
};

// Mirrors kv.h's KV_REDACTED_VALUE without depending on it — util is a leaf.
static const char util_redaction[] = "[CENSORED]";

static bool
util_param_is_secret(const char *name, size_t len)
{
  char lower[64];
  size_t i;

  if(len == 0) return(false);

  if(len >= sizeof(lower))
    len = sizeof(lower) - 1;

  for(i = 0; i < len; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  lower[len] = '\0';

  for(i = 0; i < sizeof(util_secret_frags) / sizeof(util_secret_frags[0]); i++)
    if(strstr(lower, util_secret_frags[i]) != NULL)
      return(true);

  return(false);
}

// Append [src,src+len) to out at *pos, never exceeding cap-1. Returns false
// once the buffer is full so the caller can stop early.
static bool
util_append(char *out, size_t cap, size_t *pos, const char *src, size_t len)
{
  if(*pos + len >= cap)
  {
    size_t fit = (*pos + 1 < cap) ? cap - *pos - 1 : 0;

    memcpy(out + *pos, src, fit);
    *pos += fit;
    return(false);
  }

  memcpy(out + *pos, src, len);
  *pos += len;
  return(true);
}

const char *
util_redact_url(const char *url, char *out, size_t out_cap)
{
  const char *q;
  const char *p;
  size_t pos = 0;

  if(out == NULL || out_cap == 0) return("");

  if(url == NULL)
  {
    out[0] = '\0';
    return(out);
  }

  q = strchr(url, '?');

  if(q == NULL)
  {
    snprintf(out, out_cap, "%s", url);
    return(out);
  }

  // Scheme, host and path are never secret — copy through the '?'.
  if(!util_append(out, out_cap, &pos, url, (size_t)(q - url) + 1))
  {
    out[pos] = '\0';
    return(out);
  }

  p = q + 1;

  while(*p != '\0')
  {
    const char *amp = strchr(p, '&');
    const char *end = (amp != NULL) ? amp : p + strlen(p);
    const char *eq  = memchr(p, '=', (size_t)(end - p));
    bool ok;

    if(eq != NULL && util_param_is_secret(p, (size_t)(eq - p)))
    {
      ok = util_append(out, out_cap, &pos, p, (size_t)(eq - p) + 1)
        && util_append(out, out_cap, &pos, util_redaction,
            sizeof(util_redaction) - 1);
    }

    else
      ok = util_append(out, out_cap, &pos, p, (size_t)(end - p));

    if(!ok) break;

    if(amp == NULL) break;

    if(!util_append(out, out_cap, &pos, "&", 1)) break;

    p = amp + 1;
  }

  out[pos] = '\0';
  return(out);
}

bool
util_url_is_safe_https(const char *url)
{
  struct in_addr addr;
  char host[256];
  size_t hlen;
  const char *p;
  const char *host_start;

  if(url == NULL) return(false);

  // Scheme must be exactly https://.
  if(util_strncasecmp_ascii(url, "https://", 8) != 0) return(false);

  host_start = url + 8;

  // A bracket introduces an IP literal and may introduce nothing else
  // (RFC 3986 §3.2.2), and every literal is refused below whatever it
  // addresses — so the bracket alone is the answer, and one wrapping
  // something unparsable is refused rather than retried as a name.
  // Refusing here is also why the scan below need not know brackets
  // exist: no bracketed host reaches it.
  if(*host_start == '[') return(false);

  // Host runs until ':', '/', '?', '#', or end; that colon introduces
  // a port.
  p = host_start;
  while(*p != '\0' && *p != ':' && *p != '/' && *p != '?' && *p != '#')
    p++;

  hlen = (size_t)(p - host_start);

  if(hlen == 0 || hlen >= sizeof(host)) return(false);

  for(size_t i = 0; i < hlen; i++)
    host[i] = (char)tolower((unsigned char)host_start[i]);

  host[hlen] = '\0';

  // localhost forms — names, so no literal test below reaches them.
  if(strcmp(host, "localhost") == 0)        return(false);
  if(strncmp(host, "localhost.", 10) == 0)  return(false);

  // Every IP literal is refused, whatever it addresses. Enumerating the
  // unsafe ranges would mean enumerating their spellings too, and
  // `2130706433`, `0x7f000001`, `0177.0.0.1` and `127.1` are all
  // 127.0.0.1 wearing a different hat. inet_aton() and not inet_pton()
  // precisely because it accepts that legacy set — as does the
  // getaddrinfo() curl resolves with, while inet_pton() parses none of
  // the four, so a range test built on it would pass every one. A
  // public literal is refused with them: this gate's URLs come out of
  // chat, where a bare address is not a real image host.
  if(inet_aton(host, &addr) == 1) return(false);

  return(true);
}

// eventfd wake / drain

void
util_evfd_wake(int fd, const char *ctx)
{
  uint64_t val = 1;

  if(write(fd, &val, sizeof(val)) == (ssize_t)sizeof(val))
    return;

  clam(CLAM_WARN, ctx, "eventfd wake failed: %s", strerror(errno));
}

void
util_evfd_drain(int fd, const char *ctx)
{
  uint64_t val;

  if(read(fd, &val, sizeof(val)) == (ssize_t)sizeof(val))
    return;

  // The fd is non-blocking and the wake is level-triggered, so an empty
  // counter means a concurrent reader got there first.
  if(errno == EAGAIN || errno == EWOULDBLOCK)
    return;

  clam(CLAM_WARN, ctx, "eventfd drain failed: %s", strerror(errno));
}
