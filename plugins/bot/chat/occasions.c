// botmanager — MIT
// Occasions: turning a date somebody told the bot into a moment it acts
// on. Birthdays first.
//
// "How do you know?" — *you told the room your birthday.* That is the
// whole authorization story (CHATBOT.md §The initiative rule): the only
// input is a `birthday` fact the extractor wrote from something the
// person themselves said, and the wish goes back to the venue that fact
// was observed in. Nothing here is derived, aggregated or inferred.
//
// The shape every fact-driven chore reuses:
//
//   scan the namespace for a fact family
//     → claim it once per (subject, occasion)
//       → write a presence row and let the spine do the rest
//
// Nothing in this file speaks. Delivery belongs to CARE-1/2/3: presence
// unlocks the row when the subject next turns up, the stored tuple
// routes it to the venue they said it in, and the voice governor
// decides whether now is a decent hour. A chore that spoke for itself
// would need its own copy of all three, and would get one of them
// wrong.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "db.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OCCASIONS_CTX "occasions"

// Birthday facts examined per sweep. One query per bot per hour, so the
// cap is not about cost — it is the guard that keeps a namespace which
// somehow accumulates thousands of them from turning a tick into a
// stall.
#define OCCASIONS_ROWS_MAX        256

// A wish two days late is worse than no wish, so the row dies then,
// spoken or not (the deferred spine's expiry drop). Counted from the
// sweep that wrote it, which by construction runs on the day itself.
#define OCCASIONS_WISH_TTL_SECS   (2 * 86400)

// The claim outlives the wish by a week: nothing may re-claim the same
// birthday on a later tick the same day, and the key carries the year,
// so even a purge months later cannot resurrect one.
#define OCCASIONS_CLAIM_TTL_SECS  (9 * 86400)

// MM-DD, zero-padded, year deliberately absent — a birthday is a date
// in the year, not a date. Anything else in the column is somebody
// else's schema (a hand-seeded fact, an older extractor, model slop)
// and is skipped rather than guessed at.
static bool
occasions_date_valid(const char *v)
{
  uint32_t mon;
  uint32_t day;

  if(v == NULL || strnlen(v, 6) != 5 || v[2] != '-')
    return(false);

  for(size_t i = 0; i < 5; i++)
    if(i != 2 && (v[i] < '0' || v[i] > '9'))
      return(false);

  mon = (uint32_t)(((v[0] - '0') * 10) + (v[1] - '0'));
  day = (uint32_t)(((v[3] - '0') * 10) + (v[4] - '0'));

  return(mon >= 1 && mon <= 12 && day >= 1 && day <= 31);
}

static bool
occasions_is_leap(int year)
{
  return((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
}

// Feb 29 is the one birthday that does not happen every year. It is
// wished on the 28th in a common year: the 1st of March is the wrong
// day, and staying silent three years in four is worse than either.
static bool
occasions_falls_today(const char *mmdd, const struct tm *today)
{
  // Sized for what two ints can print, not for what a date is: a
  // struct tm is an int pair and the compiler is right to say so. A
  // nonsense one simply fails the compare below.
  char stamp[24];

  snprintf(stamp, sizeof(stamp), "%02d-%02d", today->tm_mon + 1,
      today->tm_mday);

  if(strcmp(mmdd, stamp) == 0)
    return(true);

  return(strcmp(mmdd, "02-29") == 0 && strcmp(stamp, "02-28") == 0
      && !occasions_is_leap(today->tm_year + 1900));
}

// The cue payload, and the reason this chore may exist at all: it says
// what to say and how it is known, and it names the person by the name
// they are addressed by — never a dossier id, never a fact key. The
// spine's `say` frame wraps it with the venue and the voice.
static void
occasions_compose_wish(char *dst, size_t cap, const char *label)
{
  snprintf(dst, cap,
      "today is %s's birthday — you know because they told you"
      " themselves a while back, and you have not mentioned it yet",
      label);
}

void
chatbot_occasions_run(const char *bot_name, uint32_t ns_id, bot_inst_t *bot)
{
  db_result_t   *res;
  method_inst_t *method;
  char           method_name[METHOD_NAME_SZ];
  struct tm      today;
  time_t         now  = time(NULL);
  uint32_t       rows;
  uint32_t       sent = 0;
  char           sql[1024];

  if(kv_get_bot_uint(bot_name, "behavior.soul.occasions.enabled") == 0)
    return;

  // Quiet hours are NOT asked about here, deliberately. Writing a row
  // is not speech: the cue it becomes is gated at the moment it is
  // delivered, which is a sighting that may be days away. Skipping the
  // scan overnight would only mean a wish written later on the same
  // day, and would risk losing one entirely to the two-day window.
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
    clam(CLAM_DEBUG, OCCASIONS_CTX,
        "bot=%s occasions idle (no method bound to deliver on)", bot_name);
    return;
  }

  localtime_r(&now, &today);

  // Newest birthday fact per dossier, with the newest signature as the
  // deliverable identity and the fact's own channel as the venue — the
  // weather watch's watch-list shape, and for the same reasons: a
  // person who restated the date meant the restatement, and a fact told
  // in a DM is answered in that DM.
  //
  // No kind filter: the key is the contract and extractor kind labels
  // are model-chosen (memory_get_dossier_fact_by_key says so). Two
  // kinds carrying one key would each produce a row and the claim below
  // would settle it, but DISTINCT ON settles it a round-trip earlier.
  snprintf(sql, sizeof(sql),
      "SELECT DISTINCT ON (df.dossier_id) df.fact_value, df.channel,"
      " d.display_label, COALESCE(sg.nickname,''),"
      " COALESCE(sg.username,''), COALESCE(sg.hostname,''),"
      " COALESCE(sg.verified_id,''), df.dossier_id"
      " FROM dossier_facts df"
      " JOIN dossier d ON d.id = df.dossier_id"
      " LEFT JOIN LATERAL (SELECT nickname, username, hostname,"
      "  verified_id FROM dossier_signature"
      "  WHERE dossier_id = df.dossier_id"
      "  ORDER BY last_seen DESC LIMIT 1) sg ON TRUE"
      " WHERE d.ns_id = %" PRIu32 " AND df.fact_key = 'birthday'"
      " ORDER BY df.dossier_id, df.last_seen DESC"
      " LIMIT %d",
      ns_id, OCCASIONS_ROWS_MAX);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, OCCASIONS_CTX, "birthday scan failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return;
  }

  rows = res->rows;

  for(uint32_t i = 0; i < rows; i++)
  {
    method_msg_t msg;
    const char  *did;
    int64_t      dossier;
    char         mmdd [16];
    char         label[METHOD_NICKNAME_SZ];
    char         ckey [96];
    char         wish [CMD_ARG_SZ];

    db_result_copy(mmdd,  sizeof(mmdd),  res, i, 0);
    db_result_copy(label, sizeof(label), res, i, 3);

    // Signature nickname first, dossier label as fallback; a row with
    // neither is unaddressable and skipped whole.
    if(label[0] == '\0')
      db_result_copy(label, sizeof(label), res, i, 2);

    if(label[0] == '\0')
      continue;

    if(!occasions_date_valid(mmdd))
    {
      clam(CLAM_DEBUG, OCCASIONS_CTX,
          "bot=%s birthday '%s' for %s is not MM-DD — skipped",
          bot_name, mmdd, label);
      continue;
    }

    if(!occasions_falls_today(mmdd, &today))
      continue;

    did     = db_result_get(res, i, 7);
    dossier = did != NULL ? (int64_t)strtoll(did, NULL, 10) : 0;

    // Once per person per year, whatever the tick rate and however many
    // times the daemon restarts today. The year is the occasion's
    // identity; the dossier is the subject's.
    snprintf(ckey, sizeof(ckey), "bday:%" PRId64 ":%d", dossier,
        today.tm_year + 1900);

    if(!soul_claim_take(ns_id, ckey, now + OCCASIONS_CLAIM_TTL_SECS))
      continue;

    // The tuple rides whole (the dcfc359 rule): a cue missing any of it
    // resolves to no dossier and the wish arrives with no memory of the
    // person behind it. `sender` is the nickname because that is what a
    // method delivers on this method's lines — the presence trigger
    // matches the sighting against exactly these two fields.
    memset(&msg, 0, sizeof(msg));
    method_msg_bind(&msg, method);
    msg.timestamp = now;

    snprintf(msg.sender, sizeof(msg.sender), "%s", label);
    db_result_copy(msg.channel,     sizeof(msg.channel),     res, i, 1);
    db_result_copy(msg.nickname,    sizeof(msg.nickname),    res, i, 3);
    db_result_copy(msg.username,    sizeof(msg.username),    res, i, 4);
    db_result_copy(msg.hostname,    sizeof(msg.hostname),    res, i, 5);
    db_result_copy(msg.verified_id, sizeof(msg.verified_id), res, i, 6);

    occasions_compose_wish(wish, sizeof(wish), label);

    if(chatbot_deferred_insert(bot_name, ns_id, dossier, &msg, method_name,
        "occasion", DEFERRED_KIND_SAY, wish, NULL, 0, true,
        OCCASIONS_WISH_TTL_SECS) != SUCCESS)
    {
      // The claim is spent and the row is not written, so this birthday
      // is missed for the year. Loud, because it is the one outcome
      // here that is nobody's design.
      clam(CLAM_WARN, OCCASIONS_CTX,
          "bot=%s claimed %s but failed to store the wish — missed",
          bot_name, ckey);
      continue;
    }

    sent++;

    clam(CLAM_INFO, OCCASIONS_CTX,
        "bot=%s birthday wish held for %s (%s) in %s — waiting to see them",
        bot_name, label, mmdd,
        msg.channel[0] != '\0' ? msg.channel : "DM");
  }

  db_result_free(res);

  if(sent > 0)
    clam(CLAM_DEBUG, OCCASIONS_CTX, "bot=%s occasions: %u wish(es) queued",
        bot_name, sent);
}
