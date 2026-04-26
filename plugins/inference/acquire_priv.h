#ifndef BM_ACQUIRE_PRIV_H
#define BM_ACQUIRE_PRIV_H

// Internal surface of the acquire subsystem. Lives inside the
// inference plugin; not part of the cross-plugin public API (which is
// plugins/inference/inference.h's dlsym shims).

#define INFERENCE_INTERNAL
#include "inference.h"

#include "common.h"
#include "clam.h"
#include "alloc.h"
#include "kv.h"
#include "knowledge_priv.h"
#include "task.h"

#include <pthread.h>

// -----------------------------------------------------------------------
// Pre-move public API, now plugin-internal.
// -----------------------------------------------------------------------

typedef struct
{
  uint64_t total_ticks;
  uint64_t total_queries;
  uint64_t total_summaries_ok;
  uint64_t total_summaries_fail;
  uint64_t total_ingested;

  // A6R — reactive-path breakdown counters.
  uint64_t total_reactive_enqueued;
  uint64_t total_reactive_dedup_drops;
  uint64_t total_reactive_rate_drops;
  uint64_t total_reactive_drained;
  uint64_t total_reactive_kicks;

  // Personality-declared feed counters.
  uint64_t total_feed_fetches;
  uint64_t total_feed_304;
  uint64_t total_feed_new_items;
  uint64_t total_feed_errors;
} acquire_stats_t;

// Lifecycle.
void acquire_init(void);
void acquire_register_config(void);
void acquire_register_commands(void);
void acquire_exit(void);

// Registry (extern — also exposed via inference.h shims outside the
// plugin).
bool acquire_register_topics(const char *bot_name,
    const acquire_topic_t *topics, size_t n_topics,
    const char *dest_corpus);

void acquire_unregister_bot(const char *bot_name);

acq_enq_result_t acquire_enqueue_reactive(const char *bot_name,
    const char *topic_name, const char *subject);

const char *acquire_enq_result_str(acq_enq_result_t r);

// Digest helper (internal — used by the reactive pipeline and the
// /acquire test command).
typedef struct
{
  bool        ok;
  uint32_t    relevance;    // 0-100, LLM-scored
  const char *summary;      // NUL-terminated; NULL on failure
  const char *error;        // NULL on success
  void       *user_data;
} acquire_digest_response_t;

typedef void (*acquire_digest_cb_t)(
    const acquire_digest_response_t *resp);

bool acquire_digest_submit(const char *topic_name,
    const char *topic_keywords_csv,
    const char *body, size_t body_len,
    acquire_digest_cb_t cb, void *user_data);

void acquire_get_stats(acquire_stats_t *out);

void acquire_register_ingest_cb(acquire_ingest_cb_t cb, void *user);

size_t acquire_get_topic_snapshot(const char *bot_name,
    acquire_topic_t *out, size_t cap);

#define ACQUIRE_CTX                 "acquire"

#define ACQUIRE_DEF_RELEVANCE_THRESHOLD        50
#define ACQUIRE_DEF_MAX_SOURCES_PER_QUERY      3
#define ACQUIRE_DEF_TICK_CADENCE_SECS          600
#define ACQUIRE_MIN_TICK_CADENCE_SECS          5
#define ACQUIRE_DEF_DIGEST_BODY_TRUNCATE_CHARS 6000

// A8 — corpus lifecycle sweep. Single engine-global periodic task that
// prunes each bot's dest_corpus to keep it under the per-bot size cap
// and (optionally) TTL-expires old chunks.
#define ACQUIRE_DEF_SWEEP_INTERVAL_SECS        3600
#define ACQUIRE_MIN_SWEEP_INTERVAL_SECS        60
#define ACQUIRE_DEF_CORPUS_MAX_MB              200
#define ACQUIRE_SWEEP_DELETE_BATCH             100

// A6 defaults — reactive path.
#define ACQUIRE_DEF_MAX_REACTIVE_PER_HOUR      10
#define ACQUIRE_DEF_REACTIVE_DEDUP_LRU_SIZE    256
#define ACQUIRE_DEF_REACTIVE_DEDUP_TTL_SECS    3600

// Ring-buffer cap for pending reactive jobs per bot. Overflow drops
// the oldest — recent events are the point of reactive acquisition.
#define ACQUIRE_REACTIVE_RING_CAP              32

// Reactive drain cap per tick — default and compile-time ceiling for
// the runtime `acquire.max_reactive_per_tick` KV. The default of 1
// keeps a noisy channel from monopolising the engine; higher values
// let operators trade fairness for throughput (e.g. for testing, or
// on channels where reactive jobs pile up faster than one-per-tick
// can drain). Ceiling is the ring-buffer cap — there's no point
// draining more than the ring can hold.
#define ACQUIRE_DEF_MAX_REACTIVE_PER_TICK      1
#define ACQUIRE_MAX_REACTIVE_PER_TICK_CEIL     ACQUIRE_REACTIVE_RING_CAP

// Per-(bot, topic) sliding-window rate-limit counter depth. A fixed
// 32-slot ring of timestamps is ample for any realistic rate: even at
// 10 jobs/hour the window is sparse.
#define ACQUIRE_REACTIVE_RATE_WINDOW_SLOTS     32

// Hard compile-time cap on the reactive dedup LRU. Runtime knob
// `acquire.reactive_subject_lru_size` is clamped to this value.
#define ACQUIRE_REACTIVE_DEDUP_LRU_MAX         1024

// Cap on HTML-stripped body length handed to the digester. The
// digester then truncates further to `acquire.digest_body_truncate_chars`.
#define ACQUIRE_REACTIVE_STRIPPED_MAX          16384

// Upper bound on the heap buffer assembled around a digest prompt.
// Must comfortably hold the system prompt + user-prompt template +
// truncated body + keyword list.
#define ACQUIRE_DIGEST_PROMPT_MAX              8192

// I1 — image-extractor sizing. The extractor emits a hard-capped array
// so the per-source wrapper ctx stays small; the dynamic KV knob
// `acquire.image_max_per_page` is clamped at load time to this value.
#define ACQUIRE_IMAGE_MAX_PER_PAGE_CEIL        32

// Semicolon-separated image-URL skiplist cache. Bounded at parse time;
// substrings are lowercased in place so the hot-path match is a trivial
// case-matched strncasecmp.
#define ACQUIRE_IMAGE_SKIPLIST_MAX             16
#define ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ        64

// Extractor defaults.
#define ACQUIRE_DEF_IMAGES_ENABLED             true
#define ACQUIRE_DEF_IMAGE_MIN_DIM_PX           200
#define ACQUIRE_DEF_IMAGE_MAX_PER_PAGE         6
#define ACQUIRE_DEF_IMAGE_URL_SKIPLIST \
    "icon;sprite;logo;avatar;favicon;pixel;tracking"

// Feed-state sizing. Per-feed dedup + conditional-GET state lives on
// the bot entry (one slot per declared feed, across all topics); the
// tick thread reads it, the curl callback writes it, both under
// acquire_entries_lock.
#define ACQUIRE_FEED_GUID_RING                 256
#define ACQUIRE_FEED_ETAG_SZ                   128
#define ACQUIRE_FEED_LMOD_SZ                   64
#define ACQUIRE_FEED_HASH_SZ                   17  // hex FNV-1a 64-bit + NUL

typedef struct
{
  time_t    last_fetched;
  char      etag          [ACQUIRE_FEED_ETAG_SZ];
  char      last_modified [ACQUIRE_FEED_LMOD_SZ];
  char      body_hash     [ACQUIRE_FEED_HASH_SZ];
  uint64_t  seen_guids    [ACQUIRE_FEED_GUID_RING];
  uint32_t  guid_write;
  uint32_t  guid_fill;    // saturates at ring size
  uint32_t  n_fetches;
  uint32_t  n_304;
  uint32_t  n_new_items;
  uint32_t  n_errors;
} acquire_feed_state_t;

// One pending reactive job. Small (< 512 B) — lives inline inside the
// per-bot ring.
typedef struct
{
  char     topic_name[ACQUIRE_TOPIC_NAME_SZ];
  char     subject   [ACQUIRE_SUBJECT_SZ];
} acquire_reactive_job_t;

// Per-(topic) sliding-window rate counter kept alongside the ring.
// Packed into its own slot array so O(topic_count) lookup stays cheap;
// topic_name maps 1:1 to a slot via linear scan (topic lists are tiny,
// capped at a handful of entries by the personality loader).
typedef struct
{
  char      topic_name[ACQUIRE_TOPIC_NAME_SZ];
  time_t    window[ACQUIRE_REACTIVE_RATE_WINDOW_SLOTS];
  uint32_t  next;                              // write cursor (wraps)
  uint32_t  count;                             // fill (<= slots)
} acquire_rate_slot_t;

// Per-bot registry entry. `active` distinguishes a live registration
// from a tombstone left behind by acquire_unregister_bot. `task` is
// NULL only before the first register call; once spawned it is reused
// across re-registrations.
typedef struct acquire_bot_entry
{
  char                       name       [ACQUIRE_BOT_NAME_SZ];
  char                       dest_corpus[ACQUIRE_CORPUS_NAME_SZ];
  acquire_topic_t           *topics;
  size_t                     n_topics;
  bool                       active;
  task_handle_t              task;
  uint32_t                   tick_count;   // per-bot tick counter

  // A6R — on-demand drain gate. Set under acquire_entries_lock (write)
  // by acq_bot_kick_submit_deferred when a reactive enqueue arrives;
  // cleared by acq_bot_kick_drain at the start of its run. Dedups the
  // per-bot kick so a flurry of chat mentions produces at most one
  // pending drain task in flight.
  bool                       kick_pending;

  // Reactive queue. Producer = chat thread via acquire_enqueue_reactive.
  // Consumer = tick thread. Guarded by ring_mutex — deliberately
  // separate from acquire_entries_lock so the tick thread can drain
  // jobs without holding the registry rwlock across long async hops.
  pthread_mutex_t            ring_mutex;
  acquire_reactive_job_t     ring[ACQUIRE_REACTIVE_RING_CAP];
  uint32_t                   ring_head;         // next read slot
  uint32_t                   ring_tail;         // next write slot
  uint32_t                   ring_count;        // fill (<= cap)

  // Per-topic sliding-window rate counters. Only the tick thread
  // reads/writes these, so no lock needed — chat threads only touch
  // the ring above.
  acquire_rate_slot_t        rate[ACQUIRE_KEYWORDS_MAX * 2];
  size_t                     n_rate;

  // A7: sidecar counter array parallel to `topics`, same length
  // (n_topics). Incremented every time a topic fires a proactive
  // query; the 1/10 upcoming-query cadence fires when
  // counter % 10 == 9 AND the topic has a non-empty upcoming_query.
  // Reset on re-registration (along with the topics array).
  uint32_t                  *topic_proactive_counter;

  // Flattened copy of every feed declared across all topics. The three
  // arrays share a length (`n_feeds_total`); `feed_topic_idx[k]` maps
  // slot k back to the owning topic in `topics[]`. Layout mirrors the
  // A6 rate-slot table — a single linear sweep suffices on every tick.
  // Reset on re-registration.
  acquire_feed_t            *feeds;
  acquire_feed_state_t      *feed_state;
  size_t                    *feed_topic_idx;
  size_t                     n_feeds_total;
  size_t                     feed_next_idx;   // round-robin tick cursor

  struct acquire_bot_entry  *next;
} acquire_bot_entry_t;

// Cached subsystem configuration (refreshed on KV change).
typedef struct
{
  bool      enabled;
  uint32_t  relevance_threshold;      // 0-100
  uint32_t  max_sources_per_query;
  uint32_t  digest_body_truncate_chars;

  // A6 — reactive path knobs.
  uint32_t  max_reactive_per_hour;    // per-topic, per-bot
  uint32_t  reactive_dedup_lru_size;  // clamped to _MAX
  uint32_t  reactive_dedup_ttl_secs;

  // A8 — corpus lifecycle sweep cadence (seconds). Clamped at load
  // time to at least ACQUIRE_MIN_SWEEP_INTERVAL_SECS so a typo in KV
  // can't DOS the database.
  uint32_t  sweep_interval_secs;

  // Per-bot tick cadence (seconds). Picked up by the tick callback on
  // its next fire, so a runtime KV change takes effect within at most
  // one current-cadence window. Clamped to ACQUIRE_MIN_TICK_CADENCE_SECS.
  uint32_t  tick_cadence_secs;

  // Reactive-drain cap per tick. Clamped to
  // ACQUIRE_MAX_REACTIVE_PER_TICK_CEIL; zero is promoted to the default.
  uint32_t  max_reactive_per_tick;

  // I1 — image extractor knobs. Master-switch first; remaining knobs
  // are read-only after snapshot so the hot-path scan takes a single
  // mutex lock + copy.
  bool      images_enabled;
  uint32_t  image_min_dim_px;
  uint32_t  image_max_per_page;
  bool      image_require_dims;

  // Parsed form of acquire.image_url_skiplist: semicolon-separated
  // substrings, trimmed + lowercased at parse time. `n` is the number
  // of valid entries; unused slots left zeroed.
  char      image_skiplist[ACQUIRE_IMAGE_SKIPLIST_MAX]
                          [ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ];
  size_t    image_skiplist_n;

  // Master switch for the personality-declared feeds dispatcher. When
  // false, acq_feeds_tick is a no-op; SXNG + reactive paths untouched.
  bool      sources_enabled;
} acquire_cfg_t;

// One extracted image slot. Populated inside the scanner; later copied
// wholesale into knowledge_insert_image on the digest callback.
typedef struct
{
  char url     [KNOWLEDGE_IMAGE_URL_SZ];
  char caption [KNOWLEDGE_IMAGE_CAPTION_SZ];
  int  width_px;
  int  height_px;
} acq_image_extract_t;

// Module state shared across acquire.c and its siblings. Defined in
// acquire.c.
extern pthread_mutex_t      acquire_cfg_mutex;
extern acquire_cfg_t        acquire_cfg;
extern bool                 acquire_ready;
extern acquire_bot_entry_t *acquire_entries;
extern pthread_rwlock_t     acquire_entries_lock;
extern pthread_mutex_t      acquire_stat_mutex;
extern acquire_stats_t      acquire_stats;
extern pthread_mutex_t      acquire_ingest_cb_mutex;
extern acquire_ingest_cb_t  acquire_ingest_cb;
extern void                *acquire_ingest_user;

// Shared helpers (defined in acq_html.c).
size_t acq_strip_html(const char *in, size_t in_len,
    char *out, size_t out_cap);

void acq_parse_skiplist(const char *raw,
    char out[][ACQUIRE_IMAGE_SKIPLIST_ENTRY_SZ], size_t *n_out);

size_t acq_extract_images(const char *html, size_t html_len,
    const char *page_url, acq_image_extract_t *out, size_t cap);

// Promoted from acquire_html.c's static for re-use in acquire_feed.c.
// Copies <title>…</title> contents into out (NUL-terminated). Returns
// true on hit.
bool acq_find_title(const char *html, size_t html_len,
    char *out, size_t out_cap);

// Registry helper: caller must hold acquire_entries_lock (rd or wr).
// Returns NULL if no entry with `name` exists.
acquire_bot_entry_t *acquire_entry_find_locked(const char *name);

// Reactive plumbing helpers (defined in acquire.c; called by both
// acquire.c's public enqueue path and acquire_reactive.c's drain path).
const acquire_topic_t *acquire_find_topic(const acquire_bot_entry_t *e,
    const char *topic_name);

bool acquire_rate_exceeded(acquire_bot_entry_t *e, const char *topic_name,
    uint32_t max_per_hour);

bool acquire_ring_pop(acquire_bot_entry_t *e, acquire_reactive_job_t *out);

// Reactive lifecycle hooks (defined in acquire_reactive.c; called from
// acquire.c's public API and from task scheduling).
void acquire_bot_tick(task_t *t);
void acq_bot_kick_submit_deferred(acquire_bot_entry_t *e);

// Result of a single proactive-fire attempt. `OK` means a job was
// dispatched through the async chain; all other values are declined
// before any work hit the wire. Exposed for the A9 admin surface —
// `/acquire trigger` reports the decline reason back to the operator.
typedef enum
{
  ACQ_PROACTIVE_OK,
  ACQ_PROACTIVE_RATE_LIMITED,
  ACQ_PROACTIVE_NO_QUERY,       // template-only topic; no concrete query
  ACQ_PROACTIVE_NO_TOPIC,       // bot has no firable topic (weight==0)
} acq_proactive_result_t;

// Fire one proactive query for an already-resolved topic. Caller holds
// acquire_entries_lock (rdlock); releases it unconditionally before
// returning.
acq_proactive_result_t acq_proactive_fire_locked(acquire_bot_entry_t *e,
    size_t topic_idx, char *out_query, size_t out_query_sz);

// Per-tick feed dispatcher. Called from acquire_bot_tick after the
// reactive + proactive paths have had their chance. Picks at most one
// due feed per tick per bot; async thereafter.
void acq_feeds_tick(acquire_bot_entry_t *e);

// Shared ingest seam extracted from the reactive path. Both the
// reactive SXNG → fetch → digest chain and the feed dispatcher fan
// into this helper after relevance + dest_corpus gates pass. The
// caller owns `resp` + `images` for the duration of the call.
//
// Defined in acquire_reactive.c; feed-path callers pass
// `images = NULL, n_images = 0` (no image extraction for feed items
// in the first cut).
//
// Returns SUCCESS if knowledge_insert_chunk landed a row (the reactive
// caller uses this to bump its per-ctx ingest count); FAIL on chunk-
// insert failure. Does NOT report image-insert failures — those are
// logged at DEBUG and never abort the ingest.
bool acq_ingest_digest_result(const char *bot_name,
    const char *topic_name, const char *subject,
    const char *dest_corpus, bool is_proactive,
    const acquire_digest_response_t *resp,
    const acq_image_extract_t *images, size_t n_images,
    const char *page_url);

#endif // BM_ACQUIRE_PRIV_H
