// botmanager — MIT
// Command bot plugin (kind: command).

#define COMMAND_INTERNAL
#include "command.h"

// ------------------------------------------------------------------ //
// Command callbacks                                                   //
// ------------------------------------------------------------------ //

static void
cmd_identify(const cmd_ctx_t *ctx)
{
  const char *username;
  userns_t *ns;
  const char *mfa_str;
  userns_auth_t result;
  const char *pass;
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

  username = ctx->parsed->argv[0];
  pass = ctx->parsed->argv[1];

  // Require at least one MFA pattern before allowing authentication.
  ns = bot_get_userns(ctx->bot);

  if(!userns_user_has_mfa(ns, username))
  {
    clam(CLAM_WARN, "identify",
        "no MFA patterns for '%s' (sender: %s)",
        username, ctx->msg->sender);
    cmd_reply(ctx, "No MFA patterns defined for this user. "
        "An admin must add at least one before you can authenticate.");
    return;
  }

  // Verify the caller's method-facing address matches one of the
  // target user's MFA patterns. This prevents a user from identifying
  // as someone else from an unrecognized host.
  mfa_str = ctx->msg->metadata;

  if(mfa_str == NULL || mfa_str[0] == '\0')
  {
    clam(CLAM_WARN, "identify",
        "no method metadata for '%s' (sender: %s)",
        username, ctx->msg->sender);
    cmd_reply(ctx, "Cannot determine your identity from this method.");
    return;
  }

  if(!userns_user_mfa_match(ns, username, mfa_str))
  {
    clam(CLAM_WARN, "identify",
        "MFA mismatch for '%s' from '%s'", username, mfa_str);
    cmd_reply(ctx, "Authentication failed.");
    return;
  }

  // Attempt authentication.
  result = bot_session_auth(ctx->bot, ctx->msg->inst,
      ctx->msg->sender, username, pass);

  switch(result)
  {
    case USERNS_AUTH_OK:
      clam(CLAM_INFO, "identify",
          "'%s' authenticated from '%s'", username, mfa_str);
      cmd_reply(ctx, "Authenticated.");
      break;
    case USERNS_AUTH_ERR:
      clam(CLAM_WARN, "identify",
          "auth error for '%s' from '%s'", username, mfa_str);
      cmd_reply(ctx, "Authentication error.");
      break;
    default:
      // BAD_USER, BAD_PASS, NO_HASH — generic message to prevent
      // username enumeration.
      clam(CLAM_WARN, "identify",
          "auth failed for '%s' from '%s'", username, mfa_str);
      cmd_reply(ctx, "Authentication failed.");
      break;
  }
}

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
  {
    clam(CLAM_INFO, "deauth",
        "'%s' deauthenticated (sender: %s)",
        ctx->username, ctx->msg->sender);
    cmd_reply(ctx, "Session ended.");
  }

  else
  {
    clam(CLAM_WARN, "deauth",
        "failed to end session for '%s' (sender: %s)",
        ctx->username, ctx->msg->sender);
    cmd_reply(ctx, "Failed to end session.");
  }
}

//
// !register <password>
// Set initial password for a discovered (password-less) account.
// The user must have been auto-discovered via MFA pattern matching
// and not yet have a password set.
static void
cmd_register_user(const cmd_ctx_t *ctx)
{
  userns_t *ns;
  const char *mfa_str;
  const char *matched;
  userns_auth_t probe;
  const char *password;
  userns_auth_t result;
  // Must not already be authenticated.
  if(ctx->username != NULL)
  {
    cmd_reply(ctx, "Already authenticated. Use passwd to change your password.");
    return;
  }

  ns = bot_get_userns(ctx->bot);

  if(ns == NULL)
  {
    cmd_reply(ctx, "No user namespace configured for this bot.");
    return;
  }

  // Try to find the sender via MFA matching (they must be a discovered user).
  mfa_str = ctx->msg->metadata;

  if(mfa_str == NULL || mfa_str[0] == '\0')
  {
    cmd_reply(ctx, "Cannot determine your identity from this method.");
    return;
  }

  matched = userns_mfa_match(ns, mfa_str);

  if(matched == NULL)
  {
    clam(CLAM_INFO, "register",
        "no MFA match for register attempt from '%s'", mfa_str);
    cmd_reply(ctx, "No discovered account matches your identity. "
        "Use identify to authenticate with an existing account.");
    return;
  }

  // Try to authenticate with empty password to confirm they have no password.
  // If auth returns NO_HASH, they're a discovered user without a password.
  probe = userns_auth(ns, matched, "probe_will_fail", NULL);

  if(probe == USERNS_AUTH_OK || probe == USERNS_AUTH_BAD_PASS)
  {
    clam(CLAM_WARN, "register",
        "register attempt for '%s' which already has a password "
        "(from '%s')", matched, mfa_str);
    cmd_reply(ctx, "Account already has a password. Use identify to log in.");
    return;
  }

  if(probe != USERNS_AUTH_NO_HASH)
  {
    clam(CLAM_WARN, "register",
        "register probe error for '%s' from '%s'", matched, mfa_str);
    cmd_reply(ctx, "Registration error.");
    return;
  }

  // Validate password against policy.
  password = ctx->parsed->argv[0];

  if(userns_password_check(password) != SUCCESS)
  {
    clam(CLAM_INFO, "register",
        "password policy failure for '%s' from '%s'", matched, mfa_str);
    cmd_reply(ctx, "Password does not meet the password policy.");
    return;
  }

  // Set the password via admin reset (no old password required).
  if(userns_user_reset_password(ns, matched, password) != SUCCESS)
  {
    clam(CLAM_WARN, "register",
        "failed to set password for '%s' from '%s'", matched, mfa_str);
    cmd_reply(ctx, "Failed to set password.");
    return;
  }

  // Auto-authenticate.
  result = bot_session_auth(ctx->bot, ctx->msg->inst,
      ctx->msg->sender, matched, password);

  if(result == USERNS_AUTH_OK)
  {
    clam(CLAM_INFO, "register",
        "'%s' registered and authenticated from '%s'", matched, mfa_str);
    cmd_reply(ctx, "Password set. You are now authenticated.");
  }

  else
  {
    clam(CLAM_WARN, "register",
        "'%s' registered but auto-login failed from '%s'", matched, mfa_str);
    cmd_reply(ctx, "Password set, but auto-login failed. Use identify.");
  }
}

// ------------------------------------------------------------------ //
// !id — show what the bot knows about the caller or channel           //
// ------------------------------------------------------------------ //

// Iteration state for building the group list string.
typedef struct
{
  char   *buf;
  size_t  sz;
  size_t  pos;
  uint32_t count;
} id_group_state_t;

static void
id_group_cb(const char *group, uint16_t level, void *data)
{
  id_group_state_t *st = data;
  int n;

  if(st->count > 0 && st->pos < st->sz - 1)
  {
    n = snprintf(st->buf + st->pos, st->sz - st->pos, ", ");
    if(n > 0) st->pos += (size_t)n;
  }

  n = snprintf(st->buf + st->pos, st->sz - st->pos,
      CLR_CYAN "%s" CLR_RESET ":%u", group, (unsigned)level);
  if(n > 0) st->pos += (size_t)n;

  st->count++;
}

// Pad a string to exactly `width` display columns with trailing spaces.
// Only handles ASCII strings (no embedded color codes or multi-byte).
static void
id_pad_field(char *dst, size_t dst_sz, const char *str, int width)
{
  int len = (int)strlen(str);
  int pad = width - len;

  if(pad < 0) pad = 0;
  snprintf(dst, dst_sz, "%s%*s", str, pad, "");
}

// Format a single identity line for a nick. Resolves identity via
// session lookup and MFA matching. Writes the formatted line to buf.
// Returns the number of bytes written (excluding NUL).
static size_t
id_format_nick(const cmd_ctx_t *ctx, userns_t *ns,
    const char *nick, char *buf, size_t buf_sz)
{
  char nick_pad[48];
  char user_pad[48];
  char status_label[32];
  const char * status_clr;
  char status_pad[48];
  const char *username = NULL;
  bool is_authed = false;
  int n;
  size_t pos = 0;

  // Column widths (display characters, not bytes).
  enum { W_NICK = 16, W_USER = 16, W_STATUS = 14 };

  // Look up the nick's MFA context from the method driver.
  char mfa_str[METHOD_META_SZ] = {0};

  if(ctx->msg->inst != NULL)
  {
    char host[METHOD_META_SZ] = {0};

    if(method_get_context(ctx->msg->inst, nick,
        host, sizeof(host)) == SUCCESS && host[0] != '\0')
      snprintf(mfa_str, sizeof(mfa_str), "%s!%s", nick, host);
  }

  // Try to resolve identity.
  if(ns != NULL && ctx->msg->inst != NULL)
    username = bot_session_find_ex(ctx->bot, ctx->msg->inst,
        nick, mfa_str[0] != '\0' ? mfa_str : NULL, &is_authed);

  // Pad nick (ASCII, safe for snprintf padding).
  id_pad_field(nick_pad, sizeof(nick_pad), nick, W_NICK);

  if(username == NULL)
  {
    char status_pad[48];
    // Em dash is 3 bytes / 1 display char — pad manually.
    char user_pad[48];
    snprintf(user_pad, sizeof(user_pad), "\xe2\x80\x94%*s", W_USER - 1, "");

    id_pad_field(status_pad, sizeof(status_pad), "[anonymous]", W_STATUS);

    n = snprintf(buf, buf_sz,
        "  %s " CLR_GRAY "%s" CLR_RESET
        " " CLR_GRAY "%s" CLR_RESET
        " " CLR_CYAN "%s" CLR_RESET ":0",
        nick_pad, user_pad, status_pad, USERNS_GROUP_EVERYONE);
    return(n > 0 ? (size_t)n : 0);
  }

  // Pad username (ASCII).
  id_pad_field(user_pad, sizeof(user_pad), username, W_USER);

  // Build status field: pad the label, then colorize.

  if(is_authed)
  {
    status_clr = CLR_GREEN;
    snprintf(status_label, sizeof(status_label), "[identified]");
  }

  else
  {
    status_clr = CLR_YELLOW;
    snprintf(status_label, sizeof(status_label), "[unidentified]");
  }

  id_pad_field(status_pad, sizeof(status_pad), status_label, W_STATUS);

  n = snprintf(buf, buf_sz,
      "  %s " CLR_BOLD "%s" CLR_RESET " %s%s" CLR_RESET " ",
      nick_pad, user_pad, status_clr, status_pad);

  if(n > 0) pos = (size_t)n;

  // Append group memberships.
  if(ns != NULL && !userns_is_owner(username))
  {
    id_group_state_t gs = {
      .buf = buf, .sz = buf_sz, .pos = pos, .count = 0
    };

    userns_membership_iterate(ns, username, id_group_cb, &gs);
    pos = gs.pos;

    if(gs.count == 0)
    {
      n = snprintf(buf + pos, buf_sz - pos,
          CLR_GRAY "(none)" CLR_RESET);
      if(n > 0) pos += (size_t)n;
    }
  }

  else if(userns_is_owner(username))
  {
    n = snprintf(buf + pos, buf_sz - pos,
        CLR_PURPLE "*" CLR_RESET " " CLR_GRAY "(owner)" CLR_RESET);
    if(n > 0) pos += (size_t)n;
  }

  return(pos);
}

// Callback state for channel member iteration.
typedef struct
{
  const cmd_ctx_t *ctx;
  userns_t        *ns;
  const char      *self_nick;   // bot's own nick to skip
  uint32_t         count;
} id_chan_state_t;

static void
id_chan_member_cb(const char *nick, void *data)
{
  char line[1024];
  id_chan_state_t *st = data;

  // Skip the bot's own nick.
  if(st->self_nick != NULL && strcasecmp(nick, st->self_nick) == 0)
    return;


  id_format_nick(st->ctx, st->ns, nick, line, sizeof(line));
  cmd_reply(st->ctx, line);
  st->count++;
}

// Format a single-line identity summary for self-query or single-nick
// query. Uses the inline "nick (user) [status] groups: ..." format.
static void
id_format_self(const cmd_ctx_t *ctx, userns_t *ns, const char *nick,
    const char *mfa_str)
{
  const char *status;
  const char *username = NULL;
  bool is_authed = false;
  int n;
  size_t pos = 0;
  char line[1024];

  if(ns != NULL && ctx->msg->inst != NULL)
    username = bot_session_find_ex(ctx->bot, ctx->msg->inst,
        nick, mfa_str, &is_authed);

  if(username == NULL)
  {
    n = snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET
        " " CLR_GRAY "[anonymous]" CLR_RESET
        " groups: " CLR_CYAN "%s" CLR_RESET ":0",
        nick, USERNS_GROUP_EVERYONE);
    (void)n;
    cmd_reply(ctx, line);
    return;
  }


  if(is_authed)
    status = CLR_GREEN "identified" CLR_RESET;
  else
    status = CLR_YELLOW "unidentified" CLR_RESET;

  n = snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET
      " (" CLR_BOLD "%s" CLR_RESET ")"
      " [%s]"
      " groups: ",
      nick, username, status);

  if(n > 0) pos = (size_t)n;

  if(ns != NULL && !userns_is_owner(username))
  {
    id_group_state_t gs = {
      .buf = line, .sz = sizeof(line), .pos = pos, .count = 0
    };

    userns_membership_iterate(ns, username, id_group_cb, &gs);
    pos = gs.pos;

    if(gs.count == 0)
    {
      n = snprintf(line + pos, sizeof(line) - pos,
          CLR_GRAY "(none)" CLR_RESET);
      if(n > 0) pos += (size_t)n;
    }
  }

  else if(userns_is_owner(username))
  {
    n = snprintf(line + pos, sizeof(line) - pos,
        CLR_PURPLE "*" CLR_RESET " " CLR_GRAY "(owner)" CLR_RESET);
    if(n > 0) pos += (size_t)n;
  }

  cmd_reply(ctx, line);
}

static void
cmd_id(const cmd_ctx_t *ctx)
{
  userns_t *ns = bot_get_userns(ctx->bot);

  // Argument mode: !id <nickname> — single-line lookup for a specific nick.
  if(ctx->parsed != NULL && ctx->parsed->argc > 0)
  {
    const char *target = ctx->parsed->argv[0];

    // Build MFA string for the target nick from the method context.
    char mfa_str[METHOD_META_SZ] = {0};

    if(ctx->msg->inst != NULL)
    {
      char host[METHOD_META_SZ] = {0};

      if(method_get_context(ctx->msg->inst, target,
          host, sizeof(host)) == SUCCESS && host[0] != '\0')
        snprintf(mfa_str, sizeof(mfa_str), "%s!%s", target, host);
    }

    id_format_self(ctx, ns, target, mfa_str[0] != '\0' ? mfa_str : NULL);
    return;
  }

  // Channel mode (no args, in a group chat): list all channel members.
  if(ctx->msg->channel[0] != '\0' && ctx->msg->inst != NULL)
  {
    id_chan_state_t cs;
    char title[METHOD_CHANNEL_SZ + 48];
    char self_nick[METHOD_SENDER_SZ];
    snprintf(title, sizeof(title),
        "identities in " CLR_BOLD "%s" CLR_RESET ":",
        ctx->msg->channel);
    cmd_reply(ctx, title);

    cmd_reply(ctx,
        CLR_BOLD "  NICK             USER             STATUS         "
        "GROUPS" CLR_RESET);

    self_nick[0] = '\0';
    method_get_self(ctx->msg->inst, self_nick, sizeof(self_nick));

    cs = (id_chan_state_t){
      .ctx = ctx, .ns = ns,
      .self_nick = self_nick[0] != '\0' ? self_nick : NULL,
      .count = 0
    };
    method_list_channel(ctx->msg->inst, ctx->msg->channel,
        id_chan_member_cb, &cs);

    if(cs.count == 0)
      cmd_reply(ctx, "  (no members tracked)");
    else
    {
      char footer[32];
      snprintf(footer, sizeof(footer), "%u nick%s",
          cs.count, cs.count == 1 ? "" : "s");
      cmd_reply(ctx, footer);
    }

    return;
  }

  // Self mode (no args, private): show the caller's own identity.
  id_format_self(ctx, ns, ctx->msg->sender, ctx->msg->metadata);
}

// ------------------------------------------------------------------ //
// /show bot <name> command — kind summary                            //
// ------------------------------------------------------------------ //

#include "colors.h"
#include <time.h>

static void
verb_command_summary(const cmd_ctx_t *ctx)
{
  bot_inst_t *bot = ctx->bot;

  char line[256];
  uint64_t cmds = bot_cmd_count(bot);
  time_t last = bot_last_activity(bot);
  userns_t *ns = bot_get_userns(bot);

  cmd_reply(ctx, CLR_BOLD "command bot" CLR_RESET);

  snprintf(line, sizeof(line), "  commands:   %lu", (unsigned long)cmds);
  cmd_reply(ctx, line);

  if(last == 0)
    cmd_reply(ctx, "  last_seen:  " CLR_GRAY "(never)" CLR_RESET);

  else
  {
    long elapsed = (long)(time(NULL) - last);
    snprintf(line, sizeof(line),
        "  last_seen:  %lds ago", elapsed);
    cmd_reply(ctx, line);
  }

  snprintf(line, sizeof(line),
      "  methods:    %u", bot_method_count(bot));
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  namespace:  %s",
      (ns && ns->name[0]) ? ns->name : "-");
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// Bot driver callbacks                                                //
// ------------------------------------------------------------------ //

static void *
cmdbot_create(bot_inst_t *inst)
{
  const char *prefix;
  cmdbot_state_t *st = mem_alloc("command", "state", sizeof(*st));

  if(st == NULL)
    return(NULL);

  memset(st, 0, sizeof(*st));
  st->inst = inst;

  // Apply default prefix from plugin KV config.
  prefix = kv_get_str("plugin.command.prefix");

  if(prefix != NULL && prefix[0] != '\0')
    cmd_set_prefix(inst, prefix);

  return(st);
}

static void
cmdbot_destroy(void *handle)
{
  if(handle != NULL)
    mem_free(handle);
}

static bool
cmdbot_start(void *handle)
{
  (void)handle;
  return(SUCCESS);
}

static void
cmdbot_stop(void *handle)
{
  (void)handle;
}

static void
cmdbot_on_message(void *handle, const method_msg_t *msg)
{
  cmdbot_state_t *st = handle;

  // Attempt user discovery from the method metadata (e.g., nick!user@host).
  if(msg->metadata[0] != '\0')
    bot_discover_user(st->inst, msg->metadata);

  // MFA matching: refresh existing sessions and autoidentify new ones.
  if(msg->metadata[0] != '\0')
  {
    userns_t *ns = bot_get_userns(st->inst);

    if(ns != NULL)
    {
      const char *mfa_user = userns_mfa_match(ns, msg->metadata);

      if(mfa_user != NULL)
      {
        const char *existing;
        // Update persistent last-seen tracking in the user namespace.
        userns_user_touch_lastseen(ns, mfa_user,
            method_inst_kind(msg->inst), msg->metadata);

        // Refresh identity timestamp if an existing session matches.
        bot_session_refresh_mfa(st->inst, msg->inst, mfa_user);

        // Autoidentify: if no active session exists for this sender
        // and the matched user has autoidentify enabled, create one.
        existing = bot_session_find(st->inst,
            msg->inst, msg->sender);

        if(existing == NULL &&
           userns_user_get_autoidentify(ns, mfa_user))
        {
          if(bot_session_create(st->inst, msg->inst,
              msg->sender, mfa_user) == SUCCESS)
            clam(CLAM_INFO, "autoidentify",
                "'%s' auto-identified from '%s'",
                mfa_user, msg->metadata);

          else
            clam(CLAM_WARN, "autoidentify",
                "failed to create session for '%s' from '%s'",
                mfa_user, msg->metadata);
        }
      }
    }
  }

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

// kind_filter for /show bot <name> :default handler; pinned here rather
// than at function scope to keep cmdbot_init warning-clean under C90
// decl-after-statement rules.
static const char *const command_kind_filter[] = { "command", NULL };

static bool
cmdbot_init(void)
{
  if(cmd_register("command", "identify",
        "identify <username> <password>",
        "Authenticate with the bot",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_PRIVATE, METHOD_T_ANY, cmd_identify, NULL,
        NULL, NULL, cmdbot_ad_identify, 2, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("command", "deauth",
        "deauth",
        "End your authenticated session",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_deauth, NULL, NULL,
        NULL, NULL, 0, NULL, NULL) != SUCCESS)
  {
    cmd_unregister("identify");
    return(FAIL);
  }

  if(cmd_register("command", "register",
        "register <password>",
        "Set password for a discovered account",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_PRIVATE, METHOD_T_ANY, cmd_register_user,
        NULL, NULL, "reg", cmdbot_ad_register, 1, NULL, NULL) != SUCCESS)
  {
    cmd_unregister("deauth");
    cmd_unregister("identify");
    return(FAIL);
  }

  if(cmd_register("command", "id",
        "id [nickname]",
        "Show identity info for yourself, a nick, or the channel",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_id,
        NULL, NULL, NULL, cmdbot_ad_id, 1, NULL, NULL) != SUCCESS)
  {
    cmd_unregister("register");
    cmd_unregister("deauth");
    cmd_unregister("identify");
    return(FAIL);
  }

  // Empty-verb summary handler for /show bot <name> when <name> is a
  // command-kind bot. Registered as the ":default" sentinel child of
  // show/bot; the dispatcher invokes it when no verb token is given
  // and a matching kind-filtered sibling exists.
  if(cmd_register("command", ":default",
        "show bot <name>",
        "Command bot summary (prefix, sessions, bindings)",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_command_summary, NULL, "show/bot", NULL,
        NULL, 0, command_kind_filter, NULL) != SUCCESS)
  {
    cmd_unregister("id");
    cmd_unregister("register");
    cmd_unregister("deauth");
    cmd_unregister("identify");
    return(FAIL);
  }

  return(SUCCESS);
}

static void
cmdbot_deinit(void)
{
  cmd_unregister(":default");
  cmd_unregister("id");
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
