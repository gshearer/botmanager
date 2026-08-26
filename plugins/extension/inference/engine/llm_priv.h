#ifndef BM_LLM_PRIV_H
#define BM_LLM_PRIV_H

// Internal surface of the llm subsystem. Lives inside the inference
// plugin; not part of the cross-plugin public API (which is
// inference.h's dlsym shims).

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

// ⛔ There is no `queued` here and there never was one that counted: the
// engine has an in-flight set and no queue of its own. A `llm_queued_count`
// was declared, reported out of llm_get_stats and never incremented
// anywhere in the tree; it was deleted 2026-08-26 rather than filled in,
// because the queue that matters belongs to the command surface — `!ask`
// and `!imagine` each own theirs, with their own per-caller quotas — and
// below them core/curl owns the only other one (core.curl.max_active).
typedef struct
{
  uint32_t active;
  uint64_t total_requests;
  uint64_t total_retries;
  uint64_t total_errors;
  uint64_t total_prompt_tokens;
  uint64_t total_completion_tokens;
  uint64_t total_latency_ms;
  time_t   since;                    // when these counters started counting
} llm_stats_t;

// Lifecycle.
void llm_init(void);
void llm_register_config(void);
void llm_register_commands(void);

// PLIFE-5: stop scheduling new work, then cancel and drain what is
// already airborne. Retries and dialect negotiations re-arm themselves
// off the curl callback thread, so the engine must first stop topping
// up the in-flight set; what remains is cancelled at the curl layer and
// waited out here, because every one of those requests holds callback
// pointers into this plugin's mapping and core's quiescence barrier
// cannot see them (SC-LLM-INFLIGHT).
void llm_stop(void);

void llm_exit(void);

// Model introspection.
bool     llm_model_exists(const char *name);
bool     llm_model_kind(const char *name, llm_kind_t *out);
bool     llm_model_max_context(const char *name, uint32_t *out);
uint32_t llm_model_embed_dim(const char *name);

// Also declared (identically) in the public inference.h so external
// consumers can call llm_model_iterate through its dlsym shim; guarded
// so this internal copy and the public one never clash.
#ifndef BM_LLM_MODEL_ITER_CB_T_DEFINED
#define BM_LLM_MODEL_ITER_CB_T_DEFINED
typedef void (*llm_model_iter_cb_t)(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user);
#endif

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

bool llm_image_submit(const char *model_name,
    const llm_image_params_t *params, const char *prompt,
    llm_image_done_cb_t done_cb, void *user_data);

bool llm_stt_submit(const char *model_name, const void *wav, size_t wav_len,
    llm_stt_done_cb_t done_cb, void *user_data);

bool llm_tts_submit(const char *model_name, const llm_tts_params_t *params,
    const char *text, llm_tts_done_cb_t done_cb, void *user_data);

// Cancel every airborne request submitted with `user_data`. A cancelled
// request still delivers its done callback, with ok=false, so a caller
// tearing down waits for a callback it owns rather than for a transfer
// it does not. Safe to call from the requester's own stop().
// returns: how many requests were flagged.
uint32_t llm_cancel_user(const void *user_data);

void llm_get_stats(llm_stats_t *out);

// Per-model counters. Also exposed through inference.h's dlsym shim.
bool llm_model_stats(const char *name, llm_model_stats_t *out);

typedef void (*llm_iter_cb_t)(const char *model_name, llm_kind_t kind,
    bool streaming, uint32_t elapsed_secs, void *data);

void llm_iterate_active(llm_iter_cb_t cb, void *data);

#define LLM_MODEL_NAME_SZ  64
#define LLM_MODEL_ID_SZ    128
#define LLM_ENDPOINT_SZ    512
#define LLM_KV_KEY_SZ      128
#define LLM_FINISH_SZ      32
#define LLM_ERR_SZ         256

// Image (text-to-image) request/response sizing.
#define LLM_IMAGE_SIZE_SZ     16    // "1024x1024" style dimension string
#define LLM_IMAGE_MIME_SZ     32    // e.g. "image/png"
#define LLM_IMAGE_REVISED_SZ  512   // provider-rewritten prompt (optional)

// Request Content-Type, wide enough for a multipart boundary parameter.
// Matches core/curl.c's CURL_CT_SZ, which is where the value ends up.
#define LLM_CONTENT_TYPE_SZ   128

// Speech request sizing. The multipart boundary is randomised per
// request (util_rand) exactly as reachyapi's is: our payload is binary
// audio, and a fixed delimiter is a delimiter an unlucky sample run can
// forge.
#define LLM_BOUNDARY_SZ       40
#define LLM_TTS_VOICE_SZ      64

// Refuse an oversize upload here rather than pay to put it on the wire
// and have the server refuse it. Twenty-five minutes of 16 kHz mono
// S16LE is far past any utterance the ear will ever hand us.
#define LLM_STT_WAV_MAX       (48u * 1024u * 1024u)

// Bounds on the two teardown waits. `stop` cancels the in-flight set
// and waits for the callbacks it just guaranteed; the unmap sweep waits
// only for consumer callbacks already running, which is why it is much
// shorter. Both exist so a hung callback costs a WARN and a refused
// unload rather than a wedged loader thread.
#define LLM_STOP_DRAIN_SECS   10
#define LLM_UNMAP_DRAIN_SECS  2

// Per-model request-dialect negotiation (LLM-DIALECT-1).
#define LLM_DIR_FIELD_SZ    64   // canonical builder field / wire name
#define LLM_MAX_DIRECTIVES  8    // learned directives per model (room to grow)
#define LLM_NEGOTIATION_CAP 4    // runaway backstop; each retry makes progress

// Defaults (applied before KV load).
#define LLM_DEF_MAX_RETRIES       3
#define LLM_DEF_RETRY_BACKOFF_MS  500
#define LLM_DEF_TIMEOUT_SECS      300
#define LLM_DEF_MAX_CONTEXT       8192
#define LLM_DEF_STREAMING_IDLE_MS 30000

// How long a bulk embed submit will sit against a full curl queue.
// Sized to be unreachable while the endpoint answers at all — the
// queue holds 256 and frees a slot the moment any one of them
// completes — and decisive when it has stopped: well inside the 300 s
// each queued request may itself take before its own timeout fires.
#define LLM_DEF_EMBED_SUBMIT_WAIT_MS 60000

#define LLM_RETRY_CAP_MS          30000
#define LLM_ASSEMBLED_INIT_CAP    1024

typedef struct
{
  uint32_t max_retries;
  uint32_t retry_backoff_ms;
  uint32_t timeout_secs;
  uint32_t max_context_tokens;
  uint32_t streaming_idle_ms;
  uint32_t embed_submit_wait_ms;
} llm_cfg_t;

// In-memory mirror of an llm_services row: one OpenAI-compatible provider,
// keyed by name, carrying the base URL that the engine appends
// /chat/completions, /models, /embeddings to. The API token lives in the
// KV slot llm.service.<name>.creds.apikey, derived from the name (no stored
// pointer column).
typedef struct llm_service
{
  char        name[LLM_MODEL_NAME_SZ];
  char        base_url[LLM_ENDPOINT_SZ];
  struct llm_service *next;
} llm_service_t;

// In-memory mirror of an llm_models row. References a service by name; the
// base_url is resolved from that service at reload time and cached here so
// the submit path needs no second lookup.
typedef struct llm_model
{
  char        name[LLM_MODEL_NAME_SZ];
  llm_kind_t  kind;
  char        service_name[LLM_MODEL_NAME_SZ];
  char        base_url[LLM_ENDPOINT_SZ];
  char        model_id[LLM_MODEL_ID_SZ];
  uint32_t    embed_dim;
  uint32_t    max_context;
  float       default_temp;
  bool        enabled;
  struct llm_model *next;
} llm_model_t;

// Mirrors llm_kind_t; separate in case we add more. Grows in lockstep
// with it, and append-only for the same reason.
typedef enum
{
  LLM_REQ_CHAT,
  LLM_REQ_EMBED,
  LLM_REQ_IMAGE,
  LLM_REQ_STT,
  LLM_REQ_TTS
} llm_req_type_t;

// One learned request-dialect directive for a canonical field the chat-body
// builder emits (e.g. "max_tokens", "temperature"): keep it as-is, rename it
// to a different wire name, or drop it entirely. Learned by parsing a
// provider's 400 parameter error, then persisted per (service, model_id) in
// llm_model_params so later calls build the right body on the first try.
typedef enum
{
  LLM_DIR_KEEP,     // emit the canonical field unchanged (default; never stored)
  LLM_DIR_RENAME,   // emit `replacement` as the wire field name
  LLM_DIR_DROP      // omit the field so the provider default applies
} llm_dir_action_t;

typedef struct
{
  char             field[LLM_DIR_FIELD_SZ];       // canonical builder field
  llm_dir_action_t action;
  char             replacement[LLM_DIR_FIELD_SZ]; // wire name when RENAME
  bool             staged;   // request-scoped: learned this request, flush on
                             // success. Always false in the shared cache.
} llm_directive_t;

// Freelist-backed.
struct llm_request
{
  llm_req_type_t        type;

  // Model snapshot (copied at submit time so cache can mutate freely).
  char                  model_name[LLM_MODEL_NAME_SZ];
  char                  service_name[LLM_MODEL_NAME_SZ];  // keys the params table
  char                  endpoint_url[LLM_ENDPOINT_SZ];
  char                  model_id[LLM_MODEL_ID_SZ];
  char                  api_key_kv[LLM_KV_KEY_SZ];
  llm_kind_t            kind;
  uint32_t              embed_dim;

  // Request-dialect directives applied when building the chat-params tail.
  // Seeded from the shared cache at submit; augmented in place when a
  // provider 400 teaches us a new one. Staged entries flush to DB on the
  // eventual success. See LLM-DIALECT-1.
  llm_directive_t       directives[LLM_MAX_DIRECTIVES];
  uint32_t              n_directives;
  uint32_t              negotiation_attempts;  // independent of `attempt`

  // Chat-specific fields.
  llm_chat_params_t     params;
  llm_chat_done_cb_t    chat_done_cb;
  llm_chunk_cb_t        chunk_cb;
  llm_embed_done_cb_t   embed_done_cb;
  llm_image_done_cb_t   image_done_cb;
  llm_stt_done_cb_t     stt_done_cb;
  llm_tts_done_cb_t     tts_done_cb;
  void                 *user_data;

  // Image (text-to-image) request/response state. The decoded-ready b64
  // payload reuses the `assembled` buffer below (see llm_parse_image_
  // response); only the small side-channel fields live here.
  char                  image_size[LLM_IMAGE_SIZE_SZ];        // requested dims
  uint32_t              image_n;                              // v1 fixes at 1
  char                  image_mime[LLM_IMAGE_MIME_SZ];        // response MIME
  char                  image_revised[LLM_IMAGE_REVISED_SZ];  // revised_prompt

  // Request Content-Type. Empty means "application/json", which is what
  // every JSON-bodied kind wants; the speech-to-text path fills it with
  // its multipart type and boundary. Response Content-Type is captured
  // off the completion because the audio kinds are the only ones whose
  // success depends on it.
  char                  content_type[LLM_CONTENT_TYPE_SZ];
  char                  resp_content_type[LLM_CONTENT_TYPE_SZ];

  // Request body (JSON). For chat, req_body = body_prefix + params-tail;
  // the immutable prefix ({"model":...,"messages":[...]}) is retained so a
  // negotiation retry rebuilds only the tail without re-encoding messages.
  // body_prefix is NULL for embed requests (req_body built once).
  char                 *body_prefix;
  size_t                body_prefix_len;
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

  // Backpressure: when non-zero, llm_issue_request uses
  // curl_request_submit_wait and gives it this many milliseconds to
  // find a queue slot, instead of curl_request_submit's fast fail.
  // Set by llm_embed_submit_wait for bulk pipelines that cannot afford
  // silent drops. Carried through the retry path so scheduled retries
  // also wait rather than failing fast on transient saturation. Zero
  // is the ordinary non-blocking submit.
  uint32_t              submit_wait_ms;

  // The curl request carrying the current attempt, for
  // curl_request_cancel. Re-stamped on every retry; 0 while nothing of
  // this request is on the wire.
  uint64_t              curl_id;

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
extern llm_service_t   *llm_services_head;
extern pthread_rwlock_t llm_services_lock;
extern llm_cfg_t        llm_cfg;

// Shared helpers between llm.c and llm_cmd.c.
bool        llm_kind_from_str(const char *s, llm_kind_t *out);
const char *llm_kind_to_str(llm_kind_t k);
void        llm_models_reload(void);
void        llm_services_reload(void);

// Resolve a service's base URL by name into out (rdlocks the service
// list). Returns SUCCESS if the service exists, FAIL otherwise.
bool        llm_service_base_url(const char *name, char *out, size_t out_sz);

// Compose a request URL: one trailing '/' trimmed off base, then "/<op>"
// appended (op ∈ "chat/completions", "models", "embeddings"). Returns
// SUCCESS, or FAIL on a NULL/empty argument or truncation.
bool        llm_build_url(const char *base, const char *op,
                char *out, size_t out_sz);

// Fire an async GET <base_url>/models for one service and cache the
// result in llm_service_models. Defined in llm_cmd.c. Returns SUCCESS if
// the request was submitted. llm_services_refresh_all() seeds every known
// service once (startup).
bool        llm_service_refresh(const char *name);
void        llm_services_refresh_all(void);

// KV change hook for llm.service.<name>.creds.apikey (kv_cb_t). When the key is
// set to a non-empty value, re-fires llm_service_refresh() for that service
// so providers that gate /models behind auth get their model list on the
// next probe. Defined in llm_cmd.c. Invoked outside the KV lock.
void        llm_apikey_kv_cb(const char *key, void *data);

#endif // BM_LLM_PRIV_H
