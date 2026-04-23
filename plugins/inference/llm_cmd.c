// botmanager — MIT
// LLM subsystem admin commands: /llm (add/set/del/probe/test/list) and /show llm.

#include "llm_priv.h"

#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "json.h"
#include "method.h"
#include "task.h"
#include "userns.h"
#include "util.h"

#include <json-c/json.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Commands: /llm *, /show llm

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} llm_list_state_t;

static void
llm_list_iter_cb(const char *name, llm_kind_t kind,
    const char *endpoint_url, const char *model_id, uint32_t embed_dim,
    uint32_t max_context, float default_temp, bool enabled, void *user)
{
  llm_list_state_t *st;
  char line[1024];  // fits LLM_ENDPOINT_SZ + model_id + name + prefix bits
  char dim_col[32];
  (void)default_temp;
  st = user;

  // Only embed models have a meaningful dim — chat models carry 0.
  if(kind == LLM_KIND_EMBED)
    snprintf(dim_col, sizeof(dim_col), "dim=%u  ", embed_dim);
  else
    dim_col[0] = '\0';

  snprintf(line, sizeof(line),
      "%s  %-5s  %s  id=%s  %sctx=%u  %s",
      enabled ? "on " : "off",
      llm_kind_to_str(kind),
      name,
      model_id,
      dim_col,
      max_context,
      endpoint_url);

  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_llm_list(const cmd_ctx_t *ctx)
{
  llm_list_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx, "registered llm models:");
  llm_model_iterate(llm_list_iter_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// Endpoint auto-probe
//
// On `llm add`, and on demand via `llm probe <name>`, we introspect the
// upstream OpenAI-compatible API to discover values the operator would
// otherwise have to look up and type in:
//
//   * max_context — GET /v1/models, field `max_model_len`.
//                   Runs for every model (chat and embed). vLLM
//                   reports it; OpenAI's cloud API does not, in
//                   which case the KV default stays in effect.
//
//   * embed_dim   — POST /v1/embeddings with a one-byte input and
//                   count the returned vector. Runs only for embed
//                   models. The stored dim cannot be changed after
//                   the first time it's set: a mismatched probe
//                   logs a WARN and is ignored (the pgvector column
//                   on conversation_embeddings is typed to the dim
//                   we stored initially).
//
// Both probes run asynchronously on the curl worker; the command
// handler acks synchronously and the probe CLAMs its result.

typedef struct
{
  char name[LLM_MODEL_NAME_SZ];
  char model_id[LLM_MODEL_ID_SZ];
} llm_probe_ctx_t;

typedef struct
{
  char     name[LLM_MODEL_NAME_SZ];
  char     model_id[LLM_MODEL_ID_SZ];
  uint32_t expected_dim;   // 0 = no prior dim, accept whatever the probe returns
} llm_probe_dim_ctx_t;

// Rewrite an endpoint URL like "http://host:port/v1/chat/completions"
// into "http://host:port/v1/models". If the endpoint doesn't contain
// "/v1/" at all, append "/v1/models" to whatever base was given.
static bool
llm_probe_build_url(const char *endpoint, char *out, size_t out_sz)
{
  const char *v1;
  size_t      base_len;
  int n;
  if(endpoint == NULL || endpoint[0] == '\0')
    return(false);

  v1 = strstr(endpoint, "/v1/");
  base_len = v1 != NULL
                         ? (size_t)(v1 - endpoint)
                         : strlen(endpoint);

  n = snprintf(out, out_sz, "%.*s/v1/models",
      (int)base_len, endpoint);

  return(n > 0 && (size_t)n < out_sz);
}

static int64_t
llm_probe_extract_max(struct json_object *root, const char *model_id,
    char *mismatch_out, size_t mismatch_sz)
{
  struct json_object *data;
  int n;
  if(mismatch_out != NULL && mismatch_sz > 0)
    mismatch_out[0] = '\0';

  data = json_get_array(root, "data");
  if(data == NULL)
    return(0);

  n = (int)json_object_array_length(data);
  if(n <= 0)
    return(0);

  for(int i = 0; i < n; i++)
  {
    struct json_object *item = json_object_array_get_idx(data, i);
    char    item_id[LLM_MODEL_ID_SZ];
    int64_t max_len;
    if(item == NULL)
      continue;

    memset(item_id, 0, sizeof(item_id));
    max_len = 0;

    json_get_str(item, "id", item_id, sizeof(item_id));

    if(strcmp(item_id, model_id) == 0
        && json_get_int64(item, "max_model_len", &max_len)
        && max_len > 0)
      return(max_len);
  }

  // Fallback: one-item response with a non-matching id — trust it for
  // max_context, but surface the mismatch so the caller can flag it.
  if(n == 1)
  {
    struct json_object *item = json_object_array_get_idx(data, 0);
    char    item_id[LLM_MODEL_ID_SZ];
    int64_t max_len;
    if(item == NULL)
      return(0);

    memset(item_id, 0, sizeof(item_id));
    max_len = 0;

    json_get_str(item, "id", item_id, sizeof(item_id));

    if(strcmp(item_id, model_id) != 0
        && mismatch_out != NULL && mismatch_sz > 0)
      snprintf(mismatch_out, mismatch_sz, "%s", item_id);

    if(json_get_int64(item, "max_model_len", &max_len) && max_len > 0)
      return(max_len);
  }

  return(0);
}

static void
llm_probe_max_context_done_cb(const curl_response_t *resp)
{
  llm_probe_ctx_t *pctx = resp->user_data;

  struct json_object *root;
  char    upstream_id[LLM_MODEL_ID_SZ];
  char *e_name;
  db_result_t *res;
  int64_t max_len;
  char  sql[512];
  if(resp->status != 200 || resp->body == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: http=%ld curl=%d %s",
        pctx->name, resp->status, resp->curl_code,
        resp->error ? resp->error : "");
    mem_free(pctx);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len,
      "llm:probe");
  if(root == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: response was not valid JSON", pctx->name);
    mem_free(pctx);
    return;
  }

  memset(upstream_id, 0, sizeof(upstream_id));
  max_len = llm_probe_extract_max(root, pctx->model_id,
      upstream_id, sizeof(upstream_id));
  json_object_put(root);

  if(upstream_id[0] != '\0')
    clam(CLAM_WARN, "llm",
        "probe %s: endpoint advertises model_id='%s' but stored id='%s' "
        "— other probes (e.g. embed_dim) may 404; fix with "
        "`llm set %s model_id %s`",
        pctx->name, upstream_id, pctx->model_id,
        pctx->name, upstream_id);

  if(max_len <= 0)
  {
    clam(CLAM_INFO, "llm",
        "probe %s: endpoint did not report max_model_len for id=%s "
        "(keeping current value)",
        pctx->name, pctx->model_id);
    mem_free(pctx);
    return;
  }

  // Sanity cap: nothing in production today exceeds a few million
  // tokens, so a wild value likely means we parsed garbage.
  if(max_len > (int64_t)10 * 1000 * 1000)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: max_model_len=%" PRId64 " looks bogus, ignoring",
        pctx->name, max_len);
    mem_free(pctx);
    return;
  }

  e_name = db_escape(pctx->name);
  snprintf(sql, sizeof(sql),
      "UPDATE llm_models SET max_context=%" PRId64 " WHERE name='%s'",
      max_len, e_name);
  mem_free(e_name);

  res = db_result_alloc();
  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: update failed: %s",
        pctx->name, res->error);
    db_result_free(res);
    mem_free(pctx);
    return;
  }
  db_result_free(res);

  llm_models_reload();

  clam(CLAM_INFO, "llm",
      "probe %s: max_context=%" PRId64 " (auto-detected)",
      pctx->name, max_len);

  mem_free(pctx);
}

static bool
llm_probe_max_context_submit(const char *name)
{
  llm_probe_ctx_t *pctx;
  char endpoint[LLM_ENDPOINT_SZ];
  char url[LLM_ENDPOINT_SZ + 32];
  bool have_model;
  if(name == NULL || name[0] == '\0')
    return(false);

  pctx = mem_alloc("llm", "probe_ctx", sizeof(*pctx));
  if(pctx == NULL)
    return(false);

  memset(endpoint, 0, sizeof(endpoint));
  have_model = false;

  pthread_rwlock_rdlock(&llm_models_lock);
  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0)
    {
      snprintf(endpoint, sizeof(endpoint), "%s", m->endpoint_url);
      snprintf(pctx->name, sizeof(pctx->name), "%s", m->name);
      snprintf(pctx->model_id, sizeof(pctx->model_id), "%s", m->model_id);
      have_model = true;
      break;
    }
  }
  pthread_rwlock_unlock(&llm_models_lock);

  if(!have_model)
  {
    mem_free(pctx);
    return(false);
  }

  if(!llm_probe_build_url(endpoint, url, sizeof(url)))
  {
    clam(CLAM_WARN, "llm",
        "probe %s: could not derive /v1/models URL from endpoint", name);
    mem_free(pctx);
    return(false);
  }

  // curl_get uses the project's SUCCESS=false / FAIL=true convention —
  // NOT native C truthiness. "if(!curl_get(...))" would invert the
  // check, free pctx while the request is still in flight, and leave
  // the callback to use-after-free plus double-free.
  if(curl_get(url, llm_probe_max_context_done_cb, pctx) != SUCCESS)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: curl submit failed for %s", name, url);
    mem_free(pctx);
    return(false);
  }

  return(true);
}

// Count the float entries in data[0].embedding from a /v1/embeddings
// response root. Returns 0 if the shape doesn't match expectations.
static size_t
llm_probe_count_embedding(struct json_object *root)
{
  struct json_object *data = json_get_array(root, "data");
  struct json_object *first;
  struct json_object *emb;
  int n;
  if(data == NULL || json_object_array_length(data) <= 0)
    return(0);

  first = json_object_array_get_idx(data, 0);
  if(first == NULL)
    return(0);

  emb = json_get_array(first, "embedding");
  if(emb == NULL)
    return(0);

  n = (int)json_object_array_length(emb);
  return(n > 0 ? (size_t)n : 0);
}

static void
llm_probe_embed_dim_done_cb(const curl_response_t *resp)
{
  llm_probe_dim_ctx_t *pctx = resp->user_data;

  struct json_object *root;
  size_t dim;
  char *e_name;
  db_result_t *res;
  char  sql[512];
  if(resp->status != 200 || resp->body == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim http=%ld curl=%d %s",
        pctx->name, resp->status, resp->curl_code,
        resp->error ? resp->error : "");
    mem_free(pctx);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len,
      "llm:probe_dim");
  if(root == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed response was not valid JSON", pctx->name);
    mem_free(pctx);
    return;
  }

  dim = llm_probe_count_embedding(root);
  json_object_put(root);

  if(dim == 0 || dim > 65536)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: could not count embedding (got %zu)",
        pctx->name, dim);
    mem_free(pctx);
    return;
  }

  if(pctx->expected_dim != 0 && (uint32_t)dim != pctx->expected_dim)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: endpoint reports embed_dim=%zu but %u is stored; "
        "refusing to change — use `llm del` + `llm add` to switch",
        pctx->name, dim, pctx->expected_dim);
    mem_free(pctx);
    return;
  }

  if(pctx->expected_dim == (uint32_t)dim)
  {
    // Idempotent re-probe — no DB write needed.
    clam(CLAM_INFO, "llm",
        "probe %s: embed_dim=%zu (confirmed)", pctx->name, dim);
    mem_free(pctx);
    return;
  }

  e_name = db_escape(pctx->name);
  snprintf(sql, sizeof(sql),
      "UPDATE llm_models SET embed_dim=%zu WHERE name='%s'",
      dim, e_name);
  mem_free(e_name);

  res = db_result_alloc();
  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim update failed: %s",
        pctx->name, res->error);
    db_result_free(res);
    mem_free(pctx);
    return;
  }
  db_result_free(res);

  llm_models_reload();

  clam(CLAM_INFO, "llm",
      "probe %s: embed_dim=%zu (auto-detected)", pctx->name, dim);

  mem_free(pctx);
}

static bool
llm_probe_embed_dim_submit(const char *name)
{
  llm_probe_dim_ctx_t *pctx;
  char endpoint[LLM_ENDPOINT_SZ];
  char escaped_id[LLM_MODEL_ID_SZ * 2];
  char body[LLM_MODEL_ID_SZ * 2 + 64];
  bool have_model;
  int  body_len;
  if(name == NULL || name[0] == '\0')
    return(false);

  pctx = mem_alloc("llm", "probe_dim_ctx",
      sizeof(*pctx));
  if(pctx == NULL)
    return(false);

  memset(endpoint, 0, sizeof(endpoint));
  have_model = false;

  pthread_rwlock_rdlock(&llm_models_lock);
  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0 && m->kind == LLM_KIND_EMBED)
    {
      snprintf(endpoint, sizeof(endpoint), "%s", m->endpoint_url);
      snprintf(pctx->name, sizeof(pctx->name), "%s", m->name);
      snprintf(pctx->model_id, sizeof(pctx->model_id), "%s", m->model_id);
      pctx->expected_dim = m->embed_dim;
      have_model = true;
      break;
    }
  }
  pthread_rwlock_unlock(&llm_models_lock);

  if(!have_model)
  {
    mem_free(pctx);
    return(false);
  }

  // Build the POST body. Use "x" as the input — shortest payload that
  // still returns a full-dim vector. JSON-escape model_id defensively
  // in case it contains a slash or quote (it usually doesn't).
  json_escape(pctx->model_id, escaped_id, sizeof(escaped_id));

  body_len = snprintf(body, sizeof(body),
      "{\"model\":\"%s\",\"input\":\"x\"}", escaped_id);
  if(body_len <= 0 || (size_t)body_len >= sizeof(body))
  {
    mem_free(pctx);
    return(false);
  }

  if(curl_post(endpoint, "application/json", body, (size_t)body_len,
      llm_probe_embed_dim_done_cb, pctx) != SUCCESS)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim curl submit failed for %s",
        name, endpoint);
    mem_free(pctx);
    return(false);
  }

  return(true);
}

// Dispatcher called by cmd_llm_add and cmd_llm_probe. Always fires the
// max_context probe; additionally fires the embed_dim probe for embed
// models. returns: true if at least one probe was submitted.
static bool
llm_probe_submit(const char *name)
{
  bool any = false;

  llm_kind_t kind;
  if(llm_probe_max_context_submit(name))
    any = true;

  if(llm_model_kind(name, &kind) == SUCCESS && kind == LLM_KIND_EMBED)
  {
    if(llm_probe_embed_dim_submit(name))
      any = true;
  }

  return(any);
}

static void
cmd_llm_probe(const cmd_ctx_t *ctx)
{
  const char *name;
  char msg[256];
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm probe <name>");
    return;
  }

  name = ctx->parsed->argv[0];

  if(!llm_model_exists(name))
  {
    cmd_reply(ctx, "error: no such model");
    return;
  }

  if(!llm_probe_submit(name))
  {
    cmd_reply(ctx, "probe: failed to submit (see CLAM log)");
    return;
  }

  snprintf(msg, sizeof(msg),
      "probe submitted for %s — result will appear in CLAM log",
      name);
  cmd_reply(ctx, msg);
}

static void
cmd_llm_add(const cmd_ctx_t *ctx)
{
  const char *name;
  uint32_t dim;
  db_result_t *res;
  const char *kind;
  char *e_name;
  const char *url;
  char *e_kind;
  const char *mid;
  char *e_url;
  const char *dim_s;
  char *e_mid;
  llm_kind_t k;
  char sql[2048];
  if(ctx->parsed == NULL || ctx->parsed->argc < 4)
  {
    cmd_reply(ctx, "usage: llm add <name> <chat|embed> <url> <model_id> [dim]");
    return;
  }

  name = ctx->parsed->argv[0];
  kind = ctx->parsed->argv[1];
  url = ctx->parsed->argv[2];
  mid = ctx->parsed->argv[3];
  dim_s = ctx->parsed->argc > 4 ? ctx->parsed->argv[4] : "0";


  if(llm_kind_from_str(kind, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: kind must be 'chat' or 'embed'");
    return;
  }

  dim = (uint32_t)strtoul(dim_s, NULL, 10);

  // dim=0 is fine here: for embed models the probe will fill it in
  // from /v1/embeddings; for chat models it stays 0 (unused).

  e_name = db_escape(name);
  e_kind = db_escape(kind);
  e_url = db_escape(url);
  e_mid = db_escape(mid);

  // Seed max_context from the KV default. The probe fired below may
  // overwrite it with the endpoint's reported value a moment later.
  snprintf(sql, sizeof(sql),
      "INSERT INTO llm_models (name, kind, endpoint_url, model_id, "
      "embed_dim, max_context) "
      "VALUES ('%s', '%s', '%s', '%s', %u, %u)",
      e_name, e_kind, e_url, e_mid, dim, llm_cfg.max_context_tokens);

  mem_free(e_name); mem_free(e_kind); mem_free(e_url); mem_free(e_mid);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char msg[512];
    snprintf(msg, sizeof(msg), "insert failed: %s", res->error);
    cmd_reply(ctx, msg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();

  // Ask the endpoint what its real capabilities are. For chat models
  // the probe hits /v1/models (max_model_len). For embed models it
  // additionally hits /v1/embeddings to count the output dim. On
  // failure the seeded values we just inserted stay in effect.
  if(!llm_probe_submit(name))
  {
    cmd_reply(ctx, "ok (endpoint probe could not start — see CLAM log)");
    return;
  }

  if(k == LLM_KIND_EMBED)
    cmd_reply(ctx, "ok (probing endpoint for max_context and embed_dim)");
  else
    cmd_reply(ctx, "ok (probing endpoint for max_context)");
}

static void
cmd_llm_set(const cmd_ctx_t *ctx)
{
  const char *name;
  char *e_name;
  db_result_t *res;
  const char *field;
  char *e_value;
  const char *value;
  char sql[2048];
  bool numeric;
  bool floating;
  bool boolean;
  if(ctx->parsed == NULL || ctx->parsed->argc < 3)
  {
    cmd_reply(ctx, "usage: llm set <name> <field> <value>");
    return;
  }

  name = ctx->parsed->argv[0];
  field = ctx->parsed->argv[1];
  value = ctx->parsed->argv[2];

  // Allowed fields and their SQL types.
  numeric = false;
  floating = false;
  boolean = false;

  if(strcmp(field, "endpoint_url") == 0
      || strcmp(field, "model_id") == 0
      || strcmp(field, "api_key_kv") == 0)
    ;
  else if(strcmp(field, "max_context") == 0)
    numeric = true;
  else if(strcmp(field, "default_temp") == 0)
    floating = true;
  else if(strcmp(field, "enabled") == 0)
    boolean = true;
  else if(strcmp(field, "embed_dim") == 0)
  {
    // Changing embed_dim would misalign every vector already stored in
    // the conversation_embeddings pgvector column. The canonical fix
    // for a wrong dim is `llm del` + `llm add` (which reprobes and
    // ensures the embeddings table is consistent with the new model).
    cmd_reply(ctx, "error: embed_dim cannot be changed after creation "
        "(pgvector column is typed to the original dim); "
        "use `llm del` + `llm add` instead");
    return;
  }

  else
  {
    cmd_reply(ctx, "error: unknown field");
    return;
  }

  e_name = db_escape(name);
  e_value = db_escape(value);

  if(numeric)
    snprintf(sql, sizeof(sql),
        "UPDATE llm_models SET %s=%lu WHERE name='%s'",
        field, strtoul(value, NULL, 10), e_name);
  else if(floating)
    snprintf(sql, sizeof(sql),
        "UPDATE llm_models SET %s=%f WHERE name='%s'",
        field, strtod(value, NULL), e_name);
  else if(boolean)
    snprintf(sql, sizeof(sql),
        "UPDATE llm_models SET %s=%s WHERE name='%s'",
        field,
        (value[0] == 't' || value[0] == 'T' || value[0] == '1')
            ? "TRUE" : "FALSE",
        e_name);
  else
    snprintf(sql, sizeof(sql),
        "UPDATE llm_models SET %s='%s' WHERE name='%s'",
        field, e_value, e_name);

  mem_free(e_name); mem_free(e_value);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char msg[512];
    snprintf(msg, sizeof(msg), "update failed: %s", res->error);
    cmd_reply(ctx, msg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();

  cmd_reply(ctx, "ok");
}

static void
cmd_llm_del(const cmd_ctx_t *ctx)
{
  char *e_name;
  db_result_t *res;
  char sql[256];
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm del <name>");
    return;
  }

  e_name = db_escape(ctx->parsed->argv[0]);

  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_models WHERE name='%s'", e_name);

  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char msg[512];
    snprintf(msg, sizeof(msg), "delete failed: %s", res->error);
    cmd_reply(ctx, msg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();

  cmd_reply(ctx, "ok");
}

// /llm test: synchronous probe.
typedef struct
{
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  bool            done;
  bool            ok;
  long            status;
  char            content[512];
  char            err[256];
} llm_test_sync_t;

static void
cmd_llm_test_done(const llm_chat_response_t *resp)
{
  llm_test_sync_t *s = resp->user_data;

  pthread_mutex_lock(&s->mu);

  s->ok     = resp->ok;
  s->status = resp->http_status;

  if(resp->content != NULL)
  {
    size_t n = resp->content_len < sizeof(s->content) - 1
        ? resp->content_len : sizeof(s->content) - 1;
    memcpy(s->content, resp->content, n);
    s->content[n] = '\0';
  }

  if(resp->error != NULL)
    snprintf(s->err, sizeof(s->err), "%s", resp->error);

  s->done = true;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
}

static void
cmd_llm_test_embed_done(const llm_embed_response_t *resp)
{
  llm_test_sync_t *s = resp->user_data;

  pthread_mutex_lock(&s->mu);

  s->ok     = resp->ok;
  s->status = resp->http_status;

  if(resp->ok)
    snprintf(s->content, sizeof(s->content),
        "dim=%u vectors=%zu", resp->dim, resp->n_vectors);

  if(resp->error != NULL)
    snprintf(s->err, sizeof(s->err), "%s", resp->error);

  s->done = true;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
}

static void
cmd_llm_test(const cmd_ctx_t *ctx)
{
  const char *name;
  llm_test_sync_t s;
  struct timespec t0;
  bool submitted;
  struct timespec until;
  bool done;
  struct timespec t1;
  uint64_t ms;
  const char *prompt;
  char line[640];
  llm_kind_t k;
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm test <name> [prompt...]");
    return;
  }

  name = ctx->parsed->argv[0];
  prompt = ctx->parsed->argc > 1 ? ctx->parsed->argv[1] : "ping";


  if(llm_model_kind(name, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: unknown model");
    return;
  }

  memset(&s, 0, sizeof(s));
  pthread_mutex_init(&s.mu, NULL);
  pthread_cond_init(&s.cv, NULL);

  clock_gettime(CLOCK_MONOTONIC, &t0);


  if(k == LLM_KIND_CHAT)
  {
    llm_chat_params_t params = { 0 };
    llm_message_t msgs[1];
    memset(msgs, 0, sizeof(msgs));
    params.max_tokens = 32;

    msgs[0].role    = LLM_ROLE_USER;
    msgs[0].content = prompt;

    submitted = (llm_chat_submit(name, &params, msgs, 1,
        cmd_llm_test_done, NULL, &s) == SUCCESS);
  }

  else
  {
    const char *inputs[1] = { prompt };
    submitted = (llm_embed_submit(name, inputs, 1,
        cmd_llm_test_embed_done, &s) == SUCCESS);
  }

  if(!submitted)
  {
    cmd_reply(ctx, "error: submit failed");
    goto cleanup;
  }

  // Wait up to 15 seconds.
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 15;

  pthread_mutex_lock(&s.mu);

  while(!s.done)
    if(pthread_cond_timedwait(&s.cv, &s.mu, &until) != 0)
      break;

  done = s.done;
  pthread_mutex_unlock(&s.mu);

  if(!done)
  {
    cmd_reply(ctx, "timeout (15s)");
    goto cleanup;
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  ms = util_ms_since(&t0);

  if(s.ok)
    snprintf(line, sizeof(line),
        "ok (%lums, http %ld): %s",
        (unsigned long)ms, s.status, s.content);
  else
    snprintf(line, sizeof(line),
        "failed (%lums, http %ld): %s",
        (unsigned long)ms, s.status, s.err);

  cmd_reply(ctx, line);

  (void)t1;

cleanup:
  pthread_mutex_destroy(&s.mu);
  pthread_cond_destroy(&s.cv);
}

// /show llm
static void
llm_show_iter_cb(const char *model_name, llm_kind_t kind, bool streaming,
    uint32_t elapsed_secs, void *data)
{
  llm_list_state_t *st = data;
  char line[256];

  snprintf(line, sizeof(line),
      "  %-5s  %s  streaming=%s  elapsed=%us",
      llm_kind_to_str(kind),
      model_name,
      streaming ? "y" : "n",
      elapsed_secs);

  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_llm(const cmd_ctx_t *ctx)
{
  llm_stats_t s;
  char buf[512];

  uint64_t avg_ms;
  llm_list_state_t models;
  llm_get_stats(&s);

  avg_ms = s.total_requests > 0
      ? s.total_latency_ms / s.total_requests : 0;

  snprintf(buf, sizeof(buf),
      "llm: %u active, %lu requests, %lu errors, %lu retries, avg %lu ms",
      s.active,
      (unsigned long)s.total_requests,
      (unsigned long)s.total_errors,
      (unsigned long)s.total_retries,
      (unsigned long)avg_ms);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  tokens: prompt=%lu completion=%lu",
      (unsigned long)s.total_prompt_tokens,
      (unsigned long)s.total_completion_tokens);
  cmd_reply(ctx, buf);

  cmd_reply(ctx, "models:");
  models = (llm_list_state_t){ .ctx = ctx, .count = 0 };
  llm_model_iterate(llm_list_iter_cb, &models);
  if(models.count == 0)
    cmd_reply(ctx, "  (none)");

  if(s.active > 0)
  {
    llm_list_state_t st;
    cmd_reply(ctx, "in-flight:");

    st = (llm_list_state_t){ .ctx = ctx, .count = 0 };
    llm_iterate_active(llm_show_iter_cb, &st);
  }
}

// Argument descriptors.
static const cmd_arg_desc_t ad_llm_add[] = {
  { "name",     CMD_ARG_NONE,   CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ, NULL },
  { "kind",     CMD_ARG_NONE,   CMD_ARG_REQUIRED, 16,                NULL },
  { "url",      CMD_ARG_NONE,   CMD_ARG_REQUIRED, LLM_ENDPOINT_SZ,   NULL },
  { "model_id", CMD_ARG_NONE,   CMD_ARG_REQUIRED, LLM_MODEL_ID_SZ,   NULL },
  { "dim",      CMD_ARG_DIGITS, CMD_ARG_OPTIONAL, 10,                NULL },
};

static const cmd_arg_desc_t ad_llm_set[] = {
  { "name",  CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ, NULL },
  { "field", CMD_ARG_NONE, CMD_ARG_REQUIRED, 32,                NULL },
  { "value", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0,  NULL },
};

static const cmd_arg_desc_t ad_llm_del[] = {
  { "name", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_llm_probe[] = {
  { "name", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_llm_test[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ, NULL },
  { "prompt", CMD_ARG_NONE, CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,  NULL },
};

static void
cmd_llm_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /llm <subcommand> ...");
}

void
llm_register_commands(void)
{
  cmd_register("llm", "llm",
      "llm",
      "LLM model registry",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_llm_root, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  cmd_register("llm", "add",
      "llm add <name> <chat|embed> <url> <model_id> [dim]",
      "Register an LLM model",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_llm_add, NULL, "llm", "a", ad_llm_add,
      (uint8_t)(sizeof(ad_llm_add) / sizeof(ad_llm_add[0])), NULL, NULL);

  cmd_register("llm", "set",
      "llm set <name> <field> <value>",
      "Modify an LLM model field",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_llm_set, NULL, "llm", "s", ad_llm_set,
      (uint8_t)(sizeof(ad_llm_set) / sizeof(ad_llm_set[0])), NULL, NULL);

  cmd_register("llm", "del",
      "llm del <name>",
      "Delete an LLM model",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_llm_del, NULL, "llm", "d", ad_llm_del, 1, NULL, NULL);

  // Abbrev is "pb" not "p": the llm-bot plugin claims "p" for
  // /llm personality later in init, and a collision aborts plugin load.
  cmd_register("llm", "probe",
      "llm probe <name>",
      "Query the endpoint's /v1/models for max_context and store it",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_llm_probe, NULL, "llm", "pb", ad_llm_probe,
      (uint8_t)(sizeof(ad_llm_probe) / sizeof(ad_llm_probe[0])), NULL, NULL);

  cmd_register("llm", "test",
      "llm test <name> [prompt]",
      "Probe an LLM model synchronously",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_llm_test, NULL, "llm", "t", ad_llm_test,
      (uint8_t)(sizeof(ad_llm_test) / sizeof(ad_llm_test[0])), NULL, NULL);

  cmd_register("llm", "llm",
      "show llm",
      "Show LLM subsystem state",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_llm, NULL, "show", "llm", NULL, 0, NULL, NULL);

  cmd_register("llm", "models",
      "show llm models",
      "List registered LLM models",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_list, NULL, "show/llm", "m", NULL, 0, NULL, NULL);
}
