// botmanager — MIT
// attack character sheets: the loader, the registry, the macro expander,
// and the neutral fallback lines a sheet may leave unsaid.
//
// ------------------------------------------------------------------ //
// THE FAIRNESS CHARTER — read this before changing anything here      //
// ------------------------------------------------------------------ //
//
// THE LAW:
//
//   The engine owns every number. A sheet owns only words, and one
//   bucket-label per word saying which number that sentence is allowed
//   to describe.
//
// Concretely, and in this order, on every damage turn:
//
//   1. The engine rolls damage first: 1..dmg_max, uniform, class-blind,
//      identical for everybody.
//   2. The engine derives the tier from that number, through
//      atk_severity().
//   3. ONLY THEN does the class matter: a random move OF THAT TIER is
//      drawn from the attacker's sheet.
//
// So a sheet with forty `critical` lines and a sheet with one hit
// exactly as hard as each other. The move count changes variety, never
// power. Reordering steps 1 and 3 reintroduces precisely the unfairness
// this file exists to prevent.
//
// That is why the loader's coverage gate below is a HARD REJECTION and
// not a warning: [damage] must carry at least one move in every one of
// the four tiers, and [heal], if present, at least one minor and one
// major. A class missing a tier could not speak a roll the engine is
// guaranteed to produce, and any fallback to a neighbouring tier would
// bend the damage distribution.
//
// And it is why the grammar has NO numeric field and must never grow
// one — no probability, no weight, no bonus, no resistance, no
// cooldown. If per-class balance is ever wanted, that is a new
// initiative with a new charter, not a new field.
//
// THE TWO RESIDUAL ASYMMETRIES, stated honestly rather than hidden:
//
//   1. A DOT can be cancelled. If the round ends out from under it, its
//      unspent damage is lost. Against that, a DOT can land a kill after
//      its author has run out of turns; the two roughly cancel. Do NOT
//      "fix" this by exempting afflictions from the sweep — one speaking
//      into a finished round is worse.
//   2. Heal availability is a real capability difference. A cleric can
//      spend a turn restoring hit points and a wizard cannot. It costs a
//      turn, so it is a trade rather than a bonus, but it is not
//      nothing. It is deliberate, it is the operator's own brief, and it
//      is the ONLY place a class is more than cosmetic. Everything above
//      exists to keep it that way.
//
// Anyone who later adds a numeric field to the grammar has to delete
// this comment to do it.

#define ATTACK_INTERNAL
#include "attack.h"

#include "alloc.h"
#include "util.h"
#include "validate.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// The grammar                                                         //
// ------------------------------------------------------------------ //

// The only tokens a sheet may carry. The sanitiser and the expander each
// name them explicitly; they are kept together so the two can never
// drift apart unnoticed.
#define ATK_TOK_ATTACKER "attacker"
#define ATK_TOK_TARGET   "target"
#define ATK_TOK_DAMAGE   "damage"
#define ATK_TOK_HEAL     "heal"
#define ATK_TOK_AFFLICT  "affliction"

#define ATK_TOK__COUNT   5

// The longest a sheet's affliction noun may be, by the grammar. Shorter
// than ATK_NOUN_SZ on purpose: the column that stores it is VARCHAR(32)
// and the slack is what keeps a rendered line inside its bound.
#define ATK_NOUN_MAX     24

// The longest a description may be, by the brief.
#define ATK_DESC_MAX     60

static const char *const atk_tok_name[ATK_TOK__COUNT] = {
  ATK_TOK_ATTACKER, ATK_TOK_TARGET, ATK_TOK_DAMAGE, ATK_TOK_HEAL,
  ATK_TOK_AFFLICT
};

// One section's whole shape: what it is called, how many `|`-separated
// fields its lines carry, which label it leads with, and — the rule that
// replaced every special case — exactly how many times each token must
// appear in its text. No more and no less.
//
// A death line carries no number, because the blow that killed already
// said it. An affliction line names its wound and a blow line must not.
// A heal line names hit points restored and never damage dealt.
typedef struct
{
  const char *name;                // as it appears in [brackets]
  uint8_t     fields;              // 1, 2 or 3
  bool        has_tier;            // leads with minor/medium/major/critical
  bool        has_kind;            // leads with an affliction kind
  bool        has_noun;            // carries the sheet's own noun
  uint8_t     tok[ATK_TOK__COUNT]; // attacker target damage heal affliction
} atk_sec_meta_t;

static const atk_sec_meta_t atk_sec_meta[ATK_SEC__COUNT] = {
  [ATK_SEC_DAMAGE]     = { "damage",     2, true,  false, false,
                           { 1, 1, 1, 0, 0 } },
  [ATK_SEC_DOT]        = { "dot",        3, false, true,  true,
                           { 1, 1, 0, 0, 1 } },
  [ATK_SEC_HEAL]       = { "heal",       2, true,  false, false,
                           { 1, 1, 0, 1, 0 } },
  [ATK_SEC_DEATH]      = { "death",      1, false, false, false,
                           { 1, 1, 0, 0, 0 } },
  [ATK_SEC_DECAY]      = { "decay",      2, false, true,  false,
                           { 1, 1, 1, 0, 1 } },
  [ATK_SEC_DECAY_KILL] = { "decay-kill", 2, false, true,  false,
                           { 1, 1, 0, 0, 1 } },
};

// The four damage tiers by name. Indexed by atk_flavour_t, and only the
// first four entries are ever addressed: a tier label is what a sheet
// writes, and the three non-tier categories have no label of their own.
static const char *const atk_tier_name[ATK_FLAV_DEATH] = {
  "minor", "medium", "major", "critical"
};

static const char *const atk_kind_name[ATK_DOT__COUNT] = {
  "bleed", "poison", "burn", "rot", "chill", "curse", "drain", "shock"
};

// ------------------------------------------------------------------ //
// The neutral fallbacks                                               //
// ------------------------------------------------------------------ //
//
// Plain, short, and setting-free — the engine names no world, so these
// name none either. They are the floor under every line a sheet may
// omit, never a substitute for one it wrote.
//
// FORMAT CONTRACT, and a miscounted slot is a crash rather than a typo:
//
//   atk_blows[]          exactly three `const char *` — attacker, target,
//                        damage
//   atk_heals[]          exactly three — healer, target, hit points
//   atk_deaths[]         exactly two `const char *` — slayer, fallen
//   atk_inflict_*[]      exactly two — attacker, target
//   atk_decay_*[]        exactly three — source, victim, damage
//   atk_decay_kill_*[]   exactly two — source, victim
//
// The damage arrives pre-rendered and pre-colorized, so every slot is
// `%s` and there is no `%d` anywhere.

// The floor under a blow, and the one entry here that should never be
// reached: the loader's coverage gate guarantees every loaded class can
// speak every tier the engine can roll, and D7 guarantees a class is
// always loaded. It exists because "should never" is not "cannot" — a
// sheet can leave the registry between the enrolment that dealt it and
// the turn that reads the stem back — and because a pit that falls silent
// mid-blow is a worse failure than a plain sentence.
static const char *const atk_blows[] = {
  "%s strikes %s for %s damage.",
  "%s catches %s square for %s damage.",
  "%s puts one into %s for %s damage.",
};

// The floor under a heal, and reached about as rarely as the one above:
// the loader takes `[heal]` as all-or-nothing, so a class that can heal
// at all can speak both bands. It stands here for the same reason — a
// stem can go stale between the roll and the sentence.
//
// Every line has to read as well of somebody working on themselves as of
// somebody working on a neighbour: healing yourself is the default, and
// the pit says so in the same words either way.
static const char *const atk_heals[] = {
  "%s closes %s's wounds — %s hit points.",
  "%s gets %s breathing evenly again, worth %s hit points.",
  "%s works on %s until it stops being urgent — %s hit points.",
  "%s spends the turn patching %s up for %s hit points.",
};

static const char *const atk_deaths[] = {
  "%s puts %s down, and %s does not get up.",
  "%s lands the last one; %s folds and stays folded.",
  "%s finishes it. %s is dead before reaching the floor.",
  "%s takes the kill — %s has nothing left to give.",
  "%s ends %s where they stand.",
  "%s stands over %s and waits to be sure.",
  "%s closes the distance one last time; %s stops moving.",
  "%s wins it outright. %s is carried out.",
  "%s strikes %s from the roster of the living.",
  "%s leaves %s cooling on the ground.",
  "%s does not stop at enough. %s is finished.",
  "%s steps back. %s stays down for good.",
};

static const char *const atk_inflict_bleed[] = {
  "%s's cut will not close; %s is bleeding.",
  "%s opens a vein and leaves it open — %s is losing blood.",
};

static const char *const atk_inflict_poison[] = {
  "%s's edge was treated; %s begins to sweat.",
  "%s works poison into the wound. %s will feel it soon.",
};

static const char *const atk_inflict_burn[] = {
  "%s sets something alight on %s, and it keeps burning.",
  "%s leaves a burn on %s that has not stopped spreading.",
};

static const char *const atk_inflict_rot[] = {
  "%s leaves something in %s that begins to eat.",
  "%s's wound is already going bad on %s.",
};

static const char *const atk_inflict_chill[] = {
  "%s leaves a cold in %s that will not warm.",
  "%s's blow drags a deep chill through %s.",
};

static const char *const atk_inflict_curse[] = {
  "%s speaks a word over %s, and it settles in.",
  "%s marks %s with something that will not be shaken off.",
};

static const char *const atk_inflict_drain[] = {
  "%s takes hold of %s and does not let go.",
  "%s opens something in %s that keeps giving.",
};

static const char *const atk_inflict_shock[] = {
  "%s leaves %s twitching and unable to stop.",
  "%s's strike is still running through %s.",
};

static const char *const atk_decay_bleed[] = {
  "The wound %s opened runs down %s's side for %s damage.",
  "%s is long done striking; %s bleeds anyway, for %s damage.",
  "%s's cut keeps drinking from %s — %s damage.",
};

static const char *const atk_decay_poison[] = {
  "The poison %s left climbs %s's arm for %s damage.",
  "%s's poison finds another nerve in %s — %s damage.",
  "%s is not here; %s shudders through %s damage regardless.",
};

static const char *const atk_decay_burn[] = {
  "The burn %s gave %s is still eating — %s damage.",
  "%s's fire finds something else in %s to take, for %s damage.",
  "What %s lit on %s has not gone out: %s damage.",
};

static const char *const atk_decay_rot[] = {
  "What %s left in %s spreads under the skin — %s damage.",
  "%s's work feeds on %s for %s damage.",
  "The rot %s started opens %s further, for %s damage.",
};

static const char *const atk_decay_chill[] = {
  "%s's cold creeps another inch through %s for %s damage.",
  "The chill %s left greys %s's fingers — %s damage.",
  "%s is elsewhere; %s freezes through %s damage anyway.",
};

static const char *const atk_decay_curse[] = {
  "%s's word comes due on %s again — %s damage.",
  "What %s spoke over %s takes another %s damage.",
  "%s's mark tightens on %s for %s damage.",
};

static const char *const atk_decay_drain[] = {
  "%s takes another %s worth of %s without lifting a hand.",
  "The hold %s has on %s costs another %s damage.",
  "%s is still giving to %s: %s damage.",
};

static const char *const atk_decay_shock[] = {
  "%s's strike runs through %s once more for %s damage.",
  "The shock %s left snaps through %s again — %s damage.",
  "%s is nowhere near; %s convulses through %s damage.",
};

static const char *const atk_decay_kill_bleed[] = {
  "%s never struck again; %s simply ran out of blood.",
  "The wound %s opened finishes the argument. %s does not get up.",
};

static const char *const atk_decay_kill_poison[] = {
  "%s's poison stops %s's heart between one breath and the next.",
  "%s is not present for it. %s dies anyway.",
};

static const char *const atk_decay_kill_burn[] = {
  "%s's fire finishes what it started; %s stops moving.",
  "The burn %s gave eats through the last of %s.",
};

static const char *const atk_decay_kill_rot[] = {
  "What %s left in %s finally uses it up.",
  "%s's work is done from the inside; %s is finished.",
};

static const char *const atk_decay_kill_chill[] = {
  "%s's cold reaches %s's heart and stays there.",
  "The chill %s left finishes %s without being there for it.",
};

static const char *const atk_decay_kill_curse[] = {
  "%s's word comes due in full, and %s is spent.",
  "What %s spoke over %s takes the rest of it.",
};

static const char *const atk_decay_kill_drain[] = {
  "%s takes the last of %s and lets the rest fall.",
  "There is nothing left in %s for %s to give.",
};

static const char *const atk_decay_kill_shock[] = {
  "%s's strike finds the last nerve; %s goes still.",
  "The shock %s left stops %s for good.",
};

#define ATK_N(t) ((int)(sizeof(t) / sizeof((t)[0])))

// The bare noun an affliction carries when the sheet that inflicted it
// supplied none. Lower case, no article, no colour — the renderer
// colorizes it.
static const char *const atk_noun_default[ATK_DOT__COUNT] = {
  "open wound", "poison", "burn", "creeping rot", "deep chill",
  "curse", "draining wound", "lingering shock"
};

static const char *const *const atk_inflict_tbl[ATK_DOT__COUNT] = {
  atk_inflict_bleed, atk_inflict_poison, atk_inflict_burn,
  atk_inflict_rot,   atk_inflict_chill,  atk_inflict_curse,
  atk_inflict_drain, atk_inflict_shock
};

static const int atk_inflict_n[ATK_DOT__COUNT] = {
  ATK_N(atk_inflict_bleed), ATK_N(atk_inflict_poison),
  ATK_N(atk_inflict_burn),  ATK_N(atk_inflict_rot),
  ATK_N(atk_inflict_chill), ATK_N(atk_inflict_curse),
  ATK_N(atk_inflict_drain), ATK_N(atk_inflict_shock)
};

static const char *const *const atk_decay_tbl[ATK_DOT__COUNT] = {
  atk_decay_bleed, atk_decay_poison, atk_decay_burn, atk_decay_rot,
  atk_decay_chill, atk_decay_curse,  atk_decay_drain, atk_decay_shock
};

static const int atk_decay_n[ATK_DOT__COUNT] = {
  ATK_N(atk_decay_bleed), ATK_N(atk_decay_poison),
  ATK_N(atk_decay_burn),  ATK_N(atk_decay_rot),
  ATK_N(atk_decay_chill), ATK_N(atk_decay_curse),
  ATK_N(atk_decay_drain), ATK_N(atk_decay_shock)
};

static const char *const *const atk_decay_kill_tbl[ATK_DOT__COUNT] = {
  atk_decay_kill_bleed, atk_decay_kill_poison, atk_decay_kill_burn,
  atk_decay_kill_rot,   atk_decay_kill_chill,  atk_decay_kill_curse,
  atk_decay_kill_drain, atk_decay_kill_shock
};

static const int atk_decay_kill_n[ATK_DOT__COUNT] = {
  ATK_N(atk_decay_kill_bleed), ATK_N(atk_decay_kill_poison),
  ATK_N(atk_decay_kill_burn),  ATK_N(atk_decay_kill_rot),
  ATK_N(atk_decay_kill_chill), ATK_N(atk_decay_kill_curse),
  ATK_N(atk_decay_kill_drain), ATK_N(atk_decay_kill_shock)
};

// A kind arrives from a database column, so it is input like any other:
// an out-of-range value falls back to the first entry rather than
// indexing past a table.
static atk_dot_kind_t
atk_kind_clamp(atk_dot_kind_t kind)
{
  return((kind >= 0 && kind < ATK_DOT__COUNT) ? kind : ATK_DOT_BLEED);
}

const char *
atk_fallback_blow(void)
{
  return(atk_blows[util_rand(ATK_N(atk_blows))]);
}

const char *
atk_fallback_heal(void)
{
  return(atk_heals[util_rand(ATK_N(atk_heals))]);
}

const char *
atk_fallback_death(void)
{
  return(atk_deaths[util_rand(ATK_N(atk_deaths))]);
}

const char *
atk_fallback_dot_inflict(atk_dot_kind_t kind)
{
  const atk_dot_kind_t k = atk_kind_clamp(kind);

  return(atk_inflict_tbl[k][util_rand(atk_inflict_n[k])]);
}

const char *
atk_fallback_decay(atk_dot_kind_t kind)
{
  const atk_dot_kind_t k = atk_kind_clamp(kind);

  return(atk_decay_tbl[k][util_rand(atk_decay_n[k])]);
}

const char *
atk_fallback_decay_kill(atk_dot_kind_t kind)
{
  const atk_dot_kind_t k = atk_kind_clamp(kind);

  return(atk_decay_kill_tbl[k][util_rand(atk_decay_kill_n[k])]);
}

const char *
atk_fallback_noun(atk_dot_kind_t kind)
{
  return(atk_noun_default[atk_kind_clamp(kind)]);
}

// ------------------------------------------------------------------ //
// The built-in brawler                                                //
// ------------------------------------------------------------------ //

// If the sheet directory is missing, unreadable, or every sheet in it is
// rejected, this is registered instead. Twenty moves, five per tier, no
// afflictions, no heals, and not one word of setting: the pit must still
// speak on a daemon where the sheets were never deployed.
typedef struct
{
  atk_flavour_t tier;
  const char   *text;
} atk_builtin_move_t;

static const atk_builtin_move_t atk_brawler[] = {
  { ATK_FLAV_MINOR,
    "{attacker} clips {target} on the ear, more insult than injury — "
    "{damage} damage." },
  { ATK_FLAV_MINOR,
    "{attacker} treads on {target}'s foot and leans on it for "
    "{damage} damage." },
  { ATK_FLAV_MINOR,
    "{attacker} elbows {target} in the shoulder on the way past, "
    "unhurried, for {damage} damage." },
  { ATK_FLAV_MINOR,
    "{attacker} throws a handful of grit into {target}'s eyes for "
    "{damage} damage." },
  { ATK_FLAV_MINOR,
    "{attacker} scuffs {target}'s cheekbone with a knuckle for "
    "{damage} damage." },

  { ATK_FLAV_MEDIUM,
    "{attacker} drives a knee into {target}'s gut and leaves them "
    "wheezing — {damage} damage." },
  { ATK_FLAV_MEDIUM,
    "{attacker} shoulder-checks {target} into the wall for "
    "{damage} damage." },
  { ATK_FLAV_MEDIUM,
    "{attacker} hooks {target}'s ankle and drops them hard for "
    "{damage} damage." },
  { ATK_FLAV_MEDIUM,
    "{attacker} catches {target} in the mouth with a backhand for "
    "{damage} damage." },
  { ATK_FLAV_MEDIUM,
    "{attacker} puts a boot into {target}'s thigh and lets them fold — "
    "{damage} damage." },

  { ATK_FLAV_MAJOR,
    "{attacker} folds {target}'s ribs inward with an elbow for "
    "{damage} damage." },
  { ATK_FLAV_MAJOR,
    "{attacker} catches {target}'s wrist and turns the arm the wrong "
    "way round — {damage} damage." },
  { ATK_FLAV_MAJOR,
    "{attacker} slams {target} face-first into the floor for "
    "{damage} damage." },
  { ATK_FLAV_MAJOR,
    "{attacker} breaks {target}'s nose flat and lets the blood come for "
    "{damage} damage." },
  { ATK_FLAV_MAJOR,
    "{attacker} hammers {target}'s knee sideways until the joint gives "
    "— {damage} damage." },

  { ATK_FLAV_CRITICAL,
    "{attacker} drives {target}'s head into the stone until the stone "
    "gives first. {damage} CRITICAL damage!" },
  { ATK_FLAV_CRITICAL,
    "{attacker} lifts {target} clean off the floor and puts them "
    "through a table. {damage} CRITICAL damage!" },
  { ATK_FLAV_CRITICAL,
    "{attacker} breaks {target}'s ribs one at a time, in no hurry. "
    "{damage} CRITICAL damage!" },
  { ATK_FLAV_CRITICAL,
    "{attacker} crushes {target} underfoot until something deep gives "
    "way. {damage} CRITICAL damage!" },
  { ATK_FLAV_CRITICAL,
    "{attacker} beats {target} to the ground and keeps going. "
    "{damage} CRITICAL damage!" },
};

// ------------------------------------------------------------------ //
// The registry                                                        //
// ------------------------------------------------------------------ //

// Lock ordering is atk_turn_lock -> atk_class_lock, never the reverse,
// and atk_class_lock is NEVER held across file I/O. A load builds a
// whole new registry outside the lock, swaps it in under the lock, and
// frees the old one afterwards.
static atk_class_t     *atk_classes[ATK_CLASSES_MAX];
static uint32_t         atk_class_n;
static pthread_mutex_t  atk_class_lock = PTHREAD_MUTEX_INITIALIZER;

static void atk_class_destroy(atk_class_t *);

// ------------------------------------------------------------------ //
// Text helpers                                                        //
// ------------------------------------------------------------------ //

static bool
atk_tok_is(const char *tok, size_t len, const char *name)
{
  return(strlen(name) == len && strncmp(tok, name, len) == 0);
}

// Trim in place and return the same pointer, so a caller can chain.
static char *
atk_trim(char *s)
{
  size_t n;

  while(*s == ' ' || *s == '\t')
    s++;

  n = strlen(s);

  while(n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
    s[--n] = '\0';

  return(s);
}

// ------------------------------------------------------------------ //
// The sanitiser — the one place sheet text is trusted in              //
// ------------------------------------------------------------------ //

// Vet one line of sheet text and normalise it into `out`. SUCCESS only
// when every rule below holds; the turn path then performs no validation
// of its own, because there is nothing left to validate.
//
// On FAIL, `*why` names the rule that failed, in words the sheet's
// author can act on. It points at a string literal or at `scratch`, so
// it lives exactly as long as the caller's own frame.
static bool
atk_sanitize(atk_section_t sec, const char *in, char *out, size_t cap,
    char *scratch, size_t scratch_sz, const char **why)
{
  const char *end     = NULL;
  const char *p       = NULL;
  size_t      len     = 0;
  uint32_t    seen[ATK_TOK__COUNT] = { 0 };
  uint32_t    tok;

  *why = NULL;

  if(in == NULL || out == NULL || cap == 0 || sec >= ATK_SEC__COUNT)
  {
    *why = "internal: bad sanitiser call";
    return(FAIL);
  }

  // 1. Trim.
  while(*in == ' ' || *in == '\t')
    in++;

  end = in + strlen(in);

  while(end > in && (end[-1] == ' ' || end[-1] == '\t'))
    end--;

  // 2. One matched pair of surrounding quotes, which an author (or the
  //    model that helped them) adds despite being told not to.
  if(end - in >= 2 && (*in == '"' || *in == '\'') && end[-1] == *in)
  {
    in++;
    end--;
  }

  len = (size_t)(end - in);

  // 3. Empty, or too long to survive expansion.
  if(len == 0)
    *why = "empty";

  else if(len >= cap)
    *why = "too long";

  // 4. Control bytes. This kills \r and \n — protocol injection — and
  //    also \x01, the abstract colour marker atk_vis_len() treats as
  //    zero width: a sheet-supplied one would silently skew every column
  //    of `show attack`.
  for(p = in; *why == NULL && p < end; p++)
  {
    if((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
      *why = "control byte";
  }

  // 5. A percent sign. The move is passed as DATA and never reaches a
  //    printf conversion — but this costs one scan and removes the whole
  //    class of hazard from any future refactor that gets it wrong. Do
  //    not "simplify" this away.
  if(*why == NULL && memchr(in, '%', len) != NULL)
    *why = "percent sign — sheet text is never a format string";

  // 6. Every {…} must be one of the five known tokens, opened and
  //    closed.
  for(p = in; *why == NULL && p < end; p++)
  {
    const char *close = NULL;
    size_t      tlen  = 0;
    bool        known = false;

    if(*p == '}')
    {
      *why = "unopened }";
      break;
    }

    if(*p != '{')
      continue;

    close = memchr(p + 1, '}', (size_t)(end - p - 1));

    if(close == NULL)
    {
      *why = "unclosed {";
      break;
    }

    tlen = (size_t)(close - p - 1);

    for(tok = 0; tok < ATK_TOK__COUNT; tok++)
    {
      if(atk_tok_is(p + 1, tlen, atk_tok_name[tok]))
      {
        seen[tok]++;
        known = true;
        break;
      }
    }

    if(!known)
    {
      *why = "unknown {token}";
      break;
    }

    p = close;
  }

  // 7. …and the observed count must equal what this section requires,
  //    for every token. One loop, no special cases: a {damage} in a
  //    death line and an {affliction} in a blow line are both simply
  //    counts that do not match the table.
  for(tok = 0; *why == NULL && tok < ATK_TOK__COUNT; tok++)
  {
    if(seen[tok] != atk_sec_meta[sec].tok[tok])
    {
      snprintf(scratch, scratch_sz, "{%s} x%u, wanted x%u",
          atk_tok_name[tok], seen[tok],
          (unsigned)atk_sec_meta[sec].tok[tok]);
      *why = scratch;
    }
  }

  if(*why != NULL)
    return(FAIL);

  memcpy(out, in, len);
  out[len] = '\0';

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Macro expansion                                                     //
// ------------------------------------------------------------------ //

void
atk_macro_expand(char *out, size_t cap, const char *tmpl,
    const char *attacker, const char *target, const char *damage,
    const char *heal, const char *affliction)
{
  size_t used = 0;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  if(tmpl == NULL)
    return;

  if(attacker   == NULL) attacker   = "";
  if(target     == NULL) target     = "";
  if(damage     == NULL) damage     = "";
  if(heal       == NULL) heal       = "";
  if(affliction == NULL) affliction = "";

  // One forward scan over the TEMPLATE. The substituted values are
  // written straight out and are never re-examined, so a `{` inside a
  // nickname passes through as the literal byte it is. That is
  // structural here, not incidental — keep it that way.
  while(*tmpl != '\0' && used + 1 < cap)
  {
    const char *sub   = NULL;
    const char *close = NULL;
    size_t      tlen  = 0;
    size_t      slen  = 0;

    if(*tmpl == '{' && (close = strchr(tmpl + 1, '}')) != NULL)
    {
      tlen = (size_t)(close - tmpl - 1);

      if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_ATTACKER))
        sub = attacker;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_TARGET))
        sub = target;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_DAMAGE))
        sub = damage;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_HEAL))
        sub = heal;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_AFFLICT))
        sub = affliction;
    }

    if(sub == NULL)
    {
      out[used++] = *tmpl++;
      continue;
    }

    slen = strlen(sub);

    if(slen > cap - used - 1)
      slen = cap - used - 1;

    memcpy(out + used, sub, slen);
    used += slen;
    tmpl  = close + 1;
  }

  // Truncation is silent and acceptable: the sanitiser already bounded
  // the move, and this is the last line of defence, not the first.
  out[used] = '\0';
}

// ------------------------------------------------------------------ //
// Building one class                                                  //
// ------------------------------------------------------------------ //

static atk_class_t *
atk_class_new(const char *type, const char *desc, bool builtin)
{
  atk_class_t *c = mem_alloc(ATK_CTX, "class", sizeof(*c));

  memset(c, 0, sizeof(*c));
  snprintf(c->type, sizeof(c->type), "%s", type);
  snprintf(c->desc, sizeof(c->desc), "%s", desc);
  c->builtin = builtin;

  return(c);
}

static void
atk_class_destroy(atk_class_t *c)
{
  atk_section_t sec;

  if(c == NULL)
    return;

  for(sec = 0; sec < ATK_SEC__COUNT; sec++)
    if(c->move[sec] != NULL)
      mem_free(c->move[sec]);

  mem_free(c);
}

// Append one move, growing the section's array in blocks. FAIL means the
// hard bound was reached and the move was dropped; the caller logs the
// truncation once rather than per line.
static bool
atk_move_add(atk_class_t *c, atk_section_t sec, const atk_move_t *mv)
{
  if(c->n[sec] >= ATK_MOVES_MAX)
    return(FAIL);

  if(c->n[sec] == c->cap[sec])
  {
    const uint16_t want = (uint16_t)(c->cap[sec] + ATK_MOVE_GROW);

    // The allocator is strict — it aborts rather than returning NULL,
    // and realloc is for growing only — so there is no failure branch
    // here to write.
    c->move[sec] = (c->move[sec] == NULL)
        ? mem_alloc(ATK_CTX, "moves", (size_t)want * sizeof(*mv))
        : mem_realloc(c->move[sec], (size_t)want * sizeof(*mv));

    c->cap[sec] = want;
  }

  c->move[sec][c->n[sec]++] = *mv;

  return(SUCCESS);
}

static bool
atk_class_covers(const atk_class_t *c, atk_section_t sec,
    atk_flavour_t tier)
{
  uint16_t i;

  for(i = 0; i < c->n[sec]; i++)
    if(c->move[sec][i].tier == tier)
      return(true);

  return(false);
}

// ------------------------------------------------------------------ //
// Parsing one sheet                                                   //
// ------------------------------------------------------------------ //

static bool
atk_lookup_tier(const char *s, atk_section_t sec, atk_flavour_t *out)
{
  atk_flavour_t tier;

  for(tier = ATK_FLAV_MINOR; tier < ATK_FLAV_DEATH; tier++)
  {
    if(strcmp(s, atk_tier_name[tier]) != 0)
      continue;

    // A heal has two bands, not four: the engine rolls minor or major
    // and then asks the sheet for a sentence of whichever came up. There
    // is no roll a `medium` or `critical` heal could ever describe, so
    // the label is rejected rather than quietly folded into a neighbour.
    if(sec == ATK_SEC_HEAL &&
       tier != ATK_FLAV_MINOR && tier != ATK_FLAV_MAJOR)
      return(FAIL);

    *out = tier;

    return(SUCCESS);
  }

  return(FAIL);
}

static bool
atk_lookup_kind(const char *s, atk_dot_kind_t *out)
{
  atk_dot_kind_t kind;

  for(kind = 0; kind < ATK_DOT__COUNT; kind++)
  {
    if(strcmp(s, atk_kind_name[kind]) == 0)
    {
      *out = kind;
      return(SUCCESS);
    }
  }

  return(FAIL);
}

// A noun is a bare word the renderer colorizes: no article, no capital,
// no colour of its own, and short enough for the column that stores it.
static bool
atk_noun_ok(const char *s, const char **why)
{
  const char *p;

  if(s[0] == '\0')
  {
    *why = "empty affliction noun";
    return(FAIL);
  }

  if(strlen(s) > ATK_NOUN_MAX)
  {
    *why = "affliction noun over 24 bytes";
    return(FAIL);
  }

  for(p = s; *p != '\0'; p++)
  {
    if((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
    {
      *why = "control byte in affliction noun";
      return(FAIL);
    }

    if(isupper((unsigned char)*p))
    {
      *why = "affliction noun must be lower case — the renderer places it "
             "mid-sentence";
      return(FAIL);
    }
  }

  if(strncmp(s, "a ", 2) == 0 || strncmp(s, "an ", 3) == 0 ||
     strncmp(s, "the ", 4) == 0)
  {
    *why = "affliction noun carries an article — the line supplies its own";
    return(FAIL);
  }

  return(SUCCESS);
}

// Read a whole sheet into a fresh buffer. NULL when it cannot be read or
// is larger than the bound; the caller logs.
static char *
atk_sheet_slurp(const char *path, const char **why)
{
  FILE  *fp = fopen(path, "rb");
  char  *buf;
  size_t n;

  if(fp == NULL)
  {
    *why = strerror(errno);
    return(NULL);
  }

  buf = mem_alloc(ATK_CTX, "sheet", ATK_SHEET_SZ);
  n   = fread(buf, 1, ATK_SHEET_SZ - 1, fp);

  // Anything left in the file means the sheet outran the bound. Rejected
  // whole rather than half-read: a partial class could hand the engine a
  // tier it cannot speak.
  if(fgetc(fp) != EOF)
  {
    *why = "sheet larger than 128 KiB";
    mem_free(buf);
    fclose(fp);
    return(NULL);
  }

  buf[n] = '\0';
  fclose(fp);

  return(buf);
}

// Parse one sheet whole. NULL is a rejection and is already logged with
// the file, the line number and the reason — a sheet is never accepted
// in part.
static atk_class_t *
atk_sheet_parse(const char *path, const char *file, const char *stem)
{
  atk_class_t  *c        = NULL;
  char         *raw      = NULL;
  char         *cursor   = NULL;
  const char   *why      = NULL;
  atk_section_t sec      = ATK_SEC__COUNT;   // before the first [section]
  atk_flavour_t tier;
  uint32_t      lineno   = 0;
  uint32_t      dropped  = 0;
  char          type[ATK_CLASS_NAME_SZ] = { 0 };
  char          desc[ATK_CLASS_DESC_SZ] = { 0 };
  char          scratch[192];

  raw = atk_sheet_slurp(path, &why);

  if(raw == NULL)
  {
    clam(CLAM_WARN, ATK_CTX, "%s: cannot read sheet (%s)", file, why);
    return(NULL);
  }

  cursor = raw;

  while(cursor != NULL && *cursor != '\0' && why == NULL)
  {
    char      *eol  = strchr(cursor, '\n');
    char      *line = cursor;
    char      *bar  = NULL;
    char      *text = NULL;
    char      *lbl[2] = { NULL, NULL };
    atk_move_t mv;
    uint8_t    f;

    if(eol != NULL)
      *eol = '\0';

    cursor = (eol != NULL) ? eol + 1 : NULL;
    lineno++;

    // A comment starts at column 0, before the trim, exactly as the
    // grammar says: an indented `#` is text.
    if(line[0] == '#')
      continue;

    line = atk_trim(line);

    if(line[0] == '\0')
      continue;

    // ---- a section header ------------------------------------------ //

    if(line[0] == '[')
    {
      const size_t  len = strlen(line);
      atk_section_t s;

      if(line[len - 1] != ']')
      {
        why = "unterminated [section]";
        break;
      }

      line[len - 1] = '\0';
      sec = ATK_SEC__COUNT;

      for(s = 0; s < ATK_SEC__COUNT; s++)
        if(strcmp(line + 1, atk_sec_meta[s].name) == 0)
          sec = s;

      if(sec == ATK_SEC__COUNT)
      {
        why = "unknown [section]";
        break;
      }

      // The header block is closed by the first section, so this is
      // where it is judged — and where a bad `type:` is reported against
      // the line the reader is looking at rather than a later one.
      if(c == NULL)
      {
        if(type[0] == '\0' || desc[0] == '\0')
          why = "both 'type:' and 'desc:' are required";

        else if(strcmp(type, stem) != 0)
          why = "'type:' does not match the file name";

        else if(!validate_alnum(type, ATK_CLASS_NAME_SZ - 1))
          why = "'type:' is not a bare alphanumeric name";

        if(why != NULL)
          break;

        c = atk_class_new(type, desc, false);
      }

      continue;
    }

    // ---- the header block, before the first section ----------------- //

    if(sec == ATK_SEC__COUNT)
    {
      char *colon = strchr(line, ':');

      if(colon == NULL)
      {
        why = "expected 'key: value' before the first [section]";
        break;
      }

      *colon = '\0';
      text   = atk_trim(colon + 1);
      line   = atk_trim(line);

      if(strcmp(line, "type") == 0)
        snprintf(type, sizeof(type), "%s", text);

      else if(strcmp(line, "desc") == 0)
      {
        if(strlen(text) > ATK_DESC_MAX)
        {
          why = "desc is over 60 bytes";
          break;
        }

        snprintf(desc, sizeof(desc), "%s", text);
      }

      else
      {
        why = "unknown header key";
        break;
      }

      continue;
    }

    // ---- a move ----------------------------------------------------- //
    //
    // Split on the FIRST `|` (and the second, where the section has
    // three fields); every further `|` belongs to the text.

    text = line;

    for(f = 1; f < atk_sec_meta[sec].fields; f++)
    {
      bar = strchr(text, '|');

      if(bar == NULL)
      {
        why = "too few '|' fields for this section";
        break;
      }

      *bar       = '\0';
      lbl[f - 1] = atk_trim(text);
      text       = bar + 1;
    }

    if(why != NULL)
      break;

    memset(&mv, 0, sizeof(mv));
    text = atk_trim(text);

    if(atk_sec_meta[sec].has_tier)
    {
      if(atk_lookup_tier(lbl[0], sec, &tier) != SUCCESS)
      {
        snprintf(scratch, sizeof(scratch),
            "'%s' is not a tier of [%s]", lbl[0], atk_sec_meta[sec].name);
        why = scratch;
        break;
      }

      mv.tier = tier;
    }

    if(atk_sec_meta[sec].has_kind)
    {
      if(atk_lookup_kind(lbl[0], &mv.kind) != SUCCESS)
      {
        snprintf(scratch, sizeof(scratch),
            "'%s' is not one of the eight affliction kinds", lbl[0]);
        why = scratch;
        break;
      }
    }

    if(atk_sec_meta[sec].has_noun)
    {
      if(atk_noun_ok(lbl[1], &why) != SUCCESS)
        break;

      snprintf(mv.noun, sizeof(mv.noun), "%s", lbl[1]);
    }

    if(atk_sanitize(sec, text, mv.text, sizeof(mv.text),
          scratch, sizeof(scratch), &why) != SUCCESS)
      break;

    if(atk_move_add(c, sec, &mv) != SUCCESS)
      dropped++;
  }

  mem_free(raw);

  // ---- the fairness gates ------------------------------------------ //
  //
  // Hard rejections, never warnings. A class that half-loaded could hand
  // the engine a tier it cannot speak, and the whole charter rests on
  // that being impossible.

  if(why == NULL && c == NULL)
    why = "no moves — a sheet must carry at least a [damage] section";

  if(why == NULL && c->n[ATK_SEC_DAMAGE] == 0)
    why = "no [damage] section — every class must be able to strike";

  for(tier = ATK_FLAV_MINOR; why == NULL && tier < ATK_FLAV_DEATH; tier++)
  {
    if(!atk_class_covers(c, ATK_SEC_DAMAGE, tier))
    {
      snprintf(scratch, sizeof(scratch),
          "[damage] has no '%s' move — every class must be able to speak "
          "every tier the engine can roll", atk_tier_name[tier]);
      why = scratch;
    }
  }

  if(why == NULL && c->n[ATK_SEC_HEAL] > 0)
  {
    if(!atk_class_covers(c, ATK_SEC_HEAL, ATK_FLAV_MINOR) ||
       !atk_class_covers(c, ATK_SEC_HEAL, ATK_FLAV_MAJOR))
      why = "[heal] needs one 'minor' and one 'major' move — the engine "
            "rolls which band a heal lands in";
  }

  if(why != NULL)
  {
    if(lineno > 0)
      clam(CLAM_WARN, ATK_CTX, "%s:%u: %s", file, lineno, why);

    else
      clam(CLAM_WARN, ATK_CTX, "%s: %s", file, why);

    atk_class_destroy(c);
    return(NULL);
  }

  if(dropped > 0)
    clam(CLAM_WARN, ATK_CTX,
        "%s: %u move(s) past the %d-per-section bound were dropped",
        file, dropped, ATK_MOVES_MAX);

  return(c);
}

// ------------------------------------------------------------------ //
// The built-in fallback                                               //
// ------------------------------------------------------------------ //

static atk_class_t *
atk_brawler_build(void)
{
  atk_class_t *c = atk_class_new("brawler",
      "Fists, boots and whatever is on the floor.", true);
  const char  *why = NULL;
  size_t       i;
  char         scratch[192];

  for(i = 0; i < sizeof(atk_brawler) / sizeof(atk_brawler[0]); i++)
  {
    atk_move_t mv;

    memset(&mv, 0, sizeof(mv));
    mv.tier = atk_brawler[i].tier;

    // The compiled-in lines go through the same sanitiser every sheet
    // does. It costs nothing at load and it means the format contract is
    // checked rather than asserted.
    if(atk_sanitize(ATK_SEC_DAMAGE, atk_brawler[i].text, mv.text,
          sizeof(mv.text), scratch, sizeof(scratch), &why) != SUCCESS)
    {
      clam(CLAM_WARN, ATK_CTX, "built-in brawler move %zu rejected (%s)",
          i, why);
      continue;
    }

    if(atk_move_add(c, ATK_SEC_DAMAGE, &mv) != SUCCESS)
      break;
  }

  return(c);
}

// ------------------------------------------------------------------ //
// Loading the directory                                               //
// ------------------------------------------------------------------ //

static void
atk_report_bad(atk_load_report_t *rep, const char *file)
{
  rep->rejected++;

  if(rep->n_bad < ATK_CLASSES_MAX)
    snprintf(rep->bad[rep->n_bad++], ATK_SHEET_FILE_SZ, "%s", file);
}

// Build a whole new registry from `dir`. Runs entirely outside
// atk_class_lock: nothing here may touch the live registry.
static uint32_t
atk_scan_dir(const char *dir, atk_class_t **out, uint32_t cap,
    atk_load_report_t *rep)
{
  DIR           *d;
  struct dirent *ent;
  uint32_t       n = 0;

  d = opendir(dir);

  if(d == NULL)
  {
    clam(CLAM_WARN, ATK_CTX, "classes_path: cannot open '%s': %s",
        dir, strerror(errno));
    return(0);
  }

  while((ent = readdir(d)) != NULL)
  {
    const char  *file = ent->d_name;
    size_t       len  = strlen(file);
    atk_class_t *c;
    char         stem[ATK_CLASS_NAME_SZ];
    char         path[ATK_PATH_SZ + ATK_CLASS_NAME_SZ + 8];

    if(len < ATK_STEM_MIN || strcmp(file + len - 4, ".txt") != 0)
      continue;

    // The registry is full. Say so, and say it once: a sheet silently
    // never looked at is a class nobody can be dealt, and this is the
    // one path in the loader that would otherwise refuse without a
    // reason.
    if(n >= cap)
    {
      clam(CLAM_WARN, ATK_CTX,
          "'%s' and any sheet after it were not read — the registry holds "
          "at most %u classes", file, cap);
      break;
    }

    if(len - 4 > ATK_CLASS_NAME_SZ - 1)
    {
      clam(CLAM_WARN, ATK_CTX, "%s: file name is too long to be a class",
          file);
      atk_report_bad(rep, file);
      continue;
    }

    // Defence in depth against a KV-controlled directory that somehow
    // yields a separator: a stem is a bare name and is never a path.
    if(strchr(file, '/') != NULL)
      continue;

    memcpy(stem, file, len - 4);
    stem[len - 4] = '\0';

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    c = atk_sheet_parse(path, file, stem);

    if(c == NULL)
    {
      atk_report_bad(rep, file);
      continue;
    }

    clam(CLAM_INFO, ATK_CTX,
        "class '%s' loaded (%u damage, %u dot, %u heal, %u death, "
        "%u decay, %u decay-kill)",
        c->type, c->n[ATK_SEC_DAMAGE], c->n[ATK_SEC_DOT],
        c->n[ATK_SEC_HEAL], c->n[ATK_SEC_DEATH], c->n[ATK_SEC_DECAY],
        c->n[ATK_SEC_DECAY_KILL]);

    // The brief says "generally 20-100 moves"; a sheet outside that is
    // loaded anyway and merely noted. Variety is not fairness — the
    // number of moves a class owns changes how often it repeats itself
    // and nothing else.
    {
      const uint32_t spoken = (uint32_t)c->n[ATK_SEC_DAMAGE] +
                              c->n[ATK_SEC_DOT] + c->n[ATK_SEC_HEAL];

      if(spoken < 20 || spoken > 100)
        clam(CLAM_WARN, ATK_CTX,
            "class '%s' has %u moves, outside the intended 20-100",
            c->type, spoken);
    }

    out[n++] = c;
    rep->accepted++;
  }

  closedir(d);

  return(n);
}

uint32_t
atk_class_load(atk_load_report_t *rep)
{
  atk_tunables_t    t;
  atk_load_report_t local;
  atk_class_t      *fresh[ATK_CLASSES_MAX];
  atk_class_t      *old  [ATK_CLASSES_MAX];
  uint32_t          n_fresh;
  uint32_t          n_old;
  uint32_t          i;

  if(rep == NULL)
    rep = &local;

  memset(rep, 0, sizeof(*rep));
  memset(fresh, 0, sizeof(fresh));

  atk_tunables_load(&t);
  n_fresh = atk_scan_dir(t.classes_path, fresh, ATK_CLASSES_MAX, rep);

  // D7 — there is always a class. A daemon where the sheets were never
  // deployed still has a pit that speaks.
  if(n_fresh == 0)
  {
    clam(CLAM_WARN, ATK_CTX,
        "no character sheets in '%s' — falling back to the built-in "
        "brawler", t.classes_path);

    fresh[0] = atk_brawler_build();
    n_fresh  = 1;
    rep->accepted = 1;
  }

  // Only the swap is under the lock, and the old registry is freed after
  // it is released: no file I/O and no allocation ever happens with
  // atk_class_lock held.
  pthread_mutex_lock(&atk_class_lock);

  memcpy(old, atk_classes, sizeof(old));
  n_old = atk_class_n;

  memcpy(atk_classes, fresh, sizeof(atk_classes));
  atk_class_n = n_fresh;

  pthread_mutex_unlock(&atk_class_lock);

  for(i = 0; i < n_old; i++)
    atk_class_destroy(old[i]);

  return(n_fresh);
}

void
atk_class_free(void)
{
  atk_class_t *old[ATK_CLASSES_MAX];
  uint32_t     n;
  uint32_t     i;

  pthread_mutex_lock(&atk_class_lock);

  memcpy(old, atk_classes, sizeof(old));
  n = atk_class_n;

  memset(atk_classes, 0, sizeof(atk_classes));
  atk_class_n = 0;

  pthread_mutex_unlock(&atk_class_lock);

  for(i = 0; i < n; i++)
    atk_class_destroy(old[i]);
}

// ------------------------------------------------------------------ //
// Serving the registry                                                //
// ------------------------------------------------------------------ //

// The caller must hold atk_class_lock.
static const atk_class_t *
atk_class_find_locked(const char *type)
{
  uint32_t i;

  if(type == NULL || type[0] == '\0')
    return(NULL);

  for(i = 0; i < atk_class_n; i++)
    if(strcmp(atk_classes[i]->type, type) == 0)
      return(atk_classes[i]);

  return(NULL);
}

bool
atk_class_pick(char *out, size_t cap)
{
  bool ok = FAIL;

  if(out == NULL || cap == 0)
    return(FAIL);

  out[0] = '\0';

  pthread_mutex_lock(&atk_class_lock);

  if(atk_class_n > 0)
  {
    snprintf(out, cap, "%s",
        atk_classes[util_rand((int)atk_class_n)]->type);
    ok = SUCCESS;
  }

  pthread_mutex_unlock(&atk_class_lock);

  return(ok);
}

// Deliberately not a lookup failure: the caller has a turn to resolve and
// a stem that may have gone stale under it, and re-picking is the honest
// answer — under the charter, one class is worth exactly as much as
// another, so being handed a different sheet costs nobody anything.
void
atk_class_for(const char *want, char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  if(want != NULL && want[0] != '\0')
  {
    pthread_mutex_lock(&atk_class_lock);

    if(atk_class_find_locked(want) != NULL)
      snprintf(out, cap, "%s", want);

    pthread_mutex_unlock(&atk_class_lock);
  }

  if(out[0] == '\0')
    atk_class_pick(out, cap);
}

bool
atk_class_move(const char *type, atk_section_t sec, atk_flavour_t tier,
    atk_dot_kind_t kind, atk_move_t *out)
{
  const atk_class_t *c;
  uint16_t           match[ATK_MOVES_MAX];
  uint16_t           n = 0;
  uint16_t           i;
  bool               ok = FAIL;

  if(out == NULL || sec >= ATK_SEC__COUNT)
    return(FAIL);

  pthread_mutex_lock(&atk_class_lock);

  c = atk_class_find_locked(type);

  if(c != NULL)
  {
    for(i = 0; i < c->n[sec]; i++)
    {
      if(atk_sec_meta[sec].has_tier && c->move[sec][i].tier != tier)
        continue;

      // A DOT is selected by neither: the engine already rolled the
      // number, and the inflict line names none.
      if(atk_sec_meta[sec].has_kind && sec != ATK_SEC_DOT &&
         c->move[sec][i].kind != kind)
        continue;

      match[n++] = i;
    }

    if(n > 0)
    {
      *out = c->move[sec][match[util_rand((int)n)]];
      ok   = SUCCESS;
    }
  }

  pthread_mutex_unlock(&atk_class_lock);

  return(ok);
}

bool
atk_class_has(const char *type, atk_section_t sec)
{
  const atk_class_t *c;
  bool               has = false;

  if(sec >= ATK_SEC__COUNT)
    return(false);

  pthread_mutex_lock(&atk_class_lock);

  c = atk_class_find_locked(type);

  if(c != NULL)
    has = (c->n[sec] > 0);

  pthread_mutex_unlock(&atk_class_lock);

  return(has);
}

uint32_t
atk_class_list(atk_class_info_t *out, uint32_t cap)
{
  uint32_t n = 0;
  uint32_t i;

  if(out == NULL || cap == 0)
    return(0);

  pthread_mutex_lock(&atk_class_lock);

  for(i = 0; i < atk_class_n && n < cap; i++)
  {
    const atk_class_t *c = atk_classes[i];
    uint32_t           k = n;

    // Insertion sort on the way out: the registry is at most 32 entries
    // and the view wants them alphabetical.
    while(k > 0 && strcmp(out[k - 1].type, c->type) > 0)
    {
      out[k] = out[k - 1];
      k--;
    }

    memset(&out[k], 0, sizeof(out[k]));
    snprintf(out[k].type, sizeof(out[k].type), "%s", c->type);
    snprintf(out[k].desc, sizeof(out[k].desc), "%s", c->desc);
    memcpy(out[k].n, c->n, sizeof(out[k].n));
    out[k].builtin = c->builtin;
    n++;
  }

  pthread_mutex_unlock(&atk_class_lock);

  return(n);
}
