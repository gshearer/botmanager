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

// FORMAT CONTRACT: every entry takes exactly three `const char *` —
// attacker, target, damage — in that order. The damage arrives already
// rendered and colorized, so all three slots are %s and there is no %d
// anywhere. Breaking this crashes the plugin, not the line.
static const char *const melee_hits[] = {
  "%s lashes %s across the brow with a piwafwi-wrapped fist for %s damage.",
  "%s slips a hand-crossbow bolt between %s's ribs for %s damage.",
  "%s cracks %s with the flat of an adamantine blade for %s damage.",
  "%s rakes %s with House-signet talons for %s damage.",
  "%s drives a knee into %s's gut and leaves them wheezing — %s damage.",
  "%s scores %s's cheek with a poisoned nail for %s damage.",
  "%s slams %s against the Narbondel-lit stone for %s damage.",
  "%s parries, ripostes, and opens %s's forearm for %s damage.",
  "%s stamps on %s's instep with a spider-silk boot for %s damage.",
  "%s hurls a fistful of Underdark grit into %s's eyes for %s damage.",
  "%s clips %s with the pommel of a Melee-Magthere training blade for %s damage.",
  "%s trips %s face-first into a puddle of rothe filth for %s damage.",
  "%s snaps a whip-cord across %s's shoulders for %s damage.",
  "%s jabs %s with the butt of a driftglobe pole for %s damage.",
  "%s catches %s with a backhand heavy with House rings for %s damage.",
  "%s slices a shallow line down %s's arm, just to watch it bleed — %s damage.",
  "%s shoulder-checks %s into a stalagmite for %s damage.",
  "%s flicks faerie fire over %s and stabs the outline for %s damage.",
  "%s knocks the wind from %s with a spider-carved buckler for %s damage.",
  "%s hooks %s's ankle and drops them hard onto the cavern floor for %s damage.",
};

// FORMAT CONTRACT: exactly three `const char *` — attacker, target,
// damage — as above.
static const char *const melee_crits[] = {
  "%s drives a black-steel blade to the hilt through %s — Lolth leans closer. %s CRITICAL damage!",
  "%s opens %s from collarbone to hip; the wound steams in the cold. %s CRITICAL damage!",
  "%s lands a bone-splintering elbow that folds %s's ribs inward with a wet crunch. %s CRITICAL damage!",
  "%s empties a quiver of drow-poisoned bolts into %s at point-blank. %s CRITICAL damage!",
  "%s whispers to the Spider Queen and buries a dagger in %s to the guard. %s CRITICAL damage!",
  "%s drags %s face-first down a rasp of raw obsidian. %s CRITICAL damage!",
  "%s severs the tendons behind %s's knee; the scream carries to Menzoberranzan. %s CRITICAL damage!",
  "%s calls a globe of darkness and works %s over inside it, unseen and unhurried. %s CRITICAL damage!",
  "%s drives %s's skull into the stone until the stone gives first. %s CRITICAL damage!",
  "%s runs %s through and lifts them clean off the floor on the blade. %s CRITICAL damage!",
  "%s answers with a whip of fangs — five heads find %s at once. %s CRITICAL damage!",
  "%s brands %s with a red-hot House insignia and holds it there. %s CRITICAL damage!",
  "%s catches %s's wrist, reverses the arm, and snaps it the wrong way round. %s CRITICAL damage!",
  "%s throws %s down a chasm and hauls them back up by the hair to finish. %s CRITICAL damage!",
  "%s lets a yochlol's whisper guide the killing arc across %s's throat. %s CRITICAL damage!",
  "%s drives a hooked blade under %s's jaw and twists. %s CRITICAL damage!",
  "%s bathes a blade in drow poison and paints %s with it. %s CRITICAL damage!",
  "%s smashes %s through a sava board — pieces and teeth scatter alike. %s CRITICAL damage!",
  "%s crushes %s beneath an adamantine boot until something deep gives way. %s CRITICAL damage!",
  "%s carves the sigil of Bregan D'aerthe into %s's back, stroke by unhurried stroke. %s CRITICAL damage!",
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
// Rendering                                                           //
// ------------------------------------------------------------------ //

// The pool supplies the sentence and nothing else: the emoji prefix, the
// colorization, and the health tail below are melee's own, whether the
// words came from a model or from the tables above.
void
melee_render_blow(char *out, size_t cap, const char *atk_nick,
    const char *tgt_nick, int32_t dmg, bool crit, int32_t hp, int32_t hp_max,
    const melee_tunables_t *t, bool *need_refill)
{
  char atk [MELEE_NICK_SZ + 8];
  char tgt [MELEE_NICK_SZ + 8];
  char dmgs[32];
  char body[MELEE_LINE_SZ];
  char tmpl[MELEE_LLM_TMPL_SZ];

  if(out == NULL || cap == 0)
    return;

  snprintf(atk, sizeof(atk), CLR_CYAN   "%s" CLR_RESET, atk_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);

  if(crit)
    snprintf(dmgs, sizeof(dmgs), CLR_BOLD CLR_RED "%d" CLR_RESET, dmg);

  else
    snprintf(dmgs, sizeof(dmgs), CLR_YELLOW "%d" CLR_RESET, dmg);

  if(melee_pool_take(crit ? MELEE_FLAV_CRIT : MELEE_FLAV_HIT, tmpl,
        sizeof(tmpl), t != NULL ? t->llm_refill_at : 0,
        need_refill) == SUCCESS)
    melee_tmpl_expand(body, sizeof(body), tmpl, atk, tgt, dmgs);

  else
  {
    melee_pool_fallback();

    if(crit)
      snprintf(body, sizeof(body), melee_crits[util_rand(MELEE_N(melee_crits))],
          atk, tgt, dmgs);

    else
      snprintf(body, sizeof(body), melee_hits[util_rand(MELEE_N(melee_hits))],
          atk, tgt, dmgs);
  }

  // The survivor's remaining health rides on every non-fatal line; the
  // nick inside the gray block stays plain so the block reads as one.
  // A fatal blow carries no tally — the death line that follows says it.
  if(hp > 0)
    snprintf(out, cap, "%s%s " CLR_GRAY "[%s — %d/%d hp]" CLR_RESET,
        crit ? "💥 " : "⚔ ", body, tgt_nick, hp, hp_max);

  else
    snprintf(out, cap, "%s%s", crit ? "💥 " : "⚔ ", body);
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
