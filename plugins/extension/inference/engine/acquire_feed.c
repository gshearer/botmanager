// botmanager — MIT
// Acquisition engine: personality-declared RSS/HTML feed dispatcher.

#include "acquire_priv.h"

#include "curl.h"

#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Network timing for feed fetches. Kept modest so a slow feed server
// can't starve the curl worker on popular personalities with many
// feeds attached.
#define ACQ_FEED_FETCH_TIMEOUT_SECS  20
#define ACQ_FEED_USER_AGENT          "botmanager-acquire/0.1 (+feed)"

// Body size cap on the stripped-HTML buffer handed to the digester
// for the HTML path. Matches the reactive pipeline's own cap.
#define ACQ_FEED_STRIPPED_MAX        ACQUIRE_REACTIVE_STRIPPED_MAX

// Per-fire context — heap-owned, single-use. Carries enough to drive
// the async chain (fetch → parse → per-item digest submit) without
// reacquiring acquire_entries_lock from a callback. Identifiers are
// snapshotted under rdlock at tick time.
typedef struct acq_feed_ctx
{
  // Identifiers (snapshot at dispatch; safe to read from any thread).
  char                 bot_name   [ACQUIRE_BOT_NAME_SZ];
  char                 topic_name [ACQUIRE_TOPIC_NAME_SZ];
  char                 dest_corpus[ACQUIRE_CORPUS_NAME_SZ];
  char                 keywords_csv[ACQUIRE_KEYWORD_SZ * ACQUIRE_KEYWORDS_MAX
                                   + ACQUIRE_KEYWORDS_MAX];

  // Feed config snapshot. `feed_idx` is the slot number in the bot's
  // flat feeds[] / feed_state[] arrays — used to update state under
  // wrlock on the callback thread.
  size_t               feed_idx;
  acquire_feed_kind_t  kind;
  char                 url[ACQUIRE_FEED_URL_SZ];

  // State snapshot (for conditional-GET headers).
  char                 etag         [ACQUIRE_FEED_ETAG_SZ];
  char                 last_modified[ACQUIRE_FEED_LMOD_SZ];
} acq_feed_ctx_t;

// Per-item emit context. Thin wrapper around an identifier bundle the
// digest callback needs to call the shared ingest seam.
typedef struct
{
  char  bot_name   [ACQUIRE_BOT_NAME_SZ];
  char  topic_name [ACQUIRE_TOPIC_NAME_SZ];
  char  subject    [ACQUIRE_SUBJECT_SZ];
  char  dest_corpus[ACQUIRE_CORPUS_NAME_SZ];
  char  page_url   [ACQUIRE_FEED_URL_SZ];
} acq_feed_emit_ctx_t;

// Forward decls.
static void acq_feed_fire(acq_feed_ctx_t *f);
static void acq_feed_fetch_done(const curl_response_t *resp);
static void acq_feed_rss_process(acq_feed_ctx_t *f,
    const char *body, size_t len);
static void acq_feed_html_process(acq_feed_ctx_t *f,
    const char *body, size_t len);
static void acq_feed_emit_item(const char *bot, const char *topic,
    const char *keywords_csv, const char *subject,
    const char *dest_corpus, const char *page_url,
    const char *body, size_t body_len);
static void acq_feed_digest_done(const acquire_digest_response_t *resp);

// FNV-1a 64-bit hash. Used for both body-level change detection and
// per-item guid dedup. Cheap and well-distributed for short strings.
static uint64_t
fnv1a64(const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint64_t       h = 0xcbf29ce484222325ULL;

  for(size_t i = 0; i < len; i++)
  {
    h ^= (uint64_t)p[i];
    h *= 0x100000001b3ULL;
  }

  return(h);
}

// Hex-encode a 64-bit hash into a lowercase 16-char + NUL buffer.
static void
fnv1a64_hex(uint64_t h, char out[ACQUIRE_FEED_HASH_SZ])
{
  static const char digits[] = "0123456789abcdef";

  for(int i = 0; i < 16; i++)
  {
    out[15 - i] = digits[h & 0xfu];
    h >>= 4;
  }

  out[16] = '\0';
}

// SSRF guard. Personality files are trusted, but a typo or a
// copy-pasted local URL should never reach curl. We:
//   1. Require http:// or https:// scheme (no file://, no gopher://).
//   2. Extract the host portion (the bit between "://" and "/?#" or EOL).
//   3. Reject a hand-curated list of loopback / private / link-local
//      / multicast / IPv6-local / IPv4-mapped prefixes.
//
// DNS resolution is intentionally skipped — that would be a race-
// condition trap anyway (rebind-after-check). A string-prefix check
// catches every literal private range mentioned in the plan.
static bool
acq_feed_ssrf_ok(const char *url)
{
  const char *host_start;
  size_t      host_len;
  const char *p;
  char        host[256];
  size_t      i;

  if(url == NULL || url[0] == '\0')
    return(false);

  if(strncasecmp(url, "http://",  7) == 0)
    host_start = url + 7;

  else if(strncasecmp(url, "https://", 8) == 0)
    host_start = url + 8;

  else
    return(false);

  // Find the end of the host portion.
  p = host_start;

  while(*p != '\0' && *p != '/' && *p != '?' && *p != '#'
      && *p != ':')
    p++;

  host_len = (size_t)(p - host_start);

  if(host_len == 0 || host_len >= sizeof(host))
    return(false);

  // Bounded copy so we can lowercase in place.
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  for(i = 0; i < host_len; i++)
    if(host[i] >= 'A' && host[i] <= 'Z')
      host[i] = (char)(host[i] + ('a' - 'A'));

  // Exact-match rejects.
  if(strcmp(host, "localhost") == 0)
    return(false);

  // Prefix rejects. These are literal IPv4 / IPv6 prefixes from the
  // plan; any match drops the fetch.
  if(strncmp(host, "127.",      4) == 0) return(false);
  if(strncmp(host, "10.",       3) == 0) return(false);
  if(strncmp(host, "192.168.",  8) == 0) return(false);
  if(strncmp(host, "169.254.",  8) == 0) return(false);
  if(strncmp(host, "0.",        2) == 0) return(false);
  if(strncmp(host, "224.",      4) == 0) return(false);
  if(strncmp(host, "225.",      4) == 0) return(false);
  if(strncmp(host, "226.",      4) == 0) return(false);
  if(strncmp(host, "227.",      4) == 0) return(false);
  if(strncmp(host, "228.",      4) == 0) return(false);
  if(strncmp(host, "229.",      4) == 0) return(false);
  if(strncmp(host, "230.",      4) == 0) return(false);
  if(strncmp(host, "231.",      4) == 0) return(false);
  if(strncmp(host, "232.",      4) == 0) return(false);
  if(strncmp(host, "233.",      4) == 0) return(false);
  if(strncmp(host, "234.",      4) == 0) return(false);
  if(strncmp(host, "235.",      4) == 0) return(false);
  if(strncmp(host, "236.",      4) == 0) return(false);
  if(strncmp(host, "237.",      4) == 0) return(false);
  if(strncmp(host, "238.",      4) == 0) return(false);
  if(strncmp(host, "239.",      4) == 0) return(false);

  // 172.16.0.0/12 — 172.16.* through 172.31.*.
  if(strncmp(host, "172.", 4) == 0)
  {
    int o2 = atoi(host + 4);

    if(o2 >= 16 && o2 <= 31)
      return(false);
  }

  // IPv6 literals (libcurl accepts [fe80::1] syntax; also bare literal
  // forms for safety).
  if(strcmp(host, "::1") == 0)          return(false);
  if(strncmp(host, "[::1",     4) == 0) return(false);
  if(strncmp(host, "fe80::",   6) == 0) return(false);
  if(strncmp(host, "[fe80",    5) == 0) return(false);
  if(strncmp(host, "fc00::",   6) == 0) return(false);
  if(strncmp(host, "[fc00",    5) == 0) return(false);
  if(strncmp(host, "fd00::",   6) == 0) return(false);
  if(strncmp(host, "[fd00",    5) == 0) return(false);
  if(strncmp(host, "::ffff:",  7) == 0) return(false);
  if(strncmp(host, "[::ffff:", 8) == 0) return(false);

  return(true);
}

// Per-tick feed dispatcher. Picks at most one due slot per bot per
// tick, then kicks the async fetch. Cheap — the walk holds rdlock for
// the duration of the scan; the fire itself runs lock-free.
void
acq_feeds_tick(acquire_bot_entry_t *e)
{
  bool     enabled;
  bool     sources_on;
  uint32_t max_per_hour;
  size_t   n;
  size_t   start;
  time_t   now;
  acq_feed_ctx_t *fctx;

  if(!acquire_ready || e == NULL)
    return;

  pthread_mutex_lock(&acquire_cfg_mutex);
  enabled      = acquire_cfg.enabled;
  sources_on   = acquire_cfg.sources_enabled;
  max_per_hour = acquire_cfg.max_reactive_per_hour;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(!enabled || !sources_on)
    return;

  fctx = NULL;
  now  = time(NULL);

  pthread_rwlock_rdlock(&acquire_entries_lock);

  n = e->n_feeds_total;

  if(!e->active || n == 0 || e->feeds == NULL
      || e->feed_state == NULL || e->feed_topic_idx == NULL)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    return;
  }

  start = e->feed_next_idx;

  if(start >= n)
    start = 0;

  // One full pass around the ring; stop on the first due slot.
  for(size_t step = 0; step < n; step++)
  {
    size_t                      idx = (start + step) % n;
    const acquire_feed_t       *f  = &e->feeds[idx];
    const acquire_feed_state_t *st = &e->feed_state[idx];
    const acquire_topic_t      *t;
    time_t                      due;

    due = st->last_fetched + (time_t)f->cadence_secs;

    if(st->last_fetched != 0 && due > now)
      continue;

    // Respect the shared per-(bot, topic) hourly rate limiter. The
    // limiter lives on the tick thread's private slot array, so the
    // rdlock covers it fine.
    t = &e->topics[e->feed_topic_idx[idx]];

    if(acquire_rate_exceeded(e, t->name, max_per_hour))
      continue;

    // Build the dispatch ctx under the lock. One heap alloc + a
    // string-copy burst — still fine under rdlock.
    fctx = mem_alloc(ACQUIRE_CTX, "feed_ctx", sizeof(*fctx));

    snprintf(fctx->bot_name,    sizeof(fctx->bot_name),    "%s", e->name);
    snprintf(fctx->topic_name,  sizeof(fctx->topic_name),  "%s", t->name);
    snprintf(fctx->dest_corpus, sizeof(fctx->dest_corpus), "%s",
        e->dest_corpus);
    snprintf(fctx->url,         sizeof(fctx->url),         "%s", f->url);
    fctx->kind     = f->kind;
    fctx->feed_idx = idx;

    // Build keywords CSV inline — same shape as the reactive path's
    // acq_build_keywords_csv. Duplicated here to avoid pulling another
    // static seam out of acquire_reactive.c for a six-line helper.
    {
      size_t w = 0;

      fctx->keywords_csv[0] = '\0';

      for(size_t k = 0; k < t->n_keywords; k++)
      {
        const char *kw = t->keywords[k];

        int written;

        if(kw[0] == '\0')
          continue;

        written = snprintf(fctx->keywords_csv + w,
            sizeof(fctx->keywords_csv) - w,
            "%s%s", (w == 0) ? "" : ", ", kw);

        if(written < 0
            || (size_t)written >= sizeof(fctx->keywords_csv) - w)
          break;

        w += (size_t)written;
      }
    }

    snprintf(fctx->etag,          sizeof(fctx->etag),          "%s",
        st->etag);
    snprintf(fctx->last_modified, sizeof(fctx->last_modified), "%s",
        st->last_modified);

    // One due slot per tick is the budget. Record the index so we can
    // advance the cursor after releasing the rdlock (we only ever write
    // `feed_next_idx` from the tick thread, but the rwlock API forbids
    // mutation under rdlock — honour it cleanly).
    break;
  }

  pthread_rwlock_unlock(&acquire_entries_lock);

  if(fctx != NULL)
  {
    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *live = acquire_entry_find_locked(fctx->bot_name);

      if(live != NULL && live->n_feeds_total > 0)
        live->feed_next_idx = (fctx->feed_idx + 1) % live->n_feeds_total;
    }
    pthread_rwlock_unlock(&acquire_entries_lock);

    acq_feed_fire(fctx);
  }
}

// Kick off one feed fetch. Performs the SSRF check, builds a curl
// request with optional conditional-GET headers, and submits it. On
// any rejection the feed state's last_fetched is advanced so we don't
// spin on the bad URL every tick.
static void
acq_feed_fire(acq_feed_ctx_t *f)
{
  curl_request_t *req;
  char            hdr[ACQUIRE_FEED_ETAG_SZ + 32];

  if(!acq_feed_ssrf_ok(f->url))
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed SSRF reject url='%s' bot=%s topic=%s",
        f->url, f->bot_name, f->topic_name);

    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_feed_errors++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    // Advance last_fetched so the bad URL doesn't spam each tick.
    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

      if(e != NULL && e->feed_state != NULL
          && f->feed_idx < e->n_feeds_total)
      {
        e->feed_state[f->feed_idx].last_fetched = time(NULL);
        e->feed_state[f->feed_idx].n_errors++;
      }
    }
    pthread_rwlock_unlock(&acquire_entries_lock);

    mem_free(f);
    return;
  }

  req = curl_request_create(CURL_METHOD_GET, f->url,
      acq_feed_fetch_done, f);

  if(req == NULL)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed: curl_request_create failed url='%s'", f->url);
    mem_free(f);
    return;
  }

  (void)curl_request_set_user_agent(req, ACQ_FEED_USER_AGENT);
  (void)curl_request_set_timeout(req, ACQ_FEED_FETCH_TIMEOUT_SECS);
  (void)curl_request_set_prio(req, CURL_PRIO_BULK);

  if(f->etag[0] != '\0')
  {
    snprintf(hdr, sizeof(hdr), "If-None-Match: %s", f->etag);
    (void)curl_request_add_header(req, hdr);
  }

  if(f->last_modified[0] != '\0')
  {
    snprintf(hdr, sizeof(hdr), "If-Modified-Since: %s", f->last_modified);
    (void)curl_request_add_header(req, hdr);
  }

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "feed fetch bot=%s topic=%s kind=%s url='%s'%s%s",
      f->bot_name, f->topic_name,
      f->kind == ACQUIRE_FEED_RSS ? "rss" : "html",
      f->url,
      f->etag[0] ? " (If-None-Match)" : "",
      f->last_modified[0] ? " (If-Modified-Since)" : "");

  if(curl_request_submit(req) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed: curl_request_submit failed url='%s'", f->url);
    mem_free(f);
    return;
  }

  // req is now owned by the curl subsystem; our ctx is reachable via
  // the callback's user_data pointer.
}

// Completion callback for the feed fetch. Updates feed state under
// wrlock, then dispatches to the RSS or HTML processor on the happy
// path.
static void
acq_feed_fetch_done(const curl_response_t *resp)
{
  acq_feed_ctx_t *f = (acq_feed_ctx_t *)resp->user_data;

  uint64_t body_h;
  char     body_hex[ACQUIRE_FEED_HASH_SZ];
  bool     body_changed;
  long     status;

  // libcurl reports transport errors with curl_code != 0; HTTP status
  // codes from the server come in via `status`.
  status = resp->status;

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_feed_fetches++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  if(resp->curl_code != 0 || status == 0)
  {
    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_feed_errors++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

      if(e != NULL && e->feed_state != NULL
          && f->feed_idx < e->n_feeds_total)
      {
        e->feed_state[f->feed_idx].last_fetched = time(NULL);
        e->feed_state[f->feed_idx].n_errors++;
        e->feed_state[f->feed_idx].n_fetches++;
      }
    }
    pthread_rwlock_unlock(&acquire_entries_lock);

    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed fetch error url='%s' err='%s'",
        f->url, resp->error != NULL ? resp->error : "(transport)");
    mem_free(f);
    return;
  }

  // 304 Not Modified — conditional GET hit. Update state, no parse.
  if(status == 304)
  {
    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_feed_304++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

      if(e != NULL && e->feed_state != NULL
          && f->feed_idx < e->n_feeds_total)
      {
        e->feed_state[f->feed_idx].last_fetched = time(NULL);
        e->feed_state[f->feed_idx].n_fetches++;
        e->feed_state[f->feed_idx].n_304++;
      }
    }
    pthread_rwlock_unlock(&acquire_entries_lock);

    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "feed 304 url='%s' bot=%s topic=%s",
        f->url, f->bot_name, f->topic_name);
    mem_free(f);
    return;
  }

  if(status < 200 || status >= 300)
  {
    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_feed_errors++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

      if(e != NULL && e->feed_state != NULL
          && f->feed_idx < e->n_feeds_total)
      {
        e->feed_state[f->feed_idx].last_fetched = time(NULL);
        e->feed_state[f->feed_idx].n_fetches++;
        e->feed_state[f->feed_idx].n_errors++;
      }
    }
    pthread_rwlock_unlock(&acquire_entries_lock);

    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed HTTP %ld url='%s'", status, f->url);
    mem_free(f);
    return;
  }

  // 2xx with a body. Hash it; compare with stored hash to short-
  // circuit when nothing changed.
  if(resp->body == NULL || resp->body_len == 0)
  {
    // Treat empty 2xx body like a fetch we can't use.
    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

      if(e != NULL && e->feed_state != NULL
          && f->feed_idx < e->n_feeds_total)
      {
        e->feed_state[f->feed_idx].last_fetched = time(NULL);
        e->feed_state[f->feed_idx].n_fetches++;
      }
    }
    pthread_rwlock_unlock(&acquire_entries_lock);

    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "feed empty body url='%s'", f->url);
    mem_free(f);
    return;
  }

  body_h = fnv1a64(resp->body, resp->body_len);
  fnv1a64_hex(body_h, body_hex);

  body_changed = true;

  pthread_rwlock_wrlock(&acquire_entries_lock);
  {
    acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

    if(e != NULL && e->feed_state != NULL
        && f->feed_idx < e->n_feeds_total)
    {
      acquire_feed_state_t *st = &e->feed_state[f->feed_idx];

      st->last_fetched = time(NULL);
      st->n_fetches++;

      if(st->body_hash[0] != '\0'
          && strcmp(st->body_hash, body_hex) == 0)
        body_changed = false;

      else
      {
        memcpy(st->body_hash, body_hex, ACQUIRE_FEED_HASH_SZ);

        if(resp->etag != NULL && resp->etag[0] != '\0')
          snprintf(st->etag, sizeof(st->etag), "%s", resp->etag);

        if(resp->last_modified != NULL && resp->last_modified[0] != '\0')
          snprintf(st->last_modified, sizeof(st->last_modified),
              "%s", resp->last_modified);
      }
    }
  }
  pthread_rwlock_unlock(&acquire_entries_lock);

  if(!body_changed)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "feed unchanged-body url='%s'", f->url);
    mem_free(f);
    return;
  }

  if(f->kind == ACQUIRE_FEED_RSS)
    acq_feed_rss_process(f, resp->body, resp->body_len);

  else
    acq_feed_html_process(f, resp->body, resp->body_len);

  mem_free(f);
}

// Concatenate the text content of an xmlNode subtree into `out`.
// Returns bytes written (excl. NUL). libxml2's xmlNodeGetContent
// produces the same result but heap-allocates; this keeps the parser
// cheap for large feeds.
static size_t
acq_feed_xml_text(xmlNodePtr node, char *out, size_t out_cap)
{
  xmlChar *text;
  size_t   n;

  if(node == NULL || out_cap == 0)
  {
    if(out_cap > 0) out[0] = '\0';
    return(0);
  }

  text = xmlNodeGetContent(node);

  if(text == NULL)
  {
    out[0] = '\0';
    return(0);
  }

  n = strlen((const char *)text);

  if(n >= out_cap) n = out_cap - 1;

  memcpy(out, text, n);
  out[n] = '\0';

  xmlFree(text);
  return(n);
}

// Find the first direct child of `parent` whose name matches `name`
// (case-sensitive — XML names are case-sensitive, including the Atom
// vs RSS split we dispatch on). Returns NULL on miss.
static xmlNodePtr
acq_feed_xml_find_child(xmlNodePtr parent, const char *name)
{
  if(parent == NULL)
    return(NULL);

  for(xmlNodePtr c = parent->children; c != NULL; c = c->next)
    if(c->type == XML_ELEMENT_NODE
        && c->name != NULL
        && strcmp((const char *)c->name, name) == 0)
      return(c);

  return(NULL);
}

// Extract the <link> value from an Atom or RSS item. RSS uses a text
// child; Atom uses <link href="...">. Returns true on a non-empty hit.
static bool
acq_feed_xml_link(xmlNodePtr item, bool is_atom,
    char *out, size_t out_cap)
{
  xmlNodePtr link;

  if(out_cap == 0)
    return(false);

  out[0] = '\0';
  link = acq_feed_xml_find_child(item, "link");

  if(link == NULL)
    return(false);

  if(is_atom)
  {
    xmlChar *href = xmlGetProp(link, (const xmlChar *)"href");

    if(href == NULL)
      return(false);

    snprintf(out, out_cap, "%s", (const char *)href);
    xmlFree(href);
    return(out[0] != '\0');
  }

  (void)acq_feed_xml_text(link, out, out_cap);
  return(out[0] != '\0');
}

// Dispatch one RSS / Atom item: extract title + body, check the
// per-feed dedup ring, hand off to the digester for novel items.
// is_atom selects the element-name set (Atom vs RSS 2.0).
static void
acq_feed_process_item(acq_feed_ctx_t *f, xmlNodePtr item, bool is_atom,
    uint32_t *out_n_new)
{
  char  title  [ACQUIRE_SUBJECT_SZ];
  char  link   [ACQUIRE_FEED_URL_SZ];
  char  guid_buf[ACQUIRE_FEED_URL_SZ + ACQUIRE_SUBJECT_SZ + 8];
  char  desc_raw[4096];
  char  desc_stripped[4096];
  char  body   [ACQUIRE_SUBJECT_SZ + sizeof(desc_stripped) + 8];
  size_t desc_len;
  size_t body_len;
  uint64_t guid_h;
  bool   seen;
  xmlNodePtr  title_n;
  xmlNodePtr  desc_n;
  xmlNodePtr  guid_n;

  title[0] = '\0';
  link [0] = '\0';
  guid_buf[0] = '\0';
  desc_raw[0] = '\0';

  title_n = acq_feed_xml_find_child(item, "title");

  if(title_n != NULL)
    (void)acq_feed_xml_text(title_n, title, sizeof(title));

  (void)acq_feed_xml_link(item, is_atom, link, sizeof(link));

  // Description / summary / content. RSS uses <description>;
  // Atom prefers <content>, falling back to <summary>.
  if(is_atom)
  {
    desc_n = acq_feed_xml_find_child(item, "content");

    if(desc_n == NULL)
      desc_n = acq_feed_xml_find_child(item, "summary");
  }

  else
  {
    desc_n = acq_feed_xml_find_child(item, "description");
  }

  if(desc_n != NULL)
    (void)acq_feed_xml_text(desc_n, desc_raw, sizeof(desc_raw));

  desc_len = strlen(desc_raw);

  // Strip HTML/CDATA noise out of the description. acq_strip_html
  // tolerates already-plain text fine; this is belt-and-braces for
  // feeds whose description is HTML-encoded.
  if(desc_len > 0)
    desc_len = acq_strip_html(desc_raw, desc_len,
        desc_stripped, sizeof(desc_stripped));

  else
    desc_stripped[0] = '\0';

  // Guid dedup. RSS uses <guid> (can be any unique string); Atom uses
  // <id>. Fall back to a hash of link+title when neither is present.
  guid_n = is_atom
      ? acq_feed_xml_find_child(item, "id")
      : acq_feed_xml_find_child(item, "guid");

  if(guid_n != NULL)
    (void)acq_feed_xml_text(guid_n, guid_buf, sizeof(guid_buf));

  if(guid_buf[0] == '\0')
    snprintf(guid_buf, sizeof(guid_buf), "%s|%s", link, title);

  guid_h = fnv1a64(guid_buf, strlen(guid_buf));

  seen = false;

  pthread_rwlock_rdlock(&acquire_entries_lock);
  {
    acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

    if(e != NULL && e->feed_state != NULL
        && f->feed_idx < e->n_feeds_total)
    {
      const acquire_feed_state_t *st = &e->feed_state[f->feed_idx];
      uint32_t fill = st->guid_fill;

      for(uint32_t i = 0; i < fill; i++)
      {
        if(st->seen_guids[i] == guid_h)
        {
          seen = true;
          break;
        }
      }
    }
  }
  pthread_rwlock_unlock(&acquire_entries_lock);

  if(seen)
    return;

  // Push the guid hash onto the ring before fanning out. Doing this
  // unconditionally (even when the emit below fails) keeps a flapping
  // feed item from re-triggering the digester on the next fetch.
  pthread_rwlock_wrlock(&acquire_entries_lock);
  {
    acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

    if(e != NULL && e->feed_state != NULL
        && f->feed_idx < e->n_feeds_total)
    {
      acquire_feed_state_t *st = &e->feed_state[f->feed_idx];
      uint32_t slot = st->guid_write % ACQUIRE_FEED_GUID_RING;

      st->seen_guids[slot] = guid_h;
      st->guid_write++;

      if(st->guid_fill < ACQUIRE_FEED_GUID_RING)
        st->guid_fill++;
    }
  }
  pthread_rwlock_unlock(&acquire_entries_lock);

  // Build body = title + "\n\n" + stripped-description. Empty title
  // still hands off; the digester's relevance pass is the gate.
  body_len = 0;

  if(title[0] != '\0')
    body_len = (size_t)snprintf(body, sizeof(body), "%s\n\n", title);

  if(desc_len > 0)
  {
    size_t room = sizeof(body) - body_len;

    if(room > 1)
    {
      if(desc_len >= room) desc_len = room - 1;

      memcpy(body + body_len, desc_stripped, desc_len);
      body_len += desc_len;
      body[body_len] = '\0';
    }
  }

  if(body_len == 0)
    return;  // nothing useful

  acq_feed_emit_item(f->bot_name, f->topic_name, f->keywords_csv,
      title[0] ? title : f->topic_name,
      f->dest_corpus, link[0] ? link : f->url,
      body, body_len);

  (*out_n_new)++;
}

static void
acq_feed_rss_process(acq_feed_ctx_t *f, const char *body, size_t len)
{
  xmlDocPtr     doc;
  xmlNodePtr    root;
  xmlNodePtr    channel;
  bool          is_atom;
  uint32_t      n_new    = 0;
  uint32_t      n_items  = 0;
  const char   *item_tag;

  doc = xmlReadMemory(body, (int)len, f->url, NULL,
      XML_PARSE_NOENT | XML_PARSE_NONET
      | XML_PARSE_NOWARNING | XML_PARSE_NOERROR);

  if(doc == NULL)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed RSS parse failed url='%s'", f->url);
    return;
  }

  root = xmlDocGetRootElement(doc);

  if(root == NULL || root->name == NULL)
  {
    xmlFreeDoc(doc);
    return;
  }

  if(strcmp((const char *)root->name, "feed") == 0)
  {
    is_atom  = true;
    channel  = root;      // Atom has items as direct children
    item_tag = "entry";
  }

  else if(strcmp((const char *)root->name, "rss") == 0)
  {
    is_atom  = false;
    channel  = acq_feed_xml_find_child(root, "channel");
    item_tag = "item";

    if(channel == NULL)
    {
      xmlFreeDoc(doc);
      clam(CLAM_WARN, ACQUIRE_CTX,
          "feed RSS: no <channel> url='%s'", f->url);
      return;
    }
  }

  else
  {
    xmlFreeDoc(doc);
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed RSS: unknown root <%s> url='%s'",
        (const char *)root->name, f->url);
    return;
  }

  for(xmlNodePtr c = channel->children; c != NULL; c = c->next)
  {
    if(c->type != XML_ELEMENT_NODE || c->name == NULL)
      continue;

    if(strcmp((const char *)c->name, item_tag) != 0)
      continue;

    n_items++;
    acq_feed_process_item(f, c, is_atom, &n_new);
  }

  xmlFreeDoc(doc);

  if(n_new > 0)
  {
    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_feed_new_items += n_new;
    pthread_mutex_unlock(&acquire_stat_mutex);

    pthread_rwlock_wrlock(&acquire_entries_lock);
    {
      acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

      if(e != NULL && e->feed_state != NULL
          && f->feed_idx < e->n_feeds_total)
        e->feed_state[f->feed_idx].n_new_items += n_new;
    }
    pthread_rwlock_unlock(&acquire_entries_lock);
  }

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "feed RSS url='%s' items=%u kept=%u",
      f->url, n_items, n_new);
}

static void
acq_feed_html_process(acq_feed_ctx_t *f, const char *body, size_t len)
{
  htmlDocPtr   doc;
  xmlXPathContextPtr xctx;
  xmlXPathObjectPtr  xobj;
  xmlNodePtr          node;
  char         title  [ACQUIRE_SUBJECT_SZ];
  char        *stripped;
  size_t       strip_len;

  doc = htmlReadMemory(body, (int)len, f->url, NULL,
      HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING
      | HTML_PARSE_NONET  | HTML_PARSE_RECOVER);

  if(doc == NULL)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed HTML parse failed url='%s'", f->url);
    return;
  }

  stripped = mem_alloc(ACQUIRE_CTX, "feed_strip", ACQ_FEED_STRIPPED_MAX);

  // Try to find a semantic content container first: <article> or
  // <main>. If either is present, strip just that subtree; otherwise
  // fall back to stripping the full body.
  xctx = xmlXPathNewContext(doc);
  xobj = NULL;
  node = NULL;

  if(xctx != NULL)
  {
    xobj = xmlXPathEvalExpression(
        (const xmlChar *)"//article | //main", xctx);

    if(xobj != NULL && xobj->nodesetval != NULL
        && xobj->nodesetval->nodeNr > 0)
      node = xobj->nodesetval->nodeTab[0];
  }

  if(node != NULL)
  {
    xmlBufferPtr buf = xmlBufferCreate();

    if(buf != NULL)
    {
      int n = htmlNodeDump(buf, doc, node);

      if(n > 0)
        strip_len = acq_strip_html((const char *)xmlBufferContent(buf),
            (size_t)xmlBufferLength(buf),
            stripped, ACQ_FEED_STRIPPED_MAX);

      else
        strip_len = acq_strip_html(body, len,
            stripped, ACQ_FEED_STRIPPED_MAX);

      xmlBufferFree(buf);
    }

    else
      strip_len = acq_strip_html(body, len,
          stripped, ACQ_FEED_STRIPPED_MAX);
  }

  else
    strip_len = acq_strip_html(body, len,
        stripped, ACQ_FEED_STRIPPED_MAX);

  if(xobj != NULL) xmlXPathFreeObject(xobj);
  if(xctx != NULL) xmlXPathFreeContext(xctx);

  // Subject: <title> when present, else topic name.
  title[0] = '\0';
  (void)acq_find_title(body, len, title, sizeof(title));

  if(strip_len == 0)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "feed HTML empty after strip url='%s'", f->url);
    mem_free(stripped);
    xmlFreeDoc(doc);
    return;
  }

  acq_feed_emit_item(f->bot_name, f->topic_name, f->keywords_csv,
      title[0] ? title : f->topic_name,
      f->dest_corpus, f->url,
      stripped, strip_len);

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_feed_new_items++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  pthread_rwlock_wrlock(&acquire_entries_lock);
  {
    acquire_bot_entry_t *e = acquire_entry_find_locked(f->bot_name);

    if(e != NULL && e->feed_state != NULL
        && f->feed_idx < e->n_feeds_total)
      e->feed_state[f->feed_idx].n_new_items++;
  }
  pthread_rwlock_unlock(&acquire_entries_lock);

  clam(CLAM_DEBUG, ACQUIRE_CTX,
      "feed HTML url='%s' title='%s' stripped_bytes=%zu",
      f->url, title[0] ? title : "(none)", strip_len);

  mem_free(stripped);
  xmlFreeDoc(doc);
}

// Ship one item to the digester; the digest callback will route it
// into the shared ingest seam.
static void
acq_feed_emit_item(const char *bot, const char *topic,
    const char *keywords_csv, const char *subject,
    const char *dest_corpus, const char *page_url,
    const char *body, size_t body_len)
{
  acq_feed_emit_ctx_t *emit;

  emit = mem_alloc(ACQUIRE_CTX, "feed_emit", sizeof(*emit));

  snprintf(emit->bot_name,    sizeof(emit->bot_name),    "%s", bot);
  snprintf(emit->topic_name,  sizeof(emit->topic_name),  "%s", topic);
  snprintf(emit->subject,     sizeof(emit->subject),     "%s", subject);
  snprintf(emit->dest_corpus, sizeof(emit->dest_corpus), "%s", dest_corpus);
  snprintf(emit->page_url,    sizeof(emit->page_url),    "%s", page_url);

  if(acquire_digest_submit(topic, keywords_csv, body, body_len,
      acq_feed_digest_done, emit) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed digest submit failed bot=%s topic=%s",
        bot, topic);
    mem_free(emit);
    return;
  }
}

static void
acq_feed_digest_done(const acquire_digest_response_t *resp)
{
  acq_feed_emit_ctx_t *emit = (acq_feed_emit_ctx_t *)resp->user_data;

  uint32_t threshold;

  if(!resp->ok || resp->summary == NULL || resp->summary[0] == '\0')
  {
    mem_free(emit);
    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  threshold = acquire_cfg.relevance_threshold;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(resp->relevance < threshold)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "feed digest below threshold bot=%s topic=%s"
        " relevance=%u threshold=%u",
        emit->bot_name, emit->topic_name, resp->relevance, threshold);
    mem_free(emit);
    return;
  }

  if(emit->dest_corpus[0] == '\0')
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "feed: no dest_corpus for bot=%s; dropping ingest",
        emit->bot_name);
    mem_free(emit);
    return;
  }

  // is_proactive = true: feed fetches are scheduled, not user-driven.
  // Consumers downstream classify them alongside proactive SXNG
  // results.
  (void)acq_ingest_digest_result(emit->bot_name, emit->topic_name,
      emit->subject, emit->dest_corpus, true, resp,
      NULL, 0, emit->page_url);

  mem_free(emit);
}
