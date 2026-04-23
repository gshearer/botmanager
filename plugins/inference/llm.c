// botmanager — MIT
// LLM client: request builder, streaming-response dispatch, personality wiring.
#include "llm_priv.h"

#include "cmd.h"
#include "curl.h"
#include "db.h"
#include "json.h"
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
  return(FAIL);
}

const char *
llm_kind_to_str(llm_kind_t k)
{
  return(k == LLM_KIND_EMBED ? "embed" : "chat");
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
  if(req->req_body != NULL)     { mem_free(req->req_body); req->req_body = NULL; }
  if(req->assembled != NULL)    { mem_free(req->assembled); req->assembled = NULL; }
  if(req->vec_block != NULL)    { mem_free(req->vec_block); req->vec_block = NULL; }
  if(req->vectors != NULL)      { mem_free((void *)req->vectors); req->vectors = NULL; }
  if(req->sse_parser != NULL)   { sse_parser_free(req->sse_parser); req->sse_parser = NULL; }

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

// Reload the cache from DB. Clears and rebuilds. Safe to call repeatedly.
void
llm_models_reload(void)
{
  db_result_t *res;
  llm_models_clear();

  res = db_result_alloc();

  if(db_query(
      "SELECT name, kind, endpoint_url, model_id, api_key_kv, embed_dim, "
      "max_context, default_temp, enabled FROM llm_models", res) != SUCCESS
      || !res->ok)
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
    const char *url;
    const char *mid;
    const char *keykv;
    const char *dim;
    const char *maxctx;
    const char *temp;
    const char *enabled;
    memset(&m, 0, sizeof(m));

    name = db_result_get(res, r, 0);
    kind = db_result_get(res, r, 1);
    url = db_result_get(res, r, 2);
    mid = db_result_get(res, r, 3);
    keykv = db_result_get(res, r, 4);
    dim = db_result_get(res, r, 5);
    maxctx = db_result_get(res, r, 6);
    temp = db_result_get(res, r, 7);
    enabled = db_result_get(res, r, 8);

    if(name == NULL || kind == NULL)
      continue;

    snprintf(m.name, sizeof(m.name), "%s", name);
    snprintf(m.endpoint_url, sizeof(m.endpoint_url), "%s", url ? url : "");
    snprintf(m.model_id, sizeof(m.model_id), "%s", mid ? mid : "");
    snprintf(m.api_key_kv, sizeof(m.api_key_kv), "%s", keykv ? keykv : "");

    if(llm_kind_from_str(kind, &m.kind) != SUCCESS)
      continue;

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

// Ensure the llm_models table exists (idempotent, mirrors schema.sql).
static void
llm_ensure_tables(void)
{
  const char *sql =
      "CREATE TABLE IF NOT EXISTS llm_models ("
      " name          VARCHAR(64)  PRIMARY KEY,"
      " kind          VARCHAR(16)  NOT NULL,"
      " endpoint_url  TEXT         NOT NULL,"
      " model_id      VARCHAR(128) NOT NULL,"
      " api_key_kv    VARCHAR(128) NOT NULL DEFAULT '',"
      " embed_dim     INTEGER      NOT NULL DEFAULT 0,"
      " max_context   INTEGER      NOT NULL DEFAULT 8192,"
      " default_temp  REAL         NOT NULL DEFAULT 0.7,"
      " enabled       BOOLEAN      NOT NULL DEFAULT TRUE,"
      " created       TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")";

  db_result_t *res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "llm", "ensure_tables: %s", res->error);
  }

  db_result_free(res);
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
    cb(m->name, m->kind, m->endpoint_url, m->model_id, m->embed_dim,
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

  snprintf(m.name, sizeof(m.name), "%s", name);
  snprintf(m.endpoint_url, sizeof(m.endpoint_url), "%s", endpoint_url);
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

  if(dup == NULL)
  {
    pthread_mutex_unlock(&llm_test_pending_mutex);
    return(FAIL);
  }

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

// Request body assembly

// Build OpenAI-compatible chat completions body into out. Caller owns out.
static bool
llm_build_chat_body(llm_request_t *req, const llm_message_t *msgs,
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

  if(req->params.temperature > 0.0f)
    llm_buf_printf(&b, ",\"temperature\":%.3f", (double)req->params.temperature);

  if(req->params.max_tokens > 0)
    llm_buf_printf(&b, ",\"max_tokens\":%u", req->params.max_tokens);

  if(req->params.stream)
    llm_buf_puts(&b, ",\"stream\":true");

  llm_buf_putc(&b, '}');

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

// Deferred-task retry trampoline.
static void
llm_retry_task(task_t *t)
{
  llm_request_t *req = (llm_request_t *)t->data;

  pthread_mutex_lock(&llm_stat_mutex);
  llm_stat_retries++;
  pthread_mutex_unlock(&llm_stat_mutex);

  // Reset per-attempt state. Keep req_body (reusable).
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

  if(llm_issue_request(req) != SUCCESS)
  {
    // Could not reissue; deliver failure.
    snprintf(req->errbuf, sizeof(req->errbuf), "retry submit failed");

    if(req->type == LLM_REQ_CHAT)
      llm_deliver_chat(req, false, 0, req->errbuf);
    else
      llm_deliver_embed(req, false, 0, req->errbuf);
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

  ok = false;

  if(resp->curl_code == 0 && resp->status >= 200 && resp->status < 300)
  {
    if(!req->streaming && resp->body != NULL && resp->body_len > 0)
    {
      if(req->type == LLM_REQ_CHAT)
        ok = (llm_parse_chat_response(req, resp->body, resp->body_len)
              == SUCCESS);
      else
        ok = (llm_parse_embed_response(req, resp->body, resp->body_len)
              == SUCCESS);
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
      // Capture a prefix of the server's response body alongside the
      // status — OpenAI-compat endpoints (vLLM, TEI, etc.) put the
      // real reason for a 4xx inside the body, and `"http 400"` on
      // its own tells us nothing. 180 bytes of the body fits under
      // LLM_ERR_SZ (256) with the status prefix.
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

  // Retry logic: only for failures, non-streaming (or streaming with no
  // bytes delivered), and within max_retries.
  if(!ok
      && req->attempt + 1 < llm_cfg.max_retries
      && llm_is_retryable(resp->status, resp->curl_code)
      && (!req->streaming || req->bytes_seen == 0))
  {
    uint32_t backoff;
    task_t *t;
    req->attempt++;

    backoff = llm_cfg.retry_backoff_ms << req->attempt;

    if(backoff > LLM_RETRY_CAP_MS)
      backoff = LLM_RETRY_CAP_MS;

    clam(CLAM_DEBUG, "llm",
        "retrying %s attempt %u in %u ms (status=%ld)",
        req->model_name, req->attempt, backoff, resp->status);

    t = task_add_deferred("llm_retry", TASK_ANY, 50,
        backoff, llm_retry_task, req);

    if(t != NULL)
      return;

    // Retry scheduling failed — fall through to deliver failure.
    snprintf(req->errbuf, sizeof(req->errbuf), "retry scheduling failed");
  }

  if(req->type == LLM_REQ_CHAT)
    llm_deliver_chat(req, ok, resp->status, ok ? NULL : req->errbuf);
  else
    llm_deliver_embed(req, ok, resp->status, ok ? NULL : req->errbuf);
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

  if(curl_request_set_body(cr, "application/json",
      req->req_body, req->req_body_len) != SUCCESS)
    goto fail;

  // Authorization header (if api key KV name configured).
  if(req->api_key_kv[0] != '\0')
  {
    const char *key = kv_get_str(req->api_key_kv);

    if(key != NULL && key[0] != '\0')
    {
      char hdr[512];
      snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", key);
      curl_request_add_header(cr, hdr);
    }
  }

  // Accept SSE or JSON depending on streaming.
  curl_request_add_header(cr, req->streaming
      ? "Accept: text/event-stream" : "Accept: application/json");

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
  snprintf(req->model_name,   sizeof(req->model_name),   "%s", m.name);
  snprintf(req->endpoint_url, sizeof(req->endpoint_url), "%s", m.endpoint_url);
  snprintf(req->model_id,     sizeof(req->model_id),     "%s", m.model_id);
  snprintf(req->api_key_kv,   sizeof(req->api_key_kv),   "%s", m.api_key_kv);
  req->kind         = m.kind;
  req->params       = *params;
  req->chat_done_cb = done_cb;
  req->chunk_cb     = chunk_cb_or_NULL;
  req->user_data    = user_data;
  req->streaming    = params->stream;

  if(req->params.temperature == 0.0f && m.default_temp > 0.0f)
    req->params.temperature = m.default_temp;

  llm_clam_prompt_chat(model_name, &req->params, messages, n_messages);

  if(llm_build_chat_body(req, messages, n_messages) != SUCCESS)
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
  snprintf(req->model_name,   sizeof(req->model_name),   "%s", m.name);
  snprintf(req->endpoint_url, sizeof(req->endpoint_url), "%s", m.endpoint_url);
  snprintf(req->model_id,     sizeof(req->model_id),     "%s", m.model_id);
  snprintf(req->api_key_kv,   sizeof(req->api_key_kv),   "%s", m.api_key_kv);
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

void
llm_init(void)
{
  if(llm_ready)
    return;

  pthread_mutex_init(&llm_req_mutex, NULL);
  pthread_mutex_init(&llm_active_mutex, NULL);
  pthread_mutex_init(&llm_stat_mutex, NULL);
  pthread_rwlock_init(&llm_models_lock, NULL);

  llm_cfg.max_retries        = LLM_DEF_MAX_RETRIES;
  llm_cfg.retry_backoff_ms   = LLM_DEF_RETRY_BACKOFF_MS;
  llm_cfg.timeout_secs       = LLM_DEF_TIMEOUT_SECS;
  llm_cfg.max_context_tokens = LLM_DEF_MAX_CONTEXT;
  llm_cfg.streaming_idle_ms  = LLM_DEF_STREAMING_IDLE_MS;

  llm_ready = true;

  clam(CLAM_INFO, "llm", "llm subsystem initialized");
}

void
llm_register_config(void)
{
  llm_register_kv();
  llm_load_config();
  llm_ensure_tables();
  llm_models_reload();
}

void
llm_exit(void)
{
  uint32_t leaked;
  if(!llm_ready)
    return;

  llm_ready = false;

  // Drain in-flight list (the curl worker is already joined; any
  // requests left here are leaked callbacks we cannot deliver).
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

    if(r->req_body   != NULL) mem_free(r->req_body);
    if(r->assembled  != NULL) mem_free(r->assembled);
    if(r->vec_block  != NULL) mem_free(r->vec_block);
    if(r->vectors    != NULL) mem_free((void *)r->vectors);
    if(r->sse_parser != NULL) sse_parser_free(r->sse_parser);

    mem_free(r);
  }

  pthread_mutex_unlock(&llm_req_mutex);

  llm_models_clear();

  pthread_mutex_destroy(&llm_req_mutex);
  pthread_mutex_destroy(&llm_active_mutex);
  pthread_mutex_destroy(&llm_stat_mutex);
  pthread_rwlock_destroy(&llm_models_lock);

  clam(CLAM_INFO, "llm", "llm subsystem shut down");
}
