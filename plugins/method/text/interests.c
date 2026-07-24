// botmanager — MIT
// Parse a personality's `interests:` JSON frontmatter into acquire_topic_t.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "json.h"

#include <string.h>
#include <strings.h>

// Parse `mode` string into acquire_mode_t. Returns true on match.
static bool
mp_mode_from_str(const char *s, acquire_mode_t *out)
{
  if(s == NULL)
    return(false);

  if(strcasecmp(s, "active") == 0)
  {
    *out = ACQUIRE_MODE_ACTIVE;
    return(true);
  }
  if(strcasecmp(s, "reactive") == 0)
  {
    *out = ACQUIRE_MODE_REACTIVE;
    return(true);
  }
  if(strcasecmp(s, "mixed") == 0)
  {
    *out = ACQUIRE_MODE_MIXED;
    return(true);
  }

  return(false);
}

// Parse one source object: {type, url, cadence_secs}. Returns true
// only when the entry is well-formed; malformed entries log WARN and
// the caller skips them without aborting the rest of the list.
static bool
mp_source_from_obj(struct json_object *obj,
    acquire_feed_t *out, size_t topic_idx, size_t src_idx)
{
  char    type_str[16] = {0};
  int32_t cs = 0;

  memset(out, 0, sizeof(*out));

  // `type` — rss or html, case-insensitive.
  if(!json_get_str(obj, "type", type_str, sizeof(type_str))
      || type_str[0] == '\0')
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu].sources[%zu]: missing 'type' — skipping",
        topic_idx, src_idx);
    return(false);
  }

  if(strcasecmp(type_str, "rss") == 0)
    out->kind = ACQUIRE_FEED_RSS;

  else if(strcasecmp(type_str, "html") == 0)
    out->kind = ACQUIRE_FEED_HTML;

  else
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu].sources[%zu]: unknown type='%s'"
        " (expected rss/html) — skipping",
        topic_idx, src_idx, type_str);
    return(false);
  }

  // `url` — required, must fit the buffer. json_get_str already
  // NUL-terminates and truncates on overflow, so we only need to
  // reject empty values here.
  if(!json_get_str(obj, "url", out->url, sizeof(out->url))
      || out->url[0] == '\0')
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu].sources[%zu]: missing or empty 'url' — skipping",
        topic_idx, src_idx);
    return(false);
  }

  // `cadence_secs` — optional, clamped up to ACQUIRE_FEED_CADENCE_MIN.
  // DEBUG2-log when we clamp so personality authors can spot a too-
  // aggressive setting in the log tail.
  if(json_get_int(obj, "cadence_secs", &cs) && cs > 0)
    out->cadence_secs = (uint32_t)cs;

  if(out->cadence_secs < ACQUIRE_FEED_CADENCE_MIN)
  {
    if(out->cadence_secs != 0)
      clam(CLAM_DEBUG2, "chatbot",
          "interests[%zu].sources[%zu]: cadence_secs=%u"
          " clamped up to %u",
          topic_idx, src_idx, out->cadence_secs,
          (uint32_t)ACQUIRE_FEED_CADENCE_MIN);

    out->cadence_secs = ACQUIRE_FEED_CADENCE_MIN;
  }

  return(true);
}

// Copy JSON-array-of-strings into the keyword buffer. Silently skips
// non-string entries. Returns the number of slots written via *n_out.
static void
mp_copy_keywords(struct json_object *arr,
    char (*dest)[ACQUIRE_KEYWORD_SZ], size_t *n_out, size_t cap)
{
  size_t n   = 0;
  size_t len = (size_t)json_object_array_length(arr);

  for(size_t i = 0; i < len && n < cap; i++)
  {
    struct json_object *v = json_object_array_get_idx(arr, (int)i);
    const char *s;

    if(v == NULL || !json_object_is_type(v, json_type_string))
      continue;

    s = json_object_get_string(v);

    if(s == NULL || s[0] == '\0')
      continue;

    snprintf(dest[n], ACQUIRE_KEYWORD_SZ, "%s", s);
    n++;
  }

  *n_out = n;
}

// Extract one topic from a JSON object; returns true on success.
static bool
mp_topic_from_obj(struct json_object *obj,
    acquire_topic_t *out, size_t idx)
{
  struct json_object *kw;
  int32_t cs;
  int32_t pw;
  char mode_str[32] = {0};

  memset(out, 0, sizeof(*out));

  if(!json_get_str(obj, "name", out->name, sizeof(out->name))
      || out->name[0] == '\0')
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu]: missing or empty 'name' — skipping", idx);
    return(false);
  }

  if(!json_get_str(obj, "mode", mode_str, sizeof(mode_str))
      || !mp_mode_from_str(mode_str, &out->mode))
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu] (name='%s'): missing or unknown 'mode' "
        "(expected active/reactive/mixed) — skipping",
        idx, out->name);
    return(false);
  }

  // Default proactive_weight per mode; explicit value overrides.
  switch(out->mode)
  {
    case ACQUIRE_MODE_ACTIVE:   out->proactive_weight = 100; break;
    case ACQUIRE_MODE_REACTIVE: out->proactive_weight = 0;   break;
    case ACQUIRE_MODE_MIXED:    out->proactive_weight = 50;  break;
  }

  pw = 0;

  if(json_get_int(obj, "proactive_weight", &pw))
  {
    if(pw < 0)   pw = 0;
    if(pw > 100) pw = 100;
    out->proactive_weight = (uint32_t)pw;
  }

  cs = 0;

  if(json_get_int(obj, "cadence_secs", &cs) && cs > 0)
    out->cadence_secs = (uint32_t)cs;

  {
    int32_t ms = 0;
    if(json_get_int(obj, "max_sources", &ms) && ms > 0)
      out->max_sources = (uint32_t)ms;
  }

  json_get_str(obj, "query",
      out->query, sizeof(out->query));
  json_get_str(obj, "query_template",
      out->query_template, sizeof(out->query_template));
  json_get_str(obj, "upcoming_query",
      out->upcoming_query, sizeof(out->upcoming_query));

  // Optional SearXNG category ("general"/"images"/"news"/"videos"/
  // "music"). Empty / unknown is silently treated as general at
  // submit time; no point WARN'ing here when the engine handles it.
  json_get_str(obj, "category",
      out->category, sizeof(out->category));

  kw = json_get_array(obj, "keywords");

  if(kw != NULL)
    mp_copy_keywords(kw, out->keywords, &out->n_keywords,
        ACQUIRE_KEYWORDS_MAX);

  // Optional `sources` array: explicit RSS/HTML feed declarations.
  // Users write `"sources": [...]` but the topic struct calls them
  // `feeds` internally to avoid confusion with SXNG "source" results.
  {
    struct json_object *sources = json_get_array(obj, "sources");

    if(sources != NULL)
    {
      size_t src_len = (size_t)json_object_array_length(sources);
      size_t n_src = 0;

      for(size_t i = 0; i < src_len && n_src < ACQUIRE_FEEDS_MAX; i++)
      {
        struct json_object *item =
            json_object_array_get_idx(sources, (int)i);

        if(item == NULL
            || !json_object_is_type(item, json_type_object))
        {
          clam(CLAM_WARN, "chatbot",
              "interests[%zu].sources[%zu]: not an object — skipping",
              idx, i);
          continue;
        }

        if(mp_source_from_obj(item, &out->feeds[n_src], idx, i))
          n_src++;
      }

      out->n_feeds = n_src;
    }
  }

  return(true);
}

// Each array element is a JSON object with `name` + `mode` (required) and
// optional `proactive_weight`, `cadence_secs`, `max_sources`,
// `keywords` (array of strings), `query`, `query_template`, and
// `upcoming_query`.
//
// Validation is per-topic: malformed entries log WARN and skip; well-formed
// entries keep flowing. Outer-JSON parse failure is the only hard-fail path.
bool
chatbot_interests_parse(const char *json,
    acquire_topic_t *out, size_t cap, size_t *n_out)
{
  size_t len;
  size_t n;
  struct json_object *root;

  if(out == NULL || n_out == NULL || cap == 0)
    return(FAIL);

  *n_out = 0;

  if(json == NULL || json[0] == '\0')
    return(SUCCESS);

  root = json_parse_buf(json, strlen(json),
      "chatbot:interests");

  if(root == NULL)
  {
    clam(CLAM_WARN, "chatbot",
        "interests_json failed to parse as JSON — ignoring");
    return(SUCCESS);
  }

  if(!json_object_is_type(root, json_type_array))
  {
    clam(CLAM_WARN, "chatbot",
        "interests_json must be a JSON array of topic objects");
    json_object_put(root);
    return(SUCCESS);
  }

  len = (size_t)json_object_array_length(root);
  n = 0;

  for(size_t i = 0; i < len && n < cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(root, (int)i);

    if(item == NULL || !json_object_is_type(item, json_type_object))
    {
      clam(CLAM_WARN, "chatbot",
          "interests[%zu]: not an object — skipping", i);
      continue;
    }

    if(mp_topic_from_obj(item, &out[n], i))
    {
      clam(CLAM_DEBUG2, "chatbot",
          "interests[%zu] parsed: name='%s' mode=%d weight=%u"
          " keywords=%zu sources=%zu",
          i, out[n].name, (int)out[n].mode, out[n].proactive_weight,
          out[n].n_keywords, out[n].n_feeds);
      n++;
    }
  }

  json_object_put(root);

  *n_out = n;
  return(SUCCESS);
}
