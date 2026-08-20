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

// Where the character sheets live. Named once so the schema entry and
// atk_tunables_load() cannot drift; an empty directory is not one, so
// `set kv --clear` on ATK_KV_CLASSES is refused and lands back here.
#define ATK_CLASSES_DIR    "../plugins/feature/attack/characters"

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

#define ATK_KV_ROUND_IDLE  "plugin.attack.round_max_idle_secs"
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
// A character sheet's stem, and the `class` column that stores it. It
// lives up here with the other storage bounds rather than down in the
// character-sheet block because a combatant's class is part of their
// round row, and atk_player_t needs the size.
#define ATK_CLASS_NAME_SZ  32
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
  uint32_t round_max_idle_secs; // seconds of silence before the game is over
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
  // Two clocks. `age` is seconds since started_at — the brawl's whole
  // life, shown when a pit is retired. `idle` is seconds since the last
  // !attack or !heal, and it is what ends a game: once it passes
  // round_max_idle_secs the next blow opens a fresh round. Any turn that
  // lands resets last_action, so a lively pit never goes stale.
  int64_t age;
  int64_t idle;
} atk_round_t;

// One combatant's standing within a round. `class` is the sheet stem
// they were dealt at enrolment and keep for the whole brawl; it decides
// which words they speak and nothing whatsoever about the numbers.
// `bonus_pct` is the ONLY number a combatant carries between turns, and
// it is spent — reset to zero — inside the same transaction as the turn
// that uses it. It scales the number a turn deals and never the tier the
// words come from, so a deferred combatant hits harder without ever
// speaking above their roll.
typedef struct
{
  char    nickname[ATK_NICK_SZ];
  char    class[ATK_CLASS_NAME_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t last_wave;
  int32_t defers;      // deferrals spent this round
  int32_t bonus_pct;   // additive bonus pending for the next turn
} atk_player_t;

// One resolved turn, ready to be written. The `*_user` names are
// namespace-scoped usernames and are the identity of record; the
// `*_nick` names are display-only and are refreshed on every blow.
//
// Two damage numbers travel together, exactly as they do on a heal.
// `dmg` is what the engine rolled: it decides the tier, fills the crit
// columns, and is the number the room is told, because it is the force
// of the swing. `dealt` is what the health bar actually lost. They
// differ on one blow only — the killing one, where the target had less
// left than the roll — and every damage TALLY takes `dealt`, for the
// same reason atk_db_heal_apply() refuses to credit overheal.
typedef struct
{
  int64_t     round_id;
  uint32_t    ns_id;
  const char *src_user;
  const char *src_nick;
  const char *tgt_user;
  const char *tgt_nick;
  int32_t     dmg;
  int32_t     dealt;
  int32_t     wave;      // the wave the attacker is spending
  bool        crit;
  bool        new_top;   // this crit is the round's heaviest so far
  bool        fatal;     // the target reaches 0 hp
} atk_blow_t;

// One combatant a sweep landed on: everybody left standing except
// whoever swung. `hp` is what they had when the blow went wide and
// `hp_left` what it left them with — two fields rather than one that
// changes meaning, because the announcement reads both.
typedef struct
{
  char    user[ATK_USER_SZ];
  char    nick[ATK_NICK_SZ];
  int32_t hp;        // before the blow
  int32_t hp_left;   // after it, floored at zero
  int32_t hp_max;
  bool    fell;      // this blow was the one that took them
} atk_victim_t;

// One blow that went wide. The damage is rolled ONCE and lands on
// everybody identically — a roll per victim would read as several blows
// rather than as one that missed its aim — and the bonus that scaled it
// was already spent before it arrived here.
//
// `tgt_user` is the combatant who was actually named: they take the same
// damage as everyone else and are the one the lifetime `best_crit_on`
// records, because they are who the attacker was swinging at.
//
// Unlike atk_blow_t this carries no `dealt`, because atk_victim_t already
// holds the health on both sides of the blow: what each combatant lost is
// `hp - hp_left`, and the attacker is credited with that sum. A third
// field would only restate what two of them already say.
typedef struct
{
  int64_t             round_id;
  uint32_t            ns_id;
  const char         *src_user;
  const char         *src_nick;
  const char         *tgt_user;
  int32_t             dmg;
  int32_t             wave;
  bool                crit;
  bool                new_top;   // this crit is the round's heaviest so far
  const atk_victim_t *victim;    // everyone it landed on, by username
  uint32_t            n_victim;
  // The fallen the round records, which is one name however many died:
  // the lowest resulting health, ties broken by username so the value is
  // deterministic. Empty when the sweep killed nobody.
  const char         *fallen;
  uint32_t            n_fallen;
} atk_sweep_t;

// One resolved heal, ready to be written. `amt` is what the engine
// rolled; `delta` is what the health bar will actually move once the
// overheal is clamped, and it is the only number the room is ever told —
// a line announcing 18 while the bar moved 4 is a lie the round card
// would contradict on the very next `show attack`.
typedef struct
{
  int64_t     round_id;
  uint32_t    ns_id;
  const char *src_user;
  const char *src_nick;
  const char *tgt_user;
  const char *tgt_nick;
  int32_t     amt;
  int32_t     delta;
  int32_t     wave;      // the wave the healer is spending
} atk_heal_t;

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
  char    class[ATK_CLASS_NAME_SZ];
  int32_t hp;
  int32_t hp_max;
  int32_t dmg_given;
  int32_t dmg_taken;
  int32_t heal_given;
  int32_t best_crit;
  int32_t last_wave;
  int32_t bonus_pct;   // pending deferral bonus; drawn in the ragged tail
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
  int64_t heal_given;
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
  // The class the source was dealt, joined off their round row: an
  // affliction ticks in the voice of whoever left it. Empty when that
  // combatant's row is gone, and the neutral lines answer instead.
  char           class[ATK_CLASS_NAME_SZ];
  char           noun[ATK_NOUN_SZ];  // what {affliction} expands to
  atk_dot_kind_t kind;
  int32_t        dmg_plan;  // the whole damage this affliction will deal
  int32_t        dmg_done;  // how much of it has already landed
  uint32_t       max_ticks; // ticks it was minted with
  uint32_t       ticks;     // ticks already taken
  bool           expired;   // this is the last tick it will ever take
  // Past expires_at — one cadence beyond the last tick it was ever
  // scheduled for. Only a row nobody serviced can be here, so it says
  // "this wound is over" whatever stranded it.
  bool           stale;
} atk_dot_due_t;

// One decay tick, resolved and ready to be written. `dmg` is this tick's
// share of the plan and `dealt` what the victim actually lost — the same
// split atk_blow_t carries, and it matters here for one tick only. A
// tick deals less than its share exactly when it reaches zero, and that
// tick is by construction the affliction's last, so `dmg_total` can
// stop short of `dmg_plan` on a kill but the schedule that reads it back
// never sees a capped value.
typedef struct
{
  int64_t     dot_id;
  int64_t     round_id;
  uint32_t    ns_id;
  const char *victim;
  const char *source;
  int32_t     dmg;
  int32_t     dealt;
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

// What became of an attempted affliction. The turn is spent on all three
// answers and every one of them owes the room a sentence, so the caller
// may not stop at a two-state test: LANDED sits at 0 so such a test comes
// out incomplete rather than wrong, separating "a row landed" from "it
// did not" and losing only WHICH refusal to speak. CAPPED is a rule of
// the game and UNWRITTEN is a fault in the machine; they must not read
// alike in the pit.
typedef enum
{
  ATK_INFLICT_LANDED = 0,  // a row landed, and the decay task is owed a wake
  ATK_INFLICT_CAPPED,      // the victim already carries dot.stack_max
  ATK_INFLICT_UNWRITTEN    // the ledger never took it
} atk_inflict_t;

// ---- Character sheets (attack_class.c) ----------------------------- //
//
// THE LAW, in one sentence: the engine owns every number, and a sheet
// owns only words plus one bucket-label per word saying which number
// that sentence is allowed to describe. Everything below exists to keep
// that true — see attack_class.c's preamble for the whole argument.

// ATK_CLASS_NAME_SZ is up with the storage bounds: a combatant's class is
// part of their round row, so atk_player_t needs it long before here.
#define ATK_CLASS_DESC_SZ  74          // 70 by the grammar, + NUL + slack
#define ATK_CLASSES_MAX    40          // registry slots
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
// This is the UNCONSTRAINED draw and it has exactly one caller left —
// atk_class_for(), which is substituting a voice rather than dealing a
// class. Enrolment deals through atk_class_deal() instead.
bool atk_class_pick(char *out, size_t cap);

// The class stems already dealt in one round. Uniqueness is a property of
// the ROUND, so this set is read off the roster once per turn and grown by
// each deal within it — never cached between turns, where a reload or a
// second room would make it a lie.
//
// It is bounded by the registry it filters rather than by the roster: a
// round can hold any number of combatants, but never more distinct classes
// than there are sheets to deal.
typedef struct
{
  uint32_t n;
  char     type[ATK_CLASSES_MAX][ATK_CLASS_NAME_SZ];
} atk_dealt_t;

// Record `type` as taken — idempotent, and a no-op once the set is full.
// The overflow can only ever under-constrain the next deal, which costs a
// duplicate and never a wrong class.
void atk_dealt_add(atk_dealt_t *dealt, const char *type);

// Deal a class no one in this round already holds, and record it in
// `dealt` so the next deal of the same turn avoids it too. A NULL set
// means "nothing is taken" and degrades to atk_class_pick().
//
// Uniqueness yields to the pit speaking at all: when every loaded sheet is
// already in the round the draw falls back to the whole registry, logs a
// CLAM_WARN and hands out a duplicate. A brawl on the built-in brawler has
// one class for everybody, and refusing the second combatant a sheet would
// end the game rather than constrain it.
void atk_class_deal(atk_dealt_t *dealt, char *out, size_t cap);

// The class a combatant should speak with right now: their own when the
// registry still carries it, a freshly picked one when it does not — a
// sheet can be deleted between the enrolment that stored the stem and the
// turn that reads it back, and a bookkeeping gap must never cost somebody
// their swing. Writes an empty string only when nothing is loaded at all.
//
// The substitute is drawn UNCONSTRAINED, and deliberately so: this is a
// voice for one turn, not an assignment. The roster row keeps the stem it
// was dealt, so the round's dealt classes stay unique on the card and in
// the ledger even while a deleted sheet is being spoken around.
void atk_class_for(const char *want, char *out, size_t cap);

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
const char *atk_fallback_blow(void);
const char *atk_fallback_heal(void);
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
//
// `class` is written on the INSERT only, so a combatant is dealt their
// sheet exactly once and can never re-roll it by being struck again.
//
// The caller must have drawn `class` through atk_class_deal() against a
// set this round's roster answered for: no two combatants in one round
// share a class, and the turn lock is the whole of what enforces it. The
// column carries no unique index — see attack_db.c.
bool atk_db_player_enrol(int64_t round_id, uint32_t ns_id,
    const char *username, const char *nickname, const char *class,
    int32_t hp);

bool atk_db_player_get(int64_t round_id, const char *username,
    atk_player_t *out);

// Every class already dealt in this round, into `out`. Returns the number
// written. A round nobody has enrolled in yet answers zero, which is the
// correct empty set and not an error.
uint32_t atk_db_classes_dealt(int64_t round_id, atk_dealt_t *out);

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

// Everyone still standing in this round except `except`, by username so
// the order is deterministic and the caller can pick the recorded fallen
// off the front of it. Returns the number written, at most `cap`.
uint32_t atk_db_living(int64_t round_id, const char *except,
    atk_victim_t *out, uint32_t cap);

// Write a blow that went wide as one transaction: every victim's health
// and damage taken, the attacker's whole tally at once, the round
// counters, every lifetime mirror, the wave advance, and — when anybody
// reached zero — the round close, one kill for the attacker and one death
// for each of the fallen. All or nothing, exactly as a single blow is.
//
// The caller has already resolved who fell and what it left them with:
// the arithmetic runs under the turn lock off the roster it read there,
// so the announcement and the ledger can never describe different blows.
bool atk_db_sweep_apply(const atk_sweep_t *sweep);

// Write a whole heal as one transaction: the target's health and both
// heal tallies, the healer's spent wave, the round's clock, and both
// lifetime score rows. The wave turns behind it exactly as it does behind
// a blow — a pit of healers would otherwise deadlock.
//
// The overheal is clamped in SQL (`LEAST(hp + amt, hp_max)`) and the
// tallies are credited `delta`, which the caller computed under the turn
// lock from the same row it is about to write. Announce `delta`.
bool atk_db_heal_apply(const atk_heal_t *heal);

// Spend a turn that deals no instant damage — a damage-over-time turn.
// The attacker still burns their swing and still bumps `blows`, and the
// wave still turns behind them, and any deferral bonus is consumed here
// too — the affliction spends the bonused roll one tick at a time, so
// leaving it standing would let one deferral pay twice. Deliberately not
// atk_db_blow_apply() with dmg = 0: a zero-damage blow reads ambiguously
// in the ledger and in the code, and this is not the place to be clever.
bool atk_db_turn_spend(int64_t round_id, uint32_t ns_id,
    const char *username, const char *nickname, int32_t wave);

// Surrender a turn for a bonus on the next one. One transaction: the
// deferral is counted, the accumulated bonus is written, and the wave is
// SPENT — a deferral that could not turn the wave would stall the whole
// pit behind one hesitant combatant. `bonus_pct` is the already-capped
// total the caller computed under the turn lock, never the step.
bool atk_db_defer_apply(int64_t round_id, const char *username,
    const char *nickname, int32_t wave, uint32_t bonus_pct);

// Leave an affliction on a combatant. The stack cap is enforced inside
// the statement, so at the cap the INSERT writes no row — and telling
// that apart from a statement that FAILED is this function's job, not
// the caller's: only here is it visible that the ledger ran and chose to
// write nothing. `dot.stack_max` is clamped to at least 1 before it
// arrives, so zero rows affected means the cap and can mean nothing else.
atk_inflict_t atk_db_dot_inflict(const atk_dot_new_t *dot);

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

// Retire every live affliction already past expires_at — a row stranded
// by a reload or a restart, which is the only way one gets here. START
// ONLY: it must never run while the decay task is queued. Returns how
// many rows it retired.
uint32_t atk_db_dot_reap_stale(void);

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

// One finished blow line. The split of responsibilities is the charter's:
//
//   `tier`  is the ENGINE's, derived from the roll before any sheet was
//           consulted, and it — not the move — chooses the colour and the
//           emoji. Passing it separately is what keeps the dressing
//           honest even when `move` is NULL.
//   `move`  is the SHEET's, and supplies only the sentence. NULL means
//           the attacker's class had nothing to say, and the engine's own
//           neutral line stands in.
//
// `bonus_pct` renders the deferral badge; 0 draws none.
void atk_render_blow(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_flavour_t tier, const atk_move_t *move,
    int32_t dmg, uint32_t bonus_pct, int32_t hp, int32_t hp_max);

// One finished heal line, dressed as the peer of a blow so that mending
// reads as one turn among turns rather than as a system notice. The split
// is the charter's, exactly as it is for a blow: the ENGINE rolled the
// band and the number, and `move` supplies only the sentence — NULL means
// the healer's class had nothing to say and the neutral line stands in.
//
// `amt` is the CLAMPED delta, never the roll: the number spoken and the
// movement of the health bar are the same thing or the card contradicts
// the line. `bonus_pct` renders the deferral badge; 0 draws none.
void atk_render_heal(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, const atk_move_t *move, int32_t amt,
    uint32_t bonus_pct, int32_t hp, int32_t hp_max);

// The line a sweep speaks after the class move has had its say: one
// number for everybody, then the roster it landed on with each
// combatant's remaining health. Class-agnostic by design, exactly as the
// deferral line is — going wide is a rule of the pit and not a move, so
// no sheet supplies words for it and none may. A roster too long for the
// line is cut with `… and N more` rather than truncated mid-nickname.
void atk_render_sweep(char *out, size_t cap, atk_flavour_t tier,
    int32_t dmg, const atk_victim_t *v, uint32_t n);

// The killing blow, in the slayer's own voice where their sheet has one.
// `move` NULL falls back to the engine's neutral death line.
void atk_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick, const atk_move_t *move);

// The deferral. Class-agnostic by design: stepping back is the engine's
// own mechanic, not a move, so no sheet supplies words for it and none
// may — the line names the bonus the surrender bought and nothing else.
void atk_render_defer(char *out, size_t cap, const char *nick,
    uint32_t bonus_pct);

// The trout: a critical blow that kills speaks one fixed sentence in
// place of the tier line. A static easter egg, and the one piece of
// flavour that is neither themed nor overridable.
void atk_render_trout(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg);

// The three lines an affliction speaks: the turn that inflicts it, one
// tick of it doing its slow work, and the tick that finishes what the
// wound started. The inflict and tick lines never name a duration — the
// pit does not tell you how long you have — and only the tick carries the
// survivor's health tally, exactly as a blow line does.
//
// Each takes the affliction's `kind` for its glyph and colour, the
// `noun` the sheet that inflicted it chose (empty falls back to the
// engine's plain word for that kind), and the class `move` supplying the
// sentence (NULL falls back to the engine's neutral line).
// `bonus_pct` renders the deferral badge on the inflict line; 0 draws
// none. The badge is the whole of what a deferred combatant gets to see
// on an affliction turn — the bonus has already scaled the roll the
// wound will pay out, and naming the roll here would tell the victim how
// long they have.
void atk_render_dot_inflict(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_dot_kind_t kind, const char *noun,
    const atk_move_t *move, uint32_t bonus_pct);

void atk_render_dot_tick(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp, int32_t hp_max,
    atk_dot_kind_t kind, const char *noun, const atk_move_t *move);

void atk_render_dot_death(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_dot_kind_t kind, const char *noun,
    const atk_move_t *move);

// How an affliction presents itself on screen: the single-column glyph
// that marks a victim on the round card, and the colour it and its
// damage are drawn in. The engine owns exactly these two; the NOUN
// belongs to the sheet, and atk_fallback_noun() covers a row that
// carries none. Out-of-range kinds return the first entry rather than
// reading past the tables.
const char *atk_dot_emoji_of(atk_dot_kind_t kind);
const char *atk_dot_color_of(atk_dot_kind_t kind);

// ---- The decay task (attack_dot.c) --------------------------------- //

// Pick up the afflictions a previous incarnation left behind: retire the
// ones already past expires_at, then queue the task only if a live fight
// survived. start() is the ONLY caller — the reap is safe precisely
// because the task cannot be running when it happens.
void atk_dot_resume(void);

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
