// botmanager — MIT
// note command surface: `note <user> <message>`. Registered globally so
// it works from any bot on any method. Leaving a note requires a
// registered user (group `user`, level 0) — enforced by the command
// system, not here — because a note is attributed mail, not graffiti.

#define NOTE_INTERNAL
#include "note.h"

#include "colors.h"
#include "kv.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ //
// Helpers                                                             //
// ------------------------------------------------------------------ //

// Who is leaving the note. `note` is gated on the `user` group, so an
// authenticated username is the normal case; the nickname fallback only
// covers a caller the command system let through some other way.
static const char *
note_caller(const cmd_ctx_t *ctx)
{
  if(ctx->username != NULL && ctx->username[0] != '\0')
    return(ctx->username);

  if(ctx->msg != NULL && ctx->msg->nickname[0] != '\0')
    return(ctx->msg->nickname);

  return((ctx->msg != NULL) ? ctx->msg->sender : "");
}

static uint32_t
note_kv_uint(const char *key, uint32_t fallback)
{
  return(kv_exists(key) ? (uint32_t)kv_get_uint(key) : fallback);
}

// ------------------------------------------------------------------ //
// note                                                                //
// ------------------------------------------------------------------ //

static void
note_cmd_leave(const cmd_ctx_t *ctx)
{
  userns_t   *ns = userns_session_resolve(ctx);
  const char *token;
  const char *body;
  const char *from;
  const char *method;
  const char *channel;
  char        recipient[USERNS_USER_SZ];
  char        line[NOTE_BODY_SZ + 256];
  char        text[NOTE_BODY_SZ];
  time_t      lastseen  = 0;
  uint32_t    min_idle;
  int         pending;
  int64_t     id;

  if(ns == NULL)   // resolver already replied with "no namespace set"
    return;

  if(ctx->parsed == NULL || ctx->parsed->argc < 2)
  {
    cmd_reply(ctx, "usage: note <user> <message>");
    return;
  }

  token = ctx->parsed->argv[0];
  body  = ctx->parsed->argv[1];
  from  = note_caller(ctx);

  // The recipient must be a real user of this namespace — a note is
  // addressed to an identity, not to whatever nick happens to be around.
  if(!userns_user_lookup_ci(ns, token, recipient, sizeof(recipient)))
  {
    snprintf(line, sizeof(line),
        "I don't know anyone called " CLR_CYAN "%s" CLR_RESET " here.",
        token);
    cmd_reply(ctx, line);
    return;
  }

  if(strcasecmp(recipient, from) == 0)
  {
    cmd_reply(ctx, "You're right here. Just remember it.");
    return;
  }

  // The idle gate: if they were around moments ago, tell them yourself.
  // A user we have never witnessed reads as infinitely idle and passes.
  min_idle = note_kv_uint(NOTE_KV_MIN_IDLE, NOTE_DEFAULT_MIN_IDLE);

  (void)userns_user_get_lastseen(ns, recipient, &lastseen, NULL, 0,
      NULL, 0);

  if(lastseen > 0 && (time(NULL) - lastseen) < (time_t)min_idle)
  {
    char idle[32];
    char window[32];

    util_fmt_duration(time(NULL) - lastseen, idle, sizeof(idle));
    util_fmt_duration((time_t)min_idle, window, sizeof(window));

    snprintf(line, sizeof(line),
        CLR_YELLOW "%s" CLR_RESET " was around %s ago — tell them "
        "yourself. (Notes are for people idle more than %s.)",
        recipient, idle, window);
    cmd_reply(ctx, line);
    return;
  }

  // Bound the backlog so one person cannot stuff another's inbox.
  {
    uint32_t max_pending = note_kv_uint(NOTE_KV_MAX_PENDING,
        NOTE_DEFAULT_MAX_PENDING);

    pending = note_db_pending_count(ns->id, recipient);

    if(pending < 0)
    {
      cmd_reply(ctx, "I couldn't reach my notebook. :~(");
      return;
    }

    if(max_pending > 0 && (uint32_t)pending >= max_pending)
    {
      snprintf(line, sizeof(line),
          CLR_CYAN "%s" CLR_RESET " already has %d note(s) waiting. "
          "Let them catch up first.", recipient, pending);
      cmd_reply(ctx, line);
      return;
    }
  }

  // Store the message as typed, bounded by the configured cap.
  snprintf(text, sizeof(text), "%s", body);

  {
    uint32_t max_body = note_kv_uint(NOTE_KV_MAX_BODY, NOTE_BODY_SZ - 1);

    if(max_body > 0 && max_body < sizeof(text) && strlen(text) > max_body)
      text[max_body] = '\0';
  }

  method  = (ctx->msg != NULL) ? method_inst_name(ctx->msg->inst) : NULL;
  channel = (ctx->msg != NULL) ? ctx->msg->channel : "";

  id = note_db_add(ns->id, from, recipient, text,
      method != NULL ? method : "", channel != NULL ? channel : "");

  if(id < 0)
  {
    cmd_reply(ctx, "I couldn't write that down. :~(");
    return;
  }

  // Only now does the recipient become interesting to the observers.
  note_pending_add(ns->id, recipient);

  snprintf(line, sizeof(line),
      CLR_GREEN "Noted" CLR_RESET " — I'll tell " CLR_CYAN "%s"
      CLR_RESET " next time I see them.", recipient);
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_arg_desc_t note_args[] = {
  { "user",    CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ - 1, NULL },
  { "message", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST,
      NOTE_BODY_SZ - 1, NULL },
};

// NL hint: "tell X next time you see them that ..." is the natural way
// people ask for this, so the examples teach the model to rewrite the
// message into second person addressed at the recipient.
static const cmd_nl_slot_t note_slots[] = {
  { .name = "user",    .type = CMD_NL_ARG_NICK, .flags = CMD_NL_SLOT_REQUIRED },
  { .name = "message", .type = CMD_NL_ARG_FREE,
      .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t note_examples[] = {
  { .utterance  = "please tell adairen the next time you see him that "
                  "I said he's really great",
    .invocation = "/note adairen you're really great" },
  { .utterance  = "leave doc a note to check the build logs",
    .invocation = "/note doc check the build logs" },
  { .utterance  = "let hawken know the server is back up when he returns",
    .invocation = "/note hawken the server is back up" },
};

static const cmd_nl_t note_nl = {
  .when          = "User wants to leave a message for someone who is not "
                   "around, to be delivered the next time that person is "
                   "seen. Write the message in the second person, "
                   "addressed to the recipient — the delivery already "
                   "names who left it, so never write \"X says\" into the "
                   "message itself.",
  .syntax        = "/note <user> <message>",
  .slots         = note_slots,
  .slot_count    = (uint8_t)(sizeof(note_slots) / sizeof(note_slots[0])),
  .examples      = note_examples,
  .example_count = (uint8_t)(sizeof(note_examples) / sizeof(note_examples[0])),
};

bool
note_commands_register(void)
{
  if(cmd_register("note", "note",
        "note <user> <message>",
        "Leave a message for another user, delivered when they're next seen.",
        "The recipient must be a user of this namespace and must have been "
        "idle longer than plugin.note.min_idle_secs — if they were just "
        "here, tell them yourself. The note is announced wherever they next "
        "speak, addressed to them, then marked delivered.",
        USERNS_GROUP_USER, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        note_cmd_leave, NULL, NULL, NULL,
        note_args, (uint8_t)(sizeof(note_args) / sizeof(note_args[0])),
        NULL, &note_nl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

void
note_commands_unregister(void)
{
  cmd_unregister_path("note");
}
