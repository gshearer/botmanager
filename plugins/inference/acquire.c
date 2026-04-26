// botmanager — MIT
// Acquisition engine: per-bot topic registry + reactive/proactive pipeline.

#include "acquire_priv.h"
#include "knowledge_priv.h"
#include "llm_priv.h"

#include "cmd.h"
#include "curl.h"
#include "db.h"
#include "json.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"
#include "util.h"

#include <ctype.h>
#include <inttypes.h>
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Module state

bool                         acquire_ready = false;
acquire_cfg_t                acquire_cfg;
pthread_mutex_t              acquire_cfg_mutex;

acquire_bot_entry_t         *acquire_entries = NULL;
pthread_rwlock_t             acquire_entries_lock;

// Lifetime stats (bumped under acquire_stat_mutex).
pthread_mutex_t              acquire_stat_mutex;
acquire_stats_t              acquire_stats;

// V1 — single post-ingest callback slot. Guarded by its own mutex so
// register/unregister never races with the fire path. The fire site
// snapshots (cb, user) under the lock, drops it, then invokes — so a
// long-running consumer can't block subsequent acquire work.
pthread_mutex_t              acquire_ingest_cb_mutex;
acquire_ingest_cb_t          acquire_ingest_cb   = NULL;
void                        *acquire_ingest_user = NULL;

// Reactive dedup LRU. One flat array guarded by a dedicated mutex.
// Linear scan over 256 entries is trivial at the call frequencies
// this tree sees (a few chat lines per second on a busy channel);
// a hash table would be premature.
typedef struct
{
  bool     in_use;
  char     bot    [ACQUIRE_BOT_NAME_SZ];
  char     topic  [ACQUIRE_TOPIC_NAME_SZ];
  char     subject[ACQUIRE_SUBJECT_SZ];
  time_t   ts;
} acquire_dedup_slot_t;

static pthread_mutex_t       acquire_dedup_mutex;
static acquire_dedup_slot_t  acquire_dedup[ACQUIRE_REACTIVE_DEDUP_LRU_MAX];
static uint32_t              acquire_dedup_cap  = 0;    // effective size
static uint32_t              acquire_dedup_next = 0;    // LRU write cursor

// A8 — engine-global corpus-lifecycle sweep task handle. Spawned in
// acquire_register_config once the KV is loaded. Cleared to
// TASK_HANDLE_NONE on acquire_exit after cancelling; a no-op check on
// acquire_ready in the callback covers any in-flight tick that
// started before cancel.
static task_handle_t         acquire_sweep_task = TASK_HANDLE_NONE;

// Forward declarations

static void acquire_entry_free_topics(acquire_bot_entry_t *e);
void acquire_sweep_tick(task_t *t);

// Config snapshot / KV wiring

static void
acquire_load_config(void)
{
  acquire_cfg_t c;

  const char *raw_skip;
  c.enabled                    = kv_get_uint("acquire.enabled") != 0;
  c.relevance_threshold        = (uint32_t)kv_get_uint("acquire.relevance_threshold");
  c.max_sources_per_query      = (uint32_t)kv_get_uint("acquire.max_sources_per_query");
  c.digest_body_truncate_chars = (uint32_t)kv_get_uint("acquire.digest_body_truncate_chars");
  c.max_reactive_per_hour      = (uint32_t)kv_get_uint("acquire.max_reactive_per_hour");
  c.reactive_dedup_lru_size    = (uint32_t)kv_get_uint("acquire.reactive_subject_lru_size");
  c.reactive_dedup_ttl_secs    = (uint32_t)kv_get_uint("acquire.reactive_dedup_ttl_secs");
  c.sweep_interval_secs        = (uint32_t)kv_get_uint("acquire.sweep_interval_secs");
  c.tick_cadence_secs          = (uint32_t)kv_get_uint("acquire.tick_cadence_secs");
  c.max_reactive_per_tick      = (uint32_t)kv_get_uint("acquire.max_reactive_per_tick");

  // I1 — image extractor knobs.
  c.images_enabled      = kv_get_uint("acquire.images_enabled") != 0;
  c.image_min_dim_px    = (uint32_t)kv_get_uint("acquire.image_min_dim_px");
  c.image_max_per_page  = (uint32_t)kv_get_uint("acquire.image_max_per_page");
  c.image_require_dims  = kv_get_uint("acquire.image_require_dims") != 0;

  // Personality-declared feeds master switch.
  c.sources_enabled     = kv_get_uint("acquire.sources_enabled") != 0;

  memset(c.image_skiplist, 0, sizeof(c.image_skiplist));
  raw_skip = kv_get_str("acquire.image_url_skiplist");
  acq_parse_skiplist(raw_skip, c.image_skiplist, &c.image_skiplist_n);

  if(c.image_min_dim_px == 0)
    c.image_min_dim_px = ACQUIRE_DEF_IMAGE_MIN_DIM_PX;
  if(c.image_max_per_page == 0)
    c.image_max_per_page = ACQUIRE_DEF_IMAGE_MAX_PER_PAGE;
  if(c.image_max_per_page > ACQUIRE_IMAGE_MAX_PER_PAGE_CEIL)
    c.image_max_per_page = ACQUIRE_IMAGE_MAX_PER_PAGE_CEIL;

  if(c.relevance_threshold == 0 || c.relevance_threshold > 100)
    c.relevance_threshold = ACQUIRE_DEF_RELEVANCE_THRESHOLD;
  if(c.max_sources_per_query == 0)
    c.max_sources_per_query = ACQUIRE_DEF_MAX_SOURCES_PER_QUERY;
  if(c.digest_body_truncate_chars == 0)
    c.digest_body_truncate_chars = ACQUIRE_DEF_DIGEST_BODY_TRUNCATE_CHARS;
  if(c.max_reactive_per_hour == 0)
    c.max_reactive_per_hour = ACQUIRE_DEF_MAX_REACTIVE_PER_HOUR;
  if(c.reactive_dedup_lru_size == 0)
    c.reactive_dedup_lru_size = ACQUIRE_DEF_REACTIVE_DEDUP_LRU_SIZE;
  if(c.reactive_dedup_lru_size > ACQUIRE_REACTIVE_DEDUP_LRU_MAX)
    c.reactive_dedup_lru_size = ACQUIRE_REACTIVE_DEDUP_LRU_MAX;
  if(c.reactive_dedup_ttl_secs == 0)
    c.reactive_dedup_ttl_secs = ACQUIRE_DEF_REACTIVE_DEDUP_TTL_SECS;
  if(c.sweep_interval_secs == 0)
    c.sweep_interval_secs = ACQUIRE_DEF_SWEEP_INTERVAL_SECS;
  if(c.sweep_interval_secs < ACQUIRE_MIN_SWEEP_INTERVAL_SECS)
    c.sweep_interval_secs = ACQUIRE_MIN_SWEEP_INTERVAL_SECS;

  if(c.tick_cadence_secs == 0)
    c.tick_cadence_secs = ACQUIRE_DEF_TICK_CADENCE_SECS;
  if(c.tick_cadence_secs < ACQUIRE_MIN_TICK_CADENCE_SECS)
    c.tick_cadence_secs = ACQUIRE_MIN_TICK_CADENCE_SECS;

  if(c.max_reactive_per_tick == 0)
    c.max_reactive_per_tick = ACQUIRE_DEF_MAX_REACTIVE_PER_TICK;
  if(c.max_reactive_per_tick > ACQUIRE_MAX_REACTIVE_PER_TICK_CEIL)
    c.max_reactive_per_tick = ACQUIRE_MAX_REACTIVE_PER_TICK_CEIL;

  pthread_mutex_lock(&acquire_cfg_mutex);
  acquire_cfg        = c;
  acquire_dedup_cap  = c.reactive_dedup_lru_size;
  pthread_mutex_unlock(&acquire_cfg_mutex);
}

static void
acquire_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  acquire_load_config();
}

static void
acquire_register_kv(void)
{
  kv_register("acquire.enabled", KV_BOOL, "true",
      acquire_kv_changed, NULL,
      "Enable the autonomous knowledge-acquisition engine");
  kv_register("acquire.relevance_threshold", KV_UINT32, "50",
      acquire_kv_changed, NULL,
      "LLM-scored relevance cutoff (0-100) for ingesting a digest");
  kv_register("acquire.max_sources_per_query", KV_UINT32, "3",
      acquire_kv_changed, NULL,
      "Top SXNG results to fetch + digest per query");
  kv_register("acquire.digest_body_truncate_chars", KV_UINT32, "6000",
      acquire_kv_changed, NULL,
      "Max page bytes sent to the digest LLM. Higher = richer"
      " summaries at higher cost; lower = faster. Default 6000.");

  // A6 — reactive path knobs.
  kv_register("acquire.max_reactive_per_hour", KV_UINT32, "10",
      acquire_kv_changed, NULL,
      "Reactive acquisitions allowed per (bot, topic) per rolling hour."
      " Exceeded topics are skipped (with a DEBUG log) until the window"
      " slides.");
  kv_register("acquire.reactive_subject_lru_size", KV_UINT32, "256",
      acquire_kv_changed, NULL,
      "Size of the per-(bot, topic, subject) dedup LRU. Accepts up to"
      " 1024; larger values are silently clamped.");
  kv_register("acquire.reactive_dedup_ttl_secs", KV_UINT32, "3600",
      acquire_kv_changed, NULL,
      "Seconds before a dedup LRU entry expires and a repeat mention"
      " of the same subject can fire again.");

  // A8 — corpus lifecycle sweep cadence.
  kv_register("acquire.sweep_interval_secs", KV_UINT32, "3600",
      acquire_kv_changed, NULL,
      "Seconds between engine-wide corpus-lifecycle sweeps. Clamped to"
      " a 60-second minimum at load time. The sweep walks every"
      " registered bot's destination corpus, applies the per-bot TTL"
      " (if any), then oldest-first DELETEs in 100-row batches until"
      " under the per-bot size cap.");

  // Per-bot tick cadence + reactive drain cap.
  kv_register("acquire.tick_cadence_secs", KV_UINT32, "600",
      acquire_kv_changed, NULL,
      "Seconds between per-bot acquisition ticks. The tick callback"
      " re-reads this on every fire and rescheduleds itself on the new"
      " interval, so live changes take effect within at most one"
      " current-cadence window. Minimum 5 seconds.");
  kv_register("acquire.max_reactive_per_tick", KV_UINT32, "1",
      acquire_kv_changed, NULL,
      "Reactive jobs drained from each bot's ring buffer per tick."
      " Default 1 keeps a noisy channel from monopolising the engine;"
      " raise for testing or for channels where reactive jobs pile up."
      " Capped at the ring size (32).");

  // I1 — image extractor knobs. Cluster together so `botmanctl kv list
  // acquire.image*` presents them as a coherent surface.
  kv_register("acquire.images_enabled", KV_BOOL, "true",
      acquire_kv_changed, NULL,
      "Master switch for the image extractor. When false, acquire skips"
      " the extraction step entirely — no knowledge_images rows produced.");
  kv_register("acquire.image_min_dim_px", KV_UINT32, "200",
      acquire_kv_changed, NULL,
      "Minimum declared width OR height (in HTML attributes) for an"
      " <img> to be kept. Images with no declared dimensions are still"
      " accepted unless acquire.image_require_dims=true.");
  kv_register("acquire.image_max_per_page", KV_UINT32, "6",
      acquire_kv_changed, NULL,
      "Hard cap on images harvested from a single page during acquire."
      " OpenGraph and Twitter-card images take the first slots, with"
      " in-body <img> tags filling the rest. Clamped to 32.");
  kv_register("acquire.image_url_skiplist", KV_STR,
      ACQUIRE_DEF_IMAGE_URL_SKIPLIST,
      acquire_kv_changed, NULL,
      "Semicolon-separated substrings matched case-insensitively against"
      " resolved image URLs. Any hit drops the image. Parsed on load"
      " and on KV change; up to 16 entries.");
  kv_register("acquire.image_require_dims", KV_BOOL, "false",
      acquire_kv_changed, NULL,
      "When true, <img> tags missing both width and height attributes"
      " are dropped. Default false — many CMSs rely on CSS for sizing.");

  // Personality-declared feeds: master switch. When false,
  // acq_feeds_tick is a no-op per bot per tick; SXNG reactive + proactive
  // paths are unaffected.
  kv_register("acquire.sources_enabled", KV_BOOL, "true",
      acquire_kv_changed, NULL,
      "Enable personality-declared RSS/HTML source fetching. When false,"
      " the per-tick source dispatcher is a no-op; reactive and proactive"
      " paths are unaffected.");
}

// DDL

static void
acquire_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res->error[0] != '\0')
      clam(CLAM_WARN, ACQUIRE_CTX, "ensure_tables: %s", res->error);
  }

  db_result_free(res);
}

static void
acquire_ensure_tables(void)
{
  acquire_run_ddl(
      "CREATE TABLE IF NOT EXISTS acquire_topic_stats ("
      " bot_name       VARCHAR(64)  NOT NULL,"
      " topic_name     VARCHAR(64)  NOT NULL,"
      " last_proactive TIMESTAMPTZ,"
      " last_reactive  TIMESTAMPTZ,"
      " total_queries  BIGINT NOT NULL DEFAULT 0,"
      " total_ingested BIGINT NOT NULL DEFAULT 0,"
      " PRIMARY KEY (bot_name, topic_name)"
      ")");
}

// Registry helpers (must be called under acquire_entries_lock)

acquire_bot_entry_t *
acquire_entry_find_locked(const char *name)
{
  for(acquire_bot_entry_t *e = acquire_entries; e != NULL; e = e->next)
    if(strcmp(e->name, name) == 0)
      return(e);

  return(NULL);
}

static void
acquire_entry_free_topics(acquire_bot_entry_t *e)
{
  if(e->topics != NULL)
  {
    mem_free(e->topics);
    e->topics = NULL;
  }

  if(e->topic_proactive_counter != NULL)
  {
    mem_free(e->topic_proactive_counter);
    e->topic_proactive_counter = NULL;
  }

  if(e->feeds != NULL)
  {
    mem_free(e->feeds);
    e->feeds = NULL;
  }

  if(e->feed_state != NULL)
  {
    mem_free(e->feed_state);
    e->feed_state = NULL;
  }

  if(e->feed_topic_idx != NULL)
  {
    mem_free(e->feed_topic_idx);
    e->feed_topic_idx = NULL;
  }

  e->n_topics       = 0;
  e->n_feeds_total  = 0;
  e->feed_next_idx  = 0;
}

// Reactive-path plumbing (A6)

// Case-insensitive subject compare used by the dedup LRU.
static inline bool
acq_subject_equals(const char *a, const char *b)
{
  return(strcasecmp(a, b) == 0);
}

// Dedup LRU: returns true if (bot, topic, subject) was seen in the
// last TTL seconds. Any stale slot encountered along the way is
// reclaimed. Caller holds no locks; we take acquire_dedup_mutex for
// the scan + write.
static bool
acquire_dedup_check_and_insert(const char *bot, const char *topic,
    const char *subject)
{
  time_t now = time(NULL);

  uint32_t ttl;
  bool duplicate;
  uint32_t cap;
  pthread_mutex_lock(&acquire_cfg_mutex);
  ttl = acquire_cfg.reactive_dedup_ttl_secs;
  cap = acquire_dedup_cap;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(cap == 0)
    cap = ACQUIRE_DEF_REACTIVE_DEDUP_LRU_SIZE;
  if(cap > ACQUIRE_REACTIVE_DEDUP_LRU_MAX)
    cap = ACQUIRE_REACTIVE_DEDUP_LRU_MAX;

  duplicate = false;

  pthread_mutex_lock(&acquire_dedup_mutex);

  for(uint32_t i = 0; i < cap; i++)
  {
    acquire_dedup_slot_t *s = &acquire_dedup[i];

    if(!s->in_use)
      continue;

    if(now - s->ts > (time_t)ttl)
    {
      s->in_use = false;
      continue;
    }

    if(strcmp(s->bot, bot) == 0
        && strcmp(s->topic, topic) == 0
        && acq_subject_equals(s->subject, subject))
    {
      duplicate = true;
      break;
    }
  }

  if(!duplicate)
  {
    // Write at the cursor; oldest eviction.
    uint32_t slot = acquire_dedup_next % cap;

    acquire_dedup_slot_t *s = &acquire_dedup[slot];

    s->in_use = true;
    snprintf(s->bot,     sizeof(s->bot),     "%s", bot);
    snprintf(s->topic,   sizeof(s->topic),   "%s", topic);
    snprintf(s->subject, sizeof(s->subject), "%s", subject);
    s->ts = now;

    acquire_dedup_next++;
  }

  pthread_mutex_unlock(&acquire_dedup_mutex);

  return(duplicate);
}

// Per-(bot, topic) rate limiter. Returns true when the topic has
// fired its Nth job within the last hour (i.e. limit reached).
// Called only by the tick thread, so the counters need no lock.
bool
acquire_rate_exceeded(acquire_bot_entry_t *e, const char *topic_name,
    uint32_t max_per_hour)
{
  time_t now;
  uint32_t fresh;
  uint32_t pos;
  acquire_rate_slot_t *slot;
  if(max_per_hour == 0)
    return(false);

  now = time(NULL);

  // Locate or allocate the slot.
  slot = NULL;

  for(size_t i = 0; i < e->n_rate; i++)
  {
    if(strcmp(e->rate[i].topic_name, topic_name) == 0)
    {
      slot = &e->rate[i];
      break;
    }
  }

  if(slot == NULL)
  {
    if(e->n_rate >= sizeof(e->rate) / sizeof(e->rate[0]))
    {
      // No free slot — just let the job through (better than silently
      // over-rate-limiting). Would only happen with pathological
      // topic counts; the struct already sizes generously.
      return(false);
    }

    slot = &e->rate[e->n_rate++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->topic_name, sizeof(slot->topic_name), "%s", topic_name);
  }

  // Count entries within the last hour.
  fresh = 0;

  for(uint32_t i = 0; i < slot->count; i++)
    if(now - slot->window[i] <= 3600)
      fresh++;

  if(fresh >= max_per_hour)
    return(true);

  // Record this firing.
  pos = slot->next % ACQUIRE_REACTIVE_RATE_WINDOW_SLOTS;
  slot->window[pos] = now;
  slot->next++;
  if(slot->count < ACQUIRE_REACTIVE_RATE_WINDOW_SLOTS)
    slot->count++;

  return(false);
}

// Ring push (producer). Overflow drops the oldest — reactive signals
// are latency-sensitive, stale mentions are worth less than fresh.
static void
acquire_ring_push(acquire_bot_entry_t *e,
    const char *topic_name, const char *subject)
{
  acquire_reactive_job_t *job;
  pthread_mutex_lock(&e->ring_mutex);

  if(e->ring_count == ACQUIRE_REACTIVE_RING_CAP)
  {
    // Drop oldest.
    e->ring_head = (e->ring_head + 1) % ACQUIRE_REACTIVE_RING_CAP;
    e->ring_count--;
  }

  job = &e->ring[e->ring_tail];

  snprintf(job->topic_name, sizeof(job->topic_name), "%s", topic_name);
  snprintf(job->subject,    sizeof(job->subject),    "%s", subject);

  e->ring_tail = (e->ring_tail + 1) % ACQUIRE_REACTIVE_RING_CAP;
  e->ring_count++;

  pthread_mutex_unlock(&e->ring_mutex);
}

// Ring pop (consumer). Returns true on success.
bool
acquire_ring_pop(acquire_bot_entry_t *e, acquire_reactive_job_t *out)
{
  pthread_mutex_lock(&e->ring_mutex);

  if(e->ring_count == 0)
  {
    pthread_mutex_unlock(&e->ring_mutex);
    return(false);
  }

  *out = e->ring[e->ring_head];

  e->ring_head = (e->ring_head + 1) % ACQUIRE_REACTIVE_RING_CAP;
  e->ring_count--;

  pthread_mutex_unlock(&e->ring_mutex);
  return(true);
}

// Locate a topic by name in the bot's registered topic list. Called
// under acquire_entries_lock (rdlock). Returns NULL when the topic
// was unregistered between enqueue and dequeue (rare but harmless).
const acquire_topic_t *
acquire_find_topic(const acquire_bot_entry_t *e, const char *topic_name)
{
  for(size_t i = 0; i < e->n_topics; i++)
    if(strcmp(e->topics[i].name, topic_name) == 0)
      return(&e->topics[i]);

  return(NULL);
}



// Public registration API

bool
acquire_register_topics(const char *bot_name,
    const acquire_topic_t *topics, size_t n_topics,
    const char *dest_corpus)
{
  acquire_bot_entry_t *e;
  task_handle_t task_ref;
  bool created;
  if(!acquire_ready)
    return(FAIL);

  if(bot_name == NULL || bot_name[0] == '\0')
    return(FAIL);

  if(n_topics > 0 && topics == NULL)
    return(FAIL);

  pthread_rwlock_wrlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);
  created = false;

  if(e == NULL)
  {
    e = mem_alloc(ACQUIRE_CTX, "bot_entry", sizeof(*e));
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", bot_name);
    pthread_mutex_init(&e->ring_mutex, NULL);
    e->next = acquire_entries;
    acquire_entries = e;
    created = true;
  }

  else
  {
    acquire_entry_free_topics(e);

    // Reset any stale reactive state from a previous registration —
    // topic lists may have changed, and the old rate counters /
    // pending jobs are meaningless against the new keyword set.
    pthread_mutex_lock(&e->ring_mutex);
    e->ring_head  = 0;
    e->ring_tail  = 0;
    e->ring_count = 0;
    pthread_mutex_unlock(&e->ring_mutex);
    memset(e->rate, 0, sizeof(e->rate));
    e->n_rate = 0;
  }

  snprintf(e->dest_corpus, sizeof(e->dest_corpus), "%s",
      dest_corpus != NULL ? dest_corpus : "");

  if(n_topics > 0)
  {
    size_t total_feeds = 0;
    size_t k;

    e->topics = mem_alloc(ACQUIRE_CTX, "topic_list",
        n_topics * sizeof(acquire_topic_t));
    memcpy(e->topics, topics, n_topics * sizeof(acquire_topic_t));

    // A7 sidecar: one uint32_t per topic, index-aligned with
    // e->topics. Allocated in lockstep with the topic list; freed by
    // acquire_entry_free_topics. Zero-initialised so the first
    // proactive fire for each topic picks topic->query (counter==0,
    // counter%10 != 9).
    e->topic_proactive_counter = mem_alloc(ACQUIRE_CTX,
        "topic_proactive_counter", n_topics * sizeof(uint32_t));

    // Feed flattening. Sum declared feeds across all topics, then
    // allocate three parallel arrays. feed_state[] is zero-initialised
    // so last_fetched==0 fires on the first due tick after reload —
    // matches the per-run rebuild policy spelled out in the plan.
    for(size_t i = 0; i < n_topics; i++)
      total_feeds += topics[i].n_feeds;

    if(total_feeds > 0)
    {
      e->feeds = mem_alloc(ACQUIRE_CTX, "feed_list",
          total_feeds * sizeof(acquire_feed_t));
      e->feed_state = mem_alloc(ACQUIRE_CTX, "feed_state",
          total_feeds * sizeof(acquire_feed_state_t));
      e->feed_topic_idx = mem_alloc(ACQUIRE_CTX, "feed_topic_idx",
          total_feeds * sizeof(size_t));

      k = 0;

      for(size_t i = 0; i < n_topics; i++)
      {
        for(size_t j = 0; j < topics[i].n_feeds; j++)
        {
          e->feeds[k] = topics[i].feeds[j];
          e->feed_topic_idx[k] = i;
          k++;
        }
      }
    }

    e->n_feeds_total = total_feeds;
    e->feed_next_idx = 0;
  }

  e->n_topics = n_topics;
  e->active   = true;

  task_ref = e->task;

  pthread_rwlock_unlock(&acquire_entries_lock);

  // Spawn the periodic task outside the rwlock — task_add_periodic
  // may allocate and lock task_lock internally, and we want the
  // smallest possible critical section.
  if(task_ref == TASK_HANDLE_NONE)
  {
    char tname[TASK_NAME_SZ];

    uint32_t cadence_secs;
    task_handle_t t;
    snprintf(tname, sizeof(tname), "acquire:%s", bot_name);

    pthread_mutex_lock(&acquire_cfg_mutex);
    cadence_secs = acquire_cfg.tick_cadence_secs;
    pthread_mutex_unlock(&acquire_cfg_mutex);

    if(cadence_secs == 0)
      cadence_secs = ACQUIRE_DEF_TICK_CADENCE_SECS;

    t = task_add_periodic(tname, TASK_ANY, 200,
        cadence_secs * 1000,
        acquire_bot_tick, e);

    pthread_rwlock_wrlock(&acquire_entries_lock);
    e->task = t;
    pthread_rwlock_unlock(&acquire_entries_lock);
  }

  clam(CLAM_INFO, ACQUIRE_CTX,
      "registered bot='%s' topics=%zu corpus='%s'%s",
      bot_name, n_topics,
      dest_corpus != NULL ? dest_corpus : "",
      created ? " (new)" : " (replaced)");

  return(SUCCESS);
}

void
acquire_unregister_bot(const char *bot_name)
{
  acquire_bot_entry_t *e;
  if(!acquire_ready || bot_name == NULL || bot_name[0] == '\0')
    return;

  pthread_rwlock_wrlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);

  if(e != NULL)
  {
    e->active = false;
    acquire_entry_free_topics(e);
  }

  pthread_rwlock_unlock(&acquire_entries_lock);

  if(e != NULL)
    clam(CLAM_INFO, ACQUIRE_CTX, "unregistered bot='%s' (tick no-ops)",
        bot_name);
}

// Reactive enqueue (A6) — called from chat observer threads

acq_enq_result_t
acquire_enqueue_reactive(const char *bot_name, const char *topic_name,
    const char *subject)
{
  acquire_bot_entry_t *e;
  if(!acquire_ready)
    return(ACQ_ENQ_NOT_READY);

  if(bot_name   == NULL || bot_name[0]   == '\0') return(ACQ_ENQ_NOT_READY);
  if(topic_name == NULL || topic_name[0] == '\0') return(ACQ_ENQ_NOT_READY);
  if(subject    == NULL || subject[0]    == '\0') return(ACQ_ENQ_NOT_READY);

  // Dedup LRU check before doing anything expensive.
  if(acquire_dedup_check_and_insert(bot_name, topic_name, subject))
  {
    pthread_mutex_lock(&acquire_stat_mutex);
    acquire_stats.total_reactive_dedup_drops++;
    pthread_mutex_unlock(&acquire_stat_mutex);

    clam(CLAM_DEBUG2, ACQUIRE_CTX,
        "reactive dedup hit bot=%s topic=%s subject='%s' — skipping",
        bot_name, topic_name, subject);
    return(ACQ_ENQ_DEDUP);
  }

  pthread_rwlock_rdlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);

  if(e == NULL || !e->active)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    return(ACQ_ENQ_BOT_UNKNOWN);
  }

  // Verify the topic exists in the current registration — avoids
  // pushing to a ring whose dequeue will just drop the job.
  if(acquire_find_topic(e, topic_name) == NULL)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    clam(CLAM_DEBUG2, ACQUIRE_CTX,
        "reactive: topic '%s' not registered for bot=%s — skipping",
        topic_name, bot_name);
    return(ACQ_ENQ_TOPIC_UNKNOWN);
  }

  acquire_ring_push(e, topic_name, subject);

  pthread_rwlock_unlock(&acquire_entries_lock);

  pthread_mutex_lock(&acquire_stat_mutex);
  acquire_stats.total_reactive_enqueued++;
  pthread_mutex_unlock(&acquire_stat_mutex);

  // Kick the per-bot drain so the job fires within ~1 s instead of
  // waiting on the next periodic tick (up to tick_cadence_secs away).
  acq_bot_kick_submit_deferred(e);

  return(ACQ_ENQ_ACCEPTED);
}

const char *
acquire_enq_result_str(acq_enq_result_t r)
{
  switch(r)
  {
    case ACQ_ENQ_ACCEPTED:      return("accepted");
    case ACQ_ENQ_DEDUP:         return("dedup");
    case ACQ_ENQ_TOPIC_UNKNOWN: return("topic_not_registered");
    case ACQ_ENQ_BOT_UNKNOWN:   return("bot_not_registered");
    case ACQ_ENQ_NOT_READY:     return("subsystem_off");
  }

  return("unknown");
}

// Stats

void
acquire_register_ingest_cb(acquire_ingest_cb_t cb, void *user)
{
  pthread_mutex_lock(&acquire_ingest_cb_mutex);

  if(cb != NULL && acquire_ingest_cb != NULL && acquire_ingest_cb != cb)
    clam(CLAM_WARN, ACQUIRE_CTX,
        "ingest callback already registered — overwriting"
        " (only one consumer is supported in v1)");

  acquire_ingest_cb   = cb;
  acquire_ingest_user = user;

  pthread_mutex_unlock(&acquire_ingest_cb_mutex);
}

void
acquire_get_stats(acquire_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&acquire_stat_mutex);
  *out = acquire_stats;
  pthread_mutex_unlock(&acquire_stat_mutex);
}

size_t
acquire_get_topic_snapshot(const char *bot_name,
    acquire_topic_t *out, size_t cap)
{
  acquire_bot_entry_t *e;
  size_t n;
  if(bot_name == NULL || out == NULL || cap == 0)
    return(0);

  if(!acquire_ready)
    return(0);

  pthread_rwlock_rdlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);

  if(e == NULL || !e->active || e->n_topics == 0 || e->topics == NULL)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    return(0);
  }

  n = e->n_topics < cap ? e->n_topics : cap;

  memcpy(out, e->topics, n * sizeof(acquire_topic_t));

  pthread_rwlock_unlock(&acquire_entries_lock);
  return(n);
}



// Lifecycle

void
acquire_init(void)
{
  if(acquire_ready)
    return;

  pthread_mutex_init(&acquire_cfg_mutex, NULL);
  pthread_mutex_init(&acquire_stat_mutex, NULL);
  pthread_mutex_init(&acquire_dedup_mutex, NULL);
  pthread_mutex_init(&acquire_ingest_cb_mutex, NULL);
  pthread_rwlock_init(&acquire_entries_lock, NULL);

  // libxml2 global init — idempotent, thread-safe after first call.
  // Required once before any xmlReadMemory / htmlReadMemory from
  // acquire_feed.c.
  xmlInitParser();

  memset(&acquire_cfg, 0, sizeof(acquire_cfg));
  acquire_cfg.enabled                    = true;
  acquire_cfg.relevance_threshold        = ACQUIRE_DEF_RELEVANCE_THRESHOLD;
  acquire_cfg.max_sources_per_query      = ACQUIRE_DEF_MAX_SOURCES_PER_QUERY;
  acquire_cfg.digest_body_truncate_chars = ACQUIRE_DEF_DIGEST_BODY_TRUNCATE_CHARS;
  acquire_cfg.max_reactive_per_hour      = ACQUIRE_DEF_MAX_REACTIVE_PER_HOUR;
  acquire_cfg.reactive_dedup_lru_size    = ACQUIRE_DEF_REACTIVE_DEDUP_LRU_SIZE;
  acquire_cfg.reactive_dedup_ttl_secs    = ACQUIRE_DEF_REACTIVE_DEDUP_TTL_SECS;
  acquire_cfg.sweep_interval_secs        = ACQUIRE_DEF_SWEEP_INTERVAL_SECS;
  acquire_cfg.sources_enabled            = true;

  memset(&acquire_stats, 0, sizeof(acquire_stats));
  memset(acquire_dedup, 0, sizeof(acquire_dedup));
  acquire_dedup_cap  = ACQUIRE_DEF_REACTIVE_DEDUP_LRU_SIZE;
  acquire_dedup_next = 0;

  acquire_ready = true;

  clam(CLAM_INFO, ACQUIRE_CTX, "acquire subsystem initialized");
}

void
acquire_register_config(void)
{
  uint32_t sweep_interval;
  acquire_register_kv();
  acquire_load_config();
  acquire_ensure_tables();

  // A8 — engine-global corpus-lifecycle sweep. Snapshot the cadence
  // from the freshly-loaded config; the task subsystem does not
  // honour live cadence changes, but the sweep callback re-reads
  // per-bot policy from KV on every tick, so interval is the only
  // stale datum.
  pthread_mutex_lock(&acquire_cfg_mutex);
  sweep_interval = acquire_cfg.sweep_interval_secs;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(sweep_interval < ACQUIRE_MIN_SWEEP_INTERVAL_SECS)
    sweep_interval = ACQUIRE_MIN_SWEEP_INTERVAL_SECS;

  if(acquire_sweep_task == TASK_HANDLE_NONE)
    acquire_sweep_task = task_add_periodic("acquire.sweep",
        TASK_ANY, 200, sweep_interval * 1000,
        acquire_sweep_tick, NULL);
}

void
acquire_exit(void)
{
  acquire_bot_entry_t *e;
  if(!acquire_ready)
    return;

  acquire_ready = false;

  // Workers are already joined by pool_exit; safe to free entries
  // without worrying about racing task callbacks.
  pthread_rwlock_wrlock(&acquire_entries_lock);

  e = acquire_entries;

  while(e != NULL)
  {
    acquire_bot_entry_t *next = e->next;

    acquire_entry_free_topics(e);
    pthread_mutex_destroy(&e->ring_mutex);
    mem_free(e);
    e = next;
  }

  acquire_entries = NULL;

  pthread_rwlock_unlock(&acquire_entries_lock);

  pthread_rwlock_destroy(&acquire_entries_lock);
  pthread_mutex_destroy(&acquire_cfg_mutex);
  pthread_mutex_destroy(&acquire_stat_mutex);
  pthread_mutex_destroy(&acquire_dedup_mutex);
  pthread_mutex_destroy(&acquire_ingest_cb_mutex);

  // libxml2 cleanup — idempotent, releases global parser state.
  xmlCleanupParser();

  clam(CLAM_INFO, ACQUIRE_CTX, "acquire subsystem shut down");
}
