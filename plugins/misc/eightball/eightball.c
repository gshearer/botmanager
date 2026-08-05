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
  char line[256];
  int idx = util_rand(EIGHTBALL_COUNT);

  snprintf(line, sizeof(line),
      CLR_PURPLE "\xf0\x9f\x8e\xb1" CLR_RESET " %s", eightball_answers[idx]);
  cmd_reply(ctx, line);
}

// Plugin lifecycle

static const cmd_arg_desc_t eightball_args[] = {
  { "question", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static bool
eightball_init(void)
{
  if(cmd_register(EIGHTBALL_CTX, "8ball",
      "8ball <question>",
      "Ask the Magic 8-Ball a question",
      "Shake the Magic 8-Ball and receive its wisdom.\n"
      "You must ask a question for the ball to answer.\n"
      "\n"
      "Example:\n"
      "  !8ball Will it rain today?",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, eightball_cmd, NULL, NULL, "8",
      eightball_args, 1, NULL, NULL) != SUCCESS)
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
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = eightball_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = eightball_deinit,
  .ext             = NULL,
};
