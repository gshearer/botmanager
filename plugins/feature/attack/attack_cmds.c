// botmanager — MIT
// attack command surface: `!attack <nick>`, the turn engine that resolves
// one blow. Registered users only, group chat only — both enforced by
// the command system's per-leaf gate, never re-checked here.
//
// The database is authoritative for round state, and one plugin-global
// mutex serialises the whole read-modify-write of a turn so two blows
// arriving on different worker threads cannot both decrement the same
// combatant's health.

#define ATTACK_INTERNAL
#include "attack.h"

#include "bot.h"
#include "colors.h"
#include "util.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Serialises steps 5-10 of a turn: load the round, enrol, gate the wave,
// roll, write, announce. Target resolution and the presence probe run
// *outside* it — they mutate nothing and would only widen the window
// while holding a lock the method drivers know nothing about. The decay
// task takes the same lock for the same reason (see attack.h).
pthread_mutex_t atk_turn_lock = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------------ //
// Resolution helpers                                                  //
// ------------------------------------------------------------------ //

typedef struct
{
  const char *nick;
  uint32_t    seen;    // members enumerated at all
  bool        found;
} atk_presence_t;

static void
atk_presence_cb(const char *nick, void *data)
{
  atk_presence_t *p = data;

  p->seen++;

  if(nick != NULL && strcasecmp(nick, p->nick) == 0)
    p->found = true;
}

// Is `nick` in this room? A driver with no member tracking enumerates
// nobody, which must read as "cannot tell", never as "absent".
static bool
atk_target_present(const cmd_ctx_t *ctx, const char *nick)
{
  atk_presence_t p = { .nick = nick, .seen = 0, .found = false };

  method_list_channel(ctx->msg->inst, ctx->msg->channel,
      atk_presence_cb, &p);

  return(p.seen == 0 || p.found);
}

// Map a typed nickname onto a namespace-scoped username: first through
// the bot's authenticated sessions, then by taking the token as a
// username outright. Anything else is a ghost.
static bool
atk_resolve_target(const cmd_ctx_t *ctx, const userns_t *ns,
    const char *nick, char *out, size_t cap)
{
  const char *user = bot_session_find(ctx->bot, ctx->msg->inst, nick);

  if(user != NULL && user[0] != '\0')
  {
    snprintf(out, cap, "%s", user);
    return(true);
  }

  if(userns_user_exists(ns, nick))
  {
    snprintf(out, cap, "%s", nick);
    return(true);
  }

  return(false);
}

// ------------------------------------------------------------------ //
// attack --end                                                        //
// ------------------------------------------------------------------ //

// Ending a game is not winning one: the round is retired with no slayer,
// no fallen, and not one kill, death or point of damage recorded. Live
// afflictions need no attention here — a round that is no longer active
// stops its own afflictions from ever ticking again, and the decay task
// sweeps their rows on its next pass.
//
// This needs no command definition and no permission gate of its own: it
// is the `attack` leaf's own argument, so "anyone who can attack can end
// it" is satisfied by construction. And an IRC nickname can never begin
// with '-' (RFC 2812), so the comparison cannot shadow a real target.
static void
atk_cmd_end(const cmd_ctx_t *ctx, const userns_t *ns)
{
  atk_round_t round;
  char        age [64];
  char        line[ATK_LINE_SZ];
  const char *who;
  bool        found;

  who = (ctx->msg->nickname[0] != '\0') ? ctx->msg->nickname : ctx->username;

  pthread_mutex_lock(&atk_turn_lock);

  found = atk_db_round_find(ns->id, method_inst_name(ctx->msg->inst),
      ctx->msg->channel, &round);

  if(found)
    atk_db_round_abandon(round.id);

  pthread_mutex_unlock(&atk_turn_lock);

  if(!found)
  {
    cmd_reply(ctx, "⚔ Nothing is happening here.");
    return;
  }

  util_fmt_duration((time_t)round.age, age, sizeof(age));

  snprintf(line, sizeof(line),
      "⚔ " CLR_CYAN "%s" CLR_RESET " calls the fight. It ran %s and "
      "nobody won it. The next blow starts a new one.", who, age);
  cmd_reply(ctx, line);

  clam(CLAM_INFO, ATK_CTX, "round %" PRId64 " ended by %s after %" PRId64 "s",
      round.id, ctx->username, round.age);
}

// ------------------------------------------------------------------ //
// attack <nick>                                                       //
// ------------------------------------------------------------------ //

static void
atk_cmd_attack(const cmd_ctx_t *ctx)
{
  atk_tunables_t  t;
  atk_round_t     round = { 0 };
  atk_player_t    src;
  atk_player_t    tgt;
  atk_blow_t      blow;
  atk_move_t      move;
  atk_flavour_t   tier;
  userns_t       *ns;
  const char     *nick;
  const char     *method;
  const char     *channel;
  const char     *src_nick;
  char            tgt_user[ATK_USER_SZ];
  char            src_class[ATK_CLASS_NAME_SZ];
  char            tgt_class[ATK_CLASS_NAME_SZ];
  char            line[ATK_LINE_SZ];
  char            roster[ATK_ROSTER_SZ];
  int32_t         dmg;
  int32_t         new_hp;
  bool            spoken;             // the sheet answered; else neutral
  bool            dot   = false;      // this turn is an affliction
  bool            crit  = false;
  bool            fatal = false;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  // USERNS_GROUP_USER means dispatch has already rejected anonymous
  // callers; a NULL here would be a gate regression, not user input.
  if(ctx->username == NULL || ctx->username[0] == '\0')
  {
    clam(CLAM_WARN, ATK_CTX, "attack reached the pit unauthenticated");
    return;
  }

  nick = (ctx->parsed != NULL && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;

  if(nick == NULL || nick[0] == '\0')
  {
    cmd_reply(ctx, "usage: attack <nick>|--end");
    return;
  }

  // Before target resolution, and never after: --end is an argument, not
  // a child command, precisely so that it cannot shadow a nickname.
  if(strcmp(nick, "--end") == 0)
  {
    atk_cmd_end(ctx, ns);
    return;
  }

  atk_tunables_load(&t);

  method   = method_inst_name(ctx->msg->inst);
  channel  = ctx->msg->channel;
  src_nick = (ctx->msg->nickname[0] != '\0')
      ? ctx->msg->nickname : ctx->username;

  if(!atk_resolve_target(ctx, ns, nick, tgt_user, sizeof(tgt_user)))
  {
    snprintf(line, sizeof(line),
        "⚔ " CLR_PURPLE "%s" CLR_RESET
        " is no one the pit has ever heard of.", nick);
    cmd_reply(ctx, line);
    return;
  }

  if(strcasecmp(tgt_user, ctx->username) == 0)
  {
    cmd_reply(ctx, "⚔ You cannot attack yourself. Find someone else.");
    return;
  }

  if(!atk_target_present(ctx, nick))
  {
    snprintf(line, sizeof(line),
        "⚔ " CLR_PURPLE "%s" CLR_RESET
        " is not in this room. The pit takes only the present.", nick);
    cmd_reply(ctx, line);
    return;
  }

  pthread_mutex_lock(&atk_turn_lock);

  // ---- 5. the round ------------------------------------------------ //

  // One clock, and it measures the brawl's whole life rather than its
  // silence: a round runs for round_max_secs and then the game is over.
  // A pit that has merely gone quiet is ended by `attack --end`, which
  // is the better instrument because everyone standing in it can see it.
  if(atk_db_round_find(ns->id, method, channel, &round) &&
     round.age > (int64_t)t.round_max_secs)
  {
    atk_db_round_abandon(round.id);
    cmd_reply(ctx, "⚔ The old fight is over. A new one begins.");

    // Clear the whole snapshot, not just the id: the dead round's
    // top_crit would otherwise set the bar for the fresh one.
    memset(&round, 0, sizeof(round));
  }

  if(round.id <= 0)
  {
    round.id   = atk_db_round_open(ns->id, method, channel, ctx->username);
    round.wave = 1;

    if(round.id <= 0)
    {
      pthread_mutex_unlock(&atk_turn_lock);
      cmd_reply(ctx, "☠ The pit will not answer — no round could be opened.");
      return;
    }
  }

  // ---- 6. enrolment ------------------------------------------------ //
  //
  // Each combatant is dealt a class here and keeps it for the whole
  // brawl. The two draws are INDEPENDENT and duplicates are allowed: the
  // brief says random, and under the charter a duplicate costs nobody
  // anything, because two sheets of the same name roll exactly the same
  // numbers as two of different ones.

  atk_class_pick(src_class, sizeof(src_class));
  atk_class_pick(tgt_class, sizeof(tgt_class));

  atk_db_player_enrol(round.id, ns->id, ctx->username, src_nick,
      src_class, (int32_t)t.start_hp);
  atk_db_player_enrol(round.id, ns->id, tgt_user, nick,
      tgt_class, (int32_t)t.start_hp);

  if(!atk_db_player_get(round.id, ctx->username, &src) ||
     !atk_db_player_get(round.id, tgt_user, &tgt))
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "☠ The pit will not answer — the roster is unreadable.");
    return;
  }

  // ---- 7. the wave gate -------------------------------------------- //

  if(src.last_wave >= round.wave)
  {
    if(atk_db_pending(round.id, round.wave, roster, sizeof(roster)) ==
           SUCCESS && roster[0] != '\0')
      snprintf(line, sizeof(line),
          "⚔ You have spent your blow this wave. Still standing idle: "
          CLR_CYAN "%s" CLR_RESET ".", roster);

    else
      snprintf(line, sizeof(line),
          "⚔ You have spent your blow this wave.");

    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, line);
    return;
  }

  if(tgt.hp <= 0)
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "☠ That one is already cooling. Find a living quarrel.");
    return;
  }

  // ---- 8. the roll, and only then the words ------------------------- //
  //
  // THE LAW, in the order it must run (attack_class.c's charter):
  //
  //   1. the ENGINE rolls the number — uniform over 1..dmg_max, and
  //      identical for everybody standing in the pit;
  //   2. the ENGINE derives the tier from that number;
  //   3. only THEN does the class matter, and only to choose which of
  //      its sentences of that size gets spoken.
  //
  // Reordering these reintroduces exactly the unfairness the charter
  // exists to prevent: a class would then decide how hard it hits rather
  // than only how it sounds. A sheet with forty critical lines and a
  // sheet with one hit precisely as hard as each other.
  dmg  = atk_roll(&t);
  tier = atk_severity(&t, dmg);
  crit = (tier == ATK_FLAV_CRITICAL);

  // The stem stored at enrolment, unless the sheet has left the registry
  // since — a reload between one turn and the next must never cost
  // somebody their swing.
  atk_class_for(src.class, src_class, sizeof(src_class));

  // Whether the turn is an affliction is the ENGINE's roll too. A class
  // with forty affliction moves inflicts no more often than one with
  // three; it merely repeats itself less. No health check is needed here
  // and none belongs here: a DOT turn lands no instant damage, so it can
  // never be the blow that closes the round, and the kill it may
  // eventually land arrives through the decay task with the round still
  // open around it.
  dot = (t.dot_chance_pct > 0 &&
         atk_class_has(src_class, ATK_SEC_DOT) &&
         util_rand(100) < (int)t.dot_chance_pct);

  spoken = (atk_class_move(src_class, dot ? ATK_SEC_DOT : ATK_SEC_DAMAGE,
        tier, 0, &move) == SUCCESS);

  if(!spoken)
  {
    // Unreachable by the loader's coverage gate, and handled anyway: the
    // engine's neutral line stands in rather than the pit falling silent
    // mid-turn.
    memset(&move, 0, sizeof(move));
    clam(CLAM_WARN, ATK_CTX, "class '%s' had no move for tier %d",
        src_class, (int)tier);
  }

  // ---- 8a. the affliction turn -------------------------------------- //
  //
  // A DOT turn deals no instant damage. It spends the SAME roll, paid out
  // one tick at a time, so an affliction and an instant blow of the same
  // number cost the victim exactly the same in the end — which is what
  // makes owning affliction moves a difference in rhythm and never in
  // strength.

  if(dot)
  {
    const uint32_t ticks =
        ((uint32_t)dmg < t.dot_max_ticks) ? (uint32_t)dmg : t.dot_max_ticks;

    atk_dot_new_t wound = {
      .round_id    = round.id,
      .ns_id       = ns->id,
      .method      = method,
      .channel     = channel,
      .victim      = tgt_user,
      .victim_nick = nick,
      .source      = ctx->username,
      .source_nick = src_nick,
      .noun        = (move.noun[0] != '\0')
                         ? move.noun : atk_fallback_noun(move.kind),
      .kind        = move.kind,
      .dmg_plan    = dmg,
      .max_ticks   = ticks,
      .tick_secs   = t.dot_tick_secs,
      .stack_max   = t.dot_stack_max,
    };

    bool landed;

    if(atk_db_turn_spend(round.id, ns->id, ctx->username, src_nick,
          round.wave) != SUCCESS)
    {
      pthread_mutex_unlock(&atk_turn_lock);
      cmd_reply(ctx, "☠ The turn landed nowhere — the pit's ledger "
                     "refused it.");
      return;
    }

    // At the stack cap the insert lands no row and says so by returning
    // FAIL. The turn is still spent: a wasted affliction against an
    // already-afflicted victim costs the same as any other wasted turn,
    // and refunding it would hand affliction classes a free retry.
    landed = (atk_db_dot_inflict(&wound) == SUCCESS);

    if(landed)
    {
      atk_render_dot_inflict(line, sizeof(line), src_nick, nick,
          wound.kind, wound.noun, spoken ? &move : NULL);
      cmd_reply(ctx, line);
    }

    else
      clam(CLAM_DEBUG, ATK_CTX,
          "round %" PRId64 ": no affliction landed on %s", round.id,
          tgt_user);

    pthread_mutex_unlock(&atk_turn_lock);

    // Last, and with no lock held: the task system is deliberately kept
    // off the turn path entirely.
    if(landed)
      atk_dot_wake();

    return;
  }

  // ---- 9. the instant blow ------------------------------------------ //

  new_hp = (tgt.hp > dmg) ? tgt.hp - dmg : 0;
  fatal  = (new_hp == 0);

  blow = (atk_blow_t){
    .round_id = round.id,
    .ns_id    = ns->id,
    .src_user = ctx->username,
    .src_nick = src_nick,
    .tgt_user = tgt_user,
    .tgt_nick = nick,
    .dmg      = dmg,
    .wave     = round.wave,
    .crit     = crit,
    .new_top  = (crit && dmg > round.top_crit),
    .fatal    = fatal,
  };

  if(atk_db_blow_apply(&blow) != SUCCESS)
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "☠ The blow landed nowhere — the pit's ledger refused it.");
    return;
  }

  // ---- 10. announce, still under the lock so two turns cannot ------- //
  //          interleave their narration                               //

  // A critical that kills gets the trout instead of the tier line — it is
  // the line that carries the number, and the sheet's own words stand
  // aside for the one joke no class may override.
  if(crit && fatal)
    atk_render_trout(line, sizeof(line), src_nick, nick, dmg);

  else
    atk_render_blow(line, sizeof(line), src_nick, nick, tier,
        spoken ? &move : NULL, dmg, 0, new_hp, tgt.hp_max);

  cmd_reply(ctx, line);

  // The killing blow speaks in the slayer's own voice where their sheet
  // has one, and in the engine's plain words where it does not.
  if(fatal)
  {
    atk_move_t last;
    const bool own = (atk_class_move(src_class, ATK_SEC_DEATH, 0, 0, &last)
        == SUCCESS);

    atk_render_death(line, sizeof(line), src_nick, nick, own ? &last : NULL);
    cmd_reply(ctx, line);
  }

  pthread_mutex_unlock(&atk_turn_lock);

  // ---- the door ----------------------------------------------------- //
  // Strictly after the death line: on IRC the strongest ejection is a
  // KILL, and nothing sent afterwards would ever reach the fallen.

  if(fatal)
  {
    method_eject_t force = METHOD_EJECT_NONE;
    char           reason[128];

    if(t.eject_on_death)
    {
      force = method_eject_probe(ctx->msg->inst, channel, nick);

      if(force != METHOD_EJECT_NONE)
      {
        snprintf(reason, sizeof(reason), "slain by %s in the pit", src_nick);
        method_eject(ctx->msg->inst, channel, nick, force, reason);
      }
    }

    clam(CLAM_INFO, ATK_CTX, "round %" PRId64 ": %s slew %s (eject=%d)",
        round.id, ctx->username, tgt_user, (int)force);
  }
}

// ------------------------------------------------------------------ //
// heal [nick]                                                         //
// ------------------------------------------------------------------ //

// "a warrior" / "an oracle". A class stem is validated alnum by the
// loader, so the first byte is all this needs to look at.
static const char *
atk_article(const char *word)
{
  const char c = (word != NULL) ? (char)tolower((unsigned char)word[0]) : '\0';

  return((c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')
      ? "An" : "A");
}

// Mending instead of striking. It is the one place classes are more than
// cosmetic, and that is the operator's own brief: a cleric may spend a
// turn restoring hit points and a wizard may not. It costs a whole turn,
// which is what keeps it a trade rather than a gift — see the fairness
// charter in TODO.md §2, which names this as the ONLY such asymmetry.
static void
atk_cmd_heal(const cmd_ctx_t *ctx)
{
  atk_tunables_t t;
  atk_round_t    round;
  atk_player_t   src;
  atk_player_t   tgt;
  atk_heal_t     heal;
  atk_move_t     move;
  userns_t      *ns;
  const char    *nick;
  const char    *src_nick;
  char           tgt_user[ATK_USER_SZ];
  char           src_class[ATK_CLASS_NAME_SZ];
  char           line[ATK_LINE_SZ];
  char           roster[ATK_ROSTER_SZ];
  int32_t        amt;
  int32_t        delta;
  bool           major;
  bool           spoken;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  if(ctx->username == NULL || ctx->username[0] == '\0')
  {
    clam(CLAM_WARN, ATK_CTX, "heal reached the pit unauthenticated");
    return;
  }

  atk_tunables_load(&t);

  src_nick = (ctx->msg->nickname[0] != '\0')
      ? ctx->msg->nickname : ctx->username;

  // ---- resolution, outside the lock: it mutates nothing, and the ----- //
  //      presence probe must not run under a lock the method drivers    //
  //      know nothing about                                             //

  nick = (ctx->parsed != NULL && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;

  // Healing yourself is allowed, and it is the default. There is no
  // self-refusal here; that rule belongs to the blade.
  if(nick == NULL || nick[0] == '\0')
  {
    nick = src_nick;
    snprintf(tgt_user, sizeof(tgt_user), "%s", ctx->username);
  }

  else
  {
    if(!atk_resolve_target(ctx, ns, nick, tgt_user, sizeof(tgt_user)))
    {
      snprintf(line, sizeof(line),
          "✚ " CLR_GREEN "%s" CLR_RESET
          " is no one the pit has ever heard of.", nick);
      cmd_reply(ctx, line);
      return;
    }

    if(!atk_target_present(ctx, nick))
    {
      snprintf(line, sizeof(line),
          "✚ " CLR_GREEN "%s" CLR_RESET
          " is not in this room. You cannot mend what is not here.", nick);
      cmd_reply(ctx, line);
      return;
    }
  }

  pthread_mutex_lock(&atk_turn_lock);

  // ---- the round -- and `!heal` never opens or closes one ------------ //
  //
  // Only `!attack` starts a game and only `!attack` (or `attack --end`)
  // finishes one. A stale round is left exactly as it was found: mending
  // is not an event that should retire anything.

  if(!atk_db_round_find(ns->id, method_inst_name(ctx->msg->inst),
        ctx->msg->channel, &round))
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "✚ Nothing is happening here. Start something with "
                   "!attack <nick>.");
    return;
  }

  // Both must already be in the fight. `!heal` must never be a free,
  // damage-less way into one.
  if(!atk_db_player_get(round.id, ctx->username, &src))
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "✚ You are not in this fight. Join it the usual way.");
    return;
  }

  if(!atk_db_player_get(round.id, tgt_user, &tgt))
  {
    pthread_mutex_unlock(&atk_turn_lock);
    snprintf(line, sizeof(line),
        "✚ " CLR_GREEN "%s" CLR_RESET " is not in this fight.", nick);
    cmd_reply(ctx, line);
    return;
  }

  // The stem stored at enrolment, unless the sheet has left the registry
  // since — the same courtesy the blade extends.
  atk_class_for(src.class, src_class, sizeof(src_class));

  if(!atk_class_has(src_class, ATK_SEC_HEAL))
  {
    pthread_mutex_unlock(&atk_turn_lock);
    snprintf(line, sizeof(line),
        "✚ %s %s knows nothing of mending. Pick up a blade.",
        atk_article(src_class), src_class);
    cmd_reply(ctx, line);
    return;
  }

  // The wave gate, identical to a blow's: mending costs a turn, and a
  // turn is a turn.
  if(src.last_wave >= round.wave)
  {
    if(atk_db_pending(round.id, round.wave, roster, sizeof(roster)) ==
           SUCCESS && roster[0] != '\0')
      snprintf(line, sizeof(line),
          "✚ You have spent your turn this wave. Still standing idle: "
          CLR_CYAN "%s" CLR_RESET ".", roster);

    else
      snprintf(line, sizeof(line), "✚ You have spent your turn this wave.");

    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, line);
    return;
  }

  // Neither of these spends the wave: a turn thrown away on a full
  // combatant, or on a corpse, is a misfire and not a move.
  if(tgt.hp <= 0)
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "☠ That one is past mending. The pit does not give "
                   "anyone back.");
    return;
  }

  if(tgt.hp >= tgt.hp_max)
  {
    pthread_mutex_unlock(&atk_turn_lock);
    snprintf(line, sizeof(line),
        "✚ " CLR_GREEN "%s" CLR_RESET " is whole. Save it.", nick);
    cmd_reply(ctx, line);
    return;
  }

  // ---- the roll: engine first, class second, exactly as a blow ------- //
  //
  // Which band a heal lands in is the ENGINE's roll and not the healer's,
  // so a sheet with thirty major lines mends no harder than one with a
  // single line — it merely repeats itself less. atk_tunables_load() has
  // already lifted each ceiling to at least its floor, so the width can
  // never be computed from an inverted band.

  major = (util_rand(100) < (int)t.heal_major_pct);
  amt   = (int32_t)(major ? t.heal_major_min : t.heal_minor_min)
      + (int32_t)util_rand((int)((major ? t.heal_major_max - t.heal_major_min
                                        : t.heal_minor_max - t.heal_minor_min)
                                 + 1));

  // What the health bar will actually move. The roll is what SQL adds
  // before its own clamp; THIS is what the tallies take and what the room
  // is told, because a line saying 18 over a bar that moved 4 is a lie
  // the round card contradicts on the very next `show attack`.
  delta = (amt < tgt.hp_max - tgt.hp) ? amt : tgt.hp_max - tgt.hp;

  spoken = (atk_class_move(src_class, ATK_SEC_HEAL,
        major ? ATK_FLAV_MAJOR : ATK_FLAV_MINOR, 0, &move) == SUCCESS);

  if(!spoken)
  {
    // Unreachable by the loader's all-or-nothing `[heal]` gate, and
    // handled anyway: a neutral sentence beats a silent turn.
    memset(&move, 0, sizeof(move));
    clam(CLAM_WARN, ATK_CTX, "class '%s' had no %s heal move", src_class,
        major ? "major" : "minor");
  }

  heal = (atk_heal_t){
    .round_id = round.id,
    .ns_id    = ns->id,
    .src_user = ctx->username,
    .src_nick = src_nick,
    .tgt_user = tgt_user,
    .tgt_nick = nick,
    .amt      = amt,
    .delta    = delta,
    .wave     = round.wave,
  };

  if(atk_db_heal_apply(&heal) != SUCCESS)
  {
    pthread_mutex_unlock(&atk_turn_lock);
    cmd_reply(ctx, "☠ The mending held nowhere — the pit's ledger "
                   "refused it.");
    return;
  }

  // Announced under the lock, so two turns cannot interleave their
  // narration.
  atk_render_heal(line, sizeof(line), src_nick, nick,
      spoken ? &move : NULL, delta, 0, tgt.hp + delta, tgt.hp_max);
  cmd_reply(ctx, line);

  pthread_mutex_unlock(&atk_turn_lock);

  clam(CLAM_INFO, ATK_CTX, "round %" PRId64 ": %s mended %s for %d",
      round.id, ctx->username, tgt_user, delta);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

// Re-read every character sheet without a restart. A rejected sheet is
// named rather than counted: a sheet that failed to load is a class
// nobody can be dealt, and the author deserves to hear which one.
static void
atk_cmd_reload(const cmd_ctx_t *ctx)
{
  atk_tunables_t    t;
  atk_load_report_t rep;
  char              line[ATK_LINE_SZ];
  uint32_t          i;

  atk_tunables_load(&t);
  atk_class_load(&rep);

  snprintf(line, sizeof(line),
      "🎭 %" PRIu32 " class%s loaded, %" PRIu32 " rejected (%s)",
      rep.accepted, (rep.accepted == 1) ? "" : "es", rep.rejected,
      t.classes_path);
  cmd_reply(ctx, line);

  for(i = 0; i < rep.n_bad; i++)
  {
    snprintf(line, sizeof(line),
        "  rejected: %s — see the log for the line and the reason",
        rep.bad[i]);
    cmd_reply(ctx, line);
  }
}

// CMD_ARG_NONE, not CMD_ARG_ALNUM: an IRC nickname legally contains
// [ ] \ ` _ ^ { | } and '-'.
static const cmd_arg_desc_t atk_attack_args[] = {
  { "nick", CMD_ARG_NONE, CMD_ARG_REQUIRED, ATK_NICK_SZ - 1, NULL },
};

// Optional, and CMD_ARG_NONE for the same reason as the blade's: with no
// argument at all you mend yourself, which is the common case.
static const cmd_arg_desc_t atk_heal_args[] = {
  { "nick", CMD_ARG_NONE, CMD_ARG_OPTIONAL, ATK_NICK_SZ - 1, NULL },
};

bool
atk_commands_register(void)
{
  if(cmd_register("attack", "attack",
        "attack <nick>|--end",
        "Attack another combatant in the pit.",
        "Every combatant enters with full health. Damage is rolled the "
        "same way for everyone, and a blow in the top band counts as a "
        "critical hit; the first to reach 0 hit points ends the round "
        "and, where the protocol allows it, leaves the channel feet "
        "first. You may strike once per wave — once every living "
        "combatant has swung, the wave turns and anyone may go again. "
        "Targets must be registered users who are present in the room. "
        "A fight also has a life of its own and expires on its own "
        "clock; `attack --end` stops one early, and anyone who can "
        "attack can end it.",
        USERNS_GROUP_USER, 0, CMD_SCOPE_PUBLIC, METHOD_T_ANY,
        atk_cmd_attack, NULL, NULL, NULL,
        atk_attack_args,
        (uint8_t)(sizeof(atk_attack_args) / sizeof(atk_attack_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  // A child, not an argument — and the trade is deliberate. Children
  // resolve before the root's argument (core/cmd.c resolve_subcmd_locked),
  // so a user nicknamed `reload` cannot be attacked. Accepted: `reload`
  // is admin-only and rare, while `!heal` and `!defer` are frequent and
  // player-facing and therefore earn roots of their own. `--end` has no
  // such problem: an IRC nickname can never begin with '-'.
  if(cmd_register("attack", "reload",
        "attack reload",
        "Re-read the character sheets from disk.",
        "Re-scans `plugin.attack.classes_path`, re-parses every sheet in "
        "it, and swaps the whole registry at once — a sheet that fails "
        "validation leaves the previously loaded set untouched for every "
        "other class. Replies with the tally and names each rejected "
        "file; the log carries the line number and the reason. Note that "
        "a combatant nicknamed `reload` cannot be attacked, because this "
        "child resolves before the root command's argument.",
        USERNS_GROUP_ADMIN, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        atk_cmd_reload, NULL, "attack", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // A ROOT, not a child of `attack` — the opposite trade to `reload`
  // above, and for the opposite reason. Children resolve before the
  // root's argument, so `attack heal` as a child would make a combatant
  // nicknamed `heal` unattackable; the verb is frequent and
  // player-facing, so it earns a name of its own instead.
  if(cmd_register("attack", "heal",
        "heal [nick]",
        "Mend a combatant instead of striking one.",
        "Only a combatant whose character class knows healing may heal — "
        "`show attack classes` says which do. Healing spends your turn "
        "for the wave exactly as an attack does, so it is a trade and "
        "never a free action, and it is the one thing a class can do that "
        "another cannot. With no argument you mend yourself, which is "
        "allowed and is the common case. How much is restored is rolled "
        "by the pit, not by your class: a minor mend restores "
        "`plugin.attack.heal.minor_min`..`minor_max` hit points and a "
        "major one `major_min`..`major_max`. Nobody can be healed past "
        "the health they started with, and the number announced is always "
        "the number the health bar moved.",
        USERNS_GROUP_USER, 0, CMD_SCOPE_PUBLIC, METHOD_T_ANY,
        atk_cmd_heal, NULL, NULL, NULL,
        atk_heal_args,
        (uint8_t)(sizeof(atk_heal_args) / sizeof(atk_heal_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  // The read-only views hang off the core `show` parent, not off this
  // command; attack_show.c owns them.
  if(atk_show_register() != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Three paths cover every definition this plugin owns: the `attack` root
// with its `reload` child, the `heal` root, and the `show attack` card
// with its two children. Each unregisters depth-first, so no parent is
// left holding a freed child. Core would reclaim these anyway — saying so
// ourselves is what keeps the unload audit reading `deinit() complete`
// instead of naming us as the plugin that had to be tidied up after.
//
// A definition added anywhere in this plugin must fall under one of these
// paths or earn a call of its own here.
void
atk_commands_unregister(void)
{
  cmd_unregister_path("attack");
  cmd_unregister_path("heal");
  cmd_unregister_path("show/attack");
}
