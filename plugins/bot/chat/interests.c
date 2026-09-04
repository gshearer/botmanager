// botmanager — MIT
// Parse a personality's `interests:` JSON frontmatter into acquire_topic_t.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "json.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Absent `attention` — the topic gets an even share of the bot's own
// initiative. The four words below are sugar over four points on the
// same 0..100 scale a bare integer writes directly.
#define MP_ATTENTION_DEFAULT  50

// Every key a topic object may carry. The first five are the schema a
// personality author writes; the last three are advanced and stay
// undocumented in the common example. Anything else is a typo, and a
// typo that vanishes silently is the whole reason this list exists.
static const char *const mp_known_keys[] = {
  "topic", "aka", "attention", "watch",
  "category", "encyclopedic", "max_sources",
};

// Name every key we do not recognise. An operator's `"wach"` used to
// parse into a topic that simply never fetched anything.
static void
mp_warn_unknown_keys(struct json_object *obj, size_t idx, const char *topic)
{
  json_object_object_foreach(obj, key, val)
  {
    bool known = false;

    (void)val;

    for(size_t i = 0; i < sizeof(mp_known_keys) / sizeof(*mp_known_keys); i++)
    {
      if(strcmp(key, mp_known_keys[i]) == 0)
      {
        known = true;
        break;
      }
    }

    if(!known)
      clam(CLAM_WARN, "chatbot",
          "interests[%zu] (topic='%s'): unknown key '%s' — ignored",
          idx, topic, key);
  }
}

// `attention` is how much of the bot's own initiative a topic gets: 0
// never goes looking, 100 goes looking every time the dice pick it.
// ⚠ It takes a bare integer as well as a word on purpose — the
// personalities in this tree already spread across ten distinct
// weights, so a word-only scale would silently retune half of them.
//
// Returns false on a value that is neither, which skips the topic:
// guessing 50 for a typo would start a topic the author may have
// written `off` against.
static bool
mp_attention_from_obj(struct json_object *obj, uint32_t *out,
    size_t idx, const char *topic)
{
  struct json_object *v = NULL;
  const char *s;
  char *end;
  long n;

  *out = MP_ATTENTION_DEFAULT;

  if(!json_object_object_get_ex(obj, "attention", &v) || v == NULL)
    return(true);

  if(json_object_is_type(v, json_type_int)
      || json_object_is_type(v, json_type_double))
    n = (long)json_object_get_int64(v);

  else if(json_object_is_type(v, json_type_string))
  {
    s = json_object_get_string(v);

    if(s == NULL)
      s = "";

    if(strcasecmp(s, "off")    == 0) { *out = 0;   return(true); }
    if(strcasecmp(s, "low")    == 0) { *out = 25;  return(true); }
    if(strcasecmp(s, "normal") == 0) { *out = 50;  return(true); }
    if(strcasecmp(s, "high")   == 0) { *out = 100; return(true); }

    // A number written as a string is an author being careful with
    // JSON, not an error.
    errno = 0;
    n = strtol(s, &end, 10);

    if(end == s || *end != '\0' || errno != 0)
    {
      clam(CLAM_WARN, "chatbot",
          "interests[%zu] (topic='%s'): attention='%s' is neither"
          " off/low/normal/high nor a number — skipping topic",
          idx, topic, s);
      return(false);
    }
  }

  else
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu] (topic='%s'): attention must be a word or a"
        " number — skipping topic",
        idx, topic);
    return(false);
  }

  if(n < 0)   n = 0;
  if(n > 100) n = 100;

  *out = (uint32_t)n;
  return(true);
}

// Parse one `watch` entry into a feed slot. The entry is either a bare
// URL string or an object carrying `url` and an optional `every`
// (seconds). Returns true only when the entry is well-formed;
// malformed entries log WARN and the caller skips them without
// aborting the rest of the list.
static bool
mp_feed_from_json(struct json_object *item,
    acquire_feed_t *out, size_t topic_idx, size_t watch_idx)
{
  char type_str[16] = {0};
  int32_t every = 0;

  memset(out, 0, sizeof(*out));

  // Everything this tree watches today is a page, so HTML is the
  // default and `type: "rss"` is the escape hatch for a real feed. It
  // is deliberately not guessed from the URL — a `.xml` that turns out
  // to be a sitemap would fail inside libxml2 rather than at the
  // declaration.
  out->kind = ACQUIRE_FEED_HTML;

  if(json_object_is_type(item, json_type_string))
  {
    const char *url = json_object_get_string(item);

    if(url == NULL || url[0] == '\0')
    {
      clam(CLAM_WARN, "chatbot",
          "interests[%zu].watch[%zu]: empty URL — skipping",
          topic_idx, watch_idx);
      return(false);
    }

    strlcpy(out->url, url, sizeof(out->url));
    out->cadence_secs = ACQUIRE_FEED_CADENCE_MIN;
    return(true);
  }

  if(!json_object_is_type(item, json_type_object))
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu].watch[%zu]: expected a URL or an object"
        " — skipping",
        topic_idx, watch_idx);
    return(false);
  }

  // json_get_str already NUL-terminates and truncates on overflow, so
  // an empty value is the only thing left to reject here.
  if(!json_get_str(item, "url", out->url, sizeof(out->url))
      || out->url[0] == '\0')
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu].watch[%zu]: missing or empty 'url' — skipping",
        topic_idx, watch_idx);
    return(false);
  }

  if(json_get_str(item, "type", type_str, sizeof(type_str))
      && type_str[0] != '\0')
  {
    if(strcasecmp(type_str, "rss") == 0)
      out->kind = ACQUIRE_FEED_RSS;

    else if(strcasecmp(type_str, "html") != 0)
    {
      clam(CLAM_WARN, "chatbot",
          "interests[%zu].watch[%zu]: unknown type='%s'"
          " (expected rss/html) — skipping",
          topic_idx, watch_idx, type_str);
      return(false);
    }
  }

  if(json_get_int(item, "every", &every) && every > 0)
    out->cadence_secs = (uint32_t)every;

  if(out->cadence_secs < ACQUIRE_FEED_CADENCE_MIN)
  {
    if(out->cadence_secs != 0)
      clam(CLAM_DEBUG2, "chatbot",
          "interests[%zu].watch[%zu]: every=%u clamped up to %u",
          topic_idx, watch_idx, out->cadence_secs,
          (uint32_t)ACQUIRE_FEED_CADENCE_MIN);

    out->cadence_secs = ACQUIRE_FEED_CADENCE_MIN;
  }

  return(true);
}

// Append a JSON array of strings to the keyword slots, skipping
// non-string and empty entries. Returns the number of entries that did
// not fit — the caller names the topic in the warning, this does not
// know it.
static size_t
mp_append_keywords(struct json_object *arr,
    char (*dest)[ACQUIRE_KEYWORD_SZ], size_t *n, size_t cap)
{
  size_t len     = (size_t)json_object_array_length(arr);
  size_t dropped = 0;

  for(size_t i = 0; i < len; i++)
  {
    struct json_object *v = json_object_array_get_idx(arr, (int)i);
    const char *s;

    if(v == NULL || !json_object_is_type(v, json_type_string))
      continue;

    s = json_object_get_string(v);

    if(s == NULL || s[0] == '\0')
      continue;

    if(*n >= cap)
    {
      dropped++;
      continue;
    }

    strlcpy(dest[*n], s, ACQUIRE_KEYWORD_SZ);
    (*n)++;
  }

  return(dropped);
}

// Extract one topic from a JSON object; returns true on success.
static bool
mp_topic_from_obj(struct json_object *obj,
    acquire_topic_t *out, size_t idx)
{
  struct json_object *aka;
  struct json_object *watch;
  size_t dropped;

  memset(out, 0, sizeof(*out));

  // `topic` is the thing itself, and it is three fields: what the
  // topic is called, what a proactive fire searches for, and the first
  // string an inbound line is scanned against. Writing it once is the
  // point of the schema — `name`/`query`/`keywords[0]` were the same
  // string three times in every topic in this tree.
  if(!json_get_str(obj, "topic", out->name, sizeof(out->name))
      || out->name[0] == '\0')
  {
    clam(CLAM_WARN, "chatbot",
        "interests[%zu]: missing or empty 'topic' — skipping", idx);
    return(false);
  }

  strlcpy(out->query,       out->name, sizeof(out->query));
  strlcpy(out->keywords[0], out->name, sizeof(out->keywords[0]));
  out->n_keywords = 1;

  mp_warn_unknown_keys(obj, idx, out->name);

  if(!mp_attention_from_obj(obj, &out->proactive_weight, idx, out->name))
    return(false);

  // `aka` — the topic's other names. Triggers and alias search terms
  // both; they only ever reach the keyword slots.
  aka = json_get_array(obj, "aka");

  if(aka != NULL)
  {
    dropped = mp_append_keywords(aka, out->keywords, &out->n_keywords,
        ACQUIRE_KEYWORDS_MAX);

    if(dropped > 0)
      clam(CLAM_WARN, "chatbot",
          "interests[%zu] (topic='%s'): %zu aka entr%s past the"
          " %d-name limit — truncated",
          idx, out->name, dropped, dropped == 1 ? "y" : "ies",
          ACQUIRE_KEYWORDS_MAX);
  }

  // `watch` — feed URLs polled on their own cadence, independent of
  // whatever the search engines turn up.
  watch = json_get_array(obj, "watch");

  if(watch != NULL)
  {
    size_t len   = (size_t)json_object_array_length(watch);
    size_t n_fed = 0;

    for(size_t i = 0; i < len && n_fed < ACQUIRE_FEEDS_MAX; i++)
    {
      struct json_object *item = json_object_array_get_idx(watch, (int)i);

      if(item == NULL)
        continue;

      if(mp_feed_from_json(item, &out->feeds[n_fed], idx, i))
        n_fed++;
    }

    out->n_feeds = n_fed;
  }

  // Advanced keys. Each is a knob a personality rarely wants and none
  // of them belongs in the common example.
  {
    int32_t ms = 0;

    if(json_get_int(obj, "max_sources", &ms) && ms > 0)
      out->max_sources = (uint32_t)ms;
  }

  json_get_bool(obj, "encyclopedic", &out->encyclopedic);

  // Optional SearXNG category ("general"/"images"/"news"/"videos"/
  // "music"). Empty / unknown is silently treated as general at submit
  // time; no point WARN'ing here when the engine handles it.
  json_get_str(obj, "category", out->category, sizeof(out->category));

  return(true);
}

// Each array element is a JSON object with `topic` (required) and
// optional `aka`, `attention`, `watch`, plus the advanced
// `category`, `encyclopedic` and `max_sources`.
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
          "interests[%zu] parsed: topic='%s' attention=%u"
          " names=%zu watch=%zu",
          i, out[n].name, out[n].proactive_weight,
          out[n].n_keywords, out[n].n_feeds);
      n++;
    }
  }

  json_object_put(root);

  *n_out = n;
  return(SUCCESS);
}
