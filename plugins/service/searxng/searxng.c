// botmanager — MIT
// SearXNG service plugin: web-search queries against a SearXNG instance.
// Pure mechanism — no command surface. Consumers call sxng_search via
// plugin_dlsym (see searxng_api.h). Supports SearXNG's general, images,
// news, videos, and music category buckets; the per-category extras
// union on sxng_result_t is populated from engine-dependent JSON fields.
#define SEARXNG_INTERNAL
#include "searxng.h"

#include "alloc.h"
#include "clam.h"
#include "curl.h"
#include "json.h"
#include "kv.h"
#include "plugin.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Constants

#define SXNG_CTX             "searxng"

#define SXNG_URL_SZ          4096
#define SXNG_QUERY_SZ        1024
#define SXNG_HARDMAX_RESULTS 32  // upper clamp regardless of KV

// KV schema

static const plugin_kv_entry_t sxng_kv_schema[] = {
  { "plugin.searxng.endpoint",     KV_STR,    "http://localhost:8080/search",
    "SearXNG search URL (full path incl. /search)" },
  { "plugin.searxng.format",       KV_STR,    "json",
    "SearXNG response format (only 'json' is parsed)" },
  { "plugin.searxng.timeout_secs", KV_UINT32, "10",
    "Per-request timeout in seconds" },
  { "plugin.searxng.max_results",  KV_UINT32, "10",
    "Default result cap per query (hard max 32)" },
  { "plugin.searxng.min_results",  KV_UINT32, "1",
    "Floor on results returned per query (clamped to max_results)" },
  { "plugin.searxng.safesearch",   KV_UINT32, "1",
    "SafeSearch level: 0=off, 1=moderate, 2=strict" },
  { "plugin.searxng.news_query",   KV_STR,    "headlines",
    "Query used by a bare !news (headline mode)" },
  { "plugin.searxng.news_headlines", KV_UINT32, "5",
    "Headlines returned by a bare !news (clamped to max_results)" },
};

static size_t
sxng_urlencode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;

  if(cap == 0)
    return(0);

  for(const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++)
  {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                      || (c >= '0' && c <= '9')
                      || c == '-' || c == '_' || c == '.' || c == '~';

    if(unreserved)
    {
      if(n + 1 < cap)
        out[n] = (char)c;

      n++;
      continue;
    }

    if(n + 3 < cap)
    {
      out[n]     = '%';
      out[n + 1] = hex[c >> 4];
      out[n + 2] = hex[c & 0x0f];
    }

    n += 3;
  }

  out[n < cap ? n : cap - 1] = '\0';
  return(n);
}

// ----------------------------------------------------------------------
// The in-flight registry — every query borrows a foreign callback
// ----------------------------------------------------------------------

// sxng_search stores its caller's completion in a heap context of ours
// and hands core's curl layer sxng_curl_done instead. That is one
// indirection more than core can see: plugin_quiesce and plugin_audit
// both range-test curl_iter_req_t.cb, which for our transfers names THIS
// mapping, never the caller's. Both callers live elsewhere — the
// `searxng_cmd` plugin behind !search/!image/!news/!video/!music, and
// the inference engine's reactive acquisition — and a search is one of
// the longest requests this daemon makes (a SearXNG instance fans out to
// a dozen upstream engines and the timeout defaults to 10 s). Reload
// either caller inside that window and the stored pointer aims into
// freed .text.
//
// So every live query is filed here, and plugin_unmap_notify_register
// tells us when a mapping is about to go away in time to null the
// pointers that name it. A query whose caller left still completes
// normally; it simply delivers to nobody. The mechanics are commented in
// full in reachyapi.c and written up in PLUGIN.md.
//
// The caller's `user_data` is dropped with the callback and whatever it
// points at is leaked. Nothing else is possible: only the caller knows
// how to free its own context, and the caller is precisely what is no
// longer there. A bounded leak on an operator action beats a SIGSEGV.
static pthread_mutex_t sxng_active_mutex = PTHREAD_MUTEX_INITIALIZER;
static sxng_req_t     *sxng_active_head  = NULL;
static uint32_t        sxng_active_count = 0;

static void
sxng_req_track(sxng_req_t *r)
{
  pthread_mutex_lock(&sxng_active_mutex);

  r->next_active   = sxng_active_head;
  sxng_active_head = r;
  sxng_active_count++;

  pthread_mutex_unlock(&sxng_active_mutex);
}

// Unlink `r`, copy it to `out` and free it. The copy is taken under the
// lock so a completion reads the caller's callback in the same critical
// section the unmap sweep would null it in — read it afterwards and the
// two interleave, which is the whole bug.
static void
sxng_req_retire(sxng_req_t *r, sxng_req_t *out)
{
  sxng_req_t **pp;

  pthread_mutex_lock(&sxng_active_mutex);

  for(pp = &sxng_active_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != r)
      continue;

    *pp = r->next_active;
    sxng_active_count--;
    break;
  }

  *out = *r;

  pthread_mutex_unlock(&sxng_active_mutex);

  out->next_active = NULL;
  mem_free(r);
}

// A mapping is going away (core is between the plugin's deinit() and its
// residual audit, so nothing of it runs any more). Drop every callback
// that lives inside it.
//
// Residual race: a completion that has already retired its query holds
// the callback on its stack and is a few instructions from calling it.
// The window is bounded above by the quiescence poll plus the audit that
// follow this broadcast, and below by two stores — against an
// operator-timescale unload. Closing it would need core to wait on a
// lock a curl worker holds.
static void
sxng_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&sxng_active_mutex);

  for(sxng_req_t *r = sxng_active_head; r != NULL; r = r->next_active)
  {
    uintptr_t cb = (uintptr_t)fn_addr(&r->cb);

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    r->cb        = NULL;
    r->user_data = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&sxng_active_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, SXNG_CTX, "%u search(es) lost their caller to an "
        "unload; they will complete and deliver nothing", orphaned);
}

// ----------------------------------------------------------------------
// Delivery
// ----------------------------------------------------------------------

// Deliver a failure response from a RETIRED query — a stack copy the
// completion owns, already unlinked, whose callback is either the
// caller's or NULL because the caller was unloaded mid-flight.
static void
sxng_deliver_fail(const sxng_req_t *r, const char *msg)
{
  sxng_response_t resp = {
    .ok        = false,
    .error     = msg,
    .category  = r->category,
    .results   = NULL,
    .n_results = 0,
    .user_data = r->user_data,
  };

  if(r->cb != NULL)
    r->cb(&resp);
}

// Populate the per-category extras union on a freshly-extracted result
// row. The common-field extractor (sxng_result_spec) has already filled
// title / url / snippet / engine / score; this fills whatever optional
// fields the engine returned for the requested category, leaving
// missing fields zero. Engine quirks: some return `thumbnail_src`,
// others `thumbnail`; video `length` is sometimes a numeric seconds
// value and sometimes a "MM:SS" string. Both are handled.
static void
sxng_extract_extras(struct json_object *item, sxng_result_t *r,
    sxng_category_t cat)
{
  switch(cat)
  {
    case SXNG_CAT_IMAGES:
    {
      int w;
      int h;
      json_get_str(item, "img_src", r->extras.image.src,
          sizeof(r->extras.image.src));

      if(!json_get_str(item, "thumbnail_src", r->extras.image.thumbnail,
          sizeof(r->extras.image.thumbnail)))
        json_get_str(item, "thumbnail", r->extras.image.thumbnail,
            sizeof(r->extras.image.thumbnail));

      json_get_str(item, "resolution", r->extras.image.resolution,
          sizeof(r->extras.image.resolution));

      // Resolution comes as "WxH"; split into numeric fields when
      // present so consumers can min-dim filter without re-parsing.
      w = 0;
      h = 0;

      if(r->extras.image.resolution[0] != '\0'
          && sscanf(r->extras.image.resolution, "%dx%d", &w, &h) == 2)
      {
        r->extras.image.width_px  = (uint32_t)(w > 0 ? w : 0);
        r->extras.image.height_px = (uint32_t)(h > 0 ? h : 0);
      }

      break;
    }

    case SXNG_CAT_NEWS:
      json_get_str(item, "publishedDate", r->extras.news.published,
          sizeof(r->extras.news.published));
      json_get_str(item, "img_src", r->extras.news.img_src,
          sizeof(r->extras.news.img_src));
      json_get_str(item, "source", r->extras.news.source,
          sizeof(r->extras.news.source));
      break;

    case SXNG_CAT_VIDEOS:
      if(!json_get_str(item, "thumbnail_src", r->extras.video.thumbnail,
          sizeof(r->extras.video.thumbnail)))
        json_get_str(item, "thumbnail", r->extras.video.thumbnail,
            sizeof(r->extras.video.thumbnail));

      json_get_str(item, "iframe_src", r->extras.video.iframe_src,
          sizeof(r->extras.video.iframe_src));
      json_get_str(item, "author", r->extras.video.author,
          sizeof(r->extras.video.author));
      json_get_str(item, "publishedDate", r->extras.video.published,
          sizeof(r->extras.video.published));

      if(!json_get_str(item, "length", r->extras.video.length,
          sizeof(r->extras.video.length)))
      {
        int32_t secs;

        if(json_get_int(item, "length", &secs) && secs > 0)
        {
          if(secs >= 3600)
            snprintf(r->extras.video.length,
                sizeof(r->extras.video.length),
                "%d:%02d:%02d",
                secs / 3600, (secs % 3600) / 60, secs % 60);
          else
            snprintf(r->extras.video.length,
                sizeof(r->extras.video.length),
                "%d:%02d", secs / 60, secs % 60);
        }
      }

      if(r->extras.video.length[0] == '\0')
        json_get_str(item, "duration", r->extras.video.length,
            sizeof(r->extras.video.length));

      break;

    case SXNG_CAT_MUSIC:
      json_get_str(item, "author", r->extras.music.author,
          sizeof(r->extras.music.author));
      json_get_str(item, "publishedDate", r->extras.music.published,
          sizeof(r->extras.music.published));
      break;

    case SXNG_CAT_GENERAL:
    case SXNG_CAT__COUNT:
      break;
  }
}

// JSON parse specs

static const json_spec_t sxng_result_spec[] = {
  { JSON_STR,   "title",   false, offsetof(sxng_result_t, title),
    .len = sizeof(((sxng_result_t *)0)->title) },
  { JSON_STR,   "url",     true,  offsetof(sxng_result_t, url),
    .len = sizeof(((sxng_result_t *)0)->url) },
  { JSON_STR,   "content", false, offsetof(sxng_result_t, snippet),
    .len = sizeof(((sxng_result_t *)0)->snippet) },
  { JSON_STR,   "engine",  false, offsetof(sxng_result_t, engine),
    .len = sizeof(((sxng_result_t *)0)->engine) },
  { JSON_FLOAT, "score",   false, offsetof(sxng_result_t, score) },
  { JSON_END }
};

// Curl completion: parse JSON, build result array, invoke caller cb.
static void
sxng_curl_done(const curl_response_t *cresp)
{
  struct json_object *root;
  size_t want;
  sxng_response_t resp;
  struct json_object *jresults;
  size_t len;
  sxng_result_t *out;
  sxng_req_t r;

  sxng_req_retire((sxng_req_t *)cresp->user_data, &r);

  if(cresp->curl_code != 0)
  {
    char buf[256];

    snprintf(buf, sizeof(buf), "transport: %s",
        cresp->error != NULL ? cresp->error : "unknown");
    clam(CLAM_WARN, SXNG_CTX, "%s", buf);
    sxng_deliver_fail(&r, "transport error");
    return;
  }

  if(cresp->status != 200)
  {
    char buf[64];

    snprintf(buf, sizeof(buf), "HTTP %ld", cresp->status);
    clam(CLAM_WARN, SXNG_CTX, "searxng endpoint returned %s", buf);
    sxng_deliver_fail(&r, "http error");
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, SXNG_CTX);

  if(root == NULL)
  {
    sxng_deliver_fail(&r, "malformed JSON response");
    return;
  }

  jresults = json_get_array(root, "results");

  if(jresults == NULL)
  {
    json_object_put(root);
    sxng_deliver_fail(&r, "no 'results' array in response");
    return;
  }

  len = (size_t)json_object_array_length(jresults);
  want = r.n_wanted;

  if(want > SXNG_HARDMAX_RESULTS)
    want = SXNG_HARDMAX_RESULTS;
  if(len > want)
    len = want;

  out = NULL;

  if(len > 0)
  {
    size_t kept;
    out = mem_alloc(SXNG_CTX, "results", len * sizeof(*out));
    memset(out, 0, len * sizeof(*out));

    kept = 0;

    for(size_t i = 0; i < len; i++)
    {
      struct json_object *item = json_object_array_get_idx(jresults,
          (int)i);

      if(item == NULL || !json_object_is_type(item, json_type_object))
        continue;

      if(!json_extract(item, &out[kept], sxng_result_spec,
          SXNG_CTX ":result"))
        continue;   // required field (url) missing; skip row

      out[kept].category = r.category;
      sxng_extract_extras(item, &out[kept], r.category);

      kept++;
    }

    len = kept;
  }

  resp = (sxng_response_t){
    .ok        = true,
    .error     = NULL,
    .category  = r.category,
    .results   = out,
    .n_results = len,
    .user_data = r.user_data,
  };

  if(r.cb != NULL)
    r.cb(&resp);

  if(out != NULL)
    mem_free(out);

  json_object_put(root);
}

// Public API

bool
sxng_search(const char *query, sxng_category_t category, size_t n_wanted,
    sxng_done_cb_t cb, void *user_data)
{
  uint32_t to_sec;
  uint32_t safe;
  char encoded[SXNG_QUERY_SZ];
  int need;
  char url[SXNG_URL_SZ];
  curl_request_t *cr;
  const char *endpoint;
  uint32_t kv_max;
  uint32_t kv_min;
  sxng_req_t *r;
  sxng_req_t dead;

  if(cb == NULL)
    return(FAIL);

  if(query == NULL || query[0] == '\0')
    return(FAIL);

  if((unsigned)category >= SXNG_CAT__COUNT)
    category = SXNG_CAT_GENERAL;

  endpoint = kv_get_str("plugin.searxng.endpoint");

  if(endpoint == NULL || endpoint[0] == '\0')
  {
    clam(CLAM_WARN, SXNG_CTX,
        "plugin.searxng.endpoint is empty; cannot submit query");
    return(FAIL);
  }

  kv_max = (uint32_t)kv_get_uint("plugin.searxng.max_results");
  kv_min = (uint32_t)kv_get_uint("plugin.searxng.min_results");
  safe = (uint32_t)kv_get_uint("plugin.searxng.safesearch");
  to_sec = (uint32_t)kv_get_uint("plugin.searxng.timeout_secs");

  if(kv_max == 0)
    kv_max = 10;
  if(kv_max > SXNG_HARDMAX_RESULTS)
    kv_max = SXNG_HARDMAX_RESULTS;

  // Result floor. Default 1; can never exceed the per-query cap. A
  // caller passing 0 still means "use the default cap" (preserves the
  // inference layer's contract), so the floor only ever raises an
  // explicit-but-too-small request up to kv_min.
  if(kv_min == 0)
    kv_min = 1;
  if(kv_min > kv_max)
    kv_min = kv_max;

  if(n_wanted == 0 || n_wanted > kv_max)
    n_wanted = kv_max;
  if(n_wanted < kv_min)
    n_wanted = kv_min;

  if(safe > 2)
    safe = 2;


  if(sxng_urlencode(query, encoded, sizeof(encoded)) >= sizeof(encoded))
  {
    clam(CLAM_WARN, SXNG_CTX, "query too long after URL encoding");
    return(FAIL);
  }

  need = snprintf(url, sizeof(url),
      "%s?q=%s&format=json&safesearch=%u&categories=%s",
      endpoint, encoded, safe, sxng_category_name(category));

  if(need < 0 || (size_t)need >= sizeof(url))
  {
    clam(CLAM_WARN, SXNG_CTX, "assembled URL exceeds %zu bytes",
        sizeof(url));
    return(FAIL);
  }

  r = mem_alloc(SXNG_CTX, "request", sizeof(*r));
  memset(r, 0, sizeof(*r));

  r->cb        = cb;
  r->user_data = user_data;
  r->n_wanted  = n_wanted;
  r->category  = category;

  cr = curl_request_create(CURL_METHOD_GET, url,
      sxng_curl_done, r);

  if(cr == NULL)
  {
    mem_free(r);
    return(FAIL);
  }

  if(to_sec > 0)
    curl_request_set_timeout(cr, to_sec);

  curl_request_add_header(cr, "Accept: application/json");

  // File it before submitting, never after: the completion can run on a
  // curl worker before submit has even returned here, and it retires
  // what it finds.
  sxng_req_track(r);

  if(curl_request_submit(cr) != SUCCESS)
  {
    sxng_req_retire(r, &dead);
    return(FAIL);
  }

  clam(CLAM_DEBUG2, SXNG_CTX,
      "submitted query='%s' category=%s n_wanted=%zu",
      query, sxng_category_name(category), n_wanted);

  return(SUCCESS);
}

// Plugin lifecycle

// No commands, no tasks, no clam subscriptions. The one thing that does
// outlive a call is the in-flight registry, and the unmap listener that
// keeps it honest is the single registration deinit() has to mirror.
static bool
sxng_init(void)
{
  plugin_unmap_notify_register(sxng_unmap_cb, NULL);

  clam(CLAM_INFO, SXNG_CTX, "searxng plugin initialized");
  return(SUCCESS);
}

static void
sxng_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(sxng_unmap_cb);

  pthread_mutex_lock(&sxng_active_mutex);
  stranded = sxng_active_count;
  pthread_mutex_unlock(&sxng_active_mutex);

  // Nothing to free: those queries belong to curl, and their completions
  // live in the mapping now going away. Core's residual audit sees them
  // — sxng_curl_done is curl_iter_req_t.cb for every one — so it is the
  // audit that refuses the dlclose, not us. Naming the count here is
  // what makes that refusal legible.
  if(stranded > 0)
    clam(CLAM_WARN, SXNG_CTX, "%u search(es) still in flight at deinit",
        stranded);

  clam(CLAM_INFO, SXNG_CTX, "searxng plugin deinitialized");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = SXNG_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = SXNG_CTX,
  .provides        = { { .name = "service_searxng" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = sxng_kv_schema,
  .kv_schema_count = sizeof(sxng_kv_schema) / sizeof(sxng_kv_schema[0]),
  .init            = sxng_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = sxng_deinit,
  .ext             = NULL,
};
