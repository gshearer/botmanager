// botmanager — MIT
// Wordnik service plugin: keyed access to the api.wordnik.com v4
// word-of-the-day and dictionary endpoints. Normalizes each payload into
// one flat record — senses, citations and related words in fixed arrays,
// Wordnik's inline XML markup stripped out — and seals the Wordnik-side
// conventions (the "pdd" publication date, the nullable note, the two
// spellings of the per-sense dictionary id, the array-vs-object shape of
// sibling endpoints) behind wordnik_api.h. A dictionary lookup fans out
// into three parallel requests joined by a refcount, so the whole entry
// arrives in one callback without a single blocking wait. Pure
// connectivity; the command surface half owns all presentation.
#define WORDNIK_INTERNAL
#include "wordnik.h"

#include "wordnik_cmd.h"

#include <ctype.h>

// ----------------------------------------------------------------------
// KV schema
// ----------------------------------------------------------------------

// The API key lives under a "creds" segment so the KV layer treats it as
// secret-tier: `show kv` and every non-admin reader see the redaction
// marker instead of the token (see kv_is_secret_key).
static const plugin_kv_entry_t wordnik_kv_schema[] = {
  { WORDNIK_KV_API_KEY,  KV_STR,    "",
    "Wordnik API key (developer.wordnik.com); redacted from non-admin reads" },
  { WORDNIK_KV_MAX_DEFS, KV_UINT32, "3",
    "Definitions carried per word (hard max 8)" },
  { WORDNIK_KV_MAX_EX,   KV_UINT32, "2",
    "Usage citations carried per word (hard max 4)" },
  { WORDNIK_KV_MAX_REL,  KV_UINT32, "6",
    "Synonyms — and separately antonyms — carried per word (hard max 12)" },
  { WORDNIK_KV_TIMEOUT,  KV_UINT32, "10",
    "Per-request timeout in seconds" },
};

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Copy the secret-tier API key into `buf`. kv_get_creds bypasses the
// redaction that a public (non-admin) command path would otherwise hit;
// the value never leaves this plugin except as an outbound query param.
static bool
wordnik_api_key(char *buf, size_t cap)
{
  const char *k = kv_get_creds(WORDNIK_KV_API_KEY);

  snprintf(buf, cap, "%s", k != NULL ? k : "");

  return(buf[0] != '\0' ? SUCCESS : FAIL);
}

// Percent-encode `in` into `out`, keeping the RFC-3986 unreserved set
// verbatim. Returns the length the full encoding needs, which may exceed
// cap (snprintf semantics), so callers can detect truncation.
static size_t
wordnik_urlencode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t            n = 0;

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

static wordnik_status_t
wordnik_status_of_http(long http, int curl_code)
{
  if(curl_code != 0)
    return(WORDNIK_TRANSPORT);

  switch(http)
  {
    case 200: return(WORDNIK_OK);
    case 401:
    case 403: return(WORDNIK_AUTH);

    // A date Wordnik has published no word for answers 204 with an empty
    // body rather than 404 — same meaning to a reader.
    case 204:
    case 404: return(WORDNIK_NOT_FOUND);
    case 429: return(WORDNIK_RATE_LIMITED);
    default:  return(WORDNIK_TRANSPORT);
  }
}

// A date is only ever pasted into the query string, so validate it
// strictly rather than encode it: exactly YYYY-MM-DD, calendar-plausible.
// That leaves no byte a caller could smuggle a second parameter through.
static bool
wordnik_date_valid(const char *date)
{
  static const uint8_t digit_at[] = { 0, 1, 2, 3, 5, 6, 8, 9 };
  unsigned              month;
  unsigned              day;

  if(date == NULL || strnlen(date, WORDNIK_DATE_SZ) != 10)
    return(false);

  if(date[4] != '-' || date[7] != '-')
    return(false);

  for(size_t i = 0; i < sizeof(digit_at); i++)
    if(!isdigit((unsigned char)date[digit_at[i]]))
      return(false);

  month = (unsigned)((date[5] - '0') * 10 + (date[6] - '0'));
  day   = (unsigned)((date[8] - '0') * 10 + (date[9] - '0'));

  return(month >= 1 && month <= 12 && day >= 1 && day <= 31);
}

// Create, configure and submit one GET. On FAIL nothing was queued and
// the caller still owns `user_data`; on SUCCESS the completion callback
// takes ownership of it.
static bool
wordnik_submit(const char *url, curl_done_cb_t done, void *user_data)
{
  curl_request_t *cr = curl_request_create(CURL_METHOD_GET, url, done,
      user_data);
  uint32_t        timeout;

  if(cr == NULL)
    return(FAIL);

  timeout = (uint32_t)kv_get_uint(WORDNIK_KV_TIMEOUT);

  if(timeout > 0)
    curl_request_set_timeout(cr, timeout);

  curl_request_add_header(cr, "Accept: application/json");

  return(curl_request_submit(cr));
}

// Rewrite `s` in place, dropping Wordnik's inline XML markup (<xref>,
// <em>, <internalXref …>, …) and collapsing the runs of whitespace that
// removing a tag can leave behind. Purely lexical: an unterminated '<'
// swallows the remainder, which is the right call for a display string.
static void
wordnik_strip_markup(char *s)
{
  size_t w = 0;
  bool   in_tag = false;
  bool   pending_space = false;

  for(size_t r = 0; s[r] != '\0'; r++)
  {
    char c = s[r];

    if(in_tag)
    {
      if(c == '>')
        in_tag = false;

      continue;
    }

    if(c == '<')
    {
      in_tag = true;
      continue;
    }

    if(isspace((unsigned char)c))
    {
      pending_space = (w > 0);
      continue;
    }

    if(pending_space)
    {
      s[w++] = ' ';
      pending_space = false;
    }

    s[w++] = c;
  }

  s[w] = '\0';
}

// ----------------------------------------------------------------------
// Payload normalization
// ----------------------------------------------------------------------

static const json_spec_t wordnik_def_spec[] = {
  { JSON_STR, "text",         false, offsetof(wordnik_def_t, text),
    .len = sizeof(((wordnik_def_t *)0)->text) },
  { JSON_STR, "partOfSpeech", false, offsetof(wordnik_def_t, part_of_speech),
    .len = sizeof(((wordnik_def_t *)0)->part_of_speech) },
  { JSON_STR, "source",       false, offsetof(wordnik_def_t, source),
    .len = sizeof(((wordnik_def_t *)0)->source) },
  { JSON_END }
};

static const json_spec_t wordnik_example_spec[] = {
  { JSON_STR, "text",  false, offsetof(wordnik_example_t, text),
    .len = sizeof(((wordnik_example_t *)0)->text) },
  { JSON_STR, "title", false, offsetof(wordnik_example_t, title),
    .len = sizeof(((wordnik_example_t *)0)->title) },
  { JSON_STR, "url",   false, offsetof(wordnik_example_t, url),
    .len = sizeof(((wordnik_example_t *)0)->url) },
  { JSON_END }
};

// "pdd" is the plain publication date; "note" is frequently JSON null,
// which reads as absent. The word itself is the one field a consumer
// cannot do without.
static const json_spec_t wordnik_wotd_spec[] = {
  { JSON_STR, "word", true,  offsetof(wordnik_wotd_t, word),
    .len = sizeof(((wordnik_wotd_t *)0)->word) },
  { JSON_STR, "pdd",  false, offsetof(wordnik_wotd_t, date),
    .len = sizeof(((wordnik_wotd_t *)0)->date) },
  { JSON_STR, "note", false, offsetof(wordnik_wotd_t, note),
    .len = sizeof(((wordnik_wotd_t *)0)->note) },
  { JSON_OBJ_ARRAY, "definitions", false, offsetof(wordnik_wotd_t, defs),
    .sub       = wordnik_def_spec,
    .stride    = sizeof(wordnik_def_t),
    .max_count = WORDNIK_MAX_DEFS,
    .count_off = offsetof(wordnik_wotd_t, n_defs) },
  { JSON_OBJ_ARRAY, "examples", false, offsetof(wordnik_wotd_t, examples),
    .sub       = wordnik_example_spec,
    .stride    = sizeof(wordnik_example_t),
    .max_count = WORDNIK_MAX_EXAMPLES,
    .count_off = offsetof(wordnik_wotd_t, n_examples) },
  { JSON_END }
};

// Drop rows the API sent us with no body text, closing the gap so the
// count a consumer sees matches the rows it can render.
static int32_t
wordnik_compact_defs(wordnik_def_t *defs, int32_t n)
{
  int32_t kept = 0;

  for(int32_t i = 0; i < n; i++)
  {
    wordnik_strip_markup(defs[i].text);

    if(defs[i].text[0] == '\0')
      continue;

    if(kept != i)
      defs[kept] = defs[i];

    kept++;
  }

  memset(&defs[kept], 0, (size_t)(n - kept) * sizeof(*defs));
  return(kept);
}

static int32_t
wordnik_compact_examples(wordnik_example_t *ex, int32_t n)
{
  int32_t kept = 0;

  for(int32_t i = 0; i < n; i++)
  {
    wordnik_strip_markup(ex[i].text);

    if(ex[i].text[0] == '\0')
      continue;

    if(kept != i)
      ex[kept] = ex[i];

    kept++;
  }

  memset(&ex[kept], 0, (size_t)(n - kept) * sizeof(*ex));
  return(kept);
}

// Clamp a KV-configured row count into [1, hard_max].
static int32_t
wordnik_kv_limit(const char *key, int32_t fallback, int32_t hard_max)
{
  int32_t n = (int32_t)kv_get_uint(key);

  if(n <= 0)
    n = fallback;

  return(n > hard_max ? hard_max : n);
}

// Fill `out` from the endpoint's root object. Returns FAIL when the
// payload carries no word, which is the one field every consumer needs.
static bool
wordnik_parse_wotd(struct json_object *root, wordnik_wotd_t *out)
{
  char    published[32];
  int32_t max_defs = wordnik_kv_limit(WORDNIK_KV_MAX_DEFS, 3,
      WORDNIK_MAX_DEFS);
  int32_t max_ex   = wordnik_kv_limit(WORDNIK_KV_MAX_EX, 2,
      WORDNIK_MAX_EXAMPLES);

  memset(out, 0, sizeof(*out));

  if(root == NULL || !json_object_is_type(root, json_type_object))
    return(FAIL);

  if(!json_extract(root, out, wordnik_wotd_spec, WORDNIK_CTX ":wotd"))
    return(FAIL);

  // Older payloads omit "pdd" and carry only the ISO-8601 publishDate;
  // its date half is exactly the form pdd would have held.
  if(out->date[0] == '\0'
      && json_get_str(root, "publishDate", published, sizeof(published)))
  {
    published[strcspn(published, "T")] = '\0';
    snprintf(out->date, sizeof(out->date), "%.10s", published);
  }

  wordnik_strip_markup(out->note);

  out->n_defs     = wordnik_compact_defs(out->defs, out->n_defs);
  out->n_examples = wordnik_compact_examples(out->examples, out->n_examples);

  if(out->n_defs > max_defs)
    out->n_defs = max_defs;

  if(out->n_examples > max_ex)
    out->n_examples = max_ex;

  return(SUCCESS);
}

// Deliver a terminal status with no word, then release the request.
static void
wordnik_deliver_fail(wordnik_req_t *r, wordnik_status_t status)
{
  wordnik_response_t resp = {
    .status    = status,
    .wotd      = NULL,
    .user_data = r->user_data,
  };

  if(r->cb != NULL)
    r->cb(&resp);

  mem_free(r);
}

// Curl completion: parse, normalize, hand the word to the caller, free.
static void
wordnik_curl_done(const curl_response_t *cresp)
{
  wordnik_req_t      *r = (wordnik_req_t *)cresp->user_data;
  struct json_object *root;
  wordnik_wotd_t     *wotd;
  wordnik_response_t  resp;
  wordnik_status_t    status;

  status = wordnik_status_of_http(cresp->status, cresp->curl_code);

  if(status != WORDNIK_OK)
  {
    clam(CLAM_WARN, WORDNIK_CTX, "wotd request failed: %s (http %ld)",
        wordnik_status_str(status), cresp->status);
    wordnik_deliver_fail(r, status);
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, WORDNIK_CTX);

  if(root == NULL)
  {
    wordnik_deliver_fail(r, WORDNIK_MALFORMED);
    return;
  }

  // A whole word record is far too fat for the curl worker's stack.
  wotd = mem_alloc(WORDNIK_CTX, "wotd", sizeof(*wotd));

  if(wordnik_parse_wotd(root, wotd) != SUCCESS)
  {
    mem_free(wotd);
    json_object_put(root);
    wordnik_deliver_fail(r, WORDNIK_MALFORMED);
    return;
  }

  resp = (wordnik_response_t){
    .status    = WORDNIK_OK,
    .wotd      = wotd,
    .user_data = r->user_data,
  };

  clam(CLAM_DEBUG2, WORDNIK_CTX, "wotd %s: '%s' (%d defs, %d examples)",
      wotd->date[0] != '\0' ? wotd->date : "today", wotd->word,
      wotd->n_defs, wotd->n_examples);

  if(r->cb != NULL)
    r->cb(&resp);

  mem_free(wotd);
  json_object_put(root);
  mem_free(r);
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

bool
wordnik_configured(void)
{
  char key[WORDNIK_KEY_SZ];

  return(wordnik_api_key(key, sizeof(key)) == SUCCESS);
}

bool
wordnik_fetch_wotd(const char *date, wordnik_done_cb_t cb, void *user_data)
{
  char           key[WORDNIK_KEY_SZ];
  char           url[WORDNIK_URL_BUF_SZ];
  bool           dated = (date != NULL && date[0] != '\0');
  wordnik_req_t *r;
  int            need;

  if(cb == NULL)
    return(FAIL);

  if(dated && !wordnik_date_valid(date))
  {
    clam(CLAM_WARN, WORDNIK_CTX, "rejecting malformed date '%s'", date);
    return(FAIL);
  }

  if(wordnik_api_key(key, sizeof(key)) != SUCCESS)
  {
    clam(CLAM_WARN, WORDNIK_CTX, WORDNIK_KV_API_KEY " is unset");
    return(FAIL);
  }

  need = dated
      ? snprintf(url, sizeof(url), "%s?date=%s&api_key=%s",
          WORDNIK_API_WOTD, date, key)
      : snprintf(url, sizeof(url), "%s?api_key=%s", WORDNIK_API_WOTD, key);

  if(need < 0 || (size_t)need >= sizeof(url))
  {
    clam(CLAM_WARN, WORDNIK_CTX, "assembled URL exceeds %zu bytes",
        sizeof(url));
    return(FAIL);
  }

  r = mem_alloc(WORDNIK_CTX, "request", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->cb        = cb;
  r->user_data = user_data;

  if(dated)
    snprintf(r->date, sizeof(r->date), "%s", date);

  if(wordnik_submit(url, wordnik_curl_done, r) != SUCCESS)
  {
    mem_free(r);
    return(FAIL);
  }

  clam(CLAM_DEBUG2, WORDNIK_CTX, "submitted wotd date=%s",
      dated ? date : "today");

  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Dictionary lookup — a parallel fan-out joined by a refcount
// ----------------------------------------------------------------------

// The /definitions rows name their dictionary "sourceDictionary", where
// the word-of-the-day rows call the same thing "source"; the callback
// falls back to the shorter spelling when this one is absent.
static const json_spec_t wordnik_word_def_spec[] = {
  { JSON_STR, "text",             false, offsetof(wordnik_def_t, text),
    .len = sizeof(((wordnik_def_t *)0)->text) },
  { JSON_STR, "partOfSpeech",     false,
    offsetof(wordnik_def_t, part_of_speech),
    .len = sizeof(((wordnik_def_t *)0)->part_of_speech) },
  { JSON_STR, "sourceDictionary", false, offsetof(wordnik_def_t, source),
    .len = sizeof(((wordnik_def_t *)0)->source) },
  { JSON_END }
};

// /examples answers with an object wrapping the array, unlike the two
// sibling endpoints.
static const json_spec_t wordnik_examples_root_spec[] = {
  { JSON_OBJ_ARRAY, "examples", false, offsetof(wordnik_word_t, examples),
    .sub       = wordnik_example_spec,
    .stride    = sizeof(wordnik_example_t),
    .max_count = WORDNIK_MAX_EXAMPLES,
    .count_off = offsetof(wordnik_word_t, n_examples) },
  { JSON_END }
};

static const char *
wordnik_part_name(wordnik_part_t part)
{
  switch(part)
  {
    case WORDNIK_PART_DEFS:     return("definitions");
    case WORDNIK_PART_RELATED:  return("relatedWords");
    case WORDNIK_PART_EXAMPLES: return("examples");
    case WORDNIK_PART_ALL:      break;
  }

  return("unknown");
}

// Drop one reference to the join. The last one out delivers the assembled
// entry and frees it — so a caller must never touch `j` after releasing.
static void
wordnik_word_release(wordnik_word_ctx_t *j)
{
  wordnik_word_response_t resp;

  if(__atomic_fetch_sub(&j->pending, 1, __ATOMIC_ACQ_REL) != 1)
    return;

  resp = (wordnik_word_response_t){
    .status    = j->status,
    .word      = j->status == WORDNIK_OK ? &j->result : NULL,
    .user_data = j->user_data,
  };

  if(j->cb != NULL)
    j->cb(&resp);

  mem_free(j);
}

// Shared tail for every part's completion: log, drop the part's closure,
// drop its reference.
static void
wordnik_part_finished(wordnik_part_req_t *p, wordnik_status_t status)
{
  wordnik_word_ctx_t *j = p->join;

  if(status != WORDNIK_OK)
    clam(CLAM_DEBUG2, WORDNIK_CTX, "'%s' %s part: %s", j->result.word,
        wordnik_part_name(p->part), wordnik_status_str(status));

  mem_free(p);
  wordnik_word_release(j);
}

// /definitions: a bare array of sense objects. This is the part that
// decides the lookup's reported status — no definitions, no entry.
static void
wordnik_defs_done(const curl_response_t *cresp)
{
  wordnik_part_req_t *p = (wordnik_part_req_t *)cresp->user_data;
  wordnik_word_ctx_t *j = p->join;
  struct json_object *root;
  wordnik_status_t    status;
  int32_t             max = wordnik_kv_limit(WORDNIK_KV_MAX_DEFS, 3,
      WORDNIK_MAX_DEFS);
  int32_t             kept = 0;
  size_t              len;

  status = wordnik_status_of_http(cresp->status, cresp->curl_code);

  if(status != WORDNIK_OK)
  {
    j->status = status;
    wordnik_part_finished(p, status);
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, WORDNIK_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    if(root != NULL)
      json_object_put(root);

    j->status = WORDNIK_MALFORMED;
    wordnik_part_finished(p, WORDNIK_MALFORMED);
    return;
  }

  len = (size_t)json_object_array_length(root);

  for(size_t i = 0; i < len && kept < max; i++)
  {
    struct json_object *item = json_object_array_get_idx(root, (int)i);
    wordnik_def_t      *d    = &j->result.defs[kept];

    if(item == NULL || !json_object_is_type(item, json_type_object))
      continue;

    memset(d, 0, sizeof(*d));

    if(!json_extract(item, d, wordnik_word_def_spec, WORDNIK_CTX ":def"))
      continue;

    if(d->source[0] == '\0')
      json_get_str(item, "source", d->source, sizeof(d->source));

    wordnik_strip_markup(d->text);

    if(d->text[0] == '\0')
      continue;

    kept++;
  }

  memset(&j->result.defs[kept], 0,
      (size_t)(WORDNIK_MAX_DEFS - kept) * sizeof(j->result.defs[0]));
  j->result.n_defs = kept;
  j->status        = kept > 0 ? WORDNIK_OK : WORDNIK_NOT_FOUND;

  json_object_put(root);
  wordnik_part_finished(p, j->status);
}

// /relatedWords: an array of { relationshipType, words[] } buckets. We
// ask for synonyms and antonyms; anything else that shows up is ignored.
static void
wordnik_related_done(const curl_response_t *cresp)
{
  wordnik_part_req_t *p = (wordnik_part_req_t *)cresp->user_data;
  wordnik_word_ctx_t *j = p->join;
  struct json_object *root;
  wordnik_status_t    status;
  int32_t             max = wordnik_kv_limit(WORDNIK_KV_MAX_REL, 6,
      WORDNIK_MAX_RELATED);
  size_t              len;

  status = wordnik_status_of_http(cresp->status, cresp->curl_code);

  if(status != WORDNIK_OK)
  {
    wordnik_part_finished(p, status);
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, WORDNIK_CTX);

  if(root == NULL || !json_object_is_type(root, json_type_array))
  {
    if(root != NULL)
      json_object_put(root);

    wordnik_part_finished(p, WORDNIK_MALFORMED);
    return;
  }

  len = (size_t)json_object_array_length(root);

  for(size_t i = 0; i < len; i++)
  {
    struct json_object *bucket = json_object_array_get_idx(root, (int)i);
    struct json_object *words;
    char                type[32];
    char              (*dest)[WORDNIK_WORD_SZ];
    int32_t            *count;
    size_t              n_words;

    if(bucket == NULL || !json_get_str(bucket, "relationshipType", type,
        sizeof(type)))
      continue;

    if(strcasecmp(type, "synonym") == 0)
    {
      dest  = j->result.synonyms;
      count = &j->result.n_synonyms;
    }

    else if(strcasecmp(type, "antonym") == 0)
    {
      dest  = j->result.antonyms;
      count = &j->result.n_antonyms;
    }

    else
      continue;

    words   = json_get_array(bucket, "words");
    n_words = words != NULL ? (size_t)json_object_array_length(words) : 0;

    for(size_t w = 0; w < n_words && *count < max; w++)
    {
      const char *s = json_object_get_string(
          json_object_array_get_idx(words, (int)w));

      if(s == NULL || s[0] == '\0')
        continue;

      snprintf(dest[*count], WORDNIK_WORD_SZ, "%s", s);
      (*count)++;
    }
  }

  json_object_put(root);
  wordnik_part_finished(p, WORDNIK_OK);
}

// /examples: an object wrapping the citation array.
static void
wordnik_examples_done(const curl_response_t *cresp)
{
  wordnik_part_req_t *p = (wordnik_part_req_t *)cresp->user_data;
  wordnik_word_ctx_t *j = p->join;
  struct json_object *root;
  wordnik_status_t    status;
  int32_t             max = wordnik_kv_limit(WORDNIK_KV_MAX_EX, 2,
      WORDNIK_MAX_EXAMPLES);

  status = wordnik_status_of_http(cresp->status, cresp->curl_code);

  if(status != WORDNIK_OK)
  {
    wordnik_part_finished(p, status);
    return;
  }

  root = json_parse_buf(cresp->body, cresp->body_len, WORDNIK_CTX);

  if(root == NULL)
  {
    wordnik_part_finished(p, WORDNIK_MALFORMED);
    return;
  }

  json_extract(root, &j->result, wordnik_examples_root_spec,
      WORDNIK_CTX ":examples");

  j->result.n_examples = wordnik_compact_examples(j->result.examples,
      j->result.n_examples);

  if(j->result.n_examples > max)
    j->result.n_examples = max;

  json_object_put(root);
  wordnik_part_finished(p, WORDNIK_OK);
}

// Queue one part. The join's reference count is raised before the submit
// and lowered again if it fails — never through wordnik_word_release,
// because the submitter's own reference guarantees the count cannot reach
// zero here, and going through the release path would imply it might.
static bool
wordnik_submit_part(wordnik_word_ctx_t *j, wordnik_part_t part,
    const char *url)
{
  wordnik_part_req_t *p = mem_alloc(WORDNIK_CTX, "part", sizeof(*p));
  curl_done_cb_t      done;

  p->join = j;
  p->part = part;

  switch(part)
  {
    case WORDNIK_PART_RELATED:  done = wordnik_related_done;  break;
    case WORDNIK_PART_EXAMPLES: done = wordnik_examples_done; break;
    case WORDNIK_PART_DEFS:
    default:                    done = wordnik_defs_done;     break;
  }

  __atomic_fetch_add(&j->pending, 1, __ATOMIC_ACQ_REL);

  if(wordnik_submit(url, done, p) != SUCCESS)
  {
    clam(CLAM_WARN, WORDNIK_CTX, "curl refused the %s request",
        wordnik_part_name(part));
    mem_free(p);
    __atomic_fetch_sub(&j->pending, 1, __ATOMIC_ACQ_REL);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
wordnik_fetch_word(const char *word, uint32_t parts, wordnik_word_cb_t cb,
    void *user_data)
{
  char                key[WORDNIK_KEY_SZ];
  char                enc[WORDNIK_ENC_SZ];
  char                url[WORDNIK_URL_BUF_SZ];
  wordnik_word_ctx_t *j;
  int                 submitted = 0;

  if(cb == NULL || word == NULL || word[0] == '\0')
    return(FAIL);

  parts &= WORDNIK_PART_ALL;

  if(parts == 0)
    return(FAIL);

  if(wordnik_api_key(key, sizeof(key)) != SUCCESS)
  {
    clam(CLAM_WARN, WORDNIK_CTX, WORDNIK_KV_API_KEY " is unset");
    return(FAIL);
  }

  if(wordnik_urlencode(word, enc, sizeof(enc)) >= sizeof(enc))
  {
    clam(CLAM_WARN, WORDNIK_CTX, "word too long after URL encoding");
    return(FAIL);
  }

  j = mem_alloc(WORDNIK_CTX, "lookup", sizeof(*j));
  memset(j, 0, sizeof(*j));
  j->cb        = cb;
  j->user_data = user_data;
  snprintf(j->result.word, sizeof(j->result.word), "%s", word);

  // Every part that fails leaves its rows empty, so only the definitions
  // can turn a lookup into an error — and if they were asked for but
  // never even reached the wire, that is a transport failure.
  j->status  = (parts & WORDNIK_PART_DEFS) != 0 ? WORDNIK_TRANSPORT
                                               : WORDNIK_OK;
  j->pending = 1;  // the submitter's own reference, dropped below

  if((parts & WORDNIK_PART_DEFS) != 0)
  {
    snprintf(url, sizeof(url), "%s/%s/definitions?limit=%d"
        "&useCanonical=true&includeRelated=false&includeTags=false"
        "&api_key=%s", WORDNIK_API_WORD, enc,
        wordnik_kv_limit(WORDNIK_KV_MAX_DEFS, 3, WORDNIK_MAX_DEFS), key);

    if(wordnik_submit_part(j, WORDNIK_PART_DEFS, url) == SUCCESS)
      submitted++;
  }

  if((parts & WORDNIK_PART_RELATED) != 0)
  {
    snprintf(url, sizeof(url), "%s/%s/relatedWords?useCanonical=true"
        "&relationshipTypes=synonym%%2Cantonym"
        "&limitPerRelationshipType=%d&api_key=%s", WORDNIK_API_WORD, enc,
        wordnik_kv_limit(WORDNIK_KV_MAX_REL, 6, WORDNIK_MAX_RELATED), key);

    if(wordnik_submit_part(j, WORDNIK_PART_RELATED, url) == SUCCESS)
      submitted++;
  }

  if((parts & WORDNIK_PART_EXAMPLES) != 0)
  {
    snprintf(url, sizeof(url), "%s/%s/examples?includeDuplicates=false"
        "&useCanonical=true&limit=%d&api_key=%s", WORDNIK_API_WORD, enc,
        wordnik_kv_limit(WORDNIK_KV_MAX_EX, 2, WORDNIK_MAX_EXAMPLES), key);

    if(wordnik_submit_part(j, WORDNIK_PART_EXAMPLES, url) == SUCCESS)
      submitted++;
  }

  // Nothing reached the wire, so nothing will ever deliver: tear the join
  // down by hand rather than through the callback path.
  if(submitted == 0)
  {
    mem_free(j);
    return(FAIL);
  }

  clam(CLAM_DEBUG2, WORDNIK_CTX, "submitted lookup '%s' (%d parts)", word,
      submitted);

  wordnik_word_release(j);
  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
wordnik_init(void)
{
  if(!wordnik_configured())
    clam(CLAM_WARN, WORDNIK_CTX,
        "no API key: set " WORDNIK_KV_API_KEY " to enable lookups");

  // A plugin that cannot raise its own command surface is half-loaded,
  // which is worse than absent — fail the init and let the loader skip it.
  if(wordnik_cmd_register() != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, WORDNIK_CTX, "wordnik plugin initialized");
  return(SUCCESS);
}

static void
wordnik_deinit(void)
{
  wordnik_cmd_unregister();
  clam(CLAM_INFO, WORDNIK_CTX, "wordnik plugin deinitialized");
}

// The command half's upward bot_chat dependency rides on this
// descriptor. Sound only because wordnik is a dependency-graph leaf —
// nothing requires service_wordnik, so nothing inherits it. See
// `PLUGIN.md §Layer Rules` Rule 1, leaf exception.
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = WORDNIK_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = WORDNIK_CTX,
  .provides        = { { .name = "service_wordnik" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = wordnik_kv_schema,
  .kv_schema_count = sizeof(wordnik_kv_schema) / sizeof(wordnik_kv_schema[0]),
  .init            = wordnik_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = wordnik_deinit,
  .ext             = NULL,
};
