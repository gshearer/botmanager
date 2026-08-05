// botmanager — MIT
// The Reachy Mini's command surface: `/bot <name> …` moves the robot a
// bot owns and `/show bot <name> robot|moves` reports on it. Every verb
// is one async hop into the reachyapi service and one line back, so the
// whole file is a deep-copied command context, a renderer, and a table
// of small command bodies. No state survives a request — what survives
// is the bot's own KV, which the verbs write before they touch the
// robot.
#define REACHYCMD_INTERNAL
#include "reachycmd.h"

#include <stdlib.h>
#include <strings.h>  // strcasecmp / strncasecmp

// ----------------------------------------------------------------------
// Bot scope and instance KV
// ----------------------------------------------------------------------

// Every verb below is bot-scoped: the dispatcher resolves <name> and
// hands us the instance in ctx->bot. What it does NOT check is whether
// that bot has a robot — `/bot hedgehogg volume 85` resolves fine and
// means nothing. Refuse it here, and hand back the instance KV prefix
// while we are at it.
static bool
reachycmd_bot_prefix(const cmd_ctx_t *ctx, char *out, size_t cap)
{
  const char *name;
  char        line[REACHYCMD_LINE_SZ];

  if(ctx->bot == NULL)
  {
    cmd_reply(ctx, "reachy: this verb belongs to a bot — say "
        "`/bot <name> ...`, and the bot needs the reachy method bound.");
    return(FAIL);
  }

  name = bot_inst_name(ctx->bot);

  if(!bot_has_method_kind(ctx->bot, REACHYCMD_METHOD_KIND))
  {
    snprintf(line, sizeof(line), "reachy: %s has no robot — give it one "
        "with `/bot %s addmethod reachy`.", name, name);
    cmd_reply(ctx, line);
    return(FAIL);
  }

  snprintf(out, cap, "bot.%s." REACHYCMD_METHOD_KIND ".", name);

  return(SUCCESS);
}

// kv_get_str hands back internal storage only valid until the value
// changes, so a read copies out immediately.
static void
reachycmd_kv_copy(const char *prefix, const char *suffix, char *out,
    size_t cap)
{
  char        key[KV_KEY_SZ];
  const char *val;

  snprintf(key, sizeof(key), "%s%s", prefix, suffix);
  val = kv_get_str(key);

  snprintf(out, cap, "%s", val != NULL ? val : "");
}

// The KV is the control surface and the robot is a cache of it, so a
// write that fails must stop the verb dead: applying to the robot alone
// would look like it worked and be reverted by the next connect.
//
// One string-valued setter covers rows of both types the verbs touch —
// kv_set() parses against whatever type the key was registered with, so
// a KV_UINT8 out of range and a key nobody registered fail the same way
// and in the same place.
static bool
reachycmd_kv_write(const cmd_ctx_t *ctx, const char *prefix,
    const char *suffix, const char *val)
{
  char key[KV_KEY_SZ];
  char line[REACHYCMD_LINE_SZ];

  snprintf(key, sizeof(key), "%s%s", prefix, suffix);

  if(kv_set(key, val) == SUCCESS)
    return(SUCCESS);

  snprintf(line, sizeof(line), CLR_RED "reachy" CLR_RESET
      ": could not save %s — the robot was left alone.", key);
  cmd_reply(ctx, line);

  return(FAIL);
}

// ----------------------------------------------------------------------
// Holding a reply path across the async hop
// ----------------------------------------------------------------------

// The framework frees its own cmd_ctx_t the moment the command body
// returns, so a completion that fires later needs its own copy of both
// the context and the message it points at. args/username/parsed point
// into buffers that die with the original; NULL them rather than leave
// them dangling.
static void
reachycmd_hold_save(reachycmd_hold_t *h, const cmd_ctx_t *ctx)
{
  h->ctx = *ctx;

  if(ctx->msg != NULL)
    h->msg = *ctx->msg;

  h->ctx.msg      = &h->msg;
  h->ctx.args     = NULL;
  h->ctx.username = NULL;
  h->ctx.parsed   = NULL;
  h->ctx.data     = NULL;
}

static reachycmd_act_t *
reachycmd_act_new(const cmd_ctx_t *ctx, const char *what)
{
  reachycmd_act_t *a = mem_alloc(REACHYCMD_CTX, "act", sizeof(*a));

  memset(a, 0, sizeof(*a));
  reachycmd_hold_save(&a->hold, ctx);
  snprintf(a->what, sizeof(a->what), "%s", what);

  return(a);
}

// ----------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------

// What a failed hop means depends on whether the verb wrote its intent
// down first: a saved setting is applied again at the next connect, so
// the operator has lost the moment and nothing else.
static const char *
reachycmd_act_tail(const reachycmd_act_t *a)
{
  if(!a->saved)
    return("");

  return("; it is saved, and applied when the robot next connects");
}

// Shared completion for every actuating verb. `what` is a present
// participle ("playing cheerful1"), which reads correctly on its own
// after a success and in front of "failed" after anything else.
static void
reachycmd_act_done(const reachy_result_t *r)
{
  reachycmd_act_t *a = (reachycmd_act_t *)r->user_data;
  char             line[REACHYCMD_LINE_SZ];

  if(r->status == REACHY_OK)
    snprintf(line, sizeof(line), CLR_GREEN "reachy" CLR_RESET ": %s",
        a->what);

  else if(r->status == REACHY_HTTP)
    snprintf(line, sizeof(line),
        CLR_RED "reachy" CLR_RESET ": %s failed — %s (http %ld)%s",
        a->what, reachy_status_str(r->status), r->http,
        reachycmd_act_tail(a));

  else
    snprintf(line, sizeof(line),
        CLR_RED "reachy" CLR_RESET ": %s failed — %s%s",
        a->what, reachy_status_str(r->status), reachycmd_act_tail(a));

  cmd_reply(&a->hold.ctx, line);
  mem_free(a);
}

// A FAIL return means the request never left this process and the
// completion above will never run — so this is the only other place an
// actuating verb can end.
static void
reachycmd_act_refused(reachycmd_act_t *a)
{
  char line[REACHYCMD_LINE_SZ];

  snprintf(line, sizeof(line), CLR_RED "reachy" CLR_RESET
      ": %s was refused before it left — bad argument, or "
      "plugin.reachyapi.base_url is unset%s", a->what,
      reachycmd_act_tail(a));
  cmd_reply(&a->hold.ctx, line);
  mem_free(a);
}

// ----------------------------------------------------------------------
// show bot <name> moves
// ----------------------------------------------------------------------

// Case-insensitive substring test. Deliberately not strcasestr: that is
// a GNU extension and this tree does not define _GNU_SOURCE.
static bool
reachycmd_matches(const char *name, const char *filter)
{
  size_t flen;

  if(filter[0] == '\0')
    return(true);

  flen = strlen(filter);

  for(size_t i = 0; name[i] != '\0'; i++)
    if(strncasecmp(name + i, filter, flen) == 0)
      return(true);

  return(false);
}

// Append one padded cell, tracking the true length so the trailing
// padding can be trimmed before the line is sent.
static void
reachycmd_grid_push(char *line, size_t cap, size_t *len, const char *name)
{
  int wrote;

  if(*len + 1 >= cap)
    return;

  wrote = snprintf(line + *len, cap - *len, "%-*s",
      REACHYCMD_LIST_WIDTH, name);

  if(wrote > 0)
    *len += (size_t)wrote < cap - *len ? (size_t)wrote : cap - *len - 1;
}

static void
reachycmd_grid_flush(reachycmd_hold_t *h, char *line, size_t *len)
{
  while(*len > 0 && line[*len - 1] == ' ')
    line[--(*len)] = '\0';

  if(*len > 0)
    cmd_reply(&h->ctx, line);

  *len = 0;
}

static void
reachycmd_list_done(const reachy_moves_t *r)
{
  reachycmd_list_t *l = (reachycmd_list_t *)r->user_data;
  char              line[REACHYCMD_LINE_SZ];
  size_t            len  = 0;
  size_t            col  = 0;
  size_t            kept = 0;

  if(r->status != REACHY_OK)
  {
    snprintf(line, sizeof(line), CLR_RED "reachy" CLR_RESET
        ": could not read the move library — %s",
        reachy_status_str(r->status));
    cmd_reply(&l->hold.ctx, line);
    mem_free(l);
    return;
  }

  for(size_t i = 0; i < r->n_moves; i++)
  {
    if(!reachycmd_matches(r->moves[i], l->filter))
      continue;

    reachycmd_grid_push(line, sizeof(line), &len, r->moves[i]);
    kept++;

    if(++col < REACHYCMD_LIST_COLS)
      continue;

    reachycmd_grid_flush(&l->hold, line, &len);
    col = 0;
  }

  reachycmd_grid_flush(&l->hold, line, &len);

  if(kept == 0)
    snprintf(line, sizeof(line),
        "reachy: nothing in the library matches \"%s\" (%zu moves)",
        l->filter, r->n_moves);

  else if(l->filter[0] != '\0')
    snprintf(line, sizeof(line), CLR_GRAY
        "%zu of %zu moves match \"%s\"" CLR_RESET, kept, r->n_moves,
        l->filter);

  else
    snprintf(line, sizeof(line), CLR_GRAY
        "%zu moves — play one with /bot %s do <move>" CLR_RESET, kept,
        l->bot);

  cmd_reply(&l->hold.ctx, line);
  mem_free(l);
}

// ----------------------------------------------------------------------
// show bot <name> robot
// ----------------------------------------------------------------------

// `doa` NULL means the second hop never happened; everything else on
// the card came back and is still worth printing.
static void
reachycmd_show_render(reachycmd_show_t *s, const reachy_doa_t *doa)
{
  const reachy_robot_status_t *rb = &s->robot;
  char                         line[REACHYCMD_LINE_SZ];

  snprintf(line, sizeof(line),
      CLR_BOLD "reachy" CLR_RESET "  %s%s" CLR_RESET "  "
      CLR_GRAY "daemon %s" CLR_RESET,
      strcmp(rb->state, "running") == 0 ? CLR_GREEN : CLR_YELLOW,
      rb->state[0] != '\0' ? rb->state : "unknown",
      rb->version[0] != '\0' ? rb->version : "?");
  cmd_reply(&s->hold.ctx, line);

  snprintf(line, sizeof(line), "  " CLR_GRAY "media  " CLR_RESET "%s",
      rb->no_media        ? "off — the daemon was started without it"
      : rb->media_released ? "released to other processes on the robot"
                           : "held by the daemon (camera and mic live)");
  cmd_reply(&s->hold.ctx, line);

  // The one line that explains a robot which "ignores" you: with the
  // motors off it accepts every move, answers normally and does not
  // stir. Worth stating in words rather than leaving as a mode name.
  snprintf(line, sizeof(line), "  " CLR_GRAY "motors " CLR_RESET
      "%s%s" CLR_RESET,
      strcmp(rb->motors, "enabled") == 0 ? CLR_GREEN : CLR_YELLOW,
      rb->motors[0] == '\0'                    ? "unknown"
      : strcmp(rb->motors, "enabled")  == 0    ? "on — holding its pose"
      : strcmp(rb->motors, "disabled") == 0    ? "off — limp; moves will "
                                                 "play but not move it"
                                               : rb->motors);
  cmd_reply(&s->hold.ctx, line);

  if(rb->face_detected)
    snprintf(line, sizeof(line), "  " CLR_GRAY "face   " CLR_RESET
        CLR_GREEN "tracking" CLR_RESET " at x %+.2f  y %+.2f",
        rb->face_x, rb->face_y);

  else
    snprintf(line, sizeof(line), "  " CLR_GRAY "face   " CLR_RESET
        "nobody in view");

  cmd_reply(&s->hold.ctx, line);

  if(doa == NULL || doa->status != REACHY_OK)
    snprintf(line, sizeof(line), "  " CLR_GRAY "hearing" CLR_RESET
        " unavailable");

  else
    snprintf(line, sizeof(line), "  " CLR_GRAY "hearing" CLR_RESET
        " %.0f° · %s", doa->angle * 180.0 / M_PI,
        doa->speech ? CLR_GREEN "speech" CLR_RESET : "silence");

  cmd_reply(&s->hold.ctx, line);
}

static void
reachycmd_show_doa_done(const reachy_doa_t *doa)
{
  reachycmd_show_t *s = (reachycmd_show_t *)doa->user_data;

  reachycmd_show_render(s, doa);
  mem_free(s);
}

static void
reachycmd_show_status_done(const reachy_robot_status_t *st)
{
  reachycmd_show_t *s = (reachycmd_show_t *)st->user_data;
  char              line[REACHYCMD_LINE_SZ];

  s->robot = *st;

  if(st->status != REACHY_OK)
  {
    snprintf(line, sizeof(line), CLR_RED "reachy" CLR_RESET
        ": no answer from the robot — %s", reachy_status_str(st->status));
    cmd_reply(&s->hold.ctx, line);
    mem_free(s);
    return;
  }

  // Chaining a second submit from the curl worker thread is fine —
  // curl_request_submit is thread-safe from everywhere. Only the
  // blocking submit_wait variant is off-limits here.
  if(reachy_get_doa(reachycmd_show_doa_done, s) == SUCCESS)
    return;

  reachycmd_show_render(s, NULL);
  mem_free(s);
}

// ----------------------------------------------------------------------
// say — synthesize, upload, play
// ----------------------------------------------------------------------

// Every failure arm ends here: one line, then the context dies. Each
// hop names its own stage, because "it did not speak" has three quite
// different causes and the operator fixes each somewhere else.
static void
reachycmd_say_fail(reachycmd_say_t *s, const char *what)
{
  char line[REACHYCMD_LINE_SZ];

  snprintf(line, sizeof(line), CLR_RED "reachy" CLR_RESET ": %s", what);
  cmd_reply(&s->hold.ctx, line);
  mem_free(s);
}

static void
reachycmd_say_played(const reachy_result_t *r)
{
  reachycmd_say_t *s = (reachycmd_say_t *)r->user_data;
  char             line[REACHYCMD_LINE_SZ];

  if(r->status != REACHY_OK)
  {
    char reason[REACHYCMD_REASON_SZ];

    snprintf(reason, sizeof(reason), "the audio reached the robot but it "
        "would not play it — %s", reachy_status_str(r->status));
    reachycmd_say_fail(s, reason);
    return;
  }

  snprintf(line, sizeof(line),
      CLR_GREEN "reachy" CLR_RESET ": \"%s\"", s->text);
  cmd_reply(&s->hold.ctx, line);
  mem_free(s);
}

static void
reachycmd_say_uploaded(const reachy_result_t *r)
{
  reachycmd_say_t *s = (reachycmd_say_t *)r->user_data;
  char             reason[REACHYCMD_REASON_SZ];

  if(r->status != REACHY_OK)
  {
    snprintf(reason, sizeof(reason),
        "the robot would not take the audio — %s (http %ld)",
        reachy_status_str(r->status), r->http);
    reachycmd_say_fail(s, reason);
    return;
  }

  if(reachy_play_sound(REACHYCMD_SAY_FILE, reachycmd_say_played, s)
      != SUCCESS)
    reachycmd_say_fail(s, "the audio is on the robot but playing it was "
        "refused before it left");
}

// The engine hands the WAV over for the duration of this callback only,
// and reachy_upload_sound copies it into the multipart body before it
// returns — so the bytes are safe to pass straight through, and there is
// no window where anyone owns a copy of them but us.
static void
reachycmd_say_synthesized(const llm_tts_response_t *r)
{
  reachycmd_say_t *s = (reachycmd_say_t *)r->user_data;
  char             reason[REACHYCMD_REASON_SZ];

  if(!r->ok || r->bytes_len == 0)
  {
    snprintf(reason, sizeof(reason), "could not find its voice — %.256s",
        r->error != NULL && r->error[0] != '\0'
            ? r->error : "the speech host answered with no audio");
    reachycmd_say_fail(s, reason);
    return;
  }

  if(reachy_upload_sound(REACHYCMD_SAY_FILE, r->bytes, r->bytes_len,
      reachycmd_say_uploaded, s) != SUCCESS)
    reachycmd_say_fail(s, "the speech was synthesized but sending it to "
        "the robot was refused before it left");
}

// ----------------------------------------------------------------------
// Argument helpers
// ----------------------------------------------------------------------

static bool
reachycmd_flag(const char *s, bool *out)
{
  if(s == NULL)
    return(FAIL);

  if(strcasecmp(s, "on") == 0 || strcasecmp(s, "true") == 0
      || strcasecmp(s, "yes") == 0 || strcasecmp(s, "enable") == 0
      || strcmp(s, "1") == 0)
  {
    *out = true;
    return(SUCCESS);
  }

  if(strcasecmp(s, "off") == 0 || strcasecmp(s, "false") == 0
      || strcasecmp(s, "no") == 0 || strcasecmp(s, "disable") == 0
      || strcmp(s, "0") == 0)
  {
    *out = false;
    return(SUCCESS);
  }

  return(FAIL);
}

static const char *
reachycmd_arg(const cmd_ctx_t *ctx, uint8_t i)
{
  if(ctx->parsed == NULL || i >= ctx->parsed->argc)
    return(NULL);

  return(ctx->parsed->argv[i]);
}

// ----------------------------------------------------------------------
// Command bodies
// ----------------------------------------------------------------------

static void
reachycmd_list(const cmd_ctx_t *ctx)
{
  const char       *filter = reachycmd_arg(ctx, 0);
  reachycmd_list_t *l;
  char              prefix[REACHYCMD_PREFIX_SZ];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  l = mem_alloc(REACHYCMD_CTX, "list", sizeof(*l));
  memset(l, 0, sizeof(*l));
  reachycmd_hold_save(&l->hold, ctx);
  snprintf(l->bot, sizeof(l->bot), "%s", bot_inst_name(ctx->bot));

  if(filter != NULL)
    snprintf(l->filter, sizeof(l->filter), "%s", filter);

  if(reachy_list_moves(reachycmd_list_done, l) != SUCCESS)
  {
    cmd_reply(ctx, "reachy: cannot ask the robot for its move library "
        "(is plugin.reachyapi.base_url set?)");
    mem_free(l);
  }
}

static void
reachycmd_do(const cmd_ctx_t *ctx)
{
  const char      *move = ctx->parsed->argv[0];
  reachycmd_act_t *a;
  char             what[REACHY_MOVE_NAME_SZ + 32];
  char             prefix[REACHYCMD_PREFIX_SZ];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  snprintf(what, sizeof(what), "playing %s", move);
  a = reachycmd_act_new(ctx, what);

  if(reachy_play_move(move, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

// The voice is the bot's, not the plugin's: two creatures sharing one
// body should not have to share a throat.
static void
reachycmd_say(const cmd_ctx_t *ctx)
{
  llm_tts_params_t params;
  reachycmd_say_t *s;
  char             prefix[REACHYCMD_PREFIX_SZ];
  char             model[64];
  char             voice[64];
  char             speed[32];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  reachycmd_kv_copy(prefix, REACHYCMD_KV_TTS_MODEL, model, sizeof(model));
  reachycmd_kv_copy(prefix, REACHYCMD_KV_TTS_VOICE, voice, sizeof(voice));
  reachycmd_kv_copy(prefix, REACHYCMD_KV_TTS_SPEED, speed, sizeof(speed));

  if(model[0] == '\0')
  {
    char line[REACHYCMD_LINE_SZ];

    snprintf(line, sizeof(line), "reachy: %s has no voice — set "
        "%s" REACHYCMD_KV_TTS_MODEL " to a registered tts model.",
        bot_inst_name(ctx->bot), prefix);
    cmd_reply(ctx, line);
    return;
  }

  memset(&params, 0, sizeof(params));
  params.voice = voice[0] != '\0' ? voice : NULL;
  params.speed = strtod(speed, NULL);

  s = mem_alloc(REACHYCMD_CTX, "say", sizeof(*s));
  memset(s, 0, sizeof(*s));
  reachycmd_hold_save(&s->hold, ctx);
  snprintf(s->text, sizeof(s->text), "%s", ctx->parsed->argv[0]);

  if(llm_tts_submit(model, &params, s->text, reachycmd_say_synthesized, s)
      != SUCCESS)
  {
    char line[REACHYCMD_LINE_SZ];

    snprintf(line, sizeof(line), "reachy: the speech host would not take "
        "the request — is '%s' a registered, enabled tts model?", model);
    cmd_reply(ctx, line);
    mem_free(s);
  }
}

// Torque landed, so the body can be asked to move. Anything else and
// the shared renderer reports it against the same phrase — "waking the
// robot failed" is true of a wake that never got its motors on.
static void
reachycmd_wake_torque_done(const reachy_result_t *r)
{
  reachycmd_act_t *a = (reachycmd_act_t *)r->user_data;

  if(r->status != REACHY_OK)
  {
    reachycmd_act_done(r);
    return;
  }

  if(reachy_wake(reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

// Two hops, because the daemon only raises torque when it is asked to:
// wake_up alone plays the whole animation into a limp robot, reports
// success and moves nothing. Motors first, then the move.
static void
reachycmd_wake(const cmd_ctx_t *ctx)
{
  reachycmd_act_t *a;
  char             prefix[REACHYCMD_PREFIX_SZ];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  a = reachycmd_act_new(ctx, "waking the robot");

  if(reachy_motors_mode("enabled", reachycmd_wake_torque_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

// One hop, and deliberately so: goto_sleep ends by dropping torque
// itself (measured — mode goes `enabled` → `disabled` on its own about
// 2.5 s after the POST, with nothing else touching it). Disabling the
// motors from here would land while the head is still on its way down
// and drop it the rest of the way.
static void
reachycmd_sleep(const cmd_ctx_t *ctx)
{
  reachycmd_act_t *a;
  char             prefix[REACHYCMD_PREFIX_SZ];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  a = reachycmd_act_new(ctx, "putting the robot to sleep");

  if(reachy_sleep_move(reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

// The three setters below share one shape: validate, write the KV, then
// poke the robot. The order is the contract — the KV is what the driver
// re-applies on every connect, reload and restart, so a robot changed
// without it is a change that silently expires.
static void
reachycmd_volume(const cmd_ctx_t *ctx)
{
  unsigned long    pct = strtoul(ctx->parsed->argv[0], NULL, 10);
  reachycmd_act_t *a;
  char             what[REACHY_MOVE_NAME_SZ + 32];
  char             prefix[REACHYCMD_PREFIX_SZ];
  char             val[8];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  if(pct > 100)
  {
    cmd_reply(ctx, "reachy: volume runs from 0 to 100.");
    return;
  }

  snprintf(val, sizeof(val), "%lu", pct);

  if(reachycmd_kv_write(ctx, prefix, REACHYCMD_KV_VOLUME, val) != SUCCESS)
    return;

  snprintf(what, sizeof(what), "setting the volume to %lu", pct);
  a = reachycmd_act_new(ctx, what);
  a->saved = true;

  if(reachy_set_volume((uint8_t)pct, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

// `off` is stored as weight 0 rather than as a separate flag, which is
// what makes it durable: the driver reads one number and turns tracking
// on or off from its sign.
static void
reachycmd_track(const cmd_ctx_t *ctx)
{
  const char      *weight_s = reachycmd_arg(ctx, 1);
  reachycmd_act_t *a;
  char             what[REACHY_MOVE_NAME_SZ + 32];
  char             prefix[REACHYCMD_PREFIX_SZ];
  char             val[16];
  double           weight = REACHYCMD_TRACK_WEIGHT;
  bool             on;

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  if(reachycmd_flag(ctx->parsed->argv[0], &on) != SUCCESS)
  {
    cmd_reply(ctx, "Usage: /bot <name> track <on|off> [weight 0.0-1.0]");
    return;
  }

  if(weight_s != NULL)
  {
    char *end;

    weight = strtod(weight_s, &end);

    if(end == weight_s || *end != '\0' || weight < 0.0 || weight > 1.0)
    {
      cmd_reply(ctx, "reachy: the tracking weight runs from 0.0 to 1.0.");
      return;
    }
  }

  if(!on)
    weight = 0.0;

  snprintf(val, sizeof(val), "%.2f", weight);

  if(reachycmd_kv_write(ctx, prefix, REACHYCMD_KV_TRACKING, val) != SUCCESS)
    return;

  if(on)
    snprintf(what, sizeof(what), "following faces at weight %.2f", weight);

  else
    snprintf(what, sizeof(what), "%s", "letting its gaze go");

  a = reachycmd_act_new(ctx, what);
  a->saved = true;

  if(reachy_tracking(on, weight, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_wobble(const cmd_ctx_t *ctx)
{
  reachycmd_act_t *a;
  char             prefix[REACHYCMD_PREFIX_SZ];
  bool             on;

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  if(reachycmd_flag(ctx->parsed->argv[0], &on) != SUCCESS)
  {
    cmd_reply(ctx, "Usage: /bot <name> wobble <on|off>");
    return;
  }

  if(reachycmd_kv_write(ctx, prefix, REACHYCMD_KV_WOBBLE, on ? "1" : "0")
      != SUCCESS)
    return;

  a = reachycmd_act_new(ctx, on
      ? "nodding along to whatever it plays"
      : "holding its head still while it plays");
  a->saved = true;

  if(reachy_wobbling(on, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_show(const cmd_ctx_t *ctx)
{
  reachycmd_show_t *s;
  char              prefix[REACHYCMD_PREFIX_SZ];

  if(reachycmd_bot_prefix(ctx, prefix, sizeof(prefix)) != SUCCESS)
    return;

  s = mem_alloc(REACHYCMD_CTX, "show", sizeof(*s));
  memset(s, 0, sizeof(*s));
  reachycmd_hold_save(&s->hold, ctx);

  if(reachy_get_status(reachycmd_show_status_done, s) != SUCCESS)
  {
    cmd_reply(ctx, "reachy: cannot reach the robot "
        "(is plugin.reachyapi.base_url set?)");
    mem_free(s);
  }
}

// ----------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------

// CMD_ARG_NONE, not CMD_ARG_ALNUM: emotion-library names carry hyphens
// and underscores (toc-toc-toc, yes_sad1). The service validates the
// alphabet before it splices the name into a URL.
static const cmd_arg_desc_t reachycmd_do_args[] = {
  { "move", CMD_ARG_NONE, CMD_ARG_REQUIRED, REACHY_MOVE_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t reachycmd_list_args[] = {
  { "filter", CMD_ARG_NONE, CMD_ARG_OPTIONAL, REACHYCMD_FILTER_SZ - 1,
    NULL },
};

// CMD_ARG_REST: everything after the verb is the sentence, spaces and
// punctuation included. It must be the last argument, and it is the only
// one.
static const cmd_arg_desc_t reachycmd_say_args[] = {
  { "text", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST,
    REACHYCMD_SAY_SZ - 1, NULL },
};

static const cmd_arg_desc_t reachycmd_volume_args[] = {
  { "level", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 3, NULL },
};

static const cmd_arg_desc_t reachycmd_track_args[] = {
  { "state",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 8, NULL },
  { "weight", CMD_ARG_NONE,  CMD_ARG_OPTIONAL, 8, NULL },
};

static const cmd_arg_desc_t reachycmd_wobble_args[] = {
  { "state", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 8, NULL },
};

// The method kind that admits every verb below. Storage must be static
// and NUL-terminated: cmd_register keeps the pointer, and core walks the
// array to CMD_KIND_FILTER_MAX. A bot with this method bound is offered
// the verbs; every other bot never sees them — not in `/bot <name>`, not
// in `/help bot <name>`.
static const char *const reachycmd_kind_filter[] = {
  REACHYCMD_METHOD_KIND, NULL,
};

// Uniformly ADMIN at REACHYCMD_LEVEL — reading verbs as well as moving
// ones — because that is what the parent enforces and a registration
// must say what is enforced. Abbrevs are NULL throughout: an abbrev
// under `bot` competes tree-wide with every sibling whose filter
// overlaps, and `wake` and `wobble` would want the same letter.
static bool
reachycmd_register(void)
{
  if(cmd_register(REACHYCMD_CTX, "do",
        "bot <name> do <move>",
        "Play one of the robot's emotion moves.",
        "The move is played by the robot's own daemon, on its own clock; "
        "botman only names it and is told whether it started. Names come "
        "from `show bot <name> moves`. A name the library does not hold "
        "comes back as an http 404 rather than silence. The bot must "
        "have the reachy method bound; any other bot is refused by name.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_do, NULL, "bot", NULL,
        reachycmd_do_args,
        (uint8_t)(sizeof(reachycmd_do_args) / sizeof(reachycmd_do_args[0])),
        reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "say",
        "bot <name> say <text>",
        "Make the robot speak a line aloud.",
        "Three hops: the text is synthesized by the tts model named in "
        "bot.<name>.reachy.tts_model, the resulting WAV is uploaded to "
        "the robot, and the robot plays it. Turn `bot <name> wobble on` "
        "first and the head moves in time with the words. The voice and "
        "rate come from bot.<name>.reachy.tts_voice and .tts_speed — "
        "per bot, so two creatures sharing one body keep their own "
        "voices; the model row itself is registered with "
        "`llm add model tts ...`.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_say, NULL, "bot", NULL,
        reachycmd_say_args,
        (uint8_t)(sizeof(reachycmd_say_args)
                  / sizeof(reachycmd_say_args[0])),
        reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "wake",
        "bot <name> wake",
        "Bring the robot up out of rest.",
        "Enables the motors, then plays the daemon's wake_up move: the "
        "head rises, the body re-centres, the antennas come down. Both "
        "steps are needed — the daemon never raises torque on its own, "
        "and a limp robot plays the whole move without stirring. Safe to "
        "repeat; the motion takes about two and a half seconds.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_wake, NULL, "bot", NULL,
        NULL, 0, reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "sleep",
        "bot <name> sleep",
        "Settle the robot back into rest.",
        "Plays the daemon's goto_sleep move: the robot lowers itself "
        "into its shell over about two and a half seconds, and the "
        "daemon drops torque once it arrives. It is then limp, and only "
        "`bot <name> wake` will lift it again. `show bot <name> robot` "
        "reports which of the two states it is in.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_sleep, NULL, "bot", NULL,
        NULL, 0, reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "volume",
        "bot <name> volume <0-100>",
        "Set the robot's speaker level.",
        "Writes bot.<name>.reachy.volume and then applies it. The KV row "
        "is the control surface and the robot is a cache of it: the "
        "method driver re-applies that number on every connect, reload "
        "and restart, so this is the level the bot keeps rather than the "
        "level it happens to be at.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_volume, NULL, "bot", NULL,
        reachycmd_volume_args,
        (uint8_t)(sizeof(reachycmd_volume_args)
                  / sizeof(reachycmd_volume_args[0])),
        reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "track",
        "bot <name> track <on|off> [weight]",
        "Make the robot follow faces with its head.",
        "Face detection and the head motion that follows it both run on "
        "the robot; botman only switches them on. The weight is how "
        "strongly the head is pulled toward the face, from 0.0 to 1.0, "
        "and defaults to 0.6 — high enough to be obviously alive, low "
        "enough not to snap. It is stored in "
        "bot.<name>.reachy.tracking_weight, where `off` is simply 0, and "
        "re-applied whenever the bot connects.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_track, NULL, "bot", NULL,
        reachycmd_track_args,
        (uint8_t)(sizeof(reachycmd_track_args)
                  / sizeof(reachycmd_track_args[0])),
        reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "wobble",
        "bot <name> wobble <on|off>",
        "Let played audio drive the robot's head.",
        "With wobbling enabled, ANY sound the daemon plays moves the "
        "head in time with it. It costs nothing and it is what makes the "
        "robot look like it is speaking rather than broadcasting. Stored "
        "in bot.<name>.reachy.wobble and re-applied on every connect.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_wobble, NULL, "bot", NULL,
        reachycmd_wobble_args,
        (uint8_t)(sizeof(reachycmd_wobble_args)
                  / sizeof(reachycmd_wobble_args[0])),
        reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  // The two read-only views hang off `show/bot`, per feedback that
  // /show observes and /bot changes. Both report the LIVE robot; the
  // KV rows are what it will be told next time it connects, and the two
  // may legitimately disagree between a `set bot ...` and that connect.
  if(cmd_register(REACHYCMD_CTX, "robot",
        "show bot <name> robot",
        "What the bot's robot is doing right now.",
        "Two live reads in one card: the daemon's own status — its "
        "state, version, whether it still holds the camera and "
        "microphone, and the face it is currently tracking — and the "
        "microphone array's direction of arrival, which is the bearing "
        "it last heard a voice on and whether it is hearing one now. It "
        "also reports whether the motors are on at all, which is the "
        "difference between a robot ignoring you and one that "
        "physically cannot answer.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_show, NULL, "show/bot", NULL,
        NULL, 0, reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "moves",
        "show bot <name> moves [filter]",
        "The emotion moves the robot knows.",
        "Asks the robot's daemon for the move library it ships with — 84 "
        "recordings at daemon 1.9.0 — and prints them four to a line. A "
        "filter narrows the list to names containing it, case "
        "insensitively, which is the fast way to find every dance or "
        "every way of saying no. Play one with `bot <name> do <move>`.",
        USERNS_GROUP_ADMIN, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_list, NULL, "show/bot", NULL,
        reachycmd_list_args,
        (uint8_t)(sizeof(reachycmd_list_args)
                  / sizeof(reachycmd_list_args[0])),
        reachycmd_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, REACHYCMD_CTX,
      "robot verbs registered under bot and show bot");
  return(SUCCESS);
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static bool
reachycmd_init(void)
{
  // A command surface that cannot raise itself is worse than absent —
  // fail the init and let the loader skip the plugin.
  if(reachycmd_register() != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, REACHYCMD_CTX, "reachycmd plugin initialized");
  return(SUCCESS);
}

// Nine leaves under two parents this plugin does not own, so every one
// is named. There is no root to take children with it any more — and
// nothing stale may be left behind "just in case": an unresolved path is
// a silent no-op, so a leftover line would hide a real leak from
// `/plugin audit reachycmd` rather than guard against one.
static void
reachycmd_deinit(void)
{
  cmd_unregister_path("bot/do");
  cmd_unregister_path("bot/say");
  cmd_unregister_path("bot/wake");
  cmd_unregister_path("bot/sleep");
  cmd_unregister_path("bot/volume");
  cmd_unregister_path("bot/track");
  cmd_unregister_path("bot/wobble");
  cmd_unregister_path("show/bot/robot");
  cmd_unregister_path("show/bot/moves");
  clam(CLAM_INFO, REACHYCMD_CTX, "reachycmd plugin deinitialized");
}

// No plugin KV at all, by construction rather than by argument: a
// plugin-level row may only hold what is true of EVERY reachy under
// botmanager, and a voice, a volume and a gaze are true of one bot.
// They live in the method driver's instance KV, which is why this
// plugin now requires the driver as well as the service.
const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = REACHYCMD_CTX,
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = REACHYCMD_CTX,
  .provides        = { { .name = "misc_reachycmd" } },
  .provides_count  = 1,
  .requires        = { { .name = "service_reachyapi" },
                       { .name = "method_reachy" },
                       { .name = "bot_chat" },
                       { .name = "inference" } },
  .requires_count  = 4,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = reachycmd_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = reachycmd_deinit,
  .ext             = NULL,
};
