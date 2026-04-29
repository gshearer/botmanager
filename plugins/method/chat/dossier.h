#ifndef BM_DOSSIER_H
#define BM_DOSSIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Overview
//
// The dossier subsystem is the chat bot's single source of truth for
// participant memory. Each dossier represents one real person observed
// in chat, identified by the generic four-field identity tuple every
// protocol plugin emits per inbound message (nickname, username,
// hostname, verified_id). The protocol owns how it projects its native
// identity onto those fields — see plugins/protocol/<kind>/AGENTS.md
// for the per-protocol contract — and the chat plugin owns scoring.
//
// A dossier optionally links to a registered userns_user via user_id;
// when set, the dossier is the canonical identity for that user. Until
// a registration happens, dossiers are anonymous and may later be
// consolidated into a user via dossier_merge().
//
// Matching model
//
// dossier_resolve() walks every stored signature of the given
// method_kind in the given namespace, calls chat_identity_score()
// (identity.h) against the inbound signature, and picks the
// highest-scoring dossier whose confidence clears
// DOSSIER_MATCH_THRESHOLD. On match, last_seen and message_count are
// bumped, and the matched signature's seen_count is bumped (or a new
// signature row is inserted if this exact tuple has not been observed
// for that dossier yet). On miss -- and only when create_if_missing
// is true -- a new dossier and its first signature row are created
// and the fresh dossier_id is returned.
//
// Thresholds

// Minimum scorer output for two signatures to be considered the same
// dossier. Compile-time constant for v1; may become per-method-
// configurable after live observation in Chunks B-D.
#define DOSSIER_MATCH_THRESHOLD  0.75f

// Minimum token-vs-signature score for a token in a chat line to be
// treated as a mention of a dossier. Slightly lower than the sender
// threshold since token context is thinner.
#define DOSSIER_MENTION_THRESHOLD  0.7f

// Bounds. method_kind tracks the originating protocol plugin; the
// quad-tuple field bounds match METHOD_*_SZ in include/method.h.
#define DOSSIER_METHOD_KIND_SZ  64
#define DOSSIER_LABEL_SZ        128

// Public types

// A dossier identifier. 0 indicates "no dossier" / "not resolved".
typedef int64_t dossier_id_t;

// A single protocol-level identity signature. Mirrors the four-field
// tuple on method_msg_t (nickname, username, hostname, verified_id)
// plus the originating method_kind. All char* fields are borrowed
// references -- the dossier layer copies what it needs.
typedef struct dossier_sig
{
  const char *method_kind;
  const char *nickname;
  const char *username;
  const char *hostname;
  const char *verified_id;
} dossier_sig_t;

typedef bool (*dossier_sig_filter_t)(const dossier_sig_t *sig, void *user);

// Summary row returned by dossier_get(). Facts are retrieved via the
// memory_retrieve_dossier() API, not duplicated here.
#define DOSSIER_INFO_LABEL_SZ  DOSSIER_LABEL_SZ

typedef struct
{
  dossier_id_t id;
  uint32_t     ns_id;
  int          user_id_or_0;             // 0 = anonymous
  char         display_label[DOSSIER_INFO_LABEL_SZ];
  time_t       first_seen;
  time_t       last_seen;
  uint32_t     message_count;
} dossier_info_t;

// Subsystem statistics snapshot.
typedef struct
{
  uint64_t  resolves;      // lifetime dossier_resolve() calls
  uint64_t  creates;       // dossiers created (resolve miss + create)
  uint64_t  sightings;     // sightings recorded (matches + new rows)
  uint64_t  merges;        // dossiers merged (absorbed rows)
  uint64_t  scorer_calls;  // total scorer invocations
} dossier_stats_t;

// Lifecycle

// Initialize the dossier subsystem. Must be called after mem_init();
// allocates the scorer registry and stat mutex. DB is not touched here.
void dossier_init(void);

// Ensure the dossier/dossier_signature/dossier_facts tables and their
// indexes exist. Must be called after userns_init() (FK dependency).
void dossier_register_config(void);

// Shut down the subsystem. Does not touch DB. Safe to call multiple times.
void dossier_exit(void);

// Core operations

// Resolve an inbound signature to a dossier_id within a namespace.
//
// If a dossier in the namespace has any signature of the same
// method_kind that scores >= DOSSIER_MATCH_THRESHOLD against sig, the
// match's dossier_id is returned and its last_seen / message_count
// are bumped (and the signature row's seen_count, or a fresh row is
// inserted when the exact identity tuple has not been observed for
// that dossier yet).
//
// If no match clears the threshold and create_if_missing is true, a
// new dossier is created (with its initial signature row) and the new
// id is returned. Returns 0 on miss (create_if_missing=false) or on
// any error. `display_label` may be "".
dossier_id_t dossier_resolve(uint32_t ns_id, const dossier_sig_t *sig,
    const char *display_label, bool create_if_missing);

// Attach a sighting to an existing dossier explicitly, bypassing the
// scorer. Used by callers that already know which dossier applies
// (e.g., IRC NICK events in Chunk E). Upserts the signature row and
// bumps last_seen / message_count on the dossier.
bool dossier_record_sighting(dossier_id_t dossier_id,
    const dossier_sig_t *sig);

// Link or unlink a dossier to a registered user. Pass user_id=0 to
// clear the link (leaves the dossier as anonymous but retains its
// facts and signatures).
bool dossier_set_user(dossier_id_t dossier_id, int user_id);

// Collapse absorbed dossiers into the survivor: all signatures and
// facts are reassigned, message_count is summed, first_seen/last_seen
// are unified, and absorbed rows are deleted. Absorbed rows must be
// in the same namespace as the survivor (and must not include the
// survivor itself). Deduplicates on (dossier_id, method_kind, nickname,
// username, hostname, verified_id) and (dossier_id, kind, fact_key)
// via ON CONFLICT during the reassignment.
bool dossier_merge(dossier_id_t survivor_id,
    const dossier_id_t *absorbed_ids, size_t n_absorbed);

bool dossier_get(dossier_id_t dossier_id, dossier_info_t *out);

// Detach one signature row from its current dossier into a brand-new
// dossier in the same namespace. The new dossier inherits the source
// dossier's display_label; user_id is not propagated (the split is
// intended to un-merge, and attachment is an explicit admin action).
//
// Best-effort only: existing facts are NOT migrated automatically --
// the `dossier_facts` table has no source-signature provenance, so
// facts synthesized from prior sightings stay with the original
// dossier. Admins who need to move specific facts can do so via
// follow-up `memory_*_dossier_fact` calls.
//
// Returns FAIL if signature_id is not found, or if the source dossier
// would be left with zero signatures.
bool dossier_split_signature(int64_t signature_id,
    dossier_id_t *out_new_id);

// Walk signature rows in a namespace, invoking filter(). Any dossier
// with at least one signature for which filter returns true is
// appended to out_ids (deduplicated, insertion-order preserved).
// Returns the number of dossier_ids written.
//
// Used by the /dossier candidates admin command: the caller supplies
// a filter that matches signature JSON against a user's MFA patterns.
// Tests may supply arbitrary filters.
size_t dossier_find_candidates(uint32_t ns_id,
    dossier_sig_filter_t filter, void *user,
    dossier_id_t *out_ids, size_t max);

// Snapshot subsystem statistics.
void dossier_get_stats(dossier_stats_t *out);

// Register dossier display commands (/show dossier, /show dossier stats).
void dossier_show_register_commands(void);

// Register the /show dossiers container + /show dossiers candidates.
// Exposed so dossier_register_commands() (in dossier_cmd.c) can wire the
// candidates read path next to the admin mutators.
void dossier_show_register_candidates(void);

// Register the /dossier command tree (merge, split, fact set, fact del)
// and the /show dossiers candidates <bot> <user> read path. Called once
// during startup after dossier_init() and cmd_init().
void dossier_register_commands(void);

// Mention resolution

// Find dossiers mentioned by name in a chat line. Tokenizes `text` on
// whitespace and punctuation, keeps alphanumeric tokens of length >= 3
// that are not in a small built-in English stoplist, and scores each
// token against candidate dossiers' signatures via the registered
// token scorer for `method_kind`.
//
// Candidate dossiers are those seen anywhere in `ns_id` within the
// last `window_secs` seconds according to `conversation_log`. The
// namespace is the identity boundary -- users are scoped to the
// userns, so dossiers are too; a bot sharing a userns knows any
// dossier recently active anywhere in it. When `window_secs` is 0,
// no time bound is applied.
//
// Results are deduplicated; insertion order preserved in `out_ids`.
// Short ambiguous tokens (e.g. "Jo" matching Joe AND Johnson) may
// return multiple candidates -- downstream disambiguation (typically
// by an LLM extractor) is the caller's responsibility.
//
// returns: number of dossier_ids written to out_ids (0 on error or
//          no scorer registered for method_kind)
size_t dossier_find_mentions(uint32_t ns_id, const char *method_kind,
    uint32_t window_secs, const char *text,
    dossier_id_t *out_ids, size_t max);

// Test hooks (DOSSIER_TEST_HOOKS only)

#ifdef DOSSIER_TEST_HOOKS

// Delete a dossier (and everything that cascades). Intended for test
// teardown; production code uses namespace/user deletion to cascade.
//
// returns: SUCCESS or FAIL
bool dossier_test_delete(dossier_id_t dossier_id);

// Reset in-memory statistics counters. DB state is untouched.
void dossier_test_reset_stats(void);

// Probe DB availability. Tests use this to skip gracefully when no
// Postgres is reachable.
bool dossier_test_db_ok(void);

#endif // DOSSIER_TEST_HOOKS

// Internal declarations

#ifdef DOSSIER_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "alloc.h"

// Local SQL buffer caps. Long enough for any signature-JSON-bearing
// statement this module issues; callers must not exceed these.
#define DOSSIER_SQL_SZ       4096
#define DOSSIER_JSON_MAX_SZ  2048

#endif // DOSSIER_INTERNAL

#endif // BM_DOSSIER_H
