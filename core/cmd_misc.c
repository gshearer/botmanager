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

static const cmd_decl_t ping_decl = {
  .module      = "cmd",
  .name        = "ping",
  .usage       = "ping [args]",
  .description = "Reply with PONG, echoing any arguments",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_ping,
  .arg_desc    = ad_ping,
  .arg_count   = 1,
};

void
cmd_misc_register(void)
{
  cmd_register(&ping_decl);
}
