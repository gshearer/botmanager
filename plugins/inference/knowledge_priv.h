#ifndef BM_KNOWLEDGE_PRIV_H
#define BM_KNOWLEDGE_PRIV_H

// Internal surface of the knowledge subsystem. Lives inside the
// inference plugin; not part of the cross-plugin public API (which is
// plugins/inference/inference.h's dlsym shims).

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
bool knowledge_insert_chunk(const char *corpus, const char *source_url,
    const char *section_heading, const char *text, int64_t *out_id);

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
} knowledge_batch_t;

// Shared helpers between knowledge.c and knowledge_file.c.
void knowledge_cfg_snapshot(knowledge_cfg_t *out);
void knowledge_effective_embed_model(char *out, size_t out_sz);
void knowledge_batch_init(knowledge_batch_t *b, const char *corpus,
    const char *model, uint32_t max_fill);
void knowledge_batch_free(knowledge_batch_t *b);
bool knowledge_batch_add(knowledge_batch_t *b, int64_t chunk_id,
    const char *text);

// File/directory ingest pipeline — owned by knowledge_file.c, driven
// by the /knowledge ingest command in knowledge.c. Returns SUCCESS
// when the walk completes; populates the out-pointer stats.
bool knowledge_ingest_path(const char *corpus, const char *path,
    const char *base_url_or_NULL,
    size_t *out_files, size_t *out_chunks, size_t *out_skipped,
    uint64_t *out_embed_ok, uint64_t *out_embed_fail);

#endif // BM_KNOWLEDGE_PRIV_H
