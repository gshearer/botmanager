// botmanager — MIT
// melee command surface: `!melee <nick>`, the turn engine that resolves
// one blow. Registered users only, group chat only — both enforced by
// the command system's per-leaf gate, never re-checked here.
//
// The database is authoritative for round state, and one plugin-global
// mutex serialises the whole read-modify-write of a turn so two blows
// arriving on different worker threads cannot both decrement the same
// combatant's health.

#define MELEE_INTERNAL
#include "melee.h"

#include "bot.h"
#include "colors.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// Serialises steps 5-10 of a turn: load the round, enrol, gate the wave,
// roll, write, announce. Target resolution and the presence probe run
// *outside* it — they mutate nothing and would only widen the window
// while holding a lock the method drivers know nothing about.
static pthread_mutex_t melee_turn_lock = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------------ //
// Resolution helpers                                                  //
// ------------------------------------------------------------------ //

typedef struct
{
  const char *nick;
  uint32_t    seen;    // members enumerated at all
  bool        found;
} melee_presence_t;

static void
melee_presence_cb(const char *nick, void *data)
{
  melee_presence_t *p = data;

  p->seen++;

  if(nick != NULL && strcasecmp(nick, p->nick) == 0)
    p->found = true;
}

// Is `nick` in this room? A driver with no member tracking enumerates
// nobody, which must read as "cannot tell", never as "absent".
static bool
melee_target_present(const cmd_ctx_t *ctx, const char *nick)
{
  melee_presence_t p = { .nick = nick, .seen = 0, .found = false };

  method_list_channel(ctx->msg->inst, ctx->msg->channel,
      melee_presence_cb, &p);

  return(p.seen == 0 || p.found);
}

// Map a typed nickname onto a namespace-scoped username: first through
// the bot's authenticated sessions, then by taking the token as a
// username outright. Anything else is a ghost.
static bool
melee_resolve_target(const cmd_ctx_t *ctx, const userns_t *ns,
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
// melee <nick>                                                        //
// ------------------------------------------------------------------ //

static void
melee_cmd_attack(const cmd_ctx_t *ctx)
{
  melee_tunables_t  t;
  melee_round_t     round = { 0 };
  melee_player_t    atk;
  melee_player_t    tgt;
  melee_blow_t      blow;
  userns_t         *ns;
  const char       *nick;
  const char       *method;
  const char       *channel;
  const char       *atk_nick;
  char              tgt_user[MELEE_USER_SZ];
  char              line[MELEE_LINE_SZ];
  char              roster[MELEE_ROSTER_SZ];
  int32_t           dmg;
  int32_t           new_hp;
  bool              crit  = false;
  bool              fatal = false;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)          // the resolver already replied
    return;

  // USERNS_GROUP_USER means dispatch has already rejected anonymous
  // callers; a NULL here would be a gate regression, not user input.
  if(ctx->username == NULL || ctx->username[0] == '\0')
  {
    clam(CLAM_WARN, MELEE_CTX, "attack reached the pit unauthenticated");
    return;
  }

  nick = (ctx->parsed != NULL && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;

  if(nick == NULL || nick[0] == '\0')
  {
    cmd_reply(ctx, "usage: melee <nick>");
    return;
  }

  melee_tunables_load(&t);

  method   = method_inst_name(ctx->msg->inst);
  channel  = ctx->msg->channel;
  atk_nick = (ctx->msg->nickname[0] != '\0')
      ? ctx->msg->nickname : ctx->username;

  if(!melee_resolve_target(ctx, ns, nick, tgt_user, sizeof(tgt_user)))
  {
    snprintf(line, sizeof(line),
        "⚔ " CLR_PURPLE "%s" CLR_RESET
        " is no one — the Underdark keeps no ledger for ghosts.", nick);
    cmd_reply(ctx, line);
    return;
  }

  if(strcasecmp(tgt_user, ctx->username) == 0)
  {
    cmd_reply(ctx, "⚔ Turning a blade on yourself is Vhaeraun's business, "
        "not ours.");
    return;
  }

  if(!melee_target_present(ctx, nick))
  {
    snprintf(line, sizeof(line),
        "⚔ " CLR_PURPLE "%s" CLR_RESET
        " is not in this chamber. The pit takes only the present.", nick);
    cmd_reply(ctx, line);
    return;
  }

  pthread_mutex_lock(&melee_turn_lock);

  // ---- 5. the round ------------------------------------------------ //

  if(melee_db_round_find(ns->id, method, channel, &round) &&
     round.idle > (int64_t)t.round_timeout)
  {
    melee_db_round_abandon(round.id);
    cmd_reply(ctx, "⚔ The old brawl has gone cold; a new one begins.");

    // Clear the whole snapshot, not just the id: the dead round's
    // top_crit would otherwise set the bar for the fresh one.
    memset(&round, 0, sizeof(round));
  }

  if(round.id <= 0)
  {
    round.id   = melee_db_round_open(ns->id, method, channel, ctx->username);
    round.wave = 1;

    if(round.id <= 0)
    {
      pthread_mutex_unlock(&melee_turn_lock);
      cmd_reply(ctx, "☠ The pit will not answer — no round could be opened.");
      return;
    }
  }

  // ---- 6. enrolment ------------------------------------------------ //

  melee_db_player_enrol(round.id, ns->id, ctx->username, atk_nick,
      (int32_t)t.start_hp);
  melee_db_player_enrol(round.id, ns->id, tgt_user, nick,
      (int32_t)t.start_hp);

  if(!melee_db_player_get(round.id, ctx->username, &atk) ||
     !melee_db_player_get(round.id, tgt_user, &tgt))
  {
    pthread_mutex_unlock(&melee_turn_lock);
    cmd_reply(ctx, "☠ The pit will not answer — the roster is unreadable.");
    return;
  }

  // ---- 7. the wave gate -------------------------------------------- //

  if(atk.last_wave >= round.wave)
  {
    if(melee_db_pending(round.id, round.wave, roster, sizeof(roster)) ==
           SUCCESS && roster[0] != '\0')
      snprintf(line, sizeof(line),
          "⚔ You have spent your blow this wave. Still standing idle: "
          CLR_CYAN "%s" CLR_RESET ".", roster);

    else
      snprintf(line, sizeof(line),
          "⚔ You have spent your blow this wave.");

    pthread_mutex_unlock(&melee_turn_lock);
    cmd_reply(ctx, line);
    return;
  }

  if(tgt.hp <= 0)
  {
    pthread_mutex_unlock(&melee_turn_lock);
    cmd_reply(ctx, "☠ That one is already cooling. Find a living quarrel.");
    return;
  }

  // ---- 8-9. roll and write ----------------------------------------- //

  dmg    = melee_roll(&t, &crit);
  new_hp = (tgt.hp > dmg) ? tgt.hp - dmg : 0;
  fatal  = (new_hp == 0);

  blow = (melee_blow_t){
    .round_id = round.id,
    .ns_id    = ns->id,
    .atk_user = ctx->username,
    .atk_nick = atk_nick,
    .tgt_user = tgt_user,
    .tgt_nick = nick,
    .dmg      = dmg,
    .wave     = round.wave,
    .crit     = crit,
    .new_top  = (crit && dmg > round.top_crit),
    .fatal    = fatal,
  };

  if(melee_db_blow_apply(&blow) != SUCCESS)
  {
    pthread_mutex_unlock(&melee_turn_lock);
    cmd_reply(ctx, "☠ The blow landed nowhere — the pit's ledger refused it.");
    return;
  }

  // ---- 10. announce, still under the lock so two turns cannot ------- //
  //          interleave their narration                               //

  melee_render_blow(line, sizeof(line), atk_nick, nick, dmg, crit,
      new_hp, tgt.hp_max);
  cmd_reply(ctx, line);

  if(fatal)
  {
    melee_render_death(line, sizeof(line), atk_nick, nick);
    cmd_reply(ctx, line);
  }

  pthread_mutex_unlock(&melee_turn_lock);

  if(!fatal)
    return;

  // ---- the door ----------------------------------------------------- //
  // Strictly after the death line: on IRC the strongest ejection is a
  // KILL, and nothing sent afterwards would ever reach the fallen.

  {
    method_eject_t force = METHOD_EJECT_NONE;
    char           reason[128];

    if(t.eject_on_death)
    {
      force = method_eject_probe(ctx->msg->inst, channel, nick);

      if(force != METHOD_EJECT_NONE)
      {
        snprintf(reason, sizeof(reason), "slain by %s in the pit", atk_nick);
        method_eject(ctx->msg->inst, channel, nick, force, reason);
      }
    }

    clam(CLAM_INFO, MELEE_CTX, "round %" PRId64 ": %s slew %s (eject=%d)",
        round.id, ctx->username, tgt_user, (int)force);
  }
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

// CMD_ARG_NONE, not CMD_ARG_ALNUM: an IRC nickname legally contains
// [ ] \ ` _ ^ { | } and '-'.
static const cmd_arg_desc_t melee_attack_args[] = {
  { "nick", CMD_ARG_NONE, CMD_ARG_REQUIRED, MELEE_NICK_SZ - 1, NULL },
};

bool
melee_commands_register(void)
{
  if(cmd_register("melee", "melee",
        "melee <nick>",
        "Attack another combatant in the pit.",
        "Every combatant enters with full health. A blow may land "
        "ordinary or critical; the first to reach 0 hit points ends the "
        "round and, where the protocol allows it, leaves the channel "
        "feet first. You may strike once per wave — once every living "
        "combatant has swung, the wave turns and anyone may go again. "
        "Targets must be registered users who are present in the room.",
        USERNS_GROUP_USER, 0, CMD_SCOPE_PUBLIC, METHOD_T_ANY,
        melee_cmd_attack, NULL, NULL, NULL,
        melee_attack_args,
        (uint8_t)(sizeof(melee_attack_args) / sizeof(melee_attack_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  // The read-only views hang off the core `show` parent, not off this
  // command; melee_show.c owns them.
  if(melee_show_register() != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Teardown note: `melee` is a root command with no children yet, but the
// `show melee` tree that MELEE-4 attaches cannot be unregistered — the
// command system has no parent-aware unregister. Keep the whole surface
// in place for symmetry and let cmd_exit() free it at shutdown; reload
// via daemon restart, not hot-unload, exactly as userquote and whenmoon
// do.
void
melee_commands_unregister(void)
{
  clam(CLAM_DEBUG, MELEE_CTX,
      "melee command tree left registered (freed at shutdown)");
}
