#ifndef BM_LLM_PRIV_H
#define BM_LLM_PRIV_H

// Internal surface of the llm subsystem. Lives inside the inference
// plugin; not part of the cross-plugin public API (which is
// plugins/inference/inference.h's dlsym shims).

#define INFERENCE_INTERNAL
#include "inference.h"

#include "common.h"
#include "clam.h"
#include "curl.h"
#include "kv.h"
#include "alloc.h"
#include "sse.h"

#include <pthread.h>
#include <time.h>

// -----------------------------------------------------------------------
// Pre-move public API, now plugin-internal (chat plugin doesn't touch
// these; only llm.c / llm_cmd.c and the inference.c lifecycle glue do).
// -----------------------------------------------------------------------

typedef struct
{
  uint32_t active;
  uint32_t queued;
  uint64_t total_requests;
  uint64_t total_retries;
  uint64_t total_errors;
  uint64_t total_prompt_tokens;
  uint64_t total_completion_tokens;
  uint64_t total_latency_ms;
} llm_stats_t;

// Lifecycle.
void llm_init(void);
void llm_register_config(void);
void llm_register_commands(void);
void llm_exit(void);

// Model introspection.
bool     llm_model_exists(const char *name);
bool     llm_model_kind(const char *name, llm_kind_t *out);
uint32_t llm_model_embed_dim(const char *name);

typedef void (*llm_model_iter_cb_t)(const char *name, llm_kind_t kind,
    const char *endpoint_url, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user);

void llm_model_iterate(llm_model_iter_cb_t cb, void *user);

// Submit extern prototypes (also exposed through inference.h's dlsym
// shims — but the shims are only compiled outside the inference plugin
// when INFERENCE_INTERNAL is NOT defined).
bool llm_chat_submit(const char *model_name,
    const llm_chat_params_t *params,
    const llm_message_t *messages, size_t n_messages,
    llm_chat_done_cb_t done_cb,
    llm_chunk_cb_t chunk_cb_or_NULL,
    void *user_data);

bool llm_embed_submit(const char *model_name,
    const char *const *inputs, size_t n_inputs,
    llm_embed_done_cb_t done_cb, void *user_data);

bool llm_embed_submit_wait(const char *model_name,
    const char *const *inputs, size_t n_inputs,
    llm_embed_done_cb_t done_cb, void *user_data);

void llm_get_stats(llm_stats_t *out);

typedef void (*llm_iter_cb_t)(const char *model_name, llm_kind_t kind,
    bool streaming, uint32_t elapsed_secs, void *data);

void llm_iterate_active(llm_iter_cb_t cb, void *data);

#define LLM_MODEL_NAME_SZ  64
#define LLM_MODEL_ID_SZ    128
#define LLM_ENDPOINT_SZ    512
#define LLM_KV_KEY_SZ      128
#define LLM_FINISH_SZ      32
#define LLM_ERR_SZ         256

// Defaults (applied before KV load).
#define LLM_DEF_MAX_RETRIES       3
#define LLM_DEF_RETRY_BACKOFF_MS  500
#define LLM_DEF_TIMEOUT_SECS      300
#define LLM_DEF_MAX_CONTEXT       8192
#define LLM_DEF_STREAMING_IDLE_MS 30000
#define LLM_RETRY_CAP_MS          30000
#define LLM_ASSEMBLED_INIT_CAP    1024

typedef struct
{
  uint32_t max_retries;
  uint32_t retry_backoff_ms;
  uint32_t timeout_secs;
  uint32_t max_context_tokens;
  uint32_t streaming_idle_ms;
} llm_cfg_t;

// In-memory mirror of an llm_models row.
typedef struct llm_model
{
  char        name[LLM_MODEL_NAME_SZ];
  llm_kind_t  kind;
  char        endpoint_url[LLM_ENDPOINT_SZ];
  char        model_id[LLM_MODEL_ID_SZ];
  char        api_key_kv[LLM_KV_KEY_SZ];
  uint32_t    embed_dim;
  uint32_t    max_context;
  float       default_temp;
  bool        enabled;
  struct llm_model *next;
} llm_model_t;

// Mirrors llm_kind_t; separate in case we add more.
typedef enum
{
  LLM_REQ_CHAT,
  LLM_REQ_EMBED
} llm_req_type_t;

// Freelist-backed.
struct llm_request
{
  llm_req_type_t        type;

  // Model snapshot (copied at submit time so cache can mutate freely).
  char                  model_name[LLM_MODEL_NAME_SZ];
  char                  endpoint_url[LLM_ENDPOINT_SZ];
  char                  model_id[LLM_MODEL_ID_SZ];
  char                  api_key_kv[LLM_KV_KEY_SZ];
  llm_kind_t            kind;
  uint32_t              embed_dim;

  // Chat-specific fields.
  llm_chat_params_t     params;
  llm_chat_done_cb_t    chat_done_cb;
  llm_chunk_cb_t        chunk_cb;
  llm_embed_done_cb_t   embed_done_cb;
  void                 *user_data;

  // Request body (JSON), built once and reused across retries.
  char                 *req_body;
  size_t                req_body_len;

  // Streaming state.
  bool                  streaming;
  sse_parser_t         *sse_parser;
  size_t                bytes_seen;     // bytes delivered to chunk_cb

  // Assembled content buffer (grown as deltas arrive or populated from
  // non-streaming response).
  char                 *assembled;
  size_t                assembled_len;
  size_t                assembled_cap;

  // Usage counters (streaming usually reports only completion_tokens
  // in a final frame; non-streaming reports both in the single body).
  uint32_t              prompt_tokens;
  uint32_t              completion_tokens;
  char                  finish_reason[LLM_FINISH_SZ];

  // Embedding output (populated by response parser; vectors[i] points
  // into vec_block).
  float                *vec_block;
  size_t                vec_block_len;
  const float         **vectors;
  size_t                n_vectors;
  uint32_t              vectors_dim;

  // Retry state.
  uint32_t              attempt;

  // Backpressure: when true, llm_issue_request uses curl_request_submit_wait
  // (blocks on a full queue) instead of curl_request_submit (fails fast).
  // Set by llm_embed_submit_wait for bulk pipelines that cannot afford
  // silent drops. Carried through the retry path so scheduled retries
  // also block rather than failing fast on transient saturation.
  bool                  blocking_submit;

  // Timing.
  struct timespec       started;
  bool                  in_flight;

  // Terminal state.
  long                  http_status;
  char                  errbuf[LLM_ERR_SZ];

  // In-flight list linkage (protected by llm_active_mutex).
  struct llm_request   *next_active;

  // Freelist linkage.
  struct llm_request   *next_free;
};

// Shared state between llm.c and llm_cmd.c. Defined in llm.c.
extern llm_model_t     *llm_models_head;
extern pthread_rwlock_t llm_models_lock;
extern llm_cfg_t        llm_cfg;

// Shared helpers between llm.c and llm_cmd.c.
bool        llm_kind_from_str(const char *s, llm_kind_t *out);
const char *llm_kind_to_str(llm_kind_t k);
void        llm_models_reload(void);

#endif // BM_LLM_PRIV_H
