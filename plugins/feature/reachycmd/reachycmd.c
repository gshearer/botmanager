// botmanager — MIT
// The Reachy Mini's command surface: `!reachy` moves the robot and
// `show reachy` reports what it is doing. Every verb is one async hop
// into the reachyapi service and one line back, so the whole file is a
// deep-copied command context, a renderer, and a table of small command
// bodies. No state survives a request.
#define REACHYCMD_INTERNAL
#include "reachycmd.h"

#include <stdlib.h>
#include <strings.h>  // strcasecmp / strncasecmp

// ----------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------

// The voice `!reachy say` speaks in. Plugin-level rather than per-bot
// because this is a command surface and a command has no bot: when the
// method driver lands, a bound bot gets its own voice from instance KV
// and these stay the fallback for anyone typing at the robot directly.
//
// The defaults name the rows RCH-4 registers (`llm add model tts kokoro1
// senses-tts kokoro`) and the voice measured as sid 3 in the shipped
// Kokoro v1_0 model.
static const plugin_kv_entry_t reachycmd_kv_schema[] = {
  { REACHYCMD_KV_TTS_MODEL, KV_STR, "kokoro1",
    "Registered tts model `!reachy say` synthesizes through "
    "(`llm add model tts ...`); empty disables the verb" },
  { REACHYCMD_KV_TTS_VOICE, KV_STR, "af_heart",
    "Kokoro voice id; empty lets the speech host pick its own default" },
  { REACHYCMD_KV_TTS_SPEED, KV_STR, "1.0",
    "Synthesis rate multiplier — 1.0 is natural, below is slower" },
};

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
        CLR_RED "reachy" CLR_RESET ": %s failed — %s (http %ld)",
        a->what, reachy_status_str(r->status), r->http);

  else
    snprintf(line, sizeof(line),
        CLR_RED "reachy" CLR_RESET ": %s failed — %s",
        a->what, reachy_status_str(r->status));

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
      "plugin.reachyapi.base_url is unset", a->what);
  cmd_reply(&a->hold.ctx, line);
  mem_free(a);
}

// ----------------------------------------------------------------------
// !reachy list
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
        "%zu moves — play one with !reachy do <name>" CLR_RESET, kept);

  cmd_reply(&l->hold.ctx, line);
  mem_free(l);
}

// ----------------------------------------------------------------------
// show reachy
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
// !reachy say — synthesize, upload, play
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
reachycmd_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, CLR_BOLD "reachy" CLR_RESET
      " — the robot's body. What it can be told to do:");
  cmd_reply(ctx, "  list [filter] · do <move> · say <text> · wake · "
      "sleep · volume <0-100> · track <on|off> [weight] · wobble <on|off>");
  cmd_reply(ctx, CLR_GRAY
      "  `show reachy` reports what it is doing right now." CLR_RESET);
}

static void
reachycmd_list(const cmd_ctx_t *ctx)
{
  const char       *filter = reachycmd_arg(ctx, 0);
  reachycmd_list_t *l;

  l = mem_alloc(REACHYCMD_CTX, "list", sizeof(*l));
  memset(l, 0, sizeof(*l));
  reachycmd_hold_save(&l->hold, ctx);

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

  snprintf(what, sizeof(what), "playing %s", move);
  a = reachycmd_act_new(ctx, what);

  if(reachy_play_move(move, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_say(const cmd_ctx_t *ctx)
{
  const char      *model = kv_get_str(REACHYCMD_KV_TTS_MODEL);
  const char      *voice = kv_get_str(REACHYCMD_KV_TTS_VOICE);
  const char      *speed = kv_get_str(REACHYCMD_KV_TTS_SPEED);
  llm_tts_params_t params;
  reachycmd_say_t *s;

  if(model == NULL || model[0] == '\0')
  {
    cmd_reply(ctx, "reachy: no voice is configured — set "
        REACHYCMD_KV_TTS_MODEL " to a registered tts model.");
    return;
  }

  memset(&params, 0, sizeof(params));
  params.voice = voice;
  params.speed = speed != NULL ? strtod(speed, NULL) : 0.0;

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
  reachycmd_act_t *a = reachycmd_act_new(ctx, "waking the robot");

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
  reachycmd_act_t *a = reachycmd_act_new(ctx, "putting the robot to sleep");

  if(reachy_sleep_move(reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_volume(const cmd_ctx_t *ctx)
{
  unsigned long    pct = strtoul(ctx->parsed->argv[0], NULL, 10);
  reachycmd_act_t *a;
  char             what[REACHY_MOVE_NAME_SZ + 32];

  if(pct > 100)
  {
    cmd_reply(ctx, "reachy: volume runs from 0 to 100.");
    return;
  }

  snprintf(what, sizeof(what), "setting the volume to %lu", pct);
  a = reachycmd_act_new(ctx, what);

  if(reachy_set_volume((uint8_t)pct, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_track(const cmd_ctx_t *ctx)
{
  const char      *weight_s = reachycmd_arg(ctx, 1);
  reachycmd_act_t *a;
  char             what[REACHY_MOVE_NAME_SZ + 32];
  double           weight = REACHYCMD_TRACK_WEIGHT;
  bool             on;

  if(reachycmd_flag(ctx->parsed->argv[0], &on) != SUCCESS)
  {
    cmd_reply(ctx, "Usage: reachy track <on|off> [weight 0.0-1.0]");
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

  if(on)
    snprintf(what, sizeof(what), "following faces at weight %.2f", weight);

  else
    snprintf(what, sizeof(what), "%s", "letting its gaze go");

  a = reachycmd_act_new(ctx, what);

  if(reachy_tracking(on, weight, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_wobble(const cmd_ctx_t *ctx)
{
  reachycmd_act_t *a;
  bool             on;

  if(reachycmd_flag(ctx->parsed->argv[0], &on) != SUCCESS)
  {
    cmd_reply(ctx, "Usage: reachy wobble <on|off>");
    return;
  }

  a = reachycmd_act_new(ctx, on
      ? "nodding along to whatever it plays"
      : "holding its head still while it plays");

  if(reachy_wobbling(on, reachycmd_act_done, a) != SUCCESS)
    reachycmd_act_refused(a);
}

static void
reachycmd_show(const cmd_ctx_t *ctx)
{
  reachycmd_show_t *s = mem_alloc(REACHYCMD_CTX, "show", sizeof(*s));

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

static const char reachycmd_help[] =
    "Move the Reachy Mini, and give it a voice.\n"
    "\n"
    "  !reachy list [filter]      the emotion library, optionally\n"
    "                             narrowed to names containing <filter>\n"
    "  !reachy do <move>          play one of them\n"
    "  !reachy say <text>         speak a line aloud\n"
    "  !reachy wake               torque on, then rise and centre\n"
    "  !reachy sleep              settle back down, then go limp\n"
    "  !reachy volume <0-100>     speaker level\n"
    "  !reachy track <on|off> [w] follow faces, at weight w (0.0-1.0)\n"
    "  !reachy wobble <on|off>    let played audio drive head motion\n"
    "\n"
    "The moves are recordings that ship with the robot's daemon, played\n"
    "on the robot itself — botman only names one. Speech is the other\n"
    "way round: the line is synthesized here, uploaded, and played, so\n"
    "`!reachy wobble on` is what makes it look like speaking rather than\n"
    "broadcasting. `show reachy` reports the daemon state, whether a\n"
    "face is being tracked, and which way the microphone array last\n"
    "heard a voice — including whether its motors are on at all, which\n"
    "is the difference between a robot that is ignoring you and one\n"
    "that physically cannot answer.";

// Uniformly `user` at REACHYCMD_LEVEL — reading verbs as well as moving
// ones. The robot is a physical object in a room and not a toy for a
// channel to discover, so `show reachy` and `reachy list` are gated
// exactly as hard as `reachy do`. That uniformity is also what keeps the
// tree walkable: the dispatcher checks the `reachy` parent before
// resolving a child, so a parent stricter than its children would deny
// them on the way past.
static bool
reachycmd_register(void)
{
  if(cmd_register(REACHYCMD_CTX, "reachy",
        "reachy <list|do|say|wake|sleep|volume|track|wobble>",
        "Move the Reachy Mini robot.",
        reachycmd_help,
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_root, NULL, NULL, NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "list",
        "reachy list [filter]",
        "The emotion moves the robot knows.",
        "Asks the robot's daemon for the move library it ships with — 84 "
        "recordings at daemon 1.9.0 — and prints them four to a line. A "
        "filter narrows the list to names containing it, case "
        "insensitively, which is the fast way to find every dance or "
        "every way of saying no.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_list, NULL, "reachy", NULL,
        reachycmd_list_args,
        (uint8_t)(sizeof(reachycmd_list_args)
                  / sizeof(reachycmd_list_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "do",
        "reachy do <move>",
        "Play one of the robot's emotion moves.",
        "The move is played by the robot's own daemon, on its own clock; "
        "botman only names it and is told whether it started. Names come "
        "from `reachy list`. A name the library does not hold comes back "
        "as an http 404 rather than silence.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_do, NULL, "reachy", NULL,
        reachycmd_do_args,
        (uint8_t)(sizeof(reachycmd_do_args) / sizeof(reachycmd_do_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "say",
        "reachy say <text>",
        "Make the robot speak a line aloud.",
        "Three hops: the text is synthesized by the tts model named in "
        "plugin.reachycmd.tts_model, the resulting WAV is uploaded to the "
        "robot, and the robot plays it. Turn `reachy wobble on` first and "
        "the head moves in time with the words. The voice and rate come "
        "from plugin.reachycmd.tts_voice and .tts_speed; the model row "
        "itself is registered with `llm add model tts ...`.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_say, NULL, "reachy", NULL,
        reachycmd_say_args,
        (uint8_t)(sizeof(reachycmd_say_args)
                  / sizeof(reachycmd_say_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "wake",
        "reachy wake",
        "Bring the robot up out of rest.",
        "Enables the motors, then plays the daemon's wake_up move: the "
        "head rises, the body re-centres, the antennas come down. Both "
        "steps are needed — the daemon never raises torque on its own, "
        "and a limp robot plays the whole move without stirring. Safe to "
        "repeat; the motion takes about two and a half seconds.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_wake, NULL, "reachy", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "sleep",
        "reachy sleep",
        "Settle the robot back into rest.",
        "Plays the daemon's goto_sleep move: the robot lowers itself "
        "into its shell over about two and a half seconds, and the "
        "daemon drops torque once it arrives. It is then limp, and only "
        "`reachy wake` will lift it again. `show reachy` reports which "
        "of the two states it is in.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_sleep, NULL, "reachy", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "volume",
        "reachy volume <0-100>",
        "Set the robot's speaker level.",
        "Applies immediately and persists in the daemon until something "
        "else changes it.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_volume, NULL, "reachy", NULL,
        reachycmd_volume_args,
        (uint8_t)(sizeof(reachycmd_volume_args)
                  / sizeof(reachycmd_volume_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "track",
        "reachy track <on|off> [weight]",
        "Make the robot follow faces with its head.",
        "Face detection and the head motion that follows it both run on "
        "the robot; botman only switches them on. The weight is how "
        "strongly the head is pulled toward the face, from 0.0 to 1.0, "
        "and defaults to 0.6 — high enough to be obviously alive, low "
        "enough not to snap.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_track, NULL, "reachy", NULL,
        reachycmd_track_args,
        (uint8_t)(sizeof(reachycmd_track_args)
                  / sizeof(reachycmd_track_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(REACHYCMD_CTX, "wobble",
        "reachy wobble <on|off>",
        "Let played audio drive the robot's head.",
        "With wobbling enabled, ANY sound the daemon plays moves the "
        "head in time with it. It costs nothing and it is what will make "
        "the robot look like it is speaking rather than broadcasting.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_wobble, NULL, "reachy", NULL,
        reachycmd_wobble_args,
        (uint8_t)(sizeof(reachycmd_wobble_args)
                  / sizeof(reachycmd_wobble_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  // Hangs off the core `show` parent, like every other read-only view.
  if(cmd_register(REACHYCMD_CTX, "reachy",
        "show reachy",
        "What the robot is doing right now.",
        "Two live reads in one card: the daemon's own status — its state, "
        "version, whether it still holds the camera and microphone, and "
        "the face it is currently tracking — and the microphone array's "
        "direction of arrival, which is the bearing it last heard a voice "
        "on and whether it is hearing one now.",
        USERNS_GROUP_USER, REACHYCMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
        reachycmd_show, NULL, "show", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, REACHYCMD_CTX, "!reachy and show reachy registered");
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

// Two paths cover every definition this plugin owns: the `reachy` root
// takes its seven children with it, and `show/reachy` is the one leaf
// that lives under somebody else's parent.
static void
reachycmd_deinit(void)
{
  cmd_unregister_path("reachy");
  cmd_unregister_path("show/reachy");
  clam(CLAM_INFO, REACHYCMD_CTX, "reachycmd plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = REACHYCMD_CTX,
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = REACHYCMD_CTX,
  .provides        = { { .name = "misc_reachycmd" } },
  .provides_count  = 1,
  .requires        = { { .name = "service_reachyapi" },
                       { .name = "bot_chat" },
                       { .name = "inference" } },
  .requires_count  = 3,
  .kv_schema       = reachycmd_kv_schema,
  .kv_schema_count = sizeof(reachycmd_kv_schema)
                     / sizeof(reachycmd_kv_schema[0]),
  .init            = reachycmd_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = reachycmd_deinit,
  .ext             = NULL,
};
