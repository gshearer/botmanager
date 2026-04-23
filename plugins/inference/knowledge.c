// botmanager — MIT
// Corpus-scoped RAG store for per-persona external knowledge.

#include "knowledge_priv.h"
#include "llm_priv.h"

#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "task.h"

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// Module state

static bool              knowledge_ready = false;
static knowledge_cfg_t   knowledge_cfg;
static pthread_mutex_t   knowledge_cfg_mutex;

// Stats (cumulative counters; last-snapshot totals queried on demand).
static pthread_mutex_t   knowledge_stat_mutex;
static uint64_t          knowledge_stat_inserts = 0;

// Small utilities

void
knowledge_cfg_snapshot(knowledge_cfg_t *out)
{
  pthread_mutex_lock(&knowledge_cfg_mutex);
  *out = knowledge_cfg;
  pthread_mutex_unlock(&knowledge_cfg_mutex);
}

// Fill out->embed_model with the effective embed model, walking a
// three-step fallback:
//    knowledge.embed_model → memory.embed_model → llm.default_embed_model
// The first two let operators override per-subsystem; the LLM-client
// default makes the common single-model config work without setting
// any subsystem-scoped KV. Empty string out = no model; retrieval and
// insert-time embed submissions short-circuit.
void
knowledge_effective_embed_model(char *out, size_t out_sz)
{
  knowledge_cfg_t c;
  const char *mem;
  const char *dflt;
  knowledge_cfg_snapshot(&c);

  if(c.embed_model[0] != '\0')
  {
    snprintf(out, out_sz, "%s", c.embed_model);
    return;
  }

  mem = kv_get_str("memory.embed_model");
  if(mem != NULL && mem[0] != '\0')
  {
    snprintf(out, out_sz, "%s", mem);
    return;
  }

  dflt = kv_get_str("llm.default_embed_model");
  snprintf(out, out_sz, "%s", dflt ? dflt : "");
}

// KV configuration

static void
knowledge_load_config(void)
{
  knowledge_cfg_t c;

  const char *em;
  c.enabled               = kv_get_uint("knowledge.enabled") != 0;
  c.rag_top_k             = (uint32_t)kv_get_uint("knowledge.rag_top_k");
  c.rag_max_context_chars = (uint32_t)kv_get_uint("knowledge.rag_max_context_chars");
  c.chunk_max_chars       = (uint32_t)kv_get_uint("knowledge.chunk_max_chars");
  c.embed_batch_size      = (uint32_t)kv_get_uint("knowledge.embed_batch_size");

  em = kv_get_str("knowledge.embed_model");
  snprintf(c.embed_model, sizeof(c.embed_model), "%s", em ? em : "");

  if(c.rag_top_k == 0)
    c.rag_top_k = KNOWLEDGE_DEF_RAG_TOP_K;

  if(c.rag_max_context_chars == 0)
    c.rag_max_context_chars = KNOWLEDGE_DEF_RAG_MAX_CTX_CHARS;

  if(c.chunk_max_chars == 0)
    c.chunk_max_chars = KNOWLEDGE_DEF_CHUNK_MAX_CHARS;

  if(c.embed_batch_size == 0)
    c.embed_batch_size = KNOWLEDGE_DEF_EMBED_BATCH_SIZE;
  if(c.embed_batch_size > KNOWLEDGE_EMBED_BATCH_MAX)
    c.embed_batch_size = KNOWLEDGE_EMBED_BATCH_MAX;

  pthread_mutex_lock(&knowledge_cfg_mutex);
  knowledge_cfg = c;
  pthread_mutex_unlock(&knowledge_cfg_mutex);
}

static void
knowledge_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  knowledge_load_config();
}

static void
knowledge_register_kv(void)
{
  kv_register("knowledge.enabled", KV_BOOL, "true",
      knowledge_kv_changed, NULL,
      "Enable corpus-scoped knowledge RAG for llm personas");
  kv_register("knowledge.embed_model", KV_STR, "",
      knowledge_kv_changed, NULL,
      "LLM embed model for knowledge RAG"
      " (empty = inherit memory.embed_model)");
  kv_register("knowledge.rag_top_k", KV_UINT32, "5",
      knowledge_kv_changed, NULL,
      "Top-K chunks returned per knowledge_retrieve");
  kv_register("knowledge.rag_max_context_chars", KV_UINT32, "3072",
      knowledge_kv_changed, NULL,
      "Max characters of knowledge context injected into the prompt");
  kv_register("knowledge.chunk_max_chars", KV_UINT32, "1200",
      knowledge_kv_changed, NULL,
      "Max characters per chunk produced by the ingest chunker");
  kv_register("knowledge.embed_batch_size", KV_UINT32, "32",
      knowledge_kv_changed, NULL,
      "Chunks per embed request. Higher = faster ingest and better GPU"
      " utilisation on batching-aware embed endpoints; bounded at"
      " compile time by KNOWLEDGE_EMBED_BATCH_MAX. Default 32.");

  // Reply-pipeline image/citation knobs. reply.c reads them directly
  // via kv_get_* at submit time, so no cfg cache slot is needed; the
  // registration just pins the key + default + help text.
  kv_register("knowledge.rag_images_per_reply", KV_UINT32, "5",
      knowledge_kv_changed, NULL,
      "Max image rows attached to the prompt per reply (0 disables the"
      " IMAGES fence and its system-prompt instruction)");
  kv_register("knowledge.rag_images_max_chars", KV_UINT32, "1024",
      knowledge_kv_changed, NULL,
      "Byte budget for the IMAGES block body in the assembled prompt");
  kv_register("knowledge.rag_include_source_url", KV_BOOL, "true",
      knowledge_kv_changed, NULL,
      "Append (source_url) to each KNOWLEDGE chunk line so personas"
      " can cite; off reclaims prompt budget for non-citing personas");

  // Subject-supplement knobs (I3). The reply pipeline's intent regex
  // extracts a subject name; these tune the supplemental fetch.
  kv_register("knowledge.rag_image_intent_enabled", KV_BOOL, "true",
      knowledge_kv_changed, NULL,
      "Run the intent regex and supplemental subject-image fetch on"
      " incoming replies; off leaves only the RAG-attached images");
  kv_register("knowledge.rag_images_subject_limit", KV_UINT32, "8",
      knowledge_kv_changed, NULL,
      "Raw row cap pulled from knowledge_images_by_subject before the"
      " dedupe+merge+cap against rag_images_per_reply");
  kv_register("knowledge.rag_images_subject_max_age_days", KV_UINT32, "0",
      knowledge_kv_changed, NULL,
      "Age filter for subject-image fetch; 0 = unlimited. A value"
      " like 30 approximates a conversational 'lately'.");
}

// BYTEA helpers — float32 LE packing + hex serialization for Postgres.
//
// Duplicated from core/memory.c for K1. Consolidating into a shared
// core/vec.c is tracked as follow-up cleanup (see TODO.md "shared
// vector helpers" note). The two copies must stay in lock-step on the
// wire format: "'\x<hex>'::bytea" text literal, raw float32 LE bytes.

static const char knowledge_hex_digits[] = "0123456789abcdef";

static char *
knowledge_vec_to_bytea_literal(const float *vec, uint32_t dim)
{
  size_t n_bytes = (size_t)dim * sizeof(float);
  size_t cap = n_bytes * 2 + 16;
  char *out = mem_alloc("knowledge", "bytea_lit", cap);

  const unsigned char *bytes;
  size_t w;
  if(out == NULL)
    return(NULL);

  bytes = (const unsigned char *)vec;
  w = 0;
  out[w++] = '\'';
  out[w++] = '\\';
  out[w++] = 'x';

  for(size_t i = 0; i < n_bytes; i++)
  {
    out[w++] = knowledge_hex_digits[(bytes[i] >> 4) & 0xF];
    out[w++] = knowledge_hex_digits[bytes[i] & 0xF];
  }

  out[w++] = '\'';
  out[w] = '\0';
  return(out);
}

static float *
knowledge_bytea_to_vec(const char *cell, uint32_t expected_dim)
{
  size_t hex_len;
  size_t n_bytes;
  float *out;
  unsigned char *bytes;
  const char *h;
  if(cell == NULL)
    return(NULL);

  if(cell[0] != '\\' || cell[1] != 'x')
    return(NULL);

  hex_len = strlen(cell + 2);

  if((hex_len & 1) != 0)
    return(NULL);

  n_bytes = hex_len / 2;

  if(n_bytes != (size_t)expected_dim * sizeof(float))
    return(NULL);

  out = mem_alloc("knowledge", "vec", n_bytes);

  if(out == NULL)
    return(NULL);

  bytes = (unsigned char *)out;
  h = cell + 2;

  for(size_t i = 0; i < n_bytes; i++)
  {
    unsigned char hi, lo;
    char c = h[i * 2];
    if(c >= '0' && c <= '9')      hi = (unsigned char)(c - '0');
    else if(c >= 'a' && c <= 'f') hi = (unsigned char)(c - 'a' + 10);
    else if(c >= 'A' && c <= 'F') hi = (unsigned char)(c - 'A' + 10);
    else { mem_free(out); return(NULL); }

    c = h[i * 2 + 1];
    if(c >= '0' && c <= '9')      lo = (unsigned char)(c - '0');
    else if(c >= 'a' && c <= 'f') lo = (unsigned char)(c - 'a' + 10);
    else if(c >= 'A' && c <= 'F') lo = (unsigned char)(c - 'A' + 10);
    else { mem_free(out); return(NULL); }

    bytes[i] = (unsigned char)((hi << 4) | lo);
  }

  return(out);
}

float
knowledge_cosine(const float *a, const float *b, uint32_t dim)
{
  double dot = 0.0, na = 0.0, nb = 0.0;

  for(uint32_t i = 0; i < dim; i++)
  {
    dot += (double)a[i] * (double)b[i];
    na  += (double)a[i] * (double)a[i];
    nb  += (double)b[i] * (double)b[i];
  }

  if(na == 0.0 || nb == 0.0)
    return(0.0f);

  return((float)(dot / (sqrt(na) * sqrt(nb))));
}

// Schema

static void
knowledge_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "knowledge", "ensure_tables: %s", res->error);
  }

  db_result_free(res);
}

// Three tables, parallel to memory.c:
//   knowledge_corpora          — one row per named corpus (archwiki, sep, …)
//   knowledge_chunks           — text fragments keyed by corpus
//   knowledge_chunk_embeddings — float32 LE BYTEA vectors, 1:1 with chunks
// Personalities opt in via a `knowledge:` frontmatter field; reply.c splices
// retrieved chunks into the system prompt behind a <<<KNOWLEDGE>>> fence.
static void
knowledge_ensure_tables(void)
{
  knowledge_run_ddl(
      "CREATE TABLE IF NOT EXISTS knowledge_corpora ("
      " name           VARCHAR(64)  PRIMARY KEY,"
      " description    VARCHAR(200) NOT NULL DEFAULT '',"
      " last_ingested  TIMESTAMPTZ,"
      " created        TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")");

  knowledge_run_ddl(
      "CREATE TABLE IF NOT EXISTS knowledge_chunks ("
      " id              BIGSERIAL    PRIMARY KEY,"
      " corpus          VARCHAR(64)  NOT NULL"
      "   REFERENCES knowledge_corpora(name) ON DELETE CASCADE,"
      " source_url      TEXT         NOT NULL DEFAULT '',"
      " section_heading TEXT         NOT NULL DEFAULT '',"
      " text            TEXT         NOT NULL,"
      " created         TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")");

  knowledge_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_knowledge_chunks_corpus"
      " ON knowledge_chunks(corpus)");

  knowledge_run_ddl(
      "CREATE TABLE IF NOT EXISTS knowledge_chunk_embeddings ("
      " chunk_id  BIGINT      PRIMARY KEY"
      "   REFERENCES knowledge_chunks(id) ON DELETE CASCADE,"
      " model     VARCHAR(64) NOT NULL,"
      " dim       INTEGER     NOT NULL,"
      " vec       BYTEA       NOT NULL"
      ")");

  // Images harvested off pages ingested into knowledge_chunks. One row
  // per <img> / og:image / twitter:image discovered during acquire.
  // No embeddings: retrieval is identity-based (JOIN on chunk_id, or
  // scan by subject for explicit image intent).
  knowledge_run_ddl(
      "CREATE TABLE IF NOT EXISTS knowledge_images ("
      " id         BIGSERIAL    PRIMARY KEY,"
      " chunk_id   BIGINT       REFERENCES knowledge_chunks(id)"
      "              ON DELETE CASCADE,"
      " url        TEXT         NOT NULL,"
      " page_url   TEXT,"
      " caption    TEXT,"
      " subject    VARCHAR(128),"
      " width_px   INT,"
      " height_px  INT,"
      " created    TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")");

  knowledge_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_knowledge_images_chunk"
      " ON knowledge_images(chunk_id)");

  knowledge_run_ddl(
      "CREATE INDEX IF NOT EXISTS idx_knowledge_images_subject"
      " ON knowledge_images(subject)");
}

// Embedding write path

static bool
knowledge_write_embedding(int64_t chunk_id, const char *model,
    uint32_t dim, const float *vec)
{
  char *hex     = knowledge_vec_to_bytea_literal(vec, dim);
  char *e_model = db_escape(model);

  size_t cap;
  db_result_t *res;
  char *sql;
  bool ok;
  if(hex == NULL || e_model == NULL)
  {
    if(hex)     mem_free(hex);
    if(e_model) mem_free(e_model);
    return(FAIL);
  }

  cap = strlen(hex) + strlen(e_model) + 256;
  sql = mem_alloc("knowledge", "embed_sql", cap);

  if(sql == NULL)
  {
    mem_free(hex); mem_free(e_model);
    return(FAIL);
  }

  snprintf(sql, cap,
      "INSERT INTO knowledge_chunk_embeddings (chunk_id, model, dim, vec)"
      " VALUES (%" PRId64 ", '%s', %u, %s::bytea)"
      " ON CONFLICT (chunk_id) DO UPDATE"
      " SET model = EXCLUDED.model,"
      "     dim   = EXCLUDED.dim,"
      "     vec   = EXCLUDED.vec",
      chunk_id, e_model, dim, hex);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "knowledge", "write_embedding: %s", res->error);

  db_result_free(res);
  mem_free(sql);
  mem_free(hex);
  mem_free(e_model);
  return(ok ? SUCCESS : FAIL);
}

// Batched embed submission
//
// Ingest throughput at batch=1 is GPU-underutilised: each request pays
// full per-request overhead for ~1200 bytes of text. Batching 32-128
// inputs per request drops total wall-time by ~16x on typical vLLM-
// style endpoints. The batch also exists to make end-to-end ingest
// throughput predictable — at batch=1 with the curl queue capped at
// 256, sustained ingest saturates the queue almost immediately, and
// any non-blocking submit path drops chunks. The batch path uses
// llm_embed_submit_wait, which blocks the ingest thread when the
// queue is full (no silent drops, no log spam — the curl layer's
// wait path doesn't warn).
//
// Memory ownership:
//  - `texts[i]` are mem_strdup copies owned by the batch until flush.
//    The llm layer copies inputs internally during submit, so we free
//    them the moment llm_embed_submit_wait returns.
//  - `result` is a heap-owned struct the completion callback frees.

// Heap-owned context the embed-completion callback uses to match
// returned vectors back to the chunks that produced them. `texts` are
// kept alive across the submit hop so that on batch failure we can
// fall back to individual submissions and isolate the bad chunk(s)
// rather than losing the whole batch.
typedef struct
{
  size_t     n;
  char       model[KNOWLEDGE_EMBED_MODEL_SZ];
  char       corpus[KNOWLEDGE_CORPUS_NAME_SZ];
  int64_t    chunk_ids[KNOWLEDGE_EMBED_BATCH_MAX];
  char      *texts[KNOWLEDGE_EMBED_BATCH_MAX];    // mem_strdup'd; ctx owns
  bool       retry_on_fail;                       // true for N>1 batches
                                                  //  that can split into singles
} knowledge_batch_result_t;

// Free a result context's text array and the context itself.
static void
knowledge_batch_result_free(knowledge_batch_result_t *ctx)
{
  if(ctx == NULL) return;
  for(size_t i = 0; i < ctx->n; i++)
    if(ctx->texts[i] != NULL)
    {
      mem_free(ctx->texts[i]);
      ctx->texts[i] = NULL;
    }
  mem_free(ctx);
}

// Forward decl for the flush helper.
static bool knowledge_batch_flush(knowledge_batch_t *b);

void
knowledge_batch_init(knowledge_batch_t *b, const char *corpus,
    const char *model, uint32_t max_fill)
{
  memset(b, 0, sizeof(*b));
  snprintf(b->corpus, sizeof(b->corpus), "%s", corpus != NULL ? corpus : "");
  snprintf(b->model,  sizeof(b->model),  "%s", model  != NULL ? model  : "");

  if(max_fill == 0)                    max_fill = 1;
  if(max_fill > KNOWLEDGE_EMBED_BATCH_MAX)
    max_fill = KNOWLEDGE_EMBED_BATCH_MAX;
  b->max_fill = max_fill;
}

// Drop and free any outstanding text buffers (used by failure paths).
static void
knowledge_batch_drop_pending(knowledge_batch_t *b)
{
  for(size_t i = 0; i < b->n; i++)
  {
    if(b->texts[i] != NULL)
    {
      mem_free(b->texts[i]);
      b->texts[i] = NULL;
    }
  }
  b->n = 0;
}

// Append one chunk to the batch. Auto-flushes when the batch reaches
// max_fill. The caller remains responsible for reporting per-chunk
// failures; this function either stashes or flushes transparently.
bool
knowledge_batch_add(knowledge_batch_t *b, int64_t chunk_id, const char *text)
{
  if(b == NULL || text == NULL || text[0] == '\0')
    return(FAIL);

  // No model resolvable → nothing to embed. Record the chunk as a
  // failure (row is in DB, vec is missing) and skip.
  if(b->model[0] == '\0')
  {
    b->chunks_embedded_fail++;
    return(SUCCESS);
  }

  if(b->n >= b->max_fill
      && knowledge_batch_flush(b) != SUCCESS)
    return(FAIL);

  b->texts[b->n] = mem_strdup("knowledge", "batch_text", text);
  if(b->texts[b->n] == NULL)
    return(FAIL);

  b->chunk_ids[b->n] = chunk_id;
  b->n++;
  return(SUCCESS);
}

// Forward decls for the singles-retry machinery.
static void knowledge_singles_retry_task(task_t *t);
static void knowledge_spawn_singles_retry(knowledge_batch_result_t *ctx,
    size_t start, size_t count);

// Completion callback for a batched embed request. On success, writes
// one embedding row per returned vector. On failure — when the batch
// was >1 chunk AND is eligible for retry — spawns a worker task that
// re-submits each chunk as a size-1 batch, isolating the specific
// bad chunk(s) instead of losing the whole batch.
//
// Runs on the curl worker thread; must NOT block and must NOT call
// llm_embed_submit_wait directly (that would deadlock against the
// same worker that would need to drain the queue to free slots).
static void
knowledge_batch_done_cb(const llm_embed_response_t *resp)
{
  knowledge_batch_result_t *ctx = resp->user_data;
  size_t n_written;
  if(ctx == NULL)
    return;

  // Full failure — either the HTTP request errored or the server
  // returned nothing usable.
  if(!resp->ok || resp->dim == 0 || resp->n_vectors == 0)
  {
    if(ctx->retry_on_fail && ctx->n > 1)
    {
      clam(CLAM_WARN, "knowledge",
          "batch embed failed (corpus=%s n=%zu model='%s'): %s"
          " — retrying as singles to isolate bad chunk(s)",
          ctx->corpus, ctx->n, ctx->model,
          resp->error ? resp->error : "(no detail)");
      knowledge_spawn_singles_retry(ctx, 0, ctx->n);
      return;   // singles task now owns ctx's texts
    }

    // Size-1 batch, or a singles-retry submission that still failed.
    // This is a genuinely-bad chunk or a persistent fault; log the
    // specific identifying info so it's grep-able later.
    for(size_t i = 0; i < ctx->n; i++)
      clam(CLAM_WARN, "knowledge",
          "chunk id=%" PRId64 " (corpus=%s) failed embedding: %s",
          ctx->chunk_ids[i], ctx->corpus,
          resp->error ? resp->error : "(no detail)");
    knowledge_batch_result_free(ctx);
    return;
  }

  // Write the embeddings the server returned.
  n_written = resp->n_vectors < ctx->n ? resp->n_vectors : ctx->n;
  for(size_t i = 0; i < n_written; i++)
    knowledge_write_embedding(ctx->chunk_ids[i], ctx->model,
        resp->dim, resp->vectors[i]);

  // Partial response: fewer vectors than inputs. Retry the tail as
  // singles (same as a full failure for the trailing chunks).
  if(n_written < ctx->n)
  {
    size_t tail = ctx->n - n_written;
    clam(CLAM_WARN, "knowledge",
        "batch partial (corpus=%s): submitted %zu, received %zu —"
        " retrying trailing %zu chunk(s) as singles",
        ctx->corpus, ctx->n, n_written, tail);

    if(ctx->retry_on_fail)
    {
      // Free only the texts we've already embedded; pass the tail
      // to the singles task.
      for(size_t i = 0; i < n_written; i++)
      {
        if(ctx->texts[i] != NULL)
        {
          mem_free(ctx->texts[i]);
          ctx->texts[i] = NULL;
        }
      }
      knowledge_spawn_singles_retry(ctx, n_written, tail);
      return;
    }

    // Retry not allowed; log each specific missing chunk_id.
    for(size_t i = n_written; i < ctx->n; i++)
      clam(CLAM_WARN, "knowledge",
          "chunk id=%" PRId64 " (corpus=%s) missing from batch response",
          ctx->chunk_ids[i], ctx->corpus);
  }

  knowledge_batch_result_free(ctx);
}

// Task data for the singles retry. Owns the text pointers it carries
// and frees them after processing. Runs on a worker thread so it can
// safely call llm_embed_submit_wait (which may block).
typedef struct
{
  size_t    n;
  char      model[KNOWLEDGE_EMBED_MODEL_SZ];
  char      corpus[KNOWLEDGE_CORPUS_NAME_SZ];
  int64_t   chunk_ids[KNOWLEDGE_EMBED_BATCH_MAX];
  char     *texts[KNOWLEDGE_EMBED_BATCH_MAX];
} knowledge_singles_t;

// Spawn a worker-thread task that will re-submit a slice of ctx's
// chunks as individual size-1 requests. Transfers text ownership
// from ctx into the task; on return, the batch context is freed.
static void
knowledge_spawn_singles_retry(knowledge_batch_result_t *ctx,
    size_t start, size_t count)
{
  knowledge_singles_t *rt;
  if(ctx == NULL || count == 0)
  {
    knowledge_batch_result_free(ctx);
    return;
  }

  rt = mem_alloc("knowledge", "singles_task",
      sizeof(*rt));
  if(rt == NULL)
  {
    clam(CLAM_WARN, "knowledge",
        "singles retry alloc failed; chunks %zu..%zu lose embeddings",
        start, start + count);
    knowledge_batch_result_free(ctx);
    return;
  }

  rt->n = count;
  snprintf(rt->model,  sizeof(rt->model),  "%s", ctx->model);
  snprintf(rt->corpus, sizeof(rt->corpus), "%s", ctx->corpus);
  memset(rt->texts, 0, sizeof(rt->texts));

  for(size_t i = 0; i < count; i++)
  {
    rt->chunk_ids[i] = ctx->chunk_ids[start + i];
    rt->texts[i]     = ctx->texts[start + i];   // transfer ownership
    ctx->texts[start + i] = NULL;                // ctx no longer owns
  }

  // ctx's remaining texts (outside the transferred range) are freed
  // by knowledge_batch_result_free.
  knowledge_batch_result_free(ctx);

  task_add("knowledge_singles", TASK_THREAD, 100,
      knowledge_singles_retry_task, rt);
}

// Worker-thread task: resubmit each chunk as a size-1 batch via the
// blocking submit path. On failure the size-1 result-ctx's done_cb
// logs the specific chunk_id (retry_on_fail=false prevents loops).
static void
knowledge_singles_retry_task(task_t *t)
{
  knowledge_singles_t *rt = t->data;
  size_t submitted, setup_failed;
  if(rt == NULL)
    return;

  submitted = 0;
  setup_failed = 0;

  for(size_t i = 0; i < rt->n; i++)
  {
    knowledge_batch_result_t *ctx;
    const char *inputs[1];
    if(rt->texts[i] == NULL || rt->texts[i][0] == '\0')
    {
      if(rt->texts[i] != NULL) mem_free(rt->texts[i]);
      rt->texts[i] = NULL;
      continue;
    }

    // Build a size-1 result ctx, transferring text ownership.
    ctx = mem_alloc("knowledge", "single_ctx",
        sizeof(*ctx));
    if(ctx == NULL)
    {
      mem_free(rt->texts[i]);
      rt->texts[i] = NULL;
      setup_failed++;
      continue;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->n               = 1;
    ctx->chunk_ids[0]    = rt->chunk_ids[i];
    ctx->texts[0]        = rt->texts[i];   // transfer ownership
    rt->texts[i]         = NULL;
    ctx->retry_on_fail   = false;          // no further splitting
    snprintf(ctx->model,  sizeof(ctx->model),  "%s", rt->model);
    snprintf(ctx->corpus, sizeof(ctx->corpus), "%s", rt->corpus);

    memcpy(inputs, (const char *[1]){ ctx->texts[0] }, sizeof(inputs));

    if(llm_embed_submit_wait(rt->model, inputs, 1,
          knowledge_batch_done_cb, ctx) != SUCCESS)
    {
      clam(CLAM_WARN, "knowledge",
          "singles retry: submit_wait failed for chunk id=%" PRId64
          " (corpus=%s) — shutdown or setup failure",
          ctx->chunk_ids[0], rt->corpus);
      knowledge_batch_result_free(ctx);
      setup_failed++;
      continue;
    }
    submitted++;
  }

  if(submitted > 0 || setup_failed > 0)
    clam(CLAM_INFO, "knowledge",
        "singles retry complete (corpus=%s): %zu resubmitted, %zu"
        " setup-failed", rt->corpus, submitted, setup_failed);

  // Any text pointers still in rt (shouldn't be, but defensive) get
  // freed.
  for(size_t i = 0; i < rt->n; i++)
    if(rt->texts[i] != NULL)
    {
      mem_free(rt->texts[i]);
      rt->texts[i] = NULL;
    }
  mem_free(rt);
}

// Submit the pending batch (if any) as a single multi-input embed
// request. Blocks on curl_request_submit_wait when the queue is full
// (no silent drops). On SUCCESS, ownership of the mem_strdup'd texts
// transfers from `b` into the heap result context — the singles-retry
// path needs them to survive past the submit hop so it can isolate
// bad chunks without re-reading from the DB.
static bool
knowledge_batch_flush(knowledge_batch_t *b)
{
  knowledge_batch_result_t *ctx;
  const char *inputs[KNOWLEDGE_EMBED_BATCH_MAX];
  bool ok;
  if(b == NULL || b->n == 0)
    return(SUCCESS);

  ctx = mem_alloc("knowledge", "batch_ctx", sizeof(*ctx));
  if(ctx == NULL)
  {
    clam(CLAM_WARN, "knowledge",
        "batch flush alloc failure — dropping %zu pending chunk(s)",
        b->n);
    b->chunks_embedded_fail += b->n;
    knowledge_batch_drop_pending(b);
    return(FAIL);
  }

  memset(ctx, 0, sizeof(*ctx));
  ctx->n = b->n;
  snprintf(ctx->model,  sizeof(ctx->model),  "%s", b->model);
  snprintf(ctx->corpus, sizeof(ctx->corpus), "%s", b->corpus);
  memcpy(ctx->chunk_ids, b->chunk_ids, sizeof(int64_t) * b->n);
  // retry_on_fail is set below based on whether the batch is a true
  // batch (>1) or already a single-chunk request.
  ctx->retry_on_fail = (b->n > 1);

  // Build the inputs[] array of const char * that llm_embed_submit_wait
  // expects. Copies the pointers, not the text; llm does the deep copy
  // internally, and ownership of our text pointers transfers from `b`
  // to `ctx` below (not freed here, consumed later by ctx cleanup or
  // the singles-retry path).
  for(size_t i = 0; i < b->n; i++)
    inputs[i] = b->texts[i];

  ok = (llm_embed_submit_wait(b->model, inputs, b->n,
      knowledge_batch_done_cb, ctx) == SUCCESS);

  if(ok)
  {
    // Transfer text ownership from batch to result ctx.
    for(size_t i = 0; i < b->n; i++)
    {
      ctx->texts[i] = b->texts[i];
      b->texts[i]   = NULL;
    }
    b->n = 0;

    b->chunks_submitted   += ctx->n;
    b->chunks_embedded_ok += ctx->n;   // optimistic; batch cb may
                                       // downgrade on partial failure
  }

  else
  {
    clam(CLAM_WARN, "knowledge",
        "batch submit_wait returned FAIL (model='%s', n=%zu) —"
        " chunks have no embedding", b->model, b->n);
    b->chunks_embedded_fail += b->n;
    mem_free(ctx);
    knowledge_batch_drop_pending(b);   // submit never happened,
                                       // free batch's texts
  }

  return(ok ? SUCCESS : FAIL);
}

// Final flush + any cleanup. Safe to call on a zeroed batch or after
// previous flushes.
void
knowledge_batch_free(knowledge_batch_t *b)
{
  if(b == NULL) return;
  knowledge_batch_flush(b);
  knowledge_batch_drop_pending(b);
}

// Corpus administration

bool
knowledge_corpus_upsert(const char *name, const char *description)
{
  char *e_name;
  size_t sql_sz;
  db_result_t *res;
  char *e_desc;
  char *sql;
  bool ok;
  if(!knowledge_ready || name == NULL || name[0] == '\0')
    return(FAIL);

  e_name = db_escape(name);
  e_desc = db_escape(description != NULL ? description : "");

  if(e_name == NULL || e_desc == NULL)
  {
    if(e_name) mem_free(e_name);
    if(e_desc) mem_free(e_desc);
    return(FAIL);
  }

  sql_sz = strlen(e_name) + strlen(e_desc) + 256;
  sql = mem_alloc("knowledge", "corpus_sql", sql_sz);

  if(sql == NULL)
  {
    mem_free(e_name); mem_free(e_desc);
    return(FAIL);
  }

  // Only overwrite the description when a non-empty value is supplied;
  // a bare ingest call (no description arg) shouldn't clobber a
  // carefully-written prior description.
  if(description != NULL && description[0] != '\0')
    snprintf(sql, sql_sz,
        "INSERT INTO knowledge_corpora (name, description)"
        " VALUES ('%s', '%s')"
        " ON CONFLICT (name) DO UPDATE"
        " SET description = EXCLUDED.description",
        e_name, e_desc);
  else
    snprintf(sql, sql_sz,
        "INSERT INTO knowledge_corpora (name, description)"
        " VALUES ('%s', '%s')"
        " ON CONFLICT (name) DO NOTHING",
        e_name, e_desc);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "knowledge", "corpus upsert: %s", res->error);

  db_result_free(res);
  mem_free(sql);
  mem_free(e_name);
  mem_free(e_desc);
  return(ok ? SUCCESS : FAIL);
}

bool
knowledge_corpus_delete(const char *name)
{
  char *e_name;
  char sql[256];
  db_result_t *res;
  bool ok;
  if(!knowledge_ready || name == NULL || name[0] == '\0')
    return(FAIL);

  e_name = db_escape(name);
  if(e_name == NULL) return(FAIL);

  snprintf(sql, sizeof(sql),
      "DELETE FROM knowledge_corpora WHERE name = '%s'", e_name);
  mem_free(e_name);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok
      && res->rows_affected > 0;
  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

uint32_t
knowledge_corpus_iterate(knowledge_corpus_iter_cb_t cb, void *user)
{
  const char *sql;
  db_result_t *res;
  uint32_t n;
  if(!knowledge_ready || cb == NULL)
    return(0);

  sql = "SELECT c.name, c.description,"
      " COALESCE(EXTRACT(EPOCH FROM c.last_ingested)::BIGINT, 0),"
      " COALESCE((SELECT COUNT(*) FROM knowledge_chunks k"
      "           WHERE k.corpus = c.name), 0)"
      " FROM knowledge_corpora c"
      " ORDER BY c.name";

  res = db_result_alloc();
  n = 0;

  if((db_query(sql, res) == SUCCESS) && res->ok)
  {
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *name = db_result_get(res, i, 0);
      const char *desc = db_result_get(res, i, 1);
      const char *li   = db_result_get(res, i, 2);
      const char *cc   = db_result_get(res, i, 3);

      cb(name ? name : "", desc ? desc : "",
         cc ? (int64_t)strtoll(cc, NULL, 10) : 0,
         li ? (time_t)strtoll(li, NULL, 10) : 0,
         user);
      n++;
    }
  }

  db_result_free(res);
  return(n);
}

// Insert path
//
// `knowledge_insert_chunk_raw` does just the DB INSERT and corpus-
// last_ingested bump. It does NOT touch the embedding path — callers
// that want an embedding attached must route the returned chunk_id
// through a knowledge_batch_t (see knowledge_batch_add above).
//
// `knowledge_insert_chunk` is the public API; it wraps _raw with a
// transient single-chunk batch so one-off callers keep working
// without knowing about the batch machinery.

bool
knowledge_insert_chunk_raw(const char *corpus, const char *source_url,
    const char *section_heading, const char *text, int64_t *out_id)
{
  char *e_corp;
  size_t sql_sz;
  db_result_t *res;
  char *e_url;
  char *sql;
  bool ok;
  char *e_sec;
  int64_t new_id;
  char *e_txt;
  if(!knowledge_ready || corpus == NULL || corpus[0] == '\0'
      || text == NULL || text[0] == '\0')
    return(FAIL);

  e_corp = db_escape(corpus);
  e_url = db_escape(source_url      != NULL ? source_url      : "");
  e_sec = db_escape(section_heading != NULL ? section_heading : "");
  e_txt = db_escape(text);

  if(e_corp == NULL || e_url == NULL || e_sec == NULL || e_txt == NULL)
  {
    if(e_corp) mem_free(e_corp);
    if(e_url)  mem_free(e_url);
    if(e_sec)  mem_free(e_sec);
    if(e_txt)  mem_free(e_txt);
    return(FAIL);
  }

  sql_sz = strlen(e_corp) + strlen(e_url) + strlen(e_sec)
      + strlen(e_txt) + 512;
  sql = mem_alloc("knowledge", "chunk_sql", sql_sz);

  if(sql == NULL)
  {
    mem_free(e_corp); mem_free(e_url); mem_free(e_sec); mem_free(e_txt);
    return(FAIL);
  }

  snprintf(sql, sql_sz,
      "INSERT INTO knowledge_chunks"
      " (corpus, source_url, section_heading, text)"
      " VALUES ('%s', '%s', '%s', '%s')"
      " RETURNING id",
      e_corp, e_url, e_sec, e_txt);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok && res->rows == 1;
  new_id = 0;

  if(ok)
  {
    const char *id_s = db_result_get(res, 0, 0);
    if(id_s != NULL)
      new_id = (int64_t)strtoll(id_s, NULL, 10);
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "knowledge", "insert_chunk: %s", res->error);

  db_result_free(res);
  mem_free(sql);
  mem_free(e_corp);
  mem_free(e_url);
  mem_free(e_sec);
  mem_free(e_txt);

  if(!ok || new_id == 0)
    return(FAIL);

  // Bump corpus last_ingested. Best-effort; don't fail the insert on a
  // secondary update error.
  {
    char *e_c2 = db_escape(corpus);
    if(e_c2 != NULL)
    {
      char u_sql[256];
      db_result_t *ur;
      snprintf(u_sql, sizeof(u_sql),
          "UPDATE knowledge_corpora SET last_ingested = NOW()"
          " WHERE name = '%s'", e_c2);
      ur = db_result_alloc();
      db_query(u_sql, ur);
      db_result_free(ur);
      mem_free(e_c2);
    }
  }

  pthread_mutex_lock(&knowledge_stat_mutex);
  knowledge_stat_inserts++;
  pthread_mutex_unlock(&knowledge_stat_mutex);

  if(out_id != NULL)
    *out_id = new_id;

  return(SUCCESS);
}

bool
knowledge_insert_chunk(const char *corpus, const char *source_url,
    const char *section_heading, const char *text, int64_t *out_id)
{
  int64_t new_id = 0;
  char model[KNOWLEDGE_EMBED_MODEL_SZ];
  if(knowledge_insert_chunk_raw(corpus, source_url, section_heading,
        text, &new_id) != SUCCESS)
    return(FAIL);

  // Route the single chunk through a size-1 batch so the embed path
  // is identical to bulk ingest. Single-chunk batches are trivially
  // correct and let us keep exactly one embed-submit code path.
  knowledge_effective_embed_model(model, sizeof(model));

  if(model[0] != '\0')
  {
    knowledge_batch_t b;
    knowledge_batch_init(&b, corpus, model, 1);
    knowledge_batch_add(&b, new_id, text);
    knowledge_batch_free(&b);
  }

  if(out_id != NULL)
    *out_id = new_id;
  return(SUCCESS);
}

// Image insert path
//
// Synchronous INSERT into knowledge_images. No embedding work — image
// retrieval is identity-based (chunk_id JOIN, or subject scan). Width
// and height are passed through as signed ints; 0 or negative values
// land as NULL in the row, which is the right thing when a page's
// <img> tag omits the attribute (very common; CMSs rely on CSS).

bool
knowledge_insert_image(int64_t chunk_id, const char *url,
    const char *page_url, const char *caption, const char *subject,
    int width_px, int height_px)
{
  char *e_url;
  char w_lit[32];
  size_t sql_sz;
  db_result_t *res;
  char *e_page;
  char h_lit[32];
  char *sql;
  bool ok;
  char *e_cap;
  char *e_subj;
  if(!knowledge_ready || chunk_id <= 0 || url == NULL || url[0] == '\0')
    return(FAIL);

  e_url = db_escape(url);
  e_page = db_escape(page_url != NULL ? page_url : "");
  e_cap = db_escape(caption  != NULL ? caption  : "");
  e_subj = db_escape(subject  != NULL ? subject  : "");

  if(e_url == NULL || e_page == NULL || e_cap == NULL || e_subj == NULL)
  {
    if(e_url)  mem_free(e_url);
    if(e_page) mem_free(e_page);
    if(e_cap)  mem_free(e_cap);
    if(e_subj) mem_free(e_subj);
    return(FAIL);
  }

  // width/height < 1 mean "not declared in the HTML"; store as NULL so
  // the column semantics match the page's intent and future range
  // filters don't have to special-case sentinel values.

  if(width_px > 0)  snprintf(w_lit, sizeof(w_lit), "%d", width_px);
  else              snprintf(w_lit, sizeof(w_lit), "NULL");

  if(height_px > 0) snprintf(h_lit, sizeof(h_lit), "%d", height_px);
  else              snprintf(h_lit, sizeof(h_lit), "NULL");

  sql_sz = strlen(e_url) + strlen(e_page) + strlen(e_cap)
      + strlen(e_subj) + 256;
  sql = mem_alloc("knowledge", "image_sql", sql_sz);

  if(sql == NULL)
  {
    mem_free(e_url); mem_free(e_page); mem_free(e_cap); mem_free(e_subj);
    return(FAIL);
  }

  snprintf(sql, sql_sz,
      "INSERT INTO knowledge_images"
      " (chunk_id, url, page_url, caption, subject, width_px, height_px)"
      " VALUES (%" PRId64 ", '%s', '%s', '%s', '%s', %s, %s)",
      chunk_id, e_url, e_page, e_cap, e_subj, w_lit, h_lit);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok && res->error[0] != '\0')
    clam(CLAM_WARN, "knowledge", "insert_image: %s", res->error);

  db_result_free(res);
  mem_free(sql);
  mem_free(e_url);
  mem_free(e_page);
  mem_free(e_cap);
  mem_free(e_subj);

  return(ok ? SUCCESS : FAIL);
}

// Retrieval

typedef struct
{
  int64_t  id;
  float    score;
} knowledge_hit_t;

// Keep the top-K (highest score) in a descending-sorted array.
static void
knowledge_topk_insert(knowledge_hit_t *buf, size_t cap, size_t *n,
    int64_t id, float score)
{
  size_t pos;
  if(*n < cap)
  {
    size_t pos = *n;
    while(pos > 0 && buf[pos - 1].score < score)
    {
      buf[pos] = buf[pos - 1];
      pos--;
    }
    buf[pos].id = id;
    buf[pos].score = score;
    (*n)++;
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

// Build a SQL `IN ('a','b',...)` clause from a semicolon-separated
// corpus list. Escapes each name via db_escape, trims whitespace,
// skips empty entries, and writes into `out` (NUL-terminated). Returns
// the number of names that made it into the clause; 0 means the list
// was empty and `out` now holds `()` (caller should treat as empty).
//
// `out_sz` should be >= 1024; KNOWLEDGE_CORPUS_LIST_MAX * (64 quoted +
// 3 punctuation) = ~1k is the worst case at our 16-name cap.
#define KNOWLEDGE_CORPUS_LIST_MAX   16

static size_t
knowledge_build_corpus_in_clause(const char *list,
    char *out, size_t out_sz)
{
  char copy[512];
  size_t pos;
  char *saveptr;
  size_t n;
  snprintf(copy, sizeof(copy), "%s", list != NULL ? list : "");

  pos = 0;
  pos += (size_t)snprintf(out + pos, out_sz - pos, "(");

  saveptr = NULL;
  n = 0;
  for(char *tok = strtok_r(copy, ";", &saveptr);
      tok != NULL && n < KNOWLEDGE_CORPUS_LIST_MAX;
      tok = strtok_r(NULL, ";", &saveptr))
  {
    size_t tlen;
    char *e;
    while(*tok == ' ' || *tok == '\t') tok++;
    tlen = strlen(tok);
    while(tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t'))
      tok[--tlen] = '\0';
    if(tok[0] == '\0') continue;

    e = db_escape(tok);
    if(e == NULL) continue;

    pos += (size_t)snprintf(out + pos, out_sz - pos,
        "%s'%s'", n == 0 ? "" : ",", e);
    mem_free(e);
    n++;
  }

  snprintf(out + pos, out_sz - pos, ")");
  return(n);
}

// Cosine-scan the named corpora's embeddings against qvec. `corpus_list`
// is the raw semicolon-separated list; names are parsed, trimmed,
// SQL-escaped, and joined into an IN clause. Up to
// KNOWLEDGE_CORPUS_LIST_MAX names; excess silently truncated.
static void
knowledge_scan(const char *corpus_list, const char *model, uint32_t dim,
    const float *qvec, knowledge_hit_t *buf, size_t cap, size_t *n_out)
{
  char in_clause[1200];
  size_t n_names = knowledge_build_corpus_in_clause(corpus_list,
      in_clause, sizeof(in_clause));
  char *e_model;
  size_t sz;
  db_result_t *res;
  char *sql;
  if(n_names == 0)
    return;

  e_model = db_escape(model);
  if(e_model == NULL)
    return;

  sz = strlen(in_clause) + strlen(e_model) + 256;
  sql = mem_alloc("knowledge", "scan_sql", sz);

  if(sql == NULL)
  {
    mem_free(e_model);
    return;
  }

  snprintf(sql, sz,
      "SELECT e.chunk_id, e.vec"
      " FROM knowledge_chunk_embeddings e JOIN knowledge_chunks x"
      " ON e.chunk_id = x.id"
      " WHERE x.corpus IN %s AND e.model = '%s' AND e.dim = %u",
      in_clause, e_model, dim);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, "knowledge", "retrieve scan: %s", res->error);
    db_result_free(res);
    mem_free(sql);
    mem_free(e_model);
    return;
  }

  for(uint32_t i = 0; i < res->rows; i++)
  {
    const char *id_s  = db_result_get(res, i, 0);
    const char *vec_s = db_result_get(res, i, 1);
    float *rv;
    float score;
    int64_t id;
    if(id_s == NULL || vec_s == NULL) continue;

    rv = knowledge_bytea_to_vec(vec_s, dim);
    if(rv == NULL) continue;

    score = knowledge_cosine(qvec, rv, dim);
    id = (int64_t)strtoll(id_s, NULL, 10);

    knowledge_topk_insert(buf, cap, n_out, id, score);
    mem_free(rv);
  }

  db_result_free(res);
  mem_free(sql);
  mem_free(e_model);
}

static void
knowledge_build_in_list(char *out, size_t out_sz,
    const knowledge_hit_t *hits, size_t n)
{
  size_t w = 0;
  w += (size_t)snprintf(out + w, out_sz - w, "(");
  for(size_t i = 0; i < n; i++)
    w += (size_t)snprintf(out + w, out_sz - w,
        "%s%" PRId64, i == 0 ? "" : ",", hits[i].id);
  snprintf(out + w, out_sz - w, ")");
}

static void
knowledge_parse_chunk_row(const db_result_t *r, uint32_t row,
    knowledge_chunk_t *c)
{
  const char *v;
  memset(c, 0, sizeof(*c));

  v = db_result_get(r, row, 0);
  if(v != NULL) c->id = (int64_t)strtoll(v, NULL, 10);

  v = db_result_get(r, row, 1);
  if(v != NULL) snprintf(c->corpus, sizeof(c->corpus), "%s", v);

  v = db_result_get(r, row, 2);
  if(v != NULL) snprintf(c->source_url, sizeof(c->source_url), "%s", v);

  v = db_result_get(r, row, 3);
  if(v != NULL)
    snprintf(c->section_heading, sizeof(c->section_heading), "%s", v);

  v = db_result_get(r, row, 4);
  if(v != NULL) snprintf(c->text, sizeof(c->text), "%s", v);

  v = db_result_get(r, row, 5);
  if(v != NULL) c->created = (time_t)strtoll(v, NULL, 10);
}

#define KNOWLEDGE_CHUNK_SELECT_COLS \
    "id, corpus, source_url, section_heading, text," \
    " EXTRACT(EPOCH FROM created)::bigint"

static void
knowledge_deliver_hits(const knowledge_hit_t *hits, size_t n,
    knowledge_retrieve_cb_t cb, void *user)
{
  knowledge_chunk_t *chunks = NULL;
  size_t             nc     = 0;

  if(n > 0)
  {
    // top_k caps at 64 and each id renders to ≤21 bytes ("%" PRId64 plus
    // comma), so in_list fits comfortably below 2 KB; the sql buffer is
    // sized at 4 KB to absorb it with headroom for the SELECT cols.
    char in_list[32 * 64];
    char sql[4096];
    db_result_t *res;
    knowledge_build_in_list(in_list, sizeof(in_list), hits, n);

    snprintf(sql, sizeof(sql),
        "SELECT " KNOWLEDGE_CHUNK_SELECT_COLS
        " FROM knowledge_chunks WHERE id IN %s",
        in_list);

    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
    {
      chunks = mem_alloc("knowledge", "rag_chunks",
          sizeof(knowledge_chunk_t) * res->rows);

      if(chunks != NULL)
      {
        for(uint32_t i = 0; i < res->rows; i++)
        {
          knowledge_parse_chunk_row(res, i, &chunks[nc]);

          // Stamp the score from the hit buffer (SELECT order may
          // differ from ranking order; match by id).
          for(size_t h = 0; h < n; h++)
          {
            if(hits[h].id == chunks[nc].id)
            {
              chunks[nc].score = hits[h].score;
              break;
            }
          }
          nc++;
        }
      }
    }

    db_result_free(res);
  }

  cb(chunks, nc, user);

  if(chunks != NULL) mem_free(chunks);
}

static void
knowledge_retrieve_with_vec(const char *corpus, const char *model,
    uint32_t dim, const float *qvec, uint32_t top_k,
    knowledge_retrieve_cb_t cb, void *user)
{
  knowledge_hit_t buf[64];
  size_t          n;
  if(top_k == 0 || top_k > 64)
  {
    knowledge_cfg_t c;
    knowledge_cfg_snapshot(&c);
    top_k = c.rag_top_k;
    if(top_k == 0 || top_k > 64)
      top_k = KNOWLEDGE_DEF_RAG_TOP_K;
  }

  n = 0;

  knowledge_scan(corpus, model, dim, qvec, buf, top_k, &n);

  knowledge_deliver_hits(buf, n, cb, user);
}

typedef struct
{
  char                     corpus[KNOWLEDGE_CORPUS_NAME_SZ];
  char                     model[KNOWLEDGE_EMBED_MODEL_SZ];
  uint32_t                 top_k;
  knowledge_retrieve_cb_t  cb;
  void                    *user;
} knowledge_retrieval_ctx_t;

static void
knowledge_retrieve_embed_done(const llm_embed_response_t *resp)
{
  knowledge_retrieval_ctx_t *c = resp->user_data;

  if(!resp->ok || resp->n_vectors < 1 || resp->dim == 0)
  {
    clam(CLAM_WARN, "knowledge", "retrieve embed failed: %s",
        resp->error ? resp->error : "(no detail)");
    c->cb(NULL, 0, c->user);
    mem_free(c);
    return;
  }

  knowledge_retrieve_with_vec(c->corpus, c->model, resp->dim,
      resp->vectors[0], c->top_k, c->cb, c->user);

  mem_free(c);
}

bool
knowledge_retrieve(const char *corpus, const char *query,
    uint32_t top_k, knowledge_retrieve_cb_t cb, void *user)
{
  knowledge_cfg_t cfg;
  char model[KNOWLEDGE_EMBED_MODEL_SZ];
  knowledge_retrieval_ctx_t *c;
  const char *inputs[1];
  if(cb == NULL)
    return(FAIL);

  knowledge_cfg_snapshot(&cfg);

  knowledge_effective_embed_model(model, sizeof(model));

  // Sync empty path: disabled, no corpus, no model, or empty query.
  if(!knowledge_ready || !cfg.enabled
      || corpus == NULL || corpus[0] == '\0'
      || query == NULL || query[0] == '\0'
      || model[0] == '\0')
  {
    cb(NULL, 0, user);
    return(SUCCESS);
  }

  c = mem_alloc("knowledge", "rag_ctx", sizeof(*c));

  if(c == NULL)
  {
    cb(NULL, 0, user);
    return(FAIL);
  }

  snprintf(c->corpus, sizeof(c->corpus), "%s", corpus);
  snprintf(c->model,  sizeof(c->model),  "%s", model);
  c->top_k = top_k;
  c->cb    = cb;
  c->user  = user;

  memcpy(inputs, (const char *[1]){ query }, sizeof(inputs));

  if(llm_embed_submit(model, inputs, 1,
        knowledge_retrieve_embed_done, c) != SUCCESS)
  {
    clam(CLAM_WARN, "knowledge",
        "retrieve embed submit failed (model '%s')", model);
    cb(NULL, 0, user);
    mem_free(c);
    return(FAIL);
  }

  return(SUCCESS);
}

// Image retrieval — identity-based (JOIN by chunk_id). No embeddings.

// Cap the number of chunk ids expanded into the IN (...) clause. Mirrors
// the retrieve top-K ceiling (64); beyond that we'd blow the sql buffer.
#define KNOWLEDGE_IMG_CHUNK_IDS_MAX   64

size_t
knowledge_images_for_chunks(const int64_t *chunk_ids, size_t n_ids,
    knowledge_image_t *out, size_t out_cap)
{
  size_t n;
  char sql[4096];
  db_result_t *res;
  char in_list[32 * KNOWLEDGE_IMG_CHUNK_IDS_MAX];
  size_t nc;
  size_t w;
  if(!knowledge_ready || chunk_ids == NULL || n_ids == 0
      || out == NULL || out_cap == 0)
    return(0);

  n = n_ids > KNOWLEDGE_IMG_CHUNK_IDS_MAX
      ? KNOWLEDGE_IMG_CHUNK_IDS_MAX : n_ids;

  // 32 bytes per id literal leaves headroom over the worst-case
  // 21-char PRId64 plus comma.
  w = 0;
  w += (size_t)snprintf(in_list + w, sizeof(in_list) - w, "(");
  for(size_t i = 0; i < n; i++)
    w += (size_t)snprintf(in_list + w, sizeof(in_list) - w,
        "%s%" PRId64, i == 0 ? "" : ",", chunk_ids[i]);
  snprintf(in_list + w, sizeof(in_list) - w, ")");

  snprintf(sql, sizeof(sql),
      "SELECT id, chunk_id, url, COALESCE(page_url, ''),"
      " COALESCE(caption, ''), COALESCE(subject, ''),"
      " COALESCE(width_px, 0), COALESCE(height_px, 0),"
      " EXTRACT(EPOCH FROM created)::bigint"
      " FROM knowledge_images WHERE chunk_id IN %s"
      " ORDER BY created DESC LIMIT %zu",
      in_list, out_cap);

  res = db_result_alloc();
  nc = 0;

  if((db_query(sql, res) == SUCCESS) && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && nc < out_cap; i++)
    {
      knowledge_image_t *img = &out[nc];
      const char *v;
      memset(img, 0, sizeof(*img));

      v = db_result_get(res, i, 0);
      if(v != NULL) img->id = (int64_t)strtoll(v, NULL, 10);
      v = db_result_get(res, i, 1);
      if(v != NULL) img->chunk_id = (int64_t)strtoll(v, NULL, 10);
      v = db_result_get(res, i, 2);
      if(v != NULL) snprintf(img->url, sizeof(img->url), "%s", v);
      v = db_result_get(res, i, 3);
      if(v != NULL) snprintf(img->page_url, sizeof(img->page_url), "%s", v);
      v = db_result_get(res, i, 4);
      if(v != NULL) snprintf(img->caption, sizeof(img->caption), "%s", v);
      v = db_result_get(res, i, 5);
      if(v != NULL) snprintf(img->subject, sizeof(img->subject), "%s", v);
      v = db_result_get(res, i, 6);
      if(v != NULL) img->width_px = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 7);
      if(v != NULL) img->height_px = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 8);
      if(v != NULL) img->created = (time_t)strtoll(v, NULL, 10);

      nc++;
    }
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "knowledge",
        "images_for_chunks: %s", res->error);

  db_result_free(res);
  return(nc);
}

// Scan images by subject, scoped to a corpus list, ordered newest
// first. Matches subject column exactly (ILIKE) or caption as a
// substring (ILIKE '%s%'). See knowledge.h for full semantics.

size_t
knowledge_images_by_subject(const char *corpus_list,
    const char *subject, size_t limit, uint32_t max_age_days,
    knowledge_image_t *out, size_t out_cap)
{
  char in_clause[1200];
  char *e_subj;
  char age_clause[64];
  size_t sql_sz;
  db_result_t *res;
  size_t n_names;
  char *sql;
  size_t nc;
  if(!knowledge_ready || subject == NULL || subject[0] == '\0'
      || out == NULL || out_cap == 0)
    return(0);

  if(limit == 0 || limit > out_cap)
    limit = out_cap;

  // Build the corpus IN (...) fragment. Empty list is a legit degenerate
  // case — behave like a sync-empty short-circuit so callers don't need
  // to pre-check.
  n_names = knowledge_build_corpus_in_clause(corpus_list,
      in_clause, sizeof(in_clause));

  if(n_names == 0)
    return(0);

  e_subj = db_escape(subject);
  if(e_subj == NULL)
    return(0);

  // Age filter is interpolated as a raw integer literal — caller is
  // trusted (KV knob) and the column is a TIMESTAMPTZ. Zero disables.
  if(max_age_days > 0)
    snprintf(age_clause, sizeof(age_clause),
        " AND ki.created > NOW() - INTERVAL '%u days'", max_age_days);
  else
    age_clause[0] = '\0';

  sql_sz = strlen(e_subj) * 2 + strlen(in_clause)
      + strlen(age_clause) + 1024;
  sql = mem_alloc("knowledge", "img_subj_sql", sql_sz);

  if(sql == NULL)
  {
    mem_free(e_subj);
    return(0);
  }

  snprintf(sql, sql_sz,
      "SELECT ki.id, ki.chunk_id, ki.url, COALESCE(ki.page_url, ''),"
      " COALESCE(ki.caption, ''), COALESCE(ki.subject, ''),"
      " COALESCE(ki.width_px, 0), COALESCE(ki.height_px, 0),"
      " EXTRACT(EPOCH FROM ki.created)::bigint"
      " FROM knowledge_images ki"
      " JOIN knowledge_chunks kc ON kc.id = ki.chunk_id"
      " WHERE kc.corpus IN %s"
      " AND (ki.subject ILIKE '%s' OR ki.caption ILIKE '%%%s%%')"
      "%s"
      " ORDER BY ki.created DESC LIMIT %zu",
      in_clause, e_subj, e_subj, age_clause, limit);

  mem_free(e_subj);

  res = db_result_alloc();
  nc = 0;

  if((db_query(sql, res) == SUCCESS) && res->ok)
  {
    for(uint32_t i = 0; i < res->rows && nc < out_cap; i++)
    {
      knowledge_image_t *img = &out[nc];
      const char *v;
      memset(img, 0, sizeof(*img));

      v = db_result_get(res, i, 0);
      if(v != NULL) img->id = (int64_t)strtoll(v, NULL, 10);
      v = db_result_get(res, i, 1);
      if(v != NULL) img->chunk_id = (int64_t)strtoll(v, NULL, 10);
      v = db_result_get(res, i, 2);
      if(v != NULL) snprintf(img->url, sizeof(img->url), "%s", v);
      v = db_result_get(res, i, 3);
      if(v != NULL) snprintf(img->page_url, sizeof(img->page_url), "%s", v);
      v = db_result_get(res, i, 4);
      if(v != NULL) snprintf(img->caption, sizeof(img->caption), "%s", v);
      v = db_result_get(res, i, 5);
      if(v != NULL) snprintf(img->subject, sizeof(img->subject), "%s", v);
      v = db_result_get(res, i, 6);
      if(v != NULL) img->width_px = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 7);
      if(v != NULL) img->height_px = (int)strtol(v, NULL, 10);
      v = db_result_get(res, i, 8);
      if(v != NULL) img->created = (time_t)strtoll(v, NULL, 10);

      nc++;
    }
  }

  else if(res->error[0] != '\0')
    clam(CLAM_WARN, "knowledge",
        "images_by_subject: %s", res->error);

  db_result_free(res);
  mem_free(sql);
  return(nc);
}

// Stats

static uint64_t
knowledge_count_rows(const char *table)
{
  char sql[128];
  db_result_t *res;
  uint64_t n;
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);

  res = db_result_alloc();
  n = 0;

  if((db_query(sql, res) == SUCCESS) && res->ok && res->rows == 1)
  {
    const char *v = db_result_get(res, 0, 0);
    if(v != NULL) n = (uint64_t)strtoull(v, NULL, 10);
  }

  db_result_free(res);
  return(n);
}

void
knowledge_get_stats(knowledge_stats_t *out)
{
  if(out == NULL) return;

  memset(out, 0, sizeof(*out));

  if(!knowledge_ready)
    return;

  out->total_corpora = knowledge_count_rows("knowledge_corpora");
  out->total_chunks  = knowledge_count_rows("knowledge_chunks");
  out->total_embeds  = knowledge_count_rows("knowledge_chunk_embeddings");
  out->total_images  = knowledge_count_rows("knowledge_images");
}

uint32_t
knowledge_get_chunk_embedding(int64_t chunk_id, float *out, uint32_t out_cap)
{
  char sql[256];
  db_result_t *res;
  uint32_t     dim_out;
  if(!knowledge_ready || chunk_id <= 0 || out == NULL || out_cap == 0)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT dim, vec FROM knowledge_chunk_embeddings"
      " WHERE chunk_id = %" PRId64, chunk_id);

  res = db_result_alloc();
  dim_out = 0;

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows == 1)
  {
    const char *d_s = db_result_get(res, 0, 0);
    const char *v_s = db_result_get(res, 0, 1);
    uint32_t    dim = (d_s != NULL)
        ? (uint32_t)strtoul(d_s, NULL, 10) : 0;

    if(dim > 0 && dim <= out_cap && v_s != NULL)
    {
      float *vec = knowledge_bytea_to_vec(v_s, dim);
      if(vec != NULL)
      {
        memcpy(out, vec, sizeof(float) * dim);
        mem_free(vec);
        dim_out = dim;
      }
    }
    // dim > out_cap falls through returning 0; caller's semantic
    // gate bypasses cleanly rather than asserting.
  }

  db_result_free(res);
  return(dim_out);
}



// Lifecycle

void
knowledge_init(void)
{
  if(knowledge_ready)
    return;

  pthread_mutex_init(&knowledge_cfg_mutex, NULL);
  pthread_mutex_init(&knowledge_stat_mutex, NULL);

  memset(&knowledge_cfg, 0, sizeof(knowledge_cfg));
  knowledge_cfg.enabled               = true;
  knowledge_cfg.rag_top_k             = KNOWLEDGE_DEF_RAG_TOP_K;
  knowledge_cfg.rag_max_context_chars = KNOWLEDGE_DEF_RAG_MAX_CTX_CHARS;
  knowledge_cfg.chunk_max_chars       = KNOWLEDGE_DEF_CHUNK_MAX_CHARS;
  knowledge_cfg.embed_batch_size      = KNOWLEDGE_DEF_EMBED_BATCH_SIZE;

  knowledge_ready = true;

  clam(CLAM_INFO, "knowledge", "knowledge subsystem initialized");
}

void
knowledge_register_config(void)
{
  knowledge_register_kv();
  knowledge_load_config();
  knowledge_ensure_tables();
}

void
knowledge_exit(void)
{
  if(!knowledge_ready)
    return;

  knowledge_ready = false;

  pthread_mutex_destroy(&knowledge_cfg_mutex);
  pthread_mutex_destroy(&knowledge_stat_mutex);

  clam(CLAM_INFO, "knowledge", "knowledge subsystem shut down");
}
