// chatbot.h — conversational half of the chat bot plugin (kind: chat)
//
// The LLM-driven personality half: bot driver vtable, personality table
// loader, /llm personality subcommands, and the observe path that
// classifies incoming lines as WITNESS or EXCHANGE_IN, forwards them to
// memory_log_message(), and runs the speak policy / reply pipeline.
//
// It is one half of a two-half plugin. The other — always-on command
// dispatch plus the identity/auth surface — is dispatch.h. The driver
// vtable declared here (chatbot_driver) fronts both; see the deliver
// path comment above chatbot_on_message in chatbot.c for the ordering
// and for why the command half short-circuits this one.

#ifndef BM_CHATBOT_H
#define BM_CHATBOT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// The two directories the personality loader reads, declared here so
// chatbot_plugin_init()'s kv_register and personality.c's resolvers say
// the same thing once. Neither key has a meaningful empty value — an
// empty directory resolves against the daemon's working directory and
// the catalogue comes back silently empty — so `set kv --clear` on
// either is refused at the read site and lands back here.
#define CHATBOT_PERSONALITY_DIR "./personalities"
#define CHATBOT_CONTRACT_DIR    "./personalities/contracts"

// Header-only view of a personality file (frontmatter parse only).
// Used by /show personalities to render a catalogue row without
// slurping the body or the contract. `ok == false` means the parse
// failed; `err` carries a short explanation suitable for rendering in
// red. `bytes` is the filesystem size of the personality file.
typedef struct
{
  char  name[64];
  char  version[32];
  char  description[256];
  off_t bytes;
  bool  ok;
  char  err[128];
} persona_header_t;

// Forward — full-record typedef lives inside CHATBOT_INTERNAL below.
struct chatbot_personality_s;

// Read a full personality from disk. Resolves
// <bot.chat.personalitypath>/<name>.txt; parses frontmatter + body;
// populates *out. On SUCCESS the caller owns the heap-alloc'd fields
// and must release via chatbot_personality_free. `name` is the persona
// filename stem (no .txt extension).
bool chatbot_personality_read(const char *name,
    struct chatbot_personality_s *out);

// Read only the frontmatter header of <name>.txt. Cheaper than
// chatbot_personality_read for listing commands — the body is not
// slurped. On parse failure sets hdr->ok = false with hdr->err
// populated; the return value is still SUCCESS so the caller can
// render a "broken" row without aborting the whole listing.
bool chatbot_personality_read_header(const char *name,
    persona_header_t *hdr);

typedef void (*chatbot_personality_visit_cb)(const char *stem, void *data);

size_t chatbot_personality_scan(chatbot_personality_visit_cb cb,
    void *data);

// Read the output contract body at <bot.chat.contractpath>/<name>.txt.
// Skips any leading frontmatter; body is returned as a mem_alloc'd
// NUL-terminated string which the caller must mem_free. `name` is the
// contract filename stem (no .txt extension; no path separators).
bool chatbot_contract_read(const char *name, char **out_body);

// Resolve the configured contract directory into out_path. Public
// helper shared between personality.c and contract_show.c. Always
// succeeds (fallback: CHATBOT_CONTRACT_DIR).
bool chatbot_contract_path(char *out_path, size_t sz);

// Read only the frontmatter header of a contract file. Populates
// hdr->name and hdr->description. hdr->version / hdr->interests are
// unused for contracts; the shared persona_header_t is reused
// because the frontmatter grammar is identical. On parse failure,
// hdr->ok is false and hdr->err carries a short explanation.
bool chatbot_contract_read_header(const char *name, persona_header_t *hdr);

// Walk bot.chat.contractpath and invoke cb() once per *.txt stem.
size_t chatbot_contract_scan(chatbot_personality_visit_cb cb, void *data);

// Release heap-alloc'd fields inside *p. Safe on zero-initialised or
// partially-populated structures; idempotent (zeroes the freed slots).
void chatbot_personality_free(struct chatbot_personality_s *p);

#ifdef CHATBOT_INTERNAL

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inference.h"
#include "bot.h"
#include "cmd.h"
#include "common.h"
#include "cooldown.h"
#include "hold.h"
#include "extract.h"
#include "kv.h"
#include "alloc.h"
#include "memory.h"
#include "method.h"
#include "dossier.h"
#include "identity.h"
#include "plugin.h"
#include "task.h"
#include "userns.h"

#define CHATBOT_PERSONALITY_NAME_SZ   64
#define CHATBOT_PERSONALITY_DESC_SZ   200
#define CHATBOT_PERSONALITY_BODY_SZ   (16 * 1024)
#define CHATBOT_PERSONALITY_PATH_SZ   512

// Opaque blob storing the personality's `interests:` frontmatter section
// as JSON. Parsed lazily by chatbot_interests_parse() at bot start; empty
// string means "no topics declared". 16 KiB is plenty for the MVP shape
// (a handful of topics with modest keyword / allowlist arrays).
#define CHATBOT_INTERESTS_JSON_SZ     (16 * 1024)

// Semicolon-separated list of knowledge-corpus names the bot retrieves
// from. Read from `bot.<name>.corpus` KV (not from the persona —
// corpus selection is a deployment decision, not a character trait).
#define CHATBOT_CORPUS_LIST_SZ        256

// Byte budget for the rendered NL COMMANDS block in the system prompt.
// Caps the combined size of every per-command stanza so a bot with many
// NL-capable commands cannot starve the FACTS / MENTIONS / KNOWLEDGE
// sections. On overflow the builder truncates at a whole-command
// boundary and appends "… (more available)\n".
//
// ⚠⚠ Raised from 4096 by CARE-7, and the reason is the thing to
// remember: the block is sorted by command name, so an overflow does
// not degrade the vocabulary evenly — it deletes the alphabetical
// tail. At 4096 the tree assembled 4025 bytes with room for nothing
// more, and adding /pricewatch pushed /remind off the end, after which
// "remind me in 20 minutes to flip the steaks" routed to /in and was
// refused. The command still existed everywhere except in the only
// place the model could see it. The builder now says so out loud when
// it truncates (reply.c) — treat that WARN as a build-time error in a
// dressing gown, and raise this number rather than reword a `.when`.
//
// Measured 2026-08-12 against a bot whose nl_bridge_cmds is `*`, the
// worst case: the whole vocabulary renders between 8,192 and 12,288
// bytes. 16 KiB is that plus room for the next few commands, and a bot
// with a narrower allowlist never pays for what it does not offer.
#define CHATBOT_NL_COMMANDS_MAX_BYTES  16384

// Personality record (in-memory copy of a personalities table row).
//
// `body` is the persona-shaping content. The output contract is loaded
// separately via chatbot_contract_read() from a per-bot KV
// (`bot.<name>.behavior.contract`) and does not live on this record.
typedef struct chatbot_personality_s
{
  char    name[CHATBOT_PERSONALITY_NAME_SZ];
  char    description[CHATBOT_PERSONALITY_DESC_SZ];
  char   *body;            // mem_alloc'd
  char   *interests_json;  // mem_alloc'd; "" when absent
  int     version;
  time_t  updated;
} chatbot_personality_t;

// Tiny per-channel reply cooldown ring — a cooldown.h slot table, so
// its matching, eviction and reload floor are that module's (this one
// used to run a FIFO write cursor under a comment claiming LRU). Keeps
// speak policy fully in-memory without a DB round trip; one slot per
// channel or DM sender, whichever the line arrived on.
#define CHATBOT_COOLDOWN_SLOTS 16

// Paste coalescing: pending per-sender buffer. A new line from the same
// (method, sender) appends to this buffer and bumps `seq`; a deferred
// task scheduled at enqueue time flushes the block once the sender goes
// idle for coalesce_ms. If a newer line arrives first, the task finds
// seq != snapshot and no-ops (the newer schedule wins).
#define CHATBOT_COALESCE_SLOTS    16
#define CHATBOT_COALESCE_TEXT_SZ  (8 * 1024)
#define CHATBOT_COALESCE_MAX_LINES 64

// The flush task's closure (defined in chatbot.c). A slot keeps the
// live one so chatbot_coalesce_shutdown() can cancel the task and
// reclaim the closure the task would otherwise have freed itself.
typedef struct chatbot_coalesce_flush chatbot_coalesce_flush_t;

typedef struct
{
  bool            in_use;
  method_inst_t  *method;
  char            sender[METHOD_SENDER_SZ];
  char            metadata[METHOD_META_SZ];
  char            channel[METHOD_CHANNEL_SZ];
  // Carried so the flush can rebuild a message
  // chatbot_resolve_dossier() will actually resolve — see
  // identity.h §chat_identity_t for what a partial copy costs.
  chat_identity_t who;
  char            text[CHATBOT_COALESCE_TEXT_SZ];
  size_t          text_len;
  bool            was_addressed;   // sticky: any line in the block classified as EXCHANGE_IN
  // CV-7: true only when at least one line appended to this slot was
  // classified as CHATBOT_CLASSIFY_DIRECT (nick literally in line or DM).
  // Distinct from was_addressed which is set for both DIRECT and STICKY
  // promotions — preserved so the flush callback can gate the CV-4
  // direct-address SKIP fallback correctly.
  bool            any_direct;
  bool            is_action;       // first-line-wins: the slot's opening line was an IRC /me action
  bool            truncated;       // set when the block exceeded buffer/line caps
  uint32_t        lines;
  uint32_t        seq;             // bumped on each append; flush task checks for staleness
  time_t          first_ts;

  // The one flush task this slot currently owns, and the closure it
  // will free when it runs. Written under coalesce_mutex; both are the
  // slot's answer to "who is scheduled against me right now", which is
  // what lets teardown cancel it instead of racing it. An append
  // supersedes its predecessor, so at most one task per slot is ever
  // cancellable; a task that lost that race is stale, no-ops on `seq`
  // and retires itself.
  task_handle_t   flush;
  chatbot_coalesce_flush_t *flush_arg;
} chatbot_coalesce_slot_t;

// CV-7 Part B — per-(method, target) anti-repeat ring for the CV-4
// direct-address SKIP fallback. When a directly-addressed turn produces
// only SKIP-suppressed lines, reply.c emits
// CHATBOT_DIRECT_FALLBACK_TEXT via method_send. That path bypasses
// CV-6's anti-repeat because the bytes never flow through the LLM; this
// ring adds its own cooldown so the same canned string cannot spam the
// channel when the LLM keeps SKIPping within a short window.
#define CHATBOT_CV4_FALLBACK_SLOTS 8

typedef struct
{
  method_inst_t  *method;               // NULL = slot unused
  char            target[METHOD_CHANNEL_SZ];
  time_t          last_emit;
} cv4_fallback_ring_slot_t;

// Cap on the cached reactive-topic array carried on chatbot_state_t.
// Parsed once at bot start (and refreshed on personality switch),
// consulted on every inbound message. Matching the personality-loader
// topic cap keeps the per-bot footprint bounded at ~16 KiB.
#define CHATBOT_TOPIC_CACHE_MAX 32

// The names a bot answers to, resolved per inbound line: the nick the
// method knows it by, plus the short forms an operator listed in
// `bot.<name>.aka` ("hh" for hedgehogg). Per-line, like every other
// per-bot knob — a nick change or a KV edit lands on the next message.
//
// The nick and the short names are held apart on purpose, because they
// are not addressed the same way. A nick is a distinctive token and is
// recognised bare, anywhere in a line; a short name is two letters and
// counts only where it is unmistakably a summons — at the head of a
// line, or wearing an address mark ("@hh", "hey hh, ..."). See
// `chatbot_classify_message`.
//
// A short name longer than CHATBOT_AKA_SZ - 1 is dropped rather than
// truncated: a truncated name would match every longer word that starts
// with it, which is the one failure mode this whole type exists to
// avoid.
#define CHATBOT_AKA_MAX 8
#define CHATBOT_AKA_SZ  32

typedef struct
{
  char   nick[METHOD_SENDER_SZ];    // empty when the method has no name for us
  char   aka[CHATBOT_AKA_MAX][CHATBOT_AKA_SZ];
  size_t n_aka;
} chatbot_names_t;

// Sticky engagement ring: tracks the most recent (channel, user) pairs
// the bot has exchanged with so follow-up messages from the same user
// in the same channel can be promoted from WITNESS to EXCHANGE_IN
// without re-addressing the bot. Pure LRU, fixed-size — 32 slots is
// ample for even a chatty channel and keeps the footprint bounded at
// ~16 KiB regardless of message volume.
#define CHATBOT_ENGAGEMENT_SLOTS 32

typedef struct
{
  char    channel[METHOD_CHANNEL_SZ];   // empty = slot unused
  char    user[METHOD_SENDER_SZ];
  time_t  last_seen;                    // 0 = slot cleared (e.g. handoff)
} chatbot_engagement_slot_t;

typedef struct
{
  chatbot_engagement_slot_t slots[CHATBOT_ENGAGEMENT_SLOTS];
  pthread_mutex_t          mutex;
} chatbot_engagement_t;

// CV-12 — per-channel handoff ring. Records the last non-bot speaker on
// each channel so the sticky-engagement classifier can tell "the user
// is continuing their conversation with the bot" from "the user is
// answering another participant in the same channel". A STICKY
// promotion that fires while the prior non-bot speaker was a different
// user is almost always a two-friends-talking line — demote to WITNESS.
// See finding_sticky_engagement_answers_others.md for the Run-7
// observation that motivated this gate.
#define CHATBOT_HANDOFF_SLOTS                  8

typedef struct
{
  char    channel[METHOD_CHANNEL_SZ];   // empty = slot unused
  char    user[METHOD_SENDER_SZ];       // most recent non-bot speaker
  time_t  last_seen;                    // 0 = slot cleared
} chatbot_handoff_slot_t;

typedef struct
{
  chatbot_handoff_slot_t slots[CHATBOT_HANDOFF_SLOTS];
  pthread_mutex_t        mutex;
} chatbot_handoff_t;

// VF-3 — per-target witness-interject cooldown ring. Caps the rate at
// which WITNESS-driven interjects fire on a given channel (or DM
// sender) regardless of the probability roll. Direct-address replies
// do not consume this budget, so the ring is stamped only from the
// INTERJECT branch in chatbot_consider_speaking. A cooldown.h slot
// table keyed on the target; 8 slots is sufficient for a bot joined to
// a handful of them.
#define CHATBOT_WITNESS_COOLDOWN_SLOTS 8

typedef struct
{
  cooldown_slot_t slots[CHATBOT_WITNESS_COOLDOWN_SLOTS];
  pthread_mutex_t mutex;
} chatbot_witness_cooldown_t;

// V1 — per-channel volunteer cooldown ring + per-bot hourly cap.
// Pure LRU, fixed-size — a bot rarely joins more than a handful of
// channels, so 16 slots is ample. `last_attempted` holds SKIP timestamps
// so a persona that vetos a subject doesn't retry instantly.
#define CHATBOT_VOLUNTEER_CHAN_SLOTS 16

// V2 — content-level dedup rings. Each ring is a pure LRU on its
// `volunteered_at` stamp; oldest slot evicts on write. All three live
// on chatbot_volunteer_state_t and share st->volunteer.mutex with the
// V1 channel ring. The embed ring's per-slot float array dominates
// the per-bot state footprint (~128 KiB at the default caps); raise
// CHATBOT_VOLUNTEER_EMBED_DIM_MAX only if a newer embed model exceeds
// qwen3-vl-embed's 2048-dim output.
#define CHATBOT_VOLUNTEER_CHUNK_RING    32
#define CHATBOT_VOLUNTEER_SUBJECT_RING  32
#define CHATBOT_VOLUNTEER_EMBED_RING    16
#define CHATBOT_VOLUNTEER_EMBED_DIM_MAX 2048

typedef struct
{
  char    channel[METHOD_CHANNEL_SZ];   // empty = slot unused
  time_t  last_volunteered;             // last successful post
  time_t  last_attempted;               // last compose attempt (SKIP or send)
} chatbot_volunteer_slot_t;

typedef struct
{
  int64_t  chunk_id;                    // 0 = empty
  time_t   volunteered_at;              // LRU eviction key
} chatbot_volunteer_chunk_slot_t;

typedef struct
{
  int64_t  chunk_id;                    // 0 = empty
  time_t   volunteered_at;
  uint32_t dim;                         // 0 = empty
  float    vec[CHATBOT_VOLUNTEER_EMBED_DIM_MAX];
} chatbot_volunteer_embed_slot_t;

typedef struct
{
  chatbot_volunteer_slot_t         slots[CHATBOT_VOLUNTEER_CHAN_SLOTS];
  uint32_t                        per_hour_counter;
  time_t                          per_hour_window_start;
  pthread_mutex_t                 mutex;

  // V2 — content dedup rings. See the block comment above.
  chatbot_volunteer_chunk_slot_t   chunk_ring [CHATBOT_VOLUNTEER_CHUNK_RING];
  cooldown_slot_t                  subj_ring  [CHATBOT_VOLUNTEER_SUBJECT_RING];
  chatbot_volunteer_embed_slot_t   embed_ring [CHATBOT_VOLUNTEER_EMBED_RING];
} chatbot_volunteer_state_t;

// IV3 vision cooldowns — the same cooldown.h slot table as cooldowns[]
// but kept separate so vision accounting runs under its own mutex and
// can't starve reply accounting.
#define CHATBOT_VISION_CD_SLOTS 16

typedef struct
{
  cooldown_slot_t slots[CHATBOT_VISION_CD_SLOTS];
  pthread_mutex_t mutex;
} chatbot_vision_cd_t;

// Per-instance bot state.
typedef struct
{
  bot_inst_t       *inst;             // back-pointer to bot instance
  pthread_rwlock_t  lock;             // guards active_name + cooldowns + topic cache
  char              active_name[CHATBOT_PERSONALITY_NAME_SZ];

  // TEXT-COOLDOWN-1 — wall-clock stamp taken in chatbot_create().
  // Every speak gate floors its "last spoke" timestamp to this value,
  // so a handle created by a /plugin reload starts one cooldown window
  // quiet instead of reading as "never spoke" (which the gates take as
  // free to speak). Write-once at create; read without a lock.
  time_t            created_at;

  // Cached reactive-topic list. Populated in chatbot_register_interests;
  // read by chatbot_observe on every inbound line under the rdlock.
  // Empty when the active personality declares no interests.
  acquire_topic_t   topics[CHATBOT_TOPIC_CACHE_MAX];
  size_t            n_topics;

  // Snapshot of the persona name currently reflected in `topics[]`.
  // Empty string = cache not populated yet. A mismatch against
  // `active_name` means the next hot-path message must re-register.
  char              registered_persona[CHATBOT_PERSONALITY_NAME_SZ];

  uint32_t          in_flight;        // in-flight reply requests
  pthread_mutex_t   flight_mutex;

  cooldown_slot_t         cooldowns[CHATBOT_COOLDOWN_SLOTS];

  // SAN-21: the coalescer is the one path that hands work to a task
  // against this handle, so it is the one path core's delivery refcount
  // cannot cover — the handle is destroyed at *suspend* time, long
  // before plugin_unload polls quiesce. `pending` counts flush tasks
  // scheduled and not yet retired; `down` refuses new ones and tells a
  // fire already past the gate to drop its block on the floor.
  // chatbot_coalesce_shutdown() waits on `idle` for the count to reach
  // zero, which is the join PLUGIN.md §Lifecycle Contract asks stop()
  // for.
  pthread_mutex_t        coalesce_mutex;
  pthread_cond_t         coalesce_idle;
  uint32_t               coalesce_pending;
  bool                   coalesce_down;
  chatbot_coalesce_slot_t coalesce[CHATBOT_COALESCE_SLOTS];

  // CV-7 Part B — CV-4 direct-address SKIP fallback anti-repeat ring.
  // Guarded by the existing st->lock (wrlock around peek+stamp only;
  // never held across method_send).
  cv4_fallback_ring_slot_t cv4_ring[CHATBOT_CV4_FALLBACK_SLOTS];

  chatbot_engagement_t    engagement;

  // CV-12 — per-channel "last non-bot speaker" ring. Consulted by the
  // sticky-engagement classifier to demote STICKY promotions back to
  // WITNESS when the channel's prior non-bot speaker was a different
  // user within the handoff window.
  chatbot_handoff_t       handoff;

  // V1 — volunteer speech state. Per-channel last-volunteered ring +
  // hourly rate-limit counter. 16 slots is enough for a bot joined to
  // a handful of channels; beyond that, eviction is LRU on write.
  chatbot_volunteer_state_t volunteer;

  // VF-3 — per-target witness-interject cooldown ring. Dedicated
  // mutex (mirrors the volunteer ring) keeps the hot-path decide gate
  // off st->lock. Stamped only when a WITNESS interject actually
  // submits; direct-address replies do not consume the budget.
  chatbot_witness_cooldown_t witness_cd;

  // IV3 vision cooldowns. Per-channel ring throttles vision replies;
  // per-URL ring deduplicates the same image re-linked by different
  // users. Shape mirrors cooldowns[] but with its own mutex so vision
  // accounting can't starve reply accounting.
  chatbot_vision_cd_t vision_cd;      // keyed on channel or DM sender
  chatbot_vision_cd_t vision_url_cd;  // keyed on "<target>:<fnv16-of-url>"

  // Per-bot concurrent vision-fetch gate. Dedicated mutex keeps this
  // path off st->flight_mutex (which covers reply in-flight counts).
  uint32_t        vision_in_flight;
  pthread_mutex_t vision_flight_mutex;
} chatbot_state_t;

// ---- personality.c ----
// Public (file-on-demand) readers live outside CHATBOT_INTERNAL;
// see the top of this header.

// Register the /show personalities command. Called from chatbot
// plugin init. Implemented in personality_show.c.
bool chatbot_personality_show_register(void);

// Register the /show contracts command. Called from chatbot plugin
// init. Implemented in contract_show.c.
bool chatbot_contract_show_register(void);

// Resolve the configured personality directory into out_path.
// Shared helper between personality.c and personality_show.c.
// Returns true on success (always populates out_path with a default
// when the KV key is unset).
bool chatbot_personality_path(char *out_path, size_t sz);

// ---- interests.c ----

// Parse a JSON array into acquire_topic_t entries. `json` is the raw
// interests_json string from the personality frontmatter (empty OK).
// `out` is a caller-owned array of `cap` entries; populated from index
// 0 up to the number of successfully parsed topics. `*n_out` receives
// that count.
//
// Validation is per-topic: malformed entries are skipped with a WARN,
// well-formed ones continue. `mode` must be one of "active" /
// "reactive" / "mixed" (case-insensitive). proactive_weight defaults
// per mode (active=100, reactive=0, mixed=50). Unknown keys are
// ignored.
//
// returns: SUCCESS always (empty input yields 0 topics); FAIL only on
//          argument errors.
bool chatbot_interests_parse(const char *json,
    acquire_topic_t *out, size_t cap, size_t *n_out);

// ---- commands.c ----

// Register / unregister /llm personality * commands.
bool chatbot_cmds_register(void);
void chatbot_cmds_unregister(void);

// Re-register the active personality's reactive-topic cache with the
// acquisition engine. Defined in chatbot.c. Called from /bot <name>
// refresh_prompts (commands.c) after clearing st->registered_persona
// so the gate inside reliably re-runs chatbot_register_interests.
void chatbot_ensure_interests(chatbot_state_t *st);

// Register the /bot <name> dossiersweep verb (llm-kind filtered). Called
// from chatbot_cmds_register(). Implemented in dossier_cmd.c. Dossier
// admin mutators (/dossier ...) and /show dossiers candidates live in
// core now.
bool chatbot_dossiersweep_cmd_register(void);

// Register /show bot <name> <verb> handlers for llm-kind bots.
// Called from chatbot plugin init. Implemented in show_verbs.c.
bool chatbot_show_verbs_register(void);

// Parse a short duration string: "30s", "5m", "2h", "1d", or a bare
// number interpreted as seconds. Returns 0 on parse failure. Shared by
// /bot <n> hush and /remind.
uint64_t chatbot_parse_duration_secs(const char *s);

// The one reading of "hushed" for every path that can speak — the
// reply speak path, the volunteer cascade and the soul tick (D9).
// True while bot.<name>.behavior.mute_until is in the future; a lapsed
// deadline is cleared here (lazily, on next read) so an idle bot
// re-opens without a scheduled task.
bool chatbot_mute_active(const char *botname);

// ---- chatbot.c ----

// Resolve every name `botname` answers to on the method `msg` arrived
// on. Never fails: a bot the method has no name for, and with no aka
// list, simply answers to no text and relies on the method's own
// addressing verdict.
void chatbot_names_resolve(const char *botname, const method_msg_t *msg,
    chatbot_names_t *out);

mem_msg_kind_t chatbot_classify_message(const method_msg_t *msg,
    const chatbot_names_t *names);

// Global driver vtable (defined in chatbot.c).
extern const bot_driver_t chatbot_driver;

// Resolve the dossier for an inbound message. Runs the method driver's
// dossier_signature callback, consults userns MFA patterns, and calls
// dossier_resolve() with create_if_missing gated on either an MFA
// match or the per-instance chatbot.anonymous_dossiers KV toggle.
// Returns 0 when no dossier could be resolved (unsupported method,
// signature extraction failed, or no match + anonymous disabled).
// Thread-safety: same as the rest of chatbot -- called from the
// message-delivery path and the llm worker, both of which are
// well-serialized per instance.
dossier_id_t chatbot_resolve_dossier(chatbot_state_t *st,
    const method_msg_t *msg);

// Handle a METHOD_MSG_NICK_CHANGE: if the old identity resolves to an
// existing dossier, record the new identity as a sighting on the same
// dossier. Always returns SUCCESS -- no-prior-dossier is not an error.
bool chatbot_handle_nick_change(chatbot_state_t *st, const method_msg_t *msg);

// Scan an inbound message against the bot's cached reactive / mixed
// topics. For each keyword hit, extract a subject (capitalized noun
// phrase near the match, else the keyword itself) and enqueue a
// reactive acquisition job via acquire_enqueue_reactive.
//
// Runs under st->lock rdlock; must not block. Called from
// chatbot_observe after conversation logging and before the speak
// policy decision.
void chatbot_scan_reactive_topics(chatbot_state_t *st,
    const method_msg_t *msg);

// ---- speak_policy.c ----

typedef enum
{
  CHATBOT_SPEAK_IGNORE,
  CHATBOT_SPEAK_REPLY,
  CHATBOT_SPEAK_INTERJECT
} chatbot_speak_t;

// Decide whether to respond to a message. Pure function — does not
// consult state except for the supplied args, so tests can exercise it
// directly.
//
// kind:                     classification from chatbot_classify_message.
// in_flight:                current in-flight LLM requests for this bot.
// max_inflight:             hard cap from KV.
// now_secs:                 caller-supplied wall-clock (for cooldown math).
// last_reply_secs:          last time this bot replied on this target
//                           (0 = never; but see TEXT-COOLDOWN-1 — the
//                           caller floors this to the bot handle's
//                           creation time, so it is 0 only in tests).
// cooldown_secs:            cooldown between spoken replies on a target.
// last_witness_interject:   last time this bot WITNESS-interjected on this
//                           target (0 = never). VF-3 cooldown key.
// witness_cooldown_secs:    seconds between witness interjects on the same
//                           target. 0 disables the gate.
// interject_prob:           probability (0–100) of interjecting on a WITNESS.
// rand_roll:                caller's uniform [0,99] roll (0 = always fires).
// harm_signal:              caller flagged the line as containing a
//                           destructive shell pattern (chmod 777, rm -rf /,
//                           curl|sh, …). When set, WITNESS bypasses the
//                           probability roll — in_flight / cooldown still
//                           gate, and mute gates upstream: the caller runs
//                           chatbot_mute_active() before this function is
//                           ever reached (D9). EXCHANGE_IN ignores this
//                           flag.
// relevance_boost_pct:      additive percent multiplier for interject_prob
//                           when the line touches a persona interest topic
//                           keyword. 0 = no boost. The effective prob is
//                           interject_prob * (100 + boost) / 100, capped at
//                           75 absolute so on-topic chatter never makes the
//                           bot spam.
// witness_base_prob:        CV-3 untopical floor. When relevance_boost_pct
//                           is 0 the effective probability is this value
//                           instead of interject_prob, so small-talk stays
//                           rare while topical fire can stay high. 0
//                           disables untopical interjects.
chatbot_speak_t chatbot_speak_decide(mem_msg_kind_t kind,
    uint32_t in_flight, uint32_t max_inflight,
    time_t now_secs, time_t last_reply_secs, uint32_t cooldown_secs,
    time_t last_witness_interject, uint32_t witness_cooldown_secs,
    uint32_t interject_prob, uint32_t witness_base_prob,
    uint32_t rand_roll,
    bool harm_signal, uint32_t relevance_boost_pct);

// Reverse-map CHATBOT_SPEAK_* → "IGNORE" / "REPLY" / "INTERJECT" for
// log lines. Pure; never returns NULL.
const char *chatbot_speak_name(chatbot_speak_t d);

// ---- nl_bridge.c ----

// Pure slash-line parser. If text starts with a "/" followed by a
// whitespace-delimited command name, split it into cmd_name + args and
// return true. Does not consult any allowlist — the caller
// (reply_nl_bridge) owns the allowlist + cmd_permits preflight.
//
// returns: true if a command was extracted
// text:          LLM-produced reply text (may have leading whitespace)
// cmd_out:       destination for the command name
// cmd_sz:        size of cmd_out
// args_out:      destination for the argument remainder
// args_sz:       size of args_out
bool chatbot_nl_extract_cmd(const char *text,
    char *cmd_out, size_t cmd_sz,
    char *args_out, size_t args_sz);

// ---- nl_observe.c ----

// Post-dispatch observer for NL-bridged commands. Called from
// reply_nl_bridge right after cmd_dispatch, this inspects the command's
// declarative slot table and, for typed slots we care about
// (CMD_NL_ARG_LOCATION today), schedules an async task that records
// the appropriate chat-specific side effect (dossier fact upsert).
// No reply is emitted; pure side effect. `msg` is the bridge's fully
// identity-populated synth message — the observer's dossier resolve
// needs the whole tuple (CHAT-NLOBSERVE-1).
void chatbot_nl_observe_location_slot(bot_inst_t *bot,
    const method_msg_t *msg, uint32_t ns_id,
    const cmd_nl_t *nl, const char *args_post_subst);

// True when a location label is a bare 5-digit US zip. Routing gate
// shared by every geocode site in the plugin: the CITY geocoder FAILs
// every zip (CHAT-NLOBSERVE-1 mechanism 2), so zip-shaped labels must
// go to the zip geocoder.
bool chatbot_label_is_zip(const char *label);

// ---- reply.c ----

// Entry point for the reply pipeline. Called from chatbot_observe
// when speak_policy returns REPLY or INTERJECT. Ownership of st/msg is
// *not* transferred; the function copies what it needs.
// is_direct_address is true only when the current line literally
// addressed the bot (nick match or DM); sticky-promoted WITNESS lines
// must pass false so the CV-4 fallback does not fire on them.
// nl_bridge_off is true only for the interpret cue's second submit —
// it suppresses the COMMANDS prompt block and the bridge itself so an
// interpreted exchange cannot chain another command.
void chatbot_reply_submit(chatbot_state_t *st, const method_msg_t *msg,
    bool was_addressed, bool is_direct_address, bool nl_bridge_off);

// Image-vision sibling of chatbot_reply_submit. Seeds the vision
// fields (image_b64 ownership transfers in from the caller; mime is
// copied; source URL is captured for logging) and otherwise follows
// the same pipeline as the text path. On any early failure the
// function takes ownership of image_b64 and frees it.
void chatbot_reply_submit_vision(chatbot_state_t *st,
    const method_msg_t *msg, const char *source_url,
    char *image_b64, const char *image_mime);

// Compile the image-intent regex used by the reply pipeline (I3).
// Called once from chatbot_plugin_init; safe to call repeatedly (noops
// if already compiled). Returns SUCCESS or FAIL on regex compile error.
bool chatbot_reply_init(void);

// Free the image-intent regex. Paired with chatbot_reply_init; called
// from chatbot_plugin_deinit.
void chatbot_reply_deinit(void);

// ---- volunteer.c (V1 — spontaneous post-acquire speech) ----

// Register the acquire post-ingest callback. Called once from
// chatbot_plugin_init after the driver is registered; idempotent.
// returns: SUCCESS on success, FAIL on unexpected error
bool chatbot_volunteer_init(void);

// Clear the acquire post-ingest callback. Paired with
// chatbot_volunteer_init; called from chatbot_plugin_deinit.
void chatbot_volunteer_deinit(void);

// Per-bot in-flight counter (exposed for speak_policy and stats).
uint32_t chatbot_inflight_get(chatbot_state_t *st);
void chatbot_inflight_record_reply(chatbot_state_t *st,
    const char *channel_or_sender, time_t now);

// TEXT-COOLDOWN-1: the getter never returns less than st->created_at,
// so an unstamped target reads as "spoke when this handle was created"
// rather than "never spoke". Returns 0 only for an empty or NULL
// target — see cooldown.h for why that one case is unfloored.
time_t chatbot_inflight_last_reply(chatbot_state_t *st,
    const char *channel_or_sender);

// VF-3 — witness-interject cooldown helpers. Keyed by `target`
// (channel for channel traffic, sender for DMs), over a cooldown.h
// slot table under the dedicated witness_cd.mutex. TEXT-COOLDOWN-1:
// the getter floors its result to st->created_at, so it returns 0 only
// for an empty target.
time_t chatbot_last_witness_interject(chatbot_state_t *st,
    const char *target);
void chatbot_stamp_witness_interject(chatbot_state_t *st,
    const char *target, time_t now);

// Reply-path internals — chatbot_req_t and tunable caps used by reply.c.
// Kept in the INTERNAL block so sibling translation units (volunteer.c,
// chatbot.c) can introspect the request record if/when they need to.

// inference.h (already included above) provides knowledge_image_t.

#define CHATBOT_PROMPT_SZ                  (32 * 1024)
#define CHATBOT_IMAGE_SUBJECT_SZ           128

// Deterministic one-liner emitted by the CV-4 fallback when a reply
// classified as direct-address produced only SKIP-suppressed lines.
// Picked to NOT appear in any personality body so that a transcript
// line matching this string is unambiguously the CV-4 fallback and
// never LLM output — keeps attribution clean in logs and scoring.
#define CHATBOT_DIRECT_FALLBACK_TEXT       "couldn't tell you."

// Deterministic denial when the NL bridge extracts a slash command the
// caller is not permitted to run. The slash line was already
// suppressed from the wire, so without this the asker gets total
// silence (CHAT-BRIDGE-SWALLOW-1 arm 3). Same attribution rule as the
// CV-4 string: appears in no personality body.
#define CHATBOT_NL_DENIED_TEXT             "that's not something I can do for you."

// CV-7 Part B — cooldown between two CV-4 fallback emissions on the
// same (method, target). The canned fallback bypasses the LLM and
// therefore also bypasses CV-6's anti-repeat window; without this
// guard the same four-word reply can fire seconds apart when the LLM
// SKIPs several direct lines in a row. Silence is the lesser evil.
#define CHATBOT_CV4_FALLBACK_COOLDOWN_SECS 90

// Some LLMs emit the raw CTCP verb ("ACTION catches it") when they mean
// "/me catches it". Accept the bare verb as an alias so the channel
// sees a properly-framed emote instead of a literal "ACTION ..."
// PRIVMSG. Keep the prefix and its length together so the strncasecmp
// site and the `line + N` bump stay in sync.
#define CHATBOT_ACTION_PREFIX              "ACTION "
#define CHATBOT_ACTION_PREFIX_LEN          7

// Hard caps for the EXCHANGE_IN mention-expansion block. The KV knobs
// (bot.<n>.behavior.mention.{top_k,max_dossiers}) are clamped to these.
#define CHATBOT_MENTION_FACTS_CAP          10
#define CHATBOT_MENTION_DOSSIERS_CAP       MEM_MSG_REFS_MAX

// Hard cap on the CV-6 recent-own-replies anti-repeat block. The KV
// knob chatbot.recent_replies_in_prompt is clamped to this so a
// misconfigured value cannot uncap the prompt slice. The per-line
// truncation cap CHATBOT_RECENT_REPLY_TEXT_SZ lives in memory.h (same
// size the stored mem_recent_reply_t buffer uses).
#define CHATBOT_RECENT_REPLIES_MAX         10

// Forward: full definition lives in reply.c, where the mention bundle
// is assembled. chatbot_req_t only holds a heap-pointer.
struct chatbot_mention;

// Request-lifetime context. Allocated by chatbot_reply_submit, freed in
// the LLM done callback. Ownership is transferred into llm's user_data.
typedef struct
{
  // First, and linked before any early return in the submit path: from
  // here to req_free this request is findable by a bot teardown.
  chatbot_hold_t  hold;

  chatbot_state_t *st;
  method_inst_t  *method;
  bool            was_addressed;
  // CV-7: true only for CHATBOT_CLASSIFY_DIRECT (nick literally in
  // line) or DM. Sticky-promoted WITNESS lines stay false so the CV-4
  // fallback does not fire on overheard chatter.
  bool            is_direct_address;
  bool            is_action_at_bot;

  char            sender[METHOD_SENDER_SZ];
  // Protocol-level sender identity snapshotted from the incoming
  // method_msg_t.metadata. Preserved here so the NL-bridge synth msg
  // (reply_nl_bridge) can forward it to the dispatched command —
  // otherwise IRC's dossier_signature + MFA match both break on the
  // synth, and downstream features (e.g. "note the weather-queried
  // city on the sender's dossier") silently fail.
  char            sender_metadata[METHOD_META_SZ];
  // Snapshotted for the same reason as sender_metadata: every message
  // the bridge synthesizes downstream of this request (dispatch synth,
  // nl_observe, interpret cue) must carry it or chat_user_dossier_id
  // refuses to resolve — identity.h §chat_identity_t.
  chat_identity_t who;
  char            channel[METHOD_CHANNEL_SZ];
  char            reply_target[METHOD_CHANNEL_SZ];
  char            text[METHOD_TEXT_SZ];

  uint32_t        ns_id;
  int             user_id;
  dossier_id_t    dossier_id;

  // Dossiers whose names appear in r->text. Populated in
  // chatbot_reply_submit only on EXCHANGE_IN (direct address); empty on
  // WITNESS so chatter doesn't pay the fan-out cost.
  dossier_id_t    mention_ids[CHATBOT_MENTION_DOSSIERS_CAP];
  size_t          n_mentions;
  uint32_t        mention_top_k;       // clamp of bot.<n>.behavior.mention.top_k
  uint32_t        mention_max_chars;   // byte budget for the block

  char            personality_name[CHATBOT_PERSONALITY_NAME_SZ];
  char           *personality_body;    // mem_alloc'd
  char           *contract_body;       // mem_alloc'd; chatbot_contract_read
  char           *system_prompt;       // mem_alloc'd, assembled

  // Knowledge binding: semicolon-separated corpus list from
  // `bot.<name>.corpus`. Empty = no corpus retrieval; non-empty =
  // chain into knowledge_retrieve after memory_retrieve_dossier
  // completes. Knobs snapshot KV at submit time so subsequent KV
  // changes can't tear off mid-reply.
  char            knowledge_corpus[CHATBOT_CORPUS_LIST_SZ];
  uint32_t        knowledge_top_k;
  uint32_t        knowledge_max_chars;

  // Byte budget for the RECENT CONVERSATION block, snapshot from
  // `memory.rag_max_context_chars` at submit time for the same reason
  // the knowledge budget is. 0 means unbounded — the whole system
  // prompt is still capped by CHATBOT_PROMPT_SZ.
  uint32_t        memory_max_chars;

  // Image-splice knobs (I2). rag_images_per_reply doubles as the master
  // switch — 0 disables the IMAGES fence AND its system-prompt hint.
  uint32_t        images_per_reply;
  uint32_t        images_max_chars;
  bool            include_source_url;

  // Intent + subject supplement (I3). `image_subject` is populated only
  // when the intent regex matched at submit time; subject_limit /
  // max_age_days drive the DB fetch. `images_recency_ordered` is set
  // once the subject path has actually produced rows — the fence
  // header reflects it so the model knows the list is newest-first.
  bool            image_intent_enabled;
  char            image_subject[CHATBOT_IMAGE_SUBJECT_SZ];
  uint32_t        subject_limit;
  uint32_t        subject_max_age_days;
  bool            images_recency_ordered;

  // Stashed memory-retrieval results, copied out of the memory callback's
  // arrays so they survive the knowledge async hop. Freed in req_free.
  mem_fact_t        *stash_facts;
  size_t             stash_nf;
  mem_msg_t         *stash_msgs;
  size_t             stash_nm;
  // stash_mentions is a heap copy of chatbot_mention_t[] so we keep the
  // computed fact bundles around while the knowledge embed resolves.
  struct chatbot_mention *stash_mentions;
  size_t             stash_nmentions;
  // Images attached to retrieved knowledge chunks (chunk_id JOIN).
  // Populated in knowledge_cb so assemble_prompt can emit the IMAGES
  // fence. Owns the heap allocation; freed in req_free.
  knowledge_image_t *stash_images;
  size_t             stash_ni;

  // Image-vision path (IV3). Populated by chatbot_reply_submit_vision;
  // steers assemble_and_submit to emit a two-block user message.
  bool            vision_active;
  char           *image_b64;                       // mem_alloc'd, freed in req_free
  char            image_mime[32];
  char            image_source_url[1024];

  char            chat_model[64];
  float           temperature;
  uint32_t        max_tokens;
  // NL bridge allowlist. Semantics (see nl_bridge_list_permits):
  //   ""  → bridge disabled
  //   "*" → every NL-capable command that also passes cmd_permits
  //   otherwise → comma-separated command names, case-insensitive.
  char            nl_bridge_cmds[256];

  // True on the second submit of an interpreted exchange (the internal
  // cue carrying fenced command output). Leaves nl_bridge_cmds empty so
  // the COMMANDS block is never rendered and reply_nl_bridge disarms:
  // one ask → one command → one reply, no chaining, no loops.
  bool            nl_bridge_off;

  // Streaming coalescer: accumulate deltas, flush per-line via
  // method_send. done_cb sends any residual tail.
  char            stream_buf[METHOD_TEXT_SZ];
  size_t          stream_pos;
  size_t          stream_flushed;   // bytes of resp->content already sent

  // Direct-address SKIP guard (CV-4). Counts the two classes of line
  // the stream handler has processed: lines suppressed because the
  // LLM emitted the whole-line SKIP sentinel, and lines actually
  // handed off to method_send / method_send_emote. llm_done uses
  // these to fire a deterministic fallback when a directly-addressed
  // turn produced only SKIPs, so the channel isn't left hanging.
  uint32_t        skip_sentinels_seen;
  uint32_t        nonskip_lines_sent;

  // CV-6 — Recent-own-replies anti-repeat slice. Populated in
  // chatbot_reply_submit via memory_recent_own_replies; rendered in
  // assemble_prompt step 4b so the persona's "do not repeat yourself"
  // rule has teeth.
  mem_recent_reply_t recent_replies[CHATBOT_RECENT_REPLIES_MAX];
  size_t             n_recent_replies;

  // CV-13 — Trigram-Jaccard percent (0..99) required to drop an
  // outgoing line as a near-repeat of any recent_replies entry. The
  // CV-6 slice above is a soft nudge rendered into the prompt; this
  // field drives the deterministic hard guard applied post-LLM in
  // send_reply_line. Snapshotted once at chatbot_reply_submit so the
  // threshold stays stable across the streaming lines of one reply.
  // 0 = guard disabled.
  uint32_t           anti_repeat_threshold_pct;
} chatbot_req_t;

// ---- show_verbs.c — answering after the verb has returned ----

// Everything a reply needs once `cmd_ctx_t` is gone. A worker-pool
// callback fires long after the verb returned, so the target is
// snapshotted on the heap and the method instance by NAME, never by
// pointer: a `/plugin reload irc` in that window frees the instance the
// verb saw (method.h §method_msg_t).
//
// `route` is the driver-private reply address (OBS-29) and outranks the
// channel/sender pair for the reason cmd_reply prefers it -- it names
// the session that asked, not whoever the driver is serving when the
// answer exists. A driver that serves one session at a time leaves it
// empty. Omitting it loses the whole reply over botmanctl, silently.
typedef struct
{
  char inst_name[METHOD_NAME_SZ];
  char target[METHOD_SENDER_SZ];
  char route[METHOD_ROUTE_SZ];
} verb_reply_to_t;

void verb_reply_to_snapshot(verb_reply_to_t *to, const cmd_ctx_t *ctx);
const char *verb_reply_to_addr(const verb_reply_to_t *to);

// ---- persona_reply.c — a command surface borrowing the bot's voice ----

// The bot_driver_t.persona_reply slot (include/bot.h). Declines --
// leaving the caller to reply for itself -- when the conversational
// half is off, no persona resolves, or the request cannot be launched.
bool chatbot_persona_reply(void *handle, const cmd_ctx_t *ctx,
    const char *instruction, const char *plain);

// ---- interpret.c ----
// (Below chatbot_req_t — the begin call reads the request record.)

// Open an output capture for a bridged command about to be dispatched.
// Claims a collector slot, registers a reply sink (cmd.h §Reply sinks)
// and arms the hard deadline. The caller threads the returned id
// through the dispatched message's reply_sink_id; every cmd_reply the
// command makes then lands in the collector, and a settle window after
// the last line closes the capture and re-submits the block as an
// internal cue through the persona pipeline (nl_bridge_off).
// `synth` is the identity-complete message being dispatched.
// `premise` is the cue's opening sentence and belongs to the caller —
// only it knows why the command ran ("X asked … and you ran /y", "X
// asked you an hour ago to run /y when the time came"). It must read
// as a complete sentence ending in a period; the fixed instruction and
// the output fence follow it. Control bytes are flattened on copy.
// Returns 0 when no slot is free (caller dispatches uncaptured — the
// channel gets the verbatim block, never silence).
uint64_t chatbot_interpret_begin(chatbot_state_t *st,
    const method_msg_t *synth, const char *cmd, const char *args,
    const char *premise, bool was_addressed, bool is_direct);

// Retract every live capture owned by `st` (bot stop), or every
// capture regardless of owner (plugin stop). Sinks are unregistered
// first — after that no collector callback is running or will run —
// then pending settle/deadline tasks are cancelled and the slots
// cleared. Airborne command output falls through to the wire.
void chatbot_interpret_stop(chatbot_state_t *st);
void chatbot_interpret_stop_all(void);

// ---- voice.c — the voice governor (CARE-3) ----

// How much say the governor gets over a cue. The classes are about who
// chose the moment, not about what the cue says (CARE-3 §D8).
typedef enum
{
  SOUL_CLASS_TIMED       = 0,   // the human picked it: /remind, /in
  SOUL_CLASS_ASKED       = 1,   // they asked, the bot picked the moment
  SOUL_CLASS_UNSOLICITED = 2,   // nobody asked for this one
} soul_class_t;

// Idempotent chat_soul_voice_log DDL. Called from chatbot_plugin_start
// after dossier_register_config (the dossier FK target must exist).
void soul_voice_ensure_schema(void);

// May a cue of this class be spoken at this moment? Pure clock + KV,
// no DB, no accounting — this is the question a chore asks BEFORE it
// claims anything durable, because quiet hours mean later and a claim
// taken inside the window would mean never. TIMED is always open.
// `severe` passes the quiet window when quiet.override_severe is on.
bool soul_voice_window_open(const char *bot_name, soul_class_t cls,
    bool severe);

// The gate every chore calls immediately before submitting a cue: the
// quiet window plus, for UNSOLICITED, the hourly and per-person
// budgets. A permitted cue is recorded in the same statement that
// permits it, so racing chores cannot both spend the last slot.
// `dossier_id` <= 0 means the cue names no one in particular and only
// the hourly budget applies. TIMED and ASKED are never refused for
// budget; TIMED is never refused at all, so its callers record rather
// than branch. Blocking (sync db_query) — chore threads only.
bool soul_voice_permits(const char *bot_name, uint32_t ns_id,
    int64_t dossier_id, soul_class_t cls, bool severe);

// Drop log rows past the retention window. Called once per soul tick.
void soul_voice_purge(void);

// ---- deferred.c — durable work the bot owes a human (CARE-1) ----

// What a row delivers when its moment comes. Public because chores
// write rows too (CARE-5), and because these two are the whole
// contract of the table: speech held until its moment, or an action
// held until its moment and reported afterwards in voice.
typedef enum
{
  DEFERRED_KIND_SAY = 0,
  DEFERRED_KIND_RUN = 1,
} deferred_kind_t;

// Idempotent chat_deferred DDL. Called from chatbot_plugin_start after
// dossier_register_config (the dossier FK target must exist).
void chatbot_deferred_ensure_schema(void);

// The one insert every writer goes through — both user verbs and every
// chore. `secs` is a DELAY, not an instant: `due_at` is computed
// DB-side so the daemon's clock and the DB's never argue. On a
// presence row that delay is how long the row stays disarmed before it
// starts watching, never when it speaks.
//
// `expires_secs` 0 takes the default policy — 30 days for a presence
// row, so nobody who never comes back holds a pending slot forever,
// and no expiry at all for a timed one. A chore whose work has a real
// deadline (a birthday wish is worse two days late than never) passes
// its own.
//
// ⚠ `msg` supplies the identity tuple the row stores, and it rides
// whole: a chore composing one synthetically must fill it from a
// dossier signature (the dcfc359 rule) or the delivered cue resolves
// to no dossier and the reply loses the person's memory.
bool chatbot_deferred_insert(uint32_t ns_id, int64_t dossier,
    const method_msg_t *msg, const char *method_name, const char *source,
    deferred_kind_t kind, const char *body, const char *cmd_name,
    uint64_t secs, bool on_presence, uint64_t expires_secs);

// Claim and deliver every row that has come due for this namespace,
// say and run alike. Called from the soul tick's `deferred` chore, on
// the tick's worker thread — it blocks on sync db_query and dispatches
// commands, so it must never run on a callback thread.
void chatbot_deferred_run_due(const char *bot_name, uint32_t ns_id,
    chatbot_state_t *st, bot_inst_t *bot);

// One row of the soul's presence cache (CARE-2): the least a sighting
// needs to recognise its subject, and nothing durable — the row itself
// stays in the DB and the id is what gets claimed. `cls` rides along
// because quiet hours must be answered BEFORE the claim (CARE-5), and
// by then the only thing the sighting knows about the row is what the
// scan put here.
typedef struct
{
  int64_t      id;
  soul_class_t cls;
  char         nickname[METHOD_NICKNAME_SZ];
  char         sender  [METHOD_SENDER_SZ];
} chatbot_presence_row_t;

// Drop presence rows whose window has closed, then fill `out` with up
// to `max` rows still waiting for their subject to turn up. Returns how
// many were written. Blocking (sync db_query) — tick thread only.
uint32_t chatbot_deferred_presence_scan(const char *bot_name,
    uint32_t ns_id, chatbot_presence_row_t *out, uint32_t max);

// Claim one presence row by id and deliver it. A zero-row claim is the
// normal outcome for the loser of a two-witness race, not an error.
// Blocking — runs on the presence delivery task, never on the method
// thread that saw the line.
void chatbot_deferred_deliver_presence(const char *bot_name, uint32_t ns_id,
    chatbot_state_t *st, bot_inst_t *bot, int64_t id);

// Register /remind, /in (+ list, cancel) and /show deferred. Called
// from chatbot_cmds_register.
bool chatbot_deferred_register(void);

// Build the SQL predicate "this row is the caller's": the stored sender
// matches case-insensitively (nick case is not identity on IRC), or
// both carry the same verified identity. Shared by every table that
// stores the identity tuple under those column names — chat_deferred
// and chat_pricewatch today. The inputs are the caller's own message
// fields and never a name they typed, because ownership is not
// something you can type. Returns FAIL when the escape fails.
bool chatbot_row_owner_pred(const method_msg_t *msg, char *dst, size_t cap);

// Owner or namespace admin. The owner test is the stored tuple; this
// one is the caller's authenticated membership, so an unauthenticated
// nick can never reach another person's row however it is spelled.
bool chatbot_caller_is_admin(const cmd_ctx_t *ctx, const userns_t *ns);

// ---- occasions.c — dates somebody told the bot about (CARE-5) ----

// One sweep of the occasions chore: scan the namespace for birthday
// facts falling today, claim each at most once per person per year,
// and hand a wish to the deferred spine to deliver when its subject is
// next seen. Speaks nothing itself. Blocking (sync db_query) — runs on
// the tick's worker thread only.
void chatbot_occasions_run(const char *bot_name, uint32_t ns_id,
    bot_inst_t *bot);

// ---- followups.c — plans somebody told the bot about (CARE-6) ----

// One sweep of the follow-ups chore: scan the namespace for
// `upcoming_event:*` facts whose date has just passed, claim each at
// most once per person per event, and hand a question to the deferred
// spine to ask when its subject is next seen. Speaks nothing itself,
// and owns nothing about the answer — the ordinary extract sweep reads
// that like any other line. Blocking (sync db_query) — runs on the
// tick's worker thread only.
void chatbot_followups_run(const char *bot_name, uint32_t ns_id,
    bot_inst_t *bot);

// ---- pricewatch.c — a threshold on a market price (CARE-7) ----

// One bot's slot in the soul's schedule (soul.c). Opaque everywhere
// else: a chore whose work outlives its tick — CARE-7's snapshot is
// the only one — carries the handle solely to hand it back to
// soul_chore_done().
typedef struct soul_sched soul_sched_t;

// Idempotent DDL for chat_watchlist + chat_pricewatch. Called from
// chatbot_plugin_start after dossier_register_config (the dossier FK
// target must exist).
void chatbot_pricewatch_ensure_schema(void);

// Arm one threshold for one person. `dir` is the stored SMALLINT (0
// below, 1 above) and `msg` supplies the identity tuple the report
// will ride — whole, per the dcfc359 rule. The caller has already
// checked the pair against the serving bot's watchlist; this only
// writes.
bool chatbot_pricewatch_insert(uint32_t ns_id, int64_t dossier,
    const method_msg_t *msg, const char *bot_name, const char *method_name,
    const char *pair, int dir, double threshold);

// One sweep: read the bot's watchlist and its armed thresholds, ask the
// exchange for one bulk-ticker snapshot, and claim every crossing into
// a deferred report. Speaks nothing itself. Returns true when the
// snapshot is airborne — the chore's in-flight latch then belongs to
// the completion path, which releases it with soul_chore_done().
// Blocking (sync db_query) — runs on the tick's worker thread only.
bool chatbot_pricewatch_run(soul_sched_t *sched, uint32_t chore,
    const char *bot_name, uint32_t ns_id);

// Register /pricewatch (+ list, cancel), /bot <name> watchlist and
// /show bot <name> watchlist. Called from chatbot_cmds_register.
bool chatbot_pricewatch_register(void);

// ---- remember.c — the conversation-speed fact write path (FACT-3) ----

// Register /remember and /forget. Called from chatbot_cmds_register.
// Both write ONLY the invoking speaker's dossier — there is no target
// argument, which is the whole of their permission model.
bool chatbot_remember_register(void);

// ---- soul.c — the per-bot heartbeat (SOUL-2) ----

// Release a chore's in-flight latch. Called exactly once by whichever
// completion path owns an airborne chore — never by a chore that
// finished on the tick's own thread, which releases by returning false.
void soul_chore_done(soul_sched_t *s, uint32_t chore);

// Idempotent DDL for the soul's own tables (the generic claim ledger).
// Called from chatbot_plugin_start after dossier_register_config (the
// dossier FK target must exist).
void soul_ensure_schema(void);

// The soul's generic exactly-once guard (CARE-5): one row per thing a
// chore may do only once — an alert announced once per namespace, a
// birthday wished once per year. Returns true to the ONE caller that
// wins the key; false is the ordinary answer for a later tick, a
// racing peer, or a sweep a reload interrupted, and never an error.
// `expires` is when the claim stops meaning anything (0 = never); the
// purge keeps it two days past that. Keys are the chore's to compose,
// and the prefix convention (`wxalert:`, `bday:`) is what keeps two
// chores from ever arguing over one string.
bool soul_claim_take(uint32_t ns_id, const char *key, time_t expires);

// Drop claims whose meaning has expired. Called once per soul tick.
void soul_claim_purge(void);

// Arm / latch a bot's heartbeat. The extract.c scheduler pattern:
// schedule reuses a parked task when one exists, unschedule only flips
// the latch, soul_stop cancels every task (plugin stop), soul_exit
// frees the sched list (plugin deinit, after soul_stop).
void soul_schedule(const char *bot_name, uint32_t ns_id,
    uint32_t interval_secs);
void soul_unschedule(const char *bot_name);

// Presence, the soul's second trigger kind (CARE-2). Called from
// chatbot_observe for every line the conversational half sees, on the
// method's delivery thread: it must stay cheap and must never block.
// When no bot in this daemon owes presence-gated work it is one relaxed
// atomic load and a return.
void soul_on_seen(const char *bot_name, const method_msg_t *msg);
void soul_stop(void);
void soul_exit(void);


#endif // CHATBOT_INTERNAL

#endif // BM_CHATBOT_H
