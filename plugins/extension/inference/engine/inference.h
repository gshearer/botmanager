#ifndef BM_INFERENCE_H
#define BM_INFERENCE_H

// Public cross-plugin surface of the inference plugin (llm + knowledge
// + acquire). Consumers include this header; the `static inline`
// helpers below resolve the real implementation through
// `plugin_dlsym_cached("inference", …, (void **)&cached)` on first use and cache the pointer.
//
// Shim shape mirrors `acquire_reactive.c`'s
// acq_sxng_resolve() pattern: atomic-guarded static cache, union to
// launder void*↔function-pointer conversion, FATAL + abort on lookup
// miss (which implies a broken plugin-dependency graph).
//
// Types below are the public ABI — consumers name them when declaring
// callbacks and buffers. Internals live alongside the implementation
// in {llm,knowledge,acquire}_priv.h inside the inference plugin and
// are not visible through this header.

#include "common.h"  // SUCCESS / FAIL (llm_effort_from_str)
#include "clam.h"
#include "plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   // abort
#include <string.h>   // strcmp, strlen  (llm_effort_from_str)
#include <strings.h>  // strncasecmp     (llm_effort_set_admits)
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

// Model kinds. A model is registered as exactly one of these — never
// more than one.
//
// APPEND ONLY. The ordinal is the public ABI and it is what a sibling
// .so compiled against an older copy of this header still believes;
// inserting a kind renumbers every one after it. New kinds go at the
// end, and the mirror enum llm_req_type_t (llm_priv.h) grows in
// lockstep.
typedef enum
{
  LLM_KIND_CHAT,
  LLM_KIND_EMBED,
  LLM_KIND_IMAGE,
  LLM_KIND_STT,     // speech in, transcript out
  LLM_KIND_TTS      // text in, speech out
} llm_kind_t;

// Reasoning effort for a chat request — the OpenAI-compat `reasoning_effort`
// field. UNSET is the neutral value at 0, so a zeroed llm_chat_params_t still
// means "let the service's configured value decide", which is what every
// caller that has no opinion wants. NONE is NOT UNSET: it sends the string
// "none", and on a thinking-only model that is a different request from
// sending nothing (LLM.md §Thinking / reasoning models).
//
// ⛔ These are TAGS, not a scale. There is no order here and there cannot be
// one: measured 2026-08-23, RadixArk/Qwen3.8-27B-NVFP4 accepts `xhigh` and
// REJECTS `high`, `minimal` exists only on Google and `xhigh` only on vLLM.
// Never compare two of these with < or >, and never clamp against them — a
// bound on effort is a membership test. LLM.md carries the measured table.
//
// APPEND ONLY, for the reason llm_kind_t is: the ordinal is what a sibling
// .so compiled against an older copy of this header still believes.
typedef enum
{
  LLM_EFFORT_UNSET = 0,   // omit the field; the service KV decides
  LLM_EFFORT_NONE,        // send "none" — not the same as UNSET
  LLM_EFFORT_MINIMAL,
  LLM_EFFORT_LOW,
  LLM_EFFORT_MEDIUM,
  LLM_EFFORT_HIGH,
  LLM_EFFORT_XHIGH
} llm_effort_t;

// Wire spelling, or "" for UNSET. Both directions live here rather than
// behind a dlsym shim: it is a seven-entry table with no state, and a shim
// around one would be a thin wrapper.
static inline const char *
llm_effort_wire(llm_effort_t e)
{
  static const char *const names[] = {
    "", "none", "minimal", "low", "medium", "high", "xhigh"
  };

  return((size_t)e < sizeof names / sizeof names[0] ? names[e] : "");
}

// Membership of a DECLARED effort set — the comma- or space-separated wire
// spellings in llm.model.<name>.efforts.
//
// ⚠ An EMPTY set means UNDECLARED, so it admits everything. Nobody has
// measured that model yet; it does not mean the model refuses every value.
// That is the KV's declared default and a read site may not substitute
// something else for it (include/kv.h). Three callers want this — the ask
// gate, the !show renderers and the engine — so it lives here rather than
// three times.
static inline bool
llm_effort_set_admits(const char *csv, llm_effort_t e)
{
  const char *wire = llm_effort_wire(e);
  const char *p;
  size_t      n;

  if(csv == NULL || csv[0] == '\0')
    return(true);

  n = strlen(wire);

  if(n == 0)
    return(true);                 // UNSET is always admissible

  for(p = csv; *p != '\0'; p++)
  {
    if(strncasecmp(p, wire, n) != 0)
      continue;

    // Whole token only: a set naming "high" must not admit "xhigh", and
    // one naming "xhigh" must not be matched by "high" at an offset.
    if((p == csv || p[-1] == ',' || p[-1] == ' ' || p[-1] == '\t')
        && (p[n] == '\0' || p[n] == ',' || p[n] == ' ' || p[n] == '\t'))
      return(true);
  }

  return(false);
}

// Parse a wire spelling. SUCCESS/FAIL + out-param, matching
// llm_kind_from_str: an unrecognised string is a distinct outcome from
// UNSET and the caller has to be able to refuse it. NULL/"" is UNSET and
// succeeds.
static inline bool
llm_effort_from_str(const char *s, llm_effort_t *out)
{
  if(out == NULL)
    return(FAIL);

  if(s == NULL || s[0] == '\0')
  {
    *out = LLM_EFFORT_UNSET;
    return(SUCCESS);
  }

  for(int e = LLM_EFFORT_NONE; e <= LLM_EFFORT_XHIGH; e++)
    if(strcmp(s, llm_effort_wire((llm_effort_t)e)) == 0)
    {
      *out = (llm_effort_t)e;
      return(SUCCESS);
    }

  return(FAIL);
}

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
  float        temperature;    // 0 = model default
  uint32_t     max_tokens;     // 0 = model default (no upper bound sent)
  uint32_t     timeout_secs;   // 0 = KV default (llm.timeout_secs)
  bool         stream;         // true -> chunk_cb is called per content delta
  llm_effort_t effort;         // UNSET = the service's reasoning_effort decides
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

// Request-level parameters for a text-to-image generation. All zero
// fields mean "use model/config default".
typedef struct
{
  const char *size;          // e.g. "1024x1024"; NULL/"" -> provider default
  uint32_t    n;             // images to request; v1 fixes this at 1
  uint32_t    timeout_secs;  // 0 = KV default (llm.timeout_secs)
} llm_image_params_t;

// Response delivered to the image generation callback. Valid for the
// duration of the callback only -- the caller must copy any needed data
// (the b64 payload in particular is released when the callback returns).
typedef struct
{
  llm_request_t *request;
  bool           ok;              // true on HTTP 2xx with a decodable payload
  long           http_status;
  const char    *model;           // registered name
  const char    *b64;             // base64 image bytes (NUL-terminated)
  size_t         b64_len;
  const char    *mime;            // "image/png" unless the provider says otherwise
  const char    *revised_prompt;  // provider-rewritten prompt, or "" if none
  const char    *error;           // NULL on success
  void          *user_data;
} llm_image_response_t;

typedef void (*llm_image_done_cb_t)(const llm_image_response_t *resp);

// Speech-to-text. The audio is handed over as a complete WAV container
// (whisper.cpp's native form is 16 kHz mono S16LE, which costs it no
// conversion) and copied into the request body before llm_stt_submit()
// returns, so the caller keeps ownership of its buffer throughout.
typedef struct
{
  llm_request_t *request;
  bool           ok;
  long           http_status;
  const char    *model;        // registered name
  const char    *text;         // transcript, whitespace-collapsed, trimmed
  size_t         text_len;
  const char    *error;        // NULL on success
  void          *user_data;
} llm_stt_response_t;

typedef void (*llm_stt_done_cb_t)(const llm_stt_response_t *resp);

// Text-to-speech request parameters. Every zero/NULL field means "let
// the provider decide".
typedef struct
{
  const char *voice;         // provider voice id, e.g. "af_heart"
  double      speed;         // synthesis rate multiplier; 0 = default
  uint32_t    timeout_secs;  // 0 = KV default (llm.timeout_secs)
} llm_tts_params_t;

// Text-to-speech response. `bytes` is the audio container exactly as the
// provider sent it — no transcoding, no resampling — and like every
// other response here it is valid for the duration of the callback only.
typedef struct
{
  llm_request_t *request;
  bool           ok;
  long           http_status;
  const char    *model;         // registered name
  const uint8_t *bytes;         // audio payload (WAV from kokorod)
  size_t         bytes_len;
  const char    *content_type;  // as reported by the provider, or ""
  const char    *error;         // NULL on success
  void          *user_data;
} llm_tts_response_t;

typedef void (*llm_tts_done_cb_t)(const llm_tts_response_t *resp);

// Per-model visitor for llm_model_iterate(). Called once per registered
// model with its full descriptor. Guarded because the same typedef also
// appears in the plugin-internal llm_priv.h, which includes this header
// first — both definitions are identical, but the guard makes the
// include order irrelevant.
#ifndef BM_LLM_MODEL_ITER_CB_T_DEFINED
#define BM_LLM_MODEL_ITER_CB_T_DEFINED
typedef void (*llm_model_iter_cb_t)(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user);
#endif

// What one model's traffic has actually looked like, from llm_model_stats().
//
// ⛔ These do NOT belong on llm_model_iter_cb_t above: it is a public
// function-pointer type, and appending a parameter breaks every consumer
// silently at the ABI. A by-name lookup is the whole reason this is a
// second call.
//
// The mean is ok_latency_ms / (requests - errors) — a failed request's
// elapsed time is a timeout, not a speed. Guard the divide: requests ==
// errors is a model that has only ever failed.
//
// `since` is when the engine started counting, which is when the plugin
// loaded. Print it: a reload zeroes the table, and a caller that reports
// the numbers without the window invites them to be read as history.
typedef struct
{
  uint64_t requests;
  uint64_t errors;
  uint64_t ok_latency_ms;
  time_t   since;
} llm_model_stats_t;

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
// Reference: acquire_reactive.c (acq_sxng_resolve).
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

    u.obj = plugin_dlsym_cached("inference", "llm_chat_submit", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "llm_embed_submit", (void **)&cached);
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

static inline bool
llm_image_submit(const char *model_name,
    const llm_image_params_t *params, const char *prompt,
    llm_image_done_cb_t done_cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, const llm_image_params_t *,
      const char *, llm_image_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_image_submit", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_image_submit");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(model_name, params, prompt, done_cb, user_data));
}

// Transcribe `wav` (wav_len bytes of a complete audio container) with the
// registered `stt` model `model_name`.
//
// SUCCESS ⇒ the request is queued and done_cb fires exactly once, on the
//           curl worker thread.
// FAIL    ⇒ done_cb was NOT invoked and never will be; the caller still
//           owns whatever it passed as user_data.
// (Remember SUCCESS == false and FAIL == true — compare with == SUCCESS.)
// The audio is copied into the request body before this returns, so the
// caller may free its buffer immediately either way.
static inline bool
llm_stt_submit(const char *model_name, const void *wav, size_t wav_len,
    llm_stt_done_cb_t done_cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, const void *, size_t,
      llm_stt_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_stt_submit", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_stt_submit");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(model_name, wav, wav_len, done_cb, user_data));
}

// Synthesize `text` with the registered `tts` model `model_name`.
// `params` may be NULL for provider defaults throughout. Same async
// failure contract as llm_stt_submit above; `text` is copied into the
// request body before this returns.
static inline bool
llm_tts_submit(const char *model_name, const llm_tts_params_t *params,
    const char *text, llm_tts_done_cb_t done_cb, void *user_data)
{
  typedef bool (*fn_t)(const char *, const llm_tts_params_t *, const char *,
      llm_tts_done_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_tts_submit", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_tts_submit");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(model_name, params, text, done_cb, user_data));
}

static inline bool
llm_model_exists(const char *name)
{
  typedef bool (*fn_t)(const char *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_model_exists", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_model_exists");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name));
}

static inline bool
llm_model_kind(const char *name, llm_kind_t *out)
{
  typedef bool (*fn_t)(const char *, llm_kind_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_model_kind", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_model_kind");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, out));
}

// SUCCESS with *out filled if this model has been seen since *out.since;
// FAIL if it has not, which is NOT the same as zero requests — a registered
// model that has never run has no row. `out->since` is filled either way.
static inline bool
llm_model_stats(const char *name, llm_model_stats_t *out)
{
  typedef bool (*fn_t)(const char *, llm_model_stats_t *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_model_stats", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_model_stats");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, out));
}

static inline void
llm_model_iterate(llm_model_iter_cb_t cb, void *user)
{
  typedef void (*fn_t)(llm_model_iter_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_model_iterate", (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, "inference", "dlsym failed: llm_model_iterate");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  fn(cb, user);
}

// The one shim that tolerates a missing provider instead of aborting.
// Its whole purpose is teardown — a requester cancelling what it has
// airborne from its own stop() — and on that path "inference is not
// loaded" is the ordinary case rather than a programming error, with
// nothing left to cancel when it holds. Nothing is latched on the miss:
// plugin_dlsym_cached only fills the slot on a resolve.
static inline uint32_t
llm_cancel_user(const void *user_data)
{
  typedef uint32_t (*fn_t)(const void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached("inference", "llm_cancel_user", (void **)&cached);
    if(u.obj == NULL)
      return(0);

    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(user_data));
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

    u.obj = plugin_dlsym_cached("inference", "knowledge_corpus_upsert", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "knowledge_retrieve", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "knowledge_cosine", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "knowledge_get_chunk_embedding", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "knowledge_images_for_chunks", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "knowledge_images_by_subject", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "acquire_register_topics", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "acquire_unregister_bot", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "acquire_enqueue_reactive", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "acquire_register_ingest_cb", (void **)&cached);
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

    u.obj = plugin_dlsym_cached("inference", "acquire_get_topic_snapshot", (void **)&cached);
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
