#ifndef BM_KNOWLEDGE_PRIV_H
#define BM_KNOWLEDGE_PRIV_H

// Internal surface of the knowledge subsystem. Lives inside the
// inference plugin; not part of the cross-plugin public API (which is
// inference.h's dlsym shims).

#define INFERENCE_INTERNAL
#include "inference.h"

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"

#include <pthread.h>

// -----------------------------------------------------------------------
// Pre-move public API, now plugin-internal (chat plugin doesn't touch
// these — only knowledge.c / knowledge_cmd.c / knowledge_file.c and
// the acquire sibling modules call them).
// -----------------------------------------------------------------------

typedef struct
{
  uint64_t  total_corpora;     // rows in knowledge_corpora
  uint64_t  total_chunks;      // rows in knowledge_chunks
  uint64_t  total_embeds;      // rows in knowledge_chunk_embeddings
  uint64_t  total_images;      // rows in knowledge_images
} knowledge_stats_t;

// Lifecycle.
void knowledge_init(void);
void knowledge_register_config(void);
void knowledge_register_commands(void);
void knowledge_exit(void);

// Corpus CRUD.
bool knowledge_corpus_upsert(const char *name, const char *description);
bool knowledge_corpus_delete(const char *name);

typedef void (*knowledge_corpus_iter_cb_t)(const char *name,
    const char *description, int64_t chunk_count,
    time_t last_ingested, void *user);

uint32_t knowledge_corpus_iterate(knowledge_corpus_iter_cb_t cb,
    void *user);

// Chunk / image ingest.

// One chunk insert's outcome. Four, because the caller owes each a
// different thing: embed it / embed it and say the corpus was resumed /
// skip it / report a failure.
//
// INSERTED is 0 == SUCCESS (common.h), so a two-state `!= SUCCESS` test
// comes out incomplete rather than wrong: it separates "a new row
// landed" from everything else and loses only which of the other three
// obligations applies. PRESENT and UNEMBEDDED are NOT failures — the
// row is in the table either way; only UNEMBEDDED still owes a vector.
typedef enum
{
  KNOWLEDGE_CHUNK_INSERTED = 0,   // new row; embed it
  KNOWLEDGE_CHUNK_UNEMBEDDED,     // row was already there, vector-less; embed it
  KNOWLEDGE_CHUNK_PRESENT,        // row was already there and complete; skip it
  KNOWLEDGE_CHUNK_FAILED          // nothing landed; *out_id untouched
} knowledge_chunk_rc_t;

// The chunk insert, as one printf template, so the statement
// tests/test_knowledge_dedup.c replays is the statement production
// sends. Arguments in order: corpus, source_url, section_heading, text
// — every one already db_escape'd by the caller, and every one
// interpolated EXACTLY ONCE. That is what the `cand` CTE buys: the
// obvious form spells the text a second time in the md5 comparison,
// which doubles the sizing and lets the two copies drift.
//
// The row it returns is (id, inserted, embedded):
//   inserted=1              a new row landed          → embed it
//   inserted=0, embedded=0  the row was already there → embed it (resume)
//   inserted=0, embedded=1  already there and complete → skip
//
// The conflict target must be spelled exactly as the index in
// knowledge_ensure_tables() is, or Postgres refuses to infer it.
#define KNOWLEDGE_INSERT_CHUNK_SQL \
    "WITH cand AS (SELECT '%s'::text AS corpus, '%s'::text AS source_url," \
    "                     '%s'::text AS section_heading, '%s'::text AS text)," \
    "     ins AS (" \
    "       INSERT INTO knowledge_chunks" \
    "         (corpus, source_url, section_heading, text)" \
    "       SELECT corpus, source_url, section_heading, text FROM cand" \
    "       ON CONFLICT (corpus, source_url, section_heading, md5(text))" \
    "         DO NOTHING" \
    "       RETURNING id)" \
    " SELECT id, 1 AS inserted, 0 AS embedded FROM ins" \
    " UNION ALL" \
    " SELECT k.id, 0, (SELECT COUNT(*) FROM knowledge_chunk_embeddings e" \
    "                   WHERE e.chunk_id = k.id)::int" \
    "   FROM knowledge_chunks k, cand c" \
    "  WHERE k.corpus = c.corpus AND k.source_url = c.source_url" \
    "    AND k.section_heading = c.section_heading" \
    "    AND md5(k.text) = md5(c.text)" \
    "    AND NOT EXISTS (SELECT 1 FROM ins)" \
    "  LIMIT 1"

knowledge_chunk_rc_t knowledge_insert_chunk_raw(const char *corpus,
    const char *source_url, const char *section_heading, const char *text,
    int64_t *out_id);

knowledge_chunk_rc_t knowledge_insert_chunk(const char *corpus,
    const char *source_url, const char *section_heading, const char *text,
    int64_t *out_id);

void knowledge_corpus_mark_page_chunked(const char *corpus);
bool knowledge_corpus_is_page_chunked(const char *corpus);

uint32_t knowledge_page_supersede(const char *corpus,
    const char *source_url, const char *section_heading, int64_t keep_id);

bool knowledge_insert_image(int64_t chunk_id, const char *url,
    const char *page_url, const char *caption, const char *subject,
    int width_px, int height_px);

// Retrieval (extern — also exposed via inference.h shim outside the
// plugin).
bool knowledge_retrieve(const char *corpus_list, const char *query,
    uint32_t top_k, knowledge_retrieve_cb_t cb, void *user);

size_t knowledge_images_for_chunks(const int64_t *chunk_ids, size_t n_ids,
    knowledge_image_t *out, size_t out_cap);

size_t knowledge_images_by_subject(const char *corpus_list,
    const char *subject, size_t limit, uint32_t max_age_days,
    knowledge_image_t *out, size_t out_cap);

float knowledge_cosine(const float *a, const float *b, uint32_t dim);

uint32_t knowledge_get_chunk_embedding(int64_t chunk_id, float *out,
    uint32_t out_cap);

void knowledge_get_stats(knowledge_stats_t *out);

// URL-fetch knobs for /knowledge ingest. Read live off KV at fetch
// time rather than cached in knowledge_cfg_t — curl copies both, and
// neither is on a hot path.
#define KW_KV_FETCH_UA       "knowledge.fetch_user_agent"
#define KW_KV_FETCH_TIMEOUT  "knowledge.fetch_timeout_secs"

#define KNOWLEDGE_DEF_RAG_TOP_K          5
#define KNOWLEDGE_DEF_RAG_MAX_CTX_CHARS  3072
#define KNOWLEDGE_DEF_CHUNK_MAX_CHARS    1200
#define KNOWLEDGE_DEF_EMBED_BATCH_SIZE   32
#define KNOWLEDGE_EMBED_MODEL_SZ         64

// Compile-time cap on the embed batch fill. Chunks accumulate up to
// knowledge.embed_batch_size (KV, runtime cap) but can never exceed
// this many per submit — the batch structure holds fixed-size arrays.
// Typical embed endpoints accept far more, but 128 is a reasonable
// ceiling that keeps the batch struct under a page of memory.
#define KNOWLEDGE_EMBED_BATCH_MAX        128

// Cached configuration values (refreshed from KV on change).
typedef struct
{
  bool     enabled;
  uint32_t rag_top_k;
  uint32_t rag_max_context_chars;
  uint32_t chunk_max_chars;
  uint32_t embed_batch_size;
  char     embed_model[KNOWLEDGE_EMBED_MODEL_SZ];
} knowledge_cfg_t;

// Embed-submit accumulator shared between knowledge.c (corpus-level
// inserts) and knowledge_file.c (directory/file ingest pipeline). Held
// by value on the caller's stack; owns `texts` until flush.
typedef struct
{
  char      corpus[KNOWLEDGE_CORPUS_NAME_SZ];
  char      model[KNOWLEDGE_EMBED_MODEL_SZ];
  uint32_t  max_fill;                             // submit threshold

  int64_t   chunk_ids[KNOWLEDGE_EMBED_BATCH_MAX];
  char     *texts[KNOWLEDGE_EMBED_BATCH_MAX];     // mem_strdup'd
  size_t    n;

  // Stats (cumulative across this batch's lifetime).
  uint64_t  chunks_submitted;
  uint64_t  chunks_embedded_ok;                   // inferred from flush result
  uint64_t  chunks_embedded_fail;

  // Set by the first flush that could not hand its request to curl —
  // the queue never freed a slot within the bound, or the engine is
  // going away. Both mean every later flush would pay the same wait,
  // so the batch stops accepting chunks and the walk above it stops
  // emitting them: a chunk row inserted after this point could only
  // ever be a row with no embedding, invisible to retrieval — and
  // since OBS-16 it would also BLOCK the re-run that repairs it, by
  // being the row the dedup key matches. The re-run does repair it
  // (it re-embeds a vector-less row), which is exactly why the walk
  // must still stop rather than fill the corpus with more of them.
  bool      aborted;
} knowledge_batch_t;

// Shared helpers between knowledge.c and knowledge_file.c.
void knowledge_cfg_snapshot(knowledge_cfg_t *out);
void knowledge_effective_embed_model(char *out, size_t out_sz);
void knowledge_batch_init(knowledge_batch_t *b, const char *corpus,
    const char *model, uint32_t max_fill);
void knowledge_batch_free(knowledge_batch_t *b);
bool knowledge_batch_add(knowledge_batch_t *b, int64_t chunk_id,
    const char *text);

// What one walk did. `aborted` is the one field a caller must report
// rather than total: the counts below it are then a prefix of the
// corpus, not the corpus.
typedef struct
{
  size_t   files;
  size_t   chunks;
  size_t   skipped;
  size_t   duplicates;   // rows already present AND already embedded
  size_t   reembedded;   // rows already present with no vector — resumed
  uint64_t embed_ok;
  uint64_t embed_fail;
  bool     aborted;
} knowledge_ingest_stats_t;

// File/directory ingest pipeline — owned by knowledge_file.c, driven
// by the /knowledge ingest command in knowledge.c. Returns SUCCESS
// when the path was walkable at all (FAIL is a stat/open refusal, not
// an ingest that stopped early); fills `out` either way.
bool knowledge_ingest_path(const char *corpus, const char *path,
    const char *base_url_or_NULL, knowledge_ingest_stats_t *out);

// In-memory twin of knowledge_ingest_path, for a body that never was a
// file. FAIL on a NULL or empty body; fills `out` either way.
bool knowledge_ingest_text(const char *corpus, const char *source_url,
    const char *section_heading, const char *body, size_t len,
    knowledge_ingest_stats_t *out);

// URL ingest — owned by knowledge_fetch.c, driven by the /knowledge
// ingest command when its path argument carries an http(s) scheme.
// Replies to `ctx` asynchronously; the corpus upsert is its own.
struct cmd_ctx;
#ifndef BM_CMD_CTX_T_DEFINED
#define BM_CMD_CTX_T_DEFINED
typedef struct cmd_ctx cmd_ctx_t;
#endif

void knowledge_fetch_start(const cmd_ctx_t *ctx, const char *corpus,
    const char *url);

#endif // BM_KNOWLEDGE_PRIV_H
