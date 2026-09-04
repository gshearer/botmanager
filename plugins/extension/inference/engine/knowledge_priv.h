#ifndef BM_KNOWLEDGE_PRIV_H
#define BM_KNOWLEDGE_PRIV_H

// Internal surface of the knowledge subsystem. Lives inside the
// inference plugin; not part of the cross-plugin public API (which is
// inference.h's dlsym shims).

#define INFERENCE_INTERNAL
#include "inference.h"

#include "common.h"
#include "clam.h"
#include "curl.h"
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
#define KNOWLEDGE_DEF_RAG_LEXICAL_K      3
#define KNOWLEDGE_DEF_RAG_MAX_CTX_CHARS  6144
#define KNOWLEDGE_DEF_CHUNK_MAX_CHARS    1200

// Hits one retrieve may deliver, across both channels. It is also the
// ceiling knowledge_images_for_chunks and knowledge_build_in_list are
// sized against, so the two channels share it rather than each taking
// 64 of their own.
#define KNOWLEDGE_HITS_MAX               64

// Lexical candidates pulled back before the cosine tiebreak picks the
// lexical slots. Wider than the slots themselves because the idf sum
// ties readily -- four chunks matching one rare term all score it
// exactly, and which of them is worth a slot is a question only the
// dense score answers.
#define KNOWLEDGE_LEX_CAND_MAX           32

// A query lexeme this short is an addressee, an interjection or an
// abbreviation far more often than it is a subject -- "hh:" opening an
// IRC line is the shape -- and it carries a rare-token idf that buries
// the term the human actually asked about. Measured on the archwiki
// corpus: with 1- and 2-character lexemes admitted, "hh: what do you
// know about pacoloco" ranks a screenshot page above the Pacoloco one.
#define KNOWLEDGE_LEX_MIN_LEXEME         3

// A lexeme carried by more than this share of the corpus discriminates
// nothing -- it is the corpus's own subject matter -- and it is also
// the one that costs the most to aggregate. 5% keeps "pacman" and
// "arch" out of an Arch wiki ranking while leaving room for a term
// like "proxy"; at 0.5% the ceiling starts cutting real subjects.
#define KNOWLEDGE_LEX_MAX_DF_FRACTION    0.05

// ---- Retrieval ranking (knowledge_rank.c) -----------------------------
// Pure arithmetic over the two retrieval channels. No database, no
// allocator, no lock — which is what lets tests/test_knowledge_rank.c
// compile the unit on its own.

typedef struct
{
  int64_t  id;
  float    score;
} knowledge_hit_t;

// A lexical candidate. `idf` is what the SQL ranked it by — the summed
// inverse document frequency of the query lexemes it carries — and
// `cos` is filled in by the cosine walk that runs afterwards, purely to
// break the ties `idf` leaves. Four chunks that each carry one rare
// term score it identically, and only the embedder has an opinion about
// which of them is about the question.
typedef struct
{
  int64_t  id;
  float    idf;
  float    cos;
} knowledge_lex_t;

// One corpus a bot is bound to, and how much of its score survives.
// The binding string is `name[:weight]`, `;`-separated, so a corpus is
// ingested ONCE and shared: two bots with opposite priorities bind the
// same 33k rows and only their multipliers differ.
typedef struct
{
  char   name[KNOWLEDGE_CORPUS_NAME_SZ];
  float  weight;
} knowledge_corpus_weight_t;

// Names one binding string may carry. Also bounds the clause built from
// it: 16 * (64 quoted + 3 punctuation) is ~1k, which is what the
// callers' `in_clause` buffers are sized for.
#define KNOWLEDGE_CORPUS_LIST_MAX        16

// Weight a binding with no `:weight` carries, and the range a written
// one is clamped to. 0.0 is legal and means bound but never retrieved —
// a corpus you are ingesting into and do not yet trust.
#define KNOWLEDGE_CORPUS_WEIGHT_DEF      1.0f
#define KNOWLEDGE_CORPUS_WEIGHT_MAX      4.0f

// One retrieval's working set. Lives on the retrieving thread's stack
// for the length of a single knowledge_retrieve; the cosine walk reads
// `lex` while it fills `dense`, which is why the two share a frame.
typedef struct
{
  const float      *qvec;
  uint32_t          dim;

  knowledge_hit_t   dense[KNOWLEDGE_HITS_MAX];
  size_t            dense_cap;
  size_t            n_dense;

  knowledge_lex_t   lex[KNOWLEDGE_LEX_CAND_MAX];
  size_t            n_lex;
} knowledge_rank_t;

void knowledge_topk_insert(knowledge_hit_t *buf, size_t cap, size_t *n,
    int64_t id, float score);

void knowledge_lex_sort(knowledge_lex_t *lex, size_t n);

// Appends unless `out` already carries that id. ⛔ BOTH channels must
// push through this, not just the lexical one: the two rank the same
// corpora, so a duplicate is ordinary, and a merged list longer than
// the set of distinct rows behind it overflows the array
// knowledge_deliver_hits sizes from it.
void knowledge_hit_push(knowledge_hit_t *out, size_t *n, size_t cap,
    int64_t id, float score);

// Returns the number of hits written to `out`, in delivery order.
size_t knowledge_interleave(const knowledge_rank_t *r, size_t lex_k,
    knowledge_hit_t *out, size_t out_cap);

// Split a `bot.<n>.corpus` binding into names and weights. Returns how
// many entries were written; a name longer than the field, or an entry
// that is only whitespace, is dropped the way an unknown corpus already
// is. The bytes come off a KV value, so every reading here is a policy
// decision and tests/test_knowledge_corpus.c holds them.
size_t knowledge_parse_corpus_list(const char *list,
    knowledge_corpus_weight_t *out, size_t out_cap);

// Drop the entries that can never contribute a row. Returns the new
// count. ⛔ Retrieval must call this BEFORE building its IN clause:
// weighting a zero-weight corpus's rows to 0.0 would still let them
// fill a top-K that nothing else reached, and they would still count
// toward the lexical channel's document frequencies.
size_t knowledge_corpus_weight_compact(knowledge_corpus_weight_t *w,
    size_t n);

// KNOWLEDGE_CORPUS_WEIGHT_DEF for a name the list did not carry, which
// is what an empty binding gives every row.
float knowledge_corpus_weight_of(const knowledge_corpus_weight_t *w,
    size_t n, const char *name);


// Range knowledge.chunk_max_chars is clamped to at load. The floor
// keeps every splitter's advance strictly positive against
// KNOWLEDGE_CHUNK_OVERLAP; the ceiling is the chunk carrier itself.
// A chunk reaches the DB, and comes back out of it, through
// knowledge_chunk_t's fixed `text` field, so a split wider than that
// field holds is not a bigger chunk — kw_emit_chunk cuts it to fit and
// the overlap cannot reach back far enough to cover what went, which
// is a hole in the corpus rather than a shorter chunk.
#define KNOWLEDGE_MIN_CHUNK_MAX_CHARS    256
#define KNOWLEDGE_MAX_CHUNK_MAX_CHARS    (KNOWLEDGE_CHUNK_TEXT_SZ - 1)
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
  uint32_t rag_lexical_k;
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

// Put the header set a browser sends on a top-level navigation onto
// `req`. Every page fetch in this plugin owes them: a browser
// User-Agent -- which is what `core.curl.user_agent` holds -- arriving
// with no Fetch Metadata beside it contradicts itself, and a
// fingerprinting WAF scores the contradiction rather than the UA. It
// refuses with an HTTP/2 RST_STREAM sent before any status line, so the
// block reaches us as a *transport* error and reads as a flaky network.
// Reasoning and the measurement: leaf `AGENTS.md §Key Points`.
void knowledge_fetch_nav_headers(curl_request_t *req);

#endif // BM_KNOWLEDGE_PRIV_H
