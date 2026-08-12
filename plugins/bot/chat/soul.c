// botmanager — MIT
// The soul: a per-bot heartbeat that lets a persona act on its own.
// Each tick runs due chores (the deferred spine, the weather watch)
// and every chore speaks through the same cue → reply pipeline that
// volunteer speech uses, so persona, contract and markup all inherit.
//
// Durable chore state lives in the DB, never in this mapping: work
// survives restart and reload because delivery is a claimed UPDATE
// (the note plugin's exactly-once shape), not an in-memory queue. The
// scheduler side mirrors extract.c: one periodic task per bot, a
// mutex-guarded sched list whose entries outlive bot stops, and a soft
// latch instead of task churn.
//
// This file owns the schedule and the weather watch. The deferred
// spine — chat_deferred, /remind, /in — is deferred.c's; the chore
// here is one call into it.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "db.h"
#include "plugin.h"
#include "weathergov_api.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SOUL_CTX "soul"

#define SOUL_INTERVAL_DEFAULT_SECS 60
#define SOUL_INTERVAL_MIN_SECS     5

// Weather watch (D8) bounds. A sweep is one tick's fan-out: the watch
// list scan is capped, distinct coordinates are fetched once however
// many users share them, and a cue spells out at most a few alerts —
// a 20-county outbreak is one line and a count, not twenty lines.
#define SOUL_WX_INTERVAL_DEFAULT_SECS 600
#define SOUL_WX_MAX_PER_HOUR_DEFAULT  4
#define SOUL_WX_ROWS_MAX              32
#define SOUL_WX_POINTS_MAX            32
#define SOUL_WX_WATCHERS_MAX          8
#define SOUL_WX_CUE_ALERTS_MAX        3

typedef struct soul_sched soul_sched_t;

// A chore is one autonomous duty, run from the tick when its gates
// pass. kv_suffix names a per-chore knob family under
// bot.<n>.behavior.soul.<suffix>.* (NULL = no knobs of its own);
// interval_secs is the default cadence between runs, overridable
// per-bot via the knob family's own .interval_secs when kv_suffix is
// set (0 = every tick). fn runs on the tick's worker thread and may
// block on sync db_query / geocoders — the task is TASK_THREAD for
// exactly this reason. A chore that hands work to an async completion
// path returns true and the in-flight flag stays held until
// soul_chore_done(); returning false ends the chore with the tick.
typedef bool (*soul_chore_fn_t)(soul_sched_t *s, uint32_t chore,
    chatbot_state_t *st, bot_inst_t *bot);

typedef struct
{
  const char      *name;
  const char      *kv_suffix;
  uint32_t         interval_secs;
  soul_chore_fn_t  fn;
} soul_chore_t;

static bool soul_chore_deferred(soul_sched_t *, uint32_t,
    chatbot_state_t *, bot_inst_t *);
static bool soul_chore_weather(soul_sched_t *, uint32_t,
    chatbot_state_t *, bot_inst_t *);

static const soul_chore_t soul_chores[] = {
  { "deferred", NULL,      0,                             soul_chore_deferred },
  { "weather",  "weather", SOUL_WX_INTERVAL_DEFAULT_SECS, soul_chore_weather  },
};

#define SOUL_CHORE_COUNT (sizeof(soul_chores) / sizeof(soul_chores[0]))

// Per-bot schedule entry. Entries persist across bot stop/start (the
// extract.c latch pattern): stop flips active off and the parked task
// no-ops until re-scheduled; only plugin stop cancels tasks and only
// plugin deinit frees entries, in that order — each entry is a live
// task's data pointer until the cancel lands.
struct soul_sched
{
  char           bot_name[BOT_NAME_SZ];
  uint32_t       ns_id;
  bool           active;
  task_handle_t  task;
  time_t         last_ran [SOUL_CHORE_COUNT];
  bool           in_flight[SOUL_CHORE_COUNT];
  soul_sched_t  *next;
};

static soul_sched_t   *soul_sched_head = NULL;
static pthread_mutex_t soul_mutex      = PTHREAD_MUTEX_INITIALIZER;

// Statics vanish with the mapping, so an initializer is the whole init
// story; soul_exit() flips it for the teardown window.
static bool            soul_ready      = true;

// Caller holds soul_mutex.
static soul_sched_t *
soul_sched_find_locked(const char *bot_name)
{
  for(soul_sched_t *s = soul_sched_head; s != NULL; s = s->next)
    if(strcmp(s->bot_name, bot_name) == 0)
      return(s);

  return(NULL);
}

// Drop a chore's in-flight flag from an async completion path. Safe
// from any thread: sched entries are freed only at plugin deinit,
// after core's quiesce has already drained every task that could be
// holding one.
static void
soul_chore_done(soul_sched_t *s, uint32_t chore)
{
  pthread_mutex_lock(&soul_mutex);
  s->in_flight[chore] = false;
  pthread_mutex_unlock(&soul_mutex);
}

// Run one statement, logging failure. SUCCESS/FAIL so the un-claim
// path can notice a miss.
static bool
soul_db_exec(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok;

  if(res == NULL)
    return(FAIL);

  ok = (db_query(sql, res) == SUCCESS && res->ok) ? SUCCESS : FAIL;

  if(ok != SUCCESS)
    clam(CLAM_WARN, SOUL_CTX, "sql failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(ok);
}

static void
soul_copy_col(char *dst, size_t cap, const db_result_t *res,
    uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  snprintf(dst, cap, "%s", s != NULL ? s : "");
}

// ---------- chore: the deferred spine (CARE-1) ----------
//
// Every tick, on the tick's own worker thread: the whole claim →
// deliver → dispatch path lives in deferred.c, which owns the table
// and both verbs. The chore is the schedule, not the work.

static bool
soul_chore_deferred(soul_sched_t *s, uint32_t chore,
    chatbot_state_t *st, bot_inst_t *bot)
{
  (void)chore;

  chatbot_deferred_run_due(s->bot_name, s->ns_id, st, bot);
  return(false);
}

// ---------- chore: weather (D8) ----------
//
// The watch: every dossier in the bot's namespace that carries a
// city_of_interest fact (written by nl_observe after a successful
// bridged /weather) is a standing request to keep an eye on that sky.
// Each run scans the watch list, geocodes on this worker thread (the
// sync geocoders are worker-only and internally cached), collapses
// shared coordinates, and fans one weathergov_alerts_async out per
// distinct point. Completions land on the curl worker, which only
// copies the result and counts down; the LAST completion hands the
// whole sweep to a task worker for the DB claims and the cue.
//
// Exactly-once is the DB's job: an alert is announced only when this
// namespace wins the INSERT ... ON CONFLICT DO NOTHING claim on
// (alert_id, ns_id) — a reload mid-watch cannot double-announce, and
// two bots in different namespaces each announce to their own people.

typedef struct soul_wx_sweep soul_wx_sweep_t;

// One human watching one coordinate. `label` is how the cue names
// them; the tuple is what a DM delivery targets and what attributes
// the exchange to their dossier (the dcfc359 rule: the tuple rides
// every synthetic cue whole).
typedef struct
{
  char  label[METHOD_NICKNAME_SZ];
  char  city[128];                     // fact_value, as the user typed it
  char  channel[METHOD_CHANNEL_SZ];    // where the fact was observed; "" = DM
  char  sender[METHOD_SENDER_SZ];
  char  nickname[METHOD_NICKNAME_SZ];
  char  username[METHOD_USERNAME_SZ];
  char  hostname[METHOD_HOSTNAME_SZ];
  char  verified_id[METHOD_VERIFIED_ID_SZ];
} soul_wx_watcher_t;

typedef struct
{
  soul_wx_sweep_t           *sweep;    // back-pointer for the completion path
  double                     lat;
  double                     lon;
  char                       key[32];  // "%.4f,%.4f" — the dedupe identity
  soul_wx_watcher_t          watchers[SOUL_WX_WATCHERS_MAX];
  uint8_t                    n_watchers;
  bool                       got;      // callback delivered a result
  weathergov_alert_result_t  result;
} soul_wx_point_t;

// The whole fan-out, one allocation. `pending` counts airborne
// fetches; whoever decrements it to zero owns the hand-off to the
// batch task. If the chat plugin reloads with a fetch airborne the
// service suppresses our callback and this LEAKS whole — the accepted
// lifecycle outcome (weathergov_api.h); the fresh mapping starts with
// clear in-flight flags, so nothing wedges.
struct soul_wx_sweep
{
  soul_sched_t    *sched;
  uint32_t         chore;
  char             bot_name[BOT_NAME_SZ];
  uint32_t         ns_id;
  uint32_t         pending;
  uint8_t          n_points;
  soul_wx_point_t  points[SOUL_WX_POINTS_MAX];
};

// An announcement target: a channel (everyone's alerts for that
// channel batch into one cue) or a single watcher's DM. DM targets
// are per-watcher BY CONSTRUCTION — a DM cue must never leak another
// user's city into it (the DM-fact-guard discipline, MEMSTORE).
typedef struct
{
  const soul_wx_watcher_t *dm_watcher;             // NULL = channel target
  const soul_wx_watcher_t *lead;                   // identity the cue rides
  char                     channel[METHOD_CHANNEL_SZ];
  const soul_wx_point_t   *pts   [SOUL_WX_CUE_ALERTS_MAX];
  uint8_t                  alerts[SOUL_WX_CUE_ALERTS_MAX];
  uint8_t                  n_alerts;
  uint8_t                  total;                  // incl. beyond the spell-out cap
} soul_wx_target_t;

// One distinct CAP alert per sweep, with every point that carries it.
// The same alert covers every coordinate inside its polygon, so the
// per-(alert, ns) claim MUST be taken once per sweep and fanned out to
// all carrying points' watchers — claiming inside one point's walk
// hands the alert to whichever point iterates first and silently
// disinherits its siblings' watchers (measured 2026-08-11: two
// Cleveland coordinates, one zip-seeded and one city-seeded, under one
// Flood Watch — the DM watcher was never warned).
#define SOUL_WX_UNIQ_MAX  64

typedef struct
{
  const weathergov_alert_t *a;                     // first carrier's copy
  soul_wx_point_t          *carrier_pt[SOUL_WX_POINTS_MAX];
  uint8_t                   carrier_ai[SOUL_WX_POINTS_MAX];
  uint8_t                   n_carriers;
} soul_wx_uniq_t;

// Tolerant resolution, the nl_observe pattern: a missing provider
// idles the watch instead of aborting the daemon the way the
// api-header shims would. Re-attempted while incomplete, so a
// provider loaded later starts serving without a chat reload;
// plugin_dlsym_cached registers each slot for unload invalidation.
typedef bool (*soul_wx_geo_city_fn_t)(const char *, char *, size_t);
typedef bool (*soul_wx_geo_zip_fn_t)(const char *, double *, double *,
    char *, size_t);
typedef bool (*soul_wx_enabled_fn_t)(void);
typedef bool (*soul_wx_alerts_fn_t)(double, double,
    weathergov_alerts_cb_t, void *);

static soul_wx_geo_city_fn_t soul_wx_geo_city_fn;
static soul_wx_geo_zip_fn_t  soul_wx_geo_zip_fn;
static soul_wx_enabled_fn_t  soul_wx_enabled_fn;
static soul_wx_alerts_fn_t   soul_wx_alerts_fn;

static bool
soul_wx_resolve(void)
{
  union { void *obj; soul_wx_geo_city_fn_t fn; } uc;
  union { void *obj; soul_wx_geo_zip_fn_t  fn; } uz;
  union { void *obj; soul_wx_enabled_fn_t  fn; } ue;
  union { void *obj; soul_wx_alerts_fn_t   fn; } ua;

  uc.obj = plugin_dlsym_cached("openweather",
      "openweather_geocode_city_sync", (void **)&soul_wx_geo_city_fn);
  soul_wx_geo_city_fn = uc.fn;

  uz.obj = plugin_dlsym_cached("openweather",
      "openweather_geocode_zip_sync", (void **)&soul_wx_geo_zip_fn);
  soul_wx_geo_zip_fn = uz.fn;

  ue.obj = plugin_dlsym_cached("weathergov",
      "weathergov_enabled", (void **)&soul_wx_enabled_fn);
  soul_wx_enabled_fn = ue.fn;

  ua.obj = plugin_dlsym_cached("weathergov",
      "weathergov_alerts_async", (void **)&soul_wx_alerts_fn);
  soul_wx_alerts_fn = ua.fn;

  return(soul_wx_geo_city_fn != NULL && soul_wx_geo_zip_fn != NULL
      && soul_wx_enabled_fn != NULL && soul_wx_alerts_fn != NULL);
}

static weathergov_severity_t
soul_wx_severity_floor(const char *bot_name)
{
  const char *v;
  char        key[KV_KEY_SZ];

  snprintf(key, sizeof(key),
      "bot.%s.behavior.soul.weather.min_severity", bot_name);
  v = kv_get_str(key);

  if(v == NULL || v[0] == '\0')   return(WEATHERGOV_SEV_SEVERE);
  if(strcmp(v, "minor")    == 0)  return(WEATHERGOV_SEV_MINOR);
  if(strcmp(v, "moderate") == 0)  return(WEATHERGOV_SEV_MODERATE);
  if(strcmp(v, "extreme")  == 0)  return(WEATHERGOV_SEV_EXTREME);

  return(WEATHERGOV_SEV_SEVERE);
}

// City labels age: a fact written after a successful geocode can still
// fail one months later (cache cold, provider hiccup, place renamed).
// A miss is a quiet skip, never an error.
static bool
soul_wx_geocode(const char *label, double *lat, double *lon)
{
  char zip[32];

  if(chatbot_label_is_zip(label))
    return(soul_wx_geo_zip_fn(label, lat, lon, NULL, 0));

  if(soul_wx_geo_city_fn(label, zip, sizeof(zip)) != SUCCESS)
    return(FAIL);

  return(soul_wx_geo_zip_fn(zip, lat, lon, NULL, 0));
}

// "Tue 1:45 PM" in the alert's own locality — every CAP timestamp
// carries its offset inline and the service already parsed it out
// (§WXG-TRUTH 5), so this is arithmetic, not a tz-database walk.
static void
soul_wx_fmt_until(time_t ts, int32_t tz_offset, char *dst, size_t cap)
{
  struct tm tm;
  time_t    local = ts + (time_t)tz_offset;
  char      day[8];
  int       hr;

  if(ts == 0)
  {
    snprintf(dst, cap, "further notice");
    return;
  }

  gmtime_r(&local, &tm);
  strftime(day, sizeof(day), "%a", &tm);

  hr = tm.tm_hour % 12;
  if(hr == 0) hr = 12;

  snprintf(dst, cap, "%s %d:%02d %s", day, hr, tm.tm_min,
      tm.tm_hour < 12 ? "AM" : "PM");
}

// First sentence of the protective-action prose — instruction when
// present, else the description (fact 8: the two fields that exist
// precisely for an LLM to summarize). CAP prose arrives hard-wrapped,
// so control bytes become spaces and runs collapse.
static void
soul_wx_first_clause(const weathergov_alert_t *a, char *dst, size_t cap)
{
  const char *src = a->instruction[0] != '\0' ? a->instruction : a->desc;
  size_t      o   = 0;

  for(size_t i = 0; src[i] != '\0' && o + 1 < cap; i++)
  {
    unsigned char c = (unsigned char)src[i];

    if(c < 0x20 || c == 0x7f) c = ' ';
    if(c == ' ' && (o == 0 || dst[o - 1] == ' ')) continue;

    dst[o++] = (char)c;

    if(c == '.') break;
  }

  dst[o] = '\0';
}

// Bounded append; clamps the cursor at cap so every call after an
// overflow is a clean no-op rather than a size_t underflow.
static void
soul_wx_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
  va_list ap;
  int     n;

  if(*off >= cap) return;

  va_start(ap, fmt);
  n = vsnprintf(buf + *off, cap - *off, fmt, ap);
  va_end(ap);

  if(n > 0)        *off += (size_t)n;
  if(*off > cap)   *off  = cap;
}

// The exactly-once gate (fact 10): winning this INSERT is what
// authorizes an announcement, and losing it is the normal "already
// seen" answer — from a previous sweep, a racing peer, or the sweep a
// reload interrupted.
static bool
soul_wx_claim(uint32_t ns_id, const weathergov_alert_t *a)
{
  db_result_t *res;
  char        *e_id;
  char         sql[512];
  bool         fresh = false;

  e_id = db_escape(a->id);

  if(e_id == NULL)
    return(false);

  snprintf(sql, sizeof(sql),
      "INSERT INTO chat_soul_alerts_seen (alert_id, ns_id, expires)"
      " VALUES ('%s', %" PRIu32 ", to_timestamp(%lld))"
      " ON CONFLICT DO NOTHING RETURNING alert_id",
      e_id, ns_id, (long long)a->expires);
  mem_free(e_id);

  res = db_result_alloc();

  if(res == NULL)
    return(false);

  if(db_query(sql, res) != SUCCESS || !res->ok)
    clam(CLAM_WARN, SOUL_CTX, "alert claim failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  else if(res->rows > 0)
    fresh = true;

  db_result_free(res);
  return(fresh);
}

static uint32_t
soul_wx_hour_count(uint32_t ns_id)
{
  db_result_t *res;
  const char  *cell;
  char         sql[192];
  uint32_t     n = 0;

  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM chat_soul_alerts_seen WHERE ns_id = %" PRIu32
      " AND announced_at > NOW() - INTERVAL '1 hour'", ns_id);

  res = db_result_alloc();

  if(res == NULL)
    return(0);

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0
      && (cell = db_result_get(res, 0, 0)) != NULL)
    n = (uint32_t)strtoul(cell, NULL, 10);

  db_result_free(res);
  return(n);
}

static bool
soul_wx_watcher_in_target(const soul_wx_watcher_t *w,
    const soul_wx_target_t *t)
{
  if(t->dm_watcher != NULL)
    return(w == t->dm_watcher);

  return(strcmp(w->channel, t->channel) == 0);
}

static soul_wx_target_t *
soul_wx_target_for(soul_wx_target_t *targets, uint8_t *n,
    const soul_wx_watcher_t *w)
{
  soul_wx_target_t *t;

  for(uint8_t i = 0; i < *n; i++)
    if(soul_wx_watcher_in_target(w, &targets[i]))
      return(&targets[i]);

  if(*n >= SOUL_WX_ROWS_MAX)
    return(NULL);

  t = &targets[(*n)++];
  memset(t, 0, sizeof(*t));
  t->lead = w;

  if(w->channel[0] != '\0')
    snprintf(t->channel, sizeof(t->channel), "%s", w->channel);
  else
    t->dm_watcher = w;

  return(t);
}

// One cue per target: every fresh alert for a channel lands in a
// single submission (a 20-county outbreak is one line, not twenty),
// and a DM cue carries only its own watcher's sky. The cue rides the
// lead watcher's identity tuple whole (D6, the dcfc359 rule) so the
// exchange logs against a real dossier and DM delivery has a target.
static void
soul_wx_announce(chatbot_state_t *st, method_inst_t *method,
    const soul_wx_target_t *tgt, time_t now)
{
  method_msg_t             msg;
  const soul_wx_watcher_t *lead = tgt->lead;
  size_t                   off  = 0;

  memset(&msg, 0, sizeof(msg));
  msg.inst      = method;
  msg.timestamp = now;

  snprintf(msg.sender,      sizeof(msg.sender),      "%s", lead->sender);
  snprintf(msg.nickname,    sizeof(msg.nickname),    "%s", lead->nickname);
  snprintf(msg.username,    sizeof(msg.username),    "%s", lead->username);
  snprintf(msg.hostname,    sizeof(msg.hostname),    "%s", lead->hostname);
  snprintf(msg.verified_id, sizeof(msg.verified_id), "%s", lead->verified_id);

  if(tgt->dm_watcher == NULL)
    snprintf(msg.channel, sizeof(msg.channel), "%s", tgt->channel);

  soul_wx_append(msg.text, sizeof(msg.text), &off,
      "[internal cue: your weather watch just caught active National"
      " Weather Service alerts for people you know.");

  for(uint8_t i = 0; i < tgt->n_alerts; i++)
  {
    const soul_wx_point_t    *pt = tgt->pts[i];
    const weathergov_alert_t *a  = &pt->result.alerts.alerts[tgt->alerts[i]];
    char                      until[32];
    char                      clause[160];
    bool                      first = true;

    soul_wx_fmt_until(a->ends != 0 ? a->ends : a->expires, a->tz_offset,
        until, sizeof(until));
    soul_wx_first_clause(a, clause, sizeof(clause));

    soul_wx_append(msg.text, sizeof(msg.text), &off,
        " ALERT: %s until %s", a->event, until);

    if(a->impact[0] != '\0')
      soul_wx_append(msg.text, sizeof(msg.text), &off, " (%s)", a->impact);

    if(clause[0] != '\0')
      soul_wx_append(msg.text, sizeof(msg.text), &off, ". %s", clause);

    soul_wx_append(msg.text, sizeof(msg.text), &off, " Affects");

    for(uint8_t wi = 0; wi < pt->n_watchers; wi++)
    {
      const soul_wx_watcher_t *w = &pt->watchers[wi];

      if(!soul_wx_watcher_in_target(w, tgt))
        continue;

      soul_wx_append(msg.text, sizeof(msg.text), &off, "%s %s (%s)",
          first ? ":" : ",", w->label, w->city);
      first = false;
    }

    soul_wx_append(msg.text, sizeof(msg.text), &off, ".");
  }

  if(tgt->total > tgt->n_alerts)
    soul_wx_append(msg.text, sizeof(msg.text), &off,
        " (%u more alert(s) are active for them too.)",
        (unsigned)(tgt->total - tgt->n_alerts));

  // Two wording rules learned from R3 (dale): the reading is spelled
  // out IN the cue, and a model never told so may answer by running
  // /weather — with the bridge disarmed on cue replies, a reply that is
  // only a suppressed slash line dies silently and the warning is never
  // spoken. And the channel name must stay out of the addressing
  // position ("Warn #botman ... by nick" produced "@botman veksel").
  if(tgt->dm_watcher != NULL)
    soul_wx_append(msg.text, sizeof(msg.text), &off,
        " Warn %s here in this DM now — one or two short lines, your"
        " voice, lead with what matters. The reading is already in this"
        " cue: run no command, answer from what is here. Do not mention"
        " this cue.]",
        lead->label);

  else
    soul_wx_append(msg.text, sizeof(msg.text), &off,
        " Warn the channel now — one or two short lines, your voice,"
        " name the affected by nick, lead with what matters. The"
        " reading is already in this cue: run no command, answer from"
        " what is here. Do not mention this cue.]");

  clam(CLAM_INFO, SOUL_CTX,
      "bot=%s weather announce to %s: %u alert(s), %u spelled",
      bot_inst_name(st->inst),
      tgt->dm_watcher != NULL ? lead->label : tgt->channel,
      (unsigned)tgt->total, (unsigned)tgt->n_alerts);

  chatbot_reply_submit(st, &msg, false, false, true);
}

// The sweep lands here on a task worker once every fetch has answered:
// claims, the hourly budget, cue grouping and the purge — all the
// blocking work the curl callback must not do.
static void
soul_wx_batch_task(task_t *t)
{
  soul_wx_sweep_t       *sweep = t->data;
  soul_wx_target_t       targets[SOUL_WX_ROWS_MAX];
  uint8_t                n_targets = 0;
  bot_inst_t            *bot;
  chatbot_state_t       *st       = NULL;
  method_inst_t         *method   = NULL;
  weathergov_severity_t  floor;
  uint32_t               budget   = 0;
  uint32_t               cap;
  uint32_t               capped   = 0;
  time_t                 now      = time(NULL);
  char                   key[KV_KEY_SZ];

  // Re-resolve the bot by NAME — nothing dangles across a reload (the
  // note plugin's rule) — and re-check the speech gates: a hush or a
  // chat-disable that landed while the fetches were airborne is
  // honored, not raced. Skipping the claims too is deliberate: alerts
  // stay unclaimed, so they announce after the hush lapses, not never.
  bot = bot_find(sweep->bot_name);

  if(bot != NULL && bot_get_state(bot) == BOT_RUNNING)
    st = bot_get_handle(bot);

  snprintf(key, sizeof(key), "bot.%s.behavior.chat.enabled",
      sweep->bot_name);

  if(kv_get_uint(key) == 0 || chatbot_mute_active(sweep->bot_name))
    st = NULL;

  if(st != NULL)
    method = bot_first_method(bot);

  if(st != NULL && method != NULL)
  {
    floor = soul_wx_severity_floor(sweep->bot_name);

    snprintf(key, sizeof(key), "bot.%s.behavior.soul.weather.max_per_hour",
        sweep->bot_name);
    cap = (uint32_t)kv_get_uint(key);

    if(cap == 0)
      cap = SOUL_WX_MAX_PER_HOUR_DEFAULT;

    {
      uint32_t hour = soul_wx_hour_count(sweep->ns_id);

      budget = hour < cap ? cap - hour : 0;
    }

    // Pass 1 — collect the sweep's DISTINCT alerts (by CAP id) across
    // every covered point, remembering each carrying point. See
    // soul_wx_uniq_t for why claiming inside this walk would be wrong.
    soul_wx_uniq_t uniq[SOUL_WX_UNIQ_MAX];
    uint8_t        n_uniq = 0;

    for(uint8_t p = 0; p < sweep->n_points; p++)
    {
      soul_wx_point_t *pt = &sweep->points[p];

      if(!pt->got)
        continue;

      if(!pt->result.covered)
      {
        // Non-US is the silent normal (empty err); a real transport
        // or parse failure is worth a line.
        if(pt->result.err[0] != '\0')
          clam(CLAM_DEBUG, SOUL_CTX, "bot=%s watch point %s: %s",
              sweep->bot_name, pt->key, pt->result.err);
        continue;
      }

      for(uint8_t ai = 0; ai < pt->result.alerts.count; ai++)
      {
        const weathergov_alert_t *a = &pt->result.alerts.alerts[ai];
        soul_wx_uniq_t           *u = NULL;

        if(a->severity < floor)
          continue;

        for(uint8_t k = 0; k < n_uniq; k++)
          if(strcmp(uniq[k].a->id, a->id) == 0)
          {
            u = &uniq[k];
            break;
          }

        if(u == NULL)
        {
          if(n_uniq >= SOUL_WX_UNIQ_MAX)
          {
            clam(CLAM_WARN, SOUL_CTX,
                "bot=%s watch: distinct-alert table full (%u), '%s'"
                " dropped this sweep (unclaimed — it re-offers next"
                " sweep)", sweep->bot_name, (unsigned)n_uniq, a->event);
            continue;
          }

          u = &uniq[n_uniq++];
          memset(u, 0, sizeof(*u));
          u->a = a;
        }

        if(u->n_carriers < SOUL_WX_POINTS_MAX)
        {
          u->carrier_pt[u->n_carriers] = pt;
          u->carrier_ai[u->n_carriers] = ai;
          u->n_carriers++;
        }
      }
    }

    // Pass 2 — one claim per distinct alert, then fan the announcement
    // out to EVERY carrying point's watchers.
    for(uint8_t ui = 0; ui < n_uniq; ui++)
    {
      soul_wx_uniq_t   *u = &uniq[ui];
      soul_wx_target_t *seen[SOUL_WX_ROWS_MAX];   // distinct targets, not one point's watchers
      uint8_t           n_seen = 0;

      if(!soul_wx_claim(sweep->ns_id, u->a))
        continue;

      if(budget == 0)
      {
        // Deliberate: the claim above already recorded this alert as
        // seen, so a storm capped mid-outbreak does NOT re-announce
        // when the cap lifts — silence now is silence for good.
        capped++;
        continue;
      }

      budget--;

      // Route once per DISTINCT target across all carriers — two users
      // in one channel share a cue line however many points they watch;
      // a DM watcher gets an isolated cue of their own.
      for(uint8_t ci = 0; ci < u->n_carriers; ci++)
      {
        soul_wx_point_t *pt = u->carrier_pt[ci];
        uint8_t          ai = u->carrier_ai[ci];

        for(uint8_t wi = 0; wi < pt->n_watchers; wi++)
        {
          soul_wx_target_t *tg =
              soul_wx_target_for(targets, &n_targets, &pt->watchers[wi]);
          bool              dup = false;

          if(tg == NULL)
            continue;

          for(uint8_t k = 0; k < n_seen; k++)
            if(seen[k] == tg)
            {
              dup = true;
              break;
            }

          if(dup)
            continue;

          if(n_seen < SOUL_WX_ROWS_MAX)
            seen[n_seen++] = tg;

          if(tg->n_alerts < SOUL_WX_CUE_ALERTS_MAX)
          {
            tg->pts   [tg->n_alerts] = pt;
            tg->alerts[tg->n_alerts] = ai;
            tg->n_alerts++;
          }

          tg->total++;
        }
      }
    }

    if(capped > 0)
      clam(CLAM_INFO, SOUL_CTX,
          "bot=%s weather cap: %u fresh alert(s) claimed but unspoken"
          " (max_per_hour)", sweep->bot_name, capped);

    for(uint8_t i = 0; i < n_targets; i++)
      soul_wx_announce(st, method, &targets[i], now);
  }

  else
    clam(CLAM_DEBUG, SOUL_CTX,
        "bot=%s weather sweep dropped (bot gone, chat off, or muted)",
        sweep->bot_name);

  // Purge, every sweep: a row whose alert expired two days ago can
  // never match an active id again, so it buys no dedup.
  (void)soul_db_exec("DELETE FROM chat_soul_alerts_seen"
      " WHERE expires < NOW() - INTERVAL '2 days'");

  soul_chore_done(sweep->sched, sweep->chore);
  mem_free(sweep);
  t->state = TASK_ENDED;
}

// Whoever drops `pending` to zero calls this exactly once.
static void
soul_wx_sweep_finish(soul_wx_sweep_t *sweep)
{
  if(task_add(SOUL_CTX, TASK_ANY, 200, soul_wx_batch_task, sweep) != NULL)
    return;

  // No worker will ever run the batch: release the chore and the sweep
  // here or the watch never runs again. Nothing was claimed yet, so
  // the next sweep simply re-fetches and announces late.
  clam(CLAM_WARN, SOUL_CTX, "bot=%s weather batch spawn failed — dropped",
      sweep->bot_name);
  soul_chore_done(sweep->sched, sweep->chore);
  mem_free(sweep);
}

// Curl-worker soil: copy, count down, get out (weathergov_api.h — do
// not block here). The struct copy is the whole hand-off; the batch
// task reads it after the release-fence of the final decrement.
static void
soul_wx_fetch_cb(const weathergov_alert_result_t *res, void *user)
{
  soul_wx_point_t *pt = user;

  pt->result = *res;
  pt->got    = true;

  if(__atomic_sub_fetch(&pt->sweep->pending, 1, __ATOMIC_ACQ_REL) == 0)
    soul_wx_sweep_finish(pt->sweep);
}

// The chore body: scan, geocode, dedupe, fan out. Runs on the tick's
// worker thread (TASK_THREAD — the sync geocoders demand a worker,
// fact 9). Returns true once the sweep is airborne: from that moment
// the in-flight flag belongs to the completion path.
static bool
soul_chore_weather(soul_sched_t *s, uint32_t chore,
    chatbot_state_t *st, bot_inst_t *bot)
{
  soul_wx_sweep_t *sweep;
  db_result_t     *res;
  uint32_t         rows;
  char             sql[1024];
  char             key[KV_KEY_SZ];

  (void)st;
  (void)bot;

  snprintf(key, sizeof(key), "bot.%s.behavior.soul.weather.enabled",
      s->bot_name);

  if(kv_get_uint(key) == 0)
    return(false);

  if(!soul_wx_resolve())
  {
    clam(CLAM_DEBUG, SOUL_CTX,
        "bot=%s weather watch idle (weather providers not loaded)",
        s->bot_name);
    return(false);
  }

  if(!soul_wx_enabled_fn())
  {
    clam(CLAM_DEBUG, SOUL_CTX,
        "bot=%s weather watch idle (weathergov disabled)", s->bot_name);
    return(false);
  }

  // The watch list (fact 7): each dossier's NEWEST city_of_interest
  // fact — a user who moved their attention watches one sky, their
  // current one — with the newest signature as the deliverable
  // identity and the fact's channel as the delivery room.
  snprintf(sql, sizeof(sql),
      "SELECT DISTINCT ON (df.dossier_id) df.fact_value, df.channel,"
      " d.display_label, COALESCE(sg.nickname,''),"
      " COALESCE(sg.username,''), COALESCE(sg.hostname,''),"
      " COALESCE(sg.verified_id,'')"
      " FROM dossier_facts df"
      " JOIN dossier d ON d.id = df.dossier_id"
      " LEFT JOIN LATERAL (SELECT nickname, username, hostname,"
      "  verified_id FROM dossier_signature"
      "  WHERE dossier_id = df.dossier_id"
      "  ORDER BY last_seen DESC LIMIT 1) sg ON TRUE"
      " WHERE d.ns_id = %" PRIu32
      " AND df.fact_key LIKE 'city_of_interest:%%'"
      " ORDER BY df.dossier_id, df.last_seen DESC"
      " LIMIT %d",
      s->ns_id, SOUL_WX_ROWS_MAX);

  res = db_result_alloc();

  if(res == NULL || db_query(sql, res) != SUCCESS || !res->ok)
  {
    if(res != NULL)
      clam(CLAM_WARN, SOUL_CTX, "watch scan failed: %s",
          res->error[0] != '\0' ? res->error : "(no driver error)");

    db_result_free(res);
    return(false);
  }

  rows = res->rows;

  if(rows == 0)
  {
    db_result_free(res);
    return(false);
  }

  sweep = mem_alloc("chat", "soul_wx_sweep", sizeof(*sweep));

  if(sweep == NULL)
  {
    db_result_free(res);
    return(false);
  }

  memset(sweep, 0, sizeof(*sweep));
  sweep->sched = s;
  sweep->chore = chore;
  sweep->ns_id = s->ns_id;
  snprintf(sweep->bot_name, sizeof(sweep->bot_name), "%s", s->bot_name);

  for(uint32_t i = 0; i < rows; i++)
  {
    soul_wx_point_t   *pt = NULL;
    soul_wx_watcher_t *w;
    double             lat;
    double             lon;
    char               city[128];
    char               label[METHOD_NICKNAME_SZ];
    char               pkey[32];

    soul_copy_col(city,  sizeof(city),  res, i, 0);
    soul_copy_col(label, sizeof(label), res, i, 3);

    // Signature nickname first, dossier label as fallback; a row with
    // neither is unaddressable and skipped whole.
    if(label[0] == '\0')
      soul_copy_col(label, sizeof(label), res, i, 2);

    if(city[0] == '\0' || label[0] == '\0')
      continue;

    // Geocode on this worker (fact 9; the service's caches absorb the
    // repeats). A fact that no longer resolves is skipped quietly —
    // it was written only after a successful geocode, but facts age.
    if(soul_wx_geocode(city, &lat, &lon) != SUCCESS)
    {
      clam(CLAM_DEBUG, SOUL_CTX,
          "bot=%s watch geocode '%s' FAIL — skipped", s->bot_name, city);
      continue;
    }

    // ≤4 decimals is the API's own resolution (§WXG-TRUTH 4), which
    // makes the rounded string the natural coordinate identity: one
    // fetch per point however many users share it.
    snprintf(pkey, sizeof(pkey), "%.4f,%.4f", lat, lon);

    for(uint8_t p = 0; p < sweep->n_points; p++)
      if(strcmp(sweep->points[p].key, pkey) == 0)
      {
        pt = &sweep->points[p];
        break;
      }

    if(pt == NULL)
    {
      if(sweep->n_points >= SOUL_WX_POINTS_MAX)
        continue;

      pt = &sweep->points[sweep->n_points++];
      pt->sweep = sweep;
      pt->lat   = lat;
      pt->lon   = lon;
      snprintf(pt->key, sizeof(pt->key), "%s", pkey);
    }

    if(pt->n_watchers >= SOUL_WX_WATCHERS_MAX)
      continue;

    w = &pt->watchers[pt->n_watchers++];
    snprintf(w->label, sizeof(w->label), "%s", label);
    snprintf(w->city,  sizeof(w->city),  "%s", city);
    soul_copy_col(w->channel,     sizeof(w->channel),     res, i, 1);
    soul_copy_col(w->nickname,    sizeof(w->nickname),    res, i, 3);
    soul_copy_col(w->username,    sizeof(w->username),    res, i, 4);
    soul_copy_col(w->hostname,    sizeof(w->hostname),    res, i, 5);
    soul_copy_col(w->verified_id, sizeof(w->verified_id), res, i, 6);
    snprintf(w->sender, sizeof(w->sender), "%s", label);
  }

  db_result_free(res);

  if(sweep->n_points == 0)
  {
    mem_free(sweep);
    return(false);
  }

  clam(CLAM_DEBUG, SOUL_CTX, "bot=%s weather sweep: %u point(s)",
      s->bot_name, sweep->n_points);

  // `pending` covers the WHOLE fan before the first submit, so an
  // instant completion cannot zero it while later submits are still
  // being issued. A refused submit takes its own decrement here — its
  // callback will never fire. After the first successful submit the
  // sweep may be freed at any moment by the completion path; the
  // atomic is the only field this loop may still touch.
  sweep->pending = sweep->n_points;

  for(uint8_t p = 0; p < sweep->n_points; p++)
  {
    soul_wx_point_t *pt = &sweep->points[p];

    if(soul_wx_alerts_fn(pt->lat, pt->lon, soul_wx_fetch_cb, pt)
        == SUCCESS)
      continue;

    if(__atomic_sub_fetch(&sweep->pending, 1, __ATOMIC_ACQ_REL) == 0)
      soul_wx_sweep_finish(sweep);
  }

  return(true);
}

// ---------- the tick ----------

static void
soul_tick_cb(task_t *t)
{
  soul_sched_t    *s = t->data;
  bot_inst_t      *bot;
  chatbot_state_t *st;
  bool             active;
  uint32_t         interval;
  time_t           now;
  char             bot_name[BOT_NAME_SZ];
  char             key[KV_KEY_SZ];

  pthread_mutex_lock(&soul_mutex);
  active = s->active && soul_ready;
  snprintf(bot_name, sizeof(bot_name), "%s", s->bot_name);
  pthread_mutex_unlock(&soul_mutex);

  // The latch pattern: an unscheduled bot's task stays parked and
  // no-ops each fire (see extract.c for why cancelling here is more
  // lifecycle than it is worth).
  if(!active)
  {
    t->state = TASK_ENDED;
    return;
  }

  // Live cadence: re-reading the knob every tick makes a KV change
  // take effect on the next fire — in-callback interval_ms mutation is
  // the blessed mechanism (acquire_reactive precedent).
  snprintf(key, sizeof(key), "bot.%s.behavior.soul.interval_secs",
      bot_name);
  interval = (uint32_t)kv_get_uint(key);

  if(interval == 0)                     interval = SOUL_INTERVAL_DEFAULT_SECS;
  if(interval < SOUL_INTERVAL_MIN_SECS) interval = SOUL_INTERVAL_MIN_SECS;

  t->interval_ms = interval * 1000U;

  // Gates, cheapest first. A failed gate ends the tick, not the task:
  // everything is re-examined fresh on the next fire.
  bot = bot_find(bot_name);

  if(bot == NULL || bot_get_state(bot) != BOT_RUNNING
      || (st = bot_get_handle(bot)) == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  snprintf(key, sizeof(key), "bot.%s.behavior.chat.enabled", bot_name);

  if(kv_get_uint(key) == 0)
  {
    t->state = TASK_ENDED;
    return;
  }

  // D9 — the shared hush gate. Chores skipped under mute are not
  // lost: durable work (a due deferred row) stays pending in the DB and
  // delivers on the first tick after the mute lapses.
  if(chatbot_mute_active(bot_name))
  {
    clam(CLAM_DEBUG, SOUL_CTX, "bot=%s tick skipped (muted)", bot_name);
    t->state = TASK_ENDED;
    return;
  }

  now = time(NULL);

  for(uint32_t i = 0; i < SOUL_CHORE_COUNT; i++)
  {
    const soul_chore_t *c       = &soul_chores[i];
    uint32_t            cadence = c->interval_secs;
    bool                run     = false;

    // A chore with a knob family may re-pace itself per bot; the
    // registry value is the default, the KV wins, the global minimum
    // still floors it. Read outside the mutex — kv locks internally.
    if(c->kv_suffix != NULL)
    {
      uint32_t v;

      snprintf(key, sizeof(key), "bot.%s.behavior.soul.%s.interval_secs",
          bot_name, c->kv_suffix);
      v = (uint32_t)kv_get_uint(key);

      if(v > 0)
        cadence = v < SOUL_INTERVAL_MIN_SECS ? SOUL_INTERVAL_MIN_SECS : v;
    }

    pthread_mutex_lock(&soul_mutex);

    if(s->in_flight[i])
      clam(CLAM_DEBUG, SOUL_CTX,
          "bot=%s chore=%s still in flight — skipped", bot_name, c->name);

    else if(cadence > 0)
    {
      time_t last = s->last_ran[i];

      // Reload zeroes this stamp; floor it to handle creation so a
      // fresh mapping waits one window instead of reading "never ran"
      // as "due now" (the volunteer floor idiom — durable dedup state
      // stays in the DB, never here).
      if(last < st->created_at)
        last = st->created_at;

      if(now - last >= (time_t)cadence)
        run = true;
    }

    else
      run = true;

    if(run)
    {
      s->in_flight[i] = true;
      s->last_ran[i]  = now;
    }

    pthread_mutex_unlock(&soul_mutex);

    if(!run)
      continue;

    // A chore returning true has handed its work to an async
    // completion path, which owns the in-flight flag until it calls
    // soul_chore_done(); false means it finished with this tick.
    if(!c->fn(s, i, st, bot))
    {
      pthread_mutex_lock(&soul_mutex);
      s->in_flight[i] = false;
      pthread_mutex_unlock(&soul_mutex);
    }
  }

  t->state = TASK_ENDED;
}

// ---------- schedule / lifecycle ----------

void
soul_schedule(const char *bot_name, uint32_t ns_id, uint32_t interval_secs)
{
  bool          need_task;
  soul_sched_t *s;

  if(!soul_ready || bot_name == NULL || bot_name[0] == '\0')
    return;

  if(interval_secs == 0)
    interval_secs = SOUL_INTERVAL_DEFAULT_SECS;

  if(interval_secs < SOUL_INTERVAL_MIN_SECS)
    interval_secs = SOUL_INTERVAL_MIN_SECS;

  pthread_mutex_lock(&soul_mutex);
  s = soul_sched_find_locked(bot_name);

  if(s == NULL)
  {
    s = mem_alloc("chat", "soul_sched", sizeof(*s));

    if(s == NULL)
    {
      pthread_mutex_unlock(&soul_mutex);
      return;
    }

    memset(s, 0, sizeof(*s));
    snprintf(s->bot_name, sizeof(s->bot_name), "%s", bot_name);
    s->next         = soul_sched_head;
    soul_sched_head = s;
  }

  s->ns_id  = ns_id;
  s->active = true;

  // If a task already exists for this bot, it re-reads everything on
  // its next fire; otherwise spawn one. First fire is immediate by
  // task_add_periodic contract.
  need_task = (s->task == TASK_HANDLE_NONE);
  pthread_mutex_unlock(&soul_mutex);

  if(need_task)
  {
    char          tname[64];
    task_handle_t t;

    snprintf(tname, sizeof(tname), "soul.%.32s", bot_name);

    t = task_add_periodic(tname, TASK_THREAD, 200,
        interval_secs * 1000U, soul_tick_cb, s);

    pthread_mutex_lock(&soul_mutex);
    s->task = t;
    pthread_mutex_unlock(&soul_mutex);

    clam(CLAM_INFO, SOUL_CTX, "bot '%s' heartbeat every %u s",
        bot_name, interval_secs);
  }
}

void
soul_unschedule(const char *bot_name)
{
  soul_sched_t *s;

  if(bot_name == NULL)
    return;

  pthread_mutex_lock(&soul_mutex);
  s = soul_sched_find_locked(bot_name);

  if(s != NULL)
    s->active = false;

  pthread_mutex_unlock(&soul_mutex);

  if(s != NULL)
    clam(CLAM_INFO, SOUL_CTX, "bot '%s' heartbeat latched off", bot_name);
}

void
soul_stop(void)
{
  uint32_t cancelled = 0;

  pthread_mutex_lock(&soul_mutex);

  for(soul_sched_t *s = soul_sched_head; s != NULL; s = s->next)
  {
    s->active = false;

    if(s->task == TASK_HANDLE_NONE)
      continue;

    task_cancel(s->task);
    s->task = TASK_HANDLE_NONE;
    cancelled++;
  }

  pthread_mutex_unlock(&soul_mutex);

  if(cancelled > 0)
    clam(CLAM_DEBUG, SOUL_CTX, "cancelled %u heartbeat(s)", cancelled);
}

void
soul_exit(void)
{
  soul_sched_t *s;

  if(!soul_ready)
    return;

  soul_ready = false;

  // Free the sched list. Each entry is a live task's `data`, so
  // soul_stop() must already have cancelled them.
  pthread_mutex_lock(&soul_mutex);
  s               = soul_sched_head;
  soul_sched_head = NULL;

  while(s != NULL)
  {
    soul_sched_t *next = s->next;

    mem_free(s);
    s = next;
  }

  pthread_mutex_unlock(&soul_mutex);
}

// The soul's own durable state — the weather watch's alert ledger.
// Deferred work lives in chat_deferred and deferred.c raises it. The
// chat DDL discipline (memory_ensure_tables): owner-run idempotent
// batches at plugin start(), after dossier_register_config so the
// dossier(id) FK target exists.
void
soul_ensure_schema(void)
{
  // The weather watch's dedup ledger (D8): one row per alert per
  // namespace, written by the claim INSERT — which is why the primary
  // key IS the claim. Purged two days past expiry, every sweep.
  (void)soul_db_exec(
      "CREATE TABLE IF NOT EXISTS chat_soul_alerts_seen ("
      " alert_id     VARCHAR(200) NOT NULL,"
      " ns_id        INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " announced_at TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " expires      TIMESTAMPTZ,"
      " PRIMARY KEY(alert_id, ns_id)"
      ")");
}
