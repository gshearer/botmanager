// botmanager — MIT
// attack decay: the one scheduled task that services every affliction in
// every room of every namespace. This is the only part of the pit that
// runs with nobody on the other end of it, which is what makes its two
// disciplines non-negotiable: the death line goes out strictly before
// the eject, and the callback never returns TASK_FATAL.

#define ATTACK_INTERNAL
#include "attack.h"

#include "task.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

// Guards the handle and the idle clock only — never held across a
// database call, a send, or an eject.
static pthread_mutex_t atk_dot_lock       = PTHREAD_MUTEX_INITIALIZER;
static task_handle_t   atk_dot_task       = TASK_HANDLE_NONE;
static time_t          atk_dot_idle_since = 0;

// ------------------------------------------------------------------ //
// One affliction                                                      //
// ------------------------------------------------------------------ //

// The room this row names, or NULL when the method has been removed out
// from under the round. One dead room never aborts the batch.
static method_inst_t *
atk_dot_room(const atk_dot_due_t *d)
{
  method_inst_t *inst = method_find(d->method);

  if(inst == NULL)
    clam(CLAM_DEBUG, ATK_CTX,
        "affliction %" PRId64 ": method '%s' is gone", d->id, d->method);

  return(inst);
}

// Announce, and — only on a death — open the door. The order below is
// the plugin's one law, and it is easier to get wrong here than on the
// turn path because there is no cmd_reply() to hide behind.
static void
atk_dot_speak(const atk_dot_due_t *d, const atk_tunables_t *t,
    const char *line, bool fatal)
{
  method_inst_t *inst = atk_dot_room(d);
  method_eject_t force = METHOD_EJECT_NONE;
  char           reason[128];

  if(inst == NULL)
  {
    // A fatal tick already marked the row spent and closed the round;
    // only a survivable one is still live enough to cancel.
    if(!fatal)
      atk_db_dot_cancel(d->id);

    return;
  }

  method_send(inst, d->channel, line);

  if(!fatal)
    return;

  if(t->eject_on_death)
  {
    force = method_eject_probe(inst, d->channel, d->victim_nick);

    if(force != METHOD_EJECT_NONE)
    {
      snprintf(reason, sizeof(reason), "slain by %s in the pit",
          d->source_nick);
      method_eject(inst, d->channel, d->victim_nick, force, reason);
    }
  }

  clam(CLAM_INFO, ATK_CTX,
      "round %" PRId64 ": %s slew %s by %s (eject=%d)", d->round_id,
      d->source, d->victim, atk_dot_name_of(d->kind), (int)force);
}

// The lock is the turn lock, not a lock of this file's own: a tick
// decrements the same health a blow does, and the two may not race. It
// is released before the line is sent, exactly as the turn engine
// releases it before the eject.
static void
atk_dot_service(const atk_dot_due_t *d, const atk_tunables_t *t)
{
  atk_player_t  victim;
  atk_dot_hit_t hit;
  char          line[ATK_LINE_SZ];
  int32_t       dmg;
  int32_t       new_hp;
  bool          fatal;
  bool          refill = false;

  pthread_mutex_lock(&atk_turn_lock);

  // A victim already cooling took their death from someone else's blade
  // between one tick and the next. The wound is moot.
  if(!atk_db_player_get(d->round_id, d->victim, &victim) || victim.hp <= 0)
  {
    pthread_mutex_unlock(&atk_turn_lock);
    atk_db_dot_cancel(d->id);
    return;
  }

  dmg    = 1 + (int32_t)util_rand((int)t->dot_tick_dmg_max);
  new_hp = (victim.hp > dmg) ? victim.hp - dmg : 0;
  fatal  = (new_hp == 0);

  hit = (atk_dot_hit_t){
    .dot_id    = d->id,
    .round_id  = d->round_id,
    .ns_id     = d->ns_id,
    .victim    = d->victim,
    .source    = d->source,
    .dmg       = dmg,
    .tick_secs = t->dot_tick_secs,
    .last      = d->expired,
    .fatal     = fatal,
  };

  if(atk_db_dot_tick(&hit) != SUCCESS)
  {
    // Say nothing: the ledger refused the tick, so as far as the room is
    // concerned it never happened. The deadline has not moved, so the
    // next iteration tries again.
    pthread_mutex_unlock(&atk_turn_lock);
    return;
  }

  if(fatal)
    atk_render_dot_death(line, sizeof(line), d->source_nick,
        d->victim_nick, d->kind, t, &refill);

  else
    atk_render_dot_tick(line, sizeof(line), d->source_nick, d->victim_nick,
        dmg, new_hp, victim.hp_max, d->kind, t, &refill);

  pthread_mutex_unlock(&atk_turn_lock);

  atk_dot_speak(d, t, line, fatal);

  // Last, with no lock held and the door already closed behind the
  // fallen — the same ordering the turn path uses, for the same reason.
  if(refill)
    atk_llm_refill_kick(fatal ? ATK_FLAV_DOT_DEATH : ATK_FLAV_DOT_TICK,
        t);
}

// ------------------------------------------------------------------ //
// The task                                                            //
// ------------------------------------------------------------------ //

static void
atk_dot_task_cb(task_t *t)
{
  atk_tunables_t tun;
  atk_dot_due_t  rows[ATK_DOT_BATCH];
  const time_t   now = time(NULL);
  uint32_t       n;
  uint32_t       i;
  bool           idle;
  bool           retire = false;

  // First, and on every path below: TASK_FATAL takes the daemon down
  // with it, and a flavour timer may never do that.
  t->state = TASK_ENDED;

  atk_tunables_load(&tun);

  // Afflictions whose round has ended or been abandoned. Without this
  // sweep they would sit live forever and the idle clock below could
  // never start.
  atk_db_dot_sweep();

  n = atk_db_dot_due(rows, ATK_DOT_BATCH);

  for(i = 0; i < n; i++)
    atk_dot_service(&rows[i], &tun);

  if(n == ATK_DOT_BATCH)
    clam(CLAM_DEBUG, ATK_CTX,
        "decay batch full at %d — the rest wait for the next tick",
        ATK_DOT_BATCH);

  // Nothing was DUE, which is not the same as nothing being alive: a
  // long affliction waiting for its first tick must not start the clock.
  idle = (n == 0 && atk_db_dot_live() == 0);

  pthread_mutex_lock(&atk_dot_lock);

  if(!idle)
    atk_dot_idle_since = 0;

  else if(atk_dot_idle_since == 0)
    atk_dot_idle_since = now;

  else if(now - atk_dot_idle_since > (time_t)tun.dot_linger_secs)
  {
    // Cancelling our own handle from inside the callback is exactly the
    // contract in task.h: the flag makes task_finish treat this
    // TASK_ENDED as terminal instead of rescheduling, and the task then
    // disappears from `show tasks` entirely.
    task_cancel(atk_dot_task);
    atk_dot_task       = TASK_HANDLE_NONE;
    atk_dot_idle_since = 0;
    retire               = true;
  }

  pthread_mutex_unlock(&atk_dot_lock);

  if(retire)
    clam(CLAM_INFO, ATK_CTX,
        "no afflictions for %" PRIu32 "s — the decay task leaves the queue",
        tun.dot_linger_secs);
}

void
atk_dot_wake(void)
{
  atk_tunables_t t;
  uint32_t       interval;

  atk_tunables_load(&t);
  interval = t.dot_tick_secs * 1000;

  pthread_mutex_lock(&atk_dot_lock);

  atk_dot_idle_since = 0;

  if(atk_dot_task == TASK_HANDLE_NONE)
  {
    atk_dot_task = task_add_periodic("atk_dot", TASK_ANY, ATK_DOT_PRIO,
        interval, atk_dot_task_cb, NULL);

    if(atk_dot_task == TASK_HANDLE_NONE)
      clam(CLAM_WARN, ATK_CTX,
          "decay task could not be queued — afflictions will not tick");

    else
      clam(CLAM_DEBUG, ATK_CTX, "decay task queued every %" PRIu32 "s",
          t.dot_tick_secs);
  }

  pthread_mutex_unlock(&atk_dot_lock);
}

// task_cancel() does not wait for a callback already running, which is
// the same exposure atk_llm_watch(false) carries: this is the best the
// task API offers, and it closes the window that matters — a periodic
// callback rescheduled into an unloaded .so.
void
atk_dot_stop(void)
{
  pthread_mutex_lock(&atk_dot_lock);

  if(atk_dot_task != TASK_HANDLE_NONE)
  {
    task_cancel(atk_dot_task);
    atk_dot_task = TASK_HANDLE_NONE;
  }

  atk_dot_idle_since = 0;

  pthread_mutex_unlock(&atk_dot_lock);
}
