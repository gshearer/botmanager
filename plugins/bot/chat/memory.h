#ifndef BM_MEMORY_H
#define BM_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Public types

// Fact kinds: stored as SMALLINT in dossier_facts.kind.
typedef enum
{
  MEM_FACT_PREFERENCE = 0,
  MEM_FACT_ATTRIBUTE  = 1,
  MEM_FACT_RELATION   = 2,
  MEM_FACT_EVENT      = 3,
  MEM_FACT_OPINION    = 4,
  MEM_FACT_FREEFORM   = 5
} mem_fact_kind_t;

// Mask helper: matches every fact kind in memory_get_dossier_facts().
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

// Merge policies for memory_upsert_dossier_fact(). There are two, and
// the second is the interesting one (FACT-1).
//
// MEM_MERGE_OBSERVE resolves one incoming observation of
// (dossier, kind, key) against the stored row as exactly one of:
//
//   affirm   same value — refresh the evidence clock, never lower the
//            confidence, and upgrade the attribution if the affirming
//            source outranks the stored one. Evidence is evidence,
//            whoever brings it.
//   replace  different value from a source allowed to override — new
//            value, new confidence AS GIVEN (no GREATEST floor: that is
//            how rows pinned themselves at 1.0 and became immutable),
//            new clock.
//   reject   different value from a source that may not override — the
//            row is left BYTE-IDENTICAL, last_seen included. Prompt
//            injection orders facts by last_seen, so bumping it on a
//            rejected correction would promote the stale value.
//
// The ladder deciding "allowed to override" is admin_seed(3) >
// user_stated(2) > llm_extract / nl_observe / anything unknown (1).
// Higher rank replaces at any confidence; equal rank replaces at >=
// confidence, because facts describe mutable state and recency should
// win a tie; lower rank never replaces a differing value but may affirm
// an equal one. That single rule is also what keeps the hourly
// extraction sweep in its place: it fills gaps, people correct.
//
// MEM_MERGE_REPLACE is the admin door (`/dossier fact set`) and means
// exactly what it says.
typedef enum
{
  MEM_MERGE_REPLACE,          // overwrite value unconditionally
  MEM_MERGE_OBSERVE           // affirm / replace / reject, per the ladder
} mem_merge_t;

// A single fact row as delivered to a retrieval callback. Facts are
// stored dossier-keyed (mem_dossier_fact_t); this is the flattened
// shape the prompt assembler consumes, so it carries no owning id.
#define MEM_FACT_KEY_SZ    128
#define MEM_FACT_VALUE_SZ  1024
#define MEM_FACT_SOURCE_SZ 32
#define MEM_FACT_CHANNEL_SZ 128

typedef struct
{
  int64_t          id;
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
// dossier.h for the dossier identity model.
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
  // Cosine similarity to the query, set only when this row arrived via a
  // semantic-retrieval path. 0.0 on every other path (log writes, name-
  // mention fetches), where the field is meaningless.
  float           score;
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

// Register /user fact *, /user forget and /show memstore. Must be called after
// cmd_init().
void memory_register_commands(void);

// Cancel the periodic decay sweep. Must run before memory_exit() on the
// unload path: exit() tears down the state the sweep reads, and until
// the plugin is unmapped the task is still armed to read it.
void memory_stop(void);

// Shut down the subsystem. Flushes no state.
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

// Dossier-keyed facts (llm bot memory)
//
// Facts are keyed on dossier.id, never on userns_user.id: a dossier is
// what the bot has learned about a human, while a userns user is an
// auth principal. The user-keyed store these mirrored was deleted once
// it was established that nothing had written to it since dossiers
// landed.

// Upsert a dossier-keyed fact, deduplicating by (dossier_id, kind,
// fact_key). Dossier FK violations are reported and treated as FAIL
// (no row inserted).
//
// returns: SUCCESS or FAIL
// fact: fact to upsert (may not be NULL)
// policy: merge behavior on conflict
bool memory_upsert_dossier_fact(const mem_dossier_fact_t *fact,
    mem_merge_t policy);

bool memory_kind_from_name(const char *s, mem_fact_kind_t *out);

size_t memory_get_dossier_facts(int64_t dossier_id, uint32_t kinds_mask,
    mem_dossier_fact_t *out, size_t cap);

// Fetch the single best fact matching `exact_key` or the `key_prefix`
// family: an exact-key row beats any prefix row; newest last_seen
// breaks ties. No kind filter — the key is the contract, and extractor
// kind labels are model-chosen.
//
// returns: SUCCESS with *out filled iff a row matched
bool memory_get_dossier_fact_by_key(int64_t dossier_id,
    const char *exact_key, const char *key_prefix,
    mem_dossier_fact_t *out);

bool memory_forget_dossier_fact(int64_t fact_id);

// Retrieval

// Retrieval callback: delivered exactly once per retrieve call. Facts
// and msgs arrays are valid for the duration of the callback only.
//
// THREADING: the callback normally fires from the llm worker thread
// after the query embed completes, i.e. long after the submitting call
// returned. `user` must therefore be heap-owned and must not be a stack
// object belonging to the caller's frame. It fires synchronously, on
// the caller's thread, only when memory is disabled or no embed model
// is configured — so a caller may not assume either ordering.
typedef void (*memory_retrieve_cb_t)(const mem_fact_t *facts, size_t n_facts,
    const mem_msg_t *msgs, size_t n_msgs, void *user);

// Namespace-wide semantic search over conversation_log. Delivers
// messages only — facts are dossier-keyed and reached through
// memory_retrieve_dossier().
//
// OWNERSHIP: `cb` is delivered exactly once on EVERY path, including
// each path that returns FAIL. A caller that heap-allocates `user` must
// therefore let the callback free it and must NOT free it itself on a
// FAIL return — that is a double free. FAIL means "the search did not
// run", not "the callback did not fire".
// returns: SUCCESS or FAIL
bool memory_retrieve_ns(int ns_id, const char *query,
    uint32_t top_k, memory_retrieve_cb_t cb, void *user);

bool memory_retrieve_dossier(int ns_id, int64_t dossier_id,
    const char *query, uint32_t top_k, memory_retrieve_cb_t cb,
    void *user);

// Decay + stats

// Delete facts whose decayed confidence has dropped below
// memory.min_fact_confidence. Rows with source='admin_seed' are skipped.
void memory_decay_sweep(void);

void memory_get_stats(memory_stats_t *out);

// Test hooks (MEMORY_TEST_HOOKS only)

#ifdef MEMORY_TEST_HOOKS

// Test-only: check whether the DB layer is reachable. Tests use this to
// skip gracefully when no Postgres is running.
bool memory_test_db_ok(void);

// Test-only: reset in-memory stats counters to zero.
void memory_test_reset_stats(void);

bool memory_test_inject_embedding(int64_t id, const char *model,
    uint32_t dim, const float *vec);

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
#define MEM_DEF_RECALL_TOP_K             4   // 0 = recall disabled
#define MEM_DEF_RECALL_MIN_COSINE_X100   0   // 0 = no floor
#define MEM_DEF_EMBED_MIN_CHARS          24  // 0 = filter disabled
#define MEM_DEF_EMBED_BATCH_SIZE         32
#define MEM_DEF_EMBED_BURST_MAX          4   // 0 = burst gate disabled
#define MEM_DEF_EMBED_BURST_SECS         5
#define MEM_EXCLUDE_REGEX_SZ             256

// Corpus hygiene: the shipped exclusion pattern, POSIX ERE. Every
// alternative was measured against the live corpus — it drops 43 junk
// vectors (attack class tables, code/diff/pipe pastes) and touches none
// of 1,576 legitimate lines. ⛔ Never add a whitespace-run alternative
// ("   ", "\t", "[ ]{2,}"): it looks like the best junk signal available
// and it would silently eat 25 rows of one human's ordinary speech, who
// double- and triple-spaces after a sentence. See TODO.md
// §POLLUTION-TRUTH 4 — and 5, for why the diff alternative needs a space
// on BOTH sides of the sign.
#define MEM_DEF_EMBED_EXCLUDE_REGEX \
    "^[0-9]+ +[+-] |^[[:space:]]*[{}]|\\||^[a-z_]+ +[0-9]+ +[0-9]+ +[0-9]+ |^class +[a-z]+ +[a-z]+ "

// Minimum content-bearing tokens a line needs to earn a vector.
// Deliberately not a knob — see memory_text_is_embeddable().
#define MEM_EMBED_MIN_TOKENS             2

// Hard ceiling on texts per embed request. Bounds the backfill batch
// struct, which carries its ids and text pointers inline.
#define MEM_EMBED_BATCH_MAX              64

// The recall query's task instruction, and the buffer the instructed
// query is rendered into. Qwen3 embedders are asymmetric: the QUERY
// carries a task instruction, the documents never do. Measured on the
// live corpus, this exact wording puts 14/14 labelled queries at rank 1
// where raw manages 11/14 and misses three short ones entirely.
// Rewording it measurably loses ground — see TODO.md §EMBED-4.
#define MEM_RECALL_INSTRUCT_SZ  256
#define MEM_RECALL_QUERY_SZ     (MEM_MSG_TEXT_SZ + MEM_RECALL_INSTRUCT_SZ + 32)
#define MEM_DEF_RECALL_INSTRUCT \
    "Given a search query, retrieve relevant chat messages"

// Buffer sizes used across helpers.
#define MEM_SQL_SZ      4096
#define MEM_ERR_SZ      256

// MEM_MERGE_OBSERVE's decision table, as SQL. Every fragment references
// COLUMNS only — never an interpolated value — so these are literals the
// upsert pastes into its ON CONFLICT clause, and the three places the
// ladder appears cannot drift apart. Semantics: see mem_merge_t.
#define MEM_SRC_RANK(col) \
    "CASE " col " WHEN 'admin_seed' THEN 3" \
    " WHEN 'user_stated' THEN 2 ELSE 1 END"

#define MEM_FACT_AFFIRM \
    "(EXCLUDED.fact_value = dossier_facts.fact_value)"

#define MEM_FACT_REPLACES \
    "(NOT " MEM_FACT_AFFIRM " AND (" \
      MEM_SRC_RANK("EXCLUDED.source") " > " \
      MEM_SRC_RANK("dossier_facts.source") \
      " OR (" MEM_SRC_RANK("EXCLUDED.source") " = " \
      MEM_SRC_RANK("dossier_facts.source") \
      " AND EXCLUDED.confidence >= dossier_facts.confidence)))"

#define MEM_FACT_UPGRADES \
    "(" MEM_FACT_AFFIRM " AND " \
      MEM_SRC_RANK("EXCLUDED.source") " > " \
      MEM_SRC_RANK("dossier_facts.source") ")"

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
  uint32_t embed_min_chars;
  uint32_t embed_batch_size;
  uint32_t embed_burst_max;
  uint32_t embed_burst_secs;
  char     recall_instruct[MEM_RECALL_INSTRUCT_SZ];
  char     embed_exclude_regex[MEM_EXCLUDE_REGEX_SZ];
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

// The content gate on the embed write path. Shared with memory_backfill.c
// so the live path and the backfill cannot drift apart on what counts as
// worth embedding. See MEMSTORE.md §Embed eligibility.
bool memory_text_is_embeddable(const char *text, uint32_t min_chars);

// The typeable "unset" convention for a KV_STR. `/set kv` cannot store a
// true empty string — the REST argument is required and a quoted "" is
// stored as two literal bytes — so a string knob that must be
// disableable accepts `off` and `none` as well. Shared so every such
// knob answers to the same words.
bool memory_kv_str_disabled(const char *s);

// True when memory.embed_exclude_regex is actually compiled and being
// enforced. A configured-but-uncompilable pattern reads as false — the
// state view must say the gate is off rather than echo a knob nothing
// obeys.
bool memory_exclude_regex_active(void);

// Upsert one vector row. Returns SUCCESS on success.
bool memory_write_embedding(const char *table, const char *id_col, int64_t id,
    const char *model, uint32_t dim, const float *vec);

// Embed backfill (memory_backfill.c). One run at a time, tree-wide.
// memory_backfill_start() returns FAIL and fills `err` when a run is
// already active or the subsystem cannot start one.
bool memory_backfill_start(uint32_t ns_id, char *err, size_t err_sz);
void memory_backfill_status(char *out, size_t out_sz);
void memory_backfill_stop(void);
bool memory_backfill_cmd_register(void);

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
