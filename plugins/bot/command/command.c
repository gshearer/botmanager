// command.c — Command bot plugin (kind: command)

#define COMMAND_INTERNAL
#include "command.h"

// ------------------------------------------------------------------ //
// Command callbacks                                                   //
// ------------------------------------------------------------------ //

// returns: void
// ctx: command context containing args, sender, and bot reference
//
// !identify <username> <password>
// Authenticate with the bot's user namespace.
static void
cmd_identify(const cmd_ctx_t *ctx)
{
  // Already authenticated?
  if(ctx->username != NULL)
  {
    cmd_reply(ctx, "Already authenticated. Use deauth first.");
    return;
  }

  // No user namespace bound?
  if(bot_get_userns(ctx->bot) == NULL)
  {
    cmd_reply(ctx, "No user namespace configured for this bot.");
    return;
  }

  const char *username = ctx->parsed->argv[0];
  const char *pass     = ctx->parsed->argv[1];

  // Attempt authentication.
  userns_auth_t result = bot_session_auth(ctx->bot, ctx->msg->inst,
      ctx->msg->sender, username, pass);

  switch(result)
  {
    case USERNS_AUTH_OK:
      cmd_reply(ctx, "Authenticated.");
      break;
    case USERNS_AUTH_ERR:
      cmd_reply(ctx, "Authentication error.");
      break;
    default:
      // BAD_USER, BAD_PASS, NO_HASH — generic message to prevent
      // username enumeration.
      cmd_reply(ctx, "Authentication failed.");
      break;
  }
}

// returns: void
// ctx: command context for the deauth request
//
// !deauth
// End the current authenticated session.
static void
cmd_deauth(const cmd_ctx_t *ctx)
{
  if(ctx->username == NULL)
  {
    cmd_reply(ctx, "Not authenticated.");
    return;
  }

  if(bot_session_remove(ctx->bot, ctx->msg->inst,
        ctx->msg->sender) == SUCCESS)
    cmd_reply(ctx, "Session ended.");

  else
    cmd_reply(ctx, "Failed to end session.");
}

// returns: void
// ctx: command context for the register request
//
// !register <password>
// Set initial password for a discovered (password-less) account.
// The user must have been auto-discovered via MFA pattern matching
// and not yet have a password set.
static void
cmd_register_user(const cmd_ctx_t *ctx)
{
  // Must not already be authenticated.
  if(ctx->username != NULL)
  {
    cmd_reply(ctx, "Already authenticated. Use passwd to change your password.");
    return;
  }

  userns_t *ns = bot_get_userns(ctx->bot);

  if(ns == NULL)
  {
    cmd_reply(ctx, "No user namespace configured for this bot.");
    return;
  }

  // Try to find the sender via MFA matching (they must be a discovered user).
  const char *mfa_str = ctx->msg->metadata;

  if(mfa_str == NULL || mfa_str[0] == '\0')
  {
    cmd_reply(ctx, "Cannot determine your identity from this method.");
    return;
  }

  const char *matched = userns_mfa_match(ns, mfa_str);

  if(matched == NULL)
  {
    cmd_reply(ctx, "No discovered account matches your identity. "
        "Use identify to authenticate with an existing account.");
    return;
  }

  // Try to authenticate with empty password to confirm they have no password.
  // If auth returns NO_HASH, they're a discovered user without a password.
  userns_auth_t probe = userns_auth(ns, matched, "probe_will_fail", NULL);

  if(probe == USERNS_AUTH_OK || probe == USERNS_AUTH_BAD_PASS)
  {
    cmd_reply(ctx, "Account already has a password. Use identify to log in.");
    return;
  }

  if(probe != USERNS_AUTH_NO_HASH)
  {
    cmd_reply(ctx, "Registration error.");
    return;
  }

  // Validate password against policy.
  const char *password = ctx->parsed->argv[0];

  if(userns_password_check(password) != SUCCESS)
  {
    cmd_reply(ctx, "Password does not meet the password policy.");
    return;
  }

  // Set the password via admin reset (no old password required).
  if(userns_user_reset_password(ns, matched, password) != SUCCESS)
  {
    cmd_reply(ctx, "Failed to set password.");
    return;
  }

  // Auto-authenticate.
  userns_auth_t result = bot_session_auth(ctx->bot, ctx->msg->inst,
      ctx->msg->sender, matched, password);

  if(result == USERNS_AUTH_OK)
    cmd_reply(ctx, "Password set. You are now authenticated.");
  else
    cmd_reply(ctx, "Password set, but auto-login failed. Use identify.");
}

// ------------------------------------------------------------------ //
// Bot driver callbacks                                                //
// ------------------------------------------------------------------ //

// returns: void * — allocated cmdbot_state_t, or NULL on failure
// inst: bot instance to bind this command bot to
static void *
cmdbot_create(bot_inst_t *inst)
{
  cmdbot_state_t *st = mem_alloc("command", "state", sizeof(*st));

  if(st == NULL)
    return(NULL);

  memset(st, 0, sizeof(*st));
  st->inst = inst;

  // Apply default prefix from plugin KV config.
  const char *prefix = kv_get_str("plugin.command.prefix");

  if(prefix != NULL && prefix[0] != '\0')
    cmd_set_prefix(inst, prefix);

  // Enable all registered user commands (auth, service, etc.).
  cmd_enable_all(inst);

  return(st);
}

// returns: void
// handle: cmdbot_state_t to free
static void
cmdbot_destroy(void *handle)
{
  if(handle != NULL)
    mem_free(handle);
}

// returns: bool — SUCCESS always
// handle: unused
static bool
cmdbot_start(void *handle)
{
  (void)handle;
  return(SUCCESS);
}

// returns: void
// handle: unused
static void
cmdbot_stop(void *handle)
{
  (void)handle;
}

// returns: void
// handle: cmdbot_state_t for this instance
// msg: incoming method message to dispatch
static void
cmdbot_on_message(void *handle, const method_msg_t *msg)
{
  cmdbot_state_t *st = handle;

  // Attempt user discovery from the method metadata (e.g., nick!user@host).
  if(msg->metadata[0] != '\0')
    bot_discover_user(st->inst, msg->metadata);

  // Dispatch to the command system. Non-commands are silently ignored.
  cmd_dispatch(st->inst, msg);
}

// ------------------------------------------------------------------ //
// Driver and descriptor                                               //
// ------------------------------------------------------------------ //

static const bot_driver_t cmdbot_driver = {
  .name       = "command",
  .create     = cmdbot_create,
  .destroy    = cmdbot_destroy,
  .start      = cmdbot_start,
  .stop       = cmdbot_stop,
  .on_message = cmdbot_on_message,
};

// ------------------------------------------------------------------ //
// Plugin lifecycle                                                    //
// ------------------------------------------------------------------ //

// returns: bool — SUCCESS or FAIL
static bool
cmdbot_init(void)
{
  if(cmd_register("command", "identify",
        "identify <username> <password>",
        "Authenticate with the bot",
        "Authenticate with the bot's user namespace using your\n"
        "username and password. Once authenticated, you can use\n"
        "commands that require group membership or elevated\n"
        "privileges. Use deauth to end your session.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_PRIVATE, METHOD_T_ANY, cmd_identify, NULL,
        NULL, NULL, cmdbot_ad_identify, 2) != SUCCESS)
    return(FAIL);

  if(cmd_register("command", "deauth",
        "deauth",
        "End your authenticated session",
        "Ends your current authenticated session on this method.\n"
        "After deauthenticating, you are treated as an anonymous\n"
        "user and can only access commands in the everyone group\n"
        "at level 0.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_deauth, NULL, NULL,
        NULL, NULL, 0) != SUCCESS)
  {
    cmd_unregister("identify");
    return(FAIL);
  }

  if(cmd_register("command", "register",
        "register <password>",
        "Set password for a discovered account",
        "Set the initial password for your auto-discovered account.\n"
        "This command is for users whose accounts were created via\n"
        "user discovery (MFA pattern matching). Once a password is\n"
        "set, use identify to authenticate in future sessions.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_PRIVATE, METHOD_T_ANY, cmd_register_user,
        NULL, NULL, "reg", cmdbot_ad_register, 1) != SUCCESS)
  {
    cmd_unregister("deauth");
    cmd_unregister("identify");
    return(FAIL);
  }

  return(SUCCESS);
}

// returns: void
static void
cmdbot_deinit(void)
{
  cmd_unregister("register");
  cmd_unregister("identify");
  cmd_unregister("deauth");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "command",
  .version         = "1.0",
  .type            = PLUGIN_BOT,
  .kind            = "command",
  .provides        = { { .name = "bot_command" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = cmdbot_kv_schema,
  .kv_schema_count = sizeof(cmdbot_kv_schema) / sizeof(cmdbot_kv_schema[0]),
  .init            = cmdbot_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = cmdbot_deinit,
  .ext             = &cmdbot_driver,
};
