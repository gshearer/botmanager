// botmanager — MIT
// IRC dossier signature builder + scorer; standalone TU so unit tests link it.

#include "irc_dossier.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <json-c/json.h>

#include "common.h"
#include "json.h"
#include "irc.h"

static bool
irc_dossier_byte_safe(unsigned char c)
{
  if(c < 0x20 || c == '"' || c == '\\')
    return(false);

  return(true);
}

static bool
irc_dossier_field_safe(const char *s)
{
  for(; *s != '\0'; s++)
    if(!irc_dossier_byte_safe((unsigned char)*s))
      return(false);

  return(true);
}

// Parse one 16-bit IPv6 group starting at **p. On success advances *p
// past the group (stopping at ':' or end-of-string) and returns the
// group value in [0, 0xFFFF]. Returns -1 on a non-hex byte, an empty
// group, or more than 4 hex digits.
//
// Caller is expected to already have ensured *p is the first digit of
// a group (i.e. not ':' and not '\0').
static int
irc_dossier_parse_ipv6_group(const char **p)
{
  unsigned g      = 0;
  int      digits = 0;

  while(**p != '\0' && **p != ':')
  {
    int  v;
    char c = **p;

    if(c >= '0' && c <= '9')      v = c - '0';
    else if(c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if(c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else                          return(-1);

    if(digits >= 4)
      return(-1);

    g = (g << 4) | (unsigned)v;
    digits++;
    (*p)++;
  }

  if(digits == 0)
    return(-1);

  return((int)g);
}

static bool
irc_dossier_ipv6_head(const char *host, char *out, size_t out_sz)
{
  const char *p;
  unsigned g2;
  unsigned g1;
  int n;
  if(host == NULL || out == NULL || out_sz == 0)
    return(false);

  out[0] = '\0';

  g1 = 0;
  g2 = 0;
  p = host;

  // "::<...>" — both groups sit inside the leading zero run.
  if(p[0] == ':' && p[1] == ':')
  {
    // g1 and g2 stay zero.
  }

  else
  {
    int v1;
    // A single leading ':' without a following ':' is malformed.
    if(*p == ':')
      return(false);

    v1 = irc_dossier_parse_ipv6_group(&p);
    if(v1 < 0 || *p != ':')
      return(false);
    g1 = (unsigned)v1;
    p++;

    // A second ':' right here means the '::' run starts at slot 2,
    // so g2 lives inside the zero fill.
    if(*p != ':')
    {
      int v2 = irc_dossier_parse_ipv6_group(&p);
      if(v2 < 0)
        return(false);
      g2 = (unsigned)v2;
    }
  }

  n = snprintf(out, out_sz, "%x:%x", g1, g2);
  if(n <= 0 || (size_t)n >= out_sz)
  {
    out[0] = '\0';
    return(false);
  }
  return(true);
}

// Detect a dotted-quad IPv4 literal: four labels, each 1-3 digits
// in the range 0-255, separated by exactly three dots. This is how
// we decide which *end* of a dotted host to anchor on — hostnames
// carry the identifying ISP/network in the rightmost labels, IP
// literals carry it in the leftmost octets.
//
// host:   candidate string (must be NUL-terminated)
// returns: true iff host is a valid dotted-quad
static bool
irc_dossier_is_ipv4_literal(const char *host)
{
  unsigned value;
  int digits;
  int octets;
  if(host == NULL || host[0] == '\0')
    return(false);

  octets = 0;
  digits = 0;
  value = 0;

  for(const char *p = host;; p++)
  {
    if(*p >= '0' && *p <= '9')
    {
      digits++;
      if(digits > 3)
        return(false);
      value = value * 10 + (unsigned)(*p - '0');
      continue;
    }

    if(digits == 0 || value > 255)
      return(false);

    octets++;
    digits = 0;
    value  = 0;

    if(*p == '\0')
      return(octets == 4);

    if(*p != '.')
      return(false);
  }
}

static void
irc_dossier_host_tail(const char *host, char *out, size_t out_sz)
{
  const char *last;
  const char *prev;
  if(out_sz == 0)
    return;

  out[0] = '\0';

  if(host == NULL || host[0] == '\0')
    return;

  // IPv6 — anchor on the first two groups (RIR/provider side). Fall
  // through silently on parse failure; out stays empty.
  if(strchr(host, ':') != NULL)
  {
    (void)irc_dossier_ipv6_head(host, out, out_sz);
    return;
  }

  // IPv4 literal — anchor on the first two octets (network side).
  if(irc_dossier_is_ipv4_literal(host))
  {
    const char *second;
    size_t len;
    const char *first = strchr(host, '.');
    if(first == NULL)
      return;  // unreachable: ipv4 literal always has 3 dots

    second = strchr(first + 1, '.');
    if(second == NULL)
      return;  // unreachable for same reason

    len = (size_t)(second - host);
    if(len >= out_sz)
      len = out_sz - 1;
    memcpy(out, host, len);
    out[len] = '\0';
    return;
  }

  // Hostname — anchor on the last two labels (ISP/provider side).
  last = strrchr(host, '.');

  if(last == NULL)
  {
    snprintf(out, out_sz, "%s", host);
    return;
  }

  prev = last;

  while(prev > host && *(prev - 1) != '.')
    prev--;

  snprintf(out, out_sz, "%s", prev);
}

// Extract a Libera/OFTC-style account cloak from a host string.
// Matches the prefix "user/" (case-sensitive; the networks emit it
// lowercase) and copies the remainder up to the first '/', ':' or end
// of string into out. Other cloak flavours ("projects/", "staff/") are
// deliberately NOT treated as account cloaks — they identify a role,
// not a user.
//
// Returns true when a user cloak was extracted, false otherwise.
static bool
irc_dossier_extract_cloak(const char *host, char *out, size_t out_sz)
{
  static const char prefix[] = "user/";
  static const size_t plen   = sizeof(prefix) - 1;
  size_t o;
  const char *p;

  if(out_sz == 0) return(false);
  out[0] = '\0';

  if(host == NULL)
    return(false);

  if(strncmp(host, prefix, plen) != 0)
    return(false);

  p = host + plen;
  o = 0;

  while(*p != '\0' && *p != '/' && *p != ':' && o + 1 < out_sz)
    out[o++] = *p++;

  out[o] = '\0';
  return(o > 0);
}

// Normalize an IRC ident: strip a single leading '~' (legacy identd
// unverified marker — ignored in practice since the mid-2000s) and
// lowercase every byte. out is NUL-terminated on return.
static void
irc_dossier_norm_ident(const char *ident, char *out, size_t out_sz)
{
  const char *p;
  size_t o;
  if(out_sz == 0) return;
  out[0] = '\0';

  if(ident == NULL)
    return;

  p = ident;
  if(*p == '~') p++;

  o = 0;
  while(*p != '\0' && o + 1 < out_sz)
  {
    out[o++] = (char)tolower((unsigned char)*p);
    p++;
  }
  out[o] = '\0';
}

static bool
irc_dossier_split_prefix(const char *prefix,
    char *nick, size_t nick_sz,
    char *ident, size_t ident_sz,
    char *host, size_t host_sz)
{
  const char *at;
  size_t ilen;
  const char *bang;
  size_t nlen;
  nick[0]  = '\0';
  ident[0] = '\0';
  host[0]  = '\0';

  if(prefix == NULL || prefix[0] == '\0')
    return(false);

  bang = strchr(prefix, '!');
  at = strchr(prefix, '@');

  if(bang == NULL || at == NULL || at <= bang)
    return(false);

  nlen = (size_t)(bang - prefix);
  ilen = (size_t)(at - bang - 1);

  if(nlen == 0 || ilen == 0)
    return(false);

  if(nlen >= nick_sz)   nlen  = nick_sz - 1;
  if(ilen >= ident_sz)  ilen  = ident_sz - 1;

  memcpy(nick, prefix, nlen);
  nick[nlen] = '\0';

  memcpy(ident, bang + 1, ilen);
  ident[ilen] = '\0';

  snprintf(host, host_sz, "%s", at + 1);

  return(true);
}

bool
irc_dossier_build_signature(const char *prefix, char *out, size_t out_sz)
{
  char host_tail[IRC_HOST_SZ];
  char cloak[IRC_NICK_SZ];
  int n;
  char nick[IRC_NICK_SZ];
  char ident[IRC_NICK_SZ];
  char host[IRC_HOST_SZ];

  if(out == NULL || out_sz == 0)
    return(FAIL);

  out[0] = '\0';

  if(!irc_dossier_split_prefix(prefix,
      nick,  sizeof(nick),
      ident, sizeof(ident),
      host,  sizeof(host)))
    return(FAIL);

  if(!irc_dossier_field_safe(nick)
      || !irc_dossier_field_safe(ident)
      || !irc_dossier_field_safe(host))
    return(FAIL);

  irc_dossier_host_tail(host, host_tail, sizeof(host_tail));

  irc_dossier_extract_cloak(host, cloak, sizeof(cloak));
  if(!irc_dossier_field_safe(cloak))
    return(FAIL);

  n = snprintf(out, out_sz,
      "{\"nick\":\"%s\",\"ident\":\"%s\",\"host_tail\":\"%s\",\"cloak\":\"%s\"}",
      nick, ident, host_tail, cloak);

  if(n <= 0 || (size_t)n >= out_sz)
  {
    out[0] = '\0';
    return(FAIL);
  }

  return(SUCCESS);
}

static bool
irc_dossier_parse_sig(const char *sig_json,
    char *nick, size_t nick_sz,
    char *ident, size_t ident_sz,
    char *cloak, size_t cloak_sz)
{
  struct json_object *root;
  bool ok;
  nick[0]  = '\0';
  ident[0] = '\0';
  if(cloak != NULL && cloak_sz > 0) cloak[0] = '\0';

  if(sig_json == NULL)
    return(false);

  root = json_parse_buf(sig_json,
      strlen(sig_json), "irc:dossier_score");

  if(root == NULL)
    return(false);

  ok = json_get_str(root, "nick",  nick,  nick_sz)
         && json_get_str(root, "ident", ident, ident_sz);

  if(cloak != NULL && cloak_sz > 0)
    (void)json_get_str(root, "cloak", cloak, cloak_sz);

  json_object_put(root);

  return(ok);
}

// Case-insensitive prefix-match length in alphanumeric chars only.
static size_t
irc_dossier_ci_alnum_prefix(const char *a, const char *b)
{
  size_t i = 0;
  while(a[i] != '\0' && b[i] != '\0'
      && tolower((unsigned char)a[i]) == tolower((unsigned char)b[i])
      && isalnum((unsigned char)a[i]))
    i++;
  return(i);
}

// Case-insensitive substring search in ASCII; returns match length
// when `needle` is found in `hay`, 0 otherwise.
static size_t
irc_dossier_ci_substr(const char *hay, const char *needle)
{
  size_t nlen = strlen(needle);
  if(nlen == 0) return(0);
  for(const char *h = hay; *h != '\0'; h++)
  {
    size_t i = 0;
    while(i < nlen && h[i] != '\0'
        && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
      i++;
    if(i == nlen) return(nlen);
  }
  return(0);
}

float
irc_identity_token_score(const char *token, const dossier_method_sig_t *sig)
{
  char nick[IRC_NICK_SZ], ident[IRC_NICK_SZ], cloak[IRC_NICK_SZ];
  size_t nlen;
  size_t tlen;
  size_t pref;
  if(token == NULL || sig == NULL) return(0.0f);

  if(!irc_dossier_parse_sig(sig->sig_json,
        nick, sizeof(nick), ident, sizeof(ident),
        cloak, sizeof(cloak)))
    return(0.0f);

  if(nick[0] == '\0' || token[0] == '\0') return(0.0f);

  tlen = strlen(token);
  nlen = strlen(nick);

  if(tlen == nlen && strcasecmp(token, nick) == 0) return(0.95f);

  pref = irc_dossier_ci_alnum_prefix(token, nick);
  if(pref >= 3 && tlen >= 3 && nlen >= 3) return(0.8f);

  if(irc_dossier_ci_substr(nick, token) >= 4) return(0.5f);

  return(0.0f);
}

float
irc_identity_score(const dossier_method_sig_t *a,
    const dossier_method_sig_t *b)
{
  char norm_a[IRC_NICK_SZ], norm_b[IRC_NICK_SZ];
  char nick_a[IRC_NICK_SZ],  ident_a[IRC_NICK_SZ],  cloak_a[IRC_NICK_SZ];
  char nick_b[IRC_NICK_SZ],  ident_b[IRC_NICK_SZ],  cloak_b[IRC_NICK_SZ];

  if(a == NULL || b == NULL)
    return(0.0f);

  if(!irc_dossier_parse_sig(a->sig_json,
        nick_a, sizeof(nick_a), ident_a, sizeof(ident_a),
        cloak_a, sizeof(cloak_a)))
    return(0.0f);

  if(!irc_dossier_parse_sig(b->sig_json,
        nick_b, sizeof(nick_b), ident_b, sizeof(ident_b),
        cloak_b, sizeof(cloak_b)))
    return(0.0f);

  // Strongest signal: matching user-account cloak (Libera/OFTC style
  // "user/<X>" hostmask). The network guarantees these per SASL-
  // registered account; treat them as authoritative.
  if(cloak_a[0] != '\0' && cloak_b[0] != '\0')
  {
    if(strcasecmp(cloak_a, cloak_b) == 0)
      return(1.0f);

    // Both sides cloaked but cloaks differ -> server-enforced
    // separation; these are different accounts regardless of nick
    // or ident coincidence.
    return(0.0f);
  }

  // Fallback: nick + ident match (after stripping the legacy '~'
  // marker and lowercasing both fields). This is the only signal
  // available on networks that do not support account cloaks or when
  // only one side is cloaked.
  irc_dossier_norm_ident(ident_a, norm_a, sizeof(norm_a));
  irc_dossier_norm_ident(ident_b, norm_b, sizeof(norm_b));

  if(norm_a[0] == '\0' || strcmp(norm_a, norm_b) != 0)
    return(0.0f);

  // Ident matches. Bump to high confidence only when the nicks share
  // a long-enough alphanumeric prefix (case-insensitive) — guards
  // against false merges when an ident is shared between multiple
  // real people (e.g., a default "unknown" ident on a misconfigured
  // client).
  if(irc_dossier_ci_alnum_prefix(nick_a, nick_b) >= 3)
    return(0.9f);

  return(0.4f);
}
