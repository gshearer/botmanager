// botmanager — MIT
// The voice governor: one budget and one quiet window for every chore.
//
// Without this, chore #3 mints bespoke throttle #3 and a bot ends up
// per-chore polite and aggregately rude — four weather warnings, two
// birthdays and a price alert inside one minute, each of them within
// its own limit. So restraint is one gate, consulted immediately
// before a cue is submitted, and the accounting behind it is shared.
//
// Three classes decide how much say the gate gets (CARE-3 §D8):
//
//   TIMED        the human picked the moment (/remind, /in). Never
//                refused, never budgeted — only recorded.
//   ASKED        the human asked, the bot picked the moment (a price
//                watch firing). Waits out quiet hours; costs nothing.
//   UNSOLICITED  nobody asked for this one (weather, occasions,
//                follow-ups). Waits out quiet hours and pays budget.
//
// The permission and its accounting are the SAME statement: the INSERT
// carries its own budget predicates, so two chores racing for the last
// slot of the hour cannot both win it. That is the soul's own idiom —
// the claim IS the guard (soul_wx_claim, the deferred claim) — applied
// to speech instead of to work.
//
// Quiet hours are server-local. Person-local ones are reachable (city
// fact → geocode → the tz_offset an openweather fetch already carries)
// but cost a per-person resolution cache, so v1 is the server's clock.
//
// ⚠ A chore that does DURABLE work before speaking must ask
// soul_voice_window_open() FIRST and skip before claiming: quiet hours
// mean later, not never, and a claim taken inside the window would
// convert one into the other. The budget is the opposite by design —
// it is spent at the moment of speech, and what it silences stays
// silent.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "db.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#define VOICE_CTX "voice"

// Long enough to answer "was the bot rude last night?", short enough
// that the table never becomes storage. Both budget windows (an hour,
// a day) fit inside it with room to spare. A string because its only
// reader is an INTERVAL literal.
#define VOICE_RETENTION_HOURS               "48"

static const char *
voice_class_name(soul_class_t cls)
{
  switch(cls)
  {
    case SOUL_CLASS_TIMED:  return("timed");
    case SOUL_CLASS_ASKED:  return("asked");
    default:                return("unsolicited");
  }
}

static bool
voice_exec(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok;

  if(res == NULL)
    return(FAIL);

  ok = (db_query(sql, res) == SUCCESS && res->ok) ? SUCCESS : FAIL;

  if(ok != SUCCESS)
    clam(CLAM_WARN, VOICE_CTX, "sql failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

// The descriptor table in chatbot.c is the single source of every
// default here: an instance key is registered with its default at bind
// time, so a bot with no DB row of its own still reads 23 / 8 / true /
// 4 / 2. That is what lets 0 mean zero — no knob in this file carries
// the "0 = the compiled default" idiom, because a budget of none is a
// thing an operator may legitimately want.
//
// ⚠ kv_get_str returns NULL for anything that is not KV_STR, so it is
// no use for probing whether one of these is set. kv_get_uint reads
// UINT32 and BOOL alike and answers 0 for a key nobody registered —
// which for a governor is the safe direction: silence, and a log line
// saying so.
static uint32_t
voice_kv_uint(const char *bot_name, const char *suffix)
{
  char key[KV_KEY_SZ];

  snprintf(key, sizeof(key), "bot.%s.behavior.soul.%s", bot_name, suffix);
  return((uint32_t)kv_get_uint(key));
}

void
soul_voice_ensure_schema(void)
{
  // One row per cue the bot actually spoke: the ledger the budgets are
  // counted from and, incidentally, the record of every unprompted
  // thing it has said. The chat DDL discipline (memory_ensure_schema):
  // owner-run idempotent batches at plugin start(), after
  // dossier_register_config so the dossier(id) FK target exists.
  (void)voice_exec(
      "CREATE TABLE IF NOT EXISTS chat_soul_voice_log ("
      " id         BIGSERIAL    PRIMARY KEY,"
      " ns_id      INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " dossier_id BIGINT       REFERENCES dossier(id) ON DELETE SET NULL,"
      " class      SMALLINT     NOT NULL,"
      " spoken_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
      ")");

  // Both budget counts are (ns, class, recency); the per-person one
  // narrows that by dossier, which the row count at this scale makes
  // free.
  (void)voice_exec(
      "CREATE INDEX IF NOT EXISTS idx_chat_soul_voice_recent"
      " ON chat_soul_voice_log(ns_id, class, spoken_at)");
}

bool
soul_voice_window_open(const char *bot_name, soul_class_t cls, bool severe)
{
  struct tm tm;
  time_t    now = time(NULL);
  uint32_t  start;
  uint32_t  end;
  uint32_t  hour;
  bool      inside;

  // The human picked this moment. The bot does not get an opinion
  // about it, and a reminder set for 3am is a reminder set for 3am.
  if(cls == SOUL_CLASS_TIMED)
    return(true);

  start = voice_kv_uint(bot_name, "quiet.start_hour");
  end   = voice_kv_uint(bot_name, "quiet.end_hour");

  // Equal bounds disable the window by definition, and an out-of-range
  // hour disables it too: a fat-fingered knob must not silence a bot
  // for a day with nothing to show for it but quiet.
  if(start == end || start > 23 || end > 23)
    return(true);

  localtime_r(&now, &tm);
  hour = (uint32_t)tm.tm_hour;

  inside = start < end ? (hour >= start && hour < end)
                       : (hour >= start || hour < end);

  if(!inside)
    return(true);

  // The one thing worth waking someone for, and only while the
  // operator leaves the override on.
  if(severe && voice_kv_uint(bot_name, "quiet.override_severe") != 0)
    return(true);

  return(false);
}

bool
soul_voice_permits(const char *bot_name, uint32_t ns_id, int64_t dossier_id,
    soul_class_t cls, bool severe)
{
  db_result_t *res;
  char         sql[1024];
  char         person[320];
  char         dossier[32];
  uint32_t     hour_cap;
  uint32_t     day_cap;
  bool         ok = false;

  if(!soul_voice_window_open(bot_name, cls, severe))
  {
    clam(CLAM_INFO, VOICE_CTX, "bot=%s %s cue held — quiet hours",
        bot_name, voice_class_name(cls));
    return(false);
  }

  if(dossier_id > 0)
    snprintf(dossier, sizeof(dossier), "%" PRId64, dossier_id);
  else
    snprintf(dossier, sizeof(dossier), "NULL");

  if(cls != SOUL_CLASS_UNSOLICITED)
  {
    // Recorded, not judged: the budgets exist to stop the bot talking
    // to people who did not ask, and this cue is not that.
    snprintf(sql, sizeof(sql),
        "INSERT INTO chat_soul_voice_log (ns_id, dossier_id, class)"
        " VALUES (%" PRIu32 ", %s, %d) RETURNING id",
        ns_id, dossier, (int)cls);

    (void)voice_exec(sql);
    return(true);
  }

  hour_cap = voice_kv_uint(bot_name, "budget.unsolicited_per_hour");
  day_cap  = voice_kv_uint(bot_name, "budget.per_person_per_day");

  // A zero budget is a refusal, not a query: say so without a round
  // trip, and say which of the two knobs did it.
  if(hour_cap == 0 || (dossier_id > 0 && day_cap == 0))
  {
    clam(CLAM_INFO, VOICE_CTX,
        "bot=%s unsolicited cue held — %s budget is zero", bot_name,
        hour_cap == 0 ? "hourly" : "per-person");
    return(false);
  }

  // The gate and its accounting in one statement: a cue that is not
  // permitted writes nothing, and a cue that is permitted has already
  // paid for itself by the time this returns. Two chores racing for
  // the hour's last slot therefore cannot both spend it.
  person[0] = '\0';

  if(dossier_id > 0)
    snprintf(person, sizeof(person),
        " AND (SELECT COUNT(*) FROM chat_soul_voice_log"
        " WHERE ns_id = %" PRIu32 " AND dossier_id = %" PRId64
        " AND class = %d AND spoken_at > NOW() - INTERVAL '1 day') < %" PRIu32,
        ns_id, dossier_id, (int)SOUL_CLASS_UNSOLICITED, day_cap);

  snprintf(sql, sizeof(sql),
      "INSERT INTO chat_soul_voice_log (ns_id, dossier_id, class)"
      " SELECT %" PRIu32 ", %s, %d"
      " WHERE (SELECT COUNT(*) FROM chat_soul_voice_log"
      " WHERE ns_id = %" PRIu32 " AND class = %d"
      " AND spoken_at > NOW() - INTERVAL '1 hour') < %" PRIu32
      "%s RETURNING id",
      ns_id, dossier, (int)SOUL_CLASS_UNSOLICITED,
      ns_id, (int)SOUL_CLASS_UNSOLICITED, hour_cap, person);

  res = db_result_alloc();

  if(res == NULL)
    return(false);

  // A failed query is not a refusal and must not be reported as one —
  // the bot stays quiet either way, but only one of the two is the
  // governor doing its job.
  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, VOICE_CTX, "voice claim failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  else
  {
    ok = res->rows > 0;

    if(!ok)
      clam(CLAM_INFO, VOICE_CTX,
          "bot=%s unsolicited cue held — budget spent (%" PRIu32 "/h,"
          " %" PRIu32 "/person/day)", bot_name, hour_cap, day_cap);
  }

  db_result_free(res);
  return(ok);
}

void
soul_voice_purge(void)
{
  (void)voice_exec("DELETE FROM chat_soul_voice_log"
      " WHERE spoken_at < NOW() - INTERVAL '"
      VOICE_RETENTION_HOURS " hours'");
}
