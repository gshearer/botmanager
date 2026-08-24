// botmanager — MIT
// Eightball misc plugin: the Magic 8-Ball novelty command (!8ball).
#define EIGHTBALL_INTERNAL
#include "eightball.h"

#include "colors.h"
#include "util.h"

// 8ball

static const char *eightball_answers[] = {
  "It is certain.",
  "It is decidedly so.",
  "Without a doubt.",
  "Yes \xe2\x80\x93 definitely.",
  "You may rely on it.",
  "As I see it, yes.",
  "Most likely.",
  "Outlook good.",
  "Yes.",
  "Signs point to yes.",
  "Reply hazy, try again.",
  "Ask again later.",
  "Better not tell you now.",
  "Cannot predict now.",
  "Concentrate and ask again.",
  "Don't count on it.",
  "My reply is no.",
  "My sources say no.",
  "Outlook not so good.",
  "Very doubtful.",
};

#define EIGHTBALL_COUNT (sizeof(eightball_answers) / sizeof(eightball_answers[0]))

static void
eightball_cmd(const cmd_ctx_t *ctx)
{
  const char *answer = eightball_answers[util_rand(EIGHTBALL_COUNT)];
  char        line[256];
  char        instruction[512];

  snprintf(line, sizeof(line),
      CLR_PURPLE "\xf0\x9f\x8e\xb1" CLR_RESET " %s", answer);

  // What happened, never how to say it. The question is half of what
  // happened here — a verdict with nothing to be a verdict about is
  // not a line anyone can write — and it arrives whole: the argument
  // is required, and one longer than its row is refused by the parser
  // rather than truncated into it. The callee copies this before it
  // returns (plugins/bot/chat/persona_reply.c), so the stack buffer is
  // the whole lifetime it needs.
  snprintf(instruction, sizeof instruction,
      "Someone asked the Magic 8-Ball \"%s\" and the ball answered"
      " \"%s\". Deliver that verdict, in your own voice.",
      ctx->parsed->argv[0], answer);

  if(kv_get_uint(EIGHTBALL_KV_IN_VOICE) != 0
      && bot_persona_reply(ctx, instruction, line))
    return;

  cmd_reply(ctx, line);
}

// Plugin lifecycle

static const plugin_kv_entry_t eightball_kv_schema[] = {
  { EIGHTBALL_KV_IN_VOICE, KV_BOOL, "false",
    "Answer !8ball in the bot's persona voice instead of its own answer"
    " table. Costs one LLM call per question and needs a bot whose"
    " conversational half is on (bot.<name>.behavior.chat.enabled) — a"
    " command bot ignores it and keeps the table." },
};

static const cmd_arg_desc_t eightball_args[] = {
  { "question", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static const cmd_decl_t eightball_decl = {
  .module      = EIGHTBALL_CTX,
  .name        = "8ball",
  .usage       = "8ball <question>",
  .description = "Ask the Magic 8-Ball a question",
  .help_long   = "Shake the Magic 8-Ball and receive its wisdom.\n"
                 "You must ask a question for the ball to answer.\n"
                 "\n"
                 "Example:\n"
                 "  !8ball Will it rain today?",
  .group       = "everyone",
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = eightball_cmd,
  .abbrev      = "8",
  .arg_desc    = eightball_args,
  .arg_count   = 1,
};

static bool
eightball_init(void)
{
  if(cmd_register(&eightball_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static void
eightball_deinit(void)
{
  cmd_unregister_path("8ball");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "eightball",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "eightball",
  .provides        = { { .name = "misc_eightball" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = eightball_kv_schema,
  .kv_schema_count = sizeof(eightball_kv_schema)
      / sizeof(eightball_kv_schema[0]),
  .init            = eightball_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = eightball_deinit,
  .ext             = NULL,
};
