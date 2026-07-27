// botmanager — MIT
// melee combat: the damage roll and the Drow flavor that dresses it.
// Nothing here touches the database or the command context — it turns
// (tunables, two nicknames, a number) into one finished line of text.

#define MELEE_INTERNAL
#include "melee.h"

#include "colors.h"
#include "util.h"

#include <stdio.h>

// ------------------------------------------------------------------ //
// Flavor                                                              //
// ------------------------------------------------------------------ //

// The four damage tiers, one table each, keyed by melee_flavour_t. The
// words come from the DAMAGE, never from the crit roll: a critical of 10
// and an ordinary blow of 10 hurt the same, so they must read the same.
//
// FORMAT CONTRACT: every entry in all four tables takes exactly three
// `const char *` — attacker, target, damage — in that order. The damage
// arrives already rendered and colorized, so all three slots are %s and
// there is no %d anywhere. Breaking this crashes the plugin, not the
// line.

// A graze, a scuff, a stinging insult of a strike.
static const char *const melee_minor[] = {
  "%s rakes %s with House-signet talons for %s damage.",
  "%s scores %s's cheek with a poisoned nail for %s damage.",
  "%s stamps on %s's instep with a spider-silk boot for %s damage.",
  "%s hurls a fistful of Underdark grit into %s's eyes for %s damage.",
  "%s clips %s with the pommel of a Melee-Magthere training blade for %s damage.",
  "%s trips %s face-first into a puddle of rothe filth for %s damage.",
  "%s jabs %s with the butt of a driftglobe pole for %s damage.",
  "%s slices a shallow line down %s's arm, just to watch it bleed — %s damage.",
  "%s scuffs %s's cheekbone with a knuckle of House rings for %s damage.",
  "%s clips %s's ear with the flat of a dagger, more insult than injury — %s damage.",
  "%s treads on %s's cloak and tears the hem of the piwafwi for %s damage.",
  "%s elbows %s in the shoulder on the way past, unhurried, for %s damage.",
  "%s snaps a glove across %s's mouth the way a Matron scolds a servant — %s damage.",
  "%s nicks %s's knuckles and watches them fumble the grip for %s damage.",
  "%s raps %s across the shin with a still-sheathed blade for %s damage.",
  "%s spits at %s's feet and opens their chin with a lazy backhand for %s damage.",
};

// A clean landed hit that hurts, and that a duellist keeps going through.
static const char *const melee_medium[] = {
  "%s lashes %s across the brow with a piwafwi-wrapped fist for %s damage.",
  "%s cracks %s with the flat of an adamantine blade for %s damage.",
  "%s drives a knee into %s's gut and leaves them wheezing — %s damage.",
  "%s parries, ripostes, and opens %s's forearm for %s damage.",
  "%s snaps a whip-cord across %s's shoulders for %s damage.",
  "%s catches %s with a backhand heavy with House rings for %s damage.",
  "%s shoulder-checks %s into a stalagmite for %s damage.",
  "%s knocks the wind from %s with a spider-carved buckler for %s damage.",
  "%s hooks %s's ankle and drops them hard onto the cavern floor for %s damage.",
  "%s drives a fist into %s's kidney and lets them fold for %s damage.",
  "%s opens a red line along %s's collarbone with black steel for %s damage.",
  "%s beats %s's guard aside and puts a boot into their thigh for %s damage.",
  "%s slashes %s across the shoulder blade as they turn away for %s damage.",
  "%s catches %s in the mouth with a spiked bracer for %s damage.",
  "%s slams %s's back against the cavern wall and holds them there for %s damage.",
  "%s draws at arm's length and puts a hand-crossbow bolt in %s's thigh for %s damage.",
};

// Bone, blood and stagger. Badly hurt, visibly losing — never fatal.
static const char *const melee_major[] = {
  "%s slips a hand-crossbow bolt between %s's ribs for %s damage.",
  "%s slams %s against the Narbondel-lit stone for %s damage.",
  "%s flicks faerie fire over %s and stabs the outline for %s damage.",
  "%s lands a bone-splintering elbow that folds %s's ribs inward with a wet crunch — %s damage.",
  "%s whispers to the Spider Queen and buries a dagger in %s to the guard for %s damage.",
  "%s drags %s face-first down a rasp of raw obsidian for %s damage.",
  "%s calls a globe of darkness and works %s over inside it, unseen and unhurried — %s damage.",
  "%s brands %s with a red-hot House insignia and holds it there for %s damage.",
  "%s catches %s's wrist, reverses the arm, and snaps it the wrong way round — %s damage.",
  "%s bathes a blade in drow poison and paints %s with it for %s damage.",
  "%s smashes %s through a sava board and scatters pieces and teeth alike for %s damage.",
  "%s carves the sigil of Bregan D'aerthe into %s's back, stroke by unhurried stroke, for %s damage.",
  "%s breaks %s's nose flat with a pommel and lets the blood come for %s damage.",
  "%s runs a blade through %s's shoulder and leaves it there a moment — %s damage.",
  "%s hammers %s's knee sideways until the joint gives with a crack for %s damage.",
  "%s tears open %s's flank with a hooked dagger and steps back to watch — %s damage.",
};

// The heaviest thing that is still survivable — and it may say so.
static const char *const melee_critical[] = {
  "%s drives a black-steel blade to the hilt through %s — Lolth leans closer. %s CRITICAL damage!",
  "%s opens %s from collarbone to hip; the wound steams in the cold. %s CRITICAL damage!",
  "%s empties a quiver of drow-poisoned bolts into %s at point-blank. %s CRITICAL damage!",
  "%s severs the tendons behind %s's knee; the scream carries to Menzoberranzan. %s CRITICAL damage!",
  "%s drives %s's skull into the stone until the stone gives first. %s CRITICAL damage!",
  "%s runs %s through and lifts them clean off the floor on the blade. %s CRITICAL damage!",
  "%s answers with a whip of fangs — five heads find %s at once. %s CRITICAL damage!",
  "%s throws %s down a chasm and hauls them back up by the hair to finish. %s CRITICAL damage!",
  "%s lets a yochlol's whisper guide the killing arc across %s's throat. %s CRITICAL damage!",
  "%s drives a hooked blade under %s's jaw and twists. %s CRITICAL damage!",
  "%s crushes %s beneath an adamantine boot until something deep gives way. %s CRITICAL damage!",
  "%s nails %s's hand to the stone with a dagger and works on them at leisure. %s CRITICAL damage!",
  "%s opens %s's belly with an adamantine edge; Lolth's spiders gather early. %s CRITICAL damage!",
  "%s drives a spear of black steel through %s's thigh and out the other side. %s CRITICAL damage!",
  "%s slams %s face-first through a driftglobe — the light dies with the scream. %s CRITICAL damage!",
  "%s breaks %s's ribs against a stalagmite one at a time, in no hurry. %s CRITICAL damage!",
};

// FORMAT CONTRACT: every entry takes exactly two `const char *` —
// slayer, then fallen. No damage slot; the round is already over.
static const char *const melee_deaths[] = {
  "%s takes %s's head in one clean stroke — the Spider Queen is pleased.",
  "%s feeds %s to Lolth's webs; the corpse is drawn down into the dark.",
  "%s stands over %s's body and wipes the blade clean on a piwafwi.",
  "%s hurls %s screaming into the Demonweb Pits.",
  "%s claims the kill; the last of %s's blood cools on Underdark stone.",
  "%s snuffs %s out — a House sigil cracks and goes dark, unmourned.",
  "%s offers %s's heart at the altar of the Spider Queen. Accepted.",
  "%s kills %s the way the drow prefer: quickly, and before witnesses.",
  "%s severs the thread. %s does not rise again.",
  "%s stands alone as Narbondel dims — %s has spent their last breath.",
  "%s feeds %s to the driders, and the chamber falls silent.",
  "%s strikes %s from the roster of the living without breaking stride.",
  "%s spins %s's soul into the web; the Matron Mothers take note.",
  "%s ends it — %s falls to one knee, then to none.",
  "%s graduates; %s is dead. The Academy would call that a passing grade.",
  "%s leaves %s where they lie. The rothe will handle the rest.",
  "%s writes over %s's name in the House ledger, in blood.",
  "%s closes the account; %s dies staring into a dark that stares back.",
  "%s pins %s to the cavern wall and leaves the blade in as a marker.",
  "%s takes the spoils while eight legs skitter over %s's cooling body.",
};

#define MELEE_N(t) ((int)(sizeof(t) / sizeof((t)[0])))

// The four damage tiers as one indexable set, so the renderer picks a
// table by severity rather than by a chain of conditionals — a fifth
// tier would be one line here and nothing anywhere else. Indexed by
// melee_flavour_t; MELEE_FLAV_DEATH is not a damage tier and has no
// entry.
static const char *const *const melee_tbl[MELEE_FLAV_DEATH] = {
  melee_minor, melee_medium, melee_major, melee_critical
};

static const int melee_tbl_n[MELEE_FLAV_DEATH] = {
  MELEE_N(melee_minor), MELEE_N(melee_medium),
  MELEE_N(melee_major), MELEE_N(melee_critical)
};

// The dressing melee adds around the sentence, also by tier: heavier
// blows read louder. Every prefix is a single-column code point, so
// melee_vis_len() stays honest.
static const char *const melee_tier_color[MELEE_FLAV_DEATH] = {
  CLR_WHITE, CLR_YELLOW, CLR_BOLD CLR_ORANGE, CLR_BOLD CLR_RED
};

static const char *const melee_tier_emoji[MELEE_FLAV_DEATH] = {
  "⚔ ", "⚔ ", "🗡 ", "💥 "
};

// ------------------------------------------------------------------ //
// Afflictions                                                         //
// ------------------------------------------------------------------ //

// Three parallel tables keyed by melee_dot_kind_t, in the shape the tier
// tables above already establish. The name is the bare noun a line
// substitutes for {affliction}: lower case, no article, no colour — the
// renderer colorizes it.
static const char *const melee_dot_name[MELEE_DOT__COUNT] = {
  "open wound", "spider venom", "ochre burn", "myconid spores",
  "necrotic chill"
};

// Every glyph here must occupy exactly ONE display column, or
// melee_vis_len() starts lying and the round card skews. All five are
// plain U+27xx/U+26xx symbols: no variation selectors, no
// emoji-presentation code points.
static const char *const melee_dot_emoji[MELEE_DOT__COUNT] = {
  "✚", "☣", "⚗", "✺", "❆"
};

static const char *const melee_dot_color[MELEE_DOT__COUNT] = {
  CLR_RED, CLR_GREEN, CLR_ORANGE, CLR_YELLOW, CLR_CYAN
};

// FORMAT CONTRACT: every entry in all five tables below takes exactly
// two `const char *` — attacker, then target. The affliction names
// itself in the words, so there is no {affliction} slot and no damage:
// the tick lines carry the numbers, this one only says it has begun.
// Short, deliberately — a blow line is already on screen above it.
static const char *const melee_dot_bleed[] = {
  "%s's cut will not close; %s is bleeding.",
  "%s opens a vein and leaves it open — %s is losing blood.",
  "%s carves a wound %s cannot press shut.",
};

static const char *const melee_dot_venom[] = {
  "Lolth's patience runs in %s's blade — %s is envenomed.",
  "%s's edge was drow-poisoned; %s begins to sweat.",
  "%s lets spider venom find the wound. %s will feel it working.",
};

static const char *const melee_dot_acid[] = {
  "%s smears ochre slime across %s; it begins to eat.",
  "%s breaks a jelly-flask over %s. The burning starts slow.",
  "%s paints %s with something that keeps chewing.",
};

static const char *const melee_dot_spores[] = {
  "%s bursts a myconid cap over %s. The rot takes.",
  "%s drives fungal spores into %s's wound; the bloom begins.",
  "%s dusts %s with cavern rot and steps back to let it work.",
};

static const char *const melee_dot_chill[] = {
  "%s leaves a Narbondel cold in %s that will not warm.",
  "%s's blade drags a necrotic chill through %s.",
  "%s marks %s with the cold that follows a death-blow.",
};

// FORMAT CONTRACT: every entry in the five tick tables takes exactly
// three `const char *` — source, victim, damage — like a blow line, and
// for the same reason: a tick is a peer of a blow and reads as one.
static const char *const melee_dot_tick_bleed[] = {
  "%s's cut keeps drinking from %s — %s damage.",
  "The wound %s opened runs down %s's side for %s damage.",
  "%s's blade is long gone; %s bleeds anyway, for %s damage.",
};

static const char *const melee_dot_tick_venom[] = {
  "Lolth's patience works through %s's poison; %s shudders for %s damage.",
  "The venom %s left climbs %s's arm for %s damage.",
  "%s's poison finds another nerve in %s — %s damage.",
};

static const char *const melee_dot_tick_acid[] = {
  "%s's slime eats deeper into %s for %s damage.",
  "The ochre burn %s gave %s is still chewing — %s damage.",
  "%s's acid finds bone in %s for %s damage.",
};

static const char *const melee_dot_tick_spores[] = {
  "%s's spores bloom in %s's wound for %s damage.",
  "The rot %s planted spreads under %s's skin — %s damage.",
  "%s's fungus feeds on %s for %s damage.",
};

static const char *const melee_dot_tick_chill[] = {
  "%s's cold creeps another inch through %s for %s damage.",
  "The chill %s left greys %s's fingers — %s damage.",
  "Narbondel's dark works in %s's wake; %s takes %s damage.",
};

// FORMAT CONTRACT: exactly two `const char *` — source, then victim. No
// damage slot, matching melee_deaths[]: the round is already over.
static const char *const melee_dot_death_bleed[] = {
  "%s never struck again; %s simply ran out of blood.",
  "The wound %s opened finishes the argument. %s does not get up.",
};

static const char *const melee_dot_death_venom[] = {
  "%s's venom stops %s's heart between one breath and the next.",
  "Lolth takes her time. %s waits; %s stops.",
};

static const char *const melee_dot_death_acid[] = {
  "%s's slime finishes what it started — %s comes apart on the stone.",
  "The burn %s gave eats through the last of %s.",
};

static const char *const melee_dot_death_spores[] = {
  "%s's rot blooms one last time; %s is a garden now.",
  "The spores %s planted come up through %s's ribs.",
};

static const char *const melee_dot_death_chill[] = {
  "%s's cold reaches %s's heart and stays there.",
  "The chill %s left finishes %s without being present for it.",
};

// A kind arrives from a database column, so it is input like any other:
// an out-of-range value falls back to the first entry rather than
// indexing past the table.
static melee_dot_kind_t
melee_dot_clamp(melee_dot_kind_t kind)
{
  return((kind >= 0 && kind < MELEE_DOT__COUNT) ? kind : MELEE_DOT_BLEED);
}

const char *
melee_dot_name_of(melee_dot_kind_t kind)
{
  return(melee_dot_name[melee_dot_clamp(kind)]);
}

const char *
melee_dot_emoji_of(melee_dot_kind_t kind)
{
  return(melee_dot_emoji[melee_dot_clamp(kind)]);
}

const char *
melee_dot_color_of(melee_dot_kind_t kind)
{
  return(melee_dot_color[melee_dot_clamp(kind)]);
}

// The five inflict tables as one indexable set, exactly as melee_tbl[]
// gathers the damage tiers.
static const char *const *const melee_dot_tbl[MELEE_DOT__COUNT] = {
  melee_dot_bleed, melee_dot_venom, melee_dot_acid, melee_dot_spores,
  melee_dot_chill
};

static const int melee_dot_tbl_n[MELEE_DOT__COUNT] = {
  MELEE_N(melee_dot_bleed), MELEE_N(melee_dot_venom),
  MELEE_N(melee_dot_acid),  MELEE_N(melee_dot_spores),
  MELEE_N(melee_dot_chill)
};

static const char *const *const melee_dot_tick_tbl[MELEE_DOT__COUNT] = {
  melee_dot_tick_bleed, melee_dot_tick_venom, melee_dot_tick_acid,
  melee_dot_tick_spores, melee_dot_tick_chill
};

static const int melee_dot_tick_n[MELEE_DOT__COUNT] = {
  MELEE_N(melee_dot_tick_bleed), MELEE_N(melee_dot_tick_venom),
  MELEE_N(melee_dot_tick_acid),  MELEE_N(melee_dot_tick_spores),
  MELEE_N(melee_dot_tick_chill)
};

static const char *const *const melee_dot_death_tbl[MELEE_DOT__COUNT] = {
  melee_dot_death_bleed, melee_dot_death_venom, melee_dot_death_acid,
  melee_dot_death_spores, melee_dot_death_chill
};

static const int melee_dot_death_n[MELEE_DOT__COUNT] = {
  MELEE_N(melee_dot_death_bleed), MELEE_N(melee_dot_death_venom),
  MELEE_N(melee_dot_death_acid),  MELEE_N(melee_dot_death_spores),
  MELEE_N(melee_dot_death_chill)
};

void
melee_render_dot_inflict(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, melee_dot_kind_t kind)
{
  char atk [MELEE_NICK_SZ + 8];
  char tgt [MELEE_NICK_SZ + 8];
  char body[MELEE_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  kind = melee_dot_clamp(kind);

  snprintf(atk, sizeof(atk), CLR_CYAN   "%s" CLR_RESET, atk_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  snprintf(body, sizeof(body),
      melee_dot_tbl[kind][util_rand(melee_dot_tbl_n[kind])], atk, tgt);

  // The glyph that will mark the victim on the round card leads the
  // line, so the two read as the same thing.
  snprintf(out, cap, "%s %s", melee_dot_emoji_of(kind), body);
}

// ------------------------------------------------------------------ //
// The roll                                                            //
// ------------------------------------------------------------------ //

// melee_tunables_load() has already guaranteed hit_max >= 1 and
// crit_max >= crit_min, so neither util_rand argument can reach zero.
int32_t
melee_roll(const melee_tunables_t *t, bool *crit_out)
{
  bool crit;

  if(t == NULL)
    return(0);

  crit = (t->crit_pct > 0 && util_rand(100) < (int)t->crit_pct);

  if(crit_out != NULL)
    *crit_out = crit;

  if(crit)
    return((int32_t)t->crit_min +
        (int32_t)util_rand((int)(t->crit_max - t->crit_min) + 1));

  return(1 + (int32_t)util_rand((int)t->hit_max));
}

// ------------------------------------------------------------------ //
// Severity                                                            //
// ------------------------------------------------------------------ //

// The heaviest blow the current tunables can produce. Crits only widen
// the ceiling when they can actually fire AND actually exceed an
// ordinary blow — with crit_chance_pct 0 the crit band is dead weight
// and must not stretch the scale, or every real hit would read minor.
int32_t
melee_dmg_ceiling(const melee_tunables_t *t)
{
  int32_t ceiling;

  if(t == NULL)
    return(1);

  ceiling = (int32_t)t->hit_max;

  if(t->crit_pct > 0 && (int32_t)t->crit_max > ceiling)
    ceiling = (int32_t)t->crit_max;

  // melee_tunables_load() already guarantees this; belt and braces,
  // because melee_severity() divides by it.
  return(ceiling > 0 ? ceiling : 1);
}

melee_flavour_t
melee_severity(const melee_tunables_t *t, int32_t dmg)
{
  int32_t pct;

  if(t == NULL)
    return(MELEE_FLAV_MINOR);

  if(dmg < 0)
    dmg = 0;

  pct = (int32_t)((int64_t)dmg * 100 / melee_dmg_ceiling(t));

  if(pct > 100)          // a tunable moved mid-round; clamp rather than
    pct = 100;           // fall off the end of the band

  if(pct >= (int32_t)t->sev_crit_at)
    return(MELEE_FLAV_CRITICAL);

  if(pct >= (int32_t)t->sev_major_at)
    return(MELEE_FLAV_MAJOR);

  if(pct >= (int32_t)t->sev_medium_at)
    return(MELEE_FLAV_MEDIUM);

  return(MELEE_FLAV_MINOR);
}

// ------------------------------------------------------------------ //
// Rendering                                                           //
// ------------------------------------------------------------------ //

// The pool supplies the sentence and nothing else: the emoji prefix, the
// colorization, and the health tail below are melee's own, whether the
// words came from a model or from the tables above.
void
melee_render_blow(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp, int32_t hp_max,
    const melee_tunables_t *t, bool *need_refill)
{
  melee_flavour_t sev;
  char            atk [MELEE_NICK_SZ + 8];
  char            tgt [MELEE_NICK_SZ + 8];
  char            dmgs[32];
  char            body[MELEE_LINE_SZ];
  char            tmpl[MELEE_LLM_TMPL_SZ];

  if(out == NULL || cap == 0)
    return;

  // Everything below — the pool, the table, the colour, the emoji — is
  // driven by this one value, which is why the words can never again
  // disagree with the number.
  sev = melee_severity(t, dmg);

  snprintf(atk, sizeof(atk), CLR_CYAN   "%s" CLR_RESET, atk_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  snprintf(dmgs, sizeof(dmgs), "%s%d" CLR_RESET, melee_tier_color[sev], dmg);

  if(melee_pool_take(sev, tmpl, sizeof(tmpl),
        t != NULL ? t->llm_refill_at : 0, need_refill) == SUCCESS)
    melee_tmpl_expand(body, sizeof(body), tmpl, atk, tgt, dmgs);

  else
  {
    melee_pool_fallback();
    snprintf(body, sizeof(body),
        melee_tbl[sev][util_rand(melee_tbl_n[sev])], atk, tgt, dmgs);
  }

  // The survivor's remaining health rides on every non-fatal line; the
  // nick inside the gray block stays plain so the block reads as one.
  // A fatal blow carries no tally — the death line that follows says it.
  if(hp > 0)
    snprintf(out, cap, "%s%s " CLR_GRAY "[%s — %d/%d hp]" CLR_RESET,
        melee_tier_emoji[sev], body, tgt_nick, hp, hp_max);

  else
    snprintf(out, cap, "%s%s", melee_tier_emoji[sev], body);
}

// The oldest joke on IRC, kept for the one blow that earns it: a
// critical that kills. It stands where the tier line would have stood —
// it is the line that carries the number — and the ordinary death line
// still follows it, so the death-line-before-eject law is untouched. No
// pool is consulted and none is owed a refill; no health tail, because a
// fatal blow carries no tally.
void
melee_render_trout(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg)
{
  if(out == NULL || cap == 0)
    return;

  snprintf(out, cap,
      "%s" CLR_CYAN "%s" CLR_RESET " slaps " CLR_PURPLE "%s" CLR_RESET
      " across the face with a large trout for %s%d" CLR_RESET " damage!",
      melee_tier_emoji[MELEE_FLAV_CRITICAL], atk_nick, tgt_nick,
      melee_tier_color[MELEE_FLAV_CRITICAL], dmg);
}

void
melee_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick, const melee_tunables_t *t, bool *need_refill)
{
  char slayer[MELEE_NICK_SZ + 8];
  char fallen[MELEE_NICK_SZ + 8];
  char body  [MELEE_LINE_SZ];
  char tmpl  [MELEE_LLM_TMPL_SZ];

  if(out == NULL || cap == 0)
    return;

  snprintf(slayer, sizeof(slayer), CLR_CYAN   "%s" CLR_RESET, slayer_nick);
  snprintf(fallen, sizeof(fallen), CLR_PURPLE "%s" CLR_RESET, fallen_nick);

  // The death line carries no tally, so {damage} expands to nothing —
  // the sanitiser rejects any death template that asks for one.
  if(melee_pool_take(MELEE_FLAV_DEATH, tmpl, sizeof(tmpl),
        t != NULL ? t->llm_refill_at : 0, need_refill) == SUCCESS)
    melee_tmpl_expand(body, sizeof(body), tmpl, slayer, fallen, "");

  else
  {
    melee_pool_fallback();
    snprintf(body, sizeof(body),
        melee_deaths[util_rand(MELEE_N(melee_deaths))], slayer, fallen);
  }

  snprintf(out, cap, "☠ %s", body);
}

// A tick is dressed like a blow — glyph, colour, and the survivor's
// remaining health — so decay reads as a peer of a swing rather than as
// a system message. The affliction's own colour carries the number,
// which is what tells a reader at a glance that no one swung.
void
melee_render_dot_tick(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp, int32_t hp_max,
    melee_dot_kind_t kind)
{
  char src [MELEE_NICK_SZ + 8];
  char tgt [MELEE_NICK_SZ + 8];
  char dmgs[32];
  char body[MELEE_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  kind = melee_dot_clamp(kind);

  snprintf(src, sizeof(src), CLR_CYAN   "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  snprintf(dmgs, sizeof(dmgs), "%s%d" CLR_RESET, melee_dot_color_of(kind), dmg);

  snprintf(body, sizeof(body),
      melee_dot_tick_tbl[kind][util_rand(melee_dot_tick_n[kind])],
      src, tgt, dmgs);

  if(hp > 0)
    snprintf(out, cap, "%s %s " CLR_GRAY "[%s — %d/%d hp]" CLR_RESET,
        melee_dot_emoji_of(kind), body, tgt_nick, hp, hp_max);

  else
    snprintf(out, cap, "%s %s", melee_dot_emoji_of(kind), body);
}

void
melee_render_dot_death(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, melee_dot_kind_t kind)
{
  char src [MELEE_NICK_SZ + 8];
  char tgt [MELEE_NICK_SZ + 8];
  char body[MELEE_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  kind = melee_dot_clamp(kind);

  snprintf(src, sizeof(src), CLR_CYAN   "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);

  snprintf(body, sizeof(body),
      melee_dot_death_tbl[kind][util_rand(melee_dot_death_n[kind])], src, tgt);

  // The same headstone the turn engine's death line carries, so a death
  // by decay is unmistakably the same event as a death by blade.
  snprintf(out, cap, "☠ %s", body);
}
