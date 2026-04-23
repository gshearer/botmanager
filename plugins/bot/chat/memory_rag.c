// botmanager — MIT
// Chat bot memory store: RAG retrieval (cosine scan, top-K, dossier/user
// async paths).

#define MEMORY_INTERNAL
#include "memory.h"

#include "db.h"
#include "inference.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Retrieval (Chunk D)

// Scored hit buffer entry.
typedef struct
{
  int64_t id;
  float   score;
} memory_hit_t;

// In-place insertion into a top-K buffer (sorted descending).
static void
memory_topk_insert(memory_hit_t *buf, size_t cap, size_t *n_out,
    int64_t id, float score)
{
  size_t n = *n_out;
  size_t pos;

  if(n < cap)
  {
    size_t pos = n;
    while(pos > 0 && buf[pos - 1].score < score)
    {
      buf[pos] = buf[pos - 1];
      pos--;
    }
    buf[pos].id = id;
    buf[pos].score = score;
    *n_out = n + 1;
    return;
  }

  if(score <= buf[cap - 1].score)
    return;

  pos = cap - 1;
  while(pos > 0 && buf[pos - 1].score < score)
  {
    buf[pos] = buf[pos - 1];
    pos--;
  }
  buf[pos].id = id;
  buf[pos].score = score;
}

// Scan an embedding table and accumulate top-K hits against qvec.
// join_sql is the "FROM ... WHERE ..." clause (no trailing AND); this
// helper appends the model/dim filter. id_col is the embedding-table PK
// (fact_id or msg_id).
static void
memory_scan_embeddings(const char *join_sql, const char *id_col,
    const char *model, uint32_t dim, const float *qvec,
    memory_hit_t *buf, size_t cap, size_t *n_out)
{
  char *e_model = db_escape(model);
  db_result_t *res;
  size_t sz;
  char *sql;

  if(e_model == NULL) return;

  sz = strlen(join_sql) + strlen(e_model) + 256;
  sql = mem_alloc("memory", "rag_scan_sql", sz);

  if(sql == NULL) { mem_free(e_model); return; }

  snprintf(sql, sz,
      "SELECT e.%s, e.vec %s AND e.model = '%s' AND e.dim = %u",
      id_col, join_sql, e_model, dim);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "memory", "retrieve scan: %s", res->error);
    db_result_free(res);
    mem_free(sql);
    mem_free(e_model);
    return;
  }

  for(uint32_t i = 0; i < res->rows; i++)
  {
    const char *id_s  = db_result_get(res, i, 0);
    const char *vec_s = db_result_get(res, i, 1);
    float score;
    int64_t id;
    float *rv;

    if(id_s == NULL || vec_s == NULL) continue;

    rv = memory_bytea_to_vec(vec_s, dim);
    if(rv == NULL) continue;

    score = memory_cosine(qvec, rv, dim);
    id = (int64_t)strtoll(id_s, NULL, 10);

    memory_topk_insert(buf, cap, n_out, id, score);
    mem_free(rv);
  }

  db_result_free(res);
  mem_free(sql);
  mem_free(e_model);
}

static void
memory_build_in_list(char *out, size_t out_sz,
    const memory_hit_t *hits, size_t n)
{
  size_t w = 0;
  w += (size_t)snprintf(out + w, out_sz - w, "(");
  for(size_t i = 0; i < n; i++)
    w += (size_t)snprintf(out + w, out_sz - w,
        "%s%" PRId64, i == 0 ? "" : ",", hits[i].id);
  snprintf(out + w, out_sz - w, ")");
}

#define MEMORY_MSG_SELECT_COLS \
    "id, ns_id, COALESCE(user_id, 0), COALESCE(dossier_id, 0), bot_name," \
    " method, channel, kind, text, EXTRACT(EPOCH FROM ts)::bigint"

static void
memory_parse_msg_row(const db_result_t *r, uint32_t row, mem_msg_t *m)
{
  const char *v;
  memset(m, 0, sizeof(*m));

  v = db_result_get(r, row, 0); if(v) m->id = (int64_t)strtoll(v, NULL, 10);
  v = db_result_get(r, row, 1); if(v) m->ns_id = (int)strtol(v, NULL, 10);
  v = db_result_get(r, row, 2); if(v) m->user_id_or_0 = (int)strtol(v, NULL, 10);
  v = db_result_get(r, row, 3); if(v) m->dossier_id = (int64_t)strtoll(v, NULL, 10);
  v = db_result_get(r, row, 4); if(v) snprintf(m->bot_name, sizeof(m->bot_name), "%s", v);
  v = db_result_get(r, row, 5); if(v) snprintf(m->method,   sizeof(m->method),   "%s", v);
  v = db_result_get(r, row, 6); if(v) snprintf(m->channel,  sizeof(m->channel),  "%s", v);
  v = db_result_get(r, row, 7); if(v) m->kind = (mem_msg_kind_t)strtol(v, NULL, 10);
  v = db_result_get(r, row, 8); if(v) snprintf(m->text,     sizeof(m->text),     "%s", v);
  v = db_result_get(r, row, 9); if(v) m->ts = (time_t)strtoll(v, NULL, 10);
}

static void
memory_deliver_hits(const memory_hit_t *fact_hits, size_t n_facts,
    const memory_hit_t *msg_hits,  size_t n_msgs,
    memory_retrieve_cb_t cb, void *user)
{
  mem_fact_t *facts = NULL;
  mem_msg_t  *msgs  = NULL;
  size_t      nf    = 0;
  size_t      nm    = 0;

  if(n_facts > 0)
  {
    char in_list[32 * 64];
    db_result_t *res;
    char sql[2048];

    memory_build_in_list(in_list, sizeof(in_list), fact_hits, n_facts);

    snprintf(sql, sizeof(sql),
        "SELECT " MEMORY_FACT_SELECT_COLS
        " FROM user_facts WHERE id IN %s", in_list);

    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
    {
      facts = mem_alloc("memory", "rag_facts",
          sizeof(mem_fact_t) * res->rows);

      if(facts != NULL)
        for(uint32_t i = 0; i < res->rows; i++)
          memory_parse_fact_row(res, i, &facts[nf++]);
    }

    db_result_free(res);
  }

  if(n_msgs > 0)
  {
    char in_list[32 * 64];
    db_result_t *res;
    char sql[2048];

    memory_build_in_list(in_list, sizeof(in_list), msg_hits, n_msgs);

    snprintf(sql, sizeof(sql),
        "SELECT " MEMORY_MSG_SELECT_COLS
        " FROM conversation_log WHERE id IN %s", in_list);

    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
    {
      msgs = mem_alloc("memory", "rag_msgs",
          sizeof(mem_msg_t) * res->rows);

      if(msgs != NULL)
        for(uint32_t i = 0; i < res->rows; i++)
          memory_parse_msg_row(res, i, &msgs[nm++]);
    }

    db_result_free(res);
  }

  cb(facts, nf, msgs, nm, user);

  if(facts != NULL) mem_free(facts);
  if(msgs  != NULL) mem_free(msgs);
}

// Run the cosine scan + delivery with a pre-computed query vector.
static void
memory_retrieve_with_vec(int ns_id, int user_id_or_0,
    const char *model, uint32_t dim, const float *qvec, uint32_t top_k,
    memory_retrieve_cb_t cb, void *user)
{
  memory_hit_t fact_buf[64];
  memory_hit_t msg_buf[64];
  size_t       n_facts;
  size_t       n_msgs;

  if(top_k == 0 || top_k > 64)
    top_k = 8;

  n_facts = 0;
  n_msgs = 0;

  {
    char join[512];
    if(user_id_or_0 > 0)
      snprintf(join, sizeof(join),
          "FROM user_fact_embeddings e JOIN user_facts x"
          " ON e.fact_id = x.id WHERE x.user_id = %d", user_id_or_0);
    else
      snprintf(join, sizeof(join),
          "FROM user_fact_embeddings e JOIN user_facts x"
          " ON e.fact_id = x.id WHERE TRUE");

    memory_scan_embeddings(join, "fact_id", model, dim, qvec,
        fact_buf, top_k, &n_facts);
  }

  {
    char join[512];
    if(user_id_or_0 > 0)
      snprintf(join, sizeof(join),
          "FROM conversation_embeddings e JOIN conversation_log x"
          " ON e.msg_id = x.id WHERE x.ns_id = %d AND x.user_id = %d",
          ns_id, user_id_or_0);
    else
      snprintf(join, sizeof(join),
          "FROM conversation_embeddings e JOIN conversation_log x"
          " ON e.msg_id = x.id WHERE x.ns_id = %d", ns_id);

    memory_scan_embeddings(join, "msg_id", model, dim, qvec,
        msg_buf, top_k, &n_msgs);
  }

  memory_deliver_hits(fact_buf, n_facts, msg_buf, n_msgs, cb, user);
}

// Hydrate conversation_log rows by id. Returns rows matching any id in
// `hits`, bounded by `cap`. ns_id gates the lookup to the correct
// namespace (cheap guard against stale ids from a different install).
static size_t
memory_msgs_from_ids(int ns_id, const memory_hit_t *hits, size_t n_hits,
    mem_msg_t *out, size_t cap)
{
  db_result_t *res;
  size_t n;
  char sql[2048];
  char in_list[32 * 64];

  if(n_hits == 0 || out == NULL || cap == 0)
    return(0);

  memory_build_in_list(in_list, sizeof(in_list), hits, n_hits);

  snprintf(sql, sizeof(sql),
      "SELECT id, COALESCE(user_id,0), COALESCE(dossier_id,0),"
      " bot_name, method, channel, kind, text,"
      " EXTRACT(EPOCH FROM ts)::bigint"
      " FROM conversation_log"
      " WHERE ns_id = %d AND id IN %s"
      " ORDER BY ts DESC LIMIT %zu",
      ns_id, in_list, cap);

  res = db_result_alloc();
  n = 0;

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n < cap; i++)
    {
      mem_msg_t *m = &out[n++];
      const char *v;

      memset(m, 0, sizeof(*m));
      m->ns_id = ns_id;
      v = db_result_get(res, i, 0); if(v) m->id           = strtoll(v, NULL, 10);
      v = db_result_get(res, i, 1); if(v) m->user_id_or_0 = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 2); if(v) m->dossier_id   = strtoll(v, NULL, 10);
      v = db_result_get(res, i, 3); if(v) snprintf(m->bot_name, sizeof(m->bot_name), "%s", v);
      v = db_result_get(res, i, 4); if(v) snprintf(m->method,   sizeof(m->method),   "%s", v);
      v = db_result_get(res, i, 5); if(v) snprintf(m->channel,  sizeof(m->channel),  "%s", v);
      v = db_result_get(res, i, 6); if(v) m->kind = (mem_msg_kind_t)strtol(v, NULL, 10);
      v = db_result_get(res, i, 7); if(v) snprintf(m->text,     sizeof(m->text),     "%s", v);
      v = db_result_get(res, i, 8); if(v) m->ts = (time_t)strtoll(v, NULL, 10);
    }
  }

  db_result_free(res);
  return(n);
}

// Dedup-merge two mem_msg_t arrays by id, preserving first-array order.
// Output is a fresh mem_alloc'd buffer; caller frees. On OOM, *out is
// left NULL and *n_out is 0 so callers can fall back cleanly.
static void
memory_merge_msgs(const mem_msg_t *a, size_t na,
    const mem_msg_t *b, size_t nb,
    mem_msg_t **out, size_t *n_out)
{
  size_t n;
  mem_msg_t *dst;
  size_t cap;

  *out   = NULL;
  *n_out = 0;

  cap = na + nb;
  if(cap == 0)
    return;

  dst = mem_alloc("memory", "rag_merged_msgs",
      sizeof(*dst) * cap);
  if(dst == NULL)
    return;

  n = 0;
  for(size_t i = 0; i < na; i++)
    dst[n++] = a[i];

  for(size_t j = 0; j < nb; j++)
  {
    bool dup = false;
    for(size_t i = 0; i < n; i++)
    {
      if(dst[i].id == b[j].id)
      {
        dup = true;
        break;
      }
    }
    if(!dup)
      dst[n++] = b[j];
  }

  *out   = dst;
  *n_out = n;
}

// Cosine-scan conversation_embeddings restricted to a single dossier.
// Writes at most `top_k` hits to `out`; sets `*n_out` to the number
// written. Rows whose cosine is below `min_cos_x100` / 100.0 are
// dropped. When `min_cos_x100` is 0, no floor is applied.
static void
memory_recall_scan_convo(int ns_id, int64_t dossier_id,
    const char *model, uint32_t dim, const float *qvec,
    uint32_t top_k, uint32_t min_cos_x100,
    memory_hit_t *out, size_t *n_out)
{
  char join[512];
  size_t n;

  snprintf(join, sizeof(join),
      "FROM conversation_embeddings e JOIN conversation_log x"
      " ON e.msg_id = x.id"
      " WHERE x.ns_id = %d AND x.dossier_id = %" PRId64,
      ns_id, dossier_id);

  n = 0;
  memory_scan_embeddings(join, "msg_id", model, dim, qvec,
      out, top_k, &n);

  // memory_scan_embeddings returns hits sorted score-desc, so truncating
  // on the first sub-floor entry drops every remaining (lower) hit too.
  if(min_cos_x100 > 0)
  {
    float floor = (float)min_cos_x100 / 100.0f;
    for(size_t i = 0; i < n; i++)
    {
      if(out[i].score < floor)
      {
        n = i;
        break;
      }
    }
  }

  *n_out = n;
}

typedef struct
{
  int                   ns_id;
  int                   user_id_or_0;
  uint32_t              top_k;
  char                  model[MEM_EMBED_MODEL_SZ];
  memory_retrieve_cb_t  cb;
  void                 *user;
} memory_retrieval_ctx_t;

static void
memory_retrieve_embed_done(const llm_embed_response_t *resp)
{
  memory_retrieval_ctx_t *c = resp->user_data;

  if(!resp->ok || resp->n_vectors < 1 || resp->dim == 0)
  {
    clam(CLAM_WARN, "memory", "retrieve embed failed: %s",
        resp->error ? resp->error : "(no detail)");
    c->cb(NULL, 0, NULL, 0, c->user);
    mem_free(c);
    return;
  }

  memory_retrieve_with_vec(c->ns_id, c->user_id_or_0,
      c->model, resp->dim, resp->vectors[0], c->top_k, c->cb, c->user);

  mem_free(c);
}

bool
memory_retrieve(int ns_id, int user_id_or_0, const char *query,
    uint32_t top_k, memory_retrieve_cb_t cb, void *user)
{
  const char *inputs[1] = { query };
  memory_retrieval_ctx_t *c;
  mem_cfg_t cfg;

  if(cb == NULL)
    return(FAIL);

  memory_cfg_snapshot(&cfg);

  // Sync empty path: disabled, no model, or empty query.
  if(!memory_ready || !cfg.enabled || cfg.embed_model[0] == '\0'
     || query == NULL || query[0] == '\0')
  {
    cb(NULL, 0, NULL, 0, user);
    return(SUCCESS);
  }

  c = mem_alloc("memory", "rag_ctx", sizeof(*c));

  if(c == NULL)
  {
    cb(NULL, 0, NULL, 0, user);
    return(FAIL);
  }

  c->ns_id        = ns_id;
  c->user_id_or_0 = user_id_or_0;
  c->top_k        = top_k ? top_k : cfg.rag_top_k;
  c->cb           = cb;
  c->user         = user;
  snprintf(c->model, sizeof(c->model), "%s", cfg.embed_model);

  if(llm_embed_submit(cfg.embed_model, inputs, 1,
      memory_retrieve_embed_done, c) != SUCCESS)
  {
    cb(NULL, 0, NULL, 0, user);
    mem_free(c);
    return(FAIL);
  }

  return(SUCCESS);
}

// Copy a mem_dossier_fact_t row into mem_fact_t shape for assemble_prompt.
static void
memory_pf_to_fact(const mem_dossier_fact_t *pf, mem_fact_t *out)
{
  memset(out, 0, sizeof(*out));
  out->id          = pf->id;
  out->user_id     = 0;
  out->kind        = pf->kind;
  out->confidence  = pf->confidence;
  out->observed_at = pf->observed_at;
  out->last_seen   = pf->last_seen;
  snprintf(out->fact_key,   sizeof(out->fact_key),   "%s", pf->fact_key);
  snprintf(out->fact_value, sizeof(out->fact_value), "%s", pf->fact_value);
  snprintf(out->source,     sizeof(out->source),     "%s", pf->source);
  snprintf(out->channel,    sizeof(out->channel),    "%s", pf->channel);
}

// Fetch dossier_facts from *other* dossiers whose fact_key encodes an
// attitude/observation directed at `dossier_id` (convention: fact_key
// is "toward:<N>"). These surface alongside the dossier's own facts
// so the prompt carries "what others have said about N" context.
// Returns rows written.
static size_t
memory_get_toward_facts(int64_t dossier_id,
    mem_dossier_fact_t *out, size_t cap)
{
  db_result_t *res;
  size_t n;
  char sql[1024];
  char *e_key;
  char key_needle[64];

  if(!memory_ready || dossier_id <= 0 || out == NULL || cap == 0)
    return(0);

  snprintf(key_needle, sizeof(key_needle), "toward:%" PRId64, dossier_id);
  e_key = db_escape(key_needle);
  if(e_key == NULL) return(0);

  snprintf(sql, sizeof(sql),
      "SELECT " MEMORY_DOSSIER_FACT_SELECT_COLS
      " FROM dossier_facts WHERE fact_key = '%s'"
      " ORDER BY last_seen DESC LIMIT %zu",
      e_key, cap);
  mem_free(e_key);

  res = db_result_alloc();
  n = 0;
  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n < cap; i++)
      memory_parse_dossier_fact_row(res, i, &out[n++]);
  }
  db_result_free(res);
  return(n);
}

// Fetch recent conversation_log rows in which `dossier_id` was
// mentioned by name (referenced_dossiers JSONB contains N). Populates
// mem_msg_t shape for the retrieval callback.
//
// EXCHANGE_OUT rows (the bot's own prior replies) are filtered out
// unless cfg.embed_own_replies is set. Without this filter the bot
// re-reads its own previous lines on every turn and copies them
// verbatim — e.g. a llm persona that says "/me sips his coffee"
// once will repeat it indefinitely. See CHATBOT.md "Emotes" section.
static size_t
memory_get_mention_msgs(int ns_id, int64_t dossier_id,
    mem_msg_t *out, size_t cap)
{
  db_result_t *res;
  size_t n;
  char sql[1024];
  char kind_filter[64];
  mem_cfg_t cfg;

  if(!memory_ready || dossier_id <= 0 || out == NULL || cap == 0)
    return(0);

  memory_cfg_snapshot(&cfg);

  if(cfg.embed_own_replies)
    kind_filter[0] = '\0';
  else
    snprintf(kind_filter, sizeof(kind_filter),
        " AND kind <> %d", (int)MEM_MSG_EXCHANGE_OUT);

  snprintf(sql, sizeof(sql),
      "SELECT id, COALESCE(user_id,0), COALESCE(dossier_id,0),"
      " bot_name, method, channel, kind, text,"
      " EXTRACT(EPOCH FROM ts)::bigint"
      " FROM conversation_log"
      " WHERE ns_id = %d"
      "   AND referenced_dossiers @> '[%" PRId64 "]'::jsonb"
      "%s"
      " ORDER BY ts DESC LIMIT %zu",
      ns_id, dossier_id, kind_filter, cap);

  res = db_result_alloc();
  n = 0;
  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && n < cap; i++)
    {
      mem_msg_t *m = &out[n++];
      const char *v;

      memset(m, 0, sizeof(*m));
      m->ns_id = ns_id;
      v = db_result_get(res, i, 0); if(v) m->id           = strtoll(v, NULL, 10);
      v = db_result_get(res, i, 1); if(v) m->user_id_or_0 = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 2); if(v) m->dossier_id   = strtoll(v, NULL, 10);
      v = db_result_get(res, i, 3); if(v) snprintf(m->bot_name, sizeof(m->bot_name), "%s", v);
      v = db_result_get(res, i, 4); if(v) snprintf(m->method,   sizeof(m->method),   "%s", v);
      v = db_result_get(res, i, 5); if(v) snprintf(m->channel,  sizeof(m->channel),  "%s", v);
      v = db_result_get(res, i, 6); if(v) m->kind = (mem_msg_kind_t)strtol(v, NULL, 10);
      v = db_result_get(res, i, 7); if(v) snprintf(m->text,     sizeof(m->text),     "%s", v);
      v = db_result_get(res, i, 8); if(v) m->ts = (time_t)strtoll(v, NULL, 10);
    }
  }
  db_result_free(res);
  return(n);
}

// Async continuation state for semantic-recall path. Dossier-side
// facts/mention-msgs are pre-fetched synchronously; the embed round
// trip then feeds memory_retrieve_dossier_embed_done which cosine-scans
// conversation_embeddings, merges recall hits into mention_msgs, and
// delivers the combined payload through the caller's cb once.
typedef struct
{
  int                   ns_id;
  int64_t               dossier_id;
  uint32_t              top_k;
  uint32_t              min_cos_x100;
  char                  model[MEM_EMBED_MODEL_SZ];
  memory_retrieve_cb_t  cb;
  void                 *user;

  mem_fact_t           *facts;
  size_t                n_facts;
  mem_msg_t            *mention_msgs;
  size_t                n_mention_msgs;
} memory_retrieve_dossier_ctx_t;

static void
memory_retrieve_dossier_ctx_free(memory_retrieve_dossier_ctx_t *c)
{
  if(c == NULL)
    return;
  if(c->facts        != NULL) mem_free(c->facts);
  if(c->mention_msgs != NULL) mem_free(c->mention_msgs);
  mem_free(c);
}

static void
memory_retrieve_dossier_embed_done(const llm_embed_response_t *resp)
{
  memory_retrieve_dossier_ctx_t *c = resp->user_data;
  mem_msg_t recall_msgs[64];
  size_t n_recall;
  mem_msg_t *merged;
  size_t     n_merged;
  memory_hit_t hits[64];
  size_t n_hits;

  if(!resp->ok || resp->n_vectors < 1 || resp->dim == 0)
  {
    clam(CLAM_WARN, "memory",
        "recall embed failed: %s — delivering mention-only",
        resp->error ? resp->error : "(no detail)");
    c->cb(c->facts, c->n_facts,
          c->mention_msgs, c->n_mention_msgs, c->user);
    memory_retrieve_dossier_ctx_free(c);
    return;
  }

  n_hits = 0;
  memory_recall_scan_convo(c->ns_id, c->dossier_id, c->model,
      resp->dim, resp->vectors[0], c->top_k, c->min_cos_x100,
      hits, &n_hits);

  n_recall = memory_msgs_from_ids(c->ns_id, hits, n_hits,
      recall_msgs, 64);

  merged = NULL;
  n_merged = 0;
  memory_merge_msgs(c->mention_msgs, c->n_mention_msgs,
      recall_msgs, n_recall, &merged, &n_merged);

  clam(CLAM_DEBUG, "memory",
      "recall: ns=%d dossier=%" PRId64 " hits=%zu merged=%zu",
      c->ns_id, c->dossier_id, n_hits, n_merged);

  c->cb(c->facts, c->n_facts,
        merged != NULL ? merged : c->mention_msgs,
        merged != NULL ? n_merged : c->n_mention_msgs,
        c->user);

  if(merged != NULL) mem_free(merged);
  memory_retrieve_dossier_ctx_free(c);
}

// Dossier-keyed retrieval. Fetches a blended context for `dossier_id`:
//   (a) the dossier's own facts
//   (b) facts from other dossiers keyed "toward:<dossier_id>"
//   (c) recent conversation_log rows mentioning this dossier
//   (d) semantic-recall rows from conversation_embeddings matching
//       `query` (when query is non-empty and an embed model is set)
// and delivers them via cb. See MEMSTORE.md for the retrieval contract.
bool
memory_retrieve_dossier(int ns_id, int64_t dossier_id, const char *query,
    uint32_t top_k, memory_retrieve_cb_t cb, void *user)
{
  const char *inputs[1] = { query };
  memory_retrieve_dossier_ctx_t *rc;
  bool want_recall;
  uint32_t recall_k;
  size_t n_msgs;
  size_t n_own;
  size_t n_toward;
  size_t n_facts_total;
  mem_fact_t *facts;
  uint32_t own_cap;
  uint32_t toward_cap;
  uint32_t msg_cap;
  mem_dossier_fact_t *own_pf;
  mem_msg_t *msgs;
  uint32_t cap;
  mem_cfg_t cfg;

  if(cb == NULL)
    return(FAIL);

  if(!memory_ready || dossier_id <= 0)
  {
    cb(NULL, 0, NULL, 0, user);
    return(SUCCESS);
  }

  memory_cfg_snapshot(&cfg);

  if(!cfg.enabled)
  {
    cb(NULL, 0, NULL, 0, user);
    return(SUCCESS);
  }

  cap = top_k ? top_k : cfg.rag_top_k;
  if(cap == 0 || cap > 64) cap = 8;

  // Budget split. own-facts ~ 1/2, toward-facts ~ 1/4, mention-msgs ~ 1/2.
  own_cap = cap > 1 ? cap / 2 : 1;
  toward_cap = cap / 4;
  msg_cap = cap > 1 ? cap / 2 : 1;

  own_pf = mem_alloc("memory", "rag_pfacts_own",
      sizeof(*own_pf) * (own_cap + toward_cap));
  msgs = NULL;
  if(own_pf == NULL)
  {
    cb(NULL, 0, NULL, 0, user);
    return(FAIL);
  }

  n_own = memory_get_dossier_facts(dossier_id, MEM_FACT_KIND_ANY,
      own_pf, own_cap);

  n_toward = memory_get_toward_facts(dossier_id,
      own_pf + n_own, toward_cap);

  n_facts_total = n_own + n_toward;

  facts = NULL;
  if(n_facts_total > 0)
  {
    facts = mem_alloc("memory", "rag_facts",
        sizeof(*facts) * n_facts_total);
    if(facts != NULL)
    {
      for(size_t i = 0; i < n_facts_total; i++)
        memory_pf_to_fact(&own_pf[i], &facts[i]);
    }

    else
      n_facts_total = 0;
  }

  n_msgs = 0;
  if(msg_cap > 0)
  {
    msgs = mem_alloc("memory", "rag_mention_msgs", sizeof(*msgs) * msg_cap);
    if(msgs != NULL)
      n_msgs = memory_get_mention_msgs(ns_id, dossier_id, msgs, msg_cap);
  }

  mem_free(own_pf);

  // Semantic-recall path: async embed + cosine scan on
  // conversation_embeddings restricted to this dossier. Skipped (and
  // the legacy (facts, mention_msgs) pair delivered synchronously)
  // when the query is empty, no embed model is configured, or the
  // recall budget is zero.
  recall_k = cfg.recall_top_k;
  if(recall_k > 64) recall_k = 64;

  want_recall = (query != NULL && query[0] != '\0'
      && cfg.embed_model[0] != '\0' && recall_k > 0);

  if(!want_recall)
  {
    cb(facts, n_facts_total, msgs, n_msgs, user);
    if(facts != NULL) mem_free(facts);
    if(msgs  != NULL) mem_free(msgs);
    return(SUCCESS);
  }

  rc = mem_alloc("memory",
      "rag_dossier_ctx", sizeof(*rc));
  if(rc == NULL)
  {
    cb(facts, n_facts_total, msgs, n_msgs, user);
    if(facts != NULL) mem_free(facts);
    if(msgs  != NULL) mem_free(msgs);
    return(SUCCESS);
  }

  rc->ns_id          = ns_id;
  rc->dossier_id     = dossier_id;
  rc->top_k          = recall_k;
  rc->min_cos_x100   = cfg.recall_min_cosine_x100;
  rc->cb             = cb;
  rc->user           = user;
  rc->facts          = facts;
  rc->n_facts        = n_facts_total;
  rc->mention_msgs   = msgs;
  rc->n_mention_msgs = n_msgs;
  snprintf(rc->model, sizeof(rc->model), "%s", cfg.embed_model);

  if(llm_embed_submit(cfg.embed_model, inputs, 1,
      memory_retrieve_dossier_embed_done, rc) != SUCCESS)
  {
    clam(CLAM_WARN, "memory",
        "recall embed_submit failed — delivering mention-only");
    cb(facts, n_facts_total, msgs, n_msgs, user);
    memory_retrieve_dossier_ctx_free(rc);
    return(SUCCESS);
  }

  return(SUCCESS);
}

