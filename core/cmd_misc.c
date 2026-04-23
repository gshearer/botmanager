// botmanager — MIT
// Miscellaneous built-in commands (e.g. /ping) with no dedicated home.

#include "cmd.h"
#include "cmd_misc.h"
#include "userns.h"

#include <stdio.h>

// !ping

static void
cmd_ping(const cmd_ctx_t *ctx)
{
  const char *args = (ctx->parsed->argc > 0) ? ctx->parsed->argv[0] : NULL;
  char line[CMD_ARG_SZ + 16];

  if(args == NULL || args[0] == '\0')
  {
    cmd_reply(ctx, "PONG");
    return;
  }

  snprintf(line, sizeof(line), "PONG %s", args);
  cmd_reply(ctx, line);
}

static const cmd_arg_desc_t ad_ping[] = {
  { "args", CMD_ARG_NONE, CMD_ARG_OPTIONAL | CMD_ARG_REST, 0, NULL },
};

// Registration

void
cmd_misc_register(void)
{
  cmd_register("cmd", "ping",
      "ping [args]",
      "Reply with PONG, echoing any arguments",
      NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_ping, NULL, NULL, NULL, ad_ping, 1, NULL, NULL);
}
