// botmanager — MIT
// /look — the bot takes one still through its method's camera and says
// what it sees. Only a method that advertises METHOD_CAP_SIGHT has eyes;
// everywhere else the command refuses, which is also what a model that
// reaches for it there is told.
#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "hold.h"
#include "kv.h"
#include "method.h"
#include "userns.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// What the picture is looked at FOR when nobody said. The question rides
// the image into the model as the user's line, so it steers the caption.
#define LOOK_DEFAULT_QUESTION "What do you see?"

// A look outlives its command twice over — the capture, then the caption
// — so it carries a deep copy of the context (CMD.md §Asynchronous) and a
// hold, which is what lets a bot's teardown wait for it and then tell it
// the bot is gone.
typedef struct
{
  chatbot_hold_t   hold;
  chatbot_state_t *st;
  cmd_ctx_t        ctx;
  method_msg_t     msg;
  char             question[METHOD_TEXT_SZ];
} chatbot_look_t;

static void look_frame_cb(void *data, const void *image, size_t image_len,
    const char *mime);
static void look_caption_cb(void *data, const char *caption);

static void
look_free(chatbot_look_t *lk)
{
  chatbot_hold_unlink(&lk->hold);
  mem_free(lk);
}

// A refusal the model must not paraphrase into a description.
static void
look_refuse(const cmd_ctx_t *ctx, const char *why)
{
  cmd_result_set(ctx, CMD_RESULT_REFUSED);
  cmd_reply(ctx, why);
}

static void
cmd_look(const cmd_ctx_t *ctx)
{
  chatbot_state_t *st = bot_get_handle(ctx->bot);
  chatbot_look_t  *lk;

  if(st == NULL)
    return;

  if(kv_get_bot_uint(bot_inst_name(ctx->bot),
        "behavior.image_vision.enabled") == 0)
  {
    look_refuse(ctx, "my eyes are switched off.");
    return;
  }

  lk = mem_alloc("look", "req", sizeof(*lk));
  memset(lk, 0, sizeof(*lk));

  lk->st           = st;
  lk->msg          = *ctx->msg;
  lk->ctx          = *ctx;
  lk->ctx.msg      = &lk->msg;
  lk->ctx.args     = NULL;
  lk->ctx.username = NULL;
  lk->ctx.parsed   = NULL;
  lk->ctx.feat     = NULL;

  strlcpy(lk->question,
      ctx->args != NULL && ctx->args[0] != '\0'
          ? ctx->args : LOOK_DEFAULT_QUESTION,
      sizeof(lk->question));

  chatbot_hold_link(&lk->hold, st, NULL);

  if(method_capture_async(ctx->msg->inst, look_frame_cb, lk)
      == ASYNC_FAILED_UNDELIVERED)
  {
    look_free(lk);
    look_refuse(ctx, "I can't see anything from here.");
  }
}

// Driver thread (reachy: a curl worker). The image is ours only for this
// call, so it is encoded before anything else can happen.
static void
look_frame_cb(void *data, const void *image, size_t image_len,
    const char *mime)
{
  chatbot_look_t *lk = data;
  method_inst_t  *method;
  char           *b64;
  size_t          b64_cap;

  if(chatbot_hold_disowned(&lk->hold))
  {
    look_free(lk);
    return;
  }

  if(image == NULL)
  {
    look_refuse(&lk->ctx, "my camera didn't answer.");
    look_free(lk);
    return;
  }

  if(!chatbot_image_magic_ok(image, image_len, mime))
  {
    clam(CLAM_WARN, "look", "bot=%s: the camera sent %zu bytes that are "
        "not a %s", bot_inst_name(lk->ctx.bot), image_len, mime);
    look_refuse(&lk->ctx, "what my camera sent wasn't a picture.");
    look_free(lk);
    return;
  }

  b64_cap = ((image_len + 2) / 3) * 4 + 1;
  b64     = mem_alloc("look", "b64", b64_cap);

  if(util_b64_encode(image, image_len, b64, b64_cap) == 0)
  {
    mem_free(b64);
    look_refuse(&lk->ctx, "my camera didn't answer.");
    look_free(lk);
    return;
  }

  // The copy's instance pointer is not a reference; the reply pipeline
  // takes its own off a live one, found again by name.
  method = method_find(lk->msg.inst_name);

  if(method == NULL)
  {
    mem_free(b64);
    look_free(lk);
    return;
  }

  method_msg_bind(&lk->msg, method);
  strlcpy(lk->msg.text, lk->question, sizeof(lk->msg.text));

  // b64 is the pipeline's from here, on every path.
  chatbot_reply_submit_vision(lk->st, &lk->msg, NULL, b64, mime, false,
      look_caption_cb, lk);

  method_release(method);
}

// LLM worker, or a teardown's req_free. Whatever a look answers goes out
// through cmd_reply, so a model that asked through the NL bridge gets it
// back to retell in its own voice, like any other command's output.
static void
look_caption_cb(void *data, const char *caption)
{
  chatbot_look_t *lk = data;

  if(!chatbot_hold_disowned(&lk->hold))
  {
    if(caption != NULL)
      cmd_reply(&lk->ctx, caption);

    else
      look_refuse(&lk->ctx, "I looked, but I couldn't make anything out.");
  }

  look_free(lk);
}

static const cmd_nl_slot_t look_slots[] = {
  { .name  = "question",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_OPTIONAL | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t look_examples[] = {
  { .utterance  = "what do you see?",
    .invocation = "/look" },
  { .utterance  = "look at me — what color is my shirt?",
    .invocation = "/look what color is the person's shirt?" },
  { .utterance  = "is anybody else in the room?",
    .invocation = "/look how many people are in view?" },
};

static const cmd_nl_t look_nl = {
  .when          = "Someone asks you to look at something, or asks what"
                   " you can see — the room, a person, a thing held up to"
                   " you. You have a camera; this is how you use it.",
  .syntax        = "/look [question]",
  .slots         = look_slots,
  .slot_count    = (uint8_t)(sizeof(look_slots) / sizeof(look_slots[0])),
  .examples      = look_examples,
  .example_count = (uint8_t)(sizeof(look_examples)
                             / sizeof(look_examples[0])),
  .dispatch_text = NULL,
};

// Scoped to the reachy method so no text bot's NL block spends a slot on
// a command that can only refuse there. The capability, not this mask, is
// what decides whether a look can happen.
static const cmd_decl_t look_decl = {
  .module      = "chat",
  .name        = "look",
  .usage       = "look [question]",
  .description = "Look through the bot's camera and say what it sees",
  .help_long   =
      "Takes one still picture through the camera of the method you are\n"
      "talking on and describes it. Ask something after it and the\n"
      "picture is looked at for that: 'look is the door open?'.\n"
      "\n"
      "Only a method with eyes can look; everywhere else it refuses.\n"
      "behavior.image_vision.enabled switches it off for a bot.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_REACHY,
  .cb          = cmd_look,
  .nl          = &look_nl,
};

bool
chatbot_look_register(void)
{
  return(cmd_register(&look_decl));
}
