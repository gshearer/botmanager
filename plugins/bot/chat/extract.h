#ifndef BM_EXTRACT_H
#define BM_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Overview
//
// The extract subsystem turns recent conversation_log rows into
// structured dossier_facts via an LLM call. Entry point is a single
// sweep (extract_run_once) that reads rows past a per-bot high-water
// mark, builds a participants-aware prompt, parses the structured
// response, validates each candidate fact, and writes accepted facts
// through memory_upsert_dossier_fact.
//
// Chunk F-1 (this revision) lands only the plumbing: init/exit, KV
// registration hooks, a stub run_once that returns 0, and stats
// counters. Real extraction (F-2), scheduling + high-water mark (F-3),
// and the admin command + docs (F-4) land in follow-up chunks.
//
// Concurrency: extract_init / extract_exit are single-threaded. All
// other public entry points are safe to call from any task thread
// once extract_init has returned.

// Lifecycle

// Initialize the extract subsystem. Must be called after memory_init
// and llm_init (mirrors dossier_init's ordering). Allocates the stats
// mutex; does not touch the DB.
void extract_init(void);

// Shut down the extract subsystem. Safe to call from shutdown path
// after task workers have been drained.
void extract_exit(void);

// Register any subsystem-level KV defaults and ensure schema. The
// per-bot llm KV keys live in chatbot_inst_schema; this function is
// currently a no-op but is present so the init sequence mirrors
// dossier_register_config and future subsystem-level knobs have a
// defined landing spot.
void extract_register_config(void);

// Sweep

size_t extract_run_once(const char *bot_name, uint32_t ns_id);

// Start / stop a periodic extraction sweep for a bot instance.
// extract_schedule is idempotent: calling it twice for the same bot
// replaces the interval.
void extract_schedule  (const char *bot_name, uint32_t ns_id,
    uint32_t interval_secs);
void extract_unschedule(const char *bot_name);

// Statistics

typedef struct
{
  uint64_t sweeps_total;
  uint64_t sweeps_skipped_rate_limited;
  uint64_t llm_calls;
  uint64_t llm_errors;
  uint64_t facts_written;
  uint64_t facts_rejected_validation;
  uint64_t aliases_written;
  uint64_t aliases_rejected_validation;
} extract_stats_t;

// Snapshot subsystem counters. Zero-fills and populates *out.
void extract_get_stats(extract_stats_t *out);

// Internal surface (extract.c + tests)

#ifdef EXTRACT_INTERNAL

#include "memory.h"

#define EXTRACT_LABEL_SZ       64
#define EXTRACT_PROMPT_MAX_SZ  16384
#define EXTRACT_MAX_FACTS      32
#define EXTRACT_MAX_PARTS      16
#define EXTRACT_ALIAS_MIN_LEN  3
#define EXTRACT_ALIAS_MAX_LEN  32
#define EXTRACT_MAX_ALIASES    8

typedef enum
{
  EXTRACT_ROLE_SENDER    = 0,
  EXTRACT_ROLE_MENTIONED = 1
} extract_role_t;

typedef struct
{
  int64_t         dossier_id;
  char            display_label[EXTRACT_LABEL_SZ];
  extract_role_t  role;
} extract_participant_t;

typedef struct
{
  int64_t dossier_id;
  char    alias[EXTRACT_ALIAS_MAX_LEN + 1];
  float   confidence;
} extract_alias_t;

size_t extract_prompt_build(const extract_participant_t *parts, size_t n_parts,
    const mem_msg_t *msgs, size_t n_msgs,
    char *out, size_t out_sz);

// System prompt text (stable). Returned pointer is interned; caller must
// not free or mutate.
const char *extract_prompt_system(void);

size_t extract_parse_response(const char *content, size_t content_len,
    const extract_participant_t *parts, size_t n_parts,
    float min_conf, const char *channel,
    mem_dossier_fact_t *out, size_t out_cap);

// Parse aliases from the same LLM response body. On a response that
// lacks the aliases array entirely (older model / older prompt),
// returns 0 without error. Validates dossier_id-in-participants,
// alphanumeric format, length bounds, and confidence >= min_conf.
// Entries failing form checks are silently skipped (the dispatch
// layer runs DB-backed validation and bumps the counter there).
//
// returns: number of aliases written to out (0..out_cap)
size_t extract_parse_aliases(const char *content, size_t content_len,
    const extract_participant_t *parts, size_t n_parts,
    float min_conf,
    extract_alias_t *out, size_t out_cap);

// Synchronous single-batch dispatch. Builds the prompt, calls
// llm_chat_submit (blocking until done_cb fires), parses the response,
// and upserts accepted facts via memory_upsert_dossier_fact with
// MEM_MERGE_HIGHER_CONF. Bumps llm_calls / llm_errors / facts_written.
//
// returns: number of facts written (0 on no-op, error, or all rejected)
// model_name: registered chat model name (must be non-empty)
// parts / n_parts, msgs / n_msgs: batch
// channel: stamped on accepted facts
// min_conf: validation threshold
// timeout_secs: cap on the blocking wait; 0 -> 60s default
// Pull the next batch of conversation_log rows past the given high-
// water mark for a bot/namespace and assemble the participants list.
// Caller-owned buffers: msgs_out[msgs_cap], parts_out[parts_cap].
// Sets *hwm_out to the largest row id observed (caller uses this as
// the next hwm) or leaves it unchanged on empty batch.
//
// returns: number of messages written to msgs_out (0 on empty/error)
size_t extract_fetch_batch(const char *bot_name, uint32_t ns_id,
    int64_t hwm_in, uint32_t batch_cap,
    mem_msg_t *msgs_out, size_t msgs_cap,
    extract_participant_t *parts_out, size_t parts_cap,
    size_t *n_parts_out, int64_t *hwm_out);

size_t extract_dispatch(const char *bot_name, uint32_t ns_id,
    const char *model_name,
    const extract_participant_t *parts, size_t n_parts,
    const mem_msg_t *msgs, size_t n_msgs,
    const char *channel, float min_conf, uint32_t timeout_secs);

#endif // EXTRACT_INTERNAL

#endif // BM_EXTRACT_H
