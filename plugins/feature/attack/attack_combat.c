// botmanager — MIT
// attack combat: the damage roll and the dressing around a finished line.
// Nothing here touches the database or the command context — it turns
// (a tier, a move, two nicknames and a number) into one line of text.
//
// THIS FILE OWNS NO WORDS. Every sentence the pit speaks comes from a
// character sheet (attack_class.c's registry) or, where a sheet said
// nothing, from that file's neutral fallbacks. What survives here is the
// engine's own dressing: the tier colour, the tier emoji, and the glyph
// and colour of each affliction kind. The four themed damage-tier tables
// that used to live here went out with ATK-5, and the engine now names
// no setting anywhere at all.
//
// Two exceptions, and both are deliberate. The trout: IRC culture rather
// than a setting, and it predates every theme this plugin has had. And
// the deferral line: stepping back is a mechanic of the engine and not a
// move, so no sheet has a section for it — the words are as plain and as
// setting-free as the rule they describe.

#define ATTACK_INTERNAL
#include "attack.h"

#include "colors.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ //
// The engine's dressing                                               //
// ------------------------------------------------------------------ //

// What the engine adds around a sentence it did not write, by tier:
// heavier blows read louder. Every prefix is a single-column code point,
// so atk_vis_len() stays honest.
static const char *const atk_tier_color[ATK_FLAV_DEATH] = {
  CLR_WHITE, CLR_YELLOW, CLR_BOLD CLR_ORANGE, CLR_BOLD CLR_RED
};

static const char *const atk_tier_emoji[ATK_FLAV_DEATH] = {
  "⚔ ", "⚔ ", "🗡 ", "💥 "
};

// ------------------------------------------------------------------ //
// Afflictions                                                         //
// ------------------------------------------------------------------ //

// Two parallel tables keyed by atk_dot_kind_t, and deliberately only
// two: the engine owns a kind's GLYPH and its COLOUR and nothing else.
// The noun — whether a `rot` is a creeping rot, a gnawing skeleton or a
// nanite bloom — belongs to the character sheet that inflicted it, and
// the neutral fallbacks for every word a sheet may omit live together in
// attack_class.c.

// Every glyph here must occupy exactly ONE display column, or
// atk_vis_len() starts lying and the round card skews. All eight are
// plain U+26xx/U+27xx symbols: no variation selectors, no
// emoji-presentation code points.
static const char *const atk_dot_emoji[ATK_DOT__COUNT] = {
  "✚", "☣", "⚗", "✺", "❆",
  "✧", "❖", "✞"
};

static const char *const atk_dot_color[ATK_DOT__COUNT] = {
  CLR_RED, CLR_GREEN, CLR_ORANGE, CLR_YELLOW, CLR_CYAN, CLR_PURPLE,
  CLR_BOLD CLR_RED, CLR_GRAY
};

// A kind arrives from a database column, so it is input like any other:
// an out-of-range value falls back to the first entry rather than
// indexing past the tables.
static atk_dot_kind_t
atk_dot_clamp(atk_dot_kind_t kind)
{
  return((kind >= 0 && kind < ATK_DOT__COUNT) ? kind : ATK_DOT_BLEED);
}

const char *
atk_dot_emoji_of(atk_dot_kind_t kind)
{
  return(atk_dot_emoji[atk_dot_clamp(kind)]);
}

const char *
atk_dot_color_of(atk_dot_kind_t kind)
{
  return(atk_dot_color[atk_dot_clamp(kind)]);
}

// ------------------------------------------------------------------ //
// The roll                                                            //
// ------------------------------------------------------------------ //

// One uniform draw over 1..dmg_max, and nothing else. It is deliberately
// the whole of the pit's damage model: whatever class the attacker was
// dealt, this line runs first and runs the same, and only afterwards is
// the sheet asked for a sentence of the size that came up. Reordering
// those two steps is the one change that would make class assignment
// matter.
//
// atk_tunables_load() has already guaranteed dmg_max >= 1, so the
// util_rand argument can never reach zero.
int32_t
atk_roll(const atk_tunables_t *t)
{
  if(t == NULL)
    return(0);

  return(1 + (int32_t)util_rand((int)t->dmg_max));
}

// ------------------------------------------------------------------ //
// Severity                                                            //
// ------------------------------------------------------------------ //

// The heaviest blow the current tunables can produce — which, now that
// the crit band is gone, is simply the top of the one roll. There is no
// second damage source left to widen it: that was the whole point of
// deleting the independent crit roll.
int32_t
atk_dmg_ceiling(const atk_tunables_t *t)
{
  if(t == NULL)
    return(1);

  // atk_tunables_load() already guarantees this; belt and braces,
  // because atk_severity() divides by it.
  return((t->dmg_max > 0) ? (int32_t)t->dmg_max : 1);
}

atk_flavour_t
atk_severity(const atk_tunables_t *t, int32_t dmg)
{
  int32_t pct;

  if(t == NULL)
    return(ATK_FLAV_MINOR);

  if(dmg < 0)
    dmg = 0;

  pct = (int32_t)((int64_t)dmg * 100 / atk_dmg_ceiling(t));

  if(pct > 100)          // a tunable moved mid-round; clamp rather than
    pct = 100;           // fall off the end of the band

  if(pct >= (int32_t)t->sev_crit_at)
    return(ATK_FLAV_CRITICAL);

  if(pct >= (int32_t)t->sev_major_at)
    return(ATK_FLAV_MAJOR);

  if(pct >= (int32_t)t->sev_medium_at)
    return(ATK_FLAV_MEDIUM);

  return(ATK_FLAV_MINOR);
}

// ------------------------------------------------------------------ //
// Rendering                                                           //
// ------------------------------------------------------------------ //

// Every sentence the pit speaks arrives by exactly one of two roads, and
// they must never be confused:
//
//   a MOVE is a character sheet's {macro} template, expanded with data
//   through atk_macro_expand() — it is never a format string, and the
//   loader refuses any '%' in one;
//
//   a FALLBACK is one of attack_class.c's neutral lines, a printf
//   template with a fixed slot count stated beside it there.
//
// The two are resolved here, once, so that no renderer below has to know
// which road its words came down.
static void
atk_body(char *out, size_t cap, const atk_move_t *move, const char *fb,
    const char *attacker, const char *target, const char *damage,
    const char *heal, const char *affliction)
{
  if(move != NULL && move->text[0] != '\0')
  {
    atk_macro_expand(out, cap, move->text, attacker, target, damage,
        heal, affliction);
    return;
  }

  // Every neutral template takes (attacker, target, <the number this
  // line carries>) and stops where its own contract says — a line that
  // names no number never reads the third slot, and none of them has a
  // slot for the affliction, which they name in words instead. The
  // surplus argument is what the varargs contract permits, and it is what
  // keeps one call site here rather than five.
  if(damage == NULL)
    damage = (heal != NULL) ? heal : "";

  snprintf(out, cap, fb, attacker, target, damage);
}

// A tier arrives from a roll and a database column alike, so it is input
// like any other: anything outside the four damage bands is drawn as the
// lightest rather than indexing past the dressing tables.
static atk_flavour_t
atk_tier_clamp(atk_flavour_t tier)
{
  return((tier >= ATK_FLAV_MINOR && tier < ATK_FLAV_DEATH)
      ? tier : ATK_FLAV_MINOR);
}

// The affliction's own word, in the affliction's own colour. The sheet
// that inflicted it chose the noun and the row carries it, so a class's
// wording survives every tick; an empty one falls back to the engine's
// plain word for that kind.
static void
atk_noun_render(char *out, size_t cap, atk_dot_kind_t kind,
    const char *noun)
{
  if(noun == NULL || noun[0] == '\0')
    noun = atk_fallback_noun(kind);

  snprintf(out, cap, "%s%s" CLR_RESET, atk_dot_color_of(kind), noun);
}

// The sheet supplies the sentence and nothing else: the emoji prefix, the
// colorization, the deferral badge and the health tail are the engine's
// own. The TIER is the engine's too, and is passed in rather than read
// off the move — it is the roll's dressing, and it must be right even in
// the case where the attacker's class had nothing to say.
void
atk_render_blow(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_flavour_t tier, const atk_move_t *move,
    int32_t dmg, uint32_t bonus_pct, int32_t hp, int32_t hp_max)
{
  char src  [ATK_NICK_SZ + 8];
  char tgt  [ATK_NICK_SZ + 8];
  char dmgs [32];
  char badge[32] = "";
  char body [ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  tier = atk_tier_clamp(tier);

  snprintf(src, sizeof(src), CLR_CYAN   "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  snprintf(dmgs, sizeof(dmgs), "%s%d" CLR_RESET, atk_tier_color[tier], dmg);

  atk_body(body, sizeof(body), move, atk_fallback_blow(),
      src, tgt, dmgs, NULL, NULL);

  // A bonused blow says why it is heavy. Without the badge a deferred
  // turn simply looks like a suspiciously lucky roll.
  if(bonus_pct > 0)
    snprintf(badge, sizeof(badge),
        " " CLR_BOLD CLR_YELLOW "⚡+%" PRIu32 "%%" CLR_RESET, bonus_pct);

  // The survivor's remaining health rides on every non-fatal line; the
  // nick inside the gray block stays plain so the block reads as one.
  // A fatal blow carries no tally — the death line that follows says it.
  if(hp > 0)
    snprintf(out, cap, "%s%s%s " CLR_GRAY "[%s — %d/%d hp]" CLR_RESET,
        atk_tier_emoji[tier], body, badge, tgt_nick, hp, hp_max);

  else
    snprintf(out, cap, "%s%s%s", atk_tier_emoji[tier], body, badge);
}

// A heal wears the same shape as a blow — glyph, colorized names, the
// number, and the health tail — because it costs exactly what a blow
// costs: one turn. What differs is the palette. The target is GREEN
// rather than purple: purple marks whoever is having something done *to*
// them, and nothing is being done to the mended.
//
// The number is the movement of the bar, not the roll behind it. The
// clamp lives at the call site, and by the time a line reaches here there
// is only one number left to speak.
void
atk_render_heal(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, const atk_move_t *move, int32_t amt,
    uint32_t bonus_pct, int32_t hp, int32_t hp_max)
{
  char src  [ATK_NICK_SZ + 8];
  char tgt  [ATK_NICK_SZ + 8];
  char amts [32];
  char badge[32] = "";
  char body [ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  snprintf(src, sizeof(src), CLR_CYAN  "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_GREEN "%s" CLR_RESET, tgt_nick);
  snprintf(amts, sizeof(amts), CLR_BOLD CLR_GREEN "%d" CLR_RESET, amt);

  // {heal}, never {damage}: the loader keeps the two tokens apart section
  // by section, and atk_body() hands whichever one this line owns to the
  // neutral template's third slot.
  atk_body(body, sizeof(body), move, atk_fallback_heal(),
      src, tgt, NULL, amts, NULL);

  if(bonus_pct > 0)
    snprintf(badge, sizeof(badge),
        " " CLR_BOLD CLR_YELLOW "⚡+%" PRIu32 "%%" CLR_RESET, bonus_pct);

  // A heal always leaves somebody standing, so the tally always rides.
  snprintf(out, cap, "✚ %s%s " CLR_GRAY "[%s — %d/%d hp]" CLR_RESET,
      body, badge, tgt_nick, hp, hp_max);
}

// The line a blow that went wide speaks after the class move has had its
// say. It is the ENGINE's own and deliberately not a sheet's: going wide
// is a rule rather than an action, no class caused it and none may claim
// it, so the words are as plain and as setting-free as the mechanic —
// exactly the trade the deferral line makes.
//
// ONE number for everybody, spoken once and then the roster it landed on.
// A number per victim would read as several blows instead of as one that
// missed its aim, and the ledger rolled only one.
void
atk_render_sweep(char *out, size_t cap, atk_flavour_t tier, int32_t dmg,
    const atk_victim_t *v, uint32_t n)
{
  char     roster[ATK_ROSTER_SZ];
  size_t   off   = 0;
  uint32_t shown = 0;
  uint32_t i;

  if(out == NULL || cap == 0 || v == NULL)
    return;

  tier      = atk_tier_clamp(tier);
  roster[0] = '\0';

  for(i = 0; i < n; i++)
  {
    char   cell[ATK_NICK_SZ + 96];
    size_t len;

    // The fallen go gray where the standing stay purple, so the reader
    // does not have to parse four numbers to see who is still up.
    snprintf(cell, sizeof(cell), "%s%s%s" CLR_RESET " %d/%d",
        (off > 0) ? " · " : "",
        v[i].fell ? CLR_GRAY : CLR_PURPLE, v[i].nick,
        v[i].hp_left, v[i].hp_max);

    len = strlen(cell);

    // Reserve room for the tail: a roster that ran out of line has to be
    // able to say so, and a truncated nickname could name the wrong
    // combatant entirely.
    if(off + len + 24 >= sizeof(roster))
      break;

    memcpy(roster + off, cell, len + 1);
    off += len;
    shown++;
  }

  if(shown < n)
    snprintf(roster + off, sizeof(roster) - off, " … and %" PRIu32 " more",
        n - shown);

  snprintf(out, cap,
      "🌀 The blow goes wide — everyone takes %s%d" CLR_RESET ": %s",
      atk_tier_color[tier], dmg, roster);
}

// A surrendered turn. Four variants so a pit full of hesitation does not
// read like a stuck record, each naming the bonus the surrender bought —
// the number is the whole point of the move, and a line that withheld it
// would leave a reader wondering what they had just paid a turn for.
//
// Deliberately class-agnostic: every other line in the pit is a sheet's,
// and this one cannot be, because a deferral is a rule rather than an
// action. The accumulated total is spoken, never the step, because the
// total is what the next blow will actually be multiplied by.
static const char *const atk_defer_line[] = {
  "%s steps back and lets the wave pass. %s waits on their next move.",
  "%s holds their turn, watching for the opening. %s is coiled behind it.",
  "%s gives up the swing and takes the patience instead — %s.",
  "%s waits. Whatever lands next lands harder: %s."
};

void
atk_render_defer(char *out, size_t cap, const char *nick, uint32_t bonus_pct)
{
  const int n = (int)(sizeof(atk_defer_line) / sizeof(atk_defer_line[0]));
  char      who  [ATK_NICK_SZ + 8];
  char      badge[32];
  char      body [ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  snprintf(who, sizeof(who), CLR_CYAN "%s" CLR_RESET, nick);
  snprintf(badge, sizeof(badge),
      CLR_BOLD CLR_YELLOW "⚡+%" PRIu32 "%%" CLR_RESET, bonus_pct);
  snprintf(body, sizeof(body), atk_defer_line[util_rand(n)], who, badge);

  snprintf(out, cap, "⏳ %s", body);
}

// The oldest joke on IRC, kept for the one blow that earns it: a
// critical that kills. It stands where the tier line would have stood —
// it is the line that carries the number — and the ordinary death line
// still follows it, so the death-line-before-eject law is untouched. No
// health tail, because a fatal blow carries no tally.
//
// The one piece of flavour no character sheet can override, and the one
// place this file still owns a sentence.
void
atk_render_trout(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg)
{
  if(out == NULL || cap == 0)
    return;

  snprintf(out, cap,
      "%s" CLR_CYAN "%s" CLR_RESET " slaps " CLR_PURPLE "%s" CLR_RESET
      " across the face with a large trout for %s%d" CLR_RESET " damage!",
      atk_tier_emoji[ATK_FLAV_CRITICAL], src_nick, tgt_nick,
      atk_tier_color[ATK_FLAV_CRITICAL], dmg);
}

// The death line carries no tally: the blow that killed already said the
// number.
void
atk_render_death(char *out, size_t cap, const char *slayer_nick,
    const char *fallen_nick, const atk_move_t *move)
{
  char slayer[ATK_NICK_SZ + 8];
  char fallen[ATK_NICK_SZ + 8];
  char body  [ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  snprintf(slayer, sizeof(slayer), CLR_CYAN   "%s" CLR_RESET, slayer_nick);
  snprintf(fallen, sizeof(fallen), CLR_PURPLE "%s" CLR_RESET, fallen_nick);

  atk_body(body, sizeof(body), move, atk_fallback_death(),
      slayer, fallen, NULL, NULL, NULL);

  snprintf(out, cap, "☠ %s", body);
}

void
atk_render_dot_inflict(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_dot_kind_t kind, const char *noun,
    const atk_move_t *move, uint32_t bonus_pct)
{
  char src  [ATK_NICK_SZ + 8];
  char tgt  [ATK_NICK_SZ + 8];
  char aff  [ATK_NOUN_SZ + 16];
  char badge[32] = "";
  char body [ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  kind = atk_dot_clamp(kind);

  snprintf(src, sizeof(src), CLR_CYAN   "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  atk_noun_render(aff, sizeof(aff), kind, noun);

  // The inflict line names no number. The engine has already rolled the
  // whole of this affliction's damage and will pay it out one tick at a
  // time; saying it here would tell the victim exactly how long they
  // have, which the pit never does.
  atk_body(body, sizeof(body), move, atk_fallback_dot_inflict(kind),
      src, tgt, NULL, NULL, aff);

  // A bonused affliction says why it is heavy, exactly as a bonused blow
  // does. Without it a deferral that happened to land a wound looks like
  // a turn spent for nothing: the badge is the only place the spent
  // bonus surfaces on this path, since the line names no number and the
  // damage arrives later, one tick at a time.
  if(bonus_pct > 0)
    snprintf(badge, sizeof(badge),
        " " CLR_BOLD CLR_YELLOW "⚡+%" PRIu32 "%%" CLR_RESET, bonus_pct);

  // The glyph that will mark the victim on the round card leads the
  // line, so the two read as the same thing.
  snprintf(out, cap, "%s %s%s", atk_dot_emoji_of(kind), body, badge);
}

// A tick is dressed like a blow — glyph, colour, and the survivor's
// remaining health — so decay reads as a peer of a swing rather than as
// a system message. The affliction's own colour carries the number,
// which is what tells a reader at a glance that no one swung.
void
atk_render_dot_tick(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, int32_t dmg, int32_t hp, int32_t hp_max,
    atk_dot_kind_t kind, const char *noun, const atk_move_t *move)
{
  char src [ATK_NICK_SZ + 8];
  char tgt [ATK_NICK_SZ + 8];
  char aff [ATK_NOUN_SZ + 16];
  char dmgs[32];
  char body[ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  kind = atk_dot_clamp(kind);

  snprintf(src, sizeof(src), CLR_CYAN   "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  snprintf(dmgs, sizeof(dmgs), "%s%d" CLR_RESET, atk_dot_color_of(kind), dmg);
  atk_noun_render(aff, sizeof(aff), kind, noun);

  atk_body(body, sizeof(body), move, atk_fallback_decay(kind),
      src, tgt, dmgs, NULL, aff);

  if(hp > 0)
    snprintf(out, cap, "%s %s " CLR_GRAY "[%s — %d/%d hp]" CLR_RESET,
        atk_dot_emoji_of(kind), body, tgt_nick, hp, hp_max);

  else
    snprintf(out, cap, "%s %s", atk_dot_emoji_of(kind), body);
}

void
atk_render_dot_death(char *out, size_t cap, const char *src_nick,
    const char *tgt_nick, atk_dot_kind_t kind, const char *noun,
    const atk_move_t *move)
{
  char src [ATK_NICK_SZ + 8];
  char tgt [ATK_NICK_SZ + 8];
  char aff [ATK_NOUN_SZ + 16];
  char body[ATK_LINE_SZ];

  if(out == NULL || cap == 0)
    return;

  kind = atk_dot_clamp(kind);

  snprintf(src, sizeof(src), CLR_CYAN   "%s" CLR_RESET, src_nick);
  snprintf(tgt, sizeof(tgt), CLR_PURPLE "%s" CLR_RESET, tgt_nick);
  atk_noun_render(aff, sizeof(aff), kind, noun);

  // No tally, exactly as the blade's death line carries none.
  atk_body(body, sizeof(body), move, atk_fallback_decay_kill(kind),
      src, tgt, NULL, NULL, aff);

  // The same headstone the turn engine's death line carries, so a death
  // by decay is unmistakably the same event as a death by blade.
  snprintf(out, cap, "☠ %s", body);
}
