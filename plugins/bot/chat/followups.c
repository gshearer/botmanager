// botmanager — MIT
// Follow-ups: asking how the thing somebody told you about went.
//
// "How do you know?" — *you said you had it coming up.* That is the
// whole authorization story (CHATBOT.md §The initiative rule): the only
// input is an `upcoming_event:*` fact the extractor wrote from
// something a person said about their own plans, and the question goes
// back to the venue they said it in. Nothing here is derived,
// aggregated or inferred — least of all how they might feel about it.
// If they told the room the interview had them stressed, that is a
// stated fact and the reply pipeline already has it; this chore never
// asks the model to guess at a mood.
//
// occasions.c's shape, one date family over:
//
//   scan the namespace for a fact family
//     → claim it once per (subject, occasion)
//       → write a presence row and let the spine do the rest
//
// Nothing in this file speaks. Delivery belongs to CARE-1/2/3: presence
// unlocks the question when the subject next turns up, the stored tuple
// routes it to the venue they made the plan in, and the voice governor
// decides whether now is a decent hour.
//
// ⭑ The loop closes itself, and the absence of code for it is the
// feature. When the subject answers *"brutal, I finished in 4:12"*, the
// ordinary extract sweep reads that line like any other and files
// whatever facts it holds. A chore that asks a question does not get to
// own the answer.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "db.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FOLLOWUPS_CTX "followups"

// The steered fact family, and the only thing this chore reads. The
// slug after the colon names the happening; the value is its date
// (FACT_EXTRACT.md §Steered fact families).
#define FOLLOWUPS_KEY_PREFIX      "upcoming_event:"

// Events examined per sweep. One query per bot per hour, so the cap is
// not about cost — it is the guard that keeps a namespace which somehow
// accumulates thousands of them from turning a tick into a stall.
#define FOLLOWUPS_ROWS_MAX        256

// When an event is worth asking about, in whole days past. Never the
// day itself: a fact carries a date and never a time, so "today" would
// mean asking how the marathon went while it is still being run. Never
// more than a week later either — past that the question is
// archaeology, and the answer is something the room has long since
// heard.
#define FOLLOWUPS_DUE_MIN_DAYS    1
#define FOLLOWUPS_DUE_MAX_DAYS    7

// How long the written row keeps watching for its subject, counted from
// the EVENT rather than from the sweep — the deadline belongs to the
// occasion, so a fact noticed late must not buy itself a later question.
#define FOLLOWUPS_ASK_TTL_SECS    (7 * 86400)

// ...except at the far edge of the claim window, where that arithmetic
// yields a row already dead: claimed, never asked, silently nothing. A
// plan we only just heard about still gets a real chance to be answered.
#define FOLLOWUPS_MIN_WINDOW_SECS (2 * 86400)

// The claim outlives the widest window by a week, so no later tick can
// re-ask inside it. Past that the event is out of range on its own
// arithmetic and the claim has nothing left to guard.
#define FOLLOWUPS_CLAIM_TTL_SECS  (14 * 86400)

// A stated date, in the two shapes the extractor is asked for:
// YYYY-MM-DD, or YYYY-MM when somebody named only a month. Returns
// local noon on the day the event fell, or (time_t)-1 for anything else
// in the column — somebody else's schema, an older extractor, model
// slop — which is skipped rather than guessed at.
//
// Noon and not midnight, so the day arithmetic below cannot be dragged
// across a boundary by an hour of daylight saving.
static time_t
followups_event_noon(const char *v)
{
  struct tm ev;
  size_t    len;
  uint32_t  year;
  uint32_t  mon;
  uint32_t  day        = 0;
  bool      month_only;
  int       want_mon;
  int       want_mday;
  time_t    t;

  if(v == NULL)
    return((time_t)-1);

  len        = strnlen(v, 11);
  month_only = (len == 7);

  if((len != 7 && len != 10) || v[4] != '-')
    return((time_t)-1);

  if(len == 10 && v[7] != '-')
    return((time_t)-1);

  for(size_t i = 0; i < len; i++)
    if(i != 4 && i != 7 && (v[i] < '0' || v[i] > '9'))
      return((time_t)-1);

  year = (uint32_t)strtoul(v, NULL, 10);
  mon  = (uint32_t)(((v[5] - '0') * 10) + (v[6] - '0'));

  if(!month_only)
    day = (uint32_t)(((v[8] - '0') * 10) + (v[9] - '0'));

  // The shape is the sentinel, never the day number: a written-out
  // "2026-08-00" is a malformed date, not a way of naming August, and
  // day zero reaches mktime as the previous month's last day if it is
  // let through.
  if(year < 1970 || mon < 1 || mon > 12 || (!month_only && day < 1))
    return((time_t)-1);

  memset(&ev, 0, sizeof(ev));
  ev.tm_year  = (int)year - 1900;
  ev.tm_hour  = 12;
  ev.tm_isdst = -1;

  if(month_only)
  {
    // A month with no day is a promise about the whole month, so the
    // follow-up is owed once all of it has passed. Day zero of the NEXT
    // month is the last day of this one and mktime does the
    // normalisation — December included, where month 12 rolls into
    // January and the day back onto the 31st.
    ev.tm_mon  = (int)mon;
    ev.tm_mday = 0;
  }

  else
  {
    ev.tm_mon  = (int)mon - 1;
    ev.tm_mday = (int)day;
  }

  want_mon  = ev.tm_mon;
  want_mday = ev.tm_mday;
  t         = mktime(&ev);

  // A day the calendar does not have — 02-31, 04-31, and Feb 29 of a
  // common year — is not a date to guess at. mktime rolls it forward and
  // the mismatch is how we notice; the month-only case is normalised on
  // purpose and so is exempt.
  if(t == (time_t)-1
      || (!month_only && (ev.tm_mon != want_mon || ev.tm_mday != want_mday)))
    return((time_t)-1);

  return(t);
}

// Whole days between the event's noon and today's. A future date
// answers negative and a broken clock answers -1, and the window check
// rejects both without needing to tell them apart.
static long long
followups_days_ago(time_t ev_noon, time_t now)
{
  struct tm today;
  time_t    today_noon;
  long long delta;

  localtime_r(&now, &today);
  today.tm_hour  = 12;
  today.tm_min   = 0;
  today.tm_sec   = 0;
  today.tm_isdst = -1;
  today_noon     = mktime(&today);

  if(today_noon == (time_t)-1)
    return(-1);

  delta = (long long)today_noon - (long long)ev_noon;

  // Both ends are local noon, so what is left over is at most an hour
  // of daylight saving either way — rounded off, never truncated.
  return((delta + (delta >= 0 ? 43200 : -43200)) / 86400);
}

// The slug is the event and it is what the question will be about.
// Hyphens and underscores come back as spaces, so `job-interview` reads
// as "their job interview" — and the possessive in the premise below is
// what makes one sentence fit every slug: their wedding, their exam,
// their trip to japan all land correctly, where "a" would not.
static void
followups_event_label(char *dst, size_t cap, const char *fact_key)
{
  size_t n;

  snprintf(dst, cap, "%s", fact_key + strlen(FOLLOWUPS_KEY_PREFIX));

  n = strnlen(dst, cap);

  for(size_t i = 0; i < n; i++)
    if(dst[i] == '-' || dst[i] == '_')
      dst[i] = ' ';
}

// Vague on purpose. The bot knows the date the fact carried, not what
// the person actually did on it, and a premise that insists on
// "on 2026-08-05" invites a reply that reads like a calendar entry.
static const char *
followups_when_phrase(long long days)
{
  if(days <= 1)
    return("yesterday");

  if(days <= 3)
    return("a couple of days ago");

  return("in the last week");
}

static void
followups_copy_col(char *dst, size_t cap, const db_result_t *res,
    uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  snprintf(dst, cap, "%s", s != NULL ? s : "");
}

// The cue payload: what is true, and how it is known. It names the
// person by the name they are addressed by, states only what they
// themselves said, and stops — the spine's `say` frame wraps it with
// the venue and the voice, and the persona decides how warmly to ask.
static void
followups_compose_ask(char *dst, size_t cap, const char *label,
    const char *event, const char *when)
{
  snprintf(dst, cap,
      "%s told you a while back about their %s, which was %s — and you"
      " have not asked them how it went",
      label, event, when);
}

void
chatbot_followups_run(const char *bot_name, uint32_t ns_id, bot_inst_t *bot)
{
  db_result_t   *res;
  method_inst_t *method;
  char           method_name[METHOD_NAME_SZ];
  time_t         now  = time(NULL);
  uint32_t       rows;
  uint32_t       sent = 0;
  char           sql[1280];
  char           key[KV_KEY_SZ];

  snprintf(key, sizeof(key), "bot.%s.behavior.soul.followups.enabled",
      bot_name);

  if(kv_get_uint(key) == 0)
    return;

  // Quiet hours are not asked about here, for occasions.c's reason:
  // writing a row is not speech. The cue it becomes is gated at the
  // moment it is delivered, which is a sighting that may be days away.
  // The name is copied out and the reference given straight back: the
  // scan below is a database round trip, and the row it writes names
  // the method rather than pointing at it.
  method = bot_first_method(bot);
  method_name[0] = '\0';

  if(method != NULL)
  {
    strlcpy(method_name, method_inst_name(method), sizeof(method_name));
    method_release(method);
  }

  if(method_name[0] == '\0')
  {
    clam(CLAM_DEBUG, FOLLOWUPS_CTX,
        "bot=%s followups idle (no method bound to deliver on)", bot_name);
    return;
  }

  // Newest value per (dossier, event), with the newest signature as the
  // deliverable identity and the fact's own channel as the venue — the
  // occasions shape, and for the same reasons: a plan restated is the
  // restatement, and a plan mentioned in a DM is asked about in that DM.
  //
  // No kind filter: the key is the contract and extractor kind labels
  // are model-chosen (memory_get_dossier_fact_by_key says so).
  snprintf(sql, sizeof(sql),
      "SELECT DISTINCT ON (df.dossier_id, df.fact_key) df.fact_key,"
      " df.fact_value, df.channel, d.display_label,"
      " COALESCE(sg.nickname,''), COALESCE(sg.username,''),"
      " COALESCE(sg.hostname,''), COALESCE(sg.verified_id,''),"
      " df.dossier_id"
      " FROM dossier_facts df"
      " JOIN dossier d ON d.id = df.dossier_id"
      " LEFT JOIN LATERAL (SELECT nickname, username, hostname,"
      "  verified_id FROM dossier_signature"
      "  WHERE dossier_id = df.dossier_id"
      "  ORDER BY last_seen DESC LIMIT 1) sg ON TRUE"
      " WHERE d.ns_id = %" PRIu32 " AND df.fact_key LIKE '%s%%'"
      " ORDER BY df.dossier_id, df.fact_key, df.last_seen DESC"
      " LIMIT %d",
      ns_id, FOLLOWUPS_KEY_PREFIX, FOLLOWUPS_ROWS_MAX);

  res = db_result_alloc();

  if(res == NULL || db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res != NULL)
      clam(CLAM_WARN, FOLLOWUPS_CTX, "event scan failed: %s",
          res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return;
  }

  rows = res->rows;

  for(uint32_t i = 0; i < rows; i++)
  {
    method_msg_t msg;
    const char  *fact_key;
    const char  *did;
    int64_t      dossier;
    time_t       ev_noon;
    time_t       deadline;
    long long    days;
    uint64_t     ttl;
    char         value[64];
    char         label[METHOD_NICKNAME_SZ];
    char         event[MEM_FACT_KEY_SZ];
    char         ckey [MEM_FACT_KEY_SZ + 48];
    char         ask  [512];

    fact_key = db_result_get(res, i, 0);

    // The LIKE guarantees the prefix; the check is what lets
    // followups_event_label step over it without counting on SQL.
    if(fact_key == NULL
        || strncmp(fact_key, FOLLOWUPS_KEY_PREFIX,
              strlen(FOLLOWUPS_KEY_PREFIX)) != 0
        || fact_key[strlen(FOLLOWUPS_KEY_PREFIX)] == '\0')
      continue;

    followups_copy_col(value, sizeof(value), res, i, 1);
    followups_copy_col(label, sizeof(label), res, i, 4);

    // Signature nickname first, dossier label as fallback; a row with
    // neither is unaddressable and skipped whole.
    if(label[0] == '\0')
      followups_copy_col(label, sizeof(label), res, i, 3);

    if(label[0] == '\0')
      continue;

    ev_noon = followups_event_noon(value);

    if(ev_noon == (time_t)-1)
    {
      clam(CLAM_DEBUG, FOLLOWUPS_CTX,
          "bot=%s %s = '%s' for %s is not a date — skipped",
          bot_name, fact_key, value, label);
      continue;
    }

    days = followups_days_ago(ev_noon, now);

    if(days < FOLLOWUPS_DUE_MIN_DAYS || days > FOLLOWUPS_DUE_MAX_DAYS)
      continue;

    did     = db_result_get(res, i, 8);
    dossier = did != NULL ? (int64_t)strtoll(did, NULL, 10) : 0;

    // Once per person per event, whatever the tick rate and however
    // many times the daemon restarts inside the window. A dossier that
    // collects plans it never resolves therefore asks about each of
    // them exactly once and then lets it go — no cleanup pass is owed
    // here, because the facts decay on their own schedule.
    snprintf(ckey, sizeof(ckey), "fup:%" PRId64 ":%s", dossier, fact_key);

    if(!soul_claim_take(ns_id, ckey, now + FOLLOWUPS_CLAIM_TTL_SECS))
      continue;

    // The tuple rides whole (the dcfc359 rule): a cue missing any of it
    // resolves to no dossier and the question arrives with no memory of
    // the person behind it. `sender` is the nickname because that is
    // what a method delivers on this method's lines — the presence
    // trigger matches the sighting against exactly these two fields.
    memset(&msg, 0, sizeof(msg));
    method_msg_bind(&msg, method);
    msg.timestamp = now;

    snprintf(msg.sender, sizeof(msg.sender), "%s", label);
    followups_copy_col(msg.channel,     sizeof(msg.channel),     res, i, 2);
    followups_copy_col(msg.nickname,    sizeof(msg.nickname),    res, i, 4);
    followups_copy_col(msg.username,    sizeof(msg.username),    res, i, 5);
    followups_copy_col(msg.hostname,    sizeof(msg.hostname),    res, i, 6);
    followups_copy_col(msg.verified_id, sizeof(msg.verified_id), res, i, 7);

    followups_event_label(event, sizeof(event), fact_key);
    followups_compose_ask(ask, sizeof(ask), label, event,
        followups_when_phrase(days));

    deadline = ev_noon + FOLLOWUPS_ASK_TTL_SECS;
    ttl      = deadline > now ? (uint64_t)(deadline - now) : 0;

    if(ttl < FOLLOWUPS_MIN_WINDOW_SECS)
      ttl = FOLLOWUPS_MIN_WINDOW_SECS;

    if(chatbot_deferred_insert(ns_id, dossier, &msg, method_name,
        "followup", DEFERRED_KIND_SAY, ask, NULL, 0, true, ttl) != SUCCESS)
    {
      // The claim is spent and the row is not written, so this event is
      // never asked about. Loud, because it is the one outcome here
      // that is nobody's design.
      clam(CLAM_WARN, FOLLOWUPS_CTX,
          "bot=%s claimed %s but failed to store the question — missed",
          bot_name, ckey);
      continue;
    }

    sent++;

    clam(CLAM_INFO, FOLLOWUPS_CTX,
        "bot=%s holding a question about %s's %s (%s) for %s — waiting to"
        " see them", bot_name, label, event, value,
        msg.channel[0] != '\0' ? msg.channel : "DM");
  }

  db_result_free(res);

  if(sent > 0)
    clam(CLAM_DEBUG, FOLLOWUPS_CTX,
        "bot=%s followups: %u question(s) queued", bot_name, sent);
}
