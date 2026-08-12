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

// Cancel every scheduled per-bot sweep. Must run before extract_exit()
// on the unload path: exit() frees the sweep state each armed task
// holds a pointer to.
void extract_stop(void);

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

// The assembled system prompt (head + rendered vocabulary + tail).
// ~5 KiB as shipped; the builder WARNs rather than truncating quietly.
#define EXTRACT_SYSTEM_PROMPT_SZ 8192
#define EXTRACT_MAX_FACTS      32
#define EXTRACT_MAX_PARTS      16
#define EXTRACT_ALIAS_MIN_LEN  3
#define EXTRACT_ALIAS_MAX_LEN  32
#define EXTRACT_MAX_ALIASES    8

// Ceiling on the confidence a machine-read fact may claim, whatever the
// model asserts (FACT-1/F2). It buys one guarantee the merge ladder
// relies on: a stored 1.0 came from a human, so a person correcting
// themselves through /remember or /dossier always outranks the sweep's
// reading of what they said. Aliases are deliberately NOT clamped —
// they carry no value to contradict.
#define EXTRACT_MAX_FACT_CONF  0.95f

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

  // Channel provenance for facts about this participant, resolved by
  // extract_parts_assemble over exactly the rows it was handed: the
  // newest channel they spoke in, or the DM-guard sentinel "" when any
  // of their rows arrived by DM (privacy-first — a fact that MIGHT
  // derive from a DM is treated as if it did). A mentioned-only
  // participant said nothing themselves, so their facts inherit the
  // slice-wide resolution under the same rule. extract_run_once
  // partitions each sweep batch by channel before assembly
  // (CHAT-EXTRACT-PARTITION-1), so the rule degenerates to "the
  // partition's channel" and provenance is exact; over a mixed-channel
  // slice it stays the conservative safety net CHAT-EXTRACT-DMCHAN-1
  // introduced — stamping every fact with one batch-level channel had
  // let DM secrets masquerade as channel knowledge.
  char            channel[MEM_FACT_CHANNEL_SZ];
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

// Assemble the system prompt from the canonical fact vocabulary. Called
// once by extract_init, before any sweep can run; not thread-safe, and
// does not need to be.
void extract_prompt_system_init(void);

// System prompt text (stable after extract_prompt_system_init).
// Returned pointer is interned; caller must not free or mutate.
const char *extract_prompt_system(void);

// Each accepted fact is stamped with its subject participant's resolved
// channel (see extract_participant_t.channel), never a batch-level one.
size_t extract_parse_response(const char *content, size_t content_len,
    const extract_participant_t *parts, size_t n_parts,
    float min_conf,
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

// Pull the next batch of conversation_log rows past the given high-
// water mark for a bot/namespace into the caller-owned msgs_out
// [msgs_cap]. Sets *hwm_out to the largest row id observed (caller
// uses this as the next hwm) or leaves it unchanged on empty batch.
// Participants are deliberately NOT assembled here: extract_run_once
// partitions the batch by channel first and assembles per partition.
//
// returns: number of messages written to msgs_out (0 on empty/error)
size_t extract_fetch_batch(const char *bot_name, uint32_t ns_id,
    int64_t hwm_in, uint32_t batch_cap,
    mem_msg_t *msgs_out, size_t msgs_cap,
    int64_t *hwm_out);

// Assemble the participants list for exactly the rows given: unique
// senders + every referenced dossier (tagged sender/mentioned, first
// sighting wins the role), display labels, and per-subject channel
// provenance (see extract_participant_t.channel for the rule).
//
// returns: number of participants written to parts_out (0..parts_cap)
size_t extract_parts_assemble(const mem_msg_t *msgs, size_t n_msgs,
    extract_participant_t *parts_out, size_t parts_cap);

// Synchronous single-partition dispatch. Builds the prompt, calls
// llm_chat_submit (blocking until done_cb fires), parses the response,
// and upserts accepted facts via memory_upsert_dossier_fact with
// MEM_MERGE_OBSERVE — the sweep is a rank-1 source, so it fills gaps
// and affirms, and can never overwrite what a person stated. Bumps
// llm_calls / llm_errors / facts_written.
//
// returns: number of facts written (0 on no-op, error, or all rejected)
// model_name: registered chat model name (must be non-empty)
// parts / n_parts, msgs / n_msgs: one partition (parts carry channel
//   provenance)
// min_conf: validation threshold
// timeout_secs: cap on the blocking wait; 0 -> 60s default
size_t extract_dispatch(const char *bot_name, uint32_t ns_id,
    const char *model_name,
    const extract_participant_t *parts, size_t n_parts,
    const mem_msg_t *msgs, size_t n_msgs,
    float min_conf, uint32_t timeout_secs);

#endif // EXTRACT_INTERNAL

#endif // BM_EXTRACT_H
