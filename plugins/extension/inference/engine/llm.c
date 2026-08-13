// botmanager — MIT
// LLM client: request builder, streaming-response dispatch, personality wiring.
#include "llm_priv.h"

#include "cmd.h"
#include "curl.h"
#include "db.h"
#include "json.h"
#include "plugin.h"
#include "task.h"
#include "userns.h"
#include "util.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Module state

static bool             llm_ready = false;
static bool             llm_stopping = false;
llm_cfg_t               llm_cfg;

// Freelist.
static llm_request_t   *llm_req_free = NULL;
static pthread_mutex_t  llm_req_mutex;

// In-flight request list (for /show llm).
static llm_request_t   *llm_active_head = NULL;
static pthread_mutex_t  llm_active_mutex;

// Model cache.
llm_model_t            *llm_models_head = NULL;
pthread_rwlock_t        llm_models_lock;
llm_service_t          *llm_services_head = NULL;
pthread_rwlock_t        llm_services_lock;

// Learned per-model request-dialect directives (LLM-DIALECT-1). Keyed by
// (service_name, model_id); a tiny linked list mirroring llm_model_params.
typedef struct llm_model_params
{
  char             service_name[LLM_MODEL_NAME_SZ];
  char             model_id[LLM_MODEL_ID_SZ];
  llm_directive_t  directives[LLM_MAX_DIRECTIVES];
  uint32_t         n_directives;
  struct llm_model_params *next;
} llm_model_params_t;

static llm_model_params_t *llm_model_params_head = NULL;
static pthread_rwlock_t    llm_model_params_lock;

#ifdef LLM_TEST_HOOKS
// Test-only: slot holding the next canned chat-completion content. When
// non-NULL, llm_chat_submit fires done_cb synchronously with this text
// instead of hitting the network. Cleared on consume. Owned as a
// malloc'd copy of the caller's string.
static char            *llm_test_pending = NULL;
static pthread_mutex_t  llm_test_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

// Stats (protected by llm_stat_mutex; counters updated atomically-ish).
static pthread_mutex_t  llm_stat_mutex;
static uint64_t         llm_stat_total    = 0;
static uint64_t         llm_stat_retries  = 0;
static uint64_t         llm_stat_errors   = 0;
static uint64_t         llm_stat_prompt   = 0;
static uint64_t         llm_stat_compl    = 0;
static uint64_t         llm_stat_time_ms  = 0;
static uint32_t         llm_active_count  = 0;
static uint32_t         llm_queued_count  = 0;

// Small utilities

bool
llm_kind_from_str(const char *s, llm_kind_t *out)
{
  if(strcmp(s, "chat") == 0)   { *out = LLM_KIND_CHAT;  return(SUCCESS); }
  if(strcmp(s, "embed") == 0)  { *out = LLM_KIND_EMBED; return(SUCCESS); }
  if(strcmp(s, "image") == 0)  { *out = LLM_KIND_IMAGE; return(SUCCESS); }
  if(strcmp(s, "stt") == 0)    { *out = LLM_KIND_STT;   return(SUCCESS); }
  if(strcmp(s, "tts") == 0)    { *out = LLM_KIND_TTS;   return(SUCCESS); }
  return(FAIL);
}

// Real switch, not a ternary: a fall-through default would silently
// mislabel any new kind (the SUCCESS/FAIL-style silent-inversion hazard).
const char *
llm_kind_to_str(llm_kind_t k)
{
  switch(k)
  {
    case LLM_KIND_CHAT:  return("chat");
    case LLM_KIND_EMBED: return("embed");
    case LLM_KIND_IMAGE: return("image");
    case LLM_KIND_STT:   return("stt");
    case LLM_KIND_TTS:   return("tts");
  }

  return("chat");
}

// JSON writer: minimal growable string builder for request bodies

typedef struct
{
  char  *buf;
  size_t len;
  size_t cap;
} llm_buf_t;

static void
llm_buf_init(llm_buf_t *b, size_t init_cap)
{
  b->cap = init_cap > 0 ? init_cap : 256;
  b->buf = mem_alloc("llm", "json_buf", b->cap);
  b->buf[0] = '\0';
  b->len = 0;
}

static void
llm_buf_reserve(llm_buf_t *b, size_t extra)
{
  size_t needed = b->len + extra + 1;

  size_t newcap;
  if(needed <= b->cap)
    return;

  newcap = b->cap;

  while(newcap < needed)
    newcap *= 2;

  b->buf = mem_realloc(b->buf, newcap);
  b->cap = newcap;
}

static void
llm_buf_append(llm_buf_t *b, const char *s, size_t n)
{
  llm_buf_reserve(b, n);
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static void
llm_buf_putc(llm_buf_t *b, char c)
{
  llm_buf_append(b, &c, 1);
}

static void
llm_buf_puts(llm_buf_t *b, const char *s)
{
  llm_buf_append(b, s, strlen(s));
}

static void
llm_buf_printf(llm_buf_t *b, const char *fmt, ...)
{
  char tmp[128];
  va_list ap;

  int n;
  va_start(ap, fmt);
  n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  if(n < 0)
    return;

  if((size_t)n < sizeof(tmp))
  {
    llm_buf_append(b, tmp, (size_t)n);
    return;
  }

  llm_buf_reserve(b, (size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

// Write a JSON string literal: "escaped(s)". Uses json_escape (core/json.c)
// with a two-pass size/write into the growable llm_buf_t.
static void
llm_json_str(llm_buf_t *b, const char *s)
{
  size_t need;
  llm_buf_putc(b, '"');

  need = json_escape(s, NULL, 0);
  llm_buf_reserve(b, need + 1);
  json_escape(s, b->buf + b->len, b->cap - b->len);
  b->len += need;

  llm_buf_putc(b, '"');
}

// JSON scanner: tiny ad-hoc reader for the fields we need

// Extract the contents of a JSON string value starting at p (which must
// point at the opening quote). out is populated with the unescaped bytes,
// NUL-terminated. out_cap must be at least (value_span_bytes + 1). Returns
// a pointer just past the closing quote, or NULL on malformed input.
static const char *
llm_read_string(const char *p, const char *end, char *out, size_t out_cap)
{
  const char *start;
  size_t raw_len;
  if(p >= end || *p != '"')
    return(NULL);

  p++;
  start = p;

  while(p < end && *p != '"')
  {
    if(*p == '\\' && p + 1 < end)
      p += 2;
    else
      p++;
  }

  if(p >= end)
    return(NULL);

  raw_len = (size_t)(p - start);

  if(raw_len + 1 > out_cap)
    raw_len = out_cap - 1;

  json_unescape(start, raw_len, out);

  return(p + 1);
}

// Find a JSON key within a buffer and read its string value. Returns
// bytes written (not counting NUL), or -1 if not found / malformed.
// key_literal must include surrounding double quotes, e.g. "\"content\"".
static ssize_t
llm_extract_str(const char *buf, size_t len, const char *key_literal,
    char *out, size_t out_cap)
{
  const char *p = util_memstr(buf, len, key_literal);

  const char *after;
  if(p == NULL)
    return(-1);

  p += strlen(key_literal);
  p = util_skip_to_value(p, buf + len);

  if(p == NULL || *p != '"')
    return(-1);

  after = llm_read_string(p, buf + len, out, out_cap);

  if(after == NULL)
    return(-1);

  return((ssize_t)strlen(out));
}

// Find a JSON integer key. Returns SUCCESS or FAIL.
static bool
llm_extract_int(const char *buf, size_t len, const char *key_literal,
    long *out)
{
  const char *p = util_memstr(buf, len, key_literal);

  const char *after;
  if(p == NULL)
    return(FAIL);

  p += strlen(key_literal);
  p = util_skip_to_value(p, buf + len);

  if(p == NULL)
    return(FAIL);

  after = util_read_int(p, buf + len, out);

  return(after == NULL ? FAIL : SUCCESS);
}

// Request freelist + in-flight list

static llm_request_t *
llm_req_alloc(void)
{
  llm_request_t *req = NULL;

  pthread_mutex_lock(&llm_req_mutex);

  if(llm_req_free != NULL)
  {
    req = llm_req_free;
    llm_req_free = req->next_free;
  }

  pthread_mutex_unlock(&llm_req_mutex);

  if(req == NULL)
    req = mem_alloc("llm", "request", sizeof(*req));

  memset(req, 0, sizeof(*req));

  return(req);
}

static void
llm_req_release(llm_request_t *req)
{
  if(req->body_prefix != NULL)  { mem_free(req->body_prefix); req->body_prefix = NULL; }
  if(req->req_body != NULL)     { mem_free(req->req_body); req->req_body = NULL; }
  if(req->assembled != NULL)    { mem_free(req->assembled); req->assembled = NULL; }
  if(req->vec_block != NULL)    { mem_free(req->vec_block); req->vec_block = NULL; }
  if(req->vectors != NULL)      { mem_free((void *)req->vectors); req->vectors = NULL; }
  if(req->sse_parser != NULL)   { sse_parser_free(req->sse_parser); req->sse_parser = NULL; }

  req->body_prefix_len = 0;
  req->req_body_len = 0;
  req->assembled_len = 0;
  req->assembled_cap = 0;
  req->vec_block_len = 0;
  req->n_vectors = 0;
  req->vectors_dim = 0;

  pthread_mutex_lock(&llm_req_mutex);
  req->next_free = llm_req_free;
  llm_req_free = req;
  pthread_mutex_unlock(&llm_req_mutex);
}

static void
llm_active_add(llm_request_t *req)
{
  pthread_mutex_lock(&llm_active_mutex);
  req->in_flight = true;
  req->next_active = llm_active_head;
  llm_active_head = req;
  llm_active_count++;
  pthread_mutex_unlock(&llm_active_mutex);
}

static void
llm_active_remove(llm_request_t *req)
{
  pthread_mutex_lock(&llm_active_mutex);

  if(req->in_flight)
  {
    llm_request_t **pp = &llm_active_head;

    while(*pp != NULL && *pp != req)
      pp = &(*pp)->next_active;

    if(*pp == req)
    {
      *pp = req->next_active;
      llm_active_count--;
    }

    req->in_flight = false;
    req->next_active = NULL;
  }

  pthread_mutex_unlock(&llm_active_mutex);
}

static void
llm_assembled_append(llm_request_t *req, const char *s, size_t n)
{
  size_t needed = req->assembled_len + n + 1;

  if(needed > req->assembled_cap)
  {
    size_t newcap = req->assembled_cap;

    if(newcap == 0)
      newcap = LLM_ASSEMBLED_INIT_CAP;

    while(newcap < needed)
      newcap *= 2;

    if(req->assembled == NULL)
      req->assembled = mem_alloc("llm", "assembled", newcap);
    else
      req->assembled = mem_realloc(req->assembled, newcap);
    req->assembled_cap = newcap;
  }

  memcpy(req->assembled + req->assembled_len, s, n);
  req->assembled_len += n;
  req->assembled[req->assembled_len] = '\0';
}

// Model cache

// Caller must not hold the lock.
static void
llm_models_clear(void)
{
  llm_model_t *m;
  pthread_rwlock_wrlock(&llm_models_lock);

  m = llm_models_head;
  llm_models_head = NULL;

  pthread_rwlock_unlock(&llm_models_lock);

  while(m != NULL)
  {
    llm_model_t *next = m->next;
    mem_free(m);
    m = next;
  }
}

// Insert or replace (by name). Takes write lock.
static void
llm_models_upsert(const llm_model_t *src)
{
  llm_model_t *entry = mem_alloc("llm", "model", sizeof(*entry));
  llm_model_t **pp;
  memcpy(entry, src, sizeof(*entry));

  pthread_rwlock_wrlock(&llm_models_lock);

  pp = &llm_models_head;

  while(*pp != NULL)
  {
    if(strcmp((*pp)->name, src->name) == 0)
    {
      llm_model_t *old = *pp;
      entry->next = old->next;
      *pp = entry;
      pthread_rwlock_unlock(&llm_models_lock);
      mem_free(old);
      return;
    }

    pp = &(*pp)->next;
  }

  entry->next = NULL;
  *pp = entry;

  pthread_rwlock_unlock(&llm_models_lock);
}

// Snapshot a model by name into out. Returns SUCCESS or FAIL.
static bool
llm_models_snapshot(const char *name, llm_model_t *out)
{
  bool ok = FAIL;

  pthread_rwlock_rdlock(&llm_models_lock);

  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0)
    {
      memcpy(out, m, sizeof(*out));
      out->next = NULL;
      ok = SUCCESS;
      break;
    }
  }

  pthread_rwlock_unlock(&llm_models_lock);
  return(ok);
}

// Service cache
//
// Mirror of llm_services: name → base_url. Reloaded from DB alongside the
// model cache (and always before it, so model rows can resolve their
// base_url via the join). Each service's provider-token KV slot,
// llm.service.<name>.creds.apikey, is registered here.

// Caller must not hold the lock.
static void
llm_services_clear(void)
{
  llm_service_t *s;
  pthread_rwlock_wrlock(&llm_services_lock);

  s = llm_services_head;
  llm_services_head = NULL;

  pthread_rwlock_unlock(&llm_services_lock);

  while(s != NULL)
  {
    llm_service_t *next = s->next;
    mem_free(s);
    s = next;
  }
}

// Insert or replace (by name). Takes write lock.
static void
llm_services_upsert(const llm_service_t *src)
{
  llm_service_t  *entry = mem_alloc("llm", "service", sizeof(*entry));
  llm_service_t **pp;
  memcpy(entry, src, sizeof(*entry));

  pthread_rwlock_wrlock(&llm_services_lock);

  pp = &llm_services_head;

  while(*pp != NULL)
  {
    if(strcmp((*pp)->name, src->name) == 0)
    {
      llm_service_t *old = *pp;
      entry->next = old->next;
      *pp = entry;
      pthread_rwlock_unlock(&llm_services_lock);
      mem_free(old);
      return;
    }

    pp = &(*pp)->next;
  }

  entry->next = NULL;
  *pp = entry;

  pthread_rwlock_unlock(&llm_services_lock);
}

bool
llm_service_base_url(const char *name, char *out, size_t out_sz)
{
  bool ok = FAIL;
  if(name == NULL || out == NULL || out_sz == 0)
    return(FAIL);

  pthread_rwlock_rdlock(&llm_services_lock);

  for(llm_service_t *s = llm_services_head; s != NULL; s = s->next)
  {
    if(strcmp(s->name, name) == 0)
    {
      snprintf(out, out_sz, "%s", s->base_url);
      ok = SUCCESS;
      break;
    }
  }

  pthread_rwlock_unlock(&llm_services_lock);
  return(ok);
}

bool
llm_build_url(const char *base, const char *op, char *out, size_t out_sz)
{
  size_t base_len;
  int    n;
  if(base == NULL || op == NULL || out == NULL || out_sz == 0
      || base[0] == '\0' || op[0] == '\0')
    return(FAIL);

  base_len = strlen(base);

  // Trim exactly one trailing '/' so "base/" + "op" never doubles it.
  if(base[base_len - 1] == '/')
    base_len--;

  n = snprintf(out, out_sz, "%.*s/%s", (int)base_len, base, op);

  if(n < 0 || (size_t)n >= out_sz)
    return(FAIL);

  return(SUCCESS);
}

// Reload the service cache from DB and (re)register each service's
// API-token KV slot. Must run before llm_models_reload().
void
llm_services_reload(void)
{
  db_result_t *res;
  llm_services_clear();

  res = db_result_alloc();

  if(db_query("SELECT name, base_url FROM llm_services", res) != SUCCESS
      || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "llm", "service cache reload: %s", res->error);

    db_result_free(res);
    return;
  }

  for(uint32_t r = 0; r < res->rows; r++)
  {
    llm_service_t s;
    const char   *name;
    const char   *base;
    char          key[LLM_KV_KEY_SZ];
    memset(&s, 0, sizeof(s));

    name = db_result_get(res, r, 0);
    base = db_result_get(res, r, 1);

    if(name == NULL || name[0] == '\0')
      continue;

    snprintf(s.name, sizeof(s.name), "%s", name);
    snprintf(s.base_url, sizeof(s.base_url), "%s", base ? base : "");

    // Register the provider-token KV slot named after the service so an
    // operator can `set kv llm.service.<name>.creds.apikey <token>` (kv_set
    // rejects unregistered keys). kv_claim_orphans() (core init) has
    // already rehydrated any persisted token; the kv_exists guard means
    // we only ever create an empty slot, never clobber a live value.
    snprintf(key, sizeof(key), "llm.service.%s.creds.apikey", s.name);

    // Attach the change hook whether we create the slot here or it was
    // rehydrated by kv_claim_orphans (core init) before this reload — so
    // setting the key later always re-probes /models (llm_apikey_kv_cb).
    if(!kv_exists(key))
      kv_register(key, KV_STR, "", llm_apikey_kv_cb, NULL,
          "LLM provider API token sent as 'Authorization: Bearer'."
          " Keyed by service name (llm.service.<name>.creds.apikey).");
    else
      kv_set_cb(key, llm_apikey_kv_cb, NULL);

    // Per-model request-dialect quirks (max_tokens rename, temperature drop)
    // are no longer per-service KVs: they are learned per (service, model_id)
    // and stored in llm_model_params. See LLM-DIALECT-1.

    // Optional per-service 'reasoning_effort' knob (RSN-1). Thinking-first
    // providers (e.g. Gemini) bill hidden reasoning against max_tokens; set
    // this to 'none' so a terse !ask budget funds the answer, not the
    // reasoning. Empty = omit the field (byte-identical request as before).
    snprintf(key, sizeof(key), "llm.service.%s.reasoning_effort", s.name);

    if(!kv_exists(key))
      kv_register(key, KV_STR, "", NULL, NULL,
          "Optional OpenAI-compat 'reasoning_effort' sent on every chat"
          " request to this service (none|low|medium|high). Empty = omit."
          " Set 'none' for thinking models (e.g. Gemini) so the token"
          " budget funds the answer, not hidden reasoning.");

    llm_services_upsert(&s);
  }

  db_result_free(res);
}

// Reload the cache from DB. Clears and rebuilds. Safe to call repeatedly.
void
llm_models_reload(void)
{
  db_result_t *res;
  llm_models_clear();

  res = db_result_alloc();

  if(db_query(
      "SELECT m.name, m.kind, m.service_name, s.base_url, m.model_id, "
      "m.embed_dim, m.max_context, m.default_temp, m.enabled "
      "FROM llm_models m JOIN llm_services s ON s.name = m.service_name",
      res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "llm", "model cache reload: %s", res->error);

    db_result_free(res);
    return;
  }

  for(uint32_t r = 0; r < res->rows; r++)
  {
    llm_model_t m;
    const char *name;
    const char *kind;
    const char *svc;
    const char *base;
    const char *mid;
    const char *dim;
    const char *maxctx;
    const char *temp;
    const char *enabled;
    memset(&m, 0, sizeof(m));

    name = db_result_get(res, r, 0);
    kind = db_result_get(res, r, 1);
    svc = db_result_get(res, r, 2);
    base = db_result_get(res, r, 3);
    mid = db_result_get(res, r, 4);
    dim = db_result_get(res, r, 5);
    maxctx = db_result_get(res, r, 6);
    temp = db_result_get(res, r, 7);
    enabled = db_result_get(res, r, 8);

    if(name == NULL || kind == NULL)
      continue;

    snprintf(m.name, sizeof(m.name), "%s", name);
    snprintf(m.service_name, sizeof(m.service_name), "%s", svc ? svc : "");
    snprintf(m.base_url, sizeof(m.base_url), "%s", base ? base : "");
    snprintf(m.model_id, sizeof(m.model_id), "%s", mid ? mid : "");

    // A kind this binary does not know is a row written by a NEWER one
    // (the DB outlives any single build). Dropping it silently would
    // present as a model that simply stopped existing, so name it.
    if(llm_kind_from_str(kind, &m.kind) != SUCCESS)
    {
      clam(CLAM_WARN, "llm", "model %s: unknown kind '%s' — row ignored",
          name, kind);
      continue;
    }

    m.embed_dim    = dim    ? (uint32_t)strtoul(dim, NULL, 10) : 0;
    // Fall back to the KV default (llm.max_context_tokens) rather than
    // the compiled-in constant so operators have a single place to set
    // the starting value for any model whose DB column ends up NULL.
    m.max_context  = maxctx && maxctx[0] != '\0'
                     ? (uint32_t)strtoul(maxctx, NULL, 10)
                     : llm_cfg.max_context_tokens;
    m.default_temp = temp   ? strtof(temp, NULL) : 0.7f;
    m.enabled      = enabled && (enabled[0] == 't' || enabled[0] == 'T'
                                 || enabled[0] == '1');

    llm_models_upsert(&m);
  }

  db_result_free(res);
}

// Ensure the LLM registry tables exist (idempotent). Two-tier model:
// a service is one OpenAI-compatible provider (one base URL, one key);
// a model references a service by name and adds the real model_id;
// llm_service_models caches each service's discovered /models list.
static void
llm_ensure_tables(void)
{
  static const char *const ddl[] = {
      "CREATE TABLE IF NOT EXISTS llm_services ("
      " name       VARCHAR(64)  PRIMARY KEY,"
      " base_url   TEXT         NOT NULL,"
      " created    TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " refreshed  TIMESTAMPTZ,"
      " probe_http INTEGER"          // last /models probe HTTP status; NULL=never
      ")",

      "CREATE TABLE IF NOT EXISTS llm_service_models ("
      " service_name  VARCHAR(64)  NOT NULL"
      "               REFERENCES llm_services(name) ON DELETE CASCADE,"
      " model_id      VARCHAR(128) NOT NULL,"
      " max_model_len INTEGER,"
      " fetched       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " PRIMARY KEY (service_name, model_id)"
      ")",

      "CREATE TABLE IF NOT EXISTS llm_models ("
      " name          VARCHAR(64)  PRIMARY KEY,"
      " kind          VARCHAR(16)  NOT NULL,"
      " service_name  VARCHAR(64)  NOT NULL REFERENCES llm_services(name),"
      " model_id      VARCHAR(128) NOT NULL,"
      " embed_dim     INTEGER      NOT NULL DEFAULT 0,"
      " max_context   INTEGER      NOT NULL DEFAULT 8192,"
      " default_temp  REAL         NOT NULL DEFAULT 0.7,"
      " enabled       BOOLEAN      NOT NULL DEFAULT TRUE,"
      " created       TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")",

      // Learned request-dialect directives, keyed by (service, model_id) so
      // it works whether or not /models was ever probed. See LLM-DIALECT-1.
      "CREATE TABLE IF NOT EXISTS llm_model_params ("
      " service_name  VARCHAR(64)  NOT NULL"
      "               REFERENCES llm_services(name) ON DELETE CASCADE,"
      " model_id      VARCHAR(128) NOT NULL,"
      " field         VARCHAR(64)  NOT NULL,"   // canonical builder field
      " action        VARCHAR(16)  NOT NULL,"   // 'rename' | 'drop'
      " replacement   VARCHAR(64),"             // wire name for 'rename', else NULL
      " learned       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " PRIMARY KEY (service_name, model_id, field)"
      ")"
  };

  for(size_t i = 0; i < sizeof(ddl) / sizeof(ddl[0]); i++)
  {
    db_result_t *res = db_result_alloc();

    if(db_query(ddl[i], res) != SUCCESS || !res->ok)
    {
      if(res->error[0] != '\0')
        clam(CLAM_WARN, "llm", "ensure_tables: %s", res->error);
    }

    db_result_free(res);
  }
}

// Model registry public API

bool
llm_model_exists(const char *name)
{
  bool found;
  if(name == NULL)
    return(false);

  found = false;

  pthread_rwlock_rdlock(&llm_models_lock);

  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0 && m->enabled)
    {
      found = true;
      break;
    }
  }

  pthread_rwlock_unlock(&llm_models_lock);
  return(found);
}

bool
llm_model_kind(const char *name, llm_kind_t *out)
{
  bool ok;
  if(name == NULL || out == NULL)
    return(FAIL);

  ok = FAIL;

  pthread_rwlock_rdlock(&llm_models_lock);

  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0)
    {
      *out = m->kind;
      ok = SUCCESS;
      break;
    }
  }

  pthread_rwlock_unlock(&llm_models_lock);
  return(ok);
}

uint32_t
llm_model_embed_dim(const char *name)
{
  uint32_t dim;
  if(name == NULL)
    return(0);

  dim = 0;

  pthread_rwlock_rdlock(&llm_models_lock);

  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0 && m->kind == LLM_KIND_EMBED)
    {
      dim = m->embed_dim;
      break;
    }
  }

  pthread_rwlock_unlock(&llm_models_lock);
  return(dim);
}

void
llm_model_iterate(llm_model_iter_cb_t cb, void *user)
{
  if(cb == NULL)
    return;

  pthread_rwlock_rdlock(&llm_models_lock);

  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
    cb(m->name, m->kind, m->service_name, m->model_id, m->embed_dim,
       m->max_context, m->default_temp, m->enabled, user);

  pthread_rwlock_unlock(&llm_models_lock);
}

// Test hooks

#ifdef LLM_TEST_HOOKS

static bool llm_parse_embed_response(llm_request_t *req, const char *body,
    size_t len);

bool
llm_test_register_model(const char *name, llm_kind_t kind,
    const char *endpoint_url, const char *model_id, uint32_t embed_dim)
{
  if(name == NULL || endpoint_url == NULL || model_id == NULL)
    return(FAIL);

  llm_model_t m;
  memset(&m, 0, sizeof(m));

  // Test models bypass the service registry: the endpoint is treated as
  // a base URL and pinned to a synthetic "test" service name.
  snprintf(m.name, sizeof(m.name), "%s", name);
  snprintf(m.service_name, sizeof(m.service_name), "%s", "test");
  snprintf(m.base_url, sizeof(m.base_url), "%s", endpoint_url);
  snprintf(m.model_id, sizeof(m.model_id), "%s", model_id);

  m.kind         = kind;
  m.embed_dim    = embed_dim;
  m.max_context  = LLM_DEF_MAX_CONTEXT;
  m.default_temp = 0.7f;
  m.enabled      = true;

  llm_models_upsert(&m);
  return(SUCCESS);
}

bool
llm_test_inject_response(const char *content)
{
  if(content == NULL)
    return(FAIL);

  pthread_mutex_lock(&llm_test_pending_mutex);

  if(llm_test_pending != NULL)
  {
    pthread_mutex_unlock(&llm_test_pending_mutex);
    return(FAIL);
  }

  size_t n   = strlen(content);
  char  *dup = mem_alloc("llm", "test_pending", n + 1);

  memcpy(dup, content, n + 1);
  llm_test_pending = dup;

  pthread_mutex_unlock(&llm_test_pending_mutex);
  return(SUCCESS);
}

void
llm_test_clear_models(void)
{
  llm_models_clear();
}

// Drive llm_parse_embed_response directly against a canned body. Caller
// owns out_block / out_vectors via mem_free (matches internal ownership).
bool
llm_test_parse_embed(const char *body, size_t len,
    float **out_block, size_t *out_block_len,
    uint32_t *out_n_vecs, uint32_t *out_dim)
{
  llm_request_t req;
  memset(&req, 0, sizeof(req));

  bool rc = llm_parse_embed_response(&req, body, len);

  if(rc == FAIL)
  {
    if(req.vec_block != NULL)  mem_free(req.vec_block);
    if(req.vectors   != NULL)  mem_free((void *)req.vectors);
    return(FAIL);
  }

  if(out_block     != NULL) *out_block     = req.vec_block;
  else if(req.vec_block != NULL) mem_free(req.vec_block);

  if(req.vectors   != NULL) mem_free((void *)req.vectors);

  if(out_block_len != NULL) *out_block_len = req.vec_block_len;
  if(out_n_vecs    != NULL) *out_n_vecs    = (uint32_t)req.n_vectors;
  if(out_dim       != NULL) *out_dim       = req.vectors_dim;
  return(SUCCESS);
}

#endif // LLM_TEST_HOOKS

// Role serialization

static const char *
llm_role_str(llm_role_t r)
{
  switch(r)
  {
    case LLM_ROLE_SYSTEM:    return("system");
    case LLM_ROLE_USER:      return("user");
    case LLM_ROLE_ASSISTANT: return("assistant");
    default:                 return("user");
  }
}

// Prompt tracing (CLAM_DEBUG5)

// One clam() line is capped at CLAM_MSG_SZ (~1KB, silently truncated),
// but system prompts routinely run to tens of KB. Chunk the payload so
// every byte actually appears in the log; pick a payload slice small
// enough that the "role=... part=N/M" preamble stays well clear of the
// msg buffer.
#define LLM_PROMPT_LOG_CHUNK   800

static void
llm_clam_prompt_content(const char *what, size_t idx, size_t n,
    const char *role, const char *content)
{
  size_t len = (content == NULL) ? 0 : strlen(content);

  size_t total;
  size_t off;
  if(len == 0)
  {
    clam(CLAM_DEBUG5, "llm",
        "prompt %s msg[%zu/%zu] role=%s len=0 (empty)",
        what, idx, n, role);
    return;
  }

  total = (len + LLM_PROMPT_LOG_CHUNK - 1) / LLM_PROMPT_LOG_CHUNK;
  off = 0;

  for(size_t part = 1; part <= total; part++)
  {
    size_t take = len - off;

    if(take > LLM_PROMPT_LOG_CHUNK)
      take = LLM_PROMPT_LOG_CHUNK;

    clam(CLAM_DEBUG5, "llm",
        "prompt %s msg[%zu/%zu] role=%s part=%zu/%zu len=%zu: %.*s",
        what, idx, n, role, part, total, len,
        (int)take, content + off);
    off += take;
  }
}

static void
llm_clam_prompt_chat(const char *model_name,
    const llm_chat_params_t *params,
    const llm_message_t *msgs, size_t n_msgs)
{
  clam(CLAM_DEBUG5, "llm",
      "prompt chat submit model=%s n_messages=%zu temp=%.3f"
      " max_tokens=%u timeout=%us stream=%d",
      model_name, n_msgs,
      (double)params->temperature, params->max_tokens,
      params->timeout_secs, (int)params->stream);

  for(size_t i = 0; i < n_msgs; i++)
  {
    // When the caller used blocks[], emit a compact placeholder per
    // block instead of the text content. Base64 must never land in
    // the log, and the text path would NULL-deref on content.
    if(msgs[i].blocks != NULL)
    {
      const char *role = llm_role_str(msgs[i].role);

      clam(CLAM_DEBUG5, "llm",
          "prompt chat msg[%zu/%zu] role=%s blocks=%zu",
          i + 1, n_msgs, role, msgs[i].n_blocks);

      for(size_t k = 0; k < msgs[i].n_blocks; k++)
      {
        const llm_content_block_t *blk = &msgs[i].blocks[k];

        if(blk->kind == LLM_CONTENT_TEXT)
          clam(CLAM_DEBUG5, "llm",
              "  [block %zu] text: %s",
              k, blk->text != NULL ? blk->text : "");
        else
          clam(CLAM_DEBUG5, "llm",
              "  [block %zu] image mime=%s base64=%zu bytes",
              k,
              blk->image_mime != NULL ? blk->image_mime : "?",
              blk->image_b64  != NULL ? strlen(blk->image_b64) : 0);
      }
      continue;
    }

    llm_clam_prompt_content("chat", i + 1, n_msgs,
        llm_role_str(msgs[i].role), msgs[i].content);
  }
}

static void
llm_clam_prompt_embed(const char *model_name,
    const char *const *inputs, size_t n_inputs)
{
  clam(CLAM_DEBUG5, "llm",
      "prompt embed submit model=%s n_inputs=%zu",
      model_name, n_inputs);

  for(size_t i = 0; i < n_inputs; i++)
    llm_clam_prompt_content("embed", i + 1, n_inputs, "input", inputs[i]);
}

// Request-dialect negotiation (LLM-DIALECT-1)
//
// One OpenAI-compatible service can host models with incompatible request
// bodies (gpt-4o wants "max_tokens" + a temperature; gpt-5.x wants
// "max_completion_tokens" and rejects any explicit temperature). OpenAI's
// parameter 400s name their own fix, so we send the optimistic (classic)
// body, parse a recognizable failure, learn the correction per model, retry,
// and persist it — no model names in C.

// Locate the first single-quoted '<token>' that begins immediately after
// `anchor` (which must itself end at the opening quote) within [body, +len).
// Copies the token into out. Returns SUCCESS if a non-empty token fits.
static bool
llm_extract_squoted(const char *body, size_t len, const char *anchor,
    char *out, size_t out_sz)
{
  const char *a = util_memstr(body, len, anchor);
  const char *end = body + len;
  const char *p;
  const char *q;
  size_t n;

  if(a == NULL)
    return(FAIL);

  p = a + strlen(anchor);
  q = p;

  while(q < end && *q != '\'')
    q++;

  if(q >= end)
    return(FAIL);

  n = (size_t)(q - p);

  if(n == 0 || n >= out_sz)
    return(FAIL);

  memcpy(out, p, n);
  out[n] = '\0';
  return(SUCCESS);
}

// Parse an OpenAI-style invalid_request_error body into one candidate
// directive. Returns SUCCESS if the grammar matched. The offending field is
// taken verbatim from the message; the caller gates it against the set of
// fields the builder actually emits.
static bool
llm_negotiate_parse(const char *body, size_t len, llm_directive_t *out)
{
  char field[LLM_DIR_FIELD_SZ];
  char repl[LLM_DIR_FIELD_SZ];

  memset(out, 0, sizeof(*out));

  // "Unsupported parameter: '<P>' is not supported ... [Use '<Q>' instead]"
  // "Unknown parameter: '<P>'." — the images endpoint's wording for a field
  // the model does not take at all (the gpt-image family and
  // `response_format`). It never names a replacement, so it always resolves
  // to a DROP; the "Use '" probe below costs nothing and stays shared.
  if(llm_extract_squoted(body, len, "Unsupported parameter: '",
         field, sizeof(field)) == SUCCESS
      || llm_extract_squoted(body, len, "Unknown parameter: '",
         field, sizeof(field)) == SUCCESS)
  {
    snprintf(out->field, sizeof(out->field), "%s", field);

    if(llm_extract_squoted(body, len, "Use '", repl, sizeof(repl)) == SUCCESS)
    {
      out->action = LLM_DIR_RENAME;
      snprintf(out->replacement, sizeof(out->replacement), "%s", repl);
    }

    else
      out->action = LLM_DIR_DROP;

    return(SUCCESS);
  }

  // "Unsupported value: '<P>' does not support <v> ... Only the default ..."
  if(llm_extract_squoted(body, len, "Unsupported value: '",
      field, sizeof(field)) == SUCCESS)
  {
    snprintf(out->field, sizeof(out->field), "%s", field);
    out->action = LLM_DIR_DROP;
    return(SUCCESS);
  }

  return(FAIL);
}

// True for the canonical fields the params tail for `type` emits and can
// negotiate. A parsed field outside this set (e.g. the renamed wire name on a
// second failure) bails the negotiation — this bounds the retry loop.
//
// Every image field here is safe to drop, because each one's provider default
// is what we would have asked for anyway. `response_format` reads like the
// exception and is not: a provider that rejects the parameter outright has
// exactly one output form, and for an images endpoint that form is the
// inline base64 we wanted.
static bool
llm_is_builder_field(llm_req_type_t type, const char *field)
{
  if(type == LLM_REQ_IMAGE)
    return(strcmp(field, "response_format") == 0
        || strcmp(field, "size") == 0
        || strcmp(field, "n") == 0);

  return(type == LLM_REQ_CHAT
      && (strcmp(field, "temperature") == 0
       || strcmp(field, "max_tokens") == 0));
}

// The request's currently-applied directive for a canonical field, or NULL.
static const llm_directive_t *
llm_req_directive(const llm_request_t *req, const char *field)
{
  for(uint32_t i = 0; i < req->n_directives; i++)
    if(strcmp(req->directives[i].field, field) == 0)
      return(&req->directives[i]);

  return(NULL);
}

// Resolve the wire field name to emit for a canonical builder field: the
// field itself (KEEP / no directive), the replacement (RENAME), or NULL
// (DROP — emit nothing).
static const char *
llm_wire_field(const llm_request_t *req, const char *field)
{
  const llm_directive_t *d = llm_req_directive(req, field);

  if(d == NULL || d->action == LLM_DIR_KEEP)
    return(field);

  if(d->action == LLM_DIR_RENAME)
    return(d->replacement);

  return(NULL);
}

// True if `d` differs from what the request already applies for d->field
// (unseen field, or a different action/replacement). A directive we already
// applied that still 400s is not new — the caller gives up on it.
static bool
llm_directive_is_new(const llm_request_t *req, const llm_directive_t *d)
{
  const llm_directive_t *cur = llm_req_directive(req, d->field);

  if(cur == NULL || cur->action != d->action)
    return(true);

  if(d->action == LLM_DIR_RENAME
      && strcmp(cur->replacement, d->replacement) != 0)
    return(true);

  return(false);
}

// Add or replace the directive for d->field in the request's applied set.
static void
llm_req_directive_add(llm_request_t *req, const llm_directive_t *d)
{
  for(uint32_t i = 0; i < req->n_directives; i++)
    if(strcmp(req->directives[i].field, d->field) == 0)
    {
      req->directives[i] = *d;
      return;
    }

  if(req->n_directives < LLM_MAX_DIRECTIVES)
    req->directives[req->n_directives++] = *d;
}

// Learned-directive cache (mirror of llm_model_params)

static void
llm_model_params_clear(void)
{
  llm_model_params_t *node;

  pthread_rwlock_wrlock(&llm_model_params_lock);
  node = llm_model_params_head;
  llm_model_params_head = NULL;
  pthread_rwlock_unlock(&llm_model_params_lock);

  while(node != NULL)
  {
    llm_model_params_t *next = node->next;
    mem_free(node);
    node = next;
  }
}

// Insert or replace the directive for (service, model_id, d->field). Takes
// the write lock. The stored copy is never staged.
static void
llm_model_params_cache_put(const char *service, const char *model_id,
    const llm_directive_t *d)
{
  llm_model_params_t *node;

  pthread_rwlock_wrlock(&llm_model_params_lock);

  for(node = llm_model_params_head; node != NULL; node = node->next)
    if(strcmp(node->service_name, service) == 0
        && strcmp(node->model_id, model_id) == 0)
      break;

  if(node == NULL)
  {
    node = mem_alloc("llm", "model_params", sizeof(*node));
    memset(node, 0, sizeof(*node));
    snprintf(node->service_name, sizeof(node->service_name), "%s", service);
    snprintf(node->model_id, sizeof(node->model_id), "%s", model_id);
    node->next = llm_model_params_head;
    llm_model_params_head = node;
  }

  for(uint32_t i = 0; i < node->n_directives; i++)
    if(strcmp(node->directives[i].field, d->field) == 0)
    {
      node->directives[i] = *d;
      node->directives[i].staged = false;
      pthread_rwlock_unlock(&llm_model_params_lock);
      return;
    }

  if(node->n_directives < LLM_MAX_DIRECTIVES)
  {
    node->directives[node->n_directives] = *d;
    node->directives[node->n_directives].staged = false;
    node->n_directives++;
  }

  pthread_rwlock_unlock(&llm_model_params_lock);
}

// Copy the cached directive set for (service, model_id) into out[] (up to
// max). Returns the count copied. Takes the read lock.
static uint32_t
llm_model_params_lookup(const char *service, const char *model_id,
    llm_directive_t *out, uint32_t max)
{
  uint32_t n = 0;

  pthread_rwlock_rdlock(&llm_model_params_lock);

  for(llm_model_params_t *node = llm_model_params_head; node != NULL;
      node = node->next)
  {
    if(strcmp(node->service_name, service) == 0
        && strcmp(node->model_id, model_id) == 0)
    {
      for(uint32_t i = 0; i < node->n_directives && n < max; i++)
        out[n++] = node->directives[i];

      break;
    }
  }

  pthread_rwlock_unlock(&llm_model_params_lock);
  return(n);
}

// Rebuild the directive cache from llm_model_params. Called at startup after
// the services/models reload.
static void
llm_model_params_reload(void)
{
  db_result_t *res = db_result_alloc();

  llm_model_params_clear();

  if(db_query(
      "SELECT service_name, model_id, field, action, replacement"
      " FROM llm_model_params", res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "llm", "model-params reload: %s", res->error);

    db_result_free(res);
    return;
  }

  for(uint32_t r = 0; r < res->rows; r++)
  {
    llm_directive_t d;
    const char     *svc   = db_result_get(res, r, 0);
    const char     *mid   = db_result_get(res, r, 1);
    const char     *field = db_result_get(res, r, 2);
    const char     *act   = db_result_get(res, r, 3);
    const char     *repl  = db_result_get(res, r, 4);

    memset(&d, 0, sizeof(d));

    if(svc == NULL || mid == NULL || field == NULL || act == NULL)
      continue;

    snprintf(d.field, sizeof(d.field), "%s", field);

    if(strcmp(act, "rename") == 0)
    {
      d.action = LLM_DIR_RENAME;
      snprintf(d.replacement, sizeof(d.replacement), "%s", repl ? repl : "");
    }

    else if(strcmp(act, "drop") == 0)
      d.action = LLM_DIR_DROP;

    else
      continue;

    llm_model_params_cache_put(svc, mid, &d);
  }

  db_result_free(res);
}

// Persist one learned directive so it survives restart. INSERT ... ON
// CONFLICT DO UPDATE, keyed by (service, model_id, field).
static void
llm_model_params_persist(const char *service, const char *model_id,
    const llm_directive_t *d)
{
  char        *e_svc   = db_escape(service);
  char        *e_mid   = db_escape(model_id);
  char        *e_field = db_escape(d->field);
  char        *e_repl  = NULL;
  char         sql[1024];
  db_result_t *res;

  if(e_svc == NULL || e_mid == NULL || e_field == NULL)
    goto done;

  if(d->action == LLM_DIR_RENAME)
  {
    e_repl = db_escape(d->replacement);

    if(e_repl == NULL)
      goto done;

    snprintf(sql, sizeof(sql),
        "INSERT INTO llm_model_params"
        " (service_name, model_id, field, action, replacement)"
        " VALUES ('%s','%s','%s','rename','%s')"
        " ON CONFLICT (service_name, model_id, field) DO UPDATE SET"
        " action='rename', replacement=EXCLUDED.replacement, learned=NOW()",
        e_svc, e_mid, e_field, e_repl);
  }

  else
    snprintf(sql, sizeof(sql),
        "INSERT INTO llm_model_params"
        " (service_name, model_id, field, action, replacement)"
        " VALUES ('%s','%s','%s','drop',NULL)"
        " ON CONFLICT (service_name, model_id, field) DO UPDATE SET"
        " action='drop', replacement=NULL, learned=NOW()",
        e_svc, e_mid, e_field);

  res = db_result_alloc();

  if((db_query(sql, res) != SUCCESS || !res->ok) && res->error[0] != '\0')
    clam(CLAM_WARN, "llm", "persist directive: %s", res->error);

  db_result_free(res);

done:
  if(e_svc   != NULL) mem_free(e_svc);
  if(e_mid   != NULL) mem_free(e_mid);
  if(e_field != NULL) mem_free(e_field);
  if(e_repl  != NULL) mem_free(e_repl);
}

// Flush every directive the request learned this run to the DB, clearing the
// staged flag so a later delivery (should one ever recur) does not re-write.
static void
llm_model_params_flush_staged(llm_request_t *req)
{
  for(uint32_t i = 0; i < req->n_directives; i++)
    if(req->directives[i].staged)
    {
      llm_model_params_persist(req->service_name, req->model_id,
          &req->directives[i]);
      req->directives[i].staged = false;
    }
}

// Request body assembly

// Emit the mutable chat-params tail (temperature / max_tokens / stream) and
// the closing brace, applying the request's learned dialect directives: a
// DROP omits the field, a RENAME emits it under a different wire name. Keep
// `stream` non-negotiable — providers never reject it.
static void
llm_append_chat_params(llm_buf_t *b, const llm_request_t *req)
{
  const char *wf;

  if(req->params.temperature > 0.0f)
  {
    wf = llm_wire_field(req, "temperature");

    if(wf != NULL)
      llm_buf_printf(b, ",\"%s\":%.3f", wf, (double)req->params.temperature);
  }

  if(req->params.max_tokens > 0)
  {
    wf = llm_wire_field(req, "max_tokens");

    if(wf != NULL)
      llm_buf_printf(b, ",\"%s\":%u", wf, req->params.max_tokens);
  }

  // Optional per-service reasoning_effort (RSN-1). Read fresh per request so
  // an operator can retune without a restart. Kept in the params tail so a
  // DIALECT-1 negotiation retry (which rebuilds only the tail) preserves it.
  {
    char        rkey[LLM_KV_KEY_SZ];
    const char *reff;

    snprintf(rkey, sizeof(rkey), "llm.service.%s.reasoning_effort",
        req->service_name);
    reff = kv_get_str(rkey);

    if(reff != NULL && reff[0] != '\0')
      llm_buf_printf(b, ",\"reasoning_effort\":\"%s\"", reff);
  }

  if(req->params.stream)
    llm_buf_puts(b, ",\"stream\":true");

  llm_buf_putc(b, '}');
}

// Build the immutable body prefix {"model":...,"messages":[...]} into
// req->body_prefix (through the messages-array close bracket, no params, no
// closing brace). Retained so a negotiation retry rebuilds only the tail.
static bool
llm_build_chat_prefix(llm_request_t *req, const llm_message_t *msgs,
    size_t n_msgs)
{
  llm_buf_t b;
  llm_buf_init(&b, 512);

  llm_buf_puts(&b, "{\"model\":");
  llm_json_str(&b, req->model_id);

  llm_buf_puts(&b, ",\"messages\":[");

  for(size_t i = 0; i < n_msgs; i++)
  {
    if(i > 0)
      llm_buf_putc(&b, ',');

    llm_buf_puts(&b, "{\"role\":");
    llm_json_str(&b, llm_role_str(msgs[i].role));
    llm_buf_puts(&b, ",\"content\":");

    if(msgs[i].blocks != NULL)
    {
      // Structured content array: "content":[{...},{...}]
      llm_buf_putc(&b, '[');

      for(size_t k = 0; k < msgs[i].n_blocks; k++)
      {
        const llm_content_block_t *blk = &msgs[i].blocks[k];

        if(k > 0)
          llm_buf_putc(&b, ',');

        switch(blk->kind)
        {
          case LLM_CONTENT_TEXT:
            llm_buf_puts(&b, "{\"type\":\"text\",\"text\":");
            llm_json_str(&b, blk->text != NULL ? blk->text : "");
            llm_buf_putc(&b, '}');
            break;

          case LLM_CONTENT_IMAGE_BASE64:
            // {"type":"image_url","image_url":{"url":"data:<mime>;base64,<b64>"}}
            // mime comes from a controlled allowlist (image/jpeg|png|
            // gif|webp) and b64 is [A-Za-z0-9+/=] only -- both are
            // safe-for-JSON bare strings, so emit without llm_json_str
            // to avoid a ~14 MiB double-scan on large images.
            llm_buf_puts(&b,
                "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:");
            llm_buf_puts(&b, blk->image_mime != NULL
                ? blk->image_mime : "image/png");
            llm_buf_puts(&b, ";base64,");
            llm_buf_puts(&b, blk->image_b64 != NULL ? blk->image_b64 : "");
            llm_buf_puts(&b, "\"}}");
            break;
        }
      }

      llm_buf_putc(&b, ']');
    }
    else
    {
      // Legacy text fast-path -- byte-identical to today's output.
      llm_json_str(&b, msgs[i].content != NULL ? msgs[i].content : "");
    }

    llm_buf_putc(&b, '}');
  }

  llm_buf_putc(&b, ']');

  if(req->body_prefix != NULL)
    mem_free(req->body_prefix);

  req->body_prefix     = b.buf;
  req->body_prefix_len = b.len;
  return(SUCCESS);
}

// Emit the mutable image-params tail (n / size / response_format) and the
// closing brace, applying the request's learned dialect directives exactly as
// the chat tail does.
//
// We ask for base64 because the bot hosts the result itself and a
// provider-hosted URL is of no use to it — but the ask is negotiable, since a
// provider that returns base64 unconditionally rejects being told to (see
// llm_is_builder_field). `size` is omitted when unset so the provider's own
// default applies.
static void
llm_append_image_params(llm_buf_t *b, const llm_request_t *req)
{
  const char *wf;

  wf = llm_wire_field(req, "n");

  if(wf != NULL)
    llm_buf_printf(b, ",\"%s\":%u", wf, req->image_n);

  if(req->image_size[0] != '\0')
  {
    wf = llm_wire_field(req, "size");

    if(wf != NULL)
    {
      llm_buf_printf(b, ",\"%s\":", wf);
      llm_json_str(b, req->image_size);
    }
  }

  wf = llm_wire_field(req, "response_format");

  if(wf != NULL)
    llm_buf_printf(b, ",\"%s\":\"b64_json\"", wf);

  llm_buf_putc(b, '}');
}

// Compose req->req_body = body_prefix + the params tail its kind emits.
// Called on first submit and on every retry (including negotiation retries,
// where the directives — hence the tail — may have changed). The prefix, which
// carries the messages or the prompt, is never re-encoded.
static bool
llm_compose_body(llm_request_t *req)
{
  llm_buf_t b;

  if(req->body_prefix == NULL)
    return(FAIL);

  llm_buf_init(&b, req->body_prefix_len + 64);
  llm_buf_append(&b, req->body_prefix, req->body_prefix_len);

  switch(req->type)
  {
    case LLM_REQ_CHAT:  llm_append_chat_params(&b, req);  break;
    case LLM_REQ_IMAGE: llm_append_image_params(&b, req); break;

    // No other kind builds a prefix, so no other kind reaches here.
    default:
      mem_free(b.buf);
      return(FAIL);
  }

  if(req->req_body != NULL)
    mem_free(req->req_body);

  req->req_body     = b.buf;
  req->req_body_len = b.len;
  return(SUCCESS);
}

static bool
llm_build_embed_body(llm_request_t *req, const char *const *inputs,
    size_t n)
{
  llm_buf_t b;
  llm_buf_init(&b, 512);

  llm_buf_puts(&b, "{\"model\":");
  llm_json_str(&b, req->model_id);

  llm_buf_puts(&b, ",\"input\":[");

  for(size_t i = 0; i < n; i++)
  {
    if(i > 0)
      llm_buf_putc(&b, ',');

    llm_json_str(&b, inputs[i] != NULL ? inputs[i] : "");
  }

  llm_buf_puts(&b, "]}");

  req->req_body     = b.buf;
  req->req_body_len = b.len;
  return(SUCCESS);
}

// Build the immutable image-body prefix {"model":...,"prompt":...} into
// req->body_prefix (no params, no closing brace). Retained for the same
// reason the chat prefix is: a negotiation retry rebuilds only the tail.
static bool
llm_build_image_prefix(llm_request_t *req, const char *prompt)
{
  llm_buf_t b;
  llm_buf_init(&b, 512);

  llm_buf_puts(&b, "{\"model\":");
  llm_json_str(&b, req->model_id);

  llm_buf_puts(&b, ",\"prompt\":");
  llm_json_str(&b, prompt != NULL ? prompt : "");

  if(req->body_prefix != NULL)
    mem_free(req->body_prefix);

  req->body_prefix     = b.buf;
  req->body_prefix_len = b.len;
  return(SUCCESS);
}

// Build the speech-to-text request body. This is the one request the
// engine sends that is not JSON: whisper.cpp's server takes an RFC-7578
// multipart upload on /inference, and its OpenAI-compatible route does
// not exist (measured against 1.9.1). Two parts — the audio, and the
// response_format the parser downstream expects.
//
// The boundary is randomised per request rather than fixed: the payload
// is binary audio, and a constant delimiter is one an unlucky run of
// samples can forge.
static bool
llm_build_stt_body(llm_request_t *req, const void *wav, size_t wav_len)
{
  char   boundary[LLM_BOUNDARY_SZ];
  char   head[LLM_BOUNDARY_SZ + 128];
  char   tail[LLM_BOUNDARY_SZ * 2 + 128];
  char  *body;
  int    head_len;
  int    tail_len;

  snprintf(boundary, sizeof(boundary), "----botman%04x%04x%04x%04x",
      (unsigned)util_rand(0x10000), (unsigned)util_rand(0x10000),
      (unsigned)util_rand(0x10000), (unsigned)util_rand(0x10000));

  head_len = snprintf(head, sizeof(head),
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"file\";"
      " filename=\"audio.wav\"\r\n"
      "Content-Type: audio/wav\r\n"
      "\r\n",
      boundary);

  tail_len = snprintf(tail, sizeof(tail),
      "\r\n--%s\r\n"
      "Content-Disposition: form-data; name=\"response_format\"\r\n"
      "\r\n"
      "json\r\n"
      "--%s--\r\n",
      boundary, boundary);

  if(head_len < 0 || (size_t)head_len >= sizeof(head)
      || tail_len < 0 || (size_t)tail_len >= sizeof(tail))
    return(FAIL);

  body = mem_alloc("llm", "stt_body",
      (size_t)head_len + wav_len + (size_t)tail_len);

  memcpy(body, head, (size_t)head_len);
  memcpy(body + head_len, wav, wav_len);
  memcpy(body + head_len + wav_len, tail, (size_t)tail_len);

  req->req_body     = body;
  req->req_body_len = (size_t)head_len + wav_len + (size_t)tail_len;

  snprintf(req->content_type, sizeof(req->content_type),
      "multipart/form-data; boundary=%s", boundary);

  return(SUCCESS);
}

// Build the text-to-speech request body:
//   {"model":...,"input":...,"voice":...,"speed":...,"response_format":"wav"}
// Voice and speed are omitted when unset so the provider's own defaults
// apply — kokorod answers a bare {"input":...} perfectly well.
static bool
llm_build_tts_body(llm_request_t *req, const char *text,
    const char *voice, double speed)
{
  llm_buf_t b;
  llm_buf_init(&b, 512);

  llm_buf_puts(&b, "{\"model\":");
  llm_json_str(&b, req->model_id);

  llm_buf_puts(&b, ",\"input\":");
  llm_json_str(&b, text);

  if(voice != NULL && voice[0] != '\0')
  {
    llm_buf_puts(&b, ",\"voice\":");
    llm_json_str(&b, voice);
  }

  if(speed > 0.0)
    llm_buf_printf(&b, ",\"speed\":%.3f", speed);

  llm_buf_puts(&b, ",\"response_format\":\"wav\"}");

  req->req_body     = b.buf;
  req->req_body_len = b.len;
  return(SUCCESS);
}

// Response parsers

// Parse a non-streaming chat body into req->assembled + token counts.
// Returns SUCCESS if content was extracted, FAIL otherwise.
static bool
llm_parse_chat_response(llm_request_t *req, const char *body, size_t len)
{
  // Look for "message":{...,"content":"..."}. We search for the first
  // "content" that appears after "message"; on vLLM/OpenAI this is robust
  // enough for v1.
  const char *msg_key = util_memstr(body, len, "\"message\"");
  const char *search_start = msg_key != NULL ? msg_key : body;
  size_t remaining = len - (size_t)(search_start - body);

  char *out = mem_alloc("llm", "content", len + 1);
  ssize_t n = llm_extract_str(search_start, remaining, "\"content\"",
      out, len + 1);

  char fr[LLM_FINISH_SZ];
  const char *usage;
  if(n < 0)
  {
    mem_free(out);
    snprintf(req->errbuf, sizeof(req->errbuf), "no content in response");
    return(FAIL);
  }

  llm_assembled_append(req, out, (size_t)n);
  mem_free(out);

  // finish_reason

  if(llm_extract_str(body, len, "\"finish_reason\"", fr, sizeof(fr)) > 0)
    snprintf(req->finish_reason, sizeof(req->finish_reason), "%s", fr);

  // usage counters
  usage = util_memstr(body, len, "\"usage\"");

  if(usage != NULL)
  {
    size_t urem = len - (size_t)(usage - body);
    long v = 0;

    if(llm_extract_int(usage, urem, "\"prompt_tokens\"", &v) == SUCCESS)
      req->prompt_tokens = (uint32_t)v;

    if(llm_extract_int(usage, urem, "\"completion_tokens\"", &v) == SUCCESS)
      req->completion_tokens = (uint32_t)v;
  }

  return(SUCCESS);
}

// Parse an embedding response into req->vectors. Returns SUCCESS/FAIL.
static bool
llm_parse_embed_response(llm_request_t *req, const char *body, size_t len)
{
  // Expect: {"data":[{"embedding":[f1,f2,...]}, ...], ...}
  // Count embedding arrays by searching iteratively.
  const char *p = body;
  const char *end = body + len;

  // First pass: count occurrences of "embedding" and determine dim from
  // the first one.
  size_t n_vecs = 0;
  uint32_t dim = 0;

  const char *scan = p;

  size_t vi;
  while(scan < end)
  {
    const char *emb = util_memstr(scan, (size_t)(end - scan), "\"embedding\"");

    const char *after_key;
    uint32_t this_dim;
    const char *val;
    const char *arr_start;
    if(emb == NULL)
      break;

    after_key = emb + strlen("\"embedding\"");
    val = util_skip_to_value(after_key, end);

    // Response contains `"object":"embedding"` as a decoy — that match
    // is followed by a comma, not a colon. Skip past this occurrence
    // and keep scanning for the real `"embedding":[...]` key.
    if(val == NULL || *val != '[')
    {
      scan = after_key;
      continue;
    }
    emb = val;

    // Count floats in this array.
    emb++;
    this_dim = 0;
    arr_start = emb;

    while(emb < end && *emb != ']')
    {
      char *eptr;
      while(emb < end && (*emb == ' ' || *emb == ',' || *emb == '\t'
            || *emb == '\n' || *emb == '\r'))
        emb++;

      if(emb >= end || *emb == ']')
        break;

      eptr = NULL;
      strtof(emb, &eptr);

      if(eptr == emb)
        break;

      this_dim++;
      emb = eptr;
    }

    (void)arr_start;

    if(emb >= end || *emb != ']')
      break;

    if(n_vecs == 0)
      dim = this_dim;

    n_vecs++;
    scan = emb + 1;
  }

  if(n_vecs == 0 || dim == 0)
  {
    snprintf(req->errbuf, sizeof(req->errbuf), "no embeddings in response");
    return(FAIL);
  }

  req->vec_block     = mem_alloc("llm", "vec_block",
                         sizeof(float) * dim * n_vecs);
  req->vec_block_len = (size_t)dim * n_vecs;
  req->vectors       = mem_alloc("llm", "vec_ptrs",
                         sizeof(float *) * n_vecs);
  req->n_vectors     = n_vecs;
  req->vectors_dim   = dim;

  // Second pass: actually read the floats.
  vi = 0;
  scan = p;

  while(scan < end && vi < n_vecs)
  {
    const char *emb = util_memstr(scan, (size_t)(end - scan), "\"embedding\"");

    const char *after_key;
    float *dst;
    const char *val;
    if(emb == NULL)
      break;

    after_key = emb + strlen("\"embedding\"");
    val = util_skip_to_value(after_key, end);

    // Same decoy guard as pass 1: `"object":"embedding"` precedes the
    // real `"embedding":[...]` in each data item. Skip non-array hits
    // instead of breaking, otherwise vec_block ships uninitialized.
    if(val == NULL || *val != '[')
    {
      scan = after_key;
      continue;
    }

    emb = val + 1;
    dst = req->vec_block + (size_t)vi * dim;
    req->vectors[vi] = dst;

    for(uint32_t k = 0; k < dim; k++)
    {
      char *eptr;
      while(emb < end && (*emb == ' ' || *emb == ',' || *emb == '\t'
            || *emb == '\n' || *emb == '\r'))
        emb++;

      eptr = NULL;
      dst[k] = strtof(emb, &eptr);

      if(eptr == emb)
        break;

      emb = eptr;
    }

    while(emb < end && *emb != ']')
      emb++;

    if(emb >= end)
      break;

    scan = emb + 1;
    vi++;
  }

  return(SUCCESS);
}

// Parse a text-to-image response: {"data":[{"b64_json":"<base64>",
// "revised_prompt":"..."}]}. The base64 payload can be megabytes, so we
// locate its bounds and append the raw slice straight into req->assembled
// (base64's RFC 4648 alphabet contains no JSON metacharacters, so no
// unescape pass is needed) rather than routing it through a fixed buffer.
// Returns SUCCESS if a payload was captured, FAIL otherwise.
static bool
llm_parse_image_response(llm_request_t *req, const char *body, size_t len)
{
  const char *end = body + len;
  const char *key = util_memstr(body, len, "\"b64_json\"");

  const char *val;
  const char *start;
  const char *p;
  char        rev[LLM_IMAGE_REVISED_SZ];

  if(key == NULL)
  {
    snprintf(req->errbuf, sizeof(req->errbuf), "no b64_json in response");
    return(FAIL);
  }

  val = util_skip_to_value(key + strlen("\"b64_json\""), end);

  if(val == NULL || *val != '"')
  {
    snprintf(req->errbuf, sizeof(req->errbuf), "malformed b64_json value");
    return(FAIL);
  }

  start = val + 1;

  for(p = start; p < end && *p != '"'; p++)
    if(*p == '\\' && p + 1 < end)
      p++;

  if(p >= end)
  {
    snprintf(req->errbuf, sizeof(req->errbuf), "unterminated b64_json value");
    return(FAIL);
  }

  if(p == start)
  {
    snprintf(req->errbuf, sizeof(req->errbuf), "empty b64_json value");
    return(FAIL);
  }

  llm_assembled_append(req, start, (size_t)(p - start));

  // Default MIME; providers do not currently return one for images.
  snprintf(req->image_mime, sizeof(req->image_mime), "image/png");

  // Optional provider-rewritten prompt.
  if(llm_extract_str(body, len, "\"revised_prompt\"", rev, sizeof(rev)) > 0)
    snprintf(req->image_revised, sizeof(req->image_revised), "%s", rev);

  return(SUCCESS);
}

// Collapse every run of whitespace to one space and trim both ends, in
// place. Returns the new length. whisper punctuates its transcript with
// the newlines it heard pauses at — " And so, my fellow Americans, ask
// not what your country can\n do for you" — and a message carrying those
// verbatim becomes several lines wherever it is delivered.
static size_t
llm_collapse_ws(char *s)
{
  char *w   = s;
  bool  gap = false;

  for(const char *r = s; *r != '\0'; r++)
  {
    if(*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r')
    {
      gap = w != s;      // a gap before the first kept byte is not a gap
      continue;
    }

    if(gap)
      *w++ = ' ';

    gap  = false;
    *w++ = *r;
  }

  *w = '\0';
  return((size_t)(w - s));
}

// Parse a transcription response: {"text":"…"}. An empty transcript is
// a legitimate answer — the speaker said nothing intelligible — so it
// succeeds with zero length rather than failing.
static bool
llm_parse_stt_response(llm_request_t *req, const char *body, size_t len)
{
  char   *out = mem_alloc("llm", "transcript", len + 1);
  ssize_t n   = llm_extract_str(body, len, "\"text\"", out, len + 1);

  if(n < 0)
  {
    mem_free(out);
    snprintf(req->errbuf, sizeof(req->errbuf), "no text in response");
    return(FAIL);
  }

  llm_assembled_append(req, out, llm_collapse_ws(out));
  mem_free(out);

  return(SUCCESS);
}

// "Parse" a synthesis response: the provider answers with the audio
// container itself, so this is a Content-Type assertion plus a binary
// copy. Without the assertion a JSON error body served with a 200 would
// be handed downstream and uploaded to the robot as speech.
static bool
llm_parse_tts_response(llm_request_t *req, const char *body, size_t len)
{
  if(strncmp(req->resp_content_type, "audio/", 6) != 0)
  {
    snprintf(req->errbuf, sizeof(req->errbuf),
        "expected audio, got '%.64s'",
        req->resp_content_type[0] != '\0'
            ? req->resp_content_type : "no content type");
    return(FAIL);
  }

  llm_assembled_append(req, body, len);
  return(SUCCESS);
}

// Streaming chunk handler

// Called by sse_parser_feed for each complete SSE event.
static void
llm_sse_event_cb(const char *data, size_t len, void *user)
{
  llm_request_t *req = (llm_request_t *)user;

  // Strip trailing whitespace that the framer may have emitted.
  const char *delta;
  long v;
  char fr[LLM_FINISH_SZ];
  const char *scan;
  size_t scan_len;
  char *out;
  ssize_t n;
  while(len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n'))
    len--;

  if(len == 0)
    return;

  // [DONE] sentinel — stop consuming further frames.
  if(len == 6 && memcmp(data, "[DONE]", 6) == 0)
    return;

  // Extract "delta":{"content":"..."} — look for "delta" first.
  delta = util_memstr(data, len, "\"delta\"");
  scan = delta != NULL ? delta : data;
  scan_len = delta != NULL ? len - (size_t)(scan - data) : len;

  out = mem_alloc("llm", "delta", len + 1);
  n = llm_extract_str(scan, scan_len, "\"content\"", out, len + 1);

  if(n > 0)
  {
    llm_assembled_append(req, out, (size_t)n);
    req->bytes_seen += (size_t)n;

    if(req->chunk_cb != NULL)
      req->chunk_cb(req, out, (size_t)n, req->user_data);
  }

  mem_free(out);

  // Opportunistic: some servers include usage in the final frame.
  v = 0;

  if(llm_extract_int(data, len, "\"prompt_tokens\"", &v) == SUCCESS)
    req->prompt_tokens = (uint32_t)v;

  if(llm_extract_int(data, len, "\"completion_tokens\"", &v) == SUCCESS)
    req->completion_tokens = (uint32_t)v;


  if(llm_extract_str(data, len, "\"finish_reason\"", fr, sizeof(fr)) > 0
      && fr[0] != '\0')
    snprintf(req->finish_reason, sizeof(req->finish_reason), "%s", fr);
}

// curl per-chunk callback (streaming path only).
static void
llm_curl_chunk_cb(const curl_response_t *partial, const char *chunk,
    size_t chunk_len, void *user_data)
{
  llm_request_t *req;
  (void)partial;
  req = (llm_request_t *)user_data;

  if(req->sse_parser == NULL)
    req->sse_parser = sse_parser_new();

  sse_parser_feed(req->sse_parser, chunk, chunk_len,
      llm_sse_event_cb, req);
}

// Submit + retry

static bool llm_issue_request(llm_request_t *req);

// Stats accumulation on final response.
static void
llm_accumulate_stats(llm_request_t *req, bool ok)
{
  uint64_t ms = util_ms_since(&req->started);

  pthread_mutex_lock(&llm_stat_mutex);

  llm_stat_total++;

  if(!ok)
    llm_stat_errors++;

  llm_stat_prompt  += req->prompt_tokens;
  llm_stat_compl   += req->completion_tokens;
  llm_stat_time_ms += ms;

  pthread_mutex_unlock(&llm_stat_mutex);
}

// Deliver the final chat/embed callback and release the request.
static void
llm_deliver_chat(llm_request_t *req, bool ok, long http_status,
    const char *err)
{
  llm_chat_response_t resp;

  // The retried request finally succeeded — persist whatever dialect
  // directives it learned so later calls skip the negotiation round-trip.
  // (No-op when nothing was staged, i.e. every normal request.)
  if(ok)
    llm_model_params_flush_staged(req);

  memset(&resp, 0, sizeof(resp));
  resp.request           = req;
  resp.ok                = ok;
  resp.http_status       = http_status;
  resp.model             = req->model_name;
  resp.content           = req->assembled != NULL ? req->assembled : "";
  resp.content_len       = req->assembled_len;
  resp.prompt_tokens     = req->prompt_tokens;
  resp.completion_tokens = req->completion_tokens;
  resp.finish_reason     = req->finish_reason;
  resp.error             = ok ? NULL : (err != NULL ? err : req->errbuf);
  resp.user_data         = req->user_data;

  llm_active_remove(req);
  llm_accumulate_stats(req, ok);

  if(req->chat_done_cb != NULL)
    req->chat_done_cb(&resp);

  llm_req_release(req);
}

static void
llm_deliver_embed(llm_request_t *req, bool ok, long http_status,
    const char *err)
{
  llm_embed_response_t resp;

  memset(&resp, 0, sizeof(resp));
  resp.request     = req;
  resp.ok          = ok;
  resp.http_status = http_status;
  resp.model       = req->model_name;
  resp.dim         = req->vectors_dim;
  resp.vectors     = req->vectors;
  resp.n_vectors   = req->n_vectors;
  resp.error       = ok ? NULL : (err != NULL ? err : req->errbuf);
  resp.user_data   = req->user_data;

  llm_active_remove(req);
  llm_accumulate_stats(req, ok);

  if(req->embed_done_cb != NULL)
    req->embed_done_cb(&resp);

  llm_req_release(req);
}

static void
llm_deliver_image(llm_request_t *req, bool ok, long http_status,
    const char *err)
{
  llm_image_response_t resp;

  // As on the chat path: the retry that finally succeeded may have learned a
  // dialect directive (the gpt-image family's rejection of `response_format`
  // is the one measured case). Persist it so later renders build the right
  // body on the first try.
  if(ok)
    llm_model_params_flush_staged(req);

  memset(&resp, 0, sizeof(resp));
  resp.request        = req;
  resp.ok             = ok;
  resp.http_status    = http_status;
  resp.model          = req->model_name;
  resp.b64            = req->assembled != NULL ? req->assembled : "";
  resp.b64_len        = req->assembled_len;
  resp.mime           = req->image_mime[0] != '\0' ? req->image_mime
                          : "image/png";
  resp.revised_prompt = req->image_revised;
  resp.error          = ok ? NULL : (err != NULL ? err : req->errbuf);
  resp.user_data      = req->user_data;

  llm_active_remove(req);
  llm_accumulate_stats(req, ok);

  if(req->image_done_cb != NULL)
    req->image_done_cb(&resp);

  llm_req_release(req);
}

static void
llm_deliver_stt(llm_request_t *req, bool ok, long http_status,
    const char *err)
{
  llm_stt_response_t resp;

  memset(&resp, 0, sizeof(resp));
  resp.request     = req;
  resp.ok          = ok;
  resp.http_status = http_status;
  resp.model       = req->model_name;
  resp.text        = req->assembled != NULL ? req->assembled : "";
  resp.text_len    = req->assembled_len;
  resp.error       = ok ? NULL : (err != NULL ? err : req->errbuf);
  resp.user_data   = req->user_data;

  llm_active_remove(req);
  llm_accumulate_stats(req, ok);

  if(req->stt_done_cb != NULL)
    req->stt_done_cb(&resp);

  llm_req_release(req);
}

static void
llm_deliver_tts(llm_request_t *req, bool ok, long http_status,
    const char *err)
{
  llm_tts_response_t resp;

  memset(&resp, 0, sizeof(resp));
  resp.request      = req;
  resp.ok           = ok;
  resp.http_status  = http_status;
  resp.model        = req->model_name;
  resp.bytes        = (const uint8_t *)req->assembled;
  resp.bytes_len    = req->assembled_len;
  resp.content_type = req->resp_content_type;
  resp.error        = ok ? NULL : (err != NULL ? err : req->errbuf);
  resp.user_data    = req->user_data;

  llm_active_remove(req);
  llm_accumulate_stats(req, ok);

  if(req->tts_done_cb != NULL)
    req->tts_done_cb(&resp);

  llm_req_release(req);
}

// Deliver the terminal callback for whichever request type this is, then
// release the request. Single point of truth for the type→deliverer map.
static void
llm_deliver(llm_request_t *req, bool ok, long http_status, const char *err)
{
  switch(req->type)
  {
    case LLM_REQ_CHAT:  llm_deliver_chat(req, ok, http_status, err);  break;
    case LLM_REQ_EMBED: llm_deliver_embed(req, ok, http_status, err); break;
    case LLM_REQ_IMAGE: llm_deliver_image(req, ok, http_status, err); break;
    case LLM_REQ_STT:   llm_deliver_stt(req, ok, http_status, err);   break;
    case LLM_REQ_TTS:   llm_deliver_tts(req, ok, http_status, err);   break;
  }
}

// Deferred-task retry trampoline.
static void
llm_retry_task(task_t *t)
{
  llm_request_t *req = (llm_request_t *)t->data;

  pthread_mutex_lock(&llm_stat_mutex);
  llm_stat_retries++;
  pthread_mutex_unlock(&llm_stat_mutex);

  // Reset per-attempt state.
  req->http_status     = 0;
  req->errbuf[0]       = '\0';
  req->assembled_len   = 0;

  if(req->assembled != NULL)
    req->assembled[0] = '\0';

  req->prompt_tokens     = 0;
  req->completion_tokens = 0;
  req->finish_reason[0]  = '\0';
  req->bytes_seen        = 0;

  if(req->sse_parser != NULL)
    sse_parser_reset(req->sse_parser);

  // Rebuild the body tail so any directive a negotiation retry just learned
  // applies; the prefix — the messages, or the image prompt — is immutable.
  // (For a plain 429/5xx retry the directives are unchanged, so this is a
  // cheap no-op that reproduces the same body.)
  if(req->body_prefix != NULL && llm_compose_body(req) != SUCCESS)
  {
    snprintf(req->errbuf, sizeof(req->errbuf), "retry body rebuild failed");
    llm_deliver(req, false, 0, req->errbuf);
    t->state = TASK_ENDED;
    return;
  }

  if(llm_issue_request(req) != SUCCESS)
  {
    // Could not reissue; deliver failure.
    snprintf(req->errbuf, sizeof(req->errbuf), "retry submit failed");
    llm_deliver(req, false, 0, req->errbuf);
  }

  t->state = TASK_ENDED;
}

static bool
llm_is_retryable(long http_status, int curl_code)
{
  if(curl_code != 0)
    return(true);

  switch(http_status)
  {
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
      return(true);
    default:
      return(false);
  }
}

// curl completion callback.
static void
llm_curl_done_cb(const curl_response_t *resp)
{
  llm_request_t *req = (llm_request_t *)resp->user_data;

  bool ok;
  req->http_status = resp->status;

  // Captured for every kind, load-bearing for the audio ones: a 200
  // carrying JSON instead of a WAV is a failure the body alone does not
  // announce.
  snprintf(req->resp_content_type, sizeof(req->resp_content_type), "%s",
      resp->content_type != NULL ? resp->content_type : "");

  ok = false;

  if(resp->curl_code == 0 && resp->status >= 200 && resp->status < 300)
  {
    if(!req->streaming && resp->body != NULL && resp->body_len > 0)
    {
      switch(req->type)
      {
        case LLM_REQ_CHAT:
          ok = (llm_parse_chat_response(req, resp->body, resp->body_len)
                == SUCCESS);
          break;
        case LLM_REQ_EMBED:
          ok = (llm_parse_embed_response(req, resp->body, resp->body_len)
                == SUCCESS);
          break;
        case LLM_REQ_IMAGE:
          ok = (llm_parse_image_response(req, resp->body, resp->body_len)
                == SUCCESS);
          break;
        case LLM_REQ_STT:
          ok = (llm_parse_stt_response(req, resp->body, resp->body_len)
                == SUCCESS);
          break;
        case LLM_REQ_TTS:
          ok = (llm_parse_tts_response(req, resp->body, resp->body_len)
                == SUCCESS);
          break;
      }
    }

    else if(req->streaming)
    {
      // Streaming path: content already accumulated. OK if any delta arrived.
      ok = req->bytes_seen > 0;

      if(!ok)
        snprintf(req->errbuf, sizeof(req->errbuf),
            "stream ended with no content");
    }

    else
      snprintf(req->errbuf, sizeof(req->errbuf), "empty response body");
  }

  else
  {
    if(resp->error != NULL)
      snprintf(req->errbuf, sizeof(req->errbuf), "%s", resp->error);
    else if(resp->body != NULL && resp->body[0] != '\0')
    {
      char msg[LLM_ERR_SZ];

      // Providers wrap the real reason in {"error":{"message":"..."}}
      // (OpenAI-compat endpoints and Gemini, sometimes array-wrapped).
      // Surface that human-readable message rather than dumping raw JSON
      // downstream (it reaches users via e.g. the !ask reply). Fall back
      // to a body prefix when the shape is unexpected. Parse off
      // resp->body, not errbuf — DIALECT-1 below still needs the raw body.
      if(llm_extract_str(resp->body, resp->body_len, "\"message\"",
             msg, sizeof(msg)) > 0)
        snprintf(req->errbuf, sizeof(req->errbuf), "http %ld: %.200s",
            resp->status, msg);
      else
        snprintf(req->errbuf, sizeof(req->errbuf),
            "http %ld: %.180s", resp->status, resp->body);

      // Collapse newlines / CRs so the error reads as a single log
      // line (vLLM pretty-prints JSON errors with \n).
      for(char *p = req->errbuf; *p != '\0'; p++)
        if(*p == '\n' || *p == '\r') *p = ' ';
    }

    else
      snprintf(req->errbuf, sizeof(req->errbuf),
          "http %ld", resp->status);
  }

  // Dialect negotiation (LLM-DIALECT-1): an OpenAI-style 400 parameter error
  // names its own fix. Parse it off the raw body (not the truncated errbuf),
  // learn the directive, and schedule a corrective retry with a rebuilt body.
  // Gated to the two kinds that keep a rebuildable prefix (chat, image), and
  // for chat to requests that haven't streamed any bytes.
  if(!ok
      && (req->type == LLM_REQ_CHAT || req->type == LLM_REQ_IMAGE)
      && resp->status == 400
      && resp->curl_code == 0
      && (!req->streaming || req->bytes_seen == 0)
      && resp->body != NULL && resp->body_len > 0
      && util_memstr(resp->body, resp->body_len,
             "invalid_request_error") != NULL)
  {
    llm_directive_t d;

    if(llm_negotiate_parse(resp->body, resp->body_len, &d) == SUCCESS
        && llm_is_builder_field(req->type, d.field)
        && llm_directive_is_new(req, &d)
        && req->negotiation_attempts < LLM_NEGOTIATION_CAP
        && !llm_stopping)
    {
      task_handle_t nt;

      req->negotiation_attempts++;
      d.staged = true;                 // flush to DB only if the retry succeeds

      llm_req_directive_add(req, &d);  // this request's applied set
      llm_model_params_cache_put(req->service_name, req->model_id, &d);

      clam(CLAM_DEBUG, "llm",
          "negotiate %s: %s '%s'%s%s (attempt %u/%u)",
          req->model_name,
          d.action == LLM_DIR_RENAME ? "rename" : "drop", d.field,
          d.action == LLM_DIR_RENAME ? " -> " : "",
          d.action == LLM_DIR_RENAME ? d.replacement : "",
          req->negotiation_attempts, LLM_NEGOTIATION_CAP);

      // Delay 0: this is corrective, not backoff. Get off the curl callback
      // thread like the 429 path does.
      nt = task_add_deferred("llm_negotiate", TASK_ANY, 50, 0,
          llm_retry_task, req);

      if(nt != TASK_HANDLE_NONE)
        return;

      // Scheduling failed — fall through to deliver the failure.
      snprintf(req->errbuf, sizeof(req->errbuf),
          "negotiation retry scheduling failed");
    }
  }

  // Retry logic: only for failures, non-streaming (or streaming with no
  // bytes delivered), and within max_retries.
  if(!ok
      && req->attempt + 1 < llm_cfg.max_retries
      && llm_is_retryable(resp->status, resp->curl_code)
      && (!req->streaming || req->bytes_seen == 0)
      && !llm_stopping)
  {
    uint32_t backoff;
    task_handle_t t;
    req->attempt++;

    backoff = llm_cfg.retry_backoff_ms << req->attempt;

    if(backoff > LLM_RETRY_CAP_MS)
      backoff = LLM_RETRY_CAP_MS;

    clam(CLAM_DEBUG, "llm",
        "retrying %s attempt %u in %u ms (status=%ld)",
        req->model_name, req->attempt, backoff, resp->status);

    t = task_add_deferred("llm_retry", TASK_ANY, 50,
        backoff, llm_retry_task, req);

    if(t != TASK_HANDLE_NONE)
      return;

    // Retry scheduling failed — fall through to deliver failure.
    snprintf(req->errbuf, sizeof(req->errbuf), "retry scheduling failed");
  }

  llm_deliver(req, ok, resp->status, ok ? NULL : req->errbuf);
}

// Build a curl_request_t from req state and submit it. Caller manages
// req lifetime (freed by the done path). Returns SUCCESS or FAIL.
static bool
llm_issue_request(llm_request_t *req)
{
  curl_request_t *cr = curl_request_create(CURL_METHOD_POST,
      req->endpoint_url, llm_curl_done_cb, req);

  uint32_t to;
  bool ok;
  if(cr == NULL)
    return(FAIL);

  // JSON unless the request said otherwise — only the speech-to-text
  // path does, and only to carry its multipart boundary.
  if(curl_request_set_body(cr,
      req->content_type[0] != '\0' ? req->content_type : "application/json",
      req->req_body, req->req_body_len) != SUCCESS)
    goto fail;

  // Authorization header (if api key KV name configured).
  if(req->api_key_kv[0] != '\0')
  {
    const char *key = kv_get_creds(req->api_key_kv);

    if(key != NULL && key[0] != '\0')
    {
      char hdr[512];
      snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", key);
      curl_request_add_header(cr, hdr);
    }
  }

  // What we are willing to be answered with: an event stream while
  // streaming, audio from a synthesis request, JSON everywhere else.
  if(req->streaming)
    curl_request_add_header(cr, "Accept: text/event-stream");

  else if(req->type == LLM_REQ_TTS)
    curl_request_add_header(cr, "Accept: audio/wav");

  else
    curl_request_add_header(cr, "Accept: application/json");

  // Timeout.
  to = req->params.timeout_secs;

  if(to == 0)
    to = req->streaming
        ? (llm_cfg.streaming_idle_ms / 1000 + 1)
        : llm_cfg.timeout_secs;

  curl_request_set_timeout(cr, to);

  if(req->streaming)
  {
    curl_request_set_accumulate(cr, false);
    curl_request_set_chunk_cb(cr, llm_curl_chunk_cb, req);
  }

  ok = req->blocking_submit
      ? curl_request_submit_wait(cr)
      : curl_request_submit(cr);

  if(ok != SUCCESS)
    return(FAIL);

  return(SUCCESS);

fail:
  return(FAIL);
}

// Public submit API

bool
llm_chat_submit(const char *model_name,
    const llm_chat_params_t *params,
    const llm_message_t *messages, size_t n_messages,
    llm_chat_done_cb_t done_cb,
    llm_chunk_cb_t chunk_cb_or_NULL,
    void *user_data)
{
  llm_model_t m;
  llm_request_t *req;
  if(!llm_ready || model_name == NULL || params == NULL || messages == NULL
      || n_messages == 0 || done_cb == NULL)
    return(FAIL);

#ifdef LLM_TEST_HOOKS
  // Consume a pending test injection synchronously, bypassing model
  // lookup and the network path. Clears the slot on success.
  {
    pthread_mutex_lock(&llm_test_pending_mutex);
    char *canned = llm_test_pending;
    llm_test_pending = NULL;
    pthread_mutex_unlock(&llm_test_pending_mutex);

    if(canned != NULL)
    {
      llm_chat_response_t resp;
      memset(&resp, 0, sizeof(resp));
      resp.request           = NULL;
      resp.ok                = true;
      resp.http_status       = 200;
      resp.model             = model_name;
      resp.content           = canned;
      resp.content_len       = strlen(canned);
      resp.finish_reason     = "stop";
      resp.error             = NULL;
      resp.user_data         = user_data;

      done_cb(&resp);
      mem_free(canned);
      return(SUCCESS);
    }
  }
#endif


  if(llm_models_snapshot(model_name, &m) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "unknown model: %s", model_name);
    return(FAIL);
  }

  if(!m.enabled)
  {
    clam(CLAM_WARN, "llm", "model disabled: %s", model_name);
    return(FAIL);
  }

  if(m.kind != LLM_KIND_CHAT)
  {
    clam(CLAM_WARN, "llm", "model %s is not a chat model", model_name);
    return(FAIL);
  }

  req = llm_req_alloc();

  req->type = LLM_REQ_CHAT;
  snprintf(req->model_name, sizeof(req->model_name), "%s", m.name);
  snprintf(req->model_id,   sizeof(req->model_id),   "%s", m.model_id);

  if(llm_build_url(m.base_url, "chat/completions",
      req->endpoint_url, sizeof(req->endpoint_url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "cannot build chat URL for %s (service %s)",
        m.name, m.service_name);
    llm_req_release(req);
    return(FAIL);
  }

  snprintf(req->api_key_kv, sizeof(req->api_key_kv),
      "llm.service.%s.creds.apikey", m.service_name);
  snprintf(req->service_name, sizeof(req->service_name), "%s", m.service_name);

  // Seed any learned request-dialect directives so the very first body for a
  // known-quirky model is already correct (no negotiation round-trip).
  req->n_directives = llm_model_params_lookup(m.service_name, m.model_id,
      req->directives, LLM_MAX_DIRECTIVES);

  req->kind         = m.kind;
  req->params       = *params;
  req->chat_done_cb = done_cb;
  req->chunk_cb     = chunk_cb_or_NULL;
  req->user_data    = user_data;
  req->streaming    = params->stream;

  if(req->params.temperature == 0.0f && m.default_temp > 0.0f)
    req->params.temperature = m.default_temp;

  llm_clam_prompt_chat(model_name, &req->params, messages, n_messages);

  if(llm_build_chat_prefix(req, messages, n_messages) != SUCCESS
      || llm_compose_body(req) != SUCCESS)
  {
    llm_req_release(req);
    return(FAIL);
  }

  clock_gettime(CLOCK_MONOTONIC, &req->started);
  llm_active_add(req);

  if(llm_issue_request(req) != SUCCESS)
  {
    llm_active_remove(req);
    llm_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// Shared setup + submit for llm_embed_submit / llm_embed_submit_wait.
// blocking=false → fast-fail on queue-full; blocking=true → block on
// curl_request_submit_wait until a slot opens.
static bool
llm_embed_submit_impl(const char *model_name,
    const char *const *inputs, size_t n_inputs,
    llm_embed_done_cb_t done_cb, void *user_data, bool blocking)
{
  llm_model_t m;
  llm_request_t *req;
  if(!llm_ready || model_name == NULL || inputs == NULL || n_inputs == 0
      || done_cb == NULL)
    return(FAIL);


  if(llm_models_snapshot(model_name, &m) != SUCCESS || !m.enabled
      || m.kind != LLM_KIND_EMBED)
  {
    clam(CLAM_WARN, "llm", "unknown/disabled embed model: %s", model_name);
    return(FAIL);
  }

  req = llm_req_alloc();

  req->type = LLM_REQ_EMBED;
  snprintf(req->model_name, sizeof(req->model_name), "%s", m.name);
  snprintf(req->model_id,   sizeof(req->model_id),   "%s", m.model_id);

  if(llm_build_url(m.base_url, "embeddings",
      req->endpoint_url, sizeof(req->endpoint_url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "cannot build embed URL for %s (service %s)",
        m.name, m.service_name);
    llm_req_release(req);
    return(FAIL);
  }

  snprintf(req->api_key_kv, sizeof(req->api_key_kv),
      "llm.service.%s.creds.apikey", m.service_name);
  req->kind            = m.kind;
  req->embed_dim       = m.embed_dim;
  req->embed_done_cb   = done_cb;
  req->user_data       = user_data;
  req->streaming       = false;
  req->blocking_submit = blocking;

  llm_clam_prompt_embed(model_name, inputs, n_inputs);

  if(llm_build_embed_body(req, inputs, n_inputs) != SUCCESS)
  {
    llm_req_release(req);
    return(FAIL);
  }

  clock_gettime(CLOCK_MONOTONIC, &req->started);
  llm_active_add(req);

  if(llm_issue_request(req) != SUCCESS)
  {
    llm_active_remove(req);
    llm_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
llm_embed_submit(const char *model_name,
    const char *const *inputs, size_t n_inputs,
    llm_embed_done_cb_t done_cb, void *user_data)
{
  return(llm_embed_submit_impl(model_name, inputs, n_inputs,
      done_cb, user_data, false)); // blocking
}

bool
llm_embed_submit_wait(const char *model_name,
    const char *const *inputs, size_t n_inputs,
    llm_embed_done_cb_t done_cb, void *user_data)
{
  return(llm_embed_submit_impl(model_name, inputs, n_inputs,
      done_cb, user_data, true)); // blocking
}

bool
llm_image_submit(const char *model_name,
    const llm_image_params_t *params, const char *prompt,
    llm_image_done_cb_t done_cb, void *user_data)
{
  llm_model_t    m;
  llm_request_t *req;

  if(!llm_ready || model_name == NULL || prompt == NULL || prompt[0] == '\0'
      || done_cb == NULL)
    return(FAIL);

  if(llm_models_snapshot(model_name, &m) != SUCCESS || !m.enabled
      || m.kind != LLM_KIND_IMAGE)
  {
    clam(CLAM_WARN, "llm", "unknown/disabled image model: %s", model_name);
    return(FAIL);
  }

  req = llm_req_alloc();

  req->type = LLM_REQ_IMAGE;
  snprintf(req->model_name, sizeof(req->model_name), "%s", m.name);
  snprintf(req->model_id,   sizeof(req->model_id),   "%s", m.model_id);

  if(llm_build_url(m.base_url, "images/generations",
      req->endpoint_url, sizeof(req->endpoint_url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "cannot build image URL for %s (service %s)",
        m.name, m.service_name);
    llm_req_release(req);
    return(FAIL);
  }

  snprintf(req->api_key_kv, sizeof(req->api_key_kv),
      "llm.service.%s.creds.apikey", m.service_name);
  snprintf(req->service_name, sizeof(req->service_name), "%s", m.service_name);

  // Seed any learned request-dialect directives so the very first body for a
  // known-quirky model is already correct (no negotiation round-trip).
  req->n_directives = llm_model_params_lookup(m.service_name, m.model_id,
      req->directives, LLM_MAX_DIRECTIVES);

  req->kind          = m.kind;
  req->image_done_cb = done_cb;
  req->user_data     = user_data;
  req->streaming     = false;
  req->image_n       = 1;               // v1: always one image

  if(params != NULL)
  {
    if(params->size != NULL && params->size[0] != '\0')
      snprintf(req->image_size, sizeof(req->image_size), "%s", params->size);

    req->params.timeout_secs = params->timeout_secs;
  }

  if(llm_build_image_prefix(req, prompt) != SUCCESS
      || llm_compose_body(req) != SUCCESS)
  {
    llm_req_release(req);
    return(FAIL);
  }

  clock_gettime(CLOCK_MONOTONIC, &req->started);
  llm_active_add(req);

  if(llm_issue_request(req) != SUCCESS)
  {
    llm_active_remove(req);
    llm_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// The audio kinds carry no prompt worth reproducing — a WAV cannot be
// logged and a synthesis input is one short line — so they get a single
// DEBUG5 line each rather than llm_clam_prompt_*'s chunked treatment.

bool
llm_stt_submit(const char *model_name, const void *wav, size_t wav_len,
    llm_stt_done_cb_t done_cb, void *user_data)
{
  llm_model_t    m;
  llm_request_t *req;

  if(!llm_ready || model_name == NULL || wav == NULL || wav_len == 0
      || done_cb == NULL)
    return(FAIL);

  if(wav_len > LLM_STT_WAV_MAX)
  {
    clam(CLAM_WARN, "llm", "stt payload of %zu bytes exceeds the %u-byte cap",
        wav_len, LLM_STT_WAV_MAX);
    return(FAIL);
  }

  if(llm_models_snapshot(model_name, &m) != SUCCESS || !m.enabled
      || m.kind != LLM_KIND_STT)
  {
    clam(CLAM_WARN, "llm", "unknown/disabled stt model: %s", model_name);
    return(FAIL);
  }

  req = llm_req_alloc();

  req->type = LLM_REQ_STT;
  snprintf(req->model_name, sizeof(req->model_name), "%s", m.name);
  snprintf(req->model_id,   sizeof(req->model_id),   "%s", m.model_id);

  // whisper.cpp serves transcription at the server root, which is why
  // an stt service's base URL carries no /v1.
  if(llm_build_url(m.base_url, "inference",
      req->endpoint_url, sizeof(req->endpoint_url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "cannot build stt URL for %s (service %s)",
        m.name, m.service_name);
    llm_req_release(req);
    return(FAIL);
  }

  snprintf(req->api_key_kv, sizeof(req->api_key_kv),
      "llm.service.%s.creds.apikey", m.service_name);
  snprintf(req->service_name, sizeof(req->service_name), "%s", m.service_name);

  req->kind        = m.kind;
  req->stt_done_cb = done_cb;
  req->user_data   = user_data;
  req->streaming   = false;

  clam(CLAM_DEBUG5, "llm", "prompt stt submit model=%s wav=%zu bytes",
      model_name, wav_len);

  if(llm_build_stt_body(req, wav, wav_len) != SUCCESS)
  {
    llm_req_release(req);
    return(FAIL);
  }

  clock_gettime(CLOCK_MONOTONIC, &req->started);
  llm_active_add(req);

  if(llm_issue_request(req) != SUCCESS)
  {
    llm_active_remove(req);
    llm_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
llm_tts_submit(const char *model_name, const llm_tts_params_t *params,
    const char *text, llm_tts_done_cb_t done_cb, void *user_data)
{
  llm_model_t    m;
  llm_request_t *req;
  const char    *voice = NULL;
  double         speed = 0.0;

  if(!llm_ready || model_name == NULL || text == NULL || text[0] == '\0'
      || done_cb == NULL)
    return(FAIL);

  if(llm_models_snapshot(model_name, &m) != SUCCESS || !m.enabled
      || m.kind != LLM_KIND_TTS)
  {
    clam(CLAM_WARN, "llm", "unknown/disabled tts model: %s", model_name);
    return(FAIL);
  }

  req = llm_req_alloc();

  req->type = LLM_REQ_TTS;
  snprintf(req->model_name, sizeof(req->model_name), "%s", m.name);
  snprintf(req->model_id,   sizeof(req->model_id),   "%s", m.model_id);

  if(llm_build_url(m.base_url, "audio/speech",
      req->endpoint_url, sizeof(req->endpoint_url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "cannot build tts URL for %s (service %s)",
        m.name, m.service_name);
    llm_req_release(req);
    return(FAIL);
  }

  snprintf(req->api_key_kv, sizeof(req->api_key_kv),
      "llm.service.%s.creds.apikey", m.service_name);
  snprintf(req->service_name, sizeof(req->service_name), "%s", m.service_name);

  req->kind        = m.kind;
  req->tts_done_cb = done_cb;
  req->user_data   = user_data;
  req->streaming   = false;

  if(params != NULL)
  {
    voice = params->voice;
    speed = params->speed;
    req->params.timeout_secs = params->timeout_secs;
  }

  clam(CLAM_DEBUG5, "llm",
      "prompt tts submit model=%s voice=%s speed=%.3f chars=%zu: %s",
      model_name, voice != NULL && voice[0] != '\0' ? voice : "(default)",
      speed, strlen(text), text);

  if(llm_build_tts_body(req, text, voice, speed) != SUCCESS)
  {
    llm_req_release(req);
    return(FAIL);
  }

  clock_gettime(CLOCK_MONOTONIC, &req->started);
  llm_active_add(req);

  if(llm_issue_request(req) != SUCCESS)
  {
    llm_active_remove(req);
    llm_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// Stats + iteration

void
llm_get_stats(llm_stats_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));

  pthread_mutex_lock(&llm_stat_mutex);
  out->total_requests           = llm_stat_total;
  out->total_retries            = llm_stat_retries;
  out->total_errors             = llm_stat_errors;
  out->total_prompt_tokens      = llm_stat_prompt;
  out->total_completion_tokens  = llm_stat_compl;
  out->total_latency_ms         = llm_stat_time_ms;
  pthread_mutex_unlock(&llm_stat_mutex);

  pthread_mutex_lock(&llm_active_mutex);
  out->active = llm_active_count;
  pthread_mutex_unlock(&llm_active_mutex);

  out->queued = llm_queued_count;
}

void
llm_iterate_active(llm_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&llm_active_mutex);

  for(llm_request_t *r = llm_active_head; r != NULL; r = r->next_active)
  {
    uint32_t secs = (uint32_t)(util_ms_since(&r->started) / 1000);
    cb(r->model_name, r->kind, r->streaming, secs, data);
  }

  pthread_mutex_unlock(&llm_active_mutex);
}

// KV config

static void
llm_load_config(void)
{
  llm_cfg.max_retries        = (uint32_t)kv_get_uint("llm.max_retries");
  llm_cfg.retry_backoff_ms   = (uint32_t)kv_get_uint("llm.retry_backoff_ms");
  llm_cfg.timeout_secs       = (uint32_t)kv_get_uint("llm.timeout_secs");
  llm_cfg.max_context_tokens = (uint32_t)kv_get_uint("llm.max_context_tokens");
  llm_cfg.streaming_idle_ms  = (uint32_t)kv_get_uint("llm.streaming_idle_ms");

  if(llm_cfg.max_retries == 0)
    llm_cfg.max_retries = 1;   // at least one attempt
}

static void
llm_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  llm_load_config();
}

static void
llm_register_kv(void)
{
  kv_register("llm.default_chat_model", KV_STR, "",
      llm_kv_changed, NULL, "Default chat model name");
  kv_register("llm.default_embed_model", KV_STR, "",
      llm_kv_changed, NULL, "Default embedding model name");
  kv_register("llm.max_retries", KV_UINT32, "3",
      llm_kv_changed, NULL, "Max retries on 429/5xx/transport errors");
  kv_register("llm.retry_backoff_ms", KV_UINT32, "500",
      llm_kv_changed, NULL, "Initial retry backoff in milliseconds");
  kv_register("llm.timeout_secs", KV_UINT32, "300",
      llm_kv_changed, NULL, "Default request timeout in seconds");
  kv_register("llm.max_context_tokens", KV_UINT32, "8192",
      llm_kv_changed, NULL, "Default max context tokens");
  kv_register("llm.streaming_idle_ms", KV_UINT32, "30000",
      llm_kv_changed, NULL, "Streaming idle timeout in milliseconds");
}


// Lifecycle

// A request's done-callback belongs to whoever asked for the completion,
// and that requester can be unloaded while its answer is still in flight
// — a `/plugin reload` of a chat plugin with an LLM call outstanding.
// Nothing else can see these pointers: they live in this plugin's
// request list, not in any registry core walks, so the loader tells us
// the range and we drop them ourselves. The request itself is left to
// finish and free normally; it simply delivers to nobody.
static void
llm_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&llm_active_mutex);

  for(llm_request_t *r = llm_active_head; r != NULL; r = r->next_active)
  {
    uintptr_t cb = 0;

    switch(r->type)
    {
      case LLM_REQ_CHAT:  cb = (uintptr_t)fn_addr(&r->chat_done_cb);  break;
      case LLM_REQ_EMBED: cb = (uintptr_t)fn_addr(&r->embed_done_cb); break;
      case LLM_REQ_IMAGE: cb = (uintptr_t)fn_addr(&r->image_done_cb); break;
      case LLM_REQ_STT:   cb = (uintptr_t)fn_addr(&r->stt_done_cb);   break;
      case LLM_REQ_TTS:   cb = (uintptr_t)fn_addr(&r->tts_done_cb);   break;
    }

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    r->chat_done_cb  = NULL;
    r->embed_done_cb = NULL;
    r->image_done_cb = NULL;
    r->stt_done_cb   = NULL;
    r->tts_done_cb   = NULL;
    r->user_data     = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&llm_active_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, "llm", "%u in-flight request(s) lost their requester "
        "to an unload; they will complete and deliver nothing", orphaned);
}

void
llm_init(void)
{
  if(llm_ready)
    return;

  pthread_mutex_init(&llm_req_mutex, NULL);
  pthread_mutex_init(&llm_active_mutex, NULL);
  pthread_mutex_init(&llm_stat_mutex, NULL);
  pthread_rwlock_init(&llm_models_lock, NULL);
  pthread_rwlock_init(&llm_services_lock, NULL);
  pthread_rwlock_init(&llm_model_params_lock, NULL);

  llm_cfg.max_retries        = LLM_DEF_MAX_RETRIES;
  llm_cfg.retry_backoff_ms   = LLM_DEF_RETRY_BACKOFF_MS;
  llm_cfg.timeout_secs       = LLM_DEF_TIMEOUT_SECS;
  llm_cfg.max_context_tokens = LLM_DEF_MAX_CONTEXT;
  llm_cfg.streaming_idle_ms  = LLM_DEF_STREAMING_IDLE_MS;

  plugin_unmap_notify_register(llm_unmap_cb, NULL);

  llm_ready = true;

  clam(CLAM_INFO, "llm", "llm subsystem initialized");
}

void
llm_register_config(void)
{
  llm_register_kv();
  llm_load_config();
  llm_ensure_tables();
  llm_services_reload();
  llm_models_reload();
  llm_model_params_reload();

  // Warm each service's /models cache (best-effort, async). Safe here:
  // curl started with the plugin, and a failure just leaves the cache
  // empty until the first manual `llm service <name> refresh`.
  llm_services_refresh_all();
}

void
llm_stop(void)
{
  if(!llm_ready)
    return;

  llm_stopping = true;
}

void
llm_exit(void)
{
  uint32_t leaked;
  if(!llm_ready)
    return;

  llm_ready    = false;
  llm_stopping = false;

  plugin_unmap_notify_unregister(llm_unmap_cb);

  // Drain the in-flight list. Anything still here has a done-callback
  // in this plugin's .text that we can no longer deliver — which is
  // why llm_stop() ran first and why the count is worth naming.
  pthread_mutex_lock(&llm_active_mutex);

  leaked = llm_active_count;

  if(leaked > 0)
    clam(CLAM_WARN, "llm", "%u request(s) abandoned at shutdown", leaked);

  llm_active_head  = NULL;
  llm_active_count = 0;

  pthread_mutex_unlock(&llm_active_mutex);

  // Free freelist.
  pthread_mutex_lock(&llm_req_mutex);

  while(llm_req_free != NULL)
  {
    llm_request_t *r = llm_req_free;
    llm_req_free = r->next_free;

    if(r->body_prefix != NULL) mem_free(r->body_prefix);
    if(r->req_body   != NULL) mem_free(r->req_body);
    if(r->assembled  != NULL) mem_free(r->assembled);
    if(r->vec_block  != NULL) mem_free(r->vec_block);
    if(r->vectors    != NULL) mem_free((void *)r->vectors);
    if(r->sse_parser != NULL) sse_parser_free(r->sse_parser);

    mem_free(r);
  }

  pthread_mutex_unlock(&llm_req_mutex);

  llm_models_clear();
  llm_services_clear();
  llm_model_params_clear();

  pthread_mutex_destroy(&llm_req_mutex);
  pthread_mutex_destroy(&llm_active_mutex);
  pthread_mutex_destroy(&llm_stat_mutex);
  pthread_rwlock_destroy(&llm_models_lock);
  pthread_rwlock_destroy(&llm_services_lock);
  pthread_rwlock_destroy(&llm_model_params_lock);

  clam(CLAM_INFO, "llm", "llm subsystem shut down");
}
