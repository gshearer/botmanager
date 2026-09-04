// botmanager — MIT
// attack: a bot standing in the pit (featreq #83).
//
// The pit takes only the present and only the registered, and a bot is
// neither a session nor an account — so before any of this it answered
// "is no one the pit has ever heard of" to its own name. There was never
// a rule keeping bots out; there was only no name to find.
//
// This file adds the name and nothing else. Once a bot holds an account
// it enrols, is dealt a sheet, bleeds, heals, dies and scores on exactly
// the paths a person does, and no surface anywhere says which combatant
// is which — a personality may be masquerading as a person and the pit
// is not the place that gives it away (operator ruling, 2026-09-02).

#define ATTACK_INTERNAL
#include "attack.h"

#include "kv.h"
#include "task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// ------------------------------------------------------------------ //
// The per-bot knobs                                                   //
// ------------------------------------------------------------------ //

// Stable per-plugin address; register and unregister match on it.
static const char atk_bot_kv_cookie;

// Fires once per bot instance, and again for every existing bot when
// this plugin reloads, so the keys re-attach themselves. Self-contained
// by contract (include/bot.h): kv_register and nothing that could take
// the bot lock back.
static void
atk_bot_kv_cb(const char *botname, void *user)
{
  char key[KV_KEY_SZ];

  (void)user;

  // Off is the whole default. A bot nobody has let into the pit is
  // unaddressable there, which is the behaviour every bot had before
  // this existed and the one the operator asked to keep.
  snprintf(key, sizeof(key), "bot.%s." ATK_BKV_ENABLED, botname);
  kv_register(key, KV_UINT8, "0", NULL, NULL,
      "Let this bot be attacked and fight back in the pit");

  snprintf(key, sizeof(key), "bot.%s." ATK_BKV_IN_VOICE, botname);
  kv_register(key, KV_UINT8, "1", NULL, NULL,
      "Speak this bot's own turns in its persona rather than in the "
      "voice of the character sheet the pit dealt it");

  snprintf(key, sizeof(key), "bot.%s." ATK_BKV_TURN_DELAY, botname);
  kv_register(key, KV_UINT32, "20", NULL, NULL,
      "Seconds a bot waits before answering a wave; 0 makes it swing "
      "the instant the pit turns, which reads as a machine");
}

void
atk_bot_kv_register(void)
{
  bot_kv_contributor_register(atk_bot_kv_cb, NULL,
      (void *)&atk_bot_kv_cookie);
}

void
atk_bot_kv_unregister(void)
{
  bot_kv_contributor_unregister((void *)&atk_bot_kv_cookie);
}

bool
atk_bot_enabled(const char *botname)
{
  if(botname == NULL || botname[0] == '\0')
    return(false);

  return(kv_get_bot_uint(botname, ATK_BKV_ENABLED) != 0);
}

// ------------------------------------------------------------------ //
// Which bot is that?                                                  //
// ------------------------------------------------------------------ //

// One nick, carried through bot_iterate() because there is no lookup by
// nickname: the registry is keyed by instance name and the nicks live in
// KV beside it.
typedef struct
{
  const char *nick;
  const char *kind;      // method kind, for bot.<n>.<kind>.nick
  char        name[ATK_BOT_SZ];
  bool        found;
} atk_bot_match_t;

// The three nicks a method is configured with, plus the instance name.
// A bot that collided on connect is wearing nick2 or nick3 and is still
// the one standing in the room, so all three answer.
static bool
atk_bot_answers_to(const char *botname, const char *kind, const char *nick)
{
  static const char *const suffix[] = { "nick", "nick2", "nick3" };
  size_t                   i;

  if(strcasecmp(botname, nick) == 0)
    return(true);

  if(kind == NULL || kind[0] == '\0')
    return(false);

  for(i = 0; i < sizeof(suffix) / sizeof(suffix[0]); i++)
  {
    const char *configured = kv_get_bot_method_str(botname, kind, suffix[i]);

    if(configured != NULL && configured[0] != '\0' &&
        strcasecmp(configured, nick) == 0)
      return(true);
  }

  return(false);
}

static void
atk_bot_match_cb(const char *name, const char *method_kinds,
    bot_state_t state, uint32_t method_count, const char *userns_name,
    uint64_t cmd_count, time_t last_activity, void *data)
{
  atk_bot_match_t *m = data;

  (void)method_kinds;
  (void)state;
  (void)method_count;
  (void)userns_name;
  (void)cmd_count;
  (void)last_activity;

  // First match wins and the rest of the walk is a formality: bot names
  // are unique and two bots wearing one nick in one room cannot happen.
  if(m->found || !atk_bot_answers_to(name, m->kind, m->nick))
    return;

  // The knob is read here rather than by the caller so that a disabled
  // bot is not merely refused but genuinely unknown — it must fall
  // through to the human resolvers exactly as any other stranger does,
  // even once a previous enablement has left an account behind.
  if(!atk_bot_enabled(name))
    return;

  strlcpy(m->name, name, sizeof m->name);
  m->found = true;
}

bool
atk_bot_resolve(method_inst_t *inst, const char *nick, char *out, size_t cap)
{
  atk_bot_match_t m = { 0 };

  if(nick == NULL || nick[0] == '\0' || out == NULL || cap == 0)
    return(false);

  m.nick = nick;
  m.kind = (inst != NULL) ? method_inst_kind(inst) : NULL;

  bot_iterate(atk_bot_match_cb, &m);

  if(!m.found)
    return(false);

  strlcpy(out, m.name, cap);
  return(true);
}

// ------------------------------------------------------------------ //
// The account it fights under                                         //
// ------------------------------------------------------------------ //

// Created without a password, because a combatant that never
// authenticates has no use for one, and created lazily, because an
// account is owed only to a bot somebody has actually swung at. From
// here on userns_user_lookup_ci() finds it and the bot needs no
// resolver of its own — including in <p>_scores, which keys the
// lifetime record on (ns_id, username) like everybody else's.
bool
atk_bot_account(const userns_t *ns, const char *botname, char *out, size_t cap)
{
  if(ns == NULL || botname == NULL || botname[0] == '\0')
    return(false);

  // The account name is the instance name; a bot whose name will not fit
  // a username column cannot fight, and says so once rather than
  // silently fighting under a truncation that collides with a person.
  if(strlen(botname) >= ATK_USER_SZ)
  {
    clam(CLAM_WARN, ATK_CTX,
        "bot '%s' cannot hold a pit account: name exceeds %d bytes",
        botname, ATK_USER_SZ - 1);
    return(false);
  }

  // SUCCESS is false in this tree (common.h), so the create is compared
  // against it rather than negated: `!create()` reads as "it failed" and
  // means the exact opposite.
  if(!userns_user_exists(ns, botname) &&
      userns_user_create_nopass(ns, botname) != SUCCESS)
  {
    clam(CLAM_WARN, ATK_CTX, "bot '%s': pit account could not be created",
        botname);
    return(false);
  }

  strlcpy(out, botname, cap);
  return(true);
}

// ------------------------------------------------------------------ //
// What a combatant remembers                                          //
// ------------------------------------------------------------------ //

// A bot that fought through a command has no record of having fought:
// the pit answers over cmd_reply, which never reaches the bot's own
// message path, so hedgehogg was killed in #cabal and then asked "gg"
// about a match it had never heard of (board #86).
//
// Only the death is kept. It is the one thing about a brawl anybody
// still knows an hour later, and it is what the pit's own ledger keeps
// too — a round ended with `--end` records no slayer, no kill and no
// death, and this agrees with it rather than inventing a memory the
// scoreboard does not have.
void
atk_bot_remember_death(const char *method_kind, const char *channel,
    const char *slayer_bot, const char *slayer_nick,
    const char *fallen_bot, const char *fallen_nick)
{
  char line[ATK_LINE_SZ];

  // Written in the third person on purpose: it is filed as something
  // the bot WITNESSED, not as something it said, so it reads back the
  // way anything else it saw happen does.
  if(slayer_bot != NULL && slayer_bot[0] != '\0')
  {
    snprintf(line, sizeof(line), "%s killed %s in the duelling pit.",
        slayer_nick, fallen_nick);
    bot_remember_event(slayer_bot, method_kind, channel, line);
  }

  // Both, when a bot kills a bot: each remembers its own half, and
  // neither is told about the other's.
  if(fallen_bot != NULL && fallen_bot[0] != '\0')
  {
    snprintf(line, sizeof(line), "%s was killed by %s in the duelling pit.",
        fallen_nick, slayer_nick);
    bot_remember_event(fallen_bot, method_kind, channel, line);
  }
}

// ------------------------------------------------------------------ //
// What a combatant knows while it is still standing                   //
// ------------------------------------------------------------------ //

// Stable per-plugin address, like the KV cookie above.
static const char atk_bot_situation_cookie;

// The death is all a bot REMEMBERS (above), and that is right: a
// blow-by-blow in a retrieval corpus drowns it. But a bot mid-brawl
// still has to be able to answer *"why did you hit me"* while the brawl
// is happening, and it could not — the pit's own lines never reach its
// message path, and chat hands a `!attack` line to the command half
// without logging it, so a bot fought a whole round in #cabal and met
// the next question with *"I have no record of the fight"* (board #88).
//
// So the state is PULLED, once, at the moment a prompt is built, and
// nothing is stored: what is true of a fight is true for minutes.
//
// ⚠ Three indexed queries, on the thread assembling the prompt. The
// knob is asked first and answers for every bot in the tree that is not
// in the pit, which is all of them nearly all of the time.
static size_t
atk_bot_situation(const char *botname, const char *method_name,
    const char *channel, char *out, size_t out_sz, void *user)
{
  atk_card_row_t  rows[ATK_MAX_PLAYERS];
  atk_dot_mark_t  marks[ATK_MAX_PLAYERS];
  atk_card_t      card;
  atk_tunables_t  tun;
  userns_t       *ns;
  bot_inst_t     *inst;
  char            others[ATK_ROSTER_SZ];
  char            ails[ATK_TALLY_SZ];
  size_t          n_others = 0;
  size_t          n_ails   = 0;
  size_t          pos;
  uint32_t        shown;
  uint32_t        n_marks;
  uint32_t        self     = ATK_MAX_PLAYERS;

  (void)user;

  // A direct message names no room, and atk_db_card_find widens to the
  // whole namespace when the channel is empty — which would tell a bot
  // in a DM about a brawl it is not in.
  if(channel == NULL || channel[0] == '\0')
    return(0);

  if(!atk_bot_enabled(botname))
    return(0);

  inst = bot_find(botname);

  if(inst == NULL)
    return(0);

  ns = bot_get_userns(inst);

  if(ns == NULL)
    return(0);

  if(!atk_db_card_find(ns->id, method_name, channel, &card))
    return(0);

  atk_tunables_load(&tun);

  // Same comparison, in the same direction, as every other reader: a
  // round nobody has swung in for longer than the idle ceiling is over,
  // whatever the row still says. Telling a bot it is fighting in a fight
  // that finished is the mirror of the bug this fixes.
  if(card.state != ATK_ROUND_ACTIVE
      || card.idle > (int64_t)tun.round_max_idle_secs)
    return(0);

  shown   = atk_db_card_roster(card.id, rows, ATK_MAX_PLAYERS, NULL);
  n_marks = atk_db_dot_marks(card.id, marks, ATK_MAX_PLAYERS);

  for(uint32_t i = 0; i < shown; i++)
    if(strcasecmp(rows[i].user, botname) == 0)
    {
      self = i;
      break;
    }

  // On the card but not in it: the roster is the round's, and a bot may
  // be watching a fight between two other people in its own channel.
  if(self == ATK_MAX_PLAYERS)
    return(0);

  others[0] = '\0';

  // A roster longer than the buffer simply stops: the sentence is a
  // situational nudge, not a scoreboard, and `show attack` is where a
  // complete one lives.
  for(uint32_t i = 0; i < shown && n_others + 1 < sizeof(others); i++)
  {
    int wrote;

    if(i == self || rows[i].hp <= 0)
      continue;

    wrote = snprintf(others + n_others, sizeof(others) - n_others,
        "%s%s (%s, %d of %d)", (n_others > 0) ? ", " : "",
        rows[i].name, rows[i].class, rows[i].hp, rows[i].hp_max);

    if(wrote < 0 || (size_t)wrote >= sizeof(others) - n_others)
    {
      // Truncated mid-combatant. Drop the partial rather than name
      // somebody at half a hitpoint total.
      others[n_others] = '\0';
      break;
    }

    n_others += (size_t)wrote;
  }

  ails[0] = '\0';

  for(uint32_t i = 0; i < n_marks; i++)
  {
    if(strcasecmp(marks[i].victim, botname) != 0)
      continue;

    for(uint32_t k = 0; k < marks[i].n && n_ails + 1 < sizeof(ails); k++)
    {
      int wrote = snprintf(ails + n_ails, sizeof(ails) - n_ails,
          "%s%s", (n_ails > 0) ? ", " : "",
          atk_fallback_noun(marks[i].kinds[k]));

      if(wrote < 0 || (size_t)wrote >= sizeof(ails) - n_ails)
      {
        ails[n_ails] = '\0';
        break;
      }

      n_ails += (size_t)wrote;
    }

    break;
  }

  // One sentence, in the second person, describing only what the bot
  // can see for itself. No numbers a combatant does not have — the
  // engine's rolls, the tunables, the class registry — and no
  // instruction on how to fight: the turn is taken for it by the task
  // above, and a persona that starts typing !attack into a room is a
  // regression, not a feature.
  snprintf(out, out_sz,
      "You are in a fight in %s right now (wave %d). You are a %s"
      " on %d of %d hitpoints%s%s, you have dealt %d and taken %d,"
      " and still standing against you: %s.",
      channel, card.wave, rows[self].class, rows[self].hp,
      rows[self].hp_max,
      (ails[0] != '\0') ? ", suffering " : "",
      (ails[0] != '\0') ? ails : "",
      rows[self].dmg_given, rows[self].dmg_taken,
      (others[0] != '\0') ? others : "nobody");

  // Ask the BUFFER how much of that landed, never snprintf: it reports
  // what it WANTED to write, and past a truncated sentence the chatter
  // would be spliced beyond its own NUL where nothing ever reads it.
  // The same discipline bot_situation_collect applies to this function.
  pos = strnlen(out, out_sz);

  // Then what the room has actually SEEN happen, so a combatant can
  // trade banter about the fight instead of only reciting its own sheet
  // (board #90). It comes after the standing sentence on purpose: that
  // sentence is what is true, this is only what was said about it, and
  // a buffer that runs short should shed the chatter first.
  //
  // The newline goes down first and is taken back if the pit has
  // nothing to say, so a quiet round is not one dangling separator.
  if(pos + 1 < out_sz)
  {
    out[pos] = '\n';

    if(atk_chatter_recent(card.id, out + pos + 1, out_sz - pos - 1) == 0)
      out[pos] = '\0';

    else
      pos = strnlen(out, out_sz);
  }

  return(pos);
}

void
atk_bot_situation_register(void)
{
  bot_situation_register(atk_bot_situation,
      (void *)&atk_bot_situation_cookie);
}

void
atk_bot_situation_unregister(void)
{
  bot_situation_unregister((void *)&atk_bot_situation_cookie);
}

// ------------------------------------------------------------------ //
// The turn                                                            //
// ------------------------------------------------------------------ //

// Guards the handle and the idle clock only — never held across a
// database call, a turn, or a send.
static pthread_mutex_t atk_bot_lock       = PTHREAD_MUTEX_INITIALIZER;
static task_handle_t   atk_bot_task       = TASK_HANDLE_NONE;
static time_t          atk_bot_idle_since = 0;

// Who this bot swings at. Uniform over everybody else left standing AND
// still in the room.
//
// The presence probe is the load-bearing half. A person who swings at
// somebody who has left reads the refusal and picks again; a bot would
// come back to the same absent name on every tick and say so out loud
// each time, which is a flood rather than a game. So the candidates are
// filtered before one is chosen, and the refusal path stays what it is
// for a person: a thing that happens once.
//
// It is deliberately not "whoever hit me last": nothing records that
// today, and the column it would need is about to be answered better —
// ATK-BOT-3 hands the choice to the mind, which can be told who just
// struck it in the same breath as everything else it is told. A
// heuristic added here would be a column written on three damage paths
// to serve one tick of one chunk.
static bool
atk_bot_pick_target(const cmd_ctx_t *ctx, const atk_bot_due_t *d,
    char *out, size_t cap)
{
  char     living[ATK_MAX_PLAYERS][ATK_NICK_SZ];
  uint32_t present[ATK_MAX_PLAYERS];
  uint32_t n;
  uint32_t np = 0;
  uint32_t i;

  n = atk_db_bot_opponents(d->round_id, d->user, living, ATK_MAX_PLAYERS);

  for(i = 0; i < n; i++)
    if(atk_target_present(ctx, living[i]))
      present[np++] = i;

  // Nobody to swing at. Not an error and not a stall: the wave this bot
  // is holding open has nobody waiting behind it, and passing here would
  // spin the round's wave counter for as long as the room stayed empty.
  if(np == 0)
    return(false);

  strlcpy(out, living[present[(uint32_t)rand() % np]], cap);
  return(true);
}

// One bot's turn, taken through the ordinary turn path.
//
// The synthetic context is the mechanism and it is worth reading once:
// cmd_reply() needs a method name and a target, bot_persona_reply()
// needs the bot, and atk_cmd_attack() needs a parsed first argument.
// Nothing else in the pit ever asks the context a question, so those
// four fields are a whole turn's worth of caller.
//
// `reply_route` and `reply_sink_id` stay empty: no session asked for
// this and nobody is capturing it, so the line goes to the room, which
// is where a brawl belongs.
static void
atk_bot_take_turn(const atk_bot_due_t *d)
{
  method_inst_t *inst;
  method_msg_t   msg;
  cmd_args_t     args;
  cmd_ctx_t      ctx;
  atk_player_t   after;
  char           target[ATK_NICK_SZ];

  // The room may have gone since the row was written — a reloaded method
  // plugin, a bot stopped mid-brawl. One missing room never aborts the
  // batch, exactly as it never aborts a decay tick.
  inst = method_find(d->method);

  if(inst == NULL)
  {
    clam(CLAM_DEBUG, ATK_CTX, "round %" PRId64 ": method '%s' is gone",
        d->round_id, d->method);
    return;
  }

  memset(&msg,  0, sizeof(msg));
  memset(&args, 0, sizeof(args));
  memset(&ctx,  0, sizeof(ctx));

  strlcpy(msg.inst_name, d->method,  sizeof msg.inst_name);
  strlcpy(msg.channel,   d->channel, sizeof msg.channel);
  strlcpy(msg.sender,    d->nick,    sizeof msg.sender);
  strlcpy(msg.nickname,  d->nick,    sizeof msg.nickname);
  msg.inst = inst;

  ctx.bot      = bot_find(d->bot);
  ctx.msg      = &msg;
  ctx.username = d->user;
  ctx.parsed   = &args;
  ctx.name     = "attack";

  // The context is built before the target is chosen because choosing
  // one needs the room, and the room is on the context.
  if(!atk_bot_pick_target(&ctx, d, target, sizeof(target)))
  {
    clam(CLAM_DEBUG, ATK_CTX,
        "round %" PRId64 ": %s has nobody present to swing at",
        d->round_id, d->nick);
    method_release(inst);
    return;
  }

  args.argv[0] = target;
  args.argc    = 1;

  clam(CLAM_DEBUG, ATK_CTX, "round %" PRId64 ": %s swings at %s",
      d->round_id, d->nick, target);

  atk_turn_take(&ctx);

  // Did the turn actually happen? Every refusal on that path leaves the
  // wave unspent, and a bot cannot read a refusal and try something
  // else: left alone it would return to this same tick forever and hold
  // the wave open against everybody. Presence is filtered above, so what
  // is left here is transient — a target who died or left between the
  // query and the swing, a wave that turned underneath us, a ledger that
  // refused. The bot's turn is over either way, and saying so is the
  // participant/non-participant rule the operator set: declining to act
  // is still having acted.
  if(atk_db_player_get(d->round_id, d->user, &after) &&
      after.hp > 0 && after.last_wave < d->wave)
  {
    clam(CLAM_INFO, ATK_CTX,
        "round %" PRId64 " wave %d: %s could not swing — passing",
        d->round_id, d->wave, d->nick);

    atk_db_turn_pass(d->round_id, d->user, d->wave);
  }

  method_release(inst);
}

// ------------------------------------------------------------------ //
// The task                                                            //
// ------------------------------------------------------------------ //

static void
atk_bot_task_cb(task_t *t)
{
  atk_tunables_t tun;
  atk_bot_due_t  rows[ATK_BOT_BATCH];
  const time_t   now = time(NULL);
  uint32_t       n;
  uint32_t       i;
  uint32_t       taken = 0;
  bool           idle;
  bool           retire = false;

  // First, and on every path below: TASK_FATAL takes the daemon down
  // with it, and a game may never do that.
  t->state = TASK_ENDED;

  atk_tunables_load(&tun);

  n = atk_db_bot_due(rows, ATK_BOT_BATCH);

  for(i = 0; i < n; i++)
  {
    const uint64_t delay = kv_get_bot_uint(rows[i].bot, ATK_BKV_TURN_DELAY);

    // A round that has gone quiet for longer than the pit allows is over
    // however its state column reads — nothing reaps a round on a timer
    // (AGENTS.md §The two views). Swinging into one would resurrect a
    // brawl that ended hours ago, in front of people who have left.
    if(rows[i].idle > (int64_t)tun.round_max_idle_secs)
      continue;

    // Not yet. The pause is measured from the last thing that happened
    // in the pit, so a bot answers a beat after the blow rather than in
    // the same second as it — which is the difference between a
    // combatant and a reflex.
    if(rows[i].idle < (int64_t)delay)
      continue;

    atk_bot_take_turn(&rows[i]);
    taken++;
  }

  // Nothing was DUE, which is not the same as nothing being alive: a bot
  // waiting out its turn delay is neither, and must not start the clock.
  idle = (taken == 0 && atk_db_bot_live() == 0);

  pthread_mutex_lock(&atk_bot_lock);

  if(!idle)
    atk_bot_idle_since = 0;

  else if(atk_bot_idle_since == 0)
    atk_bot_idle_since = now;

  else if(now - atk_bot_idle_since > (time_t)tun.bot_linger_secs)
  {
    // Cancelling our own handle from inside the callback is the contract
    // in task.h: the flag makes task_finish treat this TASK_ENDED as
    // terminal rather than rescheduling it.
    task_cancel(atk_bot_task);
    atk_bot_task       = TASK_HANDLE_NONE;
    atk_bot_idle_since = 0;
    retire             = true;
  }

  pthread_mutex_unlock(&atk_bot_lock);

  if(retire)
    clam(CLAM_INFO, ATK_CTX,
        "no bot has stood in the pit for %" PRIu32 "s — the turn task "
        "leaves the queue", tun.bot_linger_secs);
}

void
atk_bot_wake(void)
{
  atk_tunables_t t;

  atk_tunables_load(&t);

  pthread_mutex_lock(&atk_bot_lock);

  atk_bot_idle_since = 0;

  if(atk_bot_task == TASK_HANDLE_NONE)
  {
    atk_bot_task = task_add_periodic("atk_bot", TASK_ANY, ATK_BOT_PRIO,
        t.bot_tick_secs * 1000, atk_bot_task_cb, NULL);

    if(atk_bot_task == TASK_HANDLE_NONE)
      clam(CLAM_WARN, ATK_CTX,
          "turn task could not be queued — a bot in the pit will hold "
          "the wave open");

    else
      clam(CLAM_DEBUG, ATK_CTX, "turn task queued every %" PRIu32 "s",
          t.bot_tick_secs);
  }

  pthread_mutex_unlock(&atk_bot_lock);
}

void
atk_bot_resume(void)
{
  uint32_t live = atk_db_bot_live();

  if(live == 0)
    return;

  clam(CLAM_INFO, ATK_CTX,
      "%" PRIu32 " bot(s) still standing in the pit — the turn task "
      "resumes", live);

  atk_bot_wake();
}

// task_cancel() does not wait for a callback already running. That is
// the best the task API offers, and it closes the window that matters —
// a periodic rescheduled into an unloaded mapping.
void
atk_bot_stop(void)
{
  pthread_mutex_lock(&atk_bot_lock);

  if(atk_bot_task != TASK_HANDLE_NONE)
  {
    task_cancel(atk_bot_task);
    atk_bot_task = TASK_HANDLE_NONE;
  }

  atk_bot_idle_since = 0;

  pthread_mutex_unlock(&atk_bot_lock);
}
