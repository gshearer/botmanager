// botmanager — MIT
// Small-games misc plugin: chat mini-games (8ball, dice, and friends).
#define SMALLGAMES_INTERNAL
#include "smallgames.h"

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
smallgames_init(void)
{
  if(cmd_register(SMALLGAMES_CTX, "8ball",
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
smallgames_deinit(void)
{
  cmd_unregister("8ball");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "misc_smallgames",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "misc_smallgames",
  .provides        = { { .name = "misc_misc_smallgames" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_command" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = smallgames_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = smallgames_deinit,
  .ext             = NULL,
};
