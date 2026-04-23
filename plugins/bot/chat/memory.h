#ifndef BM_MEMORY_H
#define BM_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Public types

// Fact kinds: stored as SMALLINT in user_facts.kind.
typedef enum
{
  MEM_FACT_PREFERENCE = 0,
  MEM_FACT_ATTRIBUTE  = 1,
  MEM_FACT_RELATION   = 2,
  MEM_FACT_EVENT      = 3,
  MEM_FACT_OPINION    = 4,
  MEM_FACT_FREEFORM   = 5
} mem_fact_kind_t;

// Mask helper: matches every fact kind in memory_get_facts().
#define MEM_FACT_KIND_ANY  0xFFFFFFFFu

// Convert kind enum to a bit in a kinds mask.
#define MEM_FACT_KIND_BIT(k)  ((uint32_t)1 << (uint32_t)(k))

// Conversation-log message kinds.
typedef enum
{
  MEM_MSG_WITNESS      = 0,   // overheard line; bot not addressed
  MEM_MSG_EXCHANGE_IN  = 1,   // directed at the bot
  MEM_MSG_EXCHANGE_OUT = 2    // bot's own reply
} mem_msg_kind_t;

// Merge policies for memory_upsert_fact().
typedef enum
{
  MEM_MERGE_REPLACE,          // overwrite value unconditionally
  MEM_MERGE_HIGHER_CONF,      // overwrite only if new confidence is higher
  MEM_MERGE_APPEND_HISTORY    // keep old, append new value with a separator
} mem_merge_t;

// A single fact row. Strings are caller-owned for upsert; for reads the
// storage is populated by the library and valid for the caller's buffer.
#define MEM_FACT_KEY_SZ    128
#define MEM_FACT_VALUE_SZ  1024
#define MEM_FACT_SOURCE_SZ 32
#define MEM_FACT_CHANNEL_SZ 128

typedef struct
{
  int64_t          id;                             // 0 on upsert = new
  int              user_id;                        // userns_user.id
  mem_fact_kind_t  kind;
  char             fact_key[MEM_FACT_KEY_SZ];
  char             fact_value[MEM_FACT_VALUE_SZ];
  char             source[MEM_FACT_SOURCE_SZ];     // "llm_extract","admin_seed"
  char             channel[MEM_FACT_CHANNEL_SZ];   // "" == DM
  float            confidence;
  time_t           observed_at;
  time_t           last_seen;
} mem_fact_t;

// A single dossier-keyed fact row. Mirrors mem_fact_t but is scoped to
// the dossier subsystem (the llm bot's source of truth for memory).
// dossier_id links to dossier.id rather than userns_user.id. See
// include/dossier.h for the dossier identity model.
typedef struct
{
  int64_t          id;                             // 0 on upsert = new
  int64_t          dossier_id;                     // dossier.id
  mem_fact_kind_t  kind;
  char             fact_key[MEM_FACT_KEY_SZ];
  char             fact_value[MEM_FACT_VALUE_SZ];
  char             source[MEM_FACT_SOURCE_SZ];     // "llm_extract","admin_seed"
  char             channel[MEM_FACT_CHANNEL_SZ];   // "" == DM
  float            confidence;
  time_t           observed_at;
  time_t           last_seen;
} mem_dossier_fact_t;

// A single conversation-log row.
#define MEM_MSG_BOT_SZ      64
#define MEM_MSG_METHOD_SZ   64
#define MEM_MSG_CHANNEL_SZ  128
#define MEM_MSG_TEXT_SZ     4096
#define MEM_MSG_REFS_MAX    8

typedef struct
{
  int64_t         id;                              // 0 on insert
  int             ns_id;                           // userns.id
  int             user_id_or_0;                    // 0 = unknown sender
  int64_t         dossier_id;                      // 0 = no dossier
  char            bot_name[MEM_MSG_BOT_SZ];
  char            method[MEM_MSG_METHOD_SZ];
  char            channel[MEM_MSG_CHANNEL_SZ];
  mem_msg_kind_t  kind;
  char            text[MEM_MSG_TEXT_SZ];
  time_t          ts;                              // 0 = NOW()
  // Dossier ids mentioned by name in this message's text. Populated by
  // witness-time wiring (see chatbot_log_line + dossier_find_mentions).
  // Serialized as conversation_log.referenced_dossiers JSONB; NULL when
  // n_referenced == 0 (preserves "not computed" vs "computed and empty").
  int64_t         referenced_dossiers[MEM_MSG_REFS_MAX];
  uint8_t         n_referenced;
} mem_msg_t;

// Per-line truncation cap on each recent-own-replies excerpt spliced
// into the system prompt. Sized so a caller can stack-allocate an array
// of mem_recent_reply_t without worrying about one pathological reply
// consuming the prompt budget.
#define CHATBOT_RECENT_REPLY_TEXT_SZ 256

typedef struct
{
  char    text[CHATBOT_RECENT_REPLY_TEXT_SZ];
  int64_t ts;                      // epoch seconds
} mem_recent_reply_t;

// Opaque retrieval handle (reserved for Chunk D async path).
typedef struct mem_retrieval mem_retrieval_t;

// Subsystem statistics.
typedef struct
{
  uint64_t  total_facts;      // cumulative facts upserted
  uint64_t  total_logs;       // cumulative messages logged
  uint64_t  decay_sweeps;     // number of decay sweeps run
  uint64_t  forgets;          // cumulative forget_fact + forget_user calls
} memory_stats_t;

// Lifecycle

// Initialize the memory subsystem. Must be called after llm_init().
// Allocates mutexes; does not touch DB or KV yet.
void memory_init(void);

// Register KV keys under memory.*. Must be called before kv_load() so
// loaded values land on registered keys.
void memory_register_config(void);

// Load KV into memory_cfg, ensure DB tables exist, and schedule the
// periodic decay sweep. Must be called after kv_load(), db_init(), and
// userns_init() (the tables FK into userns / userns_user).
void memory_ensure_schema(void);

// Register /memory * and /show memory commands. Must be called after
// cmd_init().
void memory_register_commands(void);

// Shut down the subsystem. Cancels the decay task; flushes no state.
void memory_exit(void);

// Conversation log

// Fetch up to max_k most-recent MEM_MSG_EXCHANGE_OUT rows for
// (bot_name, method, channel, ns_id), newest first, filtered to rows
// whose ts is within max_age_secs of "now". Returns the number of rows
// written to out[].
//
// out must have at least max_k elements. If max_k == 0, returns 0. If
// max_age_secs == 0, no age filter is applied.
size_t memory_recent_own_replies(const char *bot_name, const char *method,
    const char *channel, uint32_t ns_id, uint32_t max_age_secs,
    mem_recent_reply_t *out, size_t max_k);

// Fire-and-forget insert into conversation_log. Synchronous DB round
// trip in this chunk. Caller copies are taken; msg may be freed after
// return. In Chunk D this will also enqueue an embedding job for
// EXCHANGE_* kinds (and WITNESS when memory.witness_embeds=true).
// msg: message to log (may not be NULL)
void memory_log_message(const mem_msg_t *msg);

// Facts

// Upsert a fact, deduplicating by (user_id, kind, fact_key).
// returns: SUCCESS or FAIL
// fact: fact to upsert (may not be NULL)
// policy: merge behavior on conflict
bool memory_upsert_fact(const mem_fact_t *fact, mem_merge_t policy);

size_t memory_get_facts(int user_id, uint32_t kinds_mask,
    mem_fact_t *out, size_t cap);

bool memory_forget_fact(int64_t fact_id);

// Dossier-keyed facts (llm bot memory)

// Upsert a dossier-keyed fact, deduplicating by (dossier_id, kind,
// fact_key). Mirrors memory_upsert_fact semantics exactly, including
// merge policies. Dossier FK violations are reported and treated as
// FAIL (no row inserted).
//
// returns: SUCCESS or FAIL
// fact: fact to upsert (may not be NULL)
// policy: merge behavior on conflict
bool memory_upsert_dossier_fact(const mem_dossier_fact_t *fact,
    mem_merge_t policy);

bool memory_kind_from_name(const char *s, mem_fact_kind_t *out);

size_t memory_get_dossier_facts(int64_t dossier_id, uint32_t kinds_mask,
    mem_dossier_fact_t *out, size_t cap);

bool memory_forget_dossier_fact(int64_t fact_id);

// Delete every fact, log entry, and embedding row for the given user
// via FK ON DELETE CASCADE on userns_user.
// returns: SUCCESS or FAIL
bool memory_forget_user(int user_id);

// Retrieval (Chunk D placeholder)

// Retrieval callback: delivered once per memory_retrieve() call. Facts
// and msgs arrays are valid for the duration of the callback only.
typedef void (*memory_retrieve_cb_t)(const mem_fact_t *facts, size_t n_facts,
    const mem_msg_t *msgs, size_t n_msgs, void *user);

// In Chunk C this is a synchronous stub: cb fires immediately with
// zero results, before the call returns.
// returns: SUCCESS or FAIL
bool memory_retrieve(int ns_id, int user_id_or_0, const char *query,
    uint32_t top_k, memory_retrieve_cb_t cb, void *user);

bool memory_retrieve_dossier(int ns_id, int64_t dossier_id,
    const char *query, uint32_t top_k, memory_retrieve_cb_t cb,
    void *user);

// Decay + stats

// Delete facts whose decayed confidence has dropped below
// memory.min_fact_confidence. Rows with source='admin_seed' are skipped.
void memory_decay_sweep(void);

void memory_get_stats(memory_stats_t *out);

// /show user verb helpers — invoked by cmd_show_user (userns_cmd.c).
// Caller resolves the namespace; helpers handle user lookup + render.

struct cmd_ctx;
struct userns;

void memory_show_user_facts(const struct cmd_ctx *ctx,
    struct userns *ns, const char *username);

void memory_show_user_log(const struct cmd_ctx *ctx,
    struct userns *ns, const char *username, uint32_t limit);

void memory_show_user_rag(const struct cmd_ctx *ctx,
    struct userns *ns, const char *username, const char *query);

// Test hooks (MEMORY_TEST_HOOKS only)

#ifdef MEMORY_TEST_HOOKS

// Test-only: check whether the DB layer is reachable. Tests use this to
// skip gracefully when no Postgres is running.
bool memory_test_db_ok(void);

// Test-only: reset in-memory stats counters to zero.
void memory_test_reset_stats(void);

bool memory_test_inject_embedding(int64_t id, bool is_fact,
    const char *model, uint32_t dim, const float *vec);

// Test-only: synchronous retrieve that skips the llm_embed_submit query
// path and uses a caller-supplied query vector. Cosine-scans both
// embedding tables, invokes the callback once, returns SUCCESS.
bool memory_test_retrieve_with_vec(int ns_id, int user_id_or_0,
    const char *model, uint32_t dim, const float *query_vec,
    uint32_t top_k, memory_retrieve_cb_t cb, void *user);

#endif // MEMORY_TEST_HOOKS

// Internal structures

#ifdef MEMORY_INTERNAL

#include "common.h"
#include "clam.h"
#include "kv.h"
#include "alloc.h"

#include <pthread.h>

// Defaults (applied before KV load).
#define MEM_DEF_LOG_RETENTION_DAYS       30
#define MEM_DEF_FACT_DECAY_HALF_LIFE     30
#define MEM_DEF_MIN_FACT_CONFIDENCE_X100 60   // stored as float 0.60
#define MEM_DEF_RAG_TOP_K                8
#define MEM_DEF_RAG_MAX_CONTEXT_CHARS    2048
#define MEM_DEF_DECAY_SWEEP_INTERVAL_SEC 3600
#define MEM_EMBED_MODEL_SZ               64
#define MEM_DEF_RECALL_TOP_K             4
#define MEM_DEF_RECALL_MIN_COSINE_X100   0   // 0 = no floor

// Buffer sizes used across helpers.
#define MEM_SQL_SZ      4096
#define MEM_ERR_SZ      256

// Cached configuration values (refreshed from KV on change).
typedef struct
{
  bool     enabled;
  bool     witness_embeds;
  uint32_t log_retention_days;
  uint32_t fact_decay_half_life_days;
  float    min_fact_confidence;
  uint32_t rag_top_k;
  uint32_t rag_max_context_chars;
  bool     embed_own_replies;
  uint32_t decay_sweep_interval_secs;
  char     embed_model[MEM_EMBED_MODEL_SZ];
  uint32_t recall_top_k;
  uint32_t recall_min_cosine_x100;
} mem_cfg_t;

// Module state shared across memory.c and its siblings (memory_rag.c,
// memory_cmd.c). Defined in memory.c.
extern bool              memory_ready;
extern mem_cfg_t         memory_cfg;
extern pthread_mutex_t   memory_cfg_mutex;
extern pthread_mutex_t   memory_stat_mutex;
extern uint64_t          memory_stat_facts;
extern uint64_t          memory_stat_logs;
extern uint64_t          memory_stat_sweeps;
extern uint64_t          memory_stat_forgets;
extern time_t            memory_last_sweep;

// Cross-file helpers defined in memory.c.
void memory_cfg_snapshot(mem_cfg_t *out);

// Shared SELECT column lists + row parsers. The macros and parsers must
// change in lock-step: the parser indexes into the ordinal positions the
// SELECT list emits.
struct db_result;
typedef struct db_result db_result_t;

#define MEMORY_FACT_SELECT_COLS \
    "id, user_id, kind, fact_key, fact_value, source, channel," \
    " confidence, EXTRACT(EPOCH FROM observed_at)::bigint," \
    " EXTRACT(EPOCH FROM last_seen)::bigint"

#define MEMORY_DOSSIER_FACT_SELECT_COLS \
    "id, dossier_id, kind, fact_key, fact_value, source, channel," \
    " confidence, EXTRACT(EPOCH FROM observed_at)::bigint," \
    " EXTRACT(EPOCH FROM last_seen)::bigint"

void memory_parse_fact_row(const db_result_t *r, uint32_t row,
    mem_fact_t *f);
void memory_parse_dossier_fact_row(const db_result_t *r, uint32_t row,
    mem_dossier_fact_t *f);

// Cross-file helpers defined in memory_rag.c.
float memory_cosine(const float *a, const float *b, uint32_t dim);
float *memory_bytea_to_vec(const char *cell, uint32_t expected_dim);

// Commands registration (defined in memory_cmd.c; called from memory.c's
// memory_register_commands).
void memory_register_cmds_internal(void);

#endif // MEMORY_INTERNAL

#endif // BM_MEMORY_H
