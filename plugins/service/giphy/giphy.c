// botmanager — MIT
// Giphy service plugin: keyed access to the api.giphy.com v1 GIF
// endpoints (search, random, trending, translate). Normalizes every
// endpoint's payload — list-shaped or single-object — into one flat
// result row, and seals the Giphy-side conventions (image variants,
// stringly-typed dimensions, the media permalink form) behind
// giphy_api.h. Pure connectivity; the plugin's command surface half
// (giphy_cmd.c) owns all presentation.
#define GIPHY_INTERNAL
#include "giphy.h"

#include "giphy_cmd.h"

// ----------------------------------------------------------------------
// KV schema
// ----------------------------------------------------------------------

// The API key lives under a "creds" segment so the KV layer treats it as
// secret-tier: `show kv` and every non-admin reader see "***" instead of
// the token (see kv_is_secret_key).
static const plugin_kv_entry_t giphy_kv_schema[] = {
  { GIPHY_KV_API_KEY,  KV_STR,    "",
    "Giphy API key (developers.giphy.com); redacted from non-admin reads" },
  { GIPHY_KV_RATING,   KV_STR,    "none",
    "Maximum content rating: g, pg, pg-13, r; \"none\"/\"off\" = broadest "
    "(r). Giphy's public API serves no adult content regardless" },
  { GIPHY_KV_LANG,     KV_STR,    "en",
    "Two-letter search language hint" },
  { GIPHY_KV_MAX,      KV_UINT32, "3",
    "Default results per list query (hard max 25)" },
  { GIPHY_KV_TIMEOUT,  KV_UINT32, "10",
    "Per-request timeout in seconds" },
};

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Percent-encode `in` into `out`, keeping the RFC-3986 unreserved set
// verbatim. Returns the length the full encoding needs, which may exceed
// cap (snprintf semantics), so callers can detect truncation.
static size_t
giphy_urlencode(const char *in, char *out, size_t cap)
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

// Copy the secret-tier API key into `buf`. Reads run inside a scoped
// admin context because kv_get_str redacts secret keys for everyone
// else, and giphy_fetch is reached from public (non-admin) commands.
// The previous flag is restored so we never widen a caller's context.
static bool
giphy_api_key(char *buf, size_t cap)
{
  const char *k;
  bool        prev = kv_admin_context_active();

  kv_admin_context_set(true);
  k = kv_get_str(GIPHY_KV_API_KEY);
  snprintf(buf, cap, "%s", k != NULL ? k : "");
  kv_admin_context_set(prev);

  return(buf[0] != '\0' ? SUCCESS : FAIL);
}

static giphy_status_t
giphy_status_of_http(long http, int curl_code)
{
  if(curl_code != 0)
    return(GIPHY_TRANSPORT);

  switch(http)
  {
    case 200: return(GIPHY_OK);
    case 401:
    case 403: return(GIPHY_AUTH);
    case 404: return(GIPHY_NOT_FOUND);
    case 429: return(GIPHY_RATE_LIMITED);
    default:  return(GIPHY_TRANSPORT);
  }
}

// Giphy renders image dimensions and byte sizes as decimal *strings*
// ("480"), but has been known to emit bare numbers on some variants.
// Accept either; absent or unparseable yields 0.
static uint32_t
giphy_u32_of(struct json_object *obj, const char *key)
{
  char    buf[24];
  int32_t n;

  if(json_get_str(obj, key, buf, sizeof(buf)))
    return((uint32_t)strtoul(buf, NULL, 10));

  if(json_get_int(obj, key, &n) && n > 0)
    return((uint32_t)n);

  return(0);
}

// ----------------------------------------------------------------------
// Payload normalization
// ----------------------------------------------------------------------

static const json_spec_t giphy_gif_spec[] = {
  { JSON_STR, "id",              true,  offsetof(giphy_result_t, id),
    .len = sizeof(((giphy_result_t *)0)->id) },
  { JSON_STR, "title",           false, offsetof(giphy_result_t, title),
    .len = sizeof(((giphy_result_t *)0)->title) },
  { JSON_STR, "url",             false, offsetof(giphy_result_t, page_url),
    .len = sizeof(((giphy_result_t *)0)->page_url) },
  { JSON_STR, "username",        false, offsetof(giphy_result_t, username),
    .len = sizeof(((giphy_result_t *)0)->username) },
  { JSON_STR, "source_tld",      false, offsetof(giphy_result_t, source),
    .len = sizeof(((giphy_result_t *)0)->source) },
  { JSON_STR, "rating",          false, offsetof(giphy_result_t, rating),
    .len = sizeof(((giphy_result_t *)0)->rating) },
  { JSON_STR, "import_datetime", false, offsetof(giphy_result_t, imported),
    .len = sizeof(((giphy_result_t *)0)->imported) },
  { JSON_END }
};

// Fill one result row from a single GIF object. Returns FAIL when the
// row lacks an id, which is the one field every consumer needs (it is
// what the media permalink is built from).
static bool
giphy_parse_gif(struct json_object *item, giphy_result_t *out)
{
  struct json_object *images;
  struct json_object *orig;

  memset(out, 0, sizeof(*out));

  if(item == NULL || !json_object_is_type(item, json_type_object))
    return(FAIL);

  if(!json_extract(item, out, giphy_gif_spec, GIPHY_CTX ":gif"))
    return(FAIL);

  // The permalink Giphy itself hands out for embedding. Query-string
  // free and stable, unlike images.original.url.
  snprintf(out->media_url, sizeof(out->media_url), "%s/%s/giphy.gif",
      GIPHY_MEDIA_BASE, out->id);

  images = json_get_obj(item, "images");
  orig   = images != NULL ? json_get_obj(images, "original") : NULL;

  if(orig != NULL)
  {
    json_get_str(orig, "url",  out->gif_url,  sizeof(out->gif_url));
    json_get_str(orig, "mp4",  out->mp4_url,  sizeof(out->mp4_url));
    json_get_str(orig, "webp", out->webp_url, sizeof(out->webp_url));

    out->width_px   = giphy_u32_of(orig, "width");
    out->height_px  = giphy_u32_of(orig, "height");
    out->size_bytes = giphy_u32_of(orig, "size");
  }

  return(SUCCESS);
}

// Deliver a terminal status with no rows, then release the request.
static void
giphy_deliver_fail(giphy_req_t *r, giphy_status_t status)
{
  giphy_response_t resp = {
    .status    = status,
    .mode      = r->mode,
    .results   = NULL,
    .n_results = 0,
    .user_data = r->user_data,
  };

  if(r->cb != NULL)
    r->cb(&resp);

  mem_free(r);
}

// Curl completion: parse, normalize, hand the rows to the caller, free.
// `data` is an array for the list endpoints and a bare object for
// random/translate; both shapes land in the same result array.
static void
giphy_curl_done(const curl_response_t *cresp)
{
  giphy_req_t        *r = (giphy_req_t *)cresp->user_data;
  struct json_object *root;
  struct json_object *data;
  struct json_object *arr;
  giphy_result_t     *out;
  giphy_response_t    resp;
  giphy_status_t      status;
  size_t              len;
  size_t              kept;

  status = giphy_status_of_http(cresp->status, cresp->curl_code);

  if(status != GIPHY_OK)
  {
    clam(CLAM_WARN, GIPHY_CTX, "%s request failed: %s (http %ld)",
        giphy_mode_name(r->mode), giphy_status_str(status), cresp->status);
    giphy_deliver_fail(r, status);
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, GIPHY_CTX);

  if(root == NULL)
  {
    giphy_deliver_fail(r, GIPHY_MALFORMED);
    return;
  }

  data = json_get_obj(root, "data");
  arr  = json_get_array(root, "data");

  // A keyless or over-quota reply still comes back as HTTP 200 with an
  // empty `data` object, so an absent payload is "no results", not a
  // parse failure.
  if(data == NULL && arr == NULL)
  {
    json_object_put(root);
    giphy_deliver_fail(r, GIPHY_NOT_FOUND);
    return;
  }

  len = arr != NULL ? (size_t)json_object_array_length(arr) : 1;

  if(len > r->n_wanted)
    len = r->n_wanted;

  if(len == 0)
  {
    json_object_put(root);
    giphy_deliver_fail(r, GIPHY_NOT_FOUND);
    return;
  }

  out = mem_alloc(GIPHY_CTX, "results", len * sizeof(*out));
  memset(out, 0, len * sizeof(*out));
  kept = 0;

  for(size_t i = 0; i < len; i++)
  {
    struct json_object *item = arr != NULL
        ? json_object_array_get_idx(arr, (int)i) : data;

    if(giphy_parse_gif(item, &out[kept]) == SUCCESS)
      kept++;
  }

  resp = (giphy_response_t){
    .status    = kept > 0 ? GIPHY_OK : GIPHY_NOT_FOUND,
    .mode      = r->mode,
    .results   = kept > 0 ? out : NULL,
    .n_results = kept,
    .user_data = r->user_data,
  };

  if(r->cb != NULL)
    r->cb(&resp);

  mem_free(out);
  json_object_put(root);
  mem_free(r);
}

// ----------------------------------------------------------------------
// URL assembly
// ----------------------------------------------------------------------

// Resolve plugin.giphy.rating into a ready-to-append "&rating=<val>" URL
// fragment. Any of unset, empty, a literal "" (what /set kv stores for a
// quoted empty — the command surface cannot store a genuine empty
// string), "none", or "off" mean "broadest allowed", which maps to
// rating=r: Giphy's public API tops out there in practice, and stating
// it explicitly is safer than omitting the param, since some endpoints
// default an absent rating to g-rated. There is no true "unfiltered"
// value in the Giphy API — r is as broad as it serves.
static void
giphy_rating_frag(char *out, size_t cap)
{
  const char *rating = kv_get_str(GIPHY_KV_RATING);

  if(rating == NULL || rating[0] == '\0'
      || strcmp(rating, "\"\"") == 0
      || strcasecmp(rating, "none") == 0
      || strcasecmp(rating, "off") == 0)
    rating = "r";

  snprintf(out, cap, "&rating=%s", rating);
}

// Build the full endpoint URL for `mode`. `enc` is the already-encoded
// query, empty when the caller supplied none. Returns FAIL if the
// assembled URL would not fit.
static bool
giphy_build_url(giphy_mode_t mode, const char *key, const char *enc,
    size_t n_wanted, char *url, size_t cap)
{
  const char *lang = kv_get_str(GIPHY_KV_LANG);
  char        rating[GIPHY_RATING_SZ + 8];
  int         need;

  giphy_rating_frag(rating, sizeof(rating));

  if(lang == NULL || lang[0] == '\0')
    lang = "en";

  switch(mode)
  {
    case GIPHY_MODE_RANDOM:
      need = snprintf(url, cap, "%s/random?api_key=%s&tag=%s%s",
          GIPHY_API_BASE, key, enc, rating);
      break;

    case GIPHY_MODE_TRENDING:
      need = snprintf(url, cap,
          "%s/trending?api_key=%s&limit=%zu%s",
          GIPHY_API_BASE, key, n_wanted, rating);
      break;

    case GIPHY_MODE_TRANSLATE:
      need = snprintf(url, cap,
          "%s/translate?api_key=%s&s=%s%s&weirdness=0",
          GIPHY_API_BASE, key, enc, rating);
      break;

    case GIPHY_MODE_SEARCH:
    case GIPHY_MODE__COUNT:
    default:
      need = snprintf(url, cap,
          "%s/search?api_key=%s&q=%s&limit=%zu&offset=0%s&lang=%s",
          GIPHY_API_BASE, key, enc, n_wanted, rating, lang);
      break;
  }

  if(need < 0 || (size_t)need >= cap)
  {
    clam(CLAM_WARN, GIPHY_CTX, "assembled URL exceeds %zu bytes", cap);
    return(FAIL);
  }

  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

bool
giphy_configured(void)
{
  char key[GIPHY_KEY_SZ];

  return(giphy_api_key(key, sizeof(key)) == SUCCESS);
}

bool
giphy_fetch(giphy_mode_t mode, const char *query, size_t n_wanted,
    giphy_done_cb_t cb, void *user_data)
{
  char            key[GIPHY_KEY_SZ];
  char            enc[GIPHY_ENC_SZ];
  char            url[GIPHY_URL_BUF_SZ];
  uint32_t        timeout;
  uint32_t        kv_max;
  curl_request_t *cr;
  giphy_req_t    *r;

  if(cb == NULL)
    return(FAIL);

  if((unsigned)mode >= GIPHY_MODE__COUNT)
    mode = GIPHY_MODE_SEARCH;

  // Only the tag on a random pull is genuinely optional.
  if((query == NULL || query[0] == '\0') && mode != GIPHY_MODE_RANDOM
      && mode != GIPHY_MODE_TRENDING)
    return(FAIL);

  if(giphy_api_key(key, sizeof(key)) != SUCCESS)
  {
    clam(CLAM_WARN, GIPHY_CTX, GIPHY_KV_API_KEY " is unset");
    return(FAIL);
  }

  enc[0] = '\0';

  if(query != NULL && query[0] != '\0'
      && giphy_urlencode(query, enc, sizeof(enc)) >= sizeof(enc))
  {
    clam(CLAM_WARN, GIPHY_CTX, "query too long after URL encoding");
    return(FAIL);
  }

  kv_max = (uint32_t)kv_get_uint(GIPHY_KV_MAX);

  if(kv_max == 0)
    kv_max = 3;

  if(kv_max > GIPHY_MAX_RESULTS)
    kv_max = GIPHY_MAX_RESULTS;

  if(n_wanted == 0 || n_wanted > kv_max)
    n_wanted = kv_max;

  // Random and translate answer with exactly one GIF, by contract.
  if(mode == GIPHY_MODE_RANDOM || mode == GIPHY_MODE_TRANSLATE)
    n_wanted = 1;

  if(giphy_build_url(mode, key, enc, n_wanted, url, sizeof(url)) != SUCCESS)
    return(FAIL);

  r = mem_alloc(GIPHY_CTX, "request", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->cb        = cb;
  r->user_data = user_data;
  r->mode      = mode;
  r->n_wanted  = n_wanted;

  cr = curl_request_create(CURL_METHOD_GET, url, giphy_curl_done, r);

  if(cr == NULL)
  {
    mem_free(r);
    return(FAIL);
  }

  timeout = (uint32_t)kv_get_uint(GIPHY_KV_TIMEOUT);

  if(timeout > 0)
    curl_request_set_timeout(cr, timeout);

  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    mem_free(r);
    return(FAIL);
  }

  clam(CLAM_DEBUG2, GIPHY_CTX, "submitted mode=%s query='%s' n_wanted=%zu",
      giphy_mode_name(mode), query != NULL ? query : "", n_wanted);

  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
giphy_init(void)
{
  if(!giphy_configured())
    clam(CLAM_WARN, GIPHY_CTX,
        "no API key: set " GIPHY_KV_API_KEY " to enable lookups");

  // A plugin that cannot raise its own command surface is half-loaded,
  // which is worse than absent — fail the init and let the loader skip it.
  if(giphy_cmd_register() != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, GIPHY_CTX, "giphy plugin initialized");
  return(SUCCESS);
}

static void
giphy_deinit(void)
{
  giphy_cmd_unregister();
  clam(CLAM_INFO, GIPHY_CTX, "giphy plugin deinitialized");
}

// The command half's upward method_command dependency rides on this
// descriptor. Sound only because giphy is a dependency-graph leaf —
// nothing requires service_giphy, so nothing inherits it. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = GIPHY_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = GIPHY_CTX,
  .provides        = { { .name = "service_giphy" } },
  .provides_count  = 1,
  .requires        = { { .name = "method_command" } },
  .requires_count  = 1,
  .kv_schema       = giphy_kv_schema,
  .kv_schema_count = sizeof(giphy_kv_schema) / sizeof(giphy_kv_schema[0]),
  .init            = giphy_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = giphy_deinit,
  .ext             = NULL,
};
