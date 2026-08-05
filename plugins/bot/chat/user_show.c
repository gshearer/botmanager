// botmanager — MIT
// /show user {facts,log,rag}: chat-plugin-local reads, admin/level-100.

#include "cmd.h"
#include "common.h"
#include "memory.h"
#include "method.h"
#include "userns.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Argument descriptors -- one required user name per verb. The log
// verb also accepts an optional limit; rag takes a required query as
// the REST token.
static const cmd_arg_desc_t ad_show_user_facts[] = {
  { "user",  CMD_ARG_NONE, CMD_ARG_REQUIRED,              0, NULL },
};

static const cmd_arg_desc_t ad_show_user_log[] = {
  { "user",  CMD_ARG_NONE, CMD_ARG_REQUIRED,              0, NULL },
  { "limit", CMD_ARG_NONE, CMD_ARG_OPTIONAL,              0, NULL },
};

static const cmd_arg_desc_t ad_show_user_rag[] = {
  { "user",  CMD_ARG_NONE, CMD_ARG_REQUIRED,              0, NULL },
  { "query", CMD_ARG_NONE, CMD_ARG_REST,                  0, NULL },
};

// Resolve the session's namespace and confirm the user exists. Returns
// the namespace on success (user is valid) or NULL after emitting the
// appropriate error reply.
static userns_t *
resolve_user_ns(const cmd_ctx_t *ctx, const char *username)
{
  userns_t *ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return(NULL);

  if(!userns_user_exists(ns, username))
  {
    char buf[USERNS_USER_SZ + 32];

    snprintf(buf, sizeof(buf), "user not found: %s", username);
    cmd_reply(ctx, buf);
    return(NULL);
  }

  return(ns);
}

// /show user facts <user>
static void
cmd_show_user_facts(const cmd_ctx_t *ctx)
{
  const char *username;
  userns_t   *ns;

  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: /show user facts <user>");
    return;
  }

  username = ctx->parsed->argv[0];
  ns = resolve_user_ns(ctx, username);

  if(ns == NULL)
    return;

  memory_show_user_facts(ctx, ns, username);
}

// /show user log <user> [limit]
static void
cmd_show_user_log(const cmd_ctx_t *ctx)
{
  uint32_t limit;
  const char *username;
  userns_t   *ns;

  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: /show user log <user> [limit]");
    return;
  }

  username = ctx->parsed->argv[0];
  ns = resolve_user_ns(ctx, username);

  if(ns == NULL)
    return;

  limit = 0;

  if(ctx->parsed->argc >= 2 && ctx->parsed->argv[1] != NULL
      && ctx->parsed->argv[1][0] != '\0')
    limit = (uint32_t)strtoul(ctx->parsed->argv[1], NULL, 10);

  memory_show_user_log(ctx, ns, username, limit);
}

// /show user rag <user> <query>
static void
cmd_show_user_rag(const cmd_ctx_t *ctx)
{
  const char *username;
  const char *query;
  userns_t   *ns;

  if(ctx->parsed == NULL || ctx->parsed->argc < 2
      || ctx->parsed->argv[1] == NULL
      || ctx->parsed->argv[1][0] == '\0')
  {
    cmd_reply(ctx, "usage: /show user rag <user> <query>");
    return;
  }

  username = ctx->parsed->argv[0];
  query = ctx->parsed->argv[1];
  ns = resolve_user_ns(ctx, username);

  if(ns == NULL)
    return;

  memory_show_user_rag(ctx, ns, username, query);
}

bool
chatbot_user_show_verbs_register(void)
{
  if(cmd_register("chat", "facts",
        "show user facts <user>",
        "List facts stored for a user",
        "Prints every user-scoped fact row for <user> in the active\n"
        "namespace. Columns are kind, key, value, confidence, and\n"
        "last-seen timestamp.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_show_user_facts, NULL, "show/user", NULL,
        ad_show_user_facts,
        (uint8_t)(sizeof(ad_show_user_facts) / sizeof(ad_show_user_facts[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("chat", "log",
        "show user log <user> [limit]",
        "Show recent conversation log for a user",
        "Dumps the latest conversation_log rows addressed to or from\n"
        "<user>. [limit] caps the output (default: 20).",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_show_user_log, NULL, "show/user", NULL,
        ad_show_user_log,
        (uint8_t)(sizeof(ad_show_user_log) / sizeof(ad_show_user_log[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("chat", "rag",
        "show user rag <user> <query>",
        "Exercise memory retrieval for a user",
        "Runs memory_retrieve against <user> with <query> and prints\n"
        "the fact / message hit counts plus the raw rows returned.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_show_user_rag, NULL, "show/user", NULL,
        ad_show_user_rag,
        (uint8_t)(sizeof(ad_show_user_rag) / sizeof(ad_show_user_rag[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
