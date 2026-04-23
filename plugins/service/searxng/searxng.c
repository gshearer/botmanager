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
  { "plugin.searxng.timeout_secs", KV_UINT32, "15",
    "Per-request timeout in seconds" },
  { "plugin.searxng.max_results",  KV_UINT32, "10",
    "Default result cap per query (hard max 32)" },
  { "plugin.searxng.safesearch",   KV_UINT32, "1",
    "SafeSearch level: 0=off, 1=moderate, 2=strict" },
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

// Deliver a failure response and free the request context.
static void
sxng_deliver_fail(sxng_req_t *r, const char *msg)
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

  mem_free(r);
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
  sxng_req_t *r = (sxng_req_t *)cresp->user_data;

  if(cresp->curl_code != 0)
  {
    char buf[256];

    snprintf(buf, sizeof(buf), "transport: %s",
        cresp->error != NULL ? cresp->error : "unknown");
    clam(CLAM_WARN, SXNG_CTX, "%s", buf);
    sxng_deliver_fail(r, "transport error");
    return;
  }

  if(cresp->status != 200)
  {
    char buf[64];

    snprintf(buf, sizeof(buf), "HTTP %ld", cresp->status);
    clam(CLAM_WARN, SXNG_CTX, "searxng endpoint returned %s", buf);
    sxng_deliver_fail(r, "http error");
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, SXNG_CTX);

  if(root == NULL)
  {
    sxng_deliver_fail(r, "malformed JSON response");
    return;
  }

  jresults = json_get_array(root, "results");

  if(jresults == NULL)
  {
    json_object_put(root);
    sxng_deliver_fail(r, "no 'results' array in response");
    return;
  }

  len = (size_t)json_object_array_length(jresults);
  want = r->n_wanted;

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

      out[kept].category = r->category;
      sxng_extract_extras(item, &out[kept], r->category);

      kept++;
    }

    len = kept;
  }

  resp = (sxng_response_t){
    .ok        = true,
    .error     = NULL,
    .category  = r->category,
    .results   = out,
    .n_results = len,
    .user_data = r->user_data,
  };

  if(r->cb != NULL)
    r->cb(&resp);

  if(out != NULL)
    mem_free(out);

  json_object_put(root);
  mem_free(r);
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
  sxng_req_t *r;
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
  safe = (uint32_t)kv_get_uint("plugin.searxng.safesearch");
  to_sec = (uint32_t)kv_get_uint("plugin.searxng.timeout_secs");

  if(kv_max == 0)
    kv_max = 10;
  if(kv_max > SXNG_HARDMAX_RESULTS)
    kv_max = SXNG_HARDMAX_RESULTS;

  if(n_wanted == 0 || n_wanted > kv_max)
    n_wanted = kv_max;

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

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(r);
    return(FAIL);
  }

  clam(CLAM_DEBUG2, SXNG_CTX,
      "submitted query='%s' category=%s n_wanted=%zu",
      query, sxng_category_name(category), n_wanted);

  return(SUCCESS);
}

// Plugin lifecycle

static bool
sxng_init(void)
{
  clam(CLAM_INFO, SXNG_CTX, "searxng plugin initialized");
  return(SUCCESS);
}

static void
sxng_deinit(void)
{
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
