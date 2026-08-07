// botmanager — MIT
// urlgrabber network + parsing mechanism: find a URL on a line, fetch it
// asynchronously through the curl core, distil the document <title>, and
// announce it to the room.
//
// The title distiller is deliberately hand-rolled rather than pulled from
// a heavyweight DOM parser: we only ever want one element, we work off a
// possibly Range-truncated head, and we care about presentation — decoding
// HTML entities to real UTF-8 and collapsing whitespace so the line reads
// like something a person would type, not a raw markup dump.

#define URLGRABBER_INTERNAL
#include "urlgrabber.h"

#include "alloc.h"
#include "colors.h"
#include "curl.h"
#include "kv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ //
// Small string helpers                                                //
// ------------------------------------------------------------------ //

// Case-insensitive substring search over a NUL-terminated haystack.
static const char *
ug_stristr(const char *hay, const char *needle)
{
  size_t nlen = strlen(needle);

  if(nlen == 0)
    return(hay);

  for(; *hay != '\0'; hay++)
    if(strncasecmp(hay, needle, nlen) == 0)
      return(hay);

  return(NULL);
}

// Case-insensitive search over a bounded (not necessarily NUL-terminated)
// buffer — the response body may be binary or truncated mid-tag.
static const char *
ug_memcasefind(const char *hay, size_t hlen, const char *needle)
{
  size_t nlen = strlen(needle);

  if(nlen == 0 || hlen < nlen)
    return(NULL);

  for(size_t i = 0; i + nlen <= hlen; i++)
    if(strncasecmp(hay + i, needle, nlen) == 0)
      return(hay + i);

  return(NULL);
}

// Encode a Unicode code point as UTF-8 into buf (up to 4 bytes). Returns
// the byte count, or 0 for an out-of-range point.
static size_t
ug_utf8_encode(long cp, char *buf)
{
  if(cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    return(0);

  if(cp < 0x80)
  {
    buf[0] = (char)cp;
    return(1);
  }
  if(cp < 0x800)
  {
    buf[0] = (char)(0xC0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3F));
    return(2);
  }
  if(cp < 0x10000)
  {
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    return(3);
  }
  buf[0] = (char)(0xF0 | (cp >> 18));
  buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  buf[3] = (char)(0x80 | (cp & 0x3F));
  return(4);
}

// ------------------------------------------------------------------ //
// Host inspection (light SSRF guard)                                  //
// ------------------------------------------------------------------ //

// Extract the (lower-cased, port- and userinfo-stripped) host from a URL.
static bool
ug_host_of(const char *url, char *host, size_t cap)
{
  const char *p = ug_stristr(url, "://");
  const char *end;
  const char *at;
  size_t      n = 0;

  if(p == NULL || cap == 0)
    return(false);

  p  += 3;
  end = p + strcspn(p, "/?#");

  if((at = memchr(p, '@', (size_t)(end - p))) != NULL)   // drop user:pass@
    p = at + 1;

  if(*p == '[')                                          // [IPv6] literal
  {
    for(p++; p < end && *p != ']' && n < cap - 1; p++)
      host[n++] = (char)tolower((unsigned char)*p);
  }
  else
  {
    for(; p < end && *p != ':' && n < cap - 1; p++)      // strip :port
      host[n++] = (char)tolower((unsigned char)*p);
  }

  host[n] = '\0';
  return(n > 0);
}

// Refuse loopback / private / link-local hosts so an IRC user cannot aim
// the fetcher at an internal service. DNS names are allowed — guarding
// against a name that *resolves* to a private address needs resolution-
// time inspection and is out of scope for this cosmetic feature.
static bool
ug_host_is_safe(const char *host)
{
  unsigned a, b, c, d;
  char     tail;
  size_t   len;

  if(host[0] == '\0')
    return(false);

  if(strcasecmp(host, "localhost") == 0)
    return(false);

  len = strlen(host);
  if(len >= 6 && strcasecmp(host + len - 6, ".local") == 0)
    return(false);

  // IPv4 literal — reject the reserved / private ranges.
  if(sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
      a < 256 && b < 256 && c < 256 && d < 256)
  {
    if(a == 0 || a == 10 || a == 127)          return(false);
    if(a == 169 && b == 254)                    return(false);  // link-local
    if(a == 172 && b >= 16 && b <= 31)          return(false);  // RFC1918
    if(a == 192 && b == 168)                    return(false);
    if(a == 100 && b >= 64 && b <= 127)         return(false);  // CGNAT
    return(true);
  }

  // IPv6 literal — reject loopback, unspecified, link-local, and ULA.
  if(strchr(host, ':') != NULL)
  {
    if(strcmp(host, "::1") == 0 || strcmp(host, "::") == 0)
      return(false);
    if(strncasecmp(host, "fe80", 4) == 0)                 // link-local
      return(false);
    if(strncasecmp(host, "fc", 2) == 0 ||
        strncasecmp(host, "fd", 2) == 0)                  // unique-local
      return(false);
    {
      const char *m = ug_stristr(host, "::ffff:");        // v4-mapped
      if(m != NULL)
        return(ug_host_is_safe(m + 7));
    }
  }

  return(true);
}

// ------------------------------------------------------------------ //
// URL detection                                                       //
// ------------------------------------------------------------------ //

// Peel trailing sentence punctuation an author never meant as part of the
// link. A closing bracket is kept when its opener also appears (so URLs
// with balanced parentheses, e.g. Wikipedia, survive intact).
static void
ug_trim_url(char *s)
{
  size_t len = strlen(s);

  while(len > 0)
  {
    char c = s[len - 1];

    switch(c)
    {
      case '.': case ',': case ';': case ':':
      case '!': case '?': case '\'': case '"':
        break;
      case ')': if(strchr(s, '(') != NULL) return; break;
      case ']': if(strchr(s, '[') != NULL) return; break;
      case '}': if(strchr(s, '{') != NULL) return; break;
      case '>': if(strchr(s, '<') != NULL) return; break;
      default: return;
    }

    s[--len] = '\0';
  }
}

// True when the URL's path ends in a binary media extension — fetching it
// would only download bytes we can extract no title from.
static bool
ug_is_media(const char *url)
{
  static const char *const ext[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff",
    ".svg", ".ico", ".mp3", ".mp4", ".m4a", ".mkv", ".webm", ".mov",
    ".avi", ".mpg", ".mpeg", ".flac", ".ogg", ".wav", ".pdf", ".zip",
    ".gz", ".tar", ".rar", ".7z", ".iso", ".exe", ".dmg", ".bin", NULL
  };

  const char *end = url + strcspn(url, "?#");   // path ends at query/frag
  const char *dot = NULL;
  const char *slash = NULL;

  for(const char *c = url; c < end; c++)
  {
    if(*c == '.')       dot = c;
    else if(*c == '/')  slash = c;
  }

  if(dot == NULL || (slash != NULL && dot < slash))
    return(false);                               // no extension in the path

  {
    size_t elen = (size_t)(end - dot);

    for(int i = 0; ext[i] != NULL; i++)
      if(strlen(ext[i]) == elen && strncasecmp(ext[i], dot, elen) == 0)
        return(true);
  }

  return(false);
}

bool
ug_find_url(const char *text, char *out, size_t cap)
{
  const char *h1;
  const char *h2;
  const char *start;
  char        host[UG_HOST_SZ];
  size_t      n = 0;

  if(text == NULL || out == NULL || cap == 0)
    return(false);

  h1 = ug_stristr(text, "http://");
  h2 = ug_stristr(text, "https://");

  if(h1 == NULL && h2 == NULL)
    return(false);

  start = (h1 == NULL) ? h2 : (h2 == NULL) ? h1 : (h1 < h2 ? h1 : h2);

  // Copy up to the first whitespace or control character.
  while(start[n] != '\0' && n < cap - 1 &&
      (unsigned char)start[n] > 0x20)
  {
    out[n] = start[n];
    n++;
  }
  out[n] = '\0';

  ug_trim_url(out);

  if(out[0] == '\0' || ug_is_media(out))
    return(false);

  if(!ug_host_of(out, host, sizeof(host)) || !ug_host_is_safe(host))
    return(false);

  return(true);
}

// ------------------------------------------------------------------ //
// Title extraction                                                    //
// ------------------------------------------------------------------ //

// Decode one HTML entity beginning at s[0] == '&'. On success, writes the
// UTF-8 bytes to buf (>= 4 bytes) / *blen and returns the source bytes
// consumed. Returns 0 when there is no recognisable, ';'-terminated
// entity, leaving the caller to emit the '&' literally.
static size_t
ug_decode_entity(const char *s, size_t max, char *buf, size_t *blen)
{
  static const struct { const char *name; const char *utf8; } named[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
    { "apos", "'" }, { "nbsp", " " }, { "hellip", "\xE2\x80\xA6" },
    { "mdash", "\xE2\x80\x94" }, { "ndash", "\xE2\x80\x93" },
    { "lsquo", "\xE2\x80\x98" }, { "rsquo", "\xE2\x80\x99" },
    { "ldquo", "\xE2\x80\x9C" }, { "rdquo", "\xE2\x80\x9D" },
    { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" },
    { "trade", "\xE2\x84\xA2" }, { "deg", "\xC2\xB0" },
    { "middot", "\xC2\xB7" }, { "bull", "\xE2\x80\xA2" },
    { "eacute", "\xC3\xA9" }, { "egrave", "\xC3\xA8" }, { NULL, NULL }
  };

  size_t semi = 1;

  while(semi < max && semi <= 12 && s[semi] != ';' && s[semi] != '&')
    semi++;

  if(semi >= max || s[semi] != ';' || semi < 2)
    return(0);

  if(s[1] == '#')                                  // numeric reference
  {
    char   digits[16];
    size_t dlen = 0;
    long   cp;
    int    base = 10;
    size_t i    = 2;

    if(i < semi && (s[i] == 'x' || s[i] == 'X'))
    {
      base = 16;
      i++;
    }

    for(; i < semi && dlen < sizeof(digits) - 1; i++)
      digits[dlen++] = s[i];
    digits[dlen] = '\0';

    if(dlen == 0)
      return(0);

    cp    = strtol(digits, NULL, base);
    *blen = ug_utf8_encode(cp, buf);
    return(*blen > 0 ? semi + 1 : 0);
  }

  for(int k = 0; named[k].name != NULL; k++)        // named reference
    if(strlen(named[k].name) == semi - 1 &&
        strncmp(s + 1, named[k].name, semi - 1) == 0)
    {
      *blen = strlen(named[k].utf8);
      memcpy(buf, named[k].utf8, *blen);
      return(semi + 1);
    }

  return(0);
}

// Render decoded, whitespace-collapsed, edge-trimmed title text from a raw
// title-element body into `out`. Returns true when anything survives.
static bool
ug_render_title(const char *src, size_t slen, char *out, size_t cap)
{
  size_t o             = 0;
  bool   pending_space = false;
  bool   seen          = false;

  for(size_t i = 0; i < slen && o < cap - 1; )
  {
    unsigned char c = (unsigned char)src[i];

    if(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f')
    {
      pending_space = seen;         // ignored until a real char is seen
      i++;
      continue;
    }

    // Resolve the next glyph(s): an entity, or the literal byte.
    char   glyph[4];
    size_t glen = 0;

    if(c == '&')
    {
      size_t adv = ug_decode_entity(src + i, slen - i, glyph, &glen);

      if(adv > 0)
        i += adv;
      else
      {
        glyph[0] = '&';
        glen     = 1;
        i++;
      }
    }
    else
    {
      glyph[0] = (char)c;
      glen     = 1;
      i++;
    }

    if(glen == 0)
      continue;

    if(pending_space && o < cap - 1)
    {
      out[o++]      = ' ';
      pending_space = false;
    }

    if(o + glen >= cap)
      break;

    memcpy(out + o, glyph, glen);
    o   += glen;
    seen = true;
  }

  out[o] = '\0';
  return(seen && out[0] != '\0');
}

bool
ug_extract_title(const char *html, size_t len, char *out, size_t cap)
{
  const char *open;
  const char *gt;
  const char *close;
  const char *content;
  size_t      clen;

  if(html == NULL || len == 0 || out == NULL || cap == 0)
    return(false);

  open = ug_memcasefind(html, len, "<title");
  if(open == NULL)
    return(false);

  // Confirm this is the <title> element proper (not <titlebar> etc.): the
  // byte after "<title" must open the tag, close it, or introduce attrs.
  {
    const char *after = open + 6;                 // strlen("<title")

    if(after >= html + len)
      return(false);

    if(*after != '>' && *after != '/' && !isspace((unsigned char)*after))
      return(false);
  }

  gt = memchr(open, '>', (size_t)(html + len - open));
  if(gt == NULL)
    return(false);

  content = gt + 1;
  close   = ug_memcasefind(content, (size_t)(html + len - content),
      "</title");

  // A truncated (Range-capped) body may hold no closing tag — take what
  // we have rather than give up.
  clen = close != NULL ? (size_t)(close - content)
                       : (size_t)(html + len - content);

  return(ug_render_title(content, clen, out, cap));
}

// ------------------------------------------------------------------ //
// Async fetch + announce                                              //
// ------------------------------------------------------------------ //

// The URL is kept alongside the destination because a challenge shell earns
// one retry, and the retry re-issues the same request under another name.
typedef struct
{
  char method [METHOD_NAME_SZ];
  char channel[METHOD_CHANNEL_SZ];
  char host   [UG_HOST_SZ];
  char url    [UG_URL_SZ];
  bool retried;                       // one challenge retry, and only one
} ug_fetch_ctx_t;

// Mutually recursive with ug_fetch_submit below: a completion that turns out
// to be a challenge shell re-arms the very submit that produced it.
static void ug_fetch_done(const curl_response_t *resp);

// Only HTML-ish payloads carry a <title>. An absent Content-Type is given
// the benefit of the doubt; anything else declared non-markup is skipped.
static bool
ug_content_is_html(const char *ct)
{
  if(ct == NULL || ct[0] == '\0')
    return(true);

  return(ug_stristr(ct, "html") != NULL || ug_stristr(ct, "xml") != NULL);
}

// True when the body is a JavaScript challenge shell rather than the page —
// a document whose only content is script that solves a puzzle and resubmits.
// Such a page still carries a perfectly well-formed <title>, which is exactly
// what makes it dangerous: reddit's says "Reddit", so the naive read succeeds
// and the room is told the name of the site it was already looking at.
//
// The markers are attribute names emitted by the challenge machinery itself,
// not prose, so a page that merely *discusses* one will not match. Only the
// head of the body is scanned: a challenge shell is a stub by construction
// (reddit's is 8 KiB entire), so a marker appearing deep inside a real
// document is a false positive, not a late detection.
static bool
ug_is_challenge(const char *body, size_t len)
{
  static const char *const markers[] = {
    "name=\"js_challenge\""        // reddit, measured 2026-08-06
  };

  size_t scan = len < UG_CHALLENGE_SCAN_SZ ? len : UG_CHALLENGE_SCAN_SZ;

  for(size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
    if(ug_memcasefind(body, scan, markers[i]) != NULL)
      return(true);

  return(false);
}

// The identity worn for a challenge retry, or NULL when the operator has
// declined it. `/set kv` cannot store a genuine empty string — a bare key
// fails arg parsing and a quoted "" is stored literally — so the retry is
// waved off by the same spellings giphy's rating knob accepts.
static const char *
ug_crawler_agent(void)
{
  const char *ua = kv_get_str(UG_KV_CRAWLER_AGENT);

  if(ua == NULL || ua[0] == '\0'
      || strcmp(ua, "\"\"") == 0
      || strcasecmp(ua, "none") == 0
      || strcasecmp(ua, "off") == 0)
    return(NULL);

  return(ua);
}

// Build and submit the GET described by `fc` under the identity `ua` — the
// browser's on the first attempt, the link-preview crawler's on a challenge
// retry. The identity is passed rather than looked up so that the caller
// deciding to retry and the request carrying that decision out cannot read
// two different values of the knob. Takes ownership of `fc` unconditionally:
// it is freed here if the request never reaches the wire, and by
// ug_fetch_done otherwise.
static void
ug_fetch_submit(ug_fetch_ctx_t *fc, const char *ua)
{
  // The headers a browser sends on a top-level navigation, in the order it
  // sends them. The first UG_NAV_GENERIC suit any HTTP client; the tail is a
  // browser-only claim. See the note at the add loop for why that split, and
  // the order, are not incidental.
  static const char *const nav_headers[] = {
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language: en-US,en;q=0.9",
    "Upgrade-Insecure-Requests: 1",
    "Sec-Fetch-Dest: document",
    "Sec-Fetch-Mode: navigate",
    "Sec-Fetch-Site: none",
    "Sec-Fetch-User: ?1"
  };

  curl_request_t *req;
  uint32_t        timeout;
  uint32_t        maxbytes;
  size_t          nhdrs;

  if((req = curl_request_create(CURL_METHOD_GET, fc->url,
          ug_fetch_done, fc)) == NULL)
  {
    mem_free(fc);
    return;
  }

  timeout = (uint32_t)kv_get_uint(UG_KV_TIMEOUT);
  if(timeout > 0)
    curl_request_set_timeout(req, timeout);

  if(ua != NULL && ua[0] != '\0')
    curl_request_set_user_agent(req, ua);

  // Idempotent, best-effort, and first to shed on shutdown drain.
  curl_request_set_prio(req, CURL_PRIO_BULK);
  curl_request_set_follow_redirects(req, true);

  // Bound the download: the <title> lives in the head, so we ask for only
  // the first slice. Servers that ignore Range are still capped by the
  // curl core's global response ceiling.
  maxbytes = (uint32_t)kv_get_uint(UG_KV_MAX_BYTES);
  if(maxbytes > 0)
  {
    char range[64];

    snprintf(range, sizeof(range), "Range: bytes=0-%u", maxbytes - 1);
    curl_request_add_header(req, range);
  }

  // `user_agent` claims Firefox; a request that claims Firefox and then
  // arrives without Fetch Metadata claims two contradictory things, and a
  // fingerprinting WAF scores the contradiction rather than the UA. Akamai
  // answers such a request with an HTTP/2 RST_STREAM *before* any status
  // line, so the drop reaches us as a transport error and never looks like
  // the 403 a block is expected to be. Measured 2026-08-06 against
  // washingtonpost.com: refused 3/3 without these headers, served 3/3 with
  // them (any single Sec-Fetch-* flips it), and no other site in a 17-URL
  // sweep changed status. The curl core *prepends*, so walk the list
  // backwards to land it on the wire in browser order — after which the
  // Range above, added first, trails the set as the one non-browser tell.
  //
  // The same lesson runs the other way on the retry. Fetch Metadata is a
  // browser's account of a click it did not make on our behalf, so a crawler
  // that sends it contradicts itself precisely as the bare Firefox UA once
  // did. The retry stops at the generic two and makes no claim it cannot own.
  nhdrs = fc->retried ? UG_NAV_GENERIC
                      : sizeof(nav_headers) / sizeof(nav_headers[0]);

  for(size_t i = nhdrs; i > 0; i--)
    curl_request_add_header(req, nav_headers[i - 1]);

  // On submit failure the curl core releases `req` itself; we still own —
  // and must free — the context, since the callback will never fire.
  if(curl_request_submit(req) != SUCCESS)
    mem_free(fc);
}

// Spend the single retry `fc` is allowed, wearing the link-preview crawler's
// name; `reason` names the trigger for the log. Returns false when the retry
// is unavailable — already spent, or declined by the operator — leaving the
// caller to drop as it would have. On true, ownership of `fc` has passed to
// the new request and the caller must not touch it again.
static bool
ug_retry_as_crawler(ug_fetch_ctx_t *fc, const char *reason)
{
  const char *crawler;

  if(fc->retried)
    return(false);

  if((crawler = ug_crawler_agent()) == NULL)
    return(false);

  clam(CLAM_INFO, UG_CTX, "%s: %s, retrying as a link-preview crawler",
      fc->host, reason);

  fc->retried = true;
  ug_fetch_submit(fc, crawler);
  return(true);
}

// Completion callback — runs on the curl worker thread, so it stays fast:
// parse, format, hand the line to the method layer, free the context.
static void
ug_fetch_done(const curl_response_t *resp)
{
  ug_fetch_ctx_t *fc = resp->user_data;
  method_inst_t  *method;
  char            title[UG_TITLE_SZ];
  char            line[UG_TITLE_SZ + 64];
  uint32_t        maxlen;

  if(fc == NULL)
    return;

  // Every drop path below used to be silent, which is exactly what made a
  // WAF 403 look like a random "some URLs get ignored". Name the reason:
  // a real failure (transport error, HTTP >= 400) is worth INFO so it shows
  // up in a normal log; a benign miss (shutdown, non-markup, no <title>) is
  // DEBUG so it doesn't nag.
  if(resp->cancelled)
  {
    clam(CLAM_DEBUG, UG_CTX, "%s: fetch cancelled (drain), skipped",
        fc->host);
    goto out;
  }

  if(resp->error != NULL)
  {
    clam(CLAM_INFO, UG_CTX, "%s: fetch error, skipped (%s)",
        fc->host, resp->error);
    goto out;
  }

  if(resp->status < 200 || resp->status >= 400)
  {
    clam(CLAM_INFO, UG_CTX, "%s: HTTP %ld, skipped", fc->host, resp->status);
    goto out;
  }

  if(resp->body == NULL || resp->body_len == 0)
  {
    clam(CLAM_INFO, UG_CTX, "%s: empty body, skipped", fc->host);
    goto out;
  }

  if(!ug_content_is_html(resp->content_type))
  {
    clam(CLAM_DEBUG, UG_CTX, "%s: non-markup content-type '%s', skipped",
        fc->host, resp->content_type != NULL ? resp->content_type : "");
    goto out;
  }

  // A 200 carrying a challenge shell is a refusal wearing a success code,
  // and its <title> names the site rather than the page. Some such sites
  // keep the real markup for a named link-preview crawler — which is the one
  // thing we honestly are — so spend exactly one more request saying so.
  if(ug_is_challenge(resp->body, resp->body_len))
  {
    if(ug_retry_as_crawler(fc, "JavaScript challenge"))
      return;                      // ownership passed; must not fall through

    clam(CLAM_DEBUG, UG_CTX, "%s: JavaScript challenge, not retried, skipped",
        fc->host);
    goto out;
  }

  // A page that yields no title at all is the one place a retry is free: we
  // are about to say nothing, so the worst a second attempt can do is leave
  // us exactly as silent. That costs no site its title — a page the browser
  // identity reads fine never reaches this branch — and it picks up the
  // sites that serve a title only to a crawler without ever announcing a
  // challenge (cnn.com and imdb.com, measured 2026-08-06).
  if(!ug_extract_title(resp->body, resp->body_len, title, sizeof(title)))
  {
    if(ug_retry_as_crawler(fc, "no <title> in body"))
      return;                      // ownership passed; must not fall through

    clam(CLAM_DEBUG, UG_CTX, "%s: no <title> in body, skipped", fc->host);
    goto out;
  }

  // Cosmetic length cap, with an ellipsis to signal the cut.
  maxlen = (uint32_t)kv_get_uint(UG_KV_MAX_TITLE);
  if(maxlen > 0 && maxlen < sizeof(title) - 4 && strlen(title) > maxlen)
    memcpy(title + maxlen, "\xE2\x80\xA6", 4);     // "…" + NUL

  if((method = method_find(fc->method)) == NULL)
    goto out;                                      // bot/method torn down

  // The title, and nothing else. The host used to trail the line in grey,
  // which told the room something it already knew — a URL cannot be grabbed
  // without first being pasted in front of everyone. It survives in `fc`
  // because every drop path above logs under it, and a diagnostic that
  // names the site is the whole reason a silent WAF block is findable.
  snprintf(line, sizeof(line), CLR_CYAN "\xC2\xBB" CLR_RESET " %s", title);

  method_send(method, fc->channel, line);

out:
  mem_free(fc);
}

void
ug_fetch(const char *method_name, const char *channel, const char *url)
{
  ug_fetch_ctx_t *fc;

  if(method_name == NULL || channel == NULL || url == NULL)
    return;

  if((fc = mem_alloc(UG_CTX, "fetchctx", sizeof(*fc))) == NULL)
    return;

  memset(fc, 0, sizeof(*fc));
  snprintf(fc->method,  sizeof(fc->method),  "%s", method_name);
  snprintf(fc->channel, sizeof(fc->channel), "%s", channel);
  snprintf(fc->url,     sizeof(fc->url),     "%s", url);
  ug_host_of(url, fc->host, sizeof(fc->host));

  ug_fetch_submit(fc, kv_get_str(UG_KV_USER_AGENT));
}
