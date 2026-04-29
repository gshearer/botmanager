// botmanager — MIT
// Chat bot LLM fact extractor: sweeps conversation_log, persists to dossier_facts.
#define EXTRACT_INTERNAL
#include "extract.h"

#include "alloc.h"
#include "common.h"
#include "clam.h"
#include "db.h"
#include "dossier.h"
#include "inference.h"
#include "json.h"
#include "kv.h"
#include "task.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>

// Chunk R2 moved extract + memory into the chat plugin. Both live in the
// same libchat.so translation-unit set now, so memory_upsert_dossier_fact
// is a direct call. The plugin_dlsym shim R1 needed while extract.c
// still lived in libcore is retired.

// Module state

static bool             extract_ready = false;
static pthread_mutex_t  extract_stat_mutex;
static extract_stats_t  extract_stats;

// Stat helpers

#define STAT_BUMP(field)                                     \
  do {                                                       \
    pthread_mutex_lock(&extract_stat_mutex);                 \
    extract_stats.field++;                                   \
    pthread_mutex_unlock(&extract_stat_mutex);               \
  } while(0)

// Statistics

void
extract_get_stats(extract_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&extract_stat_mutex);
  *out = extract_stats;
  pthread_mutex_unlock(&extract_stat_mutex);
}

// Fence stripping

// Advance past a leading ```[lang]\n fence and trim a trailing ```. On
// success *pbegin / *pend point into caller's buffer (no allocation).
static void
strip_code_fence(const char *in, size_t len, const char **pbegin, size_t *pout_len)
{
  const char *begin = in;
  const char *end   = in + len;

  while(begin < end && isspace((unsigned char)*begin))
    begin++;

  if(end - begin >= 3 && begin[0] == '`' && begin[1] == '`' && begin[2] == '`')
  {
    const char *nl = memchr(begin, '\n', (size_t)(end - begin));
    if(nl != NULL)
      begin = nl + 1;
    else
      begin += 3;
  }

  while(end > begin && isspace((unsigned char)end[-1]))
    end--;

  if(end - begin >= 3 && end[-1] == '`' && end[-2] == '`' && end[-3] == '`')
  {
    end -= 3;
    while(end > begin && isspace((unsigned char)end[-1]))
      end--;
  }

  *pbegin    = begin;
  *pout_len  = (size_t)(end - begin);
}

// Kind mapping

static bool
kind_from_str(const char *s, mem_fact_kind_t *out)
{
  if(s == NULL)               return(false);
  if(strcmp(s, "preference") == 0) { *out = MEM_FACT_PREFERENCE; return(true); }
  if(strcmp(s, "attribute")  == 0) { *out = MEM_FACT_ATTRIBUTE;  return(true); }
  if(strcmp(s, "relation")   == 0) { *out = MEM_FACT_RELATION;   return(true); }
  if(strcmp(s, "event")      == 0) { *out = MEM_FACT_EVENT;      return(true); }
  if(strcmp(s, "opinion")    == 0) { *out = MEM_FACT_OPINION;    return(true); }
  if(strcmp(s, "freeform")   == 0) { *out = MEM_FACT_FREEFORM;   return(true); }
  return(false);
}

static bool
participants_contain(const extract_participant_t *parts, size_t n,
    int64_t pid)
{
  for(size_t i = 0; i < n; i++)
    if(parts[i].dossier_id == pid)
      return(true);
  return(false);
}

// Returns true if `key` is valid per the schema. When it starts with
// "toward:", the suffix must parse as a positive integer that is also
// in the participants list.
static bool
fact_key_ok(const char *key, const extract_participant_t *parts, size_t n)
{
  size_t kl = strlen(key);
  const char *tprefix;
  size_t      tlen;

  if(kl == 0 || kl >= MEM_FACT_KEY_SZ)
    return(false);

  tprefix = "toward:";
  tlen = 7;

  if(kl > tlen && memcmp(key, tprefix, tlen) == 0)
  {
    const char *digits = key + tlen;
    char       *endp   = NULL;
    long long   v      = strtoll(digits, &endp, 10);

    if(digits[0] == '\0' || endp == NULL || *endp != '\0' || v <= 0)
      return(false);

    if(!participants_contain(parts, n, (int64_t)v))
      return(false);
  }

  return(true);
}

// Parse response

size_t
extract_parse_response(const char *content, size_t content_len,
    const extract_participant_t *parts, size_t n_parts,
    float min_conf, const char *channel,
    mem_dossier_fact_t *out, size_t out_cap)
{
  size_t n_items;
  size_t n_written;
  time_t now;
  struct json_object *arr;
  struct json_object *root;
  const char *body;
  size_t      blen;

  if(content == NULL || parts == NULL || out == NULL || out_cap == 0
      || n_parts == 0)
    return(0);

  if(content_len == 0)
    content_len = strlen(content);

  body = NULL;
  blen = 0;

  strip_code_fence(content, content_len, &body, &blen);

  if(blen == 0)
  {
    STAT_BUMP(llm_errors);
    return(0);
  }

  root = json_parse_buf(body, blen, "extract");

  if(root == NULL)
  {
    STAT_BUMP(llm_errors);
    return(0);
  }

  arr = json_get_array(root, "facts");

  if(arr == NULL)
  {
    json_object_put(root);
    STAT_BUMP(llm_errors);
    return(0);
  }

  n_items = (size_t)json_object_array_length(arr);
  n_written = 0;
  now = time(NULL);

  for(size_t i = 0; i < n_items && n_written < out_cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, (int)i);

    int64_t pid = 0;
    char    kind_buf[32] = "";
    char    key_buf [MEM_FACT_KEY_SZ]   = "";
    char    val_buf [MEM_FACT_VALUE_SZ] = "";
    double  conf = 0.0;
    mem_dossier_fact_t *f;
    size_t vl;
    mem_fact_kind_t kind;

    if(!json_get_int64 (item, "dossier_id",  &pid)
        || !json_get_str (item, "kind",       kind_buf, sizeof(kind_buf))
        || !json_get_str (item, "fact_key",   key_buf,  sizeof(key_buf))
        || !json_get_str (item, "fact_value", val_buf,  sizeof(val_buf))
        || !json_get_double(item, "confidence", &conf))
    {
      STAT_BUMP(facts_rejected_validation);
      continue;
    }

    if(!kind_from_str(kind_buf, &kind))
    {
      STAT_BUMP(facts_rejected_validation);
      continue;
    }

    if(!participants_contain(parts, n_parts, pid))
    {
      STAT_BUMP(facts_rejected_validation);
      continue;
    }

    if(!fact_key_ok(key_buf, parts, n_parts))
    {
      STAT_BUMP(facts_rejected_validation);
      continue;
    }

    vl = strlen(val_buf);

    if(vl == 0 || vl >= MEM_FACT_VALUE_SZ)
    {
      STAT_BUMP(facts_rejected_validation);
      continue;
    }

    if(conf < 0.0 || conf > 1.0 || (float)conf < min_conf)
    {
      STAT_BUMP(facts_rejected_validation);
      continue;
    }

    f = &out[n_written];
    memset(f, 0, sizeof(*f));
    f->id          = 0;
    f->dossier_id  = pid;
    f->kind        = kind;
    snprintf(f->fact_key,   sizeof(f->fact_key),   "%s", key_buf);
    snprintf(f->fact_value, sizeof(f->fact_value), "%s", val_buf);
    snprintf(f->source,     sizeof(f->source),     "%s", "llm_extract");
    snprintf(f->channel,    sizeof(f->channel),    "%s", channel ? channel : "");
    f->confidence  = (float)conf;
    f->observed_at = now;
    f->last_seen   = now;
    n_written++;
  }

  json_object_put(root);
  return(n_written);
}

// Parse aliases

size_t
extract_parse_aliases(const char *content, size_t content_len,
    const extract_participant_t *parts, size_t n_parts,
    float min_conf,
    extract_alias_t *out, size_t out_cap)
{
  size_t n_items;
  size_t n_written;
  size_t cap;
  struct json_object *arr;
  struct json_object *root;
  const char *body;
  size_t      blen;

  if(content == NULL || parts == NULL || out == NULL || out_cap == 0
      || n_parts == 0)
    return(0);

  if(content_len == 0)
    content_len = strlen(content);

  body = NULL;
  blen = 0;

  strip_code_fence(content, content_len, &body, &blen);

  if(blen == 0)
    return(0);

  root = json_parse_buf(body, blen, "extract_aliases");

  if(root == NULL)
    return(0);

  arr = json_get_array(root, "aliases");

  if(arr == NULL)
  {
    json_object_put(root);
    return(0);
  }

  cap = out_cap < EXTRACT_MAX_ALIASES ? out_cap : EXTRACT_MAX_ALIASES;
  n_items = (size_t)json_object_array_length(arr);
  n_written = 0;

  for(size_t i = 0; i < n_items && n_written < cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(arr, (int)i);

    int64_t pid = 0;
    char    alias_buf[EXTRACT_ALIAS_MAX_LEN + 1] = "";
    double  conf = 0.0;
    size_t  alen;
    bool    form_ok;

    if(!json_get_int64 (item, "dossier_id", &pid)
        || !json_get_str (item, "alias",      alias_buf, sizeof(alias_buf))
        || !json_get_double(item, "confidence", &conf))
      continue;

    if(!participants_contain(parts, n_parts, pid))
      continue;

    alen = strlen(alias_buf);

    if(alen < EXTRACT_ALIAS_MIN_LEN || alen > EXTRACT_ALIAS_MAX_LEN)
      continue;

    form_ok = true;

    for(size_t c = 0; c < alen; c++)
      if(!isalnum((unsigned char)alias_buf[c]))
      {
        form_ok = false;
        break;
      }

    if(!form_ok)
      continue;

    if(conf < 0.0 || conf > 1.0 || (float)conf < min_conf)
      continue;

    memset(&out[n_written], 0, sizeof(out[n_written]));
    out[n_written].dossier_id = pid;
    out[n_written].confidence = (float)conf;
    snprintf(out[n_written].alias, sizeof(out[n_written].alias),
        "%s", alias_buf);
    n_written++;
  }

  json_object_put(root);
  return(n_written);
}

// Alias validator (DB-backed) + synthetic signature builder.

static bool
extract_alias_validate(uint32_t ns_id, int64_t dossier_id,
    const char *alias, const extract_participant_t *parts, size_t n_parts)
{
  db_result_t *r;
  char sql[512];
  size_t alen;
  bool ok;

  (void)parts;
  (void)n_parts;

  if(alias == NULL)
    return(false);

  alen = strlen(alias);

  if(alen < EXTRACT_ALIAS_MIN_LEN || alen > EXTRACT_ALIAS_MAX_LEN)
    return(false);

  for(size_t i = 0; i < alen; i++)
    if(!isalnum((unsigned char)alias[i]))
      return(false);

  // Walk every IRC signature in this namespace whose nickname matches.
  // Alias rows are identifiable by their empty username/hostname/
  // verified_id fields (see how aliases are stored below): they're
  // skipped here so we only flag true regurgitation against real
  // observed nicks, never against another known alias.
  //
  // (alias's safety against being matched as an inbound sender comes
  // from the generic scorer in chat/identity.c — username-match is
  // required for any score above 0, and an alias row carries empty
  // username, so an inbound IRC sighting can never resolve to an alias
  // row even when nicknames coincide.)
  snprintf(sql, sizeof(sql),
      "SELECT ps.dossier_id, ps.nickname"
      " FROM dossier_signature ps"
      " JOIN dossier p ON p.id = ps.dossier_id"
      " WHERE p.ns_id = %u AND ps.method_kind = 'irc'"
      "   AND ps.username <> ''"
      " LIMIT 1024",
      ns_id);

  r = db_result_alloc();
  ok = true;

  if(db_query(sql, r) != SUCCESS || !r->ok)
  {
    db_result_free(r);
    return(false);
  }

  (void)dossier_id;

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *nick = db_result_get(r, i, 1);

    if(nick == NULL || nick[0] == '\0')
      continue;

    // Step (b) self-collision OR step (c) cross-namespace collision:
    // both reject the alias. Matching the target dossier's own real
    // nick catches regurgitation; matching any other dossier's nick
    // catches cross-contamination.
    if(strcasecmp(alias, nick) == 0)
    {
      ok = false;
      break;
    }
  }

  db_result_free(r);
  return(ok);
}

// Dispatch: blocking LLM call + parse + upsert

typedef struct
{
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  bool            done;
  bool            ok;
  long            http_status;
  char           *content;
  size_t          content_len;
  char            err[256];
} extract_wait_t;

static void
dispatch_done_cb(const llm_chat_response_t *resp)
{
  extract_wait_t *w = resp->user_data;

  pthread_mutex_lock(&w->mu);

  w->ok          = resp->ok;
  w->http_status = resp->http_status;

  if(resp->ok && resp->content != NULL && resp->content_len > 0)
  {
    w->content = mem_alloc("chat", "extract_resp", resp->content_len + 1);

    if(w->content != NULL)
    {
      memcpy(w->content, resp->content, resp->content_len);
      w->content[resp->content_len] = '\0';
      w->content_len = resp->content_len;
    }
  }

  if(resp->error != NULL)
    snprintf(w->err, sizeof(w->err), "%s", resp->error);

  w->done = true;
  pthread_cond_broadcast(&w->cv);
  pthread_mutex_unlock(&w->mu);
}

size_t
extract_dispatch(const char *bot_name, uint32_t ns_id,
    const char *model_name,
    const extract_participant_t *parts, size_t n_parts,
    const mem_msg_t *msgs, size_t n_msgs,
    const char *channel, float min_conf, uint32_t timeout_secs)
{
  size_t n_written;
  mem_dossier_fact_t facts[EXTRACT_MAX_FACTS];
  size_t n_parsed;
  bool done;
  uint32_t to;
  struct timespec until;
  llm_message_t messages[2];
  memset(messages, 0, sizeof(messages));
  llm_chat_params_t params;
  extract_wait_t w;
  size_t plen;
  char *prompt;

  if(!extract_ready)
    return(0);

  if(model_name == NULL || model_name[0] == '\0'
      || parts == NULL || msgs == NULL || n_parts == 0 || n_msgs == 0)
    return(0);

  if(n_parts > EXTRACT_MAX_PARTS)
    n_parts = EXTRACT_MAX_PARTS;

  prompt = mem_alloc("chat", "extract_prompt", EXTRACT_PROMPT_MAX_SZ);

  if(prompt == NULL)
    return(0);

  plen = extract_prompt_build(parts, n_parts, msgs, n_msgs,
      prompt, EXTRACT_PROMPT_MAX_SZ);

  if(plen == 0)
  {
    mem_free(prompt);
    return(0);
  }

  memset(&w, 0, sizeof(w));
  pthread_mutex_init(&w.mu, NULL);
  pthread_cond_init(&w.cv, NULL);

  memset(&params, 0, sizeof(params));
  params.temperature  = 0.2f;
  params.max_tokens   = 1024;
  params.stream       = false;

  messages[0].role    = LLM_ROLE_SYSTEM;
  messages[0].content = extract_prompt_system();
  messages[1].role    = LLM_ROLE_USER;
  messages[1].content = prompt;

  STAT_BUMP(llm_calls);

  if(llm_chat_submit(model_name, &params, messages, 2,
        dispatch_done_cb, NULL, &w) != SUCCESS)
  {
    STAT_BUMP(llm_errors);
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    mem_free(prompt);
    return(0);
  }

  // Block until done_cb fires (or timeout).
  to = timeout_secs == 0 ? 60 : timeout_secs;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += to;

  pthread_mutex_lock(&w.mu);
  while(!w.done)
    if(pthread_cond_timedwait(&w.cv, &w.mu, &until) != 0)
      break;
  done = w.done;
  pthread_mutex_unlock(&w.mu);

  mem_free(prompt);

  if(!done || !w.ok || w.content == NULL)
  {
    STAT_BUMP(llm_errors);
    if(!done)
      clam(CLAM_WARN, "extract", "dispatch timeout (%u s)", to);

    mem_free(w.content);
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    return(0);
  }

  n_parsed = extract_parse_response(w.content, w.content_len,
      parts, n_parts, min_conf, channel,
      facts, EXTRACT_MAX_FACTS);

  // Parse aliases from the same response body before freeing it. Gating
  // + validation + upsert happen below; extract_parse_aliases only runs
  // form checks against the JSON and participants list.
  extract_alias_t aliases[EXTRACT_MAX_ALIASES];
  size_t          n_aliases    = 0;
  bool            aliases_on   = false;
  char            alias_key[160];

  if(bot_name != NULL && bot_name[0] != '\0')
  {
    snprintf(alias_key, sizeof(alias_key),
        "bot.%s.behavior.fact_extract.aliases_enabled", bot_name);
    aliases_on = (kv_get_uint(alias_key) != 0);
  }

  if(aliases_on)
  {
    uint32_t pct;
    uint32_t cap;
    float    alias_min_conf;

    snprintf(alias_key, sizeof(alias_key),
        "bot.%s.behavior.fact_extract.aliases_min_conf", bot_name);
    pct = (uint32_t)kv_get_uint(alias_key);
    if(pct > 100) pct = 100;
    alias_min_conf = (float)pct / 100.0f;

    snprintf(alias_key, sizeof(alias_key),
        "bot.%s.behavior.fact_extract.aliases_per_sweep_max", bot_name);
    cap = (uint32_t)kv_get_uint(alias_key);
    if(cap == 0) cap = 3;
    if(cap > EXTRACT_MAX_ALIASES) cap = EXTRACT_MAX_ALIASES;

    n_aliases = extract_parse_aliases(w.content, w.content_len,
        parts, n_parts, alias_min_conf,
        aliases, (size_t)cap);
  }

  mem_free(w.content);
  pthread_mutex_destroy(&w.mu);
  pthread_cond_destroy(&w.cv);

  n_written = 0;

  for(size_t i = 0; i < n_parsed; i++)
    if(memory_upsert_dossier_fact(&facts[i], MEM_MERGE_HIGHER_CONF) == SUCCESS)
      n_written++;

  if(n_written > 0)
  {
    pthread_mutex_lock(&extract_stat_mutex);
    extract_stats.facts_written += (uint64_t)n_written;
    pthread_mutex_unlock(&extract_stat_mutex);
  }

  for(size_t i = 0; i < n_aliases; i++)
  {
    dossier_sig_t  sig;

    if(!extract_alias_validate(ns_id, aliases[i].dossier_id,
                               aliases[i].alias, parts, n_parts))
    {
      STAT_BUMP(aliases_rejected_validation);
      clam(CLAM_INFO, "extract",
          "alias rejected bot=%s dossier_id=%" PRId64 " alias=\"%s\"",
          bot_name, aliases[i].dossier_id, aliases[i].alias);
      continue;
    }

    // Aliases are stored as signature rows whose only populated field
    // is nickname. The empty username/hostname/verified_id fields keep
    // the row out of inbound-sender resolution (the generic scorer
    // requires a username match for any non-zero score) while still
    // letting the token scorer find the dossier when someone says the
    // alias in chat.
    sig.method_kind = "irc";
    sig.nickname    = aliases[i].alias;
    sig.username    = "";
    sig.hostname    = "";
    sig.verified_id = "";

    if(dossier_record_sighting(aliases[i].dossier_id, &sig) == SUCCESS)
    {
      STAT_BUMP(aliases_written);
      clam(CLAM_INFO, "extract",
          "alias learned bot=%s dossier_id=%" PRId64 " alias=\"%s\" conf=%.2f",
          bot_name, aliases[i].dossier_id,
          aliases[i].alias, (double)aliases[i].confidence);
    }
    else
    {
      STAT_BUMP(aliases_rejected_validation);
    }
  }

  return(n_written);
}

// Fetch batch (recent conversation_log rows + participants assembly)

// Parse a JSONB referenced_dossiers array text like "[1,2,3]" into
// int64 ids. Silently drops non-integer entries. Returns count.
static size_t
parse_refs_jsonb(const char *text, int64_t *out, size_t cap)
{
  size_t n;
  size_t w;
  struct json_object *root;

  if(text == NULL || out == NULL || cap == 0)
    return(0);

  root = json_parse_buf(text, strlen(text), "extract_refs");
  if(root == NULL)
    return(0);

  if(!json_object_is_type(root, json_type_array))
  {
    json_object_put(root);
    return(0);
  }

  n = (size_t)json_object_array_length(root);
  w = 0;

  for(size_t i = 0; i < n && w < cap; i++)
  {
    struct json_object *item = json_object_array_get_idx(root, (int)i);
    if(json_object_is_type(item, json_type_int))
      out[w++] = (int64_t)json_object_get_int64(item);
  }

  json_object_put(root);
  return(w);
}

static bool
pid_in(const int64_t *ids, size_t n, int64_t pid)
{
  for(size_t i = 0; i < n; i++)
    if(ids[i] == pid)
      return(true);
  return(false);
}

// Escape a single-quoted SQL literal into out. Truncates on overflow.
static void
sql_escape(const char *in, char *out, size_t cap)
{
  size_t w = 0;

  for(size_t i = 0; in[i] != '\0' && w + 2 < cap; i++)
  {
    if(in[i] == '\'')
    {
      if(w + 3 >= cap) break;
      out[w++] = '\'';
      out[w++] = '\'';
    }

    else
      out[w++] = in[i];
  }

  out[w] = '\0';
}

// Look up display_label for a dossier id. Returns empty string on miss.
static void
dossier_label(int64_t pid, char *out, size_t cap)
{
  db_result_t *r;
  char sql[128];

  out[0] = '\0';
  if(pid <= 0)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT COALESCE(display_label, '') FROM dossier WHERE id = %" PRId64,
      pid);

  r = db_result_alloc();
  if(db_query(sql, r) == SUCCESS && r->ok && r->rows > 0)
  {
    const char *v = db_result_get(r, 0, 0);
    if(v != NULL)
      snprintf(out, cap, "%s", v);
  }
  db_result_free(r);
}

size_t
extract_fetch_batch(const char *bot_name, uint32_t ns_id,
    int64_t hwm_in, uint32_t batch_cap,
    mem_msg_t *msgs_out, size_t msgs_cap,
    extract_participant_t *parts_out, size_t parts_cap,
    size_t *n_parts_out, int64_t *hwm_out)
{
  db_result_t *res;
  size_t n;
  int64_t new_hwm;
  int64_t         pids[EXTRACT_MAX_PARTS];
  extract_role_t  roles[EXTRACT_MAX_PARTS];
  size_t          np;
  size_t          pcap;
  char sql[1024];
  uint32_t cap;
  char esc_bot[MEM_MSG_BOT_SZ * 2 + 4];

  if(n_parts_out != NULL) *n_parts_out = 0;
  if(bot_name == NULL || msgs_out == NULL || parts_out == NULL
      || msgs_cap == 0 || parts_cap == 0 || batch_cap == 0)
    return(0);

  cap = batch_cap < (uint32_t)msgs_cap
      ? batch_cap : (uint32_t)msgs_cap;

  sql_escape(bot_name, esc_bot, sizeof(esc_bot));

  snprintf(sql, sizeof(sql),
      "SELECT id, COALESCE(user_id,0), COALESCE(dossier_id,0),"
      " bot_name, method, channel, kind, text,"
      " EXTRACT(EPOCH FROM ts)::bigint,"
      " COALESCE(referenced_dossiers::text, '[]')"
      " FROM conversation_log"
      " WHERE bot_name = '%s' AND ns_id = %u AND id > %" PRId64
      "   AND dossier_id IS NOT NULL"
      "   AND kind <> %d"
      " ORDER BY id ASC LIMIT %u",
      esc_bot, ns_id, hwm_in, (int)MEM_MSG_EXCHANGE_OUT, cap);

  res = db_result_alloc();
  n = 0;
  new_hwm = hwm_in;

  // Collected unique participant pids, up to parts_cap.
  np = 0;
  pcap = parts_cap < EXTRACT_MAX_PARTS
      ? parts_cap : EXTRACT_MAX_PARTS;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n < msgs_cap; i++)
    {
      mem_msg_t *m = &msgs_out[n];
      const char *v;

      memset(m, 0, sizeof(*m));
      m->ns_id = (int)ns_id;

      v = db_result_get(res, i, 0); if(v) m->id           = strtoll(v, NULL, 10);
      v = db_result_get(res, i, 1); if(v) m->user_id_or_0 = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 2); if(v) m->dossier_id   = strtoll(v, NULL, 10);
      v = db_result_get(res, i, 3); if(v) snprintf(m->bot_name, sizeof(m->bot_name), "%s", v);
      v = db_result_get(res, i, 4); if(v) snprintf(m->method,   sizeof(m->method),   "%s", v);
      v = db_result_get(res, i, 5); if(v) snprintf(m->channel,  sizeof(m->channel),  "%s", v);
      v = db_result_get(res, i, 6); if(v) m->kind = (mem_msg_kind_t)strtol(v, NULL, 10);
      v = db_result_get(res, i, 7); if(v) snprintf(m->text,     sizeof(m->text),     "%s", v);
      v = db_result_get(res, i, 8); if(v) m->ts = (time_t)strtoll(v, NULL, 10);

      v = db_result_get(res, i, 9);
      if(v != NULL)
        m->n_referenced = (uint8_t)parse_refs_jsonb(v,
            m->referenced_dossiers, MEM_MSG_REFS_MAX);

      if(m->id > new_hwm)
        new_hwm = m->id;

      // Sender dossier.
      if(m->dossier_id > 0 && !pid_in(pids, np, m->dossier_id)
          && np < pcap)
      {
        pids [np] = m->dossier_id;
        roles[np] = EXTRACT_ROLE_SENDER;
        np++;
      }

      // Referenced dossiers.
      for(uint8_t k = 0; k < m->n_referenced && np < pcap; k++)
      {
        int64_t rp = m->referenced_dossiers[k];
        if(rp > 0 && !pid_in(pids, np, rp))
        {
          pids [np] = rp;
          roles[np] = EXTRACT_ROLE_MENTIONED;
          np++;
        }
      }

      n++;
    }
  }
  db_result_free(res);

  if(n == 0)
    return(0);

  // Resolve display labels for each participant.
  for(size_t i = 0; i < np; i++)
  {
    memset(&parts_out[i], 0, sizeof(parts_out[i]));
    parts_out[i].dossier_id = pids[i];
    parts_out[i].role       = roles[i];
    dossier_label(pids[i], parts_out[i].display_label,
        sizeof(parts_out[i].display_label));
  }

  if(n_parts_out != NULL) *n_parts_out = np;
  if(hwm_out     != NULL) *hwm_out     = new_hwm;
  return(n);
}

// Schedule + real run_once (rate-limited)

#define EXTRACT_SCHED_BOT_SZ   MEM_MSG_BOT_SZ
#define EXTRACT_RATE_RING_SZ   64

typedef struct extract_sched
{
  char          bot_name[EXTRACT_SCHED_BOT_SZ];
  uint32_t      ns_id;
  bool          active;
  task_handle_t task;

  // Rate-limit: ring buffer of wall-clock seconds when extract_run_once
  // last executed for this bot. Counts entries within the last 3600s.
  uint32_t  samples[EXTRACT_RATE_RING_SZ];
  uint8_t   ring_head;
  uint8_t   ring_count;

  struct extract_sched *next;
} extract_sched_t;

static pthread_mutex_t  extract_sched_mutex = PTHREAD_MUTEX_INITIALIZER;
static extract_sched_t *extract_sched_head  = NULL;

static extract_sched_t *
sched_find_locked(const char *bot_name)
{
  for(extract_sched_t *s = extract_sched_head; s != NULL; s = s->next)
    if(strcmp(s->bot_name, bot_name) == 0)
      return(s);
  return(NULL);
}

// Record a sweep timestamp in the ring; returns true if the sweep
// should proceed (under the rate limit), false if rate-limited.
static bool
rate_check_and_record(extract_sched_t *s, uint32_t max_per_hour)
{
  uint32_t now = (uint32_t)time(NULL);
  uint32_t cutoff = now - 3600;

  // Count samples within the last hour.
  uint32_t in_window = 0;
  for(uint8_t i = 0; i < s->ring_count; i++)
  {
    uint8_t idx = (uint8_t)((s->ring_head + EXTRACT_RATE_RING_SZ - 1 - i)
        % EXTRACT_RATE_RING_SZ);
    if(s->samples[idx] >= cutoff)
      in_window++;
  }

  if(max_per_hour > 0 && in_window >= max_per_hour)
    return(false);

  s->samples[s->ring_head] = now;
  s->ring_head = (uint8_t)((s->ring_head + 1) % EXTRACT_RATE_RING_SZ);
  if(s->ring_count < EXTRACT_RATE_RING_SZ)
    s->ring_count++;
  return(true);
}

size_t
extract_run_once(const char *bot_name, uint32_t ns_id)
{
  const char *channel;
  size_t written;
  const char *cm;
  mem_msg_t             msgs[EXTRACT_MAX_FACTS];
  extract_participant_t parts[EXTRACT_MAX_PARTS];
  size_t                n_parts;
  int64_t               new_hwm;
  size_t n_msgs;
  extract_sched_t *s;
  float min_conf;
  uint32_t max_per_hour;
  int64_t hwm;
  uint32_t batch_cap;
  char key[128];

  if(!extract_ready || bot_name == NULL || bot_name[0] == '\0')
    return(0);

  STAT_BUMP(sweeps_total);

  // Read per-bot KV.

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.enabled", bot_name);
  if(kv_get_uint(key) == 0)
    return(0);

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.batch_cap", bot_name);
  batch_cap = (uint32_t)kv_get_uint(key);
  if(batch_cap == 0) batch_cap = 20;

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.hwm", bot_name);
  hwm = (int64_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.max_per_hour", bot_name);
  max_per_hour = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.min_conf", bot_name);
  min_conf = (float)kv_get_uint(key) / 100.0f;

  // Rate-limit check + record.
  pthread_mutex_lock(&extract_sched_mutex);
  s = sched_find_locked(bot_name);

  if(s != NULL && !rate_check_and_record(s, max_per_hour))
  {
    pthread_mutex_unlock(&extract_sched_mutex);
    STAT_BUMP(sweeps_skipped_rate_limited);
    return(0);
  }
  pthread_mutex_unlock(&extract_sched_mutex);

  // Fetch the batch.
  n_parts = 0;
  new_hwm = hwm;

  n_msgs = extract_fetch_batch(bot_name, ns_id, hwm, batch_cap,
      msgs, sizeof(msgs)/sizeof(msgs[0]),
      parts, sizeof(parts)/sizeof(parts[0]),
      &n_parts, &new_hwm);

  if(n_msgs == 0 || n_parts == 0)
    return(0);

  // Chat model lookup.
  snprintf(key, sizeof(key), "bot.%s.chat_model", bot_name);
  cm = kv_get_str(key);
  if(cm == NULL || cm[0] == '\0')
    cm = kv_get_str("llm.default_chat_model");

  if(cm == NULL || cm[0] == '\0')
  {
    clam(CLAM_WARN, "extract", "no chat model for bot '%s'", bot_name);
    return(0);
  }

  channel = msgs[0].channel;

  written = extract_dispatch(bot_name, ns_id, cm,
      parts, n_parts, msgs, n_msgs,
      channel, min_conf, 0);

  // Advance the high-water mark on any successful dispatch so the same
  // rows aren't reprocessed every tick. On hard failure (written == 0
  // and dispatch saw an error), we still advance: reprocessing a known-
  // bad batch just wastes LLM calls.
  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.hwm", bot_name);
  (void)kv_set_uint(key, (uint64_t)new_hwm);

  return(written);
}

static void
extract_sweep_task_cb(task_t *t)
{
  extract_sched_t *s = t->data;
  bool     active;
  char     bot_name[EXTRACT_SCHED_BOT_SZ];
  uint32_t ns_id;

  if(s == NULL)
    return;

  pthread_mutex_lock(&extract_sched_mutex);
  active = s->active;
  ns_id = s->ns_id;
  snprintf(bot_name, sizeof(bot_name), "%s", s->bot_name);
  pthread_mutex_unlock(&extract_sched_mutex);

  if(active && extract_ready)
    (void)extract_run_once(bot_name, ns_id);

  // Leaving the task alive on inactive so we don't wrestle with the
  // task system's lifecycle. Returning without TASK_ENDED here ends
  // this iteration; periodic machinery reschedules unconditionally.
  // To fully stop, unregister and set active=false; the callback then
  // no-ops every tick until the bot is re-scheduled (cheap).
  t->state = TASK_ENDED;
}

void
extract_schedule(const char *bot_name, uint32_t ns_id,
    uint32_t interval_secs)
{
  bool need_task;
  extract_sched_t *s;

  if(!extract_ready || bot_name == NULL || bot_name[0] == '\0')
    return;

  if(interval_secs == 0)
    interval_secs = 300;

  pthread_mutex_lock(&extract_sched_mutex);
  s = sched_find_locked(bot_name);

  if(s == NULL)
  {
    s = mem_alloc("chat", "extract_state", sizeof(*s));
    if(s == NULL)
    {
      pthread_mutex_unlock(&extract_sched_mutex);
      return;
    }
    snprintf(s->bot_name, sizeof(s->bot_name), "%s", bot_name);
    s->next            = extract_sched_head;
    extract_sched_head = s;
  }

  s->ns_id  = ns_id;
  s->active = true;

  // If a task is already running for this bot, let it continue; we've
  // replaced the data it reads. Otherwise, spawn one.
  need_task = (s->task == TASK_HANDLE_NONE);
  pthread_mutex_unlock(&extract_sched_mutex);

  if(need_task)
  {
    char tname[64];
    task_handle_t t;

    snprintf(tname, sizeof(tname), "extract.%.32s", bot_name);

    t = task_add_periodic(tname, TASK_ANY, 200,
        interval_secs * 1000, extract_sweep_task_cb, s);

    pthread_mutex_lock(&extract_sched_mutex);
    s->task = t;
    pthread_mutex_unlock(&extract_sched_mutex);

    clam(CLAM_INFO, "extract",
        "scheduled sweep for bot '%s' every %u s", bot_name, interval_secs);
  }
}

void
extract_unschedule(const char *bot_name)
{
  extract_sched_t *s;

  if(bot_name == NULL)
    return;

  pthread_mutex_lock(&extract_sched_mutex);
  s = sched_find_locked(bot_name);

  if(s != NULL)
  {
    s->active = false;
    // Keep the slot + task around: task_cb sees active==false and no-ops.
    // Cleanup on process shutdown.
  }
  pthread_mutex_unlock(&extract_sched_mutex);

  if(s != NULL)
    clam(CLAM_INFO, "extract", "unscheduled sweep for bot '%s'", bot_name);
}

// Lifecycle

void
extract_init(void)
{
  if(extract_ready)
    return;

  pthread_mutex_init(&extract_stat_mutex, NULL);
  memset(&extract_stats, 0, sizeof(extract_stats));

  extract_ready = true;
  clam(CLAM_INFO, "extract", "extract subsystem initialized");
}

void
extract_register_config(void)
{
  // No subsystem-level KV in F-2; per-bot keys live in chatbot_inst_schema.
}

void
extract_exit(void)
{
  extract_sched_t *s;

  if(!extract_ready)
    return;

  extract_ready = false;

  // Mark all scheduled sweeps inactive and free the list. The periodic
  // tasks themselves are joined by the task system during shutdown.
  pthread_mutex_lock(&extract_sched_mutex);
  s = extract_sched_head;
  while(s != NULL)
  {
    extract_sched_t *n = s->next;
    s->active = false;
    mem_free(s);
    s = n;
  }
  extract_sched_head = NULL;
  pthread_mutex_unlock(&extract_sched_mutex);

  pthread_mutex_destroy(&extract_stat_mutex);

  clam(CLAM_INFO, "extract", "extract subsystem shut down");
}
