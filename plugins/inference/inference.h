#ifndef BM_INFERENCE_H
#define BM_INFERENCE_H

// Public cross-plugin surface of the inference plugin (llm + knowledge
// + acquire). Consumers include this header; the `static inline`
// helpers below resolve the real implementation through
// `plugin_dlsym("inference", …)` on first use and cache the pointer.
//
// Shim shape mirrors `plugins/inference/acquire_reactive.c`'s
// acq_sxng_resolve() pattern: atomic-guarded static cache, union to
// launder void*↔function-pointer conversion, FATAL + abort on lookup
// miss (which implies a broken plugin-dependency graph).
//
// Types below are the public ABI — consumers name them when declaring
// callbacks and buffers. Internals live alongside the implementation
// in {llm,knowledge,acquire}_priv.h inside the inference plugin and
// are not visible through this header.

#include "clam.h"
#include "plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  // abort
#include <time.h>

// -----------------------------------------------------------------------
// LLM types
// -----------------------------------------------------------------------

// Message roles for chat completion.
typedef enum
{
  LLM_ROLE_SYSTEM,
  LLM_ROLE_USER,
  LLM_ROLE_ASSISTANT
} llm_role_t;

// Model kinds. A model is registered as chat OR embed, never both.
typedef enum
{
  LLM_KIND_CHAT,
  LLM_KIND_EMBED
} llm_kind_t;

// Opaque request handle.
typedef struct llm_request llm_request_t;

// Content-block kinds for multimodal messages. When a message uses
// blocks (see llm_message_t.blocks), each block is one of these.
typedef enum
{
  LLM_CONTENT_TEXT,
  LLM_CONTENT_IMAGE_BASE64,
} llm_content_kind_t;

// Single content block inside a multimodal message.
// - TEXT:           text is the UTF-8 content.
// - IMAGE_BASE64:   image_mime is the MIME type (e.g. "image/png");
//                   image_b64 is the RFC 4648 base64 payload
//                   (NUL-terminated, standard alphabet, padded).
//
// Lifetime matches llm_message_t: the callee copies all strings
// during llm_chat_submit(); caller may free after return.
typedef struct
{
  llm_content_kind_t  kind;
  const char         *text;        // kind == LLM_CONTENT_TEXT
  const char         *image_mime;  // kind == LLM_CONTENT_IMAGE_BASE64
  const char         *image_b64;   // kind == LLM_CONTENT_IMAGE_BASE64
} llm_content_block_t;

// Single chat message. Strings are caller-owned and must remain valid
// until llm_chat_submit() returns -- the call copies all data
// internally. For multimodal content (text + image), set blocks /
// n_blocks and leave content NULL; the text fast-path and the blocks
// path are mutually exclusive.
//
// NOTE: stack-declared arrays of llm_message_t MUST be zero-
// initialized (memset or {0}) before use. The blocks pointer below
// would otherwise read stack garbage.
typedef struct
{
  llm_role_t                  role;
  const char                 *content;      // UTF-8; ignored when blocks != NULL
  const llm_content_block_t  *blocks;       // NULL = text fast-path
  size_t                      n_blocks;
} llm_message_t;

// Request-level parameters. All zero fields mean "use model/config default".
typedef struct
{
  float    temperature;    // 0 = model default
  uint32_t max_tokens;     // 0 = model default (no upper bound sent)
  uint32_t timeout_secs;   // 0 = KV default (llm.timeout_secs)
  bool     stream;         // true -> chunk_cb is called per content delta
} llm_chat_params_t;

// Response delivered to the chat completion callback. Valid for the
// duration of the callback only -- caller must copy any needed data.
typedef struct
{
  llm_request_t *request;
  bool           ok;                   // true on HTTP 2xx with parseable body
  long           http_status;
  const char    *model;                // registered name
  const char    *content;              // full assembled text (NUL-terminated)
  size_t         content_len;
  uint32_t       prompt_tokens;
  uint32_t       completion_tokens;
  const char    *finish_reason;        // "stop" | "length" | "" | ...
  const char    *error;                // NULL on success
  void          *user_data;
} llm_chat_response_t;

// Per-delta callback for streaming chat. delta is the new tokens only
// (no accumulation). Invoked on the curl worker thread; must be fast.
typedef void (*llm_chunk_cb_t)(llm_request_t *req,
    const char *delta, size_t delta_len, void *user);

// Completion callback for a chat request.
typedef void (*llm_chat_done_cb_t)(const llm_chat_response_t *resp);

// Embedding response.
typedef struct
{
  llm_request_t *request;
  bool           ok;
  long           http_status;
  const char    *model;
  uint32_t       dim;                  // vector dimension
  const float  **vectors;              // vectors[i] has length dim
  size_t         n_vectors;
  const char    *error;
  void          *user_data;
} llm_embed_response_t;

typedef void (*llm_embed_done_cb_t)(const llm_embed_response_t *resp);

// -----------------------------------------------------------------------
// Knowledge types
// -----------------------------------------------------------------------

#define KNOWLEDGE_CORPUS_NAME_SZ   64
#define KNOWLEDGE_CORPUS_DESC_SZ   200
#define KNOWLEDGE_SOURCE_URL_SZ    512
#define KNOWLEDGE_SECTION_SZ       256
#define KNOWLEDGE_CHUNK_TEXT_SZ    4096

#define KNOWLEDGE_IMAGE_URL_SZ      1024
#define KNOWLEDGE_IMAGE_CAPTION_SZ   256
#define KNOWLEDGE_IMAGE_SUBJECT_SZ   128

typedef struct
{
  int64_t  id;
  char     corpus[KNOWLEDGE_CORPUS_NAME_SZ];
  char     source_url[KNOWLEDGE_SOURCE_URL_SZ];
  char     section_heading[KNOWLEDGE_SECTION_SZ];
  char     text[KNOWLEDGE_CHUNK_TEXT_SZ];
  float    score;
  time_t   created;
} knowledge_chunk_t;

typedef struct
{
  int64_t  id;
  int64_t  chunk_id;
  char     url     [KNOWLEDGE_IMAGE_URL_SZ];
  char     page_url[KNOWLEDGE_IMAGE_URL_SZ];
  char     caption [KNOWLEDGE_IMAGE_CAPTION_SZ];
  char     subject [KNOWLEDGE_IMAGE_SUBJECT_SZ];
  int      width_px;
  int      height_px;
  time_t   created;
} knowledge_image_t;

typedef void (*knowledge_retrieve_cb_t)(const knowledge_chunk_t *chunks,
    size_t n, void *user);

// -----------------------------------------------------------------------
// Acquire types
// -----------------------------------------------------------------------

#define ACQUIRE_TOPIC_NAME_SZ      64
#define ACQUIRE_TOPIC_QUERY_SZ     512
#define ACQUIRE_TOPIC_CATEGORY_SZ  16   // SearXNG category wire name
#define ACQUIRE_KEYWORD_SZ         64
#define ACQUIRE_KEYWORDS_MAX       16
#define ACQUIRE_BOT_NAME_SZ        64
#define ACQUIRE_CORPUS_NAME_SZ     64
#define ACQUIRE_DIGEST_SUMMARY_SZ  1200
#define ACQUIRE_SUBJECT_SZ         128

// Personality-declared feeds attached per topic (new in the sources
// work). Users write `"sources": [...]` in the personality JSON; the
// parser maps that key onto the `feeds` field below so internal code
// can refer to feeds/sources unambiguously (the pre-existing
// `acq_source_ctx_t` names an SXNG search-result wrapper).
#define ACQUIRE_FEED_URL_SZ        1024
#define ACQUIRE_FEEDS_MAX          8
#define ACQUIRE_FEED_CADENCE_MIN   300

typedef enum
{
  ACQUIRE_FEED_RSS,        // RSS 2.0 or Atom — auto-detected at parse
  ACQUIRE_FEED_HTML,
} acquire_feed_kind_t;

typedef struct
{
  acquire_feed_kind_t kind;
  char                url[ACQUIRE_FEED_URL_SZ];
  uint32_t            cadence_secs;    // clamped to ACQUIRE_FEED_CADENCE_MIN
} acquire_feed_t;

typedef enum
{
  ACQUIRE_MODE_ACTIVE,
  ACQUIRE_MODE_REACTIVE,
  ACQUIRE_MODE_MIXED,
} acquire_mode_t;

typedef struct
{
  char            name[ACQUIRE_TOPIC_NAME_SZ];
  acquire_mode_t  mode;
  uint32_t        proactive_weight;
  uint32_t        cadence_secs;

  // Per-topic override for acquire.max_sources_per_query. 0 means
  // "inherit global"; nonzero values are clamped to the global cap at
  // dispatch time, so a personality can ask for fewer but not more.
  uint32_t        max_sources;

  char            keywords[ACQUIRE_KEYWORDS_MAX][ACQUIRE_KEYWORD_SZ];
  size_t          n_keywords;

  char            query         [ACQUIRE_TOPIC_QUERY_SZ];
  char            query_template[ACQUIRE_TOPIC_QUERY_SZ];
  char            upcoming_query[ACQUIRE_TOPIC_QUERY_SZ];

  // SearXNG category wire name ("general"/"images"/"news"/"videos"/
  // "music"); empty means general. Stored as a string so this header
  // does not have to pull in the searxng_api.h types — the acquire
  // engine resolves the enum via sxng_category_from_name() at submit
  // time, and unknown values fall back to general.
  char            category[ACQUIRE_TOPIC_CATEGORY_SZ];

  // Personality-declared feeds. Each slot carries a kind + URL +
  // cadence. Rebuilt on personality reload like the rest of this
  // struct; runtime state (last-fetched, ETag, dedup ring) lives
  // separately on acquire_bot_entry_t.
  acquire_feed_t  feeds[ACQUIRE_FEEDS_MAX];
  size_t          n_feeds;
} acquire_topic_t;

typedef enum
{
  ACQ_ENQ_ACCEPTED,
  ACQ_ENQ_DEDUP,
  ACQ_ENQ_TOPIC_UNKNOWN,
  ACQ_ENQ_BOT_UNKNOWN,
  ACQ_ENQ_NOT_READY,
} acq_enq_result_t;

typedef enum
{
  ACQUIRE_INGEST_REACTIVE,
  ACQUIRE_INGEST_PROACTIVE
} acquire_ingest_mode_t;

typedef void (*acquire_ingest_cb_t)(
    const char *bot_name, const char *topic_name,
    const char *subject, const char *corpus,
    int64_t chunk_id, uint32_t relevance,
    acquire_ingest_mode_t mode, void *user);

// -----------------------------------------------------------------------
// dlsym shim helpers
// -----------------------------------------------------------------------
//
// Reference: plugins/inference/acquire_reactive.c (acq_sxng_resolve).
// Each shim caches the resolved function pointer in a static atomic-
// guarded slot so subsequent calls take one relaxed-acquire load. On a
// cold cache the loader uses plugin_dlsym; a NULL return means the
// inference plugin was not loaded (or the symbol was renamed) — a
// programming error, fatal.
//
// Inside the inference plugin itself the shims are unnecessary (and
// would collide with the real definitions), so translation units
// within the plugin define INFERENCE_INTERNAL before including their
// *_priv.h headers to skip the inline block below.

#ifndef INFERENCE_INTERNAL

// -------- LLM --------

static inline bool
llm_chat_submit(const char *model_name,
    const llm_chat_params_t *params,
    const llm_message_t *messages, size_t n_messages,
    llm_chat_done_cb_t done_cb,
    llm_chunk_cb_t chunk_cb_or_NULL,
    void *user_data)
{
  typedef bool (*fn_t)(const char *, const llm_chat_params_t *,
      const llm_message_t *, size_t, llm_chat_done_cb_t,
      llm_chunk_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "llm_chat_submit");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_chat_submit");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(model_name, params, messages, n_messages,
      done_cb, chunk_cb_or_NULL, user_data));
}

static inline bool
llm_embed_submit(const char *model_name,
    const char *const *inputs, size_t n_inputs,
    llm_embed_done_cb_t done_cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, const char *const *, size_t,
      llm_embed_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "llm_embed_submit");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_embed_submit");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(model_name, inputs, n_inputs, done_cb, user_data));
}

// -------- Knowledge --------

static inline bool
knowledge_corpus_upsert(const char *name, const char *description)
{
  typedef bool (*fn_t)(const char *, const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "knowledge_corpus_upsert");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: knowledge_corpus_upsert");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, description));
}

static inline bool
knowledge_retrieve(const char *corpus_list, const char *query,
    uint32_t top_k, knowledge_retrieve_cb_t cb, void *user)
{
  typedef bool (*fn_t)(const char *, const char *, uint32_t,
      knowledge_retrieve_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "knowledge_retrieve");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: knowledge_retrieve");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(corpus_list, query, top_k, cb, user));
}

static inline float
knowledge_cosine(const float *a, const float *b, uint32_t dim)
{
  typedef float (*fn_t)(const float *, const float *, uint32_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "knowledge_cosine");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: knowledge_cosine");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(a, b, dim));
}

static inline uint32_t
knowledge_get_chunk_embedding(int64_t chunk_id, float *out, uint32_t out_cap)
{
  typedef uint32_t (*fn_t)(int64_t, float *, uint32_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "knowledge_get_chunk_embedding");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: knowledge_get_chunk_embedding");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(chunk_id, out, out_cap));
}

static inline size_t
knowledge_images_for_chunks(const int64_t *chunk_ids, size_t n_ids,
    knowledge_image_t *out, size_t out_cap)
{
  typedef size_t (*fn_t)(const int64_t *, size_t, knowledge_image_t *, size_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "knowledge_images_for_chunks");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: knowledge_images_for_chunks");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(chunk_ids, n_ids, out, out_cap));
}

static inline size_t
knowledge_images_by_subject(const char *corpus_list, const char *subject,
    size_t limit, uint32_t max_age_days,
    knowledge_image_t *out, size_t out_cap)
{
  typedef size_t (*fn_t)(const char *, const char *, size_t, uint32_t,
      knowledge_image_t *, size_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "knowledge_images_by_subject");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: knowledge_images_by_subject");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(corpus_list, subject, limit, max_age_days, out, out_cap));
}

// -------- Acquire --------

static inline bool
acquire_register_topics(const char *bot_name,
    const acquire_topic_t *topics, size_t n_topics,
    const char *dest_corpus)
{
  typedef bool (*fn_t)(const char *, const acquire_topic_t *, size_t,
      const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "acquire_register_topics");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: acquire_register_topics");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(bot_name, topics, n_topics, dest_corpus));
}

static inline void
acquire_unregister_bot(const char *bot_name)
{
  typedef void (*fn_t)(const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "acquire_unregister_bot");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: acquire_unregister_bot");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(bot_name);
}

static inline acq_enq_result_t
acquire_enqueue_reactive(const char *bot_name, const char *topic_name,
    const char *subject)
{
  typedef acq_enq_result_t (*fn_t)(const char *, const char *, const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "acquire_enqueue_reactive");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: acquire_enqueue_reactive");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(bot_name, topic_name, subject));
}

static inline void
acquire_register_ingest_cb(acquire_ingest_cb_t cb, void *user)
{
  typedef void (*fn_t)(acquire_ingest_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "acquire_register_ingest_cb");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: acquire_register_ingest_cb");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(cb, user);
}

static inline size_t
acquire_get_topic_snapshot(const char *bot_name,
    acquire_topic_t *out, size_t cap)
{
  typedef size_t (*fn_t)(const char *, acquire_topic_t *, size_t);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym("inference", "acquire_get_topic_snapshot");
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: acquire_get_topic_snapshot");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(bot_name, out, cap));
}

#endif // INFERENCE_INTERNAL

#endif // BM_INFERENCE_H
