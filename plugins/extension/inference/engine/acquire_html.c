// botmanager — MIT
// Acquisition engine: HTML-to-text stripper + image extractor.

#include "acquire_priv.h"
#include "knowledge_priv.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Minimal in-place HTML-to-text stripper for digest input. Walks the
// input once:
//   - Tags (<...>) are elided. Script/style blocks are stripped as
//     ordinary tags; their contents remain but the digester tolerates
//     the noise fine.
//   - A small set of entities is decoded (&amp; &lt; &gt; &quot;
//     &apos; &nbsp; &#38; &#039;). Unknown entities copy through.
//   - Whitespace runs collapse to single spaces.
//   - Leading/trailing whitespace trimmed.
//
// The output is NUL-terminated, guaranteed <= out_cap - 1 bytes.
// Returns bytes written (excl. NUL).
size_t
acq_strip_html(const char *in, size_t in_len, char *out, size_t out_cap)
{
  size_t w;
  bool   last_ws;
  if(out_cap == 0) return(0);

  w = 0;
  last_ws = true;

  for(size_t i = 0; i < in_len && w + 1 < out_cap; i++)
  {
    unsigned char c = (unsigned char)in[i];

    // Tag elision.
    if(c == '<')
    {
      while(i < in_len && in[i] != '>')
        i++;
      continue;
    }

    // Entity decode (very small table).
    if(c == '&')
    {
      // Find the ';' within a short window.
      size_t end = i + 1;

      while(end < in_len && end - i < 8 && in[end] != ';')
        end++;

      if(end < in_len && in[end] == ';')
      {
        size_t len = end - i - 1;
        const char *ent = in + i + 1;
        char decoded = 0;

        if(len == 3 && strncasecmp(ent, "amp", 3) == 0)   decoded = '&';
        else if(len == 2 && strncasecmp(ent, "lt", 2) == 0)   decoded = '<';
        else if(len == 2 && strncasecmp(ent, "gt", 2) == 0)   decoded = '>';
        else if(len == 4 && strncasecmp(ent, "quot", 4) == 0) decoded = '"';
        else if(len == 4 && strncasecmp(ent, "apos", 4) == 0) decoded = '\'';
        else if(len == 4 && strncasecmp(ent, "nbsp", 4) == 0) decoded = ' ';
        else if(len >= 2 && ent[0] == '#')
        {
          // Numeric entity — accept decimal only, common subset.
          long v = strtol(ent + 1, NULL, 10);
          if(v > 0 && v < 128) decoded = (char)v;
        }

        if(decoded != 0)
        {
          if(decoded == ' ' || decoded == '\t' || decoded == '\n')
          {
            if(!last_ws)
            {
              out[w++] = ' ';
              last_ws = true;
            }
          }

          else
          {
            out[w++] = decoded;
            last_ws  = false;
          }

          i = end;                // jump past ';'
          continue;
        }

        // Unknown entity — skip the whole &...; token to avoid noise.
        i = end;
        continue;
      }

      // No terminator — treat '&' as literal.
    }

    if(c == '\r' || c == '\n' || c == '\t' || c == ' ')
    {
      if(!last_ws)
      {
        out[w++] = ' ';
        last_ws  = true;
      }
      continue;
    }

    out[w++] = (char)c;
    last_ws  = false;
  }

  // Trim trailing whitespace.
  while(w > 0 && out[w - 1] == ' ')
    w--;

  out[w] = '\0';
  return(w);
}

// I1 — image extractor.
//
// Runs during the curl callback (acq_reactive_curl_done) while the raw
// HTML body is still valid. Hand-rolled scanner — no third-party HTML
// parser, pragmatic rather than strict:
//
//   - Scans for <img ...>, <meta property="og:image" ...>,
//     <meta name="twitter:image" ...>, plus <meta property="og:title"
//     ...> and <title>…</title> for the page-level caption fallback.
//   - Resolves relative URLs against the page URL (scheme/host for
//     leading "/", scheme for leading "//", page-dir for anything
//     else). Drops data: URIs outright.
//   - Filters: KV-driven substring skiplist (case-insensitive), min
//     declared dimension, optional require-dims-declared mode.
//   - OpenGraph image anchors position 0; Twitter card dedupes
//     against OG; subsequent <img> tags fill remaining slots up to
//     the KV cap. Hard compile-time ceiling ACQUIRE_IMAGE_MAX_PER_PAGE_CEIL.
//
// The scanner is deliberately tolerant of malformed markup: any
// parsing step that can't complete within a bounded window simply
// drops the current match and moves on. The point is best-effort
// harvest, not spec compliance.

// Lowercase in place through the end of a fixed-size buffer. Cheap
// alternative to pulling in <ctype.h> per-scan for just this helper.
static void
acq_str_lower(char *s)
{
  for(; *s != '\0'; s++)
    if(*s >= 'A' && *s <= 'Z')
      *s = (char)(*s + ('a' - 'A'));
}

// Trim leading/trailing ASCII whitespace in place. Returns new length.
static size_t
acq_str_trim(char *s)
{
  size_t len = strlen(s);

  size_t lead;
  while(len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'
        || s[len - 1] == '\r' || s[len - 1] == '\n'))
    s[--len] = '\0';

  lead = 0;

  while(s[lead] == ' ' || s[lead] == '\t'
      || s[lead] == '\r' || s[lead] == '\n')
    lead++;

  if(lead > 0)
  {
    memmove(s, s + lead, len - lead + 1);
    len -= lead;
  }

  return(len);
}

// Parse the raw `acquire.image_url_skiplist` KV string into a freshly
// populated cfg fragment. Caller holds no locks; result is meant to be
// stored under acquire_cfg_mutex alongside the rest of the snapshot.
void
acq_parse_skiplist(const char *raw, char out[][ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ],
    size_t *n_out)
{
  char buf[ACQUIRE_IMAGE_SKIPLIST_MAX * ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ + 8];
  char *saveptr;
  size_t n;
  *n_out = 0;

  if(raw == NULL || raw[0] == '\0')
    return;

  // Work on a bounded local copy — strtok_r mutates, and the KV layer
  // hands back a pointer into its own storage.
  snprintf(buf, sizeof(buf), "%s", raw);

  saveptr = NULL;
  n = 0;

  for(char *tok = strtok_r(buf, ";", &saveptr);
      tok != NULL && n < ACQUIRE_IMAGE_SKIPLIST_MAX;
      tok = strtok_r(NULL, ";", &saveptr))
  {
    char slot[ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ];

    snprintf(slot, sizeof(slot), "%s", tok);

    if(acq_str_trim(slot) == 0)
      continue;

    acq_str_lower(slot);
    snprintf(out[n], ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ, "%s", slot);
    n++;
  }

  *n_out = n;
}

// Case-insensitive substring: returns the first occurrence of `needle`
// (already lowercase) in `hay`, or NULL. `hay_len` is optional — pass
// SIZE_MAX for NUL-terminated strings. Avoids the non-POSIX strcasestr.
static const char *
acq_strcasestr_n(const char *hay, size_t hay_len, const char *needle)
{
  size_t nlen = strlen(needle);

  size_t last;
  if(nlen == 0)
    return(hay);

  if(hay_len == (size_t)-1)
    hay_len = strlen(hay);

  if(hay_len < nlen)
    return(NULL);

  last = hay_len - nlen;

  for(size_t i = 0; i <= last; i++)
  {
    size_t j = 0;

    while(j < nlen)
    {
      char a = hay[i + j];
      char b = needle[j];

      if(a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));

      if(a != b)
        break;

      j++;
    }

    if(j == nlen)
      return(hay + i);
  }

  return(NULL);
}

// Match a fixed literal at position `p` in a bounded window, case-
// insensitive. Returns true on match; never reads past `end`.
static bool
acq_tag_starts(const char *p, const char *end, const char *lit)
{
  size_t llen = strlen(lit);

  if((size_t)(end - p) < llen)
    return(false);

  for(size_t i = 0; i < llen; i++)
  {
    char a = p[i];
    char b = lit[i];

    if(a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if(b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));

    if(a != b)
      return(false);
  }

  return(true);
}

// Find the tag terminator '>' starting at `p`, not past `end`. Returns
// a pointer to the '>' (exclusive of attribute bodies, same-level) or
// NULL if not found in the window. For our purposes tags live on one
// conceptual line; we don't need quoted-attribute awareness here
// because the downstream attribute extractor handles quoting.
static const char *
acq_find_tag_end(const char *p, const char *end)
{
  // Tolerate quoted attribute values that contain '>' characters.
  bool in_s = false;    // inside single-quoted value
  bool in_d = false;    // inside double-quoted value

  for(const char *q = p; q < end; q++)
  {
    if(*q == '\\' && q + 1 < end) { q++; continue; }
    if(!in_d && *q == '\'')        in_s = !in_s;
    else if(!in_s && *q == '"')    in_d = !in_d;
    else if(!in_s && !in_d && *q == '>') return(q);
  }

  return(NULL);
}

// Extract an attribute value from a tag body [tag_start, tag_end).
// Attribute name matched case-insensitively; value may be single-,
// double-quoted, or unquoted (up to whitespace / tag-end). Copies at
// most `out_cap - 1` bytes, NUL-terminates, returns true on hit.
static bool
acq_extract_attr(const char *tag_start, const char *tag_end,
    const char *name, char *out, size_t out_cap)
{
  size_t nlen;
  const char *p;
  if(out_cap == 0)
    return(false);

  out[0] = '\0';
  nlen = strlen(name);

  p = tag_start;

  while(p < tag_end)
  {
    // Skip to next attribute boundary (whitespace).
    bool name_match;
    const char *after;
    const char *v;
    char   quote;
    size_t vlen;
    const char *vs;
    const char *ve;
    while(p < tag_end && *p != ' ' && *p != '\t'
        && *p != '\n' && *p != '\r')
      p++;

    while(p < tag_end && (*p == ' ' || *p == '\t'
        || *p == '\n' || *p == '\r'))
      p++;

    if(p >= tag_end) break;

    // Compare name prefix (case-insensitive).
    if((size_t)(tag_end - p) < nlen + 1)
      break;

    name_match = true;

    for(size_t i = 0; i < nlen; i++)
    {
      char a = p[i];
      char b = name[i];

      if(a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
      if(b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));

      if(a != b) { name_match = false; break; }
    }

    if(!name_match)
    {
      p++;
      continue;
    }

    // Confirm boundary after name: '=' or whitespace.
    after = p + nlen;

    if(after >= tag_end)
      break;

    if(*after != '=' && *after != ' ' && *after != '\t'
        && *after != '\n' && *after != '\r' && *after != '/')
    {
      p++;
      continue;
    }

    // Skip spaces + '='.
    v = after;

    while(v < tag_end && (*v == ' ' || *v == '\t'
        || *v == '\n' || *v == '\r'))
      v++;

    if(v >= tag_end || *v != '=')
    {
      p++;
      continue;
    }

    v++;

    while(v < tag_end && (*v == ' ' || *v == '\t'
        || *v == '\n' || *v == '\r'))
      v++;

    if(v >= tag_end)
      break;

    quote = '\0';

    if(*v == '"' || *v == '\'')
    {
      quote = *v++;
      vs = v;
      ve = v;

      while(ve < tag_end && *ve != quote)
        ve++;
    }

    else
    {
      vs = v;
      ve = v;

      while(ve < tag_end && *ve != ' ' && *ve != '\t'
          && *ve != '\n' && *ve != '\r' && *ve != '/')
        ve++;
    }

    vlen = (size_t)(ve - vs);

    if(vlen >= out_cap) vlen = out_cap - 1;

    memcpy(out, vs, vlen);
    out[vlen] = '\0';

    return(true);
  }

  return(false);
}

// Parse an integer attribute value. Returns 0 when the attribute is
// missing or unparseable (e.g. "50%", "auto"). The caller treats 0 as
// "not declared" uniformly.
static int
acq_extract_attr_int(const char *tag_start, const char *tag_end,
    const char *name)
{
  char buf[32];

  long v;
  if(!acq_extract_attr(tag_start, tag_end, name, buf, sizeof(buf)))
    return(0);

  v = strtol(buf, NULL, 10);

  if(v <= 0 || v > INT32_MAX)
    return(0);

  return((int)v);
}

// Resolve `src` against `page_url`:
//   - "data:*"       → dropped (returns false)
//   - "http*://*"    → copied as-is
//   - "//host/path"  → prefix with "https:"
//   - "/path..."     → attach to scheme+host of page_url
//   - anything else  → strip page_url's query/fragment, take path up to
//                      last '/', append src.
// Copies into `out` (NUL-terminated, truncated on overflow).
static bool
acq_resolve_image_url(const char *src, const char *page_url,
    char *out, size_t out_cap)
{
  const char *proto_end;
  const char *host_start;
  size_t scheme_len;
  const char *path_end;
  const char *last_slash;
  size_t dir_len;
  const char *host_end;
  size_t host_len;
  const char *walk;
  char root[512];
  if(src == NULL || src[0] == '\0' || out_cap == 0)
    return(false);

  out[0] = '\0';

  if(strncasecmp(src, "data:", 5) == 0)
    return(false);

  if(strncasecmp(src, "http://",  7) == 0
      || strncasecmp(src, "https://", 8) == 0)
  {
    snprintf(out, out_cap, "%s", src);
    return(true);
  }

  if(src[0] == '/' && src[1] == '/')
  {
    snprintf(out, out_cap, "https:%s", src);
    return(true);
  }

  // Anything from here on needs the page URL's scheme + host, which we
  // parse strictly enough to keep the resolver tight.
  proto_end = strstr(page_url != NULL ? page_url : "", "://");

  if(proto_end == NULL)
  {
    // Can't resolve — copy the raw src and let the skiplist / downstream
    // filters drop it if pathological.
    snprintf(out, out_cap, "%s", src);
    return(true);
  }

  host_start = proto_end + 3;
  host_end = host_start;

  while(*host_end != '\0' && *host_end != '/' && *host_end != '?'
      && *host_end != '#')
    host_end++;

  scheme_len = (size_t)(proto_end - page_url);
  host_len = (size_t)(host_end - host_start);

  // Compose scheme://host into a local buffer first; this lets both
  // branches below reuse it without slicing through page_url a second
  // time.

  if(scheme_len + host_len + 4 >= sizeof(root))
    return(false);

  snprintf(root, sizeof(root), "%.*s://%.*s",
      (int)scheme_len, page_url,
      (int)host_len, host_start);

  if(src[0] == '/')
  {
    snprintf(out, out_cap, "%s%s", root, src);
    return(true);
  }

  // Relative path: strip query/fragment, take directory (path up to
  // last '/'), append src.
  path_end = host_end;

  while(*path_end != '\0' && *path_end != '?' && *path_end != '#')
    path_end++;

  // Rewind to the last '/' in the path portion (after the host).
  last_slash = host_end;
  walk = host_end;

  while(walk < path_end)
  {
    if(*walk == '/')
      last_slash = walk;
    walk++;
  }

  dir_len = (size_t)(last_slash - host_start);

  if(host_len + dir_len + strlen(src) + 8 >= out_cap)
    return(false);

  snprintf(out, out_cap, "%s%.*s/%s",
      root, (int)dir_len, host_start, src);

  return(true);
}

// Find <title>…</title>; copy contents into `out`, NUL-terminated.
// Returns true on a hit. Length-bounded; suitable for sub-scanning.
// Exposed via acquire_priv.h for acquire_feed.c's HTML path.
bool
acq_find_title(const char *html, size_t html_len,
    char *out, size_t out_cap)
{
  const char *end = html + html_len;
  const char *open_tag = NULL;

  const char *tag_close;
  const char *body;
  size_t n;
  const char *close_tag;
  for(const char *p = html; p + 6 < end; p++)
  {
    if(*p == '<' && acq_tag_starts(p, end, "<title"))
    {
      open_tag = p;
      break;
    }
  }

  if(open_tag == NULL)
    return(false);

  tag_close = acq_find_tag_end(open_tag, end);

  if(tag_close == NULL)
    return(false);

  body = tag_close + 1;
  close_tag = NULL;

  for(const char *p = body; p + 7 < end; p++)
  {
    if(*p == '<' && acq_tag_starts(p, end, "</title"))
    {
      close_tag = p;
      break;
    }
  }

  if(close_tag == NULL)
    return(false);

  n = (size_t)(close_tag - body);

  if(n >= out_cap) n = out_cap - 1;

  memcpy(out, body, n);
  out[n] = '\0';
  acq_str_trim(out);
  return(out[0] != '\0');
}

// Snapshot just the image-filter bits of acquire_cfg while holding the
// cfg mutex once. Keeps the hot-path scan out of the lock.
typedef struct
{
  bool     enabled;
  uint32_t min_dim_px;
  uint32_t max_per_page;
  bool     require_dims;
  char     skiplist[ACQUIRE_IMAGE_SKIPLIST_MAX]
                  [ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ];
  size_t   skiplist_n;
} acq_image_cfg_t;

static void
acq_image_cfg_snapshot(acq_image_cfg_t *out)
{
  pthread_mutex_lock(&acquire_cfg_mutex);
  out->enabled      = acquire_cfg.images_enabled;
  out->min_dim_px   = acquire_cfg.image_min_dim_px;
  out->max_per_page = acquire_cfg.image_max_per_page;
  out->require_dims = acquire_cfg.image_require_dims;
  out->skiplist_n   = acquire_cfg.image_skiplist_n;
  memcpy(out->skiplist, acquire_cfg.image_skiplist, sizeof(out->skiplist));
  pthread_mutex_unlock(&acquire_cfg_mutex);
}

// Returns true if `url` matches any entry in the snapshot's skiplist.
static bool
acq_url_skiplisted(const acq_image_cfg_t *cfg, const char *url)
{
  for(size_t i = 0; i < cfg->skiplist_n; i++)
    if(acq_strcasestr_n(url, (size_t)-1, cfg->skiplist[i]) != NULL)
      return(true);

  return(false);
}

// Would adding a URL to `out` duplicate an existing slot? Case-
// case-insensitive but once a URL lands through resolution, a bit-for-
// bit repeat is the only duplicate we can reliably collapse without
// fully canonicalising.
static bool
acq_out_contains(const acq_image_extract_t *out, size_t n, const char *url)
{
  for(size_t i = 0; i < n; i++)
    if(strcmp(out[i].url, url) == 0)
      return(true);

  return(false);
}

// Apply the config filters to a candidate image and, if accepted, write
// it into `out[*n]`. Returns true when the slot advanced. `had_dims`
// records whether the source HTML declared width/height attributes
// (independent of value).
static bool
acq_image_accept(const acq_image_cfg_t *cfg, const char *url,
    const char *caption, int width_px, int height_px, bool had_dims,
    acq_image_extract_t *out, size_t *n, size_t cap)
{
  uint32_t min_d;
  acq_image_extract_t *slot;
  if(url == NULL || url[0] == '\0' || *n >= cap)
    return(false);

  if(acq_url_skiplisted(cfg, url))
    return(false);

  // Minimum declared dimension filter: skip when ANY declared side is
  // below threshold. Undeclared sides (0) are not penalised here — the
  // require_dims rule below owns that policy.
  min_d = cfg->min_dim_px;

  if(width_px > 0 && (uint32_t)width_px < min_d)
    return(false);

  if(height_px > 0 && (uint32_t)height_px < min_d)
    return(false);

  if(cfg->require_dims && !had_dims)
    return(false);

  if(acq_out_contains(out, *n, url))
    return(false);

  slot = &out[*n];

  snprintf(slot->url,     sizeof(slot->url),     "%s", url);
  snprintf(slot->caption, sizeof(slot->caption), "%s",
      caption != NULL ? caption : "");
  slot->width_px  = width_px;
  slot->height_px = height_px;

  (*n)++;
  return(true);
}

// Main scanner. Walks the HTML body once, harvesting OpenGraph first,
// Twitter-card second, then in-body <img> tags until the cap is hit.
// Caption fallback chain for OG/Twitter: og:title → <title> → empty.
size_t
acq_extract_images(const char *html, size_t html_len,
    const char *page_url, acq_image_extract_t *out, size_t cap)
{
  acq_image_cfg_t cfg;
  size_t runtime_cap;
  size_t n;
  const char *end;
  const char *meta_caption;
  char page_title[KNOWLEDGE_IMAGE_CAPTION_SZ];
  char og_image     [KNOWLEDGE_IMAGE_URL_SZ];
  char og_title  [KNOWLEDGE_IMAGE_CAPTION_SZ];
  char twitter_image[KNOWLEDGE_IMAGE_URL_SZ];
  if(html == NULL || html_len == 0 || cap == 0)
    return(0);

  acq_image_cfg_snapshot(&cfg);

  if(!cfg.enabled || cfg.max_per_page == 0)
    return(0);

  runtime_cap = cfg.max_per_page;

  if(runtime_cap > cap)
    runtime_cap = cap;

  n = 0;

  // Caption fallbacks — scanned once up front.
  page_title[0] = '\0';
  og_title[0] = '\0';

  acq_find_title(html, html_len, page_title, sizeof(page_title));

  // Walk once for og:* / twitter:* metas and <img> tags.
  end = html + html_len;
  og_image[0] = '\0';
  twitter_image[0] = '\0';

  for(const char *p = html; p < end; )
  {
    const char *tag_end;
    if(*p != '<')
    {
      p++;
      continue;
    }

    tag_end = acq_find_tag_end(p, end);

    if(tag_end == NULL)
      break;

    if(acq_tag_starts(p, end, "<meta"))
    {
      char prop [64] = "";
      char nameb[64] = "";
      char content[KNOWLEDGE_IMAGE_URL_SZ] = "";

      acq_extract_attr(p, tag_end, "property", prop,  sizeof(prop));
      acq_extract_attr(p, tag_end, "name",     nameb, sizeof(nameb));
      acq_extract_attr(p, tag_end, "content",  content, sizeof(content));

      if(strcasecmp(prop, "og:image") == 0 && og_image[0] == '\0')
        snprintf(og_image, sizeof(og_image), "%s", content);

      else if(strcasecmp(nameb, "twitter:image") == 0
          && twitter_image[0] == '\0')
        snprintf(twitter_image, sizeof(twitter_image), "%s", content);

      else if(strcasecmp(prop, "og:title") == 0 && og_title[0] == '\0')
        snprintf(og_title, sizeof(og_title), "%s", content);
    }

    p = tag_end + 1;
  }

  // Settle on the page-level caption for meta-image rows.
  meta_caption = og_title[0]   != '\0' ? og_title   :
      page_title[0] != '\0' ? page_title : "";

  // OpenGraph first (authoritative page-level image).
  if(og_image[0] != '\0')
  {
    char resolved[KNOWLEDGE_IMAGE_URL_SZ];

    if(acq_resolve_image_url(og_image, page_url, resolved, sizeof(resolved)))
      acq_image_accept(&cfg, resolved, meta_caption,
          0, 0, false, out, &n, runtime_cap);
  }

  // Twitter card second — deduped against OG.
  if(twitter_image[0] != '\0' && n < runtime_cap)
  {
    char resolved[KNOWLEDGE_IMAGE_URL_SZ];

    if(acq_resolve_image_url(twitter_image, page_url,
          resolved, sizeof(resolved)))
      acq_image_accept(&cfg, resolved, meta_caption,
          0, 0, false, out, &n, runtime_cap);
  }

  // In-body <img> tags — second pass.
  for(const char *p = html; p < end && n < runtime_cap; )
  {
    const char *tag_end;
    char src [KNOWLEDGE_IMAGE_URL_SZ];
    char wbuf[16];
    char alt [KNOWLEDGE_IMAGE_CAPTION_SZ];
    char hbuf[16];
    bool had_w;
    bool had_h;
    int  width_px;
    int  height_px;
    char resolved[KNOWLEDGE_IMAGE_URL_SZ];
    if(*p != '<')
    {
      p++;
      continue;
    }

    tag_end = acq_find_tag_end(p, end);

    if(tag_end == NULL)
      break;

    if(!acq_tag_starts(p, end, "<img"))
    {
      p = tag_end + 1;
      continue;
    }

    src[0] = '\0';
    alt[0] = '\0';

    if(!acq_extract_attr(p, tag_end, "src", src, sizeof(src))
        || src[0] == '\0')
    {
      p = tag_end + 1;
      continue;
    }

    acq_extract_attr(p, tag_end, "alt", alt, sizeof(alt));

    wbuf[0] = '\0';
    hbuf[0] = '\0';
    had_w = acq_extract_attr(p, tag_end, "width",  wbuf, sizeof(wbuf));
    had_h = acq_extract_attr(p, tag_end, "height", hbuf, sizeof(hbuf));
    width_px = acq_extract_attr_int(p, tag_end, "width");
    height_px = acq_extract_attr_int(p, tag_end, "height");


    if(!acq_resolve_image_url(src, page_url, resolved, sizeof(resolved)))
    {
      p = tag_end + 1;
      continue;
    }

    acq_image_accept(&cfg, resolved, alt, width_px, height_px,
        had_w && had_h, out, &n, runtime_cap);

    p = tag_end + 1;
  }

  return(n);
}
