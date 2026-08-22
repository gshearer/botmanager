// botmanager — MIT
// LLM registry admin commands: /llm (add/del service|model, service
// refresh, probe, test) and /show llm [models|service].

#include "llm_priv.h"

#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "json.h"
#include "method.h"
#include "userns.h"
#include "util.h"

#include <json-c/json.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Commands: /llm *, /show llm

// The canonical wire id for a model, given whatever an operator typed or a
// provider listed.
//
// Google namespaces its /models listing — "models/gemini-3.5-flash" — but
// the id itself is the bare tail. Both forms reach chat completions, so the
// prefix looks harmless right up until someone registers an image model
// with it: /images/generations accepts ONLY the bare form and answers the
// other with a 404 that blames the model for not existing. Worse, the
// listing is what the registration warning checks against, so the prefixed
// id — the broken one — is the form that registers quietly.
//
// Canonicalising on the way in, into the cache and into a registration,
// means every kind gets an id that works, `show llm models` reads
// uniformly, and the cached listing is something an operator can paste
// straight into `llm add model`.
//
// The test is the exact literal prefix, so a provider whose ids genuinely
// contain a slash (vLLM's "nvidia/Gemma-4-31B-IT-NVFP4") is untouched.
static const char *
llm_model_id_canon(const char *model_id)
{
  static const char prefix[] = "models/";

  if(model_id != NULL && strncmp(model_id, prefix, sizeof(prefix) - 1) == 0)
    return(model_id + sizeof(prefix) - 1);

  return(model_id);
}

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} llm_list_state_t;

static void
llm_list_iter_cb(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id, uint32_t embed_dim,
    uint32_t max_context, float default_temp, bool enabled, void *user)
{
  llm_list_state_t *st;
  char line[1024];  // fits service name + model_id + name + prefix bits
  char dim_col[32];
  (void)default_temp;
  st = user;

  // Only embed models have a meaningful dim — chat models carry 0.
  if(kind == LLM_KIND_EMBED)
    snprintf(dim_col, sizeof(dim_col), "dim=%u  ", embed_dim);
  else
    dim_col[0] = '\0';

  snprintf(line, sizeof(line),
      "%s  %-5s  %s  %s  id=%s  %sctx=%u",
      enabled ? "on " : "off",
      llm_kind_to_str(kind),
      name,
      service_name,
      model_id,
      dim_col,
      max_context);

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

// -----------------------------------------------------------------------
// Service /models cache refresh
//
// A service's /models list is fetched asynchronously and cached in the
// llm_service_models table so `show llm service <name> models` is a fast
// DB read. Fired on `llm add service`, `llm service <name> refresh`, and
// once per service at startup (llm_services_refresh_all). No auth header
// is attached — a service whose /models needs a key simply caches nothing
// and models are added by hand (the add path warns, never blocks).
// -----------------------------------------------------------------------

typedef struct
{
  char name[LLM_MODEL_NAME_SZ];
} llm_refresh_ctx_t;

// Replace the cached model list for one service from a /models JSON body:
// clear the service's rows, then upsert one per data[] item (id + optional
// max_model_len), and stamp llm_services.refreshed. Returns the number of
// models cached, or -1 on a DB error.
static int
llm_service_models_store(const char *service, struct json_object *root)
{
  struct json_object *data;
  char        *e_svc;
  db_result_t *res;
  char         sql[512];
  int          n;
  int          stored = 0;

  data = json_get_array(root, "data");
  if(data == NULL)
    return(0);

  n     = (int)json_object_array_length(data);
  e_svc = db_escape(service);

  // db_escape returns NULL when the pool has no connection to escape
  // against — the query would fail anyway, and mem_free aborts on NULL.
  if(e_svc == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: database unavailable", service);
    return(-1);
  }

  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_service_models WHERE service_name='%s'", e_svc);
  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, "llm", "refresh %s: cache clear failed: %s",
        service, res->error);
    db_result_free(res);
    mem_free(e_svc);
    return(-1);
  }

  db_result_free(res);

  for(int i = 0; i < n; i++)
  {
    struct json_object *item = json_object_array_get_idx(data, i);
    char    model_id[LLM_MODEL_ID_SZ];
    char   *e_mid;
    int64_t max_len = 0;
    if(item == NULL)
      continue;

    memset(model_id, 0, sizeof(model_id));
    json_get_str(item, "id", model_id, sizeof(model_id));

    if(model_id[0] == '\0')
      continue;

    e_mid = db_escape(llm_model_id_canon(model_id));

    if(e_mid == NULL)
      continue;

    if(json_get_int64(item, "max_model_len", &max_len) && max_len > 0)
      snprintf(sql, sizeof(sql),
          "INSERT INTO llm_service_models (service_name, model_id, "
          "max_model_len) VALUES ('%s', '%s', %" PRId64 ") "
          "ON CONFLICT (service_name, model_id) DO UPDATE "
          "SET max_model_len=EXCLUDED.max_model_len, fetched=NOW()",
          e_svc, e_mid, max_len);
    else
      snprintf(sql, sizeof(sql),
          "INSERT INTO llm_service_models (service_name, model_id) "
          "VALUES ('%s', '%s') "
          "ON CONFLICT (service_name, model_id) DO UPDATE SET fetched=NOW()",
          e_svc, e_mid);

    mem_free(e_mid);

    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok)
      stored++;

    db_result_free(res);
  }

  snprintf(sql, sizeof(sql),
      "UPDATE llm_services SET refreshed=NOW() WHERE name='%s'", e_svc);
  res = db_result_alloc();
  db_query(sql, res);
  db_result_free(res);

  mem_free(e_svc);
  return(stored);
}

// Stamp llm_services.probe_http with the most recent /models probe status
// (0 on transport error) so `show llm service` can surface auth failures
// without the value ever being printed as a secret.
static void
llm_service_set_probe_http(const char *name, long status)
{
  char        *e_name;
  db_result_t *res;
  char         sql[256];

  e_name = db_escape(name);

  if(e_name == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "UPDATE llm_services SET probe_http=%ld WHERE name='%s'",
      status, e_name);
  mem_free(e_name);

  res = db_result_alloc();
  db_query(sql, res);
  db_result_free(res);
}

static void
llm_service_refresh_done_cb(const curl_response_t *resp)
{
  llm_refresh_ctx_t  *rctx = resp->user_data;
  struct json_object *root;
  int                 stored;

  // Record the probe outcome regardless of success so the show surface can
  // report "needs API key" (401/403) or a failed probe (non-200 / 0).
  llm_service_set_probe_http(rctx->name, resp->status);

  if(resp->status != 200 || resp->body == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: http=%ld curl=%d %s",
        rctx->name, resp->status, resp->curl_code,
        resp->error ? resp->error : "");

    // Auth-gated /models: nudge the operator toward the key. Setting it
    // fires llm_apikey_kv_cb, which re-probes automatically.
    if(resp->status == 401 || resp->status == 403)
      clam(CLAM_INFO, "llm",
          "service %s requires an API key: "
          "set kv llm.service.%s.creds.apikey <key>", rctx->name, rctx->name);

    mem_free(rctx);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, "llm:refresh");
  if(root == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: response was not valid JSON",
        rctx->name);
    mem_free(rctx);
    return;
  }

  stored = llm_service_models_store(rctx->name, root);
  json_object_put(root);

  if(stored >= 0)
    clam(CLAM_INFO, "llm", "service %s: cached %d models", rctx->name, stored);

  mem_free(rctx);
}

// Fire an async GET <base_url>/models for one service. The probe runs
// keyless by default (local providers don't gate /models); an Authorization
// header is attached only when llm.service.<name>.creds.apikey holds a value, so a
// key-gated /models succeeds once the operator sets the key. Returns SUCCESS
// if the request was submitted (curl worker owns the heap ctx thereafter).
bool
llm_service_refresh(const char *name)
{
  llm_refresh_ctx_t *rctx;
  curl_request_t    *cr;
  char base[LLM_ENDPOINT_SZ];
  char url[LLM_ENDPOINT_SZ + 16];
  char kvkey[LLM_KV_KEY_SZ];
  const char *apikey;
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  if(llm_service_base_url(name, base, sizeof(base)) != SUCCESS)
    return(FAIL);

  if(llm_build_url(base, "models", url, sizeof(url)) != SUCCESS)
    return(FAIL);

  rctx = mem_alloc("llm", "refresh_ctx", sizeof(*rctx));
  snprintf(rctx->name, sizeof(rctx->name), "%s", name);

  cr = curl_request_create(CURL_METHOD_GET, url,
      llm_service_refresh_done_cb, rctx);
  if(cr == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: request create failed", name);
    mem_free(rctx);
    return(FAIL);
  }

  // Bearer token only when configured — keyless providers probe fine
  // without it, and an empty "Bearer " would break some gateways.
  snprintf(kvkey, sizeof(kvkey), "llm.service.%s.creds.apikey", name);
  apikey = kv_get_creds(kvkey);

  if(apikey != NULL && apikey[0] != '\0')
  {
    char hdr[LLM_KV_KEY_SZ + 512];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", apikey);
    curl_request_add_header(cr, hdr);
  }

  // curl_request_submit uses SUCCESS=false / FAIL=true and releases cr
  // internally on failure — but never frees user_data, so rctx is ours to
  // free here. On success the done callback frees it.
  if(curl_request_submit(cr) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "refresh %s: curl submit failed for %s", name, url);
    mem_free(rctx);
    return(FAIL);
  }

  return(SUCCESS);
}

// KV change hook on llm.service.<name>.creds.apikey. Parses the service
// name out of the key and re-probes /models when the value becomes
// non-empty. Fired outside the KV lock (core/kv.c), so reading kv_get_str
// and submitting curl from here is deadlock-safe.
void
llm_apikey_kv_cb(const char *key, void *data)
{
  static const char pfx[] = "llm.service.";
  const char *tail;
  const char *dot;
  const char *val;
  char        name[LLM_MODEL_NAME_SZ];
  size_t      n;
  (void)data;
  if(key == NULL || strncmp(key, pfx, sizeof(pfx) - 1) != 0)
    return;

  tail = key + (sizeof(pfx) - 1);
  dot  = strstr(tail, ".creds.apikey");

  if(dot == NULL || dot == tail)
    return;

  n = (size_t)(dot - tail);

  if(n >= sizeof(name))
    return;

  memcpy(name, tail, n);
  name[n] = '\0';

  // Empty value = key cleared; nothing to re-probe.
  val = kv_get_str(key);

  if(val == NULL || val[0] == '\0')
    return;

  clam(CLAM_INFO, "llm",
      "api key set for service %s — re-probing /models", name);
  llm_service_refresh(name);
}

// Startup seed: fire one refresh per known service so the /models cache is
// warm. Names are snapshotted under the lock, then curl is submitted after
// the lock is dropped (never hold a lock across a submit).
void
llm_services_refresh_all(void)
{
  char   names[64][LLM_MODEL_NAME_SZ];
  size_t count = 0;

  pthread_rwlock_rdlock(&llm_services_lock);

  for(llm_service_t *s = llm_services_head; s != NULL && count < 64;
      s = s->next)
  {
    snprintf(names[count], LLM_MODEL_NAME_SZ, "%s", s->name);
    count++;
  }

  pthread_rwlock_unlock(&llm_services_lock);

  for(size_t i = 0; i < count; i++)
    llm_service_refresh(names[i]);
}

// -----------------------------------------------------------------------
// embed_dim probe
//
// POST <base_url>/embeddings with a one-byte input and count the returned
// vector. Runs only for embed models, on `llm add model … embed` and on
// demand via `llm probe <name>`. The stored dim cannot be changed after
// it's first set (the pgvector column on conversation_embeddings is typed
// to it): a mismatched probe logs a WARN and is ignored. max_context is
// NOT probed here — it comes from the service /models cache.
// -----------------------------------------------------------------------

typedef struct
{
  char     name[LLM_MODEL_NAME_SZ];
  char     model_id[LLM_MODEL_ID_SZ];
  uint32_t expected_dim;   // 0 = no prior dim, accept whatever comes back
} llm_probe_dim_ctx_t;

// Count the float entries in data[0].embedding from an /embeddings
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
  size_t       dim;
  char        *e_name;
  db_result_t *res;
  char         sql[512];
  if(resp->status != 200 || resp->body == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim http=%ld curl=%d %s",
        pctx->name, resp->status, resp->curl_code,
        resp->error ? resp->error : "");
    mem_free(pctx);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, "llm:probe_dim");
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
        "probe %s: could not count embedding (got %zu)", pctx->name, dim);
    mem_free(pctx);
    return;
  }

  if(pctx->expected_dim != 0 && (uint32_t)dim != pctx->expected_dim)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: endpoint reports embed_dim=%zu but %u is stored; "
        "refusing to change — use `llm del model` + `llm add model` to switch",
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

  if(e_name == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim not stored (database unavailable)", pctx->name);
    mem_free(pctx);
    return;
  }

  snprintf(sql, sizeof(sql),
      "UPDATE llm_models SET embed_dim=%zu WHERE name='%s'", dim, e_name);
  mem_free(e_name);

  res = db_result_alloc();
  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim update failed: %s", pctx->name, res->error);
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
  char base[LLM_ENDPOINT_SZ];
  char url[LLM_ENDPOINT_SZ + 16];
  char escaped_id[LLM_MODEL_ID_SZ * 2];
  char body[LLM_MODEL_ID_SZ * 2 + 64];
  bool have_model;
  int  body_len;
  if(name == NULL || name[0] == '\0')
    return(false);

  pctx = mem_alloc("llm", "probe_dim_ctx", sizeof(*pctx));

  memset(base, 0, sizeof(base));
  have_model = false;

  pthread_rwlock_rdlock(&llm_models_lock);
  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0 && m->kind == LLM_KIND_EMBED)
    {
      snprintf(base, sizeof(base), "%s", m->base_url);
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

  if(llm_build_url(base, "embeddings", url, sizeof(url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "probe %s: cannot build embeddings URL", name);
    mem_free(pctx);
    return(false);
  }

  // Build the POST body. Use "x" as the input — shortest payload that
  // still returns a full-dim vector. JSON-escape model_id defensively in
  // case it contains a slash or quote (it usually doesn't).
  json_escape(pctx->model_id, escaped_id, sizeof(escaped_id));

  body_len = snprintf(body, sizeof(body),
      "{\"model\":\"%s\",\"input\":\"x\"}", escaped_id);
  if(body_len <= 0 || (size_t)body_len >= sizeof(body))
  {
    mem_free(pctx);
    return(false);
  }

  if(curl_post(url, "application/json", body, (size_t)body_len,
      llm_probe_embed_dim_done_cb, pctx) != SUCCESS)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim curl submit failed for %s", name, url);
    mem_free(pctx);
    return(false);
  }

  return(true);
}

static void
cmd_llm_probe(const cmd_ctx_t *ctx)
{
  const char *name;
  llm_kind_t  k;
  char msg[256];
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm probe <name>");
    return;
  }

  name = ctx->parsed->argv[0];

  if(llm_model_kind(name, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: no such model");
    return;
  }

  if(k != LLM_KIND_EMBED)
  {
    cmd_reply(ctx, "nothing to probe: max_context comes from the service "
        "model cache — run `llm service <service> refresh`");
    return;
  }

  if(!llm_probe_embed_dim_submit(name))
  {
    cmd_reply(ctx, "probe: failed to submit (see CLAM log)");
    return;
  }

  snprintf(msg, sizeof(msg),
      "embed_dim probe submitted for %s — result will appear in CLAM log",
      name);
  cmd_reply(ctx, msg);
}

// -----------------------------------------------------------------------
// Service CRUD
// -----------------------------------------------------------------------

static void
cmd_llm_add_service(const cmd_ctx_t *ctx)
{
  const char  *name;
  const char  *base_url;
  char        *e_name;
  char        *e_url;
  db_result_t *res;
  char         sql[2048];
  char         msg[512];
  if(ctx->parsed == NULL || ctx->parsed->argc < 2)
  {
    cmd_reply(ctx, "usage: llm add service <name> <base_url>");
    return;
  }

  name     = ctx->parsed->argv[0];
  base_url = ctx->parsed->argv[1];

  e_name = db_escape(name);
  e_url  = db_escape(base_url);

  if(e_name == NULL || e_url == NULL)
  {
    if(e_name != NULL)
      mem_free(e_name);

    if(e_url != NULL)
      mem_free(e_url);

    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "INSERT INTO llm_services (name, base_url) VALUES ('%s', '%s')",
      e_name, e_url);
  mem_free(e_name);
  mem_free(e_url);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    snprintf(msg, sizeof(msg), "insert failed: %s", res->error);
    cmd_reply(ctx, msg);
    db_result_free(res);
    return;
  }

  db_result_free(res);

  // Reload registers the llm.service.<name>.creds.apikey KV slot; the refresh
  // seeds the /models cache asynchronously and runs keyless. If the
  // provider gates /models behind auth, the probe records 401/403 and
  // `show llm service` will flag "needs API key" — set it then and the
  // KV hook re-probes automatically.
  llm_services_reload();
  llm_service_refresh(name);

  snprintf(msg, sizeof(msg),
      "ok — probing /models; check `show llm service %s`", name);
  cmd_reply(ctx, msg);
}

static void
cmd_llm_del_service(const cmd_ctx_t *ctx)
{
  const char  *name;
  char        *e_name;
  db_result_t *res;
  char         sql[512];
  long         refs;
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm del service <name>");
    return;
  }

  name   = ctx->parsed->argv[0];
  e_name = db_escape(name);

  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  // Block deletion while any defined model still references the service.
  snprintf(sql, sizeof(sql),
      "SELECT count(*) FROM llm_models WHERE service_name='%s'", e_name);
  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "error: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    mem_free(e_name);
    return;
  }

  refs = (res->rows > 0 && db_result_get(res, 0, 0) != NULL)
      ? strtol(db_result_get(res, 0, 0), NULL, 10) : 0;
  db_result_free(res);

  if(refs > 0)
  {
    char emsg[128];
    snprintf(emsg, sizeof(emsg),
        "error: %ld model(s) still reference this service", refs);
    cmd_reply(ctx, emsg);
    mem_free(e_name);
    return;
  }

  // Cache rows (llm_service_models) cascade on the FK.
  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_services WHERE name='%s'", e_name);
  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "delete failed: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  if(res->rows_affected == 0)
  {
    char emsg[128];
    snprintf(emsg, sizeof(emsg), "no such service: %s", name);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_services_reload();
  cmd_reply(ctx, "ok");
}

static void
cmd_llm_service(const cmd_ctx_t *ctx)
{
  const char *name;
  const char *action;
  if(ctx->parsed == NULL || ctx->parsed->argc < 2)
  {
    cmd_reply(ctx, "usage: llm service <name> refresh");
    return;
  }

  name   = ctx->parsed->argv[0];
  action = ctx->parsed->argv[1];

  if(strcmp(action, "refresh") == 0)
  {
    if(llm_service_refresh(name) != SUCCESS)
    {
      cmd_reply(ctx, "error: no such service, or refresh could not start");
      return;
    }

    cmd_reply(ctx, "ok (refreshing model list — count appears in CLAM log)");
    return;
  }

  cmd_reply(ctx, "usage: llm service <name> refresh");
}

// -----------------------------------------------------------------------
// Model CRUD
// -----------------------------------------------------------------------

static void
cmd_llm_add_model(const cmd_ctx_t *ctx)
{
  const char  *name;
  const char  *service;
  const char  *model_id;
  const char  *kind_s;
  llm_kind_t   k;
  char        *e_name;
  char        *e_svc;
  char        *e_mid;
  db_result_t *res;
  uint32_t     max_ctx;
  bool         in_cache;
  char         sql[2048];
  if(ctx->parsed == NULL || ctx->parsed->argc < 4)
  {
    cmd_reply(ctx,
        "usage: llm add model <chat|embed|image|stt|tts> <name> <service> <model_id>");
    return;
  }

  kind_s   = ctx->parsed->argv[0];
  name     = ctx->parsed->argv[1];
  service  = ctx->parsed->argv[2];
  model_id = llm_model_id_canon(ctx->parsed->argv[3]);

  if(llm_kind_from_str(kind_s, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: type must be 'chat', 'embed', 'image', 'stt' or 'tts'");
    return;
  }

  e_svc = db_escape(service);

  if(e_svc == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  // Service must exist.
  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM llm_services WHERE name='%s'", e_svc);
  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok || res->rows == 0)
  {
    cmd_reply(ctx, "error: no such service (add it with `llm add service`)");
    db_result_free(res);
    mem_free(e_svc);
    return;
  }

  db_result_free(res);

  // Seed max_context from the cached /models entry when present; warn
  // (non-fatal) if the model_id isn't in the service's cache yet.
  e_mid    = db_escape(model_id);
  max_ctx  = llm_cfg.max_context_tokens;
  in_cache = false;

  if(e_mid == NULL)
  {
    mem_free(e_svc);
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT max_model_len FROM llm_service_models "
      "WHERE service_name='%s' AND model_id='%s'", e_svc, e_mid);
  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *ml = db_result_get(res, 0, 0);
    in_cache = true;

    if(ml != NULL && ml[0] != '\0')
      max_ctx = (uint32_t)strtoul(ml, NULL, 10);
  }

  db_result_free(res);

  // A miss means one of two very different things, and saying "warning"
  // for both taught operators to read a benign one as a rejection. A
  // service's listing is fetched asynchronously when it is added, so
  // registering a model in the first seconds of a new service's life
  // always misses — there was nothing to check against. Only a miss
  // against a listing we actually hold says anything about the id.
  if(!in_cache)
  {
    bool have_listing = false;

    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM llm_service_models WHERE service_name='%s' LIMIT 1",
        e_svc);
    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
      have_listing = true;

    db_result_free(res);

    if(have_listing)
      cmd_reply(ctx, "warning: this service's /models list does not advertise "
          "that model_id — adding anyway (check the spelling against "
          "`show llm service <name> models`)");
    else
      cmd_reply(ctx, "note: this service has no cached /models list yet, so "
          "the model_id could not be verified — it is fetched in the "
          "background when a service is added (`llm service <name> refresh`)");
  }

  e_name = db_escape(name);

  if(e_name == NULL)
  {
    mem_free(e_svc);
    mem_free(e_mid);
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "INSERT INTO llm_models (name, kind, service_name, model_id, "
      "embed_dim, max_context) VALUES ('%s', '%s', '%s', '%s', 0, %u)",
      e_name, llm_kind_to_str(k), e_svc, e_mid, max_ctx);

  mem_free(e_name);
  mem_free(e_svc);
  mem_free(e_mid);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "insert failed: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();

  // Embed models need their output dimension probed once.
  if(k == LLM_KIND_EMBED)
  {
    if(llm_probe_embed_dim_submit(name))
      cmd_reply(ctx, "ok (probing endpoint for embed_dim)");
    else
      cmd_reply(ctx, "ok (embed_dim probe could not start — see CLAM log)");
  }

  else
    cmd_reply(ctx, "ok");
}

static void
cmd_llm_del_model(const cmd_ctx_t *ctx)
{
  const char  *name;
  char        *e_name;
  db_result_t *res;
  char         sql[256];
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm del model <name>");
    return;
  }

  name   = ctx->parsed->argv[0];
  e_name = db_escape(name);

  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_models WHERE name='%s'", e_name);
  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "delete failed: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  // A DELETE that matched nothing still "succeeds" — report it instead of
  // a misleading "ok" (e.g. `llm del model chat g4nv` deletes name='chat',
  // not the model, because del takes just <name> unlike add's typed form).
  if(res->rows_affected == 0)
  {
    char emsg[128];
    snprintf(emsg, sizeof(emsg), "no such model: %s", name);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();
  cmd_reply(ctx, "ok");
}

// /llm test: a bounded synchronous probe.
//
// This is the documented exception to the daemon's non-blocking rule
// (see DESIGN.md): an operator asked "does this model answer?" and wants
// the answer in the same breath. The wait is therefore confined to the
// command thread and bounded — but the request outlives the bound. A
// model slower than its window keeps running, and its callback fires on
// a curl worker after the command has already replied and returned.
//
// The waiter is consequently heap-owned and reference-counted, never a
// stack local: one reference for the command, one for the in-flight
// request, and whoever drops the last one frees it. Abandoning a stack
// waiter would hand the curl worker a dead frame to write 512 bytes
// into — silent corruption that surfaces as a crash somewhere else
// entirely, minutes later.
//
// The windows below are deliberately code, not KV. The wait is served by
// whatever thread dispatched the command, and over botmanctl that thread
// is the control socket's own poll task — cmd_dispatch_as() invokes the
// handler inline — so a generous operator-tunable knob here is an
// invitation to take the control surface offline for minutes while
// diagnosing a slow model. Image generation is inherently slower than a
// chat ping and gets its own window; no other kind needs one, and a
// model slower than its window is not a failure, it is a log entry.
#define LLM_TEST_WAIT_SECS        15
#define LLM_TEST_WAIT_IMAGE_SECS  30

typedef struct
{
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  bool            done;
  bool            ok;
  long            status;
  char            content[512];
  char            err[256];

  uint32_t        refs;        // guarded by mu; 0 = free it
  bool            abandoned;   // command gave up: report to the log instead
  char            model[LLM_MODEL_NAME_SZ];
  struct timespec t0;          // probe start, for the late-result latency
} llm_test_sync_t;

// Two references: the command's own, and the one the request carries as
// user_data. A failed submit means the second never materialises, so the
// caller releases twice.
static llm_test_sync_t *
llm_test_sync_create(const char *model)
{
  llm_test_sync_t *s;

  s = mem_alloc("llm", "test", sizeof(*s));
  memset(s, 0, sizeof(*s));

  pthread_mutex_init(&s->mu, NULL);
  pthread_cond_init(&s->cv, NULL);

  s->refs = 2;
  snprintf(s->model, sizeof(s->model), "%s", model);
  clock_gettime(CLOCK_MONOTONIC, &s->t0);

  return(s);
}

static void
llm_test_sync_release(llm_test_sync_t *s)
{
  uint32_t remaining;

  pthread_mutex_lock(&s->mu);
  remaining = --s->refs;
  pthread_mutex_unlock(&s->mu);

  if(remaining > 0)
    return;

  pthread_mutex_destroy(&s->mu);
  pthread_cond_destroy(&s->cv);
  mem_free(s);
}

// Every kind's callback ends here with the result fields already filled:
// wake a waiting command, or — when the command timed out and left — put
// the result the operator asked for in the log, where it is still worth
// having. Consumes the request's reference, so `s` is unsafe to touch on
// return.
static void
llm_test_sync_complete(llm_test_sync_t *s)
{
  char     line[832];
  bool     abandoned;
  uint64_t ms;

  pthread_mutex_lock(&s->mu);

  s->done   = true;
  abandoned = s->abandoned;
  ms        = util_ms_since(&s->t0);

  // Format under the lock, emit outside it: clam() takes a lock of its
  // own and nothing here needs to hold two at once.
  if(abandoned)
    snprintf(line, sizeof(line), "test %s: late result after %lums, http %ld: %s",
        s->model, (unsigned long)ms, s->status,
        s->ok ? s->content : s->err);

  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);

  if(abandoned)
    clam(CLAM_INFO, "llm", "%s", line);

  llm_test_sync_release(s);
}

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

  pthread_mutex_unlock(&s->mu);

  llm_test_sync_complete(s);
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

  pthread_mutex_unlock(&s->mu);

  llm_test_sync_complete(s);
}

static void
cmd_llm_test_image_done(const llm_image_response_t *resp)
{
  llm_test_sync_t *s = resp->user_data;

  pthread_mutex_lock(&s->mu);

  s->ok     = resp->ok;
  s->status = resp->http_status;

  if(resp->ok)
    snprintf(s->content, sizeof(s->content),
        "%s b64_len=%zu", resp->mime, resp->b64_len);

  if(resp->error != NULL)
    snprintf(s->err, sizeof(s->err), "%s", resp->error);

  pthread_mutex_unlock(&s->mu);

  llm_test_sync_complete(s);
}

static void
cmd_llm_test(const cmd_ctx_t *ctx)
{
  llm_test_sync_t *s;
  const char      *name;
  const char      *prompt;
  struct timespec  until;
  char             line[832];
  uint64_t         ms;
  uint32_t         wait_secs;
  llm_kind_t       k;
  bool             submitted;
  bool             done;

  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm test <name> [prompt...]");
    return;
  }

  name   = ctx->parsed->argv[0];
  prompt = ctx->parsed->argc > 1 ? ctx->parsed->argv[1] : "ping";

  if(llm_model_kind(name, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: unknown model");
    return;
  }

  // The speech kinds are deliberately not testable from here: one wants
  // a WAV this command has no way to hold and the other answers with
  // one. Say so plainly rather than fall through to the embed arm and
  // fail against its kind guard with a baffling message.
  if(k != LLM_KIND_CHAT && k != LLM_KIND_IMAGE && k != LLM_KIND_EMBED)
  {
    cmd_reply(ctx, "error: `llm test` does not cover the speech kinds — "
        "try `/bot <name> say <text>` for a tts model, and speak to the robot "
        "for an stt one");
    return;
  }

  s = llm_test_sync_create(name);

  if(k == LLM_KIND_CHAT)
  {
    llm_chat_params_t params = { 0 };
    llm_message_t     msgs[1];

    memset(msgs, 0, sizeof(msgs));
    // Wide enough that a thinking model can reason and still answer. At
    // 32 the reasoning pass consumed the whole budget and the probe came
    // back "no content in response" — a healthy model reported as broken,
    // which is the opposite of what a diagnostic is for. The extra tokens
    // cost nothing next to being lied to.
    params.max_tokens = 512;

    msgs[0].role    = LLM_ROLE_USER;
    msgs[0].content = prompt;

    submitted = (llm_chat_submit(name, &params, msgs, 1,
        cmd_llm_test_done, NULL, s) == SUCCESS);
  }

  else if(k == LLM_KIND_IMAGE)
  {
    llm_image_params_t params = { 0 };

    params.n = 1;

    submitted = (llm_image_submit(name, &params, prompt,
        cmd_llm_test_image_done, s) == SUCCESS);
  }

  else
  {
    const char *inputs[1] = { prompt };

    submitted = (llm_embed_submit(name, inputs, 1,
        cmd_llm_test_embed_done, s) == SUCCESS);
  }

  if(!submitted)
  {
    cmd_reply(ctx, "error: submit failed");

    // No callback will ever fire, so the request's reference is ours to
    // drop as well as our own.
    llm_test_sync_release(s);
    llm_test_sync_release(s);
    return;
  }

  wait_secs = (k == LLM_KIND_IMAGE)
      ? LLM_TEST_WAIT_IMAGE_SECS : LLM_TEST_WAIT_SECS;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += wait_secs;

  pthread_mutex_lock(&s->mu);

  while(!s->done)
    if(pthread_cond_timedwait(&s->cv, &s->mu, &until) != 0)
      break;

  done = s->done;
  ms   = util_ms_since(&s->t0);

  // Claim the result while still holding the lock the callback needs, so
  // there is no window in which both sides believe they own the reply.
  if(done)
    snprintf(line, sizeof(line), "%s (%lums, http %ld): %s",
        s->ok ? "ok" : "failed", (unsigned long)ms, s->status,
        s->ok ? s->content : s->err);
  else
    s->abandoned = true;

  pthread_mutex_unlock(&s->mu);

  if(!done)
    snprintf(line, sizeof(line),
        "still running after %us — this model is slower than the probe "
        "window; the result will be logged when it lands", wait_secs);

  cmd_reply(ctx, line);
  llm_test_sync_release(s);
}

// -----------------------------------------------------------------------
// /show llm [models|service]
// -----------------------------------------------------------------------

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

// Render one colorized service summary line. The API token is checked for
// presence only — its value is NEVER printed.
static void
llm_service_line(const cmd_ctx_t *ctx, const char *name, const char *base,
    const char *refreshed, const char *cached, const char *defined,
    const char *probe_http)
{
  char        key[LLM_KV_KEY_SZ];
  const char *token;
  bool        key_set;
  long        probe;
  char        note[96];
  char        line[768];
  if(name == NULL)
    return;

  snprintf(key, sizeof(key), "llm.service.%s.creds.apikey", name);
  token   = kv_get_str(key);
  key_set = token != NULL && token[0] != '\0';

  // Translate the last /models probe status into an at-a-glance note.
  // -1 = never probed; 0 = transport failure; 401/403 = auth wall.
  probe   = (probe_http && probe_http[0]) ? strtol(probe_http, NULL, 10) : -1;
  note[0] = '\0';

  if(probe == 401 || probe == 403)
    snprintf(note, sizeof(note), "  " CLR_RED "needs API key" CLR_RESET);
  else if(probe == 0)
    snprintf(note, sizeof(note), "  " CLR_RED "probe failed" CLR_RESET);
  else if(probe > 0 && probe != 200)
    snprintf(note, sizeof(note),
        "  " CLR_RED "probe http=%ld" CLR_RESET, probe);

  snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET "  " CLR_GRAY "%s" CLR_RESET
      "  " CLR_CYAN "models=%s" CLR_RESET "  defined=%s  key=%s%s" CLR_RESET
      "  " CLR_GRAY "refreshed=%s" CLR_RESET "%s",
      name,
      base ? base : "",
      cached ? cached : "0",
      defined ? defined : "0",
      key_set ? CLR_GREEN : CLR_RED,
      key_set ? "set" : "unset",
      (refreshed && refreshed[0]) ? refreshed : "never",
      note);

  cmd_reply(ctx, line);
}

static void
cmd_show_llm_service_models(const cmd_ctx_t *ctx, const char *name)
{
  char        *e_name;
  db_result_t *res;
  char         sql[512];
  uint32_t     shown = 0;

  e_name = db_escape(name);

  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT model_id, max_model_len FROM llm_service_models "
      "WHERE service_name='%s' ORDER BY model_id", e_name);
  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    char line[256];

    for(uint32_t r = 0; r < res->rows; r++)
    {
      const char *mid = db_result_get(res, r, 0);
      const char *ml  = db_result_get(res, r, 1);

      if(ml != NULL && ml[0] != '\0')
        snprintf(line, sizeof(line),
            "  %s  " CLR_GRAY "ctx=%s" CLR_RESET, mid ? mid : "?", ml);
      else
        snprintf(line, sizeof(line), "  %s", mid ? mid : "?");

      cmd_reply(ctx, line);
      shown++;
    }
  }

  db_result_free(res);

  if(shown == 0)
  {
    char msg[256];
    snprintf(msg, sizeof(msg),
        "  (none — run `llm service %s refresh`)", name);
    cmd_reply(ctx, msg);
  }
}

// Shared SELECT for the list (0-arg) and detail (<name>) surfaces; the
// WHERE clause is appended by the caller (empty for the full list).
static void
cmd_show_llm_service_summary(const cmd_ctx_t *ctx, const char *where_name)
{
  db_result_t     *res;
  char             sql[1024];
  llm_list_state_t st = { .ctx = ctx, .count = 0 };
  char             where[128];

  where[0] = '\0';

  if(where_name != NULL)
  {
    char *e_name = db_escape(where_name);

    // An unescapable filter must not widen into "list everything".
    if(e_name == NULL)
    {
      cmd_reply(ctx, "error: database unavailable");
      return;
    }

    snprintf(where, sizeof(where), " WHERE s.name='%s'", e_name);
    mem_free(e_name);
  }

  snprintf(sql, sizeof(sql),
      "SELECT s.name, s.base_url, s.refreshed, "
      "(SELECT count(*) FROM llm_service_models c "
      "WHERE c.service_name=s.name), "
      "(SELECT count(*) FROM llm_models m WHERE m.service_name=s.name), "
      "s.probe_http "
      "FROM llm_services s%s ORDER BY s.name", where);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t r = 0; r < res->rows; r++)
    {
      llm_service_line(ctx,
          db_result_get(res, r, 0), db_result_get(res, r, 1),
          db_result_get(res, r, 2), db_result_get(res, r, 3),
          db_result_get(res, r, 4), db_result_get(res, r, 5));
      st.count++;
    }
  }

  db_result_free(res);

  if(st.count == 0)
  {
    if(where_name != NULL)
      cmd_reply(ctx, "error: no such service");
    else
      cmd_reply(ctx,
          "  (none — add one with `llm add service <name> <base_url>`)");
  }
}

static void
cmd_show_llm_service(const cmd_ctx_t *ctx)
{
  const char *name;

  // 0 args → colorized list of all services.
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, CLR_BOLD "llm services" CLR_RESET);
    cmd_show_llm_service_summary(ctx, NULL);
    return;
  }

  name = ctx->parsed->argv[0];

  // `<name> models` → cached /models list.
  if(ctx->parsed->argc >= 2 && strcmp(ctx->parsed->argv[1], "models") == 0)
  {
    cmd_show_llm_service_models(ctx, name);
    return;
  }

  // `<name>` → one-service detail.
  cmd_show_llm_service_summary(ctx, name);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_add_service[] = {
  { "name",     CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "base_url", CMD_ARG_NONE, CMD_ARG_REQUIRED, 0,                     NULL },
};

static const cmd_arg_desc_t ad_add_model[] = {
  { "type",     CMD_ARG_NONE, CMD_ARG_REQUIRED, 16,                    NULL },
  { "name",     CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "service",  CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "model_id", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_ID_SZ - 1,   NULL },
};

static const cmd_arg_desc_t ad_del_one[] = {
  { "name", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_llm_service[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "action", CMD_ARG_NONE, CMD_ARG_REQUIRED, 16,                    NULL },
};

static const cmd_arg_desc_t ad_llm_probe[] = {
  { "name", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_llm_test[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_REQUIRED,                LLM_MODEL_NAME_SZ - 1, NULL },
  { "prompt", CMD_ARG_NONE, CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,                     NULL },
};

static const cmd_arg_desc_t ad_show_service[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_OPTIONAL, LLM_MODEL_NAME_SZ - 1, NULL },
  { "action", CMD_ARG_NONE, CMD_ARG_OPTIONAL, 16,                    NULL },
};

static void
cmd_llm_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /llm <add|del|service|probe|test> ...");
}

static void
cmd_llm_add_usage(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: llm add service <name> <base_url>");
  cmd_reply(ctx, "       llm add model <chat|embed|image|stt|tts> <name> <service> <model_id>");
}

static void
cmd_llm_del_usage(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: llm del service <name>  |  llm del model <name>");
}

void
llm_register_commands(void)
{
  cmd_register("llm", "llm",
      "llm",
      "LLM model registry",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_root, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  // add → { service, model }
  cmd_register("llm", "add",
      "llm add <service|model> ...",
      "Create an LLM service or model",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_add_usage, NULL, "llm", "a", NULL, 0, NULL, NULL);

  cmd_register("llm", "service",
      "llm add service <name> <base_url>",
      "Register an OpenAI-compatible provider (base URL)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_add_service, NULL, "llm/add", "s", ad_add_service,
      (uint8_t)(sizeof(ad_add_service) / sizeof(ad_add_service[0])),
      NULL, NULL);

  cmd_register("llm", "model",
      "llm add model <chat|embed|image|stt|tts> <name> <service> <model_id>",
      "Register a model against a service",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_add_model, NULL, "llm/add", "m", ad_add_model,
      (uint8_t)(sizeof(ad_add_model) / sizeof(ad_add_model[0])), NULL, NULL);

  // del → { service, model }
  cmd_register("llm", "del",
      "llm del <service|model> <name>",
      "Delete an LLM service or model",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_del_usage, NULL, "llm", "d", NULL, 0, NULL, NULL);

  cmd_register("llm", "service",
      "llm del service <name>",
      "Delete a service (blocked while models reference it)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_del_service, NULL, "llm/del", "s", ad_del_one, 1, NULL, NULL);

  cmd_register("llm", "model",
      "llm del model <name>",
      "Delete a defined model",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_del_model, NULL, "llm/del", "m", ad_del_one, 1, NULL, NULL);

  // service <name> <action> (refresh)
  cmd_register("llm", "service",
      "llm service <name> refresh",
      "Refresh a service's cached /models list",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_service, NULL, "llm", "sv", ad_llm_service,
      (uint8_t)(sizeof(ad_llm_service) / sizeof(ad_llm_service[0])),
      NULL, NULL);

  // Abbrev is "pb" not "p": the llm-bot plugin claims "p" for
  // /llm personality later in init, and a collision aborts plugin load.
  cmd_register("llm", "probe",
      "llm probe <name>",
      "Re-probe an embed model's output dimension",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_llm_probe, NULL, "llm", "pb", ad_llm_probe,
      (uint8_t)(sizeof(ad_llm_probe) / sizeof(ad_llm_probe[0])), NULL, NULL);

  cmd_register("llm", "test",
      "llm test <name> [prompt]",
      "Probe an LLM model synchronously",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
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

  cmd_register("llm", "service",
      "show llm service [<name> [models]]",
      "Show LLM services (list, detail, or cached /models)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_llm_service, NULL, "show/llm", "s", ad_show_service,
      (uint8_t)(sizeof(ad_show_service) / sizeof(ad_show_service[0])),
      NULL, NULL);
}
