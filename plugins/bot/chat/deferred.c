// botmanager — MIT
// The deferred spine: durable work the bot owes a human, and the two
// verbs that create it.
//
// One table, two kinds. A `say` row is speech held until its moment
// (`/remind`); a `run` row is an ACTION held until its moment (`/in`)
// — "at time T, dispatch command C as principal P, and report the
// outcome in their venue, in voice." Deferred execution is privilege
// persistence, so the asker's whole identity tuple is stored and the
// permission gate runs TWICE: once at schedule time, once at fire time
// (CHATBOT.md §The initiative rule, acts vs observations). A principal
// demoted in between gets a spoken refusal, never a silent dispatch.
//
// Exactly-once is the DB's, never this mapping's: the claim-then-read
// UPDATE is the guard, so a reload or a crash mid-window delivers late
// rather than twice or never. Everything durable a later chore needs
// — a birthday wish waiting on a sighting, a follow-up question —
// lands in this same table.
//
// A row's moment is either a clock or a person. `due_at` names the
// first; `deliver_on_presence` names the second (CARE-2), and the two
// claim paths are disjoint by construction — the due sweep claims only
// `deliver_on_presence = FALSE`, a sighting claims one row by id. Both
// project the same columns and share one deliver, so say/run, expiry
// and the method-gone un-claim behave identically whichever moment
// arrived. CARE-5/6 are the first writers of presence rows.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "colors.h"
#include "db.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEFERRED_CTX "deferred"

// Horizon. Beyond this the ack would be a promise nobody remembers
// making. (Was SOUL_REMIND_MAX_SECS; it governs both verbs now.)
#define DEFERRED_MAX_SECS       (30ULL * 86400ULL)

// One claim batch per tick per bot; a backlog drains at this rate.
#define DEFERRED_CLAIM_MAX      5

// Recurrence floor. Nothing in this chunk writes a repeating row —
// CARE-5/6/7 will — so this is the guard that keeps a hand-seeded row
// from turning the tick into a spin loop.
#define DEFERRED_REPEAT_MIN     300

// The word that buys a presence row instead of a timed one, and how
// long it stays disarmed first. You are, necessarily, present when you
// ask for "when I'm back" — so the row waits out a floor before it
// starts watching, and the next thing you say after that delivers it.
// Long enough that it does not fire into the same conversation; short
// enough that a coffee break counts as being away.
#define DEFERRED_BACK_WORD           "back"
#define DEFERRED_BACK_QUIET_DEFAULT  1800

// Rendered rows per `in list` / `show deferred` page. A list longer
// than this is a paging problem, not a listing problem.
#define DEFERRED_LIST_MAX       20

// Claim RETURNING order. The fire path reads by index and the column
// list below is the only place the order is written, so the two live
// together and an inserted column breaks one build, not one delivery.
// Both claim paths — the due sweep and a presence sighting — project
// exactly this shape, which is what lets them share one deliver.
enum
{
  DC_ID = 0, DC_DOSSIER, DC_SOURCE, DC_KIND, DC_SENDER, DC_NICKNAME,
  DC_USERNAME, DC_HOSTNAME, DC_VERIFIED_ID, DC_METADATA, DC_METHOD_NAME,
  DC_CHANNEL, DC_BODY, DC_CMD_NAME, DC_REPEAT, DC_CREATED, DC_EXPIRED,
};

#define DEFERRED_CLAIM_COLS \
    "id, dossier_id, source, kind, sender, nickname, username, hostname," \
    " verified_id, metadata, method_name, channel, body, cmd_name," \
    " repeat_secs, EXTRACT(EPOCH FROM created_at)::BIGINT AS created_epoch," \
    " (expires_at IS NOT NULL AND expires_at < NOW()) AS expired"

// ---------- schema ----------

void
chatbot_deferred_ensure_schema(void)
{
  db_result_t *res = db_result_alloc();

  // The chat DDL discipline (memory_ensure_schema): owner-run
  // idempotent batches at plugin start(), after dossier_register_config
  // so the dossier(id) FK target exists.
  (void)db_query(
      "CREATE TABLE IF NOT EXISTS chat_deferred ("
      " id           BIGSERIAL    PRIMARY KEY,"
      " ns_id        INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " dossier_id   BIGINT       REFERENCES dossier(id) ON DELETE SET NULL,"
      " source       VARCHAR(16)  NOT NULL,"
      " kind         SMALLINT     NOT NULL,"
      " sender       VARCHAR(128) NOT NULL,"
      " nickname     VARCHAR(64)  NOT NULL DEFAULT '',"
      " username     VARCHAR(64)  NOT NULL DEFAULT '',"
      " hostname     VARCHAR(128) NOT NULL DEFAULT '',"
      " verified_id  VARCHAR(128) NOT NULL DEFAULT '',"
      " metadata     VARCHAR(512) NOT NULL DEFAULT '',"
      " method_name  VARCHAR(64)  NOT NULL,"
      " channel      VARCHAR(128) NOT NULL DEFAULT '',"
      " body         TEXT         NOT NULL,"
      " cmd_name     VARCHAR(32)  NOT NULL DEFAULT '',"
      " repeat_secs  INTEGER      NOT NULL DEFAULT 0,"
      " deliver_on_presence BOOLEAN NOT NULL DEFAULT FALSE,"
      " due_at       TIMESTAMPTZ  NOT NULL,"
      " expires_at   TIMESTAMPTZ,"
      " created_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " delivered_at TIMESTAMPTZ"
      ")", res);

  (void)db_query(
      "CREATE INDEX IF NOT EXISTS idx_chat_deferred_due"
      " ON chat_deferred(ns_id, due_at) WHERE delivered_at IS NULL", res);

  db_result_free(res);
}

// ---------- small shared plumbing ----------

// Copy stored user text into a prompt-bound buffer: control bytes
// become spaces so a line typed weeks ago cannot smuggle framing into
// today's cue.
static void
deferred_scrub_copy(char *dst, size_t cap, const char *src)
{
  size_t o = 0;

  for(size_t i = 0; src[i] != '\0' && o + 1 < cap; i++)
  {
    unsigned char c = (unsigned char)src[i];

    dst[o++] = (char)((c < 0x20 || c == 0x7f) ? ' ' : c);
  }

  dst[o] = '\0';
}

// Release a batch of db_escape() results. ⚠ mem_free() ABORTS the
// daemon on NULL (core/alloc.c — the strict allocator), and db_escape()
// returns NULL under connection-pool pressure, so a partly-failed batch
// must never be freed flat. Every escape site here goes through this.
static void
deferred_free_escaped(char **v, size_t n)
{
  for(size_t i = 0; i < n; i++)
    if(v[i] != NULL)
      mem_free(v[i]);
}

// A row is the caller's when its stored sender matches (nick case is
// not identity on IRC) or when both carry the same verified identity.
// The escaped inputs are the caller's own message fields, never a
// user-supplied name — ownership is not something you can type.
//
// Five surfaces need this predicate now, and since CARE-7 they are not
// all in this file: chat_pricewatch stores the identity tuple under
// the same column names and asks the same question of it. The contract
// is chatbot.h's.
bool
chatbot_row_owner_pred(const method_msg_t *msg, char *dst, size_t cap)
{
  char *e[2];
  bool  ok = FAIL;

  e[0] = db_escape(msg->sender);
  e[1] = db_escape(msg->verified_id);

  if(e[0] != NULL && e[1] != NULL)
  {
    if(e[1][0] != '\0')
      snprintf(dst, cap,
          "(lower(sender) = lower('%s') OR verified_id = '%s')", e[0], e[1]);

    else
      snprintf(dst, cap, "(lower(sender) = lower('%s'))", e[0]);

    ok = SUCCESS;
  }

  deferred_free_escaped(e, 2);
  return(ok);
}

// The count behind the per-owner rate limit, or -1 when the count could
// not be taken. A limit whose test failed must refuse, not admit: the
// caller distinguishes the two and says which happened.
static int64_t
deferred_pending_for(uint32_t ns_id, const char *owner_pred)
{
  db_result_t *res;
  const char  *cell;
  char         sql[1024];
  int64_t      n = -1;

  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM chat_deferred WHERE ns_id = %" PRIu32
      " AND delivered_at IS NULL AND %s", ns_id, owner_pred);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0
      && (cell = db_result_get(res, 0, 0)) != NULL)
    n = (int64_t)strtoll(cell, NULL, 10);

  else
    clam(CLAM_WARN, DEFERRED_CTX, "pending count failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(n);
}

// The one insert both verbs and every chore go through; the contract
// is chatbot.h's.
bool
chatbot_deferred_insert(uint32_t ns_id, int64_t dossier,
    const method_msg_t *msg, const char *method_name, const char *source,
    deferred_kind_t kind, const char *body, const char *cmd_name,
    uint64_t secs, bool on_presence, uint64_t expires_secs)
{
  // Escaped in one array so the all-or-nothing guard and the release
  // are each one loop; E_COUNT keeps the two from drifting apart.
  enum
  {
    E_SENDER = 0, E_NICK, E_USER, E_HOST, E_VID, E_META, E_METH,
    E_CHAN, E_BODY, E_CMD, E_COUNT,
  };
  char *e[E_COUNT];
  bool  ok    = FAIL;
  bool  whole = true;

  e[E_SENDER] = db_escape(msg->sender);
  e[E_NICK]   = db_escape(msg->nickname);
  e[E_USER]   = db_escape(msg->username);
  e[E_HOST]   = db_escape(msg->hostname);
  e[E_VID]    = db_escape(msg->verified_id);
  e[E_META]   = db_escape(msg->metadata);
  e[E_METH]   = db_escape(method_name);
  e[E_CHAN]   = db_escape(msg->channel);
  e[E_BODY]   = db_escape(body);
  e[E_CMD]    = db_escape(cmd_name != NULL ? cmd_name : "");

  for(size_t i = 0; i < E_COUNT; i++)
    if(e[i] == NULL)
      whole = false;

  if(whole)
  {
    char dossier_cell[32];
    char expires_cell[64];
    char sql[4096];

    if(dossier > 0)
      snprintf(dossier_cell, sizeof(dossier_cell), "%" PRId64, dossier);

    else
      snprintf(dossier_cell, sizeof(dossier_cell), "NULL");

    // A caller with a real deadline names it. Otherwise a presence row
    // gets the same 30-day horizon a timed one is capped at: somebody
    // who never comes back would otherwise hold a slot against their
    // own pending cap forever, and the tick's expiry sweep dropping it
    // unspoken is the honest end for work whose moment never arrived.
    if(expires_secs > 0)
      snprintf(expires_cell, sizeof(expires_cell),
          "NOW() + %llu * INTERVAL '1 second'",
          (unsigned long long)expires_secs);

    else if(on_presence)
      snprintf(expires_cell, sizeof(expires_cell), "NOW() + INTERVAL '30 days'");

    else
      snprintf(expires_cell, sizeof(expires_cell), "NULL");

    snprintf(sql, sizeof(sql),
        "INSERT INTO chat_deferred"
        " (ns_id, dossier_id, source, kind, sender, nickname, username,"
        "  hostname, verified_id, metadata, method_name, channel, body,"
        "  cmd_name, deliver_on_presence, due_at, expires_at)"
        " VALUES (%" PRIu32 ", %s, '%s', %d, '%s', '%s', '%s', '%s',"
        " '%s', '%s', '%s', '%s', '%s', '%s', %s,"
        " NOW() + %llu * INTERVAL '1 second', %s)",
        ns_id, dossier_cell, source, (int)kind, e[E_SENDER], e[E_NICK],
        e[E_USER], e[E_HOST], e[E_VID], e[E_META], e[E_METH], e[E_CHAN],
        e[E_BODY], e[E_CMD], on_presence ? "TRUE" : "FALSE",
        (unsigned long long)secs, expires_cell);

    ok = db_exec(sql, DEFERRED_CTX);
  }

  deferred_free_escaped(e, E_COUNT);
  return(ok);
}

// ---------- fire time ----------

// Rebuild the deliverable message from the stored row. The tuple rides
// whole (the dcfc359 coalescer lesson: a synthetic cue missing any of
// it resolves to no dossier and the reply loses the asker's memory),
// and the method is re-resolved by NAME so nothing dangles across a
// reload — msg->inst comes back HELD, and the caller owes it a
// method_release() once the row is delivered or abandoned.
static method_inst_t *
deferred_row_to_msg(const db_result_t *res, uint32_t i, method_msg_t *msg,
    time_t now)
{
  method_inst_t *inst;
  char           mname[METHOD_NAME_SZ];

  db_result_copy(mname, sizeof(mname), res, i, DC_METHOD_NAME);
  inst = method_find(mname);

  if(inst == NULL)
    return(NULL);

  memset(msg, 0, sizeof(*msg));
  method_msg_bind(msg, inst);
  msg->timestamp = now;

  db_result_copy(msg->sender,      sizeof(msg->sender),      res, i, DC_SENDER);
  db_result_copy(msg->nickname,    sizeof(msg->nickname),    res, i, DC_NICKNAME);
  db_result_copy(msg->username,    sizeof(msg->username),    res, i, DC_USERNAME);
  db_result_copy(msg->hostname,    sizeof(msg->hostname),    res, i, DC_HOSTNAME);
  db_result_copy(msg->verified_id, sizeof(msg->verified_id), res, i, DC_VERIFIED_ID);
  db_result_copy(msg->metadata,    sizeof(msg->metadata),    res, i, DC_METADATA);
  db_result_copy(msg->channel,     sizeof(msg->channel),     res, i, DC_CHANNEL);

  return(inst);
}

// Put a claimed row back to pending so a later moment can deliver it.
//
// ⚠ Never call this for a REPEATING row. The claim statement already
// wrote its successor, so un-claiming re-offers this occurrence on the
// very next tick and writes another successor, and another — one child
// per tick for as long as the condition lasts (measured 2026-08-11:
// three ticks, three rows). A recurrence is by definition re-offered,
// so for one the honest outcome is to skip the occurrence and let the
// successor stand. Returns whether the row was actually returned.
static bool
deferred_unclaim(int64_t id, int64_t repeat_secs)
{
  char sql[128];

  if(repeat_secs >= DEFERRED_REPEAT_MIN)
    return(false);

  snprintf(sql, sizeof(sql),
      "UPDATE chat_deferred SET delivered_at = NULL WHERE id = %" PRId64, id);
  (void)db_exec(sql, DEFERRED_CTX);
  return(true);
}

// Who chose the moment is a property of the SOURCE, not of the row: a
// human typed /remind and /in, so those are TIMED and the governor may
// not refuse them however late the hour. A price watch splits the
// difference and is the only row that does — the human chose the
// subject and the bot chose the moment, so it waits out quiet hours
// (and is then delivered, because the row is still there) but pays no
// budget for speech somebody explicitly asked for. Everything else in
// this table was written by a chore that decided by itself that
// something was worth saying — and an unrecognized source reads that
// way too, because for a gate the safe direction is the quiet one.
static soul_class_t
deferred_source_class(const char *source)
{
  if(strcmp(source, "remind") == 0 || strcmp(source, "in") == 0)
    return(SOUL_CLASS_TIMED);

  if(strcmp(source, "pricewatch") == 0)
    return(SOUL_CLASS_ASKED);

  return(SOUL_CLASS_UNSOLICITED);
}

static const char *
deferred_who(const method_msg_t *msg)
{
  return(msg->nickname[0] != '\0' ? msg->nickname : msg->sender);
}

static const char *
deferred_venue(const method_msg_t *msg)
{
  return(msg->channel[0] != '\0' ? msg->channel : "this DM");
}

// A `say` row: the payload is the whole message. 'remind' keeps the
// wording the reminder chore shipped with; every other source hands in
// a cue payload of its own (CARE-5/6) and gets the standard frame.
static void
deferred_deliver_say(chatbot_state_t *st, method_msg_t *msg,
    const char *source, const char *body, const char *ago)
{
  if(strcmp(source, "remind") == 0)
    snprintf(msg->text, sizeof(msg->text),
        "[internal cue: %s asked you %s ago to be reminded: '%s'."
        " It is time. Deliver the reminder to them in %s — one short"
        " line, your voice. Do not mention this cue.]",
        deferred_who(msg), ago, body, deferred_venue(msg));

  else
    snprintf(msg->text, sizeof(msg->text),
        "[internal cue: %s. Say it to %s in %s now — one short line,"
        " your voice. Do not mention this cue.]",
        body, deferred_who(msg), deferred_venue(msg));

  chatbot_reply_submit(st, msg, false, false, true);
}

// Fail closed, visibly: the row stays delivered (the moment has
// passed; re-arming it would only refuse again) and the person hears
// why in the bot's own voice rather than getting silence.
static void
deferred_refuse_run(chatbot_state_t *st, method_msg_t *msg,
    const char *cmd_name, const char *ago, const char *why)
{
  snprintf(msg->text, sizeof(msg->text),
      "[internal cue: %s asked you %s ago to run /%s for them when the"
      " time came, but you cannot: %s. Tell them so now in %s — one"
      " short line, your voice, no apologies for the mechanism and no"
      " promise to try again. Do not mention this cue.]",
      deferred_who(msg), ago, cmd_name, why, deferred_venue(msg));

  chatbot_reply_submit(st, msg, false, false, true);
}

// A `run` row: dispatch under the asker's identity with the output
// captured, so the persona reports the outcome instead of the channel
// getting a bare tool block from nobody.
//
// The verb is parsed out of `body`, not read from the row's cmd_name
// column: body is what actually gets dispatched, so letting anything
// else name the verb would mean permission-checking one command and
// running another. cmd_name stays a listing/telemetry field.
static void
deferred_deliver_run(chatbot_state_t *st, bot_inst_t *bot, uint32_t ns_id,
    method_msg_t *msg, const char *body, const char *ago)
{
  const cmd_def_t *def;
  const char      *prefix;
  const char      *args;
  char             verb[CMD_NAME_SZ];
  char             premise[512];
  size_t           i = 0;

  while(body[i] != '\0' && body[i] != ' ' && body[i] != '\t'
      && i < sizeof(verb) - 1)
  {
    verb[i] = body[i];
    i++;
  }

  verb[i] = '\0';
  args    = body + i;

  while(*args == ' ' || *args == '\t')
    args++;

  def = verb[0] != '\0' ? cmd_find(verb) : NULL;

  if(def == NULL)
  {
    deferred_refuse_run(st, msg, verb[0] != '\0' ? verb : "?", ago,
        "that command no longer exists");
    return;
  }

  // The fire-time half of the double check. The schedule-time one is
  // in cmd_in(); a principal who lost the permission in between is
  // refused here, in voice.
  if(!cmd_permits(bot, msg, def))
  {
    clam(CLAM_INFO, DEFERRED_CTX,
        "bot=%s deferred /%s for %s denied at fire time",
        bot_inst_name(bot), verb, msg->sender);
    deferred_refuse_run(st, msg, verb,
        ago, "they are no longer allowed to run it");
    return;
  }

  prefix = cmd_get_prefix(bot);

  if(prefix == NULL || prefix[0] == '\0')
    prefix = "/";

  snprintf(premise, sizeof(premise),
      "%s asked you %s ago to run /%s%s%s when the time came, and you"
      " just did.", deferred_who(msg), ago, verb,
      args[0] != '\0' ? " " : "", args);

  // Capture first: the sink id must ride the message the dispatch
  // sees. Slot exhaustion returns 0 and the command's own output goes
  // to the wire verbatim — never silence.
  msg->reply_sink_id = chatbot_interpret_begin(st, msg, verb, args,
      premise, false, false);

  if(args[0] != '\0')
    snprintf(msg->text, sizeof(msg->text), "%s%s %s", prefix, verb, args);

  else
    snprintf(msg->text, sizeof(msg->text), "%s%s", prefix, verb);

  clam(CLAM_INFO, DEFERRED_CTX,
      "bot=%s deferred dispatch '%s' as %s in %s (set %s ago)",
      bot_inst_name(bot), msg->text, msg->sender,
      msg->channel[0] != '\0' ? msg->channel : "DM", ago);

  // cmd_dispatch — not cmd_dispatch_resolved — on purpose: it walks
  // subcommands and re-runs the full permission / scope / method gate,
  // so even a row whose verb grew children since it was written lands
  // on the right leaf under the right check.
  (void)cmd_dispatch(bot, msg);

  // The same post-dispatch observer the bridge runs (reply.c): a
  // scheduled /weather is as much a standing request as a spoken one,
  // so the location slot writes its city_of_interest fact either way.
  chatbot_nl_observe_location_slot(bot, msg, ns_id, cmd_get_nl(def), args);
}

// Deliver one already-claimed row. Both claim paths land here, so the
// expiry drop, the method-gone un-claim and the say/run split are
// written once. `res` must project DEFERRED_CLAIM_COLS.
static void
deferred_deliver_row(const char *bot_name, uint32_t ns_id,
    chatbot_state_t *st, bot_inst_t *bot, const db_result_t *res,
    uint32_t i, time_t now)
{
  method_msg_t msg;
  int64_t      id      = db_result_get_i64(res, i, DC_ID, 0);
  int64_t      kind    = db_result_get_i64(res, i, DC_KIND, DEFERRED_KIND_SAY);
  time_t       created = (time_t)db_result_get_i64(res, i, DC_CREATED, now);
  const char  *expired = db_result_get(res, i, DC_EXPIRED);
  char         source[24];
  char         body[CMD_ARG_SZ];
  char         ago[UTIL_DURATION_SZ];

  db_result_copy(source, sizeof(source), res, i, DC_SOURCE);

  // Expiry is claim-and-drop: a wish two days stale or a watch whose
  // window closed is worse spoken late than never spoken at all.
  if(expired != NULL && (expired[0] == 't' || expired[0] == 'T'))
  {
    clam(CLAM_DEBUG, DEFERRED_CTX,
        "bot=%s deferred %" PRId64 " (%s) expired — dropped unspoken",
        bot_name, id, source);
    return;
  }

  if(deferred_row_to_msg(res, i, &msg, now) == NULL)
  {
    // Claim-then-fail must un-claim: with the method gone (unbound,
    // mid-reload) the row goes back to pending, so a re-bound method
    // delivers LATE rather than never. Leaving it claimed would eat
    // the work silently — the one outcome worse than lateness.
    bool back = deferred_unclaim(id, db_result_get_i64(res, i, DC_REPEAT, 0));

    clam(CLAM_WARN, DEFERRED_CTX,
        "bot=%s deferred %" PRId64 " method gone — %s", bot_name, id,
        back ? "unclaimed" : "occurrence skipped (successor stands)");
    return;
  }

  {
    const char *cell = db_result_get(res, i, DC_BODY);

    deferred_scrub_copy(body, sizeof(body), cell != NULL ? cell : "");
  }

  util_fmt_duration(now > created ? now - created : 0, ago, sizeof(ago));

  clam(CLAM_INFO, DEFERRED_CTX,
      "bot=%s delivering %s %" PRId64 " (%s) to %s in %s (set %s ago)",
      bot_name, kind == DEFERRED_KIND_RUN ? "run" : "say", id, source,
      msg.sender, msg.channel[0] != '\0' ? msg.channel : "DM", ago);

  // The governor, once, at the moment of speech. A TIMED row is never
  // refused — the human picked this moment — so for /remind and /in
  // this call is purely what puts the cue in the voice log (CARE-3
  // §D8). For a chore-written row it is a real gate, and by here the
  // only thing it can still fail is the budget: quiet hours were
  // answered before the claim.
  //
  // A refusal returns the row rather than eating it, which is where a
  // deferred cue parts company with a weather cue. Weather has no row
  // to go back to, so what its budget silences stays silent; a row can
  // simply wait for the next moment, and its own expires_at is the
  // statement of how late is too late. A birthday wish held by a busy
  // hour is spoken on the next sighting, or not at all after two days
  // — which is exactly the promise the chore made when it wrote it.
  if(!soul_voice_permits(bot_name, ns_id,
      db_result_get_i64(res, i, DC_DOSSIER, 0),
      deferred_source_class(source), false))
  {
    bool back = deferred_unclaim(id, db_result_get_i64(res, i, DC_REPEAT, 0));

    clam(CLAM_INFO, DEFERRED_CTX,
        "bot=%s deferred %" PRId64 " (%s) held by the voice governor — %s",
        bot_name, id, source,
        back ? "returned to pending" : "occurrence skipped (successor stands)");
    method_release(msg.inst);
    return;
  }

  if(kind == DEFERRED_KIND_RUN)
    deferred_deliver_run(st, bot, ns_id, &msg, body, ago);

  else
    deferred_deliver_say(st, &msg, source, body, ago);

  method_release(msg.inst);
}

void
chatbot_deferred_run_due(const char *bot_name, uint32_t ns_id,
    chatbot_state_t *st, bot_inst_t *bot)
{
  db_result_t *res;
  char         sql[2048];
  uint32_t     rows;
  time_t       now;

  // Claim-then-read in one statement (note_db_claim's shape): two
  // racing witnesses cannot both deliver a row, and the claim is what
  // makes delivery restart- and reload-safe — the guard is in the DB,
  // not in this mapping. The recurrence insert rides the same
  // statement, so a daemon that dies mid-tick never double-schedules:
  // the claim IS the write barrier.
  snprintf(sql, sizeof(sql),
      "WITH claimed AS ("
      "UPDATE chat_deferred SET delivered_at = NOW() WHERE id IN ("
      "SELECT id FROM chat_deferred WHERE ns_id = %" PRIu32
      " AND delivered_at IS NULL AND deliver_on_presence = FALSE"
      " AND due_at <= NOW()"
      " ORDER BY due_at ASC LIMIT %d)"
      " RETURNING " DEFERRED_CLAIM_COLS ","
      // Carried past the projection the deliver path reads so the
      // recurrence INSERT below can copy the row whole. dossier_id is
      // not repeated here — the claim columns already carry it, and a
      // duplicate name would make the SELECT below ambiguous.
      " ns_id, deliver_on_presence, expires_at),"
      " renewed AS ("
      "INSERT INTO chat_deferred"
      " (ns_id, dossier_id, source, kind, sender, nickname, username,"
      "  hostname, verified_id, metadata, method_name, channel, body,"
      "  cmd_name, repeat_secs, deliver_on_presence, due_at, expires_at)"
      " SELECT ns_id, dossier_id, source, kind, sender, nickname, username,"
      "  hostname, verified_id, metadata, method_name, channel, body,"
      "  cmd_name, repeat_secs, deliver_on_presence,"
      "  NOW() + repeat_secs * INTERVAL '1 second', expires_at"
      " FROM claimed WHERE repeat_secs >= %d AND NOT expired)"
      " SELECT * FROM claimed ORDER BY id ASC",
      ns_id, DEFERRED_CLAIM_MAX, DEFERRED_REPEAT_MIN);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, DEFERRED_CTX, "deferred claim failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return;
  }

  now  = time(NULL);
  rows = res->rows;

  for(uint32_t i = 0; i < rows; i++)
    deferred_deliver_row(bot_name, ns_id, st, bot, res, i, now);

  db_result_free(res);
}

// ---------- fire time: presence (CARE-2) ----------
//
// A presence row is work whose moment is a person, not a clock. The
// tick never claims one — its claim carries `deliver_on_presence =
// FALSE` — so these two entry points are the whole of its lifecycle:
// the tick offers the waiting subjects to the soul's cache, and a
// sighting comes back here to claim exactly one row by id.
//
// The venue is the row's, not the sighting's: being seen in #botman
// only *unlocks* a wish told in a DM, it does not move it into the
// channel. deferred_row_to_msg already reads the stored channel, so
// provenance routes delivery for free.

uint32_t
chatbot_deferred_presence_scan(const char *bot_name, uint32_t ns_id,
    chatbot_presence_row_t *out, uint32_t max)
{
  db_result_t *res;
  char         sql[1024];
  uint32_t     n = 0;

  // Expiry belongs to the tick, never to the sighting: a window that
  // closed while its subject was away must not sit in the cache waiting
  // to be spoken days late. Claim-and-log, same as the due sweep.
  snprintf(sql, sizeof(sql),
      "UPDATE chat_deferred SET delivered_at = NOW()"
      " WHERE ns_id = %" PRIu32 " AND delivered_at IS NULL"
      " AND deliver_on_presence AND expires_at IS NOT NULL"
      " AND expires_at < NOW() RETURNING id, source", ns_id);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *id  = db_result_get(res, i, 0);
      const char *src = db_result_get(res, i, 1);

      clam(CLAM_DEBUG, DEFERRED_CTX,
          "bot=%s presence %s (%s) expired unseen — dropped unspoken",
          bot_name, id != NULL ? id : "?", src != NULL ? src : "?");
    }

  db_result_free(res);

  if(out == NULL || max == 0)
    return(0);

  // `source` is projected for its class alone: the sighting path has
  // to know whether quiet hours apply to a row before it may claim it,
  // and the cache is all it will have to go on.
  snprintf(sql, sizeof(sql),
      "SELECT id, nickname, sender, source FROM chat_deferred"
      " WHERE ns_id = %" PRIu32 " AND delivered_at IS NULL"
      " AND deliver_on_presence AND due_at <= NOW()"
      " ORDER BY due_at ASC LIMIT %" PRIu32, ns_id, max);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, DEFERRED_CTX, "presence scan failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return(0);
  }

  for(uint32_t i = 0; i < res->rows && n < max; i++, n++)
  {
    char source[24];

    out[n].id = db_result_get_i64(res, i, 0, 0);
    db_result_copy(out[n].nickname, sizeof(out[n].nickname), res, i, 1);
    db_result_copy(out[n].sender,   sizeof(out[n].sender),   res, i, 2);
    db_result_copy(source,          sizeof(source),          res, i, 3);
    out[n].cls = deferred_source_class(source);
  }

  db_result_free(res);
  return(n);
}

void
chatbot_deferred_deliver_presence(const char *bot_name, uint32_t ns_id,
    chatbot_state_t *st, bot_inst_t *bot, int64_t id)
{
  db_result_t *res;
  char         sql[1024];

  snprintf(sql, sizeof(sql),
      "UPDATE chat_deferred SET delivered_at = NOW()"
      " WHERE id = %" PRId64 " AND ns_id = %" PRIu32
      " AND delivered_at IS NULL AND deliver_on_presence"
      " RETURNING " DEFERRED_CLAIM_COLS, id, ns_id);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, DEFERRED_CTX, "presence claim failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return;
  }

  // Zero rows is the ordinary losing side of a race — two bots in one
  // namespace both saw the line and the other one is delivering it.
  // Nothing to say about that.
  if(res->rows > 0)
    deferred_deliver_row(bot_name, ns_id, st, bot, res, 0, time(NULL));

  db_result_free(res);
}

// ---------- schedule time: the shared preamble ----------

// Everything both verbs need and neither may skip. Each refusal is its
// own reply — a user who mistyped a duration should never have to
// guess which of five gates it was.
typedef struct
{
  chatbot_state_t *st;
  userns_t        *ns;
  const char      *method_name;
  char             owner_pred[768];
  uint64_t         secs;
  bool             on_presence;
} deferred_ask_t;

static bool
deferred_ask_open(const cmd_ctx_t *ctx, const char *duration,
    deferred_ask_t *a)
{
  uint32_t cap;
  int64_t  pending;

  memset(a, 0, sizeof(*a));

  // "back" is a moment, not a length — the one place the two kinds of
  // moment meet a user. It rides the duration slot rather than a flag
  // or a child verb so that both verbs get it, and so `in list`, `in
  // cancel` and the pending cap need to know nothing new. The floor it
  // resolves to is when the row starts WATCHING, not when it speaks.
  a->on_presence = (strcasecmp(duration, DEFERRED_BACK_WORD) == 0);

  if(a->on_presence)
  {
    a->secs = (uint64_t)kv_get_bot_uint(bot_inst_name(ctx->bot),
        "behavior.soul.deferred.back_quiet_secs");

    if(a->secs == 0)
      a->secs = DEFERRED_BACK_QUIET_DEFAULT;
  }

  else
    a->secs = chatbot_parse_duration_secs(duration);

  if(a->secs == 0)
  {
    cmd_reply(ctx, "bad duration (use e.g. 30s, 5m, 2h, 1d — or 'back'"
        " to wait until you're next around)");
    return(FAIL);
  }

  if(a->secs > DEFERRED_MAX_SECS)
  {
    cmd_reply(ctx, "that's too far out — keep it under 30 days");
    return(FAIL);
  }

  a->st = bot_get_handle(ctx->bot);
  a->ns = bot_get_userns(ctx->bot);

  if(a->st == NULL || a->ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace to keep deferred work in");
    return(FAIL);
  }

  // Both kinds are delivered as persona speech — even a `run` reports
  // its outcome in voice — so a command-only bot would accept the work
  // and never speak it. Refuse the dead letter up front.
  if(kv_get_bot_uint(bot_inst_name(ctx->bot), "behavior.chat.enabled") == 0)
  {
    cmd_reply(ctx, "this bot doesn't speak (chat is disabled) — it"
        " would take this and never deliver it");
    return(FAIL);
  }

  a->method_name = method_inst_name(ctx->msg->inst);

  if(a->method_name == NULL || a->method_name[0] == '\0')
  {
    cmd_reply(ctx, "cannot tell which method to deliver on");
    return(FAIL);
  }

  if(chatbot_row_owner_pred(ctx->msg, a->owner_pred,
      sizeof(a->owner_pred)) != SUCCESS)
  {
    cmd_reply(ctx, "failed to prepare the request");
    return(FAIL);
  }

  cap = (uint32_t)kv_get_bot_uint_or_default(bot_inst_name(ctx->bot),
      "behavior.soul.deferred.max_pending");

  pending = deferred_pending_for(a->ns->id, a->owner_pred);

  if(pending < 0)
  {
    cmd_reply(ctx, "I couldn't check what you already have pending — "
        "try again shortly");
    return(FAIL);
  }

  if(pending >= (int64_t)cap)
  {
    char msg[128];

    snprintf(msg, sizeof(msg),
        "you already have %u things pending with me — cancel one first"
        " (see 'in list')", cap);
    cmd_reply(ctx, msg);
    return(FAIL);
  }

  return(SUCCESS);
}

// ---------- /remind ----------

static const cmd_arg_desc_t ad_remind[] = {
  { "duration", CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
  { "message",  CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static void
cmd_remind(const cmd_ctx_t *ctx)
{
  deferred_ask_t a;
  char           ack[96];

  if(deferred_ask_open(ctx, ctx->parsed->argv[0], &a) != SUCCESS)
    return;

  if(chatbot_deferred_insert(a.ns->id, chatbot_resolve_dossier(a.st, ctx->msg),
      ctx->msg, a.method_name, "remind", DEFERRED_KIND_SAY,
      ctx->parsed->argv[1], NULL, a.secs, a.on_presence, 0) != SUCCESS)
  {
    cmd_reply(ctx, "failed to store the reminder");
    return;
  }

  clam(CLAM_INFO, DEFERRED_CTX,
      "bot=%s reminder set by %s for %s in '%s' (%llu s%s)",
      bot_inst_name(ctx->bot), ctx->msg->sender, ctx->parsed->argv[0],
      ctx->msg->channel, (unsigned long long)a.secs,
      a.on_presence ? ", on presence" : "");

  if(a.on_presence)
    snprintf(ack, sizeof(ack), "noted — I'll remind you when you're back.");

  else
    snprintf(ack, sizeof(ack), "noted — I'll remind you in %s.",
        ctx->parsed->argv[0]);

  cmd_reply(ctx, ack);
}

// ---------- /in ----------

// Strip an optional command prefix from the wrapped text: people type
// what they would have typed ("in 5m !weather 45069"), and the stored
// verb is a name, never a rendered line.
static const char *
deferred_strip_prefix(const bot_inst_t *bot, const char *text)
{
  const char *prefix = cmd_get_prefix(bot);
  size_t      plen;

  while(*text == ' ' || *text == '\t')
    text++;

  plen = prefix != NULL ? strlen(prefix) : 0;

  if(plen > 0 && strncmp(text, prefix, plen) == 0)
    text += plen;

  else if(*text == '/')
    text++;

  while(*text == ' ' || *text == '\t')
    text++;

  return(text);
}

static void
cmd_in(const cmd_ctx_t *ctx)
{
  deferred_ask_t   a;
  const cmd_def_t *def;
  const char      *wrapped;
  const char      *args;
  char             verb[CMD_NAME_SZ];
  char             body[CMD_ARG_SZ];
  char             ack[384];
  size_t           i = 0;

  if(deferred_ask_open(ctx, ctx->parsed->argv[0], &a) != SUCCESS)
    return;

  wrapped = deferred_strip_prefix(ctx->bot, ctx->parsed->argv[1]);

  while(wrapped[i] != '\0' && wrapped[i] != ' ' && wrapped[i] != '\t'
      && i < sizeof(verb) - 1)
  {
    verb[i] = wrapped[i];
    i++;
  }

  verb[i] = '\0';
  args    = wrapped + i;

  while(*args == ' ' || *args == '\t')
    args++;

  if(verb[0] == '\0')
  {
    cmd_reply(ctx, "tell me what to run, e.g. 'in 1h weather 45069'");
    return;
  }

  // Deferring a deferral is always nonsense, and it is not only `/in
  // in` — `/in back remind about the PR` parses, passes every other
  // gate (remind exists and is NL-capable) and stores a row that can
  // only ever refuse itself, because at fire time /remind reads "about"
  // as its duration. Observed from the bridge 2026-08-12, not imagined.
  if(strcasecmp(verb, "in") == 0 || strcasecmp(verb, "remind") == 0)
  {
    cmd_reply(ctx, "I can't defer a deferral — ask me directly and I'll"
        " hold it myself");
    return;
  }

  def = cmd_find(verb);

  if(def == NULL)
  {
    char msg[96];

    snprintf(msg, sizeof(msg), "I don't know a command called '%s'", verb);
    cmd_reply(ctx, msg);
    return;
  }

  // The uniform rule, both invocation paths: only a command the model
  // is allowed to reach for conversationally may be put on a timer.
  // That is what keeps /in a user feature rather than a way to
  // schedule the daemon's own lifecycle verbs.
  if(cmd_get_nl(def) == NULL)
  {
    char msg[128];

    snprintf(msg, sizeof(msg),
        "'%s' isn't something I can run on a timer — only conversational"
        " commands qualify", verb);
    cmd_reply(ctx, msg);
    return;
  }

  // Schedule-time half of the double check (fire time repeats it).
  if(!cmd_permits(ctx->bot, ctx->msg, def))
  {
    char msg[96];

    snprintf(msg, sizeof(msg), "you're not allowed to run '%s'", verb);
    cmd_reply(ctx, msg);
    return;
  }

  // Args are deliberately NOT dry-parsed: the command is the authority
  // on its own arguments, so a bad-args row voices the command's own
  // error through the capture when it fires. Honest beats clever.
  if(args[0] != '\0')
    snprintf(body, sizeof(body), "%s %s", verb, args);

  else
    snprintf(body, sizeof(body), "%s", verb);

  if(chatbot_deferred_insert(a.ns->id, chatbot_resolve_dossier(a.st, ctx->msg),
      ctx->msg, a.method_name, "in", DEFERRED_KIND_RUN, body, verb,
      a.secs, a.on_presence, 0) != SUCCESS)
  {
    cmd_reply(ctx, "failed to store it");
    return;
  }

  clam(CLAM_INFO, DEFERRED_CTX,
      "bot=%s deferred '%s' set by %s for %s in '%s' (%llu s%s)",
      bot_inst_name(ctx->bot), body, ctx->msg->sender,
      ctx->parsed->argv[0], ctx->msg->channel,
      (unsigned long long)a.secs, a.on_presence ? ", on presence" : "");

  if(a.on_presence)
    snprintf(ack, sizeof(ack), "alright — I'll run %s when you're back.",
        body);

  else
    snprintf(ack, sizeof(ack), "alright — I'll run %s in %s.",
        body, ctx->parsed->argv[0]);

  cmd_reply(ctx, ack);
}

// ---------- /in list · /in cancel · /show deferred ----------

// Shared row renderer. `who` is rendered only by the admin view — a
// user listing their own work already knows whose it is.
static void
deferred_render(const cmd_ctx_t *ctx, const db_result_t *res, bool with_who)
{
  char line[512];

  for(uint32_t i = 0; i < res->rows; i++)
  {
    char        due  [UTIL_DURATION_SZ];
    char        who  [160] = "";
    char        id   [24];
    char        src  [24];
    char        venue[METHOD_CHANNEL_SZ];
    char        body [192];
    const char *pres = db_result_get(res, i, 7);
    int64_t     secs = db_result_get_i64(res, i, 4, 0);
    bool        back = (pres != NULL && (pres[0] == 't' || pres[0] == 'T'));

    char        when [48];

    util_fmt_duration(secs > 0 ? (time_t)secs : 0, due, sizeof(due));

    // A presence row has no due time to render — its moment is a
    // person. While it counts out its quiet floor it is not even
    // watching yet, and "in 20m" would promise a delivery that only a
    // sighting can cause. Third person on purpose: the admin view
    // renders the same string about somebody else.
    if(back)
      snprintf(when, sizeof(when), "when back%s", secs > 0 ? " (arming)" : "");

    else if(secs > 0)
      snprintf(when, sizeof(when), "in %s", due);

    else
      snprintf(when, sizeof(when), "now");

    db_result_copy(id,    sizeof(id),    res, i, 0);
    db_result_copy(src,   sizeof(src),   res, i, 1);
    db_result_copy(body,  sizeof(body),  res, i, 3);
    db_result_copy(venue, sizeof(venue), res, i, 5);

    if(with_who)
    {
      char nick[METHOD_SENDER_SZ];

      db_result_copy(nick, sizeof(nick), res, i, 6);
      snprintf(who, sizeof(who), CLR_CYAN "%s" CLR_RESET " ", nick);
    }

    snprintf(line, sizeof(line),
        "  " CLR_BOLD "%s" CLR_RESET "  %s%s/%s  %s  %s: %s",
        id, who, src,
        db_result_get_i64(res, i, 2, DEFERRED_KIND_SAY) == DEFERRED_KIND_RUN
            ? "run" : "say",
        when, venue[0] != '\0' ? venue : "DM", body);

    cmd_reply(ctx, line);
  }
}

// Columns 0..7 are what deferred_render reads; every listing query
// below produces exactly this shape.
#define DEFERRED_LIST_COLS \
    "id, source, kind, body," \
    " GREATEST(0, EXTRACT(EPOCH FROM (due_at - NOW()))::BIGINT)," \
    " channel, COALESCE(NULLIF(nickname,''), sender), deliver_on_presence"

static void
cmd_in_list(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  userns_t    *ns = bot_get_userns(ctx->bot);
  char         pred[768];
  char         sql[1536];

  if(ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace");
    return;
  }

  if(chatbot_row_owner_pred(ctx->msg, pred, sizeof(pred)) != SUCCESS)
  {
    cmd_reply(ctx, "failed to prepare the query");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT " DEFERRED_LIST_COLS " FROM chat_deferred"
      " WHERE ns_id = %u AND delivered_at IS NULL AND %s"
      " ORDER BY due_at ASC LIMIT %d", ns->id, pred, DEFERRED_LIST_MAX);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "could not read your pending work");
    return;
  }

  if(res->rows == 0)
    cmd_reply(ctx, "nothing pending.");

  else
  {
    cmd_reply(ctx, "pending:");
    deferred_render(ctx, res, false);
  }

  db_result_free(res);
}

// Owner or admin. The owner test is the stored tuple; the admin test
// is the caller's authenticated membership, so an unauthenticated
// nick can never reach another person's row however it is spelled.
// Shared with the price watch's cancel since CARE-7 — the same
// question about a different table.
bool
chatbot_caller_is_admin(const cmd_ctx_t *ctx, const userns_t *ns)
{
  if(ctx->username == NULL || ctx->username[0] == '\0')
    return(false);

  if(userns_is_owner(ctx->username))
    return(true);

  return(userns_member_level(ns, ctx->username, USERNS_GROUP_ADMIN) >= 100);
}

static const cmd_arg_desc_t ad_in_cancel[] = {
  { "id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 20, NULL },
};

static void
cmd_in_cancel(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  userns_t    *ns = bot_get_userns(ctx->bot);
  char         pred[768];
  char         sql[1536];
  char         ack[96];
  int64_t      id;

  if(ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace");
    return;
  }

  id = (int64_t)strtoll(ctx->parsed->argv[0], NULL, 10);

  if(chatbot_caller_is_admin(ctx, ns))
    snprintf(pred, sizeof(pred), "TRUE");

  else if(chatbot_row_owner_pred(ctx->msg, pred, sizeof(pred)) != SUCCESS)
  {
    cmd_reply(ctx, "failed to prepare the query");
    return;
  }

  // DELETE, not a claim: a cancelled row is not late work, it is work
  // that never happens, and leaving a tombstone would only confuse the
  // pending count.
  snprintf(sql, sizeof(sql),
      "DELETE FROM chat_deferred WHERE id = %" PRId64
      " AND ns_id = %u AND delivered_at IS NULL AND %s RETURNING id",
      id, ns->id, pred);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "could not cancel it");
    return;
  }

  if(res->rows == 0)
    cmd_reply(ctx, "no pending item with that id that's yours to cancel");

  else
  {
    snprintf(ack, sizeof(ack), "cancelled %" PRId64 ".", id);
    cmd_reply(ctx, ack);
  }

  db_result_free(res);
}

static const cmd_arg_desc_t ad_show_deferred[] = {
  { "nick", CMD_ARG_NONE, CMD_ARG_OPTIONAL, 64, NULL },
};

static void
cmd_show_deferred(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  userns_t    *ns = userns_session_resolve(ctx);
  char         filter[256] = "";
  char         sql[1536];

  if(ns == NULL)
  {
    cmd_reply(ctx, "no namespace resolved");
    return;
  }

  if(ctx->parsed != NULL && ctx->parsed->argc > 0
      && ctx->parsed->argv[0] != NULL && ctx->parsed->argv[0][0] != '\0')
  {
    char *e_nick = db_escape(ctx->parsed->argv[0]);

    if(e_nick == NULL)
    {
      cmd_reply(ctx, "failed to prepare the query");
      return;
    }

    snprintf(filter, sizeof(filter),
        " AND (lower(sender) = lower('%s') OR lower(nickname) = lower('%s'))",
        e_nick, e_nick);
    mem_free(e_nick);
  }

  snprintf(sql, sizeof(sql),
      "SELECT " DEFERRED_LIST_COLS " FROM chat_deferred"
      " WHERE ns_id = %u AND delivered_at IS NULL%s"
      " ORDER BY due_at ASC LIMIT %d", ns->id, filter, DEFERRED_LIST_MAX);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "query failed");
    return;
  }

  if(res->rows == 0)
    cmd_reply(ctx, "no pending deferred work.");

  else
  {
    char head[96];

    snprintf(head, sizeof(head),
        "pending deferred work in " CLR_BOLD "%s" CLR_RESET ":", ns->name);
    cmd_reply(ctx, head);
    deferred_render(ctx, res, true);
  }

  db_result_free(res);
}

// ---------- registration ----------

static const cmd_arg_desc_t ad_in[] = {
  { "duration", CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
  { "command",  CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static const cmd_nl_slot_t in_slots[] = {
  { .name  = "duration",
    .type  = CMD_NL_ARG_DURATION,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "command",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t in_examples[] = {
  { .utterance  = "in an hour turn off the outside lights",
    .invocation = "/in 1h lights off outside" },
  { .utterance  = "check the weather in 45069 in 20 minutes",
    .invocation = "/in 20m weather 45069" },
  { .utterance  = "run the weather for 45069 next time I show up",
    .invocation = "/in back weather 45069" },
};

static const cmd_nl_t in_nl = {
  // ⚠⚠ `.when` is the ROUTING field — it is what decides /in against
  // /remind — and it is deliberately byte-identical to the pre-'back'
  // wording. Measured 2026-08-12: adding one clause about the 'back'
  // duration here pulled "remind me in 20 minutes to water the
  // tomatoes" (a canonical /remind utterance, no 'back' in it at all)
  // onto /in, which then refused because 'water' is not a verb. The
  // same probe routed correctly on the binary one commit earlier. A
  // form change belongs in .syntax and .examples; never buy it with a
  // clause in the field that carries the choice.
  .when          = "Someone asks you to DO something later — run a"
                   " command, check something, or act at a future time"
                   " (not merely be reminded).",
  .syntax        = "/in <duration|back> <command and its arguments>"
                   " — 'back' means when they are next seen speaking,"
                   " instead of after a length of time",
  .slots         = in_slots,
  .slot_count    = (uint8_t)(sizeof(in_slots) / sizeof(in_slots[0])),
  .examples      = in_examples,
  .example_count = (uint8_t)(sizeof(in_examples) / sizeof(in_examples[0])),
  .dispatch_text = NULL,
};

static const cmd_nl_slot_t remind_slots[] = {
  { .name  = "duration",
    .type  = CMD_NL_ARG_DURATION,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "message",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t remind_examples[] = {
  { .utterance  = "remind me in 20 minutes to flip the steaks",
    .invocation = "/remind 20m flip the steaks" },
  { .utterance  = "poke me about the laundry in two hours",
    .invocation = "/remind 2h the laundry" },
  { .utterance  = "remind me when I'm back to look at the pull request",
    .invocation = "/remind back look at the pull request" },
};

static const cmd_nl_t remind_nl = {
  // Left byte-identical for the same reason as /in's — see the note
  // above the sibling field.
  .when          = "Someone asks to be reminded of something after a"
                   " delay, or asks you to poke them later.",
  .syntax        = "/remind <duration|back> <message>"
                   " — 'back' means when they are next seen speaking,"
                   " instead of after a length of time",
  .slots         = remind_slots,
  .slot_count    = (uint8_t)(sizeof(remind_slots) / sizeof(remind_slots[0])),
  .examples      = remind_examples,
  .example_count = (uint8_t)(sizeof(remind_examples)
                             / sizeof(remind_examples[0])),
  .dispatch_text = NULL,
};

static const cmd_decl_t remind_decl = {
  .module      = "chat",
  .name        = "remind",
  .usage       = "remind <duration|back> <message>",
  .description = "Set a reminder the bot delivers when it comes due",
  .help_long   =
      "Stores a reminder and delivers it in the bot's own voice once\n"
      "due (checked every behavior.soul.interval_secs, default 60 s).\n"
      "Durations read like 30s, 5m, 2h or 1d, up to 30 days. Set in a\n"
      "channel it is delivered there; set in a DM it comes back as a\n"
      "DM. Sugar over the same deferred spine /in uses, so 'in list'\n"
      "and 'in cancel' see reminders too. Survives restarts and\n"
      "reloads.\n"
      "\n"
      "Say 'back' instead of a duration to be reminded when you are\n"
      "next around rather than at a set time. Since you are here when\n"
      "you ask, it waits out a quiet period first\n"
      "(behavior.soul.deferred.back_quiet_secs, default 30 min) and\n"
      "then delivers on the next thing you say.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_remind,
  .arg_desc    = ad_remind,
  .arg_count   = (uint8_t)(sizeof(ad_remind) / sizeof(ad_remind[0])),
  .nl          = &remind_nl,
};

static const cmd_decl_t in_decl = {
  .module      = "chat",
  .name        = "in",
  .usage       = "in <duration|back> <command> [args]",
  .description = "Run a command later, as you, and report back in voice",
  .help_long   =
      "Schedules any conversational command to run after a delay, in\n"
      "your name: 'in 20m weather 45069'. When it fires the bot runs\n"
      "the command as you — your permissions are re-checked at that\n"
      "moment, and a refusal is spoken, never silent — captures the\n"
      "output and tells you the outcome in its own voice, in the\n"
      "channel or DM you asked from.\n"
      "\n"
      "Only commands the bot can reach conversationally qualify, and\n"
      "only top-level ones. Durations read like 30s, 5m, 2h or 1d, up\n"
      "to 30 days. 'in list' shows what you have pending; 'in cancel\n"
      "<id>' drops one.\n"
      "\n"
      "Say 'back' instead of a duration — 'in back weather 45069' —\n"
      "to have it run when you are next around rather than at a set\n"
      "time. Since you are here when you ask, it waits out a quiet\n"
      "period first (behavior.soul.deferred.back_quiet_secs, default\n"
      "30 min) and then runs on the next thing you say.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_in,
  .arg_desc    = ad_in,
  .arg_count   = (uint8_t)(sizeof(ad_in) / sizeof(ad_in[0])),
  .nl          = &in_nl,
};

static const cmd_decl_t in_list_decl = {
  .module      = "chat",
  .name        = "list",
  .usage       = "in list",
  .description = "List your pending deferred work",
  .help_long   = "Everything you have waiting — reminders and scheduled\n"
                 "commands alike — with the id 'in cancel' takes.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_in_list,
  .parent_path = "in",
};

static const cmd_decl_t in_cancel_decl = {
  .module      = "chat",
  .name        = "cancel",
  .usage       = "in cancel <id>",
  .description = "Cancel one pending deferred item by id",
  .help_long   =
      "Drops a pending item. Yours to cancel means the request came\n"
      "from you; an admin may cancel any row in the namespace.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_in_cancel,
  .parent_path = "in",
  .arg_desc    = ad_in_cancel,
  .arg_count   = (uint8_t)(sizeof(ad_in_cancel) / sizeof(ad_in_cancel[0])),
};

static const cmd_decl_t show_deferred_decl = {
  .module      = "chat",
  .name        = "deferred",
  .usage       = "show deferred [<nick>]",
  .description = "Pending deferred work in the working namespace",
  .help_long   =
      "Namespace-wide view of everything waiting to fire: id, who\n"
      "asked, source, kind, how long until it is due and the venue it\n"
      "will land in. With a nick, only that person's rows.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_deferred,
  .parent_path = "show",
  .arg_desc    = ad_show_deferred,
  .arg_count   = (uint8_t)(sizeof(ad_show_deferred)
                 / sizeof(ad_show_deferred[0])),
};

bool
chatbot_deferred_register(void)
{
  if(cmd_register(&remind_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&in_decl) != SUCCESS)
    goto fail_in;

  if(cmd_register(&in_list_decl) != SUCCESS)
    goto fail_list;

  if(cmd_register(&in_cancel_decl) != SUCCESS)
    goto fail_cancel;

  if(cmd_register(&show_deferred_decl) != SUCCESS)
    goto fail_show;

  return(SUCCESS);

  // Registration failure is FATAL to the load and the daemon does not
  // come up with a half-registered surface; unwind what we can name.
fail_show:
  cmd_unregister_path("in/cancel");
fail_cancel:
  cmd_unregister_path("in/list");
fail_list:
  cmd_unregister_path("in");
fail_in:
  cmd_unregister_path("remind");
  return(FAIL);
}
