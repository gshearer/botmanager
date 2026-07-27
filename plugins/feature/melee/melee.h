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

#include <pthread.h>
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

// Severity banding: the percentage of the heaviest possible blow at
// which each tier BEGINS. Below the first is minor.
#define MELEE_KV_SEV_MEDIUM  "plugin.melee.sev.medium_at"
#define MELEE_KV_SEV_MAJOR   "plugin.melee.sev.major_at"
#define MELEE_KV_SEV_CRIT    "plugin.melee.sev.critical_at"

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

// Damage over time: a wound that keeps working after the blow that made
// it. `dot.chance_pct` at 0 turns the whole feature off.
#define MELEE_KV_DOT_CHANCE  "plugin.melee.dot.chance_pct"
#define MELEE_KV_DOT_MIN     "plugin.melee.dot.min_secs"
#define MELEE_KV_DOT_MAX     "plugin.melee.dot.max_secs"
#define MELEE_KV_DOT_TICK    "plugin.melee.dot.tick_secs"
#define MELEE_KV_DOT_DMG     "plugin.melee.dot.tick_dmg_max"
#define MELEE_KV_DOT_STACK   "plugin.melee.dot.stack_max"
#define MELEE_KV_DOT_LINGER  "plugin.melee.dot.linger_secs"

// Storage bounds. The table prefix is a SQL identifier, so it is
// validated as strict alnum/underscore before it can reach a query.
#define MELEE_PREFIX_SZ      32                 // table-name prefix
#define MELEE_TABLE_SZ       64                 // prefix + "_players"
#define MELEE_USER_SZ        USERNS_USER_SZ     // 31
#define MELEE_NICK_SZ        METHOD_NICKNAME_SZ // 64
#define MELEE_CHAN_SZ        METHOD_CHANNEL_SZ  // 128
#define MELEE_METHOD_SZ      64                 // method instance name
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
// (8 bytes) with a colorized number (~20). {affliction} (12 bytes) is
// replaced by the longest name in melee_dot_name[] ("myconid spores",
// 14) plus ~10 of colour, another ~+14. Worst-case growth is about
// +164 bytes, and melee_render_blow then adds the emoji prefix and the
// " [nick — hp/hp hp]" tail, another ~90. 256 + 164 + 90 = 510 < 512.
// It fits, but only just: lengthen an affliction name and this sum has
// to be redone, or MELEE_LINE_SZ raised. Do not raise this without
// redoing that sum either.
#define MELEE_LLM_TMPL_SZ    256
// Filesystem path to the persona prompt, relative to the daemon CWD.
#define MELEE_LLM_PATH_SZ    256
// Longest model name the llm subsystem will hand back.
#define MELEE_LLM_MODEL_SZ   64

// The seven independent flavour pools: four damage tiers, the killing
// blow, and the two an affliction speaks. The order is used as an array
// index AND as the severity ordering itself (MINOR < MEDIUM < MAJOR <
// CRITICAL); keep the enum and every table keyed by it in step.
//
// The four damage tiers must stay contiguous and ascending — that is
// what melee_severity() returns as an ordering, and the tables sized
// MELEE_FLAV_DEATH cover exactly them. The tail beyond them is three
// non-tier categories in no particular order: the sanitiser reads its
// requirements from a table indexed by category, so nothing keys off any
// one of them being last.
typedef enum
{
  MELEE_FLAV_MINOR = 0,
  MELEE_FLAV_MEDIUM,
  MELEE_FLAV_MAJOR,
  MELEE_FLAV_CRITICAL,
  MELEE_FLAV_DEATH,
  MELEE_FLAV_DOT_TICK,
  MELEE_FLAV_DOT_DEATH,
  MELEE_FLAV__COUNT
} melee_flavour_t;

// The afflictions a blow may leave behind. The kind is chosen at inflict
// time and stored on the row, because it is what makes the words
// specific: the flavour layer substitutes its name rather than keeping a
// pool per affliction.
typedef enum
{
  MELEE_DOT_BLEED = 0,
  MELEE_DOT_VENOM,
  MELEE_DOT_ACID,
  MELEE_DOT_SPORES,
  MELEE_DOT_CHILL,
  MELEE_DOT__COUNT
} melee_dot_kind_t;

// Damage-over-time lifecycle, as stored in <prefix>_dots.state.
#define MELEE_DOT_LIVE      0   // still ticking
#define MELEE_DOT_SPENT     1   // ran its course, or landed the death blow
#define MELEE_DOT_CANCELLED 2   // the round ended out from under it

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
  uint32_t sev_medium_at;    // pct of the heaviest blow: medium begins
  uint32_t sev_major_at;     // ... major begins    (> sev_medium_at)
  uint32_t sev_crit_at;      // ... critical begins (> sev_major_at)
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

  // Damage over time. `dot_chance_pct` 0 is the off switch and is
  // checked before anything else on the inflict path.
  uint32_t dot_chance_pct;   // percent chance a non-fatal blow afflicts
  uint32_t dot_min_secs;     // shortest an affliction lasts
  uint32_t dot_max_secs;     // longest (>= dot_min_secs)
  uint32_t dot_tick_secs;    // seconds between decay ticks (<= min_secs)
  uint32_t dot_tick_dmg_max; // a tick rolls 1..N damage
  uint32_t dot_stack_max;    // afflictions one victim may carry at once
  uint32_t dot_linger_secs;  // decay task's idle life after the last DOT
} melee_tunables_t;

// The four table names for the configured prefix, resolved together so
// the prefix is validated exactly once per operation.
typedef struct
{
  char rounds [MELEE_TABLE_SZ];
  char players[MELEE_TABLE_SZ];
  char scores [MELEE_TABLE_SZ];
  char dots   [MELEE_TABLE_SZ];
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

// One combatant's row on the round card. `user` is the identity the
// affliction markers are matched on — it is never rendered.
typedef struct
{
  char    name[MELEE_NICK_SZ];
  char    user[MELEE_USER_SZ];
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

// The afflictions one combatant carries right now, for the round card.
// `n` is bounded by the stack cap, which melee_tunables_load() clamps to
// MELEE_DOT_STACK_CAP.
#define MELEE_DOT_STACK_CAP 4

typedef struct
{
  char             victim[MELEE_USER_SZ];
  melee_dot_kind_t kinds[MELEE_DOT_STACK_CAP];
  uint32_t         n;
} melee_dot_mark_t;

// How many afflictions one decay iteration may service — and so how
// much it may say into a channel in one pass. Anything over the bound
// waits for the next tick rather than flooding the room.
#define MELEE_DOT_BATCH 32

// Mid-range: the pit's decay is neither urgent nor background scavenging.
#define MELEE_DOT_PRIO  128

// One live affliction, due now, as the decay task sees it. `method` and
// `channel` come off the row itself: the task holds no command context
// and no round, and must address a room from a bare row.
typedef struct
{
  int64_t          id;
  int64_t          round_id;
  uint32_t         ns_id;
  char             method [MELEE_METHOD_SZ];
  char             channel[MELEE_CHAN_SZ];
  char             victim [MELEE_USER_SZ];
  char             victim_nick[MELEE_NICK_SZ];
  char             source [MELEE_USER_SZ];
  char             source_nick[MELEE_NICK_SZ];
  melee_dot_kind_t kind;
  bool             expired;   // this is the last tick it will ever take
} melee_dot_due_t;

// One decay tick, resolved and ready to be written.
typedef struct
{
  int64_t     dot_id;
  int64_t     round_id;
  uint32_t    ns_id;
  const char *victim;
  const char *source;
  int32_t     dmg;
  uint32_t    tick_secs;
  bool        last;      // the affliction is spent after this tick
  bool        fatal;     // this tick takes the victim to 0 hp
} melee_dot_hit_t;

// One affliction about to be inflicted. Like melee_blow_t, the `*_nick`
// names are display-only and the usernames are the identity of record.
typedef struct
{
  int64_t          round_id;
  uint32_t         ns_id;
  const char      *method;
  const char      *channel;
  const char      *victim;
  const char      *victim_nick;
  const char      *source;
  const char      *source_nick;
  melee_dot_kind_t kind;
  uint32_t         secs;       // total lifetime
  uint32_t         tick_secs;  // cadence, and so the first tick's delay
  uint32_t         stack_max;  // enforced in SQL, not read-then-write
} melee_dot_new_t;

// ---- Plugin core (melee.c) ----------------------------------------- //

// Serialises everything that mutates a round: the turn engine's steps
// 5-10 and, on the other side of the plugin, every decay tick — a tick
// decrements the same health a blow does. Defined in melee_cmds.c. The
// lock ordering is melee_turn_lock -> melee_pool_lock, never the
// reverse, and neither is ever held across a send.
extern pthread_mutex_t melee_turn_lock;

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

// Leave an affliction on a combatant. The stack cap is enforced inside
// the statement, so at the cap this lands no row and returns FAIL — the
// intended silence, not an error. SUCCESS means a row landed and the
// caller owes the room an inflict line.
bool melee_db_dot_inflict(const melee_dot_new_t *dot);

// Who in this round is currently afflicted, and with what. Returns the
// number of victims written, at most `cap`.
uint32_t melee_db_dot_marks(int64_t round_id, melee_dot_mark_t *out,
    uint32_t cap);

// Afflictions due a tick right now, oldest deadline first, at most
// `cap`. Only rows whose round is still active are returned — an
// affliction dies with its round.
uint32_t melee_db_dot_due(melee_dot_due_t *out, uint32_t cap);

// How many afflictions are still live in a still-active round. The
// decay task asks only when nothing was due, so that a long affliction
// waiting for its first tick cannot start the idle clock.
uint32_t melee_db_dot_live(void);

// Retire every live affliction whose round has ended or been abandoned.
// Without this the idle counter could never reach zero.
bool melee_db_dot_sweep(void);

// Retire one affliction the decay task could not act on — an absent
// method, a victim already dead.
bool melee_db_dot_cancel(int64_t dot_id);

// Write one decay tick as one transaction: the victim's health, both
// damage tallies in both tables, the affliction's own counters and next
// deadline, and — only when the tick is fatal — the round close and the
// kill/death tally. A tick never touches last_wave, blows, crits or the
// round's wave: decay is not a swing.
bool melee_db_dot_tick(const melee_dot_hit_t *hit);

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

// The heaviest damage the current tunables can roll — the reference the
// severity band is a percentage OF. Never returns 0; the band divides
// by it.
int32_t melee_dmg_ceiling(const melee_tunables_t *t);

// Which of the four damage tiers `dmg` belongs to, by percentage of
// melee_dmg_ceiling(). Never returns MELEE_FLAV_DEATH — a fatal blow
// still renders its own tier, and the death LINE is a separate pool.
melee_flavour_t melee_severity(const melee_tunables_t *t, int32_t dmg);

// Both renderers consult the flavour pool first and fall back to the
// static tables. `need_refill` (may be NULL) reports that the pool they
// drew from has reached its low-water mark; the CALLER kicks the refill,
// after the turn lock is released.
//
// The blow takes no `crit` flag: the words, the colour and the emoji all
// come from melee_severity(t, dmg). The crit roll still widens the
// damage band and still feeds the scoreboard — it simply no longer
// chooses the sentence.
void melee_render_blow(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp,
    int32_t hp_max, const melee_tunables_t *t, bool *need_refill);

void melee_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick, const melee_tunables_t *t, bool *need_refill);

// The trout: a critical blow that kills speaks one fixed sentence in
// place of the tier line. A static easter egg — never model-authored,
// never pooled, and so it takes no tunables and flags no refill.
void melee_render_trout(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg);

// How an affliction presents itself: the bare noun substituted into a
// line, the single-column glyph that marks a victim on the round card,
// and the colour both are drawn in. Out-of-range kinds return the first
// entry rather than reading past the tables.
// The line that announces a fresh affliction, spoken right after the
// blow that left it. It never names the duration: the pit does not tell
// you how long you have.
void melee_render_dot_inflict(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, melee_dot_kind_t kind);

// One tick of an affliction doing its slow work, and the tick that
// finishes what a blade started. The tick carries the survivor's health
// tally, exactly as a blow line does, so decay reads as a peer of a
// blow; the death line carries none.
void melee_render_dot_tick(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp, int32_t hp_max,
    melee_dot_kind_t kind, const melee_tunables_t *t, bool *need_refill);

void melee_render_dot_death(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, melee_dot_kind_t kind, const melee_tunables_t *t,
    bool *need_refill);

const char *melee_dot_name_of (melee_dot_kind_t kind);
const char *melee_dot_emoji_of(melee_dot_kind_t kind);
const char *melee_dot_color_of(melee_dot_kind_t kind);

// ---- The decay task (melee_dot.c) ---------------------------------- //

// Start the decay task if it is not already queued, and clear its idle
// clock either way. Called after an affliction lands, OUTSIDE the turn
// lock — the same discipline that keeps the flavour refill off the turn
// path.
void melee_dot_wake(void);

// Cancel the decay task and forget its handle. melee_deinit() MUST call
// this: a periodic callback pointing into an unloaded .so is a jump into
// freed memory on the next tick.
void melee_dot_stop(void);

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

// Fill all five pools at startup, so the pit is flavoured before the
// first blow rather than after it.
void melee_llm_prime(const melee_tunables_t *t);

// Throw away every pooled line and start again. Called when the operator
// changes what the model was told — the preamble file, or the model
// itself. Safe from any thread; takes melee_pool_lock only, and never
// holds it across the re-prime.
void melee_llm_invalidate(const char *why);

// Watch the two keys that decide what the model is told, so a change to
// either empties the pools. Installed in start(), and CLEARED in
// deinit(): the KV entries outlive the plugin, and a callback left
// pointing into an unloaded .so is a jump into freed memory on the next
// `set kv`.
void melee_llm_watch(bool on);

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

// Substitute {attacker}/{target}/{damage}/{affliction} into a bounded
// buffer, copying every other byte literally. Never hands `tmpl` to a
// printf conversion, and never rescans what it substituted — a `{`
// inside a nickname is data. The values arrive already colorized,
// exactly as they do for the static tables; NULL is legal for a token
// the category's line can never carry and expands to nothing.
void melee_tmpl_expand(char *out, size_t cap, const char *tmpl,
    const char *attacker, const char *target, const char *damage,
    const char *affliction);

// ---- Command surface (melee_cmds.c) -------------------------------- //

bool melee_commands_register(void);
void melee_commands_unregister(void);

// ---- The read-only views (melee_show.c) ---------------------------- //

// Attach `show melee` and `show melee scores` beneath the core `show`
// parent. Called from melee_commands_register().
bool melee_show_register(void);

#endif // MELEE_INTERNAL

#endif // BM_MELEE_H
