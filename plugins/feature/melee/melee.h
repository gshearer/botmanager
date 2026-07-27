#ifndef BM_MELEE_H
#define BM_MELEE_H

// botmanager — MIT
// melee: the Drow duelling pit. `!melee <nick>` rolls damage against
// another registered user; hit points, waves, and per-round + lifetime
// scores persist in the caller's user namespace. The first death ends
// the round, and where the protocol allows it the fallen are removed
// from the room through the generic method eject primitive.
//
// ---- Public-facing: nothing. The plugin talks to the core exclusively
// through cmd_register / cmd_reply / db_* / kv_* / method_*. All
// declarations below are internal, gated on MELEE_INTERNAL.

#ifdef MELEE_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define MELEE_CTX            "melee"

// KV keys (registered under the plugin schema).
#define MELEE_KV_PREFIX      "plugin.melee.table_prefix"
#define MELEE_KV_START_HP    "plugin.melee.start_hp"
#define MELEE_KV_HIT_MAX     "plugin.melee.hit_max"
#define MELEE_KV_CRIT_PCT    "plugin.melee.crit_chance_pct"
#define MELEE_KV_CRIT_MIN    "plugin.melee.crit_min"
#define MELEE_KV_CRIT_MAX    "plugin.melee.crit_max"
#define MELEE_KV_TIMEOUT     "plugin.melee.round_timeout"
#define MELEE_KV_EJECT       "plugin.melee.eject_on_death"
#define MELEE_KV_SCORE_ROWS  "plugin.melee.scoreboard_rows"

// LLM-authored combat flavour. Every one of these is inert while
// MELEE_KV_LLM_MODEL names nothing usable: the pit then speaks from the
// static tables in melee_combat.c, exactly as it always has.
#define MELEE_KV_LLM_MODEL   "plugin.melee.llm.model"
#define MELEE_KV_LLM_PROMPT  "plugin.melee.llm.prompt_file"
#define MELEE_KV_LLM_POOL    "plugin.melee.llm.pool_size"
#define MELEE_KV_LLM_REFILL  "plugin.melee.llm.refill_at"
#define MELEE_KV_LLM_TEMP    "plugin.melee.llm.temperature_pct"
#define MELEE_KV_LLM_TOKENS  "plugin.melee.llm.max_tokens"
#define MELEE_KV_LLM_TIMEOUT "plugin.melee.llm.timeout_secs"
#define MELEE_KV_LLM_RETRY   "plugin.melee.llm.retry_secs"

// Storage bounds. The table prefix is a SQL identifier, so it is
// validated as strict alnum/underscore before it can reach a query.
#define MELEE_PREFIX_SZ      32                 // table-name prefix
#define MELEE_TABLE_SZ       64                 // prefix + "_players"
#define MELEE_USER_SZ        USERNS_USER_SZ     // 31
#define MELEE_NICK_SZ        METHOD_NICKNAME_SZ // 64
#define MELEE_CHAN_SZ        METHOD_CHANNEL_SZ  // 128
#define MELEE_MAX_PLAYERS    32                 // per round, for the show card
// The ceiling melee_tunables_load() clamps scoreboard_rows to. The
// leaderboard's row array is sized from this, so the two must agree.
#define MELEE_MAX_SCORE_ROWS 50
#define MELEE_LINE_SZ        512                // one rendered, colorized line
// A comma-joined display list. Deliberately well under MELEE_LINE_SZ so
// it always fits inside the sentence that carries it; a roster longer
// than this would not survive an IRC line anyway.
#define MELEE_ROSTER_SZ      320

// Ceiling on model-authored lines held per flavour category; the
// pool_size knob clamps to it.
#define MELEE_LLM_POOL_MAX   64
// One un-expanded template. The arithmetic that fixes this number:
// MELEE_LINE_SZ is 512 and must hold the EXPANDED line. Expansion
// replaces {attacker}/{target} (10 and 8 bytes) with a colorized nick
// (MELEE_NICK_SZ 63 + ~10 bytes of colour = ~73 each) and {damage}
// (8 bytes) with a colorized number (~20). Worst-case growth is about
// +150 bytes, and melee_render_blow then adds the emoji prefix and the
// " [nick — hp/hp hp]" tail, another ~90. 256 + 150 + 90 = 496 < 512.
// Do not raise this without redoing that sum.
#define MELEE_LLM_TMPL_SZ    256
// Filesystem path to the persona prompt, relative to the daemon CWD.
#define MELEE_LLM_PATH_SZ    256
// Longest model name the llm subsystem will hand back.
#define MELEE_LLM_MODEL_SZ   64

// The three independent flavour pools. The order is used as an array
// index; keep the enum and every table keyed by it in step.
typedef enum
{
  MELEE_FLAV_HIT = 0,
  MELEE_FLAV_CRIT,
  MELEE_FLAV_DEATH,
  MELEE_FLAV__COUNT
} melee_flavour_t;

// Round lifecycle, as stored in <prefix>_rounds.state.
#define MELEE_ROUND_ACTIVE    0
#define MELEE_ROUND_ENDED     1
#define MELEE_ROUND_ABANDONED 2

// Every knob an operator can turn, already sanitised. Read once per
// command with melee_tunables_load(); never read the raw KV at a use
// site — an operator can set hit_max to 0 or invert the crit band.
typedef struct
{
  uint32_t start_hp;         // hit points each combatant enters with
  uint32_t hit_max;          // ordinary blow rolls 1..hit_max
  uint32_t crit_pct;         // percent chance a blow lands critical
  uint32_t crit_min;         // critical damage floor
  uint32_t crit_max;         // critical damage ceiling (>= crit_min)
  uint32_t round_timeout;    // seconds of silence before a round is cold
  uint32_t scoreboard_rows;  // rows shown by `show melee scores`
  bool     eject_on_death;   // remove the fallen where the method allows

  // Flavour authorship. `llm_model` empty is the off switch, and is the
  // shipped default: the pit speaks from its static tables until an
  // operator names a chat model.
  char     llm_model[MELEE_LLM_MODEL_SZ];  // empty = feature off
  char     llm_prompt[MELEE_LLM_PATH_SZ];  // empty/unreadable = no persona
  uint32_t llm_pool;         // lines held per category
  uint32_t llm_refill_at;    // low-water mark; always < llm_pool
  uint32_t llm_temp_pct;     // sampling temperature x100
  uint32_t llm_max_tokens;   // ceiling per refill request
  uint32_t llm_timeout;      // seconds per refill request
  uint32_t llm_retry;        // seconds a failed category waits
} melee_tunables_t;

// The three table names for the configured prefix, resolved together so
// the prefix is validated exactly once per operation.
typedef struct
{
  char rounds [MELEE_TABLE_SZ];
  char players[MELEE_TABLE_SZ];
  char scores [MELEE_TABLE_SZ];
} melee_tables_t;

// A brawl in progress, as the turn engine needs to see it.
typedef struct
{
  int64_t id;
  int32_t wave;
  int32_t blows;
  int32_t top_crit;
  int64_t idle;        // seconds since last_action
} melee_round_t;

// One combatant's standing within a round.
typedef struct
{
  char    nickname[MELEE_NICK_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t last_wave;
} melee_player_t;

// One resolved turn, ready to be written. The `*_user` names are
// namespace-scoped usernames and are the identity of record; the
// `*_nick` names are display-only and are refreshed on every blow.
typedef struct
{
  int64_t     round_id;
  uint32_t    ns_id;
  const char *atk_user;
  const char *atk_nick;
  const char *tgt_user;
  const char *tgt_nick;
  int32_t     dmg;
  int32_t     wave;      // the wave the attacker is spending
  bool        crit;
  bool        new_top;   // this crit is the round's heaviest so far
  bool        fatal;     // the target reaches 0 hp
} melee_blow_t;

// ---- The read-only views (show melee, show melee scores) ------------ //
//
// These three carry display names, never identities: `name` is the last
// nickname the pit saw, falling back to the username. Nothing here is
// ever fed back into a query.

// The header of one round, live or finished.
typedef struct
{
  int64_t id;
  char    channel[MELEE_CHAN_SZ];
  int32_t state;                 // MELEE_ROUND_*
  int32_t wave;
  int32_t blows;
  int64_t length;                // seconds start -> end, or -> now
  int32_t top_crit;              // heaviest critical of the round, 0 = none
  char    top_by[MELEE_USER_SZ];
  char    top_on[MELEE_USER_SZ];
  char    slayer[MELEE_USER_SZ]; // set once the round has been won
  char    fallen[MELEE_USER_SZ];
} melee_card_t;

// One combatant's row on the round card.
typedef struct
{
  char    name[MELEE_NICK_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t dmg_given;
  int32_t dmg_taken;
  int32_t best_crit;
  int32_t last_wave;
} melee_card_row_t;

// One combatant's row on the lifetime leaderboard.
typedef struct
{
  char    name[MELEE_NICK_SZ];
  int32_t rounds;
  int32_t kills;
  int32_t deaths;
  int64_t dmg_given;
  int64_t dmg_taken;
  int32_t crits;
  int32_t best_crit;
} melee_score_row_t;

// ---- Plugin core (melee.c) ----------------------------------------- //

void melee_tunables_load(melee_tunables_t *out);

// ---- DB layer (melee_db.c) ----------------------------------------- //

// Resolve + validate the configured table names into `out`. FAIL when
// the KV prefix is not a safe SQL identifier.
bool melee_tables_resolve(melee_tables_t *out);

// Ensure the three tables + their indexes exist. Idempotent; runs once.
bool melee_schema_ensure(void);

// The active round in this room, if there is one.
bool melee_db_round_find(uint32_t ns_id, const char *method,
    const char *channel, melee_round_t *out);

// Retire a round nobody came back to.
bool melee_db_round_abandon(int64_t round_id);

// Open a round. Returns the new id (>0) or -1.
int64_t melee_db_round_open(uint32_t ns_id, const char *method,
    const char *channel, const char *opener);

// Enrol a combatant at full health, bumping <p>_scores.rounds iff they
// were not already in this round. Returns true when newly enrolled.
bool melee_db_player_enrol(int64_t round_id, uint32_t ns_id,
    const char *username, const char *nickname, int32_t hp);

bool melee_db_player_get(int64_t round_id, const char *username,
    melee_player_t *out);

// Comma-joined display names of the living who have not swung in
// `wave`. Writes an empty string when nobody is pending.
bool melee_db_pending(int64_t round_id, int32_t wave, char *out,
    size_t cap);

// Write a whole turn as one transaction: both combatants, the round
// counters, both lifetime score rows, the wave advance, and — when the
// blow is fatal — the round close and the kill/death tally. All or
// nothing; a daemon death mid-turn can never leave the attacker charged
// for a blow the target never took.
bool melee_db_blow_apply(const melee_blow_t *blow);

// ---- DB reads for the views (melee_db.c) --------------------------- //

// The round `show melee` should describe: the room's active round when
// there is one, else the room's most recent brawl. An empty `channel`
// (a direct message has no room) widens the search to the whole
// namespace. false when the namespace has never fought here.
bool melee_db_card_find(uint32_t ns_id, const char *method,
    const char *channel, melee_card_t *out);

// Fill `out` with up to `cap` combatants, healthiest first. `total` (may
// be NULL) reports how many the round actually holds, so the caller can
// note what the cap left out. Returns the number written.
uint32_t melee_db_card_roster(int64_t round_id, melee_card_row_t *out,
    uint32_t cap, uint32_t *total);

// Lifetime standings for a namespace, heaviest damage dealt first.
// Returns the number written, at most min(cap, limit).
uint32_t melee_db_scores(uint32_t ns_id, uint32_t limit,
    melee_score_row_t *out, uint32_t cap);

// The single heaviest critical anyone in the namespace has landed.
// false when no crit has ever been struck.
bool melee_db_deadliest(uint32_t ns_id, char *by, size_t by_cap,
    char *on, size_t on_cap, int32_t *dmg);

// ---- Combat (melee_combat.c) --------------------------------------- //

// Roll one blow. *crit_out reports whether it landed critical.
int32_t melee_roll(const melee_tunables_t *t, bool *crit_out);

// Both renderers consult the flavour pool first and fall back to the
// static tables. `need_refill` (may be NULL) reports that the pool they
// drew from has reached its low-water mark; the CALLER kicks the refill,
// after the turn lock is released.
void melee_render_blow(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg, bool crit, int32_t hp,
    int32_t hp_max, const melee_tunables_t *t, bool *need_refill);

void melee_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick, const melee_tunables_t *t, bool *need_refill);

// ---- LLM-authored flavour (melee_llm.c) ---------------------------- //

// True only when the inference plugin is loaded AND `t->llm_model` names
// a usable chat model. The plugin_find() gate comes FIRST and is not
// optional: the dlsym shims in inference.h abort() when inference is
// absent, and melee must stay deployable on a daemon with no LLM.
bool melee_llm_enabled(const melee_tunables_t *t);

// Why flavour authorship is off, or NULL when it is on. Same four gates
// as melee_llm_enabled(), in the same order, phrased for a reader.
const char *melee_llm_offreason(const melee_tunables_t *t);

// Pop a uniformly-chosen template out of `cat`'s pool, swap-removing it
// so one pool generation never speaks the same line twice. SUCCESS with
// `out` populated, or FAIL when the pool is empty — on FAIL the caller
// renders from the static tables. *need_refill is set when the pool has
// fallen to `refill_at`; the CALLER kicks the refill, and only after it
// has released every lock it holds.
bool melee_pool_take(melee_flavour_t cat, char *out, size_t cap,
    uint32_t refill_at, bool *need_refill);

// Record that a pool came up dry and the static tables spoke instead.
void melee_pool_fallback(void);

// Arm a background refill for one category. Idempotent and cheap: a
// no-op when the feature is off, when a refill is already in flight, or
// while the failure backoff is still running. Never call it while
// holding melee_turn_lock — keeping the turn path free of the task
// system entirely is what makes "no LLM on the critical path"
// inspectable rather than argued.
void melee_llm_refill_kick(melee_flavour_t cat, const melee_tunables_t *t);

// Fill all three pools at startup, so the pit is flavoured before the
// first blow rather than after it.
void melee_llm_prime(const melee_tunables_t *t);

// One category's pool as `show melee llm` sees it.
typedef struct
{
  uint32_t depth;         // unspoken lines held
  uint64_t served;        // templates handed to the renderer
  uint64_t rejected;      // model lines the sanitiser threw away
  int64_t  wait;          // seconds of failure backoff left, 0 = none
  bool     inflight;      // a refill is outstanding
  char     last_error[128];
} melee_pool_stat_t;

void melee_pool_stats(melee_flavour_t cat, melee_pool_stat_t *out);

// How often a pool came up dry and the static tables spoke instead.
uint64_t melee_pool_fallbacks(void);

// Substitute {attacker}/{target}/{damage} into a bounded buffer, copying
// every other byte literally. Never hands `tmpl` to a printf conversion,
// and never rescans what it substituted — a `{` inside a nickname is
// data. The three values arrive already colorized, exactly as they do
// for the static tables.
void melee_tmpl_expand(char *out, size_t cap, const char *tmpl,
    const char *attacker, const char *target, const char *damage);

// ---- Command surface (melee_cmds.c) -------------------------------- //

bool melee_commands_register(void);
void melee_commands_unregister(void);

// ---- The read-only views (melee_show.c) ---------------------------- //

// Attach `show melee` and `show melee scores` beneath the core `show`
// parent. Called from melee_commands_register().
bool melee_show_register(void);

#endif // MELEE_INTERNAL

#endif // BM_MELEE_H
