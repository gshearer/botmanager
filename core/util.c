// botmanager — MIT
// Small string, path, and time helpers shared across the daemon.
#include "util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "clam.h"

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

// Returns true if host matches an unsafe IPv4 prefix. host is already
// lowercased and unbracketed.
static bool
util_host_is_ipv4_unsafe(const char *host)
{
  unsigned int a;
  unsigned int b;
  int n;

  // Loopback prefix and zero-net prefix as cheap textual checks first.
  if(strncmp(host, "127.", 4) == 0)     return(true);
  if(strncmp(host, "10.", 3) == 0)      return(true);
  if(strncmp(host, "192.168.", 8) == 0) return(true);
  if(strncmp(host, "169.254.", 8) == 0) return(true);
  if(strncmp(host, "0.", 2) == 0)       return(true);
  if(strcmp(host, "0.0.0.0") == 0)      return(true);

  // 172.16.0.0/12 → first octet 172, second in [16, 31].
  if(sscanf(host, "%u.%u%n", &a, &b, &n) == 2)
    if(a == 172 && b >= 16 && b <= 31)
      return(true);

  return(false);
}

bool
util_url_is_safe_https(const char *url)
{
  char host[256];
  size_t hlen;
  const char *p;
  const char *host_start;

  if(url == NULL) return(false);

  // Scheme must be exactly https://.
  if(util_strncasecmp_ascii(url, "https://", 8) != 0) return(false);

  host_start = url + 8;

  // Strip optional IPv6 bracket.
  if(*host_start == '[')
    host_start++;

  // Host runs until ']', '/', ':', '?', '#', or end.
  p = host_start;
  while(*p != '\0' && *p != ']' && *p != '/' && *p != ':'
      && *p != '?' && *p != '#')
    p++;

  hlen = (size_t)(p - host_start);

  if(hlen == 0 || hlen >= sizeof(host)) return(false);

  for(size_t i = 0; i < hlen; i++)
    host[i] = (char)tolower((unsigned char)host_start[i]);

  host[hlen] = '\0';

  // localhost forms.
  if(strcmp(host, "localhost") == 0)        return(false);
  if(strncmp(host, "localhost.", 10) == 0)  return(false);

  // Cloud metadata literal (belt-and-braces; also caught by 169.254.).
  if(strcmp(host, "169.254.169.254") == 0)  return(false);

  // IPv6 literal forms.
  if(strcmp(host, "::1") == 0)              return(false);
  if(strncmp(host, "fe80:", 5) == 0)        return(false);
  if(strncmp(host, "fc", 2) == 0
      && (host[2] == ':' || (host[2] >= '0' && host[2] <= '9')
          || (host[2] >= 'a' && host[2] <= 'f')))
    return(false);
  if(strncmp(host, "fd", 2) == 0
      && (host[2] == ':' || (host[2] >= '0' && host[2] <= '9')
          || (host[2] >= 'a' && host[2] <= 'f')))
    return(false);

  if(util_host_is_ipv4_unsafe(host)) return(false);

  return(true);
}
