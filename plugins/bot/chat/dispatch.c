// botmanager — MIT
// Command-dispatch half of the chat bot plugin: identity, auth, dispatch.

#define TEXT_DISPATCH_INTERNAL
#include "dispatch.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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

  // Already identified (temp MFA or permanent pattern)?
  if(ctx->username != NULL)
  {
    cmd_reply(ctx, "Already identified. Use deauth first.");
    return;
  }

  ns = bot_get_userns(ctx->bot);

  if(ns == NULL)
  {
    cmd_reply(ctx, "No user namespace configured for this bot.");
    return;
  }

  username = ctx->parsed->argv[0];
  pass = ctx->parsed->argv[1];

  // A temporary MFA is the sender's exact metadata; a method that
  // carries none cannot mint one.
  mfa_str = ctx->msg->metadata;

  if(mfa_str == NULL || mfa_str[0] == '\0')
  {
    clam(CLAM_WARN, "identify",
        "no method metadata for '%s' (sender: %s)",
        username, ctx->msg->sender);
    cmd_reply(ctx, "This method carries no identity metadata; "
        "identify is unavailable here.");
    return;
  }

  // The password is the whole proof — mint a temporary MFA from the
  // caller's current metadata on success. No permanent-pattern gate:
  // identify exists precisely for users whose pattern no longer
  // matches the host they are on.
  result = userns_auth(ns, username, pass, mfa_str);

  switch(result)
  {
    case USERNS_AUTH_OK:
      if(userns_tmfa_add(ns, username, mfa_str) != SUCCESS)
      {
        clam(CLAM_WARN, "identify",
            "auth ok but temp MFA mint failed for '%s' from '%s'",
            username, mfa_str);
        cmd_reply(ctx, "Authentication error.");
        break;
      }

      clam(CLAM_INFO, "identify",
          "'%s' authenticated from '%s'", username, mfa_str);
      cmd_reply(ctx, "Identified. This hostmask is now you, "
          "refreshed by anything you say, until it idles out.");
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
  userns_t *ns;

  if(ctx->username == NULL)
  {
    cmd_reply(ctx, "Not identified.");
    return;
  }

  ns = bot_get_userns(ctx->bot);

  if(ns != NULL && ctx->msg->metadata[0] != '\0' &&
     userns_tmfa_del(ns, ctx->username, ctx->msg->metadata) > 0)
  {
    clam(CLAM_INFO, "deauth",
        "'%s' dropped temporary MFA (sender: %s)",
        ctx->username, ctx->msg->sender);
    cmd_reply(ctx, "Identity dropped for this hostmask.");
  }

  else
    cmd_reply(ctx, "No temporary identity to drop — your identity "
        "comes from a permanent MFA pattern.");
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
    cmd_reply(ctx, "Already authenticated. Use "
        "user password <oldpassword> <newpassword> "
        "to change your password.");
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

  // Auto-identify: mint a temp MFA from the caller's metadata. Their
  // permanent pattern already matched (that is what found the account),
  // so this matters only when autoidentify is off for the user.
  result = userns_auth(ns, matched, password, mfa_str);

  if(result == USERNS_AUTH_OK &&
     userns_tmfa_add(ns, matched, mfa_str) == SUCCESS)
  {
    clam(CLAM_INFO, "register",
        "'%s' registered and authenticated from '%s'", matched, mfa_str);
    cmd_reply(ctx, "Password set. You are now identified.");
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

  // Try to resolve identity: full resolution first, then a bare
  // pattern match (recognized, but autoidentify is off and no temp
  // MFA — visible yet not acting-as).
  char ubuf[USERNS_USER_SZ];

  if(ns != NULL && ctx->msg->inst != NULL)
  {
    if(bot_identity_resolve(ctx->bot, ctx->msg->inst, nick,
        mfa_str[0] != '\0' ? mfa_str : NULL, ubuf, sizeof(ubuf)))
    {
      username = ubuf;
      is_authed = true;
    }

    else if(mfa_str[0] != '\0')
      username = userns_mfa_match(ns, mfa_str);
  }

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

  char ubuf[USERNS_USER_SZ];

  if(ns != NULL && ctx->msg->inst != NULL)
  {
    if(bot_identity_resolve(ctx->bot, ctx->msg->inst, nick,
        mfa_str, ubuf, sizeof(ubuf)))
    {
      username = ubuf;
      is_authed = true;
    }

    else if(mfa_str != NULL && mfa_str[0] != '\0')
      username = userns_mfa_match(ns, mfa_str);
  }

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
// /show bot <name> — command-half rows                                //
// ------------------------------------------------------------------ //

// Column alignment matches the conversational rows emitted around this
// block by show_verbs.c's unified :default verb; keep the two in step.
void
text_dispatch_summary(const cmd_ctx_t *ctx, bot_inst_t *bot)
{
  char line[256];
  uint64_t cmds;
  time_t last;
  userns_t *ns;

  if(ctx == NULL || bot == NULL)
    return;

  cmds = bot_cmd_count(bot);
  last = bot_last_activity(bot);
  ns = bot_get_userns(bot);

  snprintf(line, sizeof(line), "  commands:    %lu", (unsigned long)cmds);
  cmd_reply(ctx, line);

  if(last == 0)
    cmd_reply(ctx, "  last_seen:   " CLR_GRAY "(never)" CLR_RESET);

  else
  {
    long elapsed = (long)(time(NULL) - last);

    snprintf(line, sizeof(line), "  last_seen:   %lds ago", elapsed);
    cmd_reply(ctx, line);
  }

  snprintf(line, sizeof(line), "  namespace:   %s",
      (ns && ns->name[0]) ? ns->name : "-");
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// Per-line identity bookkeeping                                       //
// ------------------------------------------------------------------ //

void
text_identity_observe(bot_inst_t *inst, const method_msg_t *msg)
{
  userns_t *ns;
  const char *mfa_user;

  if(inst == NULL || msg == NULL || msg->metadata[0] == '\0')
    return;

  // Attempt user discovery from the method metadata (e.g. nick!user@host).
  bot_discover_user(inst, msg->metadata);

  // MFA matching: refresh existing sessions and autoidentify new ones.
  ns = bot_get_userns(inst);

  if(ns == NULL)
    return;

  mfa_user = userns_mfa_match(ns, msg->metadata);

  // Update persistent last-seen tracking in the user namespace.
  if(mfa_user != NULL)
    userns_user_touch_lastseen(ns, mfa_user,
        method_inst_kind(msg->inst), msg->metadata);

  // Called for its side effects, not its answer: any witnessed line
  // whose metadata exactly matches a temporary MFA refreshes it, and
  // the lazy expiry (with its one-time notice) fires here too.
  {
    char ubuf[USERNS_USER_SZ];

    (void)bot_identity_resolve(inst, msg->inst, msg->sender,
        msg->metadata, ubuf, sizeof(ubuf));
  }
}

// ------------------------------------------------------------------ //
// Command dispatch                                                    //
// ------------------------------------------------------------------ //

// Mirror of cmd_dispatch's own prefix resolution: a per-method override
// at bot.<bot>.<method-kind>.prefix wins over the bot-level prefix. Keep
// this in step with core/cmd.c — the two must agree on what counts as
// command-shaped, or the conversational half starts seeing command
// traffic (or worse, swallows ordinary talk).
static const char *
text_prefix_for(bot_inst_t *inst, const method_msg_t *msg)
{
  const char *kind;
  const char *bname;
  const char *pfx;
  char key[KV_KEY_SZ];

  if(msg->inst == NULL)
    return(cmd_get_prefix(inst));

  kind  = method_inst_kind(msg->inst);
  bname = bot_inst_name(inst);

  if(kind == NULL || bname == NULL)
    return(cmd_get_prefix(inst));

  snprintf(key, sizeof(key), "bot.%s.%s.prefix", bname, kind);
  pfx = kv_get_str(key);

  return((pfx != NULL && pfx[0] != '\0') ? pfx : cmd_get_prefix(inst));
}

bool
text_dispatch_message(bot_inst_t *inst, const method_msg_t *msg)
{
  const char *prefix;
  const char *rest;
  size_t pfx_len;

  if(inst == NULL || msg == NULL || msg->text[0] == '\0')
    return(false);

  prefix = text_prefix_for(inst, msg);

  if(prefix == NULL || prefix[0] == '\0')
    return(false);

  pfx_len = strlen(prefix);

  if(strncmp(msg->text, prefix, pfx_len) != 0)
    return(false);

  // A bare prefix, or one followed by whitespace, is not a command:
  // cmd_dispatch rejects it and so must we, or "! that was funny" would
  // never reach the conversational half.
  rest = msg->text + pfx_len;

  if(*rest == '\0' || *rest == ' ' || *rest == '\t')
    return(false);

  // The return value is deliberately discarded. An unknown verb or a
  // denied command is still command traffic, and handing either to the
  // LLM would be a surprise; the prefix alone decides ownership.
  (void)cmd_dispatch(inst, msg);
  return(true);
}

// ------------------------------------------------------------------ //
// Command registration                                                //
// ------------------------------------------------------------------ //

bool
text_dispatch_register(void)
{
  if(cmd_register("chat", "identify",
        "identify <username> <password>",
        "Authenticate with the bot",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_PRIVATE, METHOD_T_ANY, cmd_identify, NULL,
        NULL, NULL, text_ad_identify, 2, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("chat", "deauth",
        "deauth",
        "End your authenticated session",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_deauth, NULL, NULL,
        NULL, NULL, 0, NULL, NULL) != SUCCESS)
  {
    cmd_unregister_path("identify");
    return(FAIL);
  }

  if(cmd_register("chat", "register",
        "register <password>",
        "Set password for a discovered account",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_PRIVATE, METHOD_T_ANY, cmd_register_user,
        NULL, NULL, "reg", text_ad_register, 1, NULL, NULL) != SUCCESS)
  {
    cmd_unregister_path("deauth");
    cmd_unregister_path("identify");
    return(FAIL);
  }

  if(cmd_register("chat", "id",
        "id [nickname]",
        "Show identity info for yourself, a nick, or the channel",
        NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_id,
        NULL, NULL, NULL, text_ad_id, 1, NULL, NULL) != SUCCESS)
  {
    cmd_unregister_path("register");
    cmd_unregister_path("deauth");
    cmd_unregister_path("identify");
    return(FAIL);
  }

  return(SUCCESS);
}

void
text_dispatch_unregister(void)
{
  cmd_unregister_path("id");
  cmd_unregister_path("register");
  cmd_unregister_path("identify");
  cmd_unregister_path("deauth");
}
