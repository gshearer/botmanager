#ifndef BM_ATTACK_H
#define BM_ATTACK_H

// botmanager — MIT
// attack: the duelling pit. `!attack <nick>` rolls damage against
// another registered user; hit points, waves, and per-round + lifetime
// scores persist in the caller's user namespace. The first death ends
// the round, and where the protocol allows it the fallen are removed
// from the room through the generic method eject primitive.
//
// ---- Public-facing: nothing. The plugin talks to the core exclusively
// through cmd_register / cmd_reply / db_* / kv_* / method_*. All
// declarations below are internal, gated on ATTACK_INTERNAL.

#ifdef ATTACK_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

#include <pthread.h>
#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define ATK_CTX            "attack"

// KV keys (registered under the plugin schema). Twenty-five, and the
// count is load-bearing: every number the pit uses is here, because a
// character sheet may not carry one.
#define ATK_KV_PREFIX      "plugin.attack.table_prefix"
#define ATK_KV_CLASSES     "plugin.attack.classes_path"
#define ATK_KV_START_HP    "plugin.attack.start_hp"

// Every damage roll is 1..dmg_max, uniform and class-blind. The tier is
// derived from the number AFTER it is rolled, which is what keeps class
// assignment cosmetic.
#define ATK_KV_DMG_MAX     "plugin.attack.dmg_max"

// Severity banding: the percentage of the heaviest possible blow at
// which each tier BEGINS. Below the first is minor.
#define ATK_KV_SEV_MEDIUM  "plugin.attack.sev.medium_at"
#define ATK_KV_SEV_MAJOR   "plugin.attack.sev.major_at"
#define ATK_KV_SEV_CRIT    "plugin.attack.sev.critical_at"

#define ATK_KV_ROUND_MAX   "plugin.attack.round_max_secs"
#define ATK_KV_EJECT       "plugin.attack.eject_on_death"
#define ATK_KV_SCORE_ROWS  "plugin.attack.scoreboard_rows"
#define ATK_KV_AOE_PCT     "plugin.attack.aoe_chance_pct"

// Damage over time: a turn that spends its whole rolled damage over
// several ticks instead of landing it at once. `dot.chance_pct` at 0
// turns the whole feature off; `dot.max_ticks` is the chattiness cap.
#define ATK_KV_DOT_CHANCE  "plugin.attack.dot.chance_pct"
#define ATK_KV_DOT_TICKS   "plugin.attack.dot.max_ticks"
#define ATK_KV_DOT_TICK    "plugin.attack.dot.tick_secs"
#define ATK_KV_DOT_STACK   "plugin.attack.dot.stack_max"
#define ATK_KV_DOT_LINGER  "plugin.attack.dot.linger_secs"

// Healing. The engine rolls whether a heal is minor or major and then
// how much; the sheet only supplies the sentence.
#define ATK_KV_HEAL_PCT    "plugin.attack.heal.major_pct"
#define ATK_KV_HEAL_MIN_LO "plugin.attack.heal.minor_min"
#define ATK_KV_HEAL_MIN_HI "plugin.attack.heal.minor_max"
#define ATK_KV_HEAL_MAJ_LO "plugin.attack.heal.major_min"
#define ATK_KV_HEAL_MAJ_HI "plugin.attack.heal.major_max"

// Deferral. Bonuses are ADDITIVE and capped: three deferrals buy at most
// double damage, never more, whatever defer.max is raised to.
#define ATK_KV_DEFER_MAX   "plugin.attack.defer.max"
#define ATK_KV_DEFER_LO    "plugin.attack.defer.step_min_pct"
#define ATK_KV_DEFER_HI    "plugin.attack.defer.step_max_pct"
#define ATK_KV_DEFER_CAP   "plugin.attack.defer.bonus_cap_pct"

// Storage bounds. The table prefix is a SQL identifier, so it is
// validated as strict alnum/underscore before it can reach a query.
#define ATK_PREFIX_SZ      32                 // table-name prefix
#define ATK_TABLE_SZ       64                 // prefix + "_players"
#define ATK_USER_SZ        USERNS_USER_SZ     // 31
#define ATK_NICK_SZ        METHOD_NICKNAME_SZ // 64
#define ATK_CHAN_SZ        METHOD_CHANNEL_SZ  // 128
#define ATK_METHOD_SZ      64                 // method instance name
#define ATK_MAX_PLAYERS    32                 // per round, for the show card
// The ceiling atk_tunables_load() clamps scoreboard_rows to. The
// leaderboard's row array is sized from this, so the two must agree.
#define ATK_MAX_SCORE_ROWS 50
// The character-sheet directory, as configured. Relative paths resolve
// against the daemon's CWD (build/), which is why the default is one.
#define ATK_PATH_SZ        256
// The bare noun a line substitutes for {affliction}. 24 by the grammar,
// plus slack; the column that stores it is VARCHAR(32).
#define ATK_NOUN_SZ        32
#define ATK_LINE_SZ        640                // one rendered, colorized line
// A comma-joined display list. Deliberately well under ATK_LINE_SZ so
// it always fits inside the sentence that carries it; a roster longer
// than this would not survive an IRC line anyway.
#define ATK_ROSTER_SZ      320

// One un-expanded move, as a character sheet writes it. The arithmetic
// that fixes this number, and that ATK_LINE_SZ above is sized from:
//
//   ATK_LINE_SZ must hold the EXPANDED line. Expansion replaces
//   {attacker}/{target} (10 and 8 bytes) with a colorized nickname
//   (ATK_NICK_SZ 63 + ~10 of colour ~= 73 each), {damage} or {heal}
//   (8 or 6 bytes) with a colorized number (~20), and {affliction}
//   (12 bytes) with a SHEET-SUPPLIED noun of up to 24 bytes plus ~10 of
//   colour. Worst-case growth ~= +175 bytes. atk_render_blow() then adds
//   the emoji prefix, the deferral badge (<= 10) and the
//   " [nick — hp/hp hp]" tail, ~= +100.  256 + 175 + 100 = 531 of 640.
//
// Do not shrink either constant, and do not lengthen the noun bound,
// without redoing that sum.
#define ATK_MOVE_SZ        256

// The seven line categories: four damage tiers, the killing blow, and
// the two an affliction speaks. The order is used as an array index AND
// as the severity ordering itself (MINOR < MEDIUM < MAJOR < CRITICAL);
// keep the enum and every table keyed by it in step.
//
// The four damage tiers must stay contiguous and ascending — that is
// what atk_severity() returns as an ordering, and the tables sized
// ATK_FLAV_DEATH cover exactly them. The tail beyond them is three
// non-tier categories in no particular order; nothing keys off any one
// of them being last.
typedef enum
{
  ATK_FLAV_MINOR = 0,
  ATK_FLAV_MEDIUM,
  ATK_FLAV_MAJOR,
  ATK_FLAV_CRITICAL,
  ATK_FLAV_DEATH,
  ATK_FLAV_DOT_TICK,
  ATK_FLAV_DOT_DEATH,
  ATK_FLAV__COUNT
} atk_flavour_t;

// The afflictions a blow may leave behind. The kind is chosen at inflict
// time and stored on the row, because it is what makes the words
// specific: the renderer substitutes its name rather than keeping a
// separate table per affliction.
//
// The engine owns the KIND — a glyph and a colour, and nothing else. The
// NOUN belongs to the character sheet, so the same `rot` may be a
// gnawing skeleton to one class and a nanite bloom to another. All eight
// are deliberately plain: a sword, a spell and a plasma coil must all be
// able to use them.
typedef enum
{
  ATK_DOT_BLEED = 0,
  ATK_DOT_POISON,
  ATK_DOT_BURN,
  ATK_DOT_ROT,
  ATK_DOT_CHILL,
  ATK_DOT_CURSE,
  ATK_DOT_DRAIN,
  ATK_DOT_SHOCK,
  ATK_DOT__COUNT
} atk_dot_kind_t;

// Damage-over-time lifecycle, as stored in <prefix>_dots.state.
#define ATK_DOT_LIVE      0   // still ticking
#define ATK_DOT_SPENT     1   // ran its course, or landed the death blow
#define ATK_DOT_CANCELLED 2   // the round ended out from under it

// Round lifecycle, as stored in <prefix>_rounds.state.
#define ATK_ROUND_ACTIVE    0
#define ATK_ROUND_ENDED     1
#define ATK_ROUND_ABANDONED 2

// Every knob an operator can turn, already sanitised. Read once per
// command with atk_tunables_load(); never read the raw KV at a use
// site — an operator can set dmg_max to 0 or invert a band.
//
// This struct is the WHOLE of the pit's arithmetic. A character sheet
// supplies words and one bucket label per word; it may never supply a
// number, a weight or a bonus, and there is deliberately nowhere here
// for a per-class value to live.
typedef struct
{
  char     classes_path[ATK_PATH_SZ]; // where the character sheets live
  uint32_t start_hp;         // hit points each combatant enters with
  uint32_t dmg_max;          // every damage roll is 1..dmg_max
  uint32_t sev_medium_at;    // pct of the heaviest blow: medium begins
  uint32_t sev_major_at;     // ... major begins    (> sev_medium_at)
  uint32_t sev_crit_at;      // ... critical begins (> sev_major_at)
  uint32_t round_max_secs;   // a brawl's whole life; then the game is over
  uint32_t scoreboard_rows;  // rows shown by `show attack scores`
  uint32_t aoe_pct;          // chance a damage turn sweeps the pit
  bool     eject_on_death;   // remove the fallen where the method allows

  // Damage over time. `dot_chance_pct` 0 is the off switch and is
  // checked before anything else on the inflict path. A DOT carries no
  // damage budget of its own: it spreads the ordinary roll across its
  // ticks, so afflictions are a difference in rhythm, never in strength.
  uint32_t dot_chance_pct;   // percent chance a turn is a DOT
  uint32_t dot_max_ticks;    // the chattiness cap: most ticks one may speak
  uint32_t dot_tick_secs;    // seconds between decay ticks
  uint32_t dot_stack_max;    // afflictions one victim may carry at once
  uint32_t dot_linger_secs;  // decay task's idle life after the last DOT

  // Healing. Which band a heal lands in is the engine's roll, not the
  // healer's; the sheet supplies words for whichever came up.
  uint32_t heal_major_pct;   // chance a heal is major rather than minor
  uint32_t heal_minor_min;   // minor heal floor
  uint32_t heal_minor_max;   // minor heal ceiling (>= heal_minor_min)
  uint32_t heal_major_min;   // major heal floor
  uint32_t heal_major_max;   // major heal ceiling (>= heal_major_min)

  // Deferral. Additive and capped — multiplicative stacking would be
  // abused, and the cap is what forbids a one-shot.
  uint32_t defer_max;        // deferrals one combatant may spend per round
  uint32_t defer_step_lo;    // smallest bonus one deferral adds
  uint32_t defer_step_hi;    // largest (>= defer_step_lo)
  uint32_t defer_cap_pct;    // hard ceiling on accumulated bonus
} atk_tunables_t;

// The four table names for the configured prefix, resolved together so
// the prefix is validated exactly once per operation.
typedef struct
{
  char rounds [ATK_TABLE_SZ];
  char players[ATK_TABLE_SZ];
  char scores [ATK_TABLE_SZ];
  char dots   [ATK_TABLE_SZ];
} atk_tables_t;

// A brawl in progress, as the turn engine needs to see it.
typedef struct
{
  int64_t id;
  int32_t wave;
  int32_t blows;
  int32_t top_crit;
  // Seconds since started_at. There is exactly one clock: a brawl lasts
  // round_max_secs and then the game is over. Nothing measures silence —
  // `attack --end` is what retires a pit that has gone stale, and it is
  // a better instrument because the people standing in it can see it.
  int64_t age;
} atk_round_t;

// One combatant's standing within a round.
typedef struct
{
  char    nickname[ATK_NICK_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t last_wave;
} atk_player_t;

// One resolved turn, ready to be written. The `*_user` names are
// namespace-scoped usernames and are the identity of record; the
// `*_nick` names are display-only and are refreshed on every blow.
typedef struct
{
  int64_t     round_id;
  uint32_t    ns_id;
  const char *src_user;
  const char *src_nick;
  const char *tgt_user;
  const char *tgt_nick;
  int32_t     dmg;
  int32_t     wave;      // the wave the attacker is spending
  bool        crit;
  bool        new_top;   // this crit is the round's heaviest so far
  bool        fatal;     // the target reaches 0 hp
} atk_blow_t;

// ---- The read-only views (show attack, show attack scores) ---------- //
//
// These three carry display names, never identities: `name` is the last
// nickname the pit saw, falling back to the username. Nothing here is
// ever fed back into a query.

// The header of one round, live or finished.
typedef struct
{
  int64_t id;
  char    channel[ATK_CHAN_SZ];
  int32_t state;                 // ATK_ROUND_*
  int32_t wave;
  int32_t blows;
  int64_t length;                // seconds start -> end, or -> now
  int32_t top_crit;              // heaviest critical of the round, 0 = none
  char    top_by[ATK_USER_SZ];
  char    top_on[ATK_USER_SZ];
  char    slayer[ATK_USER_SZ]; // set once the round has been won
  char    fallen[ATK_USER_SZ];
} atk_card_t;

// One combatant's row on the round card. `user` is the identity the
// affliction markers are matched on — it is never rendered.
typedef struct
{
  char    name[ATK_NICK_SZ];
  char    user[ATK_USER_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t dmg_given;
  int32_t dmg_taken;
  int32_t best_crit;
  int32_t last_wave;
} atk_card_row_t;

// One combatant's row on the lifetime leaderboard.
typedef struct
{
  char    name[ATK_NICK_SZ];
  int32_t rounds;
  int32_t kills;
  int32_t deaths;
  int64_t dmg_given;
  int64_t dmg_taken;
  int32_t crits;
  int32_t best_crit;
} atk_score_row_t;

// The afflictions one combatant carries right now, for the round card.
// `n` is bounded by the stack cap, which atk_tunables_load() clamps to
// ATK_DOT_STACK_CAP.
#define ATK_DOT_STACK_CAP 4

typedef struct
{
  char           victim[ATK_USER_SZ];
  atk_dot_kind_t kinds[ATK_DOT_STACK_CAP];
  uint32_t       n;
} atk_dot_mark_t;

// How many afflictions one decay iteration may service — and so how
// much it may say into a channel in one pass. Anything over the bound
// waits for the next tick rather than flooding the room.
#define ATK_DOT_BATCH 32

// Mid-range: the pit's decay is neither urgent nor background scavenging.
#define ATK_DOT_PRIO  128

// One live affliction, due now, as the decay task sees it. `method` and
// `channel` come off the row itself: the task holds no command context
// and no round, and must address a room from a bare row.
typedef struct
{
  int64_t        id;
  int64_t        round_id;
  uint32_t       ns_id;
  char           method [ATK_METHOD_SZ];
  char           channel[ATK_CHAN_SZ];
  char           victim [ATK_USER_SZ];
  char           victim_nick[ATK_NICK_SZ];
  char           source [ATK_USER_SZ];
  char           source_nick[ATK_NICK_SZ];
  char           noun[ATK_NOUN_SZ];  // what {affliction} expands to
  atk_dot_kind_t kind;
  int32_t        dmg_plan;  // the whole damage this affliction will deal
  int32_t        dmg_done;  // how much of it has already landed
  uint32_t       max_ticks; // ticks it was minted with
  uint32_t       ticks;     // ticks already taken
  bool           expired;   // this is the last tick it will ever take
} atk_dot_due_t;

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
} atk_dot_hit_t;

// One affliction about to be inflicted. Like atk_blow_t, the `*_nick`
// names are display-only and the usernames are the identity of record.
typedef struct
{
  int64_t        round_id;
  uint32_t       ns_id;
  const char    *method;
  const char    *channel;
  const char    *victim;
  const char    *victim_nick;
  const char    *source;
  const char    *source_nick;
  const char    *noun;       // the sheet's own word for this affliction
  atk_dot_kind_t kind;
  // The whole rolled damage, spread across `max_ticks` instead of
  // landing at once. A DOT has no budget of its own, which is what makes
  // "some classes have DOTs" a difference in rhythm and not in strength.
  int32_t        dmg_plan;
  uint32_t       max_ticks;  // min(dot.max_ticks, dmg_plan)
  uint32_t       tick_secs;  // cadence, and so the first tick's delay
  uint32_t       stack_max;  // enforced in SQL, not read-then-write
} atk_dot_new_t;

// ---- Character sheets (attack_class.c) ----------------------------- //
//
// THE LAW, in one sentence: the engine owns every number, and a sheet
// owns only words plus one bucket-label per word saying which number
// that sentence is allowed to describe. Everything below exists to keep
// that true — see attack_class.c's preamble for the whole argument.

#define ATK_CLASS_NAME_SZ  32          // sheet stem, and the `class` column
#define ATK_CLASS_DESC_SZ  64          // 60 by the grammar, + NUL + slack
#define ATK_CLASSES_MAX    32          // registry slots
#define ATK_MOVES_MAX     128          // per section, the hard array bound
#define ATK_MOVE_GROW      32          // moves one mem_realloc adds
#define ATK_SHEET_SZ    (128 * 1024)   // one sheet, read whole
#define ATK_STEM_MIN         5         // "x.txt": the shortest legal name
// A sheet's FILE name, not its stem: the longest legal stem plus ".txt".
// Sized from the stem so the two can never drift apart — a report that
// truncated the extension off a rejected file would be naming a file
// that is not there.
#define ATK_SHEET_FILE_SZ  (ATK_CLASS_NAME_SZ + 4)

// The six sections of a sheet, in the order the grammar lists them.
// Used as an array index; keep atk_sec_meta[] in attack_class.c in step.
typedef enum
{
  ATK_SEC_DAMAGE = 0,
  ATK_SEC_DOT,
  ATK_SEC_HEAL,
  ATK_SEC_DEATH,
  ATK_SEC_DECAY,
  ATK_SEC_DECAY_KILL,
  ATK_SEC__COUNT
} atk_section_t;

// One move, exactly as a sheet wrote it: a sentence, the label saying
// which roll it may describe, and — for an affliction — the class's own
// word for the thing it left behind. There is deliberately nowhere here
// for a number, a weight or a cooldown to live.
typedef struct
{
  char           text[ATK_MOVE_SZ];
  char           noun[ATK_NOUN_SZ];  // ATK_SEC_DOT only
  atk_flavour_t  tier;               // DAMAGE + HEAL only
  atk_dot_kind_t kind;               // DOT / DECAY / DECAY_KILL only
} atk_move_t;

// One loaded class. The move arrays are heap, grown in blocks of
// ATK_MOVE_GROW; the whole struct is owned by the registry and is
// replaced wholesale on a reload, never edited in place.
typedef struct
{
  char        type[ATK_CLASS_NAME_SZ];
  char        desc[ATK_CLASS_DESC_SZ];
  atk_move_t *move[ATK_SEC__COUNT];
  uint16_t    n   [ATK_SEC__COUNT];
  uint16_t    cap [ATK_SEC__COUNT];
  bool        builtin;               // the compiled-in fallback brawler
} atk_class_t;

// One row of `show attack classes`. A copy, taken under the registry
// lock, so the view never renders from a registry a reload may swap.
typedef struct
{
  char     type[ATK_CLASS_NAME_SZ];
  char     desc[ATK_CLASS_DESC_SZ];
  uint16_t n[ATK_SEC__COUNT];
  bool     builtin;
} atk_class_info_t;

// What one atk_class_load() did, for the reload verb to report. Every
// rejected sheet is named: a sheet that silently failed to load is a
// class nobody can be dealt, and the author deserves to hear about it.
typedef struct
{
  uint32_t accepted;
  uint32_t rejected;
  uint32_t n_bad;                                    // names actually kept
  char     bad[ATK_CLASSES_MAX][ATK_SHEET_FILE_SZ];
} atk_load_report_t;

// Re-scan the sheet directory and swap in a whole new registry. The scan
// and the parse happen OUTSIDE the registry lock; only the pointer swap
// is under it. `rep` may be NULL. Returns the number accepted — 1 with
// `builtin` set when the directory yielded nothing at all, because the
// pit must still speak on a daemon where the sheets were never deployed.
uint32_t atk_class_load(atk_load_report_t *rep);

// Drop the registry. atk_deinit() calls this before unregistering the
// commands.
void atk_class_free(void);

// A random class stem, uniformly drawn. SUCCESS when one was written.
bool atk_class_pick(char *out, size_t cap);

// Draw a random move of `sec` from `type`, filtered by `tier` for DAMAGE
// and HEAL, by `kind` for DECAY and DECAY_KILL, and by neither for DOT
// and DEATH; pass 0 for the unused selector.
//
// FAIL means the matching subset is empty, and the caller must then use
// the engine's neutral fallback — NEVER another tier. Substituting a
// tier would bend the damage distribution, which is the one thing the
// loader's coverage gate exists to prevent.
bool atk_class_move(const char *type, atk_section_t sec,
    atk_flavour_t tier, atk_dot_kind_t kind, atk_move_t *out);

// Does this class own any move of this section at all? A natural
// predicate, not SUCCESS/FAIL: true means yes.
bool atk_class_has(const char *type, atk_section_t sec);

// Copy the registry into `out`, alphabetical. Returns rows written.
uint32_t atk_class_list(atk_class_info_t *out, uint32_t cap);

// Expand {attacker} {target} {damage} {heal} {affliction} in `tmpl`.
// A NULL substitution is a token the loader guaranteed absent from this
// section — reachable only through a bug, and then harmlessly: it
// expands to nothing rather than leaving a brace token on screen.
void atk_macro_expand(char *out, size_t cap, const char *tmpl,
    const char *attacker, const char *target, const char *damage,
    const char *heal, const char *affliction);

// The neutral fallbacks: one plain, setting-free line for each thing a
// sheet may leave unsaid. These are printf templates, not macro
// templates — the FORMAT CONTRACT is in attack_class.c beside them and a
// miscounted slot is a crash, not a typo. Never NULL.
const char *atk_fallback_death(void);
const char *atk_fallback_dot_inflict(atk_dot_kind_t kind);
const char *atk_fallback_decay(atk_dot_kind_t kind);
const char *atk_fallback_decay_kill(atk_dot_kind_t kind);

// The bare noun an affliction of this kind carries when the sheet that
// inflicted it supplied none.
const char *atk_fallback_noun(atk_dot_kind_t kind);

// ---- Plugin core (attack.c) ---------------------------------------- //

// Serialises everything that mutates a round: the turn engine's steps
// 5-10 and, on the other side of the plugin, every decay tick — a tick
// decrements the same health a blow does. Defined in attack_cmds.c, and
// never held across a send.
extern pthread_mutex_t atk_turn_lock;

void atk_tunables_load(atk_tunables_t *out);

// ---- DB layer (attack_db.c) ---------------------------------------- //

// Resolve + validate the configured table names into `out`. FAIL when
// the KV prefix is not a safe SQL identifier.
bool atk_tables_resolve(atk_tables_t *out);

// Ensure the three tables + their indexes exist. Idempotent; runs once.
bool atk_schema_ensure(void);

// The active round in this room, if there is one.
bool atk_db_round_find(uint32_t ns_id, const char *method,
    const char *channel, atk_round_t *out);

// Retire a round nobody came back to.
bool atk_db_round_abandon(int64_t round_id);

// Open a round. Returns the new id (>0) or -1.
int64_t atk_db_round_open(uint32_t ns_id, const char *method,
    const char *channel, const char *opener);

// Enrol a combatant at full health, bumping <p>_scores.rounds iff they
// were not already in this round. Returns true when newly enrolled.
bool atk_db_player_enrol(int64_t round_id, uint32_t ns_id,
    const char *username, const char *nickname, int32_t hp);

bool atk_db_player_get(int64_t round_id, const char *username,
    atk_player_t *out);

// Comma-joined display names of the living who have not swung in
// `wave`. Writes an empty string when nobody is pending.
bool atk_db_pending(int64_t round_id, int32_t wave, char *out,
    size_t cap);

// Write a whole turn as one transaction: both combatants, the round
// counters, both lifetime score rows, the wave advance, and — when the
// blow is fatal — the round close and the kill/death tally. All or
// nothing; a daemon death mid-turn can never leave the attacker charged
// for a blow the target never took.
bool atk_db_blow_apply(const atk_blow_t *blow);

// Leave an affliction on a combatant. The stack cap is enforced inside
// the statement, so at the cap this lands no row and returns FAIL — the
// intended silence, not an error. SUCCESS means a row landed and the
// caller owes the room an inflict line.
bool atk_db_dot_inflict(const atk_dot_new_t *dot);

// Who in this round is currently afflicted, and with what. Returns the
// number of victims written, at most `cap`.
uint32_t atk_db_dot_marks(int64_t round_id, atk_dot_mark_t *out,
    uint32_t cap);

// Afflictions due a tick right now, oldest deadline first, at most
// `cap`. Only rows whose round is still active are returned — an
// affliction dies with its round.
uint32_t atk_db_dot_due(atk_dot_due_t *out, uint32_t cap);

// How many afflictions are still live in a still-active round. The
// decay task asks only when nothing was due, so that a long affliction
// waiting for its first tick cannot start the idle clock.
uint32_t atk_db_dot_live(void);

// Retire every live affliction whose round has ended or been abandoned.
// Without this the idle counter could never reach zero.
bool atk_db_dot_sweep(void);

// Retire one affliction the decay task could not act on — an absent
// method, a victim already dead.
bool atk_db_dot_cancel(int64_t dot_id);

// Write one decay tick as one transaction: the victim's health, both
// damage tallies in both tables, the affliction's own counters and next
// deadline, and — only when the tick is fatal — the round close and the
// kill/death tally. A tick never touches last_wave, blows, crits or the
// round's wave: decay is not a swing.
bool atk_db_dot_tick(const atk_dot_hit_t *hit);

// ---- DB reads for the views (attack_db.c) -------------------------- //

// The round `show attack` should describe: the room's active round when
// there is one, else the room's most recent brawl. An empty `channel`
// (a direct message has no room) widens the search to the whole
// namespace. false when the namespace has never fought here.
bool atk_db_card_find(uint32_t ns_id, const char *method,
    const char *channel, atk_card_t *out);

// Fill `out` with up to `cap` combatants, healthiest first. `total` (may
// be NULL) reports how many the round actually holds, so the caller can
// note what the cap left out. Returns the number written.
uint32_t atk_db_card_roster(int64_t round_id, atk_card_row_t *out,
    uint32_t cap, uint32_t *total);

// Lifetime standings for a namespace, heaviest damage dealt first.
// Returns the number written, at most min(cap, limit).
uint32_t atk_db_scores(uint32_t ns_id, uint32_t limit,
    atk_score_row_t *out, uint32_t cap);

// The single heaviest critical anyone in the namespace has landed.
// false when no crit has ever been struck.
bool atk_db_deadliest(uint32_t ns_id, char *by, size_t by_cap,
    char *on, size_t on_cap, int32_t *dmg);

// ---- Combat (attack_combat.c) -------------------------------------- //

// Roll one blow: 1..dmg_max, uniform, and identical for everybody. This
// happens BEFORE the attacker's class is consulted — the number decides
// which sentence the sheet is asked for, never the other way round. It
// takes no crit flag because there is no longer a crit roll: a blow is
// critical iff its tier is, which is what stops the round card from ever
// contradicting the words.
int32_t atk_roll(const atk_tunables_t *t);

// The heaviest damage the current tunables can roll — the reference the
// severity band is a percentage OF. Never returns 0; the band divides
// by it.
int32_t atk_dmg_ceiling(const atk_tunables_t *t);

// Which of the four damage tiers `dmg` belongs to, by percentage of
// atk_dmg_ceiling(). Never returns ATK_FLAV_DEATH — a fatal blow
// still renders its own tier, and the death LINE is a separate table.
atk_flavour_t atk_severity(const atk_tunables_t *t, int32_t dmg);

// The blow takes no `crit` flag: the words, the colour and the emoji all
// come from atk_severity(t, dmg). The crit roll still widens the
// damage band and still feeds the scoreboard — it simply no longer
// chooses the sentence.
void atk_render_blow(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp,
    int32_t hp_max, const atk_tunables_t *t);

void atk_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick);

// The trout: a critical blow that kills speaks one fixed sentence in
// place of the tier line. A static easter egg, and the one piece of
// flavour that is neither themed nor overridable.
void atk_render_trout(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg);

// The line that announces a fresh affliction, spoken right after the
// blow that left it. It never names the duration: the pit does not tell
// you how long you have.
void atk_render_dot_inflict(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_dot_kind_t kind);

// One tick of an affliction doing its slow work, and the tick that
// finishes what a blade started. The tick carries the survivor's health
// tally, exactly as a blow line does, so decay reads as a peer of a
// blow; the death line carries none.
void atk_render_dot_tick(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp, int32_t hp_max,
    atk_dot_kind_t kind);

void atk_render_dot_death(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_dot_kind_t kind);

// How an affliction presents itself on screen: the single-column glyph
// that marks a victim on the round card, and the colour it and its
// damage are drawn in. The engine owns exactly these two; the NOUN
// belongs to the sheet, and atk_fallback_noun() covers a row that
// carries none. Out-of-range kinds return the first entry rather than
// reading past the tables.
const char *atk_dot_emoji_of(atk_dot_kind_t kind);
const char *atk_dot_color_of(atk_dot_kind_t kind);

// ---- The decay task (attack_dot.c) --------------------------------- //

// Start the decay task if it is not already queued, and clear its idle
// clock either way. Called after an affliction lands, OUTSIDE the turn
// lock — the turn path never touches the task system while it holds it.
void atk_dot_wake(void);

// Cancel the decay task and forget its handle. atk_deinit() MUST call
// this: a periodic callback pointing into an unloaded .so is a jump into
// freed memory on the next tick.
void atk_dot_stop(void);

// ---- Command surface (attack_cmds.c) ------------------------------- //

bool atk_commands_register(void);
void atk_commands_unregister(void);

// ---- The read-only views (attack_show.c) --------------------------- //

// What the four damage tiers actually pay at the current tunables, read
// back out of atk_severity() itself one damage value at a time rather
// than re-derived from the KV percentages — so the line can never
// disagree with the renderer. Not static: ATK-4 gives it its home under
// `show attack classes`, where a reader asking what a class's `critical`
// moves are worth can find it.
void atk_flav_bands(const cmd_ctx_t *ctx, const atk_tunables_t *t);

// Attach `show attack` and `show attack scores` beneath the core `show`
// parent. Called from atk_commands_register().
bool atk_show_register(void);

#endif // ATTACK_INTERNAL

#endif // BM_ATTACK_H
