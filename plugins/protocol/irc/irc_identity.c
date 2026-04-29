// botmanager — MIT
// IRC identity field projection: parses an IRC prefix
// (nick!ident@host) into the generic nickname/username/hostname/
// verified_id tuple consumers see in method_msg_t. Lives in its own
// translation unit so unit tests can link the helpers without pulling
// in the whole IRC plugin.

#include "irc_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "irc.h"

// Byte-safety check shared by every projected field. We refuse control
// bytes and the two characters that would require JSON escaping if a
// downstream consumer ever serializes the tuple — keeps the fields
// shell- and JSON-safe at the source.
static bool
irc_identity_byte_safe(unsigned char c)
{
  if(c < 0x20 || c == '"' || c == '\\')
    return(false);

  return(true);
}

static bool
irc_identity_field_safe(const char *s)
{
  for(; *s != '\0'; s++)
    if(!irc_identity_byte_safe((unsigned char)*s))
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
irc_identity_parse_ipv6_group(const char **p)
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
irc_identity_ipv6_head(const char *host, char *out, size_t out_sz)
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
  p  = host;

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

    v1 = irc_identity_parse_ipv6_group(&p);
    if(v1 < 0 || *p != ':')
      return(false);
    g1 = (unsigned)v1;
    p++;

    // A second ':' right here means the '::' run starts at slot 2,
    // so g2 lives inside the zero fill.
    if(*p != ':')
    {
      int v2 = irc_identity_parse_ipv6_group(&p);
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

// Detect a dotted-quad IPv4 literal: four labels, each 1-3 digits in
// the range 0-255, separated by exactly three dots.
static bool
irc_identity_is_ipv4_literal(const char *host)
{
  unsigned value;
  int digits;
  int octets;

  if(host == NULL || host[0] == '\0')
    return(false);

  octets = 0;
  digits = 0;
  value  = 0;

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

// Project a host into its identifying tail. See irc_identity.h header
// comment for the asymmetry rationale (hostnames vs IP literals).
static void
irc_identity_host_tail(const char *host, char *out, size_t out_sz)
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
    (void)irc_identity_ipv6_head(host, out, out_sz);
    return;
  }

  // IPv4 literal — anchor on the first two octets (network side).
  if(irc_identity_is_ipv4_literal(host))
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

// Extract a Libera/OFTC-style user-account cloak from a host string.
// Matches the prefix "user/" (case-sensitive; the networks emit it
// lowercase) and copies the remainder up to the first '/', ':' or end
// of string into out. Other cloak flavours ("projects/", "staff/") are
// deliberately NOT treated as account cloaks — they identify a role,
// not a user.
//
// Returns true when a user cloak was extracted, false otherwise.
static bool
irc_identity_extract_cloak(const char *host, char *out, size_t out_sz)
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

// Normalize an IRC ident into the generic username field: strip a
// single leading '~' (legacy identd unverified marker — ignored in
// practice since the mid-2000s) and lowercase every byte.
static void
irc_identity_norm_ident(const char *ident, char *out, size_t out_sz)
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

// Split "nick!ident@host" into its three components. Returns false on
// any malformed input. Output buffers are NUL-terminated on success
// and reset to "" on failure.
static bool
irc_identity_split_prefix(const char *prefix,
    char *nick,  size_t nick_sz,
    char *ident, size_t ident_sz,
    char *host,  size_t host_sz)
{
  const char *at;
  size_t      ilen;
  const char *bang;
  size_t      nlen;

  nick[0]  = '\0';
  ident[0] = '\0';
  host[0]  = '\0';

  if(prefix == NULL || prefix[0] == '\0')
    return(false);

  bang = strchr(prefix, '!');
  at   = strchr(prefix, '@');

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
irc_identity_fill_quad(const char *prefix,
    char *out_nickname,    size_t out_nickname_sz,
    char *out_username,    size_t out_username_sz,
    char *out_hostname,    size_t out_hostname_sz,
    char *out_verified_id, size_t out_verified_id_sz)
{
  char nick [IRC_NICK_SZ];
  char ident[IRC_NICK_SZ];
  char host [IRC_HOST_SZ];

  if(out_nickname == NULL    || out_nickname_sz    == 0
      || out_username == NULL    || out_username_sz    == 0
      || out_hostname == NULL    || out_hostname_sz    == 0
      || out_verified_id == NULL || out_verified_id_sz == 0)
    return(FAIL);

  out_nickname   [0] = '\0';
  out_username   [0] = '\0';
  out_hostname   [0] = '\0';
  out_verified_id[0] = '\0';

  if(!irc_identity_split_prefix(prefix,
        nick,  sizeof(nick),
        ident, sizeof(ident),
        host,  sizeof(host)))
    return(FAIL);

  if(!irc_identity_field_safe(nick)
      || !irc_identity_field_safe(ident)
      || !irc_identity_field_safe(host))
    return(FAIL);

  // nickname: nick verbatim.
  snprintf(out_nickname, out_nickname_sz, "%s", nick);

  // username: ident with leading '~' stripped, lowercased.
  irc_identity_norm_ident(ident, out_username, out_username_sz);

  // hostname: host_tail.
  irc_identity_host_tail(host, out_hostname, out_hostname_sz);

  // verified_id: SASL account portion of a "user/<X>" cloak, when present.
  (void)irc_identity_extract_cloak(host,
      out_verified_id, out_verified_id_sz);

  if(!irc_identity_field_safe(out_verified_id))
  {
    out_verified_id[0] = '\0';
    return(FAIL);
  }

  return(SUCCESS);
}
