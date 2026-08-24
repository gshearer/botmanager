// botmanager — MIT
// /user administration commands: create, modify, list, delete.
#define USERNS_CMD_INTERNAL
#include "userns.h"
#include "botmanctl.h"

// Custom validators

// Permission level: 1-5 digits, value 0-65535.
static bool
admin_validate_level(const char *str)
{
  unsigned long v;

  if(!validate_digits(str, 1, 5))
    return false;

  v = strtoul(str, NULL, 10);

  return v <= 65535;
}

// The literal verb in `/set user <username> pass <password>`. It sits
// between two variable arguments, so it cannot be a tree child — the
// resolver only matches a child in the first token after the parent.
static bool
set_user_verb_check(const char *str)
{
  return strcasecmp(str, "pass") == 0;
}

// Namespace resolution

userns_t *
userns_session_resolve(const cmd_ctx_t *ctx)
{
  if(ctx->bot != NULL && ctx->msg != NULL && ctx->msg->inst != NULL)
  {
    userns_t *bound = bot_get_userns(ctx->bot);

    // Identified caller — check their persistent cd override.
    if(bound != NULL && ctx->username != NULL)
    {
      char cd_name[USERNS_NAME_SZ];

      if(userns_user_get_cd(bound, ctx->username,
            cd_name, sizeof(cd_name)) == SUCCESS && cd_name[0] != '\0')
      {
        userns_t *ns = userns_find(cd_name);

        if(ns != NULL)
          return(ns);

        // cd was set but namespace no longer exists — fall through.
      }
    }

    if(bound != NULL)
      return(bound);
  }

  else
  {
    // Session — check per-client override.
    const char *session_ns = botmanctl_get_user_ns();

    if(session_ns[0] != '\0')
    {
      userns_t *ns = userns_find(session_ns);

      if(ns != NULL)
        return(ns);
    }
  }

  cmd_reply(ctx, "no namespace set \xe2\x80\x94 use user cd <namespace>");
  return(NULL);
}

// Argument descriptors (no namespace arg — it comes from cd)

static const cmd_arg_desc_t ad_user_cd[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_user_add[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                USERNS_USER_SZ - 1, NULL },
  { "password", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,                  NULL },
};

static const cmd_arg_desc_t ad_user_del[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_user_addmfa[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                USERNS_USER_SZ - 1, NULL },
  { "pattern",  CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,                  NULL },
};

static const cmd_arg_desc_t ad_user_delmfa[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                USERNS_USER_SZ - 1, NULL },
  { "pattern",  CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,                  NULL },
};

static const cmd_arg_desc_t ad_user_autoidentify[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ - 1, NULL },
  { "on|off",   CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 4,                  NULL },
};

static const cmd_arg_desc_t ad_user_addgroup[] = {
  { "name",        CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                USERNS_GROUP_SZ - 1, NULL },
  { "description", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,                   NULL },
};

static const cmd_arg_desc_t ad_user_delgroup[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_GROUP_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_user_grant[] = {
  { "username", CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, USERNS_USER_SZ - 1,  NULL },
  { "group",    CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, USERNS_GROUP_SZ - 1, NULL },
  { "level",    CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, 0,                   admin_validate_level },
};

static const cmd_arg_desc_t ad_user_revoke[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ - 1,  NULL },
  { "group",    CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_GROUP_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_user_addns[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_user_delns[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ - 1, NULL },
};

// Self-service password change, on the /user side of the tree.
static const cmd_arg_desc_t ad_user_password[] = {
  { "oldpassword", CMD_ARG_NONE, CMD_ARG_REQUIRED,                0, NULL },
  { "newpassword", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

// /set user <username> pass <password> — administrative reset. The
// middle "pass" token is an argument, not a subcommand; see
// set_user_verb_check.
static const cmd_arg_desc_t ad_set_user[] = {
  { "username", CMD_ARG_ALNUM,  CMD_ARG_REQUIRED,                USERNS_USER_SZ - 1, NULL },
  { "pass",     CMD_ARG_CUSTOM, CMD_ARG_REQUIRED,                8,                  set_user_verb_check },
  { "password", CMD_ARG_NONE,   CMD_ARG_REQUIRED | CMD_ARG_REST, 0,                  NULL },
};

static const cmd_arg_desc_t ad_set_user_groupdesc[] = {
  { "groupname",   CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                USERNS_GROUP_SZ - 1, NULL },
  { "description", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,                   NULL },
};

// /show subcommand argument descriptors. /show user takes a username
// and nothing else — it has no verbs.
static const cmd_arg_desc_t ad_show_user[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, USERNS_USER_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_show_group[] = {
  { "groupname", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, USERNS_GROUP_SZ - 1, NULL },
};

// /user parent — show usage and current namespace

static void
cmd_user_parent(const cmd_ctx_t *ctx)
{
  const char *cd_name = NULL;
  char        cd_buf[USERNS_NAME_SZ] = {0};

  if(ctx->bot != NULL && ctx->msg != NULL && ctx->msg->inst != NULL)
  {
    userns_t *bound = bot_get_userns(ctx->bot);

    if(bound != NULL && ctx->username != NULL &&
       userns_user_get_cd(bound, ctx->username,
          cd_buf, sizeof(cd_buf)) == SUCCESS && cd_buf[0] != '\0')
      cd_name = cd_buf;

    else if(bound != NULL)
      cd_name = bound->name;
  }

  else
  {
    const char *session_ns = botmanctl_get_user_ns();

    if(session_ns[0] != '\0')
      cd_name = session_ns;
  }

  if(cd_name != NULL && cd_name[0] != '\0')
  {
    char buf[USERNS_NAME_SZ + 48];
    snprintf(buf, sizeof(buf),
        "working namespace: " CLR_BOLD "%s" CLR_RESET, cd_name);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "no working namespace set");

  cmd_reply(ctx, "subcommands: cd addns delns add del pass addmfa delmfa autoidentify "
      "addgroup delgroup grant revoke");
}

// /user cd <namespace>

static void
cmd_user_cd(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];
  userns_t   *ns;
  char        buf[USERNS_NAME_SZ + 32];

  ns = userns_find(ns_name);

  if(ns == NULL)
  {
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(ctx->bot != NULL && ctx->msg != NULL && ctx->msg->inst != NULL)
  {
    userns_t *bound = bot_get_userns(ctx->bot);

    if(bound == NULL || ctx->username == NULL ||
       userns_user_set_cd(bound, ctx->username, ns_name) != SUCCESS)
    {
      cmd_reply(ctx, "cannot set working namespace: "
          "you must be identified");
      return;
    }
  }

  else
    botmanctl_set_user_ns(ns_name);

  snprintf(buf, sizeof(buf), "namespace set to %s", ns_name);
  cmd_reply(ctx, buf);
}

// /user add <username> <password>

static void
cmd_user_add(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;
  const char *password;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  password = ctx->parsed->argv[1];

  if(userns_user_create(ns, username, password) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user created: %s", username);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to create user (bad name, password policy, "
        "or duplicate)");
}

// /user del <username>

static void
cmd_user_del(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];

  if(userns_user_delete(ns, username) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user deleted: %s", username);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to delete user (not found?)");
}

// /user addmfa <username> <pattern>

static void
cmd_user_addmfa(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;
  const char *pattern;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  pattern  = ctx->parsed->argv[1];

  if(userns_user_add_mfa(ns, username, pattern) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "MFA pattern added for %s", username);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to add MFA pattern (invalid format, "
        "duplicate, user not found, or limit reached)");
}

// /user delmfa <username> <pattern>

static void
cmd_user_delmfa(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;
  const char *pattern;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  pattern  = ctx->parsed->argv[1];

  if(userns_user_remove_mfa(ns, username, pattern) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "MFA pattern removed for %s", username);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to remove MFA pattern (not found?)");
}

// /user autoidentify <username> <on|off>

static void
cmd_user_autoidentify(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;
  const char *toggle;
  bool        enable;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  toggle   = ctx->parsed->argv[1];

  if(strcasecmp(toggle, "on") == 0)
    enable = true;
  else if(strcasecmp(toggle, "off") == 0)
    enable = false;
  else
  {
    cmd_reply(ctx, "usage: user autoidentify <username> <on|off>");
    return;
  }

  if(userns_user_set_autoidentify(ns, username, enable) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 48];
    snprintf(buf, sizeof(buf), "autoidentify %s for %s",
        enable ? "enabled" : "disabled", username);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to set autoidentify (user not found?)");
}

// /user addgroup <name> [description]

static void
cmd_user_addgroup(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *group;
  const char *desc;
  bool        ok;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  group = ctx->parsed->argv[0];
  desc  = (ctx->parsed->argc > 1) ? ctx->parsed->argv[1] : NULL;

  if(desc != NULL)
    ok = userns_group_create_desc(ns, group, desc);
  else
    ok = userns_group_create(ns, group);

  if(ok == SUCCESS)
  {
    char buf[USERNS_GROUP_SZ + 32];
    snprintf(buf, sizeof(buf), "group created: %s", group);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to create group (bad name or duplicate)");
}

// /user delgroup <name>

static void
cmd_user_delgroup(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *group;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  group = ctx->parsed->argv[0];

  if(userns_group_is_builtin(group))
  {
    cmd_reply(ctx, "cannot delete built-in group");
    return;
  }

  if(userns_group_delete(ns, group) == SUCCESS)
  {
    char buf[USERNS_GROUP_SZ + 32];
    snprintf(buf, sizeof(buf), "group deleted: %s", group);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to delete group (not found?)");
}

// /user grant <username> <group> <level>

static void
cmd_user_grant(const cmd_ctx_t *ctx)
{
  userns_t      *ns;
  const char    *username;
  const char    *group;
  unsigned long  level;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  group    = ctx->parsed->argv[1];
  level    = strtoul(ctx->parsed->argv[2], NULL, 10);

  // If already a member, update the level. Otherwise, add.
  if(userns_member_check(ns, username, group))
  {
    if(userns_member_set_level(ns, username, group, (uint16_t)level)
        == SUCCESS)
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "%s level in %s set to %lu",
          username, group, level);
      cmd_reply(ctx, buf);
    }

    else
      cmd_reply(ctx, "failed to update level");
  }

  else
  {
    if(userns_member_add(ns, username, group, (uint16_t)level) == SUCCESS)
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "%s added to %s at level %lu",
          username, group, level);
      cmd_reply(ctx, buf);
    }

    else
      cmd_reply(ctx, "failed to grant membership (user or group "
          "not found?)");
  }
}

// /user revoke <username> <group>

static void
cmd_user_revoke(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;
  const char *group;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  group    = ctx->parsed->argv[1];

  if(userns_member_remove(ns, username, group) == SUCCESS)
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s removed from %s", username, group);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to revoke membership (not a member?)");
}

// /user addns <namespace> — create a user namespace

static void
cmd_user_addns(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];
  userns_t   *ns;
  char        buf[USERNS_NAME_SZ + 48];

  // Check if it already exists.
  if(userns_find(ns_name) != NULL)
  {
    snprintf(buf, sizeof(buf), "namespace already exists: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  ns = userns_get(ns_name);

  if(ns == NULL)
  {
    cmd_reply(ctx, "failed to create namespace");
    return;
  }

  snprintf(buf, sizeof(buf), "created namespace: %s", ns_name);
  cmd_reply(ctx, buf);

  // Auto-cd into the new namespace.
  if(ctx->bot != NULL && ctx->msg != NULL && ctx->msg->inst != NULL)
  {
    userns_t *bound = bot_get_userns(ctx->bot);

    if(bound != NULL && ctx->username != NULL)
      userns_user_set_cd(bound, ctx->username, ns_name);
  }

  else
    botmanctl_set_user_ns(ns_name);

  snprintf(buf, sizeof(buf), "namespace set to %s", ns_name);
  cmd_reply(ctx, buf);
}

// /user delns <namespace> — delete a user namespace

static void
cmd_user_delns(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];

  if(userns_find(ns_name) == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  // Clear userns on any bots bound to this namespace.
  bot_clear_userns(ns_name);

  if(userns_delete(ns_name) == SUCCESS)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "deleted namespace: %s", ns_name);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to delete namespace");
}

// /show users — colorized table of all users in working namespace

typedef struct
{
  const cmd_ctx_t *ctx;
  const userns_t  *ns;
  uint32_t         count;
} show_users_state_t;

static void
fmt_relative_time(time_t ts, char *out, size_t sz)
{
  time_t now;
  long   delta;

  if(ts <= 0)
  {
    snprintf(out, sz, CLR_GRAY "never" CLR_RESET);
    return;
  }

  now   = time(NULL);
  delta = (long)(now - ts);

  if(delta < 0) delta = 0;

  if(delta < 60)
    snprintf(out, sz, "%lds ago", delta);
  else if(delta < 3600)
    snprintf(out, sz, "%ldm ago", delta / 60);
  else if(delta < 86400)
    snprintf(out, sz, "%ldh ago", delta / 3600);
  else
    snprintf(out, sz, "%ldd ago", delta / 86400);
}

static void
show_users_cb(const char *username, const char *uuid,
    const char *description, void *data)
{
  show_users_state_t *st = data;
  time_t              ls_ts = 0;
  char                ls_method[64] = {0};
  char                ls_mfa[USERNS_MFA_PATTERN_SZ] = {0};
  char                age[32];
  const char         *desc;
  char                namecol[64];
  char                line[512];

  (void)uuid;

  userns_user_get_lastseen(st->ns, username,
      &ls_ts, ls_method, sizeof(ls_method),
      ls_mfa, sizeof(ls_mfa));

  fmt_relative_time(ls_ts, age, sizeof(age));

  desc = (description != NULL && description[0] != '\0')
      ? description
      : CLR_GRAY "\xe2\x80\x94" CLR_RESET;

  // Right-pad the bold name to a visible width of 18 by accounting for
  // the embedded color codes (4 ESC bytes).
  snprintf(namecol, sizeof(namecol),
      CLR_BOLD "%-18s" CLR_RESET, username);

  snprintf(line, sizeof(line),
      "  %s  %-12s  %-32s",
      namecol, age, desc);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_users(const cmd_ctx_t *ctx)
{
  userns_t          *ns;
  char               hdr[USERNS_NAME_SZ + 64];
  show_users_state_t st;

  ns = userns_session_resolve(ctx);
  if(ns == NULL) return;

  snprintf(hdr, sizeof(hdr),
      "users in " CLR_BOLD "%s" CLR_RESET ":", ns->name);
  cmd_reply(ctx, hdr);

  cmd_reply(ctx,
      "  " CLR_BOLD "USERNAME           LAST SEEN     DESCRIPTION" CLR_RESET);

  st = (show_users_state_t){ .ctx = ctx, .ns = ns, .count = 0 };
  userns_user_iterate(ns, show_users_cb, &st);

  {
    char footer[64];

    if(st.count == 0)
    {
      cmd_reply(ctx, "  " CLR_GRAY "(none)" CLR_RESET);
      return;
    }

    snprintf(footer, sizeof(footer),
        CLR_GRAY "%u user%s" CLR_RESET,
        st.count, st.count == 1 ? "" : "s");
    cmd_reply(ctx, footer);
  }
}

// /show user [username] — list all users, or detailed info on one user

static void
show_user_membership_cb(const char *group, uint16_t level, void *data)
{
  userns_cmd_list_state_t *st = data;
  char line[128];

  snprintf(line, sizeof(line), "    %-16s level %u",
      group, (unsigned)level);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
show_user_mfa_cb(const char *pattern, void *data)
{
  userns_cmd_mfa_state_t *st = data;
  char line[USERNS_MFA_PATTERN_SZ + 8];

  snprintf(line, sizeof(line), "    %s", pattern);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /show user <name> -- single-user detail. This is an auth view only:
// what the bot has *learned* about a human hangs off a dossier, not a
// userns user, and is read with /show dossier <name>.

static void
cmd_show_user(const cmd_ctx_t *ctx)
{
  userns_t               *ns;
  const char             *username;
  char                    uuid[USERNS_UUID_SZ] = {0};
  char                    desc[USERNS_DESC_SZ] = {0};
  char                    hdr[USERNS_NAME_SZ + USERNS_USER_SZ + 32];
  char                    line[256];
  time_t                  ls_ts = 0;
  char                    ls_method[64] = {0};
  char                    ls_mfa[USERNS_MFA_PATTERN_SZ] = {0};
  userns_cmd_list_state_t gi;
  userns_cmd_mfa_state_t  mi;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  // No argument: print usage.
  if(ctx->parsed == NULL || ctx->parsed->argc == 0)
  {
    cmd_reply(ctx,
        "usage: show user <username>");
    cmd_reply(ctx,
        CLR_GRAY "(use show users to list all users;"
        " show dossier <name> for what the bot has learned)" CLR_RESET);
    return;
  }

  // With argument: detailed info on one user.
  username = ctx->parsed->argv[0];

  if(!userns_user_exists(ns, username))
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user not found: %s", username);
    cmd_reply(ctx, buf);
    return;
  }

  userns_user_get_info(ns, username, uuid, sizeof(uuid),
      desc, sizeof(desc));

  snprintf(hdr, sizeof(hdr),
      "user " CLR_BOLD "%s" CLR_RESET " in %s:", username, ns->name);
  cmd_reply(ctx, hdr);

  snprintf(line, sizeof(line),
      "  " CLR_CYAN "UUID:" CLR_RESET "        %s",
      uuid[0] != '\0' ? uuid : "(none)");
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  " CLR_CYAN "Description:" CLR_RESET " %s",
      desc[0] != '\0' ? desc : "(none)");
  cmd_reply(ctx, line);

  // Last seen info.
  if(userns_user_get_lastseen(ns, username,
      &ls_ts, ls_method, sizeof(ls_method),
      ls_mfa, sizeof(ls_mfa)) == SUCCESS && ls_ts > 0)
  {
    struct tm tm_buf;
    char      ts_str[64];
    char      ls_line[512];

    localtime_r(&ls_ts, &tm_buf);
    strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S %Z", &tm_buf);

    snprintf(ls_line, sizeof(ls_line),
        "  " CLR_CYAN "Last seen:" CLR_RESET "  %s (%s -> %s)",
        ts_str, ls_method, ls_mfa);
    cmd_reply(ctx, ls_line);
  }

  else
  {
    snprintf(line, sizeof(line),
        "  " CLR_CYAN "Last seen:" CLR_RESET "  (never)");
    cmd_reply(ctx, line);
  }

  cmd_reply(ctx, "  " CLR_CYAN "Groups:" CLR_RESET);

  gi = (userns_cmd_list_state_t){ .ctx = ctx, .count = 0 };
  userns_membership_iterate(ns, username, show_user_membership_cb, &gi);

  if(gi.count == 0)
    cmd_reply(ctx, "    (none)");

  cmd_reply(ctx, "  " CLR_CYAN "MFA patterns:" CLR_RESET);

  mi = (userns_cmd_mfa_state_t){ .ctx = ctx, .count = 0 };
  userns_user_list_mfa(ns, username, show_user_mfa_cb, &mi);

  if(mi.count == 0)
    cmd_reply(ctx, "    (none)");
}

// /show userns — list all defined namespaces

static void
show_userns_cb(const char *name, void *data)
{
  userns_cmd_list_state_t *st = data;
  char line[USERNS_NAME_SZ + 8];

  snprintf(line, sizeof(line), "  %s", name);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_userns(const cmd_ctx_t *ctx)
{
  userns_cmd_list_state_t st;

  cmd_reply(ctx,
      CLR_BOLD "NAMESPACE" CLR_RESET);

  st = (userns_cmd_list_state_t){ .ctx = ctx, .count = 0 };
  userns_iterate(show_userns_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
  else
  {
    char footer[32];
    snprintf(footer, sizeof(footer), "%u namespace%s",
        st.count, st.count == 1 ? "" : "s");
    cmd_reply(ctx, footer);
  }
}

// /show group [name] — list all groups, or members of a specific group

static void
show_groups_cb(const char *name, const char *description, void *data)
{
  userns_cmd_group_state_t *st = data;
  char line[256];

  if(description[0] != '\0')
    snprintf(line, sizeof(line), "  %-24s %s",
        name, description);

  else
    snprintf(line, sizeof(line), "  %-24s " CLR_GRAY "\xe2\x80\x94" CLR_RESET,
        name);

  cmd_reply(st->ctx, line);
  st->count++;
}

static void
show_group_members_cb(const char *username, uint16_t level, void *data)
{
  userns_cmd_list_state_t *st = data;
  char line[128];

  snprintf(line, sizeof(line), "  %-24s %5u", username, (unsigned)level);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_group(const cmd_ctx_t *ctx)
{
  userns_t                *ns;
  userns_cmd_group_state_t st;
  char                     hdr[USERNS_NAME_SZ + 64];

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  // If a groupname argument is provided, show members of that group.
  if(ctx->parsed != NULL && ctx->parsed->argc > 0 &&
     ctx->parsed->argv[0] != NULL && ctx->parsed->argv[0][0] != '\0')
  {
    const char             *groupname = ctx->parsed->argv[0];
    char                    ghdr[USERNS_NAME_SZ + USERNS_GROUP_SZ + 48];
    userns_cmd_list_state_t gst;

    if(!userns_group_exists(ns, groupname))
    {
      char buf[USERNS_GROUP_SZ + 32];
      snprintf(buf, sizeof(buf), "group not found: %s", groupname);
      cmd_reply(ctx, buf);
      return;
    }

    snprintf(ghdr, sizeof(ghdr),
        "members of group " CLR_BOLD "%s" CLR_RESET " in %s:",
        groupname, ns->name);
    cmd_reply(ctx, ghdr);

    cmd_reply(ctx,
        "  " CLR_BOLD "USERNAME                 LEVEL" CLR_RESET);

    gst = (userns_cmd_list_state_t){ .ctx = ctx, .count = 0 };
    userns_group_members_iterate(ns, groupname,
        show_group_members_cb, &gst);

    if(gst.count == 0)
      cmd_reply(ctx, "  (none)");

    return;
  }

  // No argument — list all groups.
  st = (userns_cmd_group_state_t){ .ctx = ctx, .count = 0 };

  snprintf(hdr, sizeof(hdr),
      "groups in " CLR_BOLD "%s" CLR_RESET ":", ns->name);
  cmd_reply(ctx, hdr);

  cmd_reply(ctx,
      "  " CLR_BOLD "GROUP                    DESCRIPTION" CLR_RESET);

  userns_group_iterate(ns, show_groups_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
  else
  {
    char footer[32];
    snprintf(footer, sizeof(footer), "%u group%s",
        st.count, st.count == 1 ? "" : "s");
    cmd_reply(ctx, footer);
  }
}

// /set user <username> pass <password> — administrative reset. The
// caller's admin rights are the authority, so no old password is
// required; this is the lockout-recovery path for an account whose
// holder cannot log in. The self-service twin is `user password`.
//
// The password is on the command line, so this path is private-only like
// its twin. The gate cannot live in the registration: `set user` also
// carries the `groupdesc` child, which is a public-safe command sharing
// the one registration — hence the same test the dispatcher applies,
// applied here to the pass path alone.

static void
cmd_set_user(const cmd_ctx_t *ctx)
{
  char        buf[USERNS_USER_SZ + 32];
  userns_t   *ns;
  const char *username;
  const char *password;

  if(ctx->msg != NULL && ctx->msg->inst != NULL
      && ctx->msg->channel[0] != '\0')
  {
    clam(CLAM_WARN, "set user pass",
        "'%s' refused: password reset attempted in public channel '%s'",
        ctx->username != NULL ? ctx->username : "(unknown)",
        ctx->msg->channel);

    cmd_reply(ctx, "This command can only be used in private messages — "
        "and the password you just typed is now public. Choose another.");
    return;
  }

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->parsed->argv[0];
  password = ctx->parsed->argv[2];

  if(!userns_user_exists(ns, username))
  {
    snprintf(buf, sizeof(buf), "user not found: %s", username);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_password_check(password) != SUCCESS)
  {
    cmd_reply(ctx, "password does not meet the password policy");
    return;
  }

  if(userns_user_reset_password(ns, username, password) != SUCCESS)
  {
    cmd_reply(ctx, "failed to reset password");
    return;
  }

  // userns_user_reset_password logs the subject; the actor is only
  // knowable here, and an admin resetting someone else's credential is
  // exactly the event an audit reader wants attributed.
  clam(CLAM_INFO, "set user pass",
      "'%s' reset the password for '%s' in '%s'",
      ctx->username != NULL ? ctx->username : "(unknown)",
      username, ns->name);

  snprintf(buf, sizeof(buf), "password reset for %s", username);
  cmd_reply(ctx, buf);
}

// user password <oldpassword> <newpassword> — change your own password.
// The current password must be supplied and verify: an authenticated
// session alone is not sufficient authority to rotate a credential.
// Private-scoped, because both passwords travel in the command line.

static void
cmd_user_password(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *username;
  const char *old_password;
  const char *new_password;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  username = ctx->username;

  if(username == NULL || username[0] == '\0')
  {
    cmd_reply(ctx, "cannot determine your identity — are you authenticated?");
    return;
  }

  old_password = ctx->parsed->argv[0];
  new_password = ctx->parsed->argv[1];

  // Policy is checked here as well as inside the setter so a rejected
  // new password is distinguishable from a wrong current password.
  if(userns_password_check(new_password) != SUCCESS)
  {
    cmd_reply(ctx, "new password does not meet the password policy");
    return;
  }

  if(userns_user_set_password(ns, username, old_password,
        new_password) == SUCCESS)
    cmd_reply(ctx, "password changed");

  else
    cmd_reply(ctx, "failed to change password — current password incorrect");
}

// /set user groupdesc <groupname> <description>

static void
cmd_set_user_groupdesc(const cmd_ctx_t *ctx)
{
  userns_t   *ns;
  const char *groupname;
  const char *description;

  ns = userns_session_resolve(ctx);

  if(ns == NULL)
    return;

  groupname   = ctx->parsed->argv[0];
  description = ctx->parsed->argv[1];

  if(!userns_group_exists(ns, groupname))
  {
    char buf[USERNS_GROUP_SZ + 32];
    snprintf(buf, sizeof(buf), "group not found: %s", groupname);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_group_set_description(ns, groupname, description) == SUCCESS)
  {
    char buf[USERNS_GROUP_SZ + 48];
    snprintf(buf, sizeof(buf), "description updated for group %s", groupname);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to update group description");
}

// Registration

static const cmd_decl_t user_decl = {
  .module      = "userns",
  .name        = "user",
  .usage       = "user <subcommand> ...",
  .description = "User and group management",
  .help_long   =
      "Manages namespaces, users, groups, MFA patterns, and permissions.\n"
      "Set the working namespace with user cd <namespace>.\n"
      "Subcommands: cd addns delns add del password addmfa delmfa\n"
      "autoidentify addgroup delgroup grant revoke",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_parent,
  .abbrev      = "u",
};

static const cmd_decl_t user_cd_decl = {
  .module      = "userns",
  .name        = "cd",
  .usage       = "user cd <namespace>",
  .description = "Set working namespace",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_cd,
  .parent_path = "user",
  .arg_desc    = ad_user_cd,
  .arg_count   = 1,
};

static const cmd_decl_t user_addns_decl = {
  .module      = "userns",
  .name        = "addns",
  .usage       = "user addns <namespace>",
  .description = "Create a user namespace",
  .help_long   =
      "Creates a new user namespace with the given name. The namespace\n"
      "is seeded with built-in groups (owner, admin, user, everyone)\n"
      "and the @owner user.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_addns,
  .parent_path = "user",
  .arg_desc    = ad_user_addns,
  .arg_count   = 1,
};

static const cmd_decl_t user_delns_decl = {
  .module      = "userns",
  .name        = "delns",
  .usage       = "user delns <namespace>",
  .description = "Delete a user namespace",
  .help_long   = "Deletes a user namespace and all its users, groups, and\n"
                 "memberships. Any bots bound to this namespace will have\n"
                 "their binding cleared.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_delns,
  .parent_path = "user",
  .arg_desc    = ad_user_delns,
  .arg_count   = 1,
};

static const cmd_decl_t user_add_decl = {
  .module      = "userns",
  .name        = "add",
  .usage       = "user add <username> <password>",
  .description = "Create a user",
  .help_long   =
      "Creates a new user in the working namespace. The password\n"
      "must meet the password policy (length, uppercase, lowercase,\n"
      "digit, and symbol). The user is automatically added to the\n"
      "everyone and user groups at level 0.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_add,
  .parent_path = "user",
  .arg_desc    = ad_user_add,
  .arg_count   = 2,
};

static const cmd_decl_t user_del_decl = {
  .module      = "userns",
  .name        = "del",
  .usage       = "user del <username>",
  .description = "Delete a user",
  .help_long   =
      "Deletes a user and all their group memberships. This action\n"
      "cannot be undone.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_del,
  .parent_path = "user",
  .arg_desc    = ad_user_del,
  .arg_count   = 1,
};

static const cmd_decl_t user_password_decl = {
  .module      = "userns",
  .name        = "password",
  .usage       = "user password <oldpassword> <newpassword>",
  .description = "Change your own password",
  .help_long   =
      "Changes the password of the currently authenticated user. The\n"
      "current password must be supplied and verify, and the new one\n"
      "must meet the password policy. Private messages only, since\n"
      "both passwords appear in the command line.",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_PRIVATE,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_password,
  .parent_path = "user",
  .arg_desc    = ad_user_password,
  .arg_count   = 2,
};

static const cmd_decl_t user_addmfa_decl = {
  .module      = "userns",
  .name        = "addmfa",
  .usage       = "user addmfa <username> <pattern>",
  .description = "Add an MFA pattern for a user",
  .help_long   = "Adds an MFA pattern for the specified user. Pattern must\n"
                 "be in handle!username@hostname format with glob (* and ?)\n"
                 "supported in handle and hostname. Security constraints:\n"
                 "at least 3 non-glob handle chars and 6 non-glob hostname\n"
                 "chars. Regular expressions are not supported.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_addmfa,
  .parent_path = "user",
  .arg_desc    = ad_user_addmfa,
  .arg_count   = 2,
};

static const cmd_decl_t user_delmfa_decl = {
  .module      = "userns",
  .name        = "delmfa",
  .usage       = "user delmfa <username> <pattern>",
  .description = "Remove an MFA pattern from a user",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_delmfa,
  .parent_path = "user",
  .arg_desc    = ad_user_delmfa,
  .arg_count   = 2,
};

static const cmd_decl_t user_autoidentify_decl = {
  .module      = "userns",
  .name        = "autoidentify",
  .usage       = "user autoidentify <username> <on|off>",
  .description = "Enable or disable autoidentify for a user",
  .help_long   =
      "When autoidentify is enabled, the bot automatically creates\n"
      "an authenticated session for a user whose MFA pattern matches\n"
      "an incoming message, without requiring `identify`.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_autoidentify,
  .parent_path = "user",
  .abbrev      = "ai",
  .arg_desc    = ad_user_autoidentify,
  .arg_count   = 2,
};

static const cmd_decl_t user_addgroup_decl = {
  .module      = "userns",
  .name        = "addgroup",
  .usage       = "user addgroup <name> <description>",
  .description = "Create a group",
  .help_long   = "Creates a new group in the working namespace with the\n"
                 "given name and description.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_addgroup,
  .parent_path = "user",
  .abbrev      = "ag",
  .arg_desc    = ad_user_addgroup,
  .arg_count   = 2,
};

static const cmd_decl_t user_delgroup_decl = {
  .module      = "userns",
  .name        = "delgroup",
  .usage       = "user delgroup <name>",
  .description = "Delete a group",
  .help_long   = "Deletes a group and all its memberships. Built-in groups\n"
                 "(owner, admin, user, everyone) cannot be deleted.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_delgroup,
  .parent_path = "user",
  .abbrev      = "dg",
  .arg_desc    = ad_user_delgroup,
  .arg_count   = 1,
};

static const cmd_decl_t user_grant_decl = {
  .module      = "userns",
  .name        = "grant",
  .usage       = "user grant <username> <group> <level>",
  .description = "Grant group membership",
  .help_long   = "Adds a user to a group with the specified privilege level\n"
                 "(0-65535). If already a member, updates the level.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_grant,
  .parent_path = "user",
  .abbrev      = "gr",
  .arg_desc    = ad_user_grant,
  .arg_count   = 3,
};

static const cmd_decl_t user_revoke_decl = {
  .module      = "userns",
  .name        = "revoke",
  .usage       = "user revoke <username> <group>",
  .description = "Revoke group membership",
  .help_long   = "Removes a user from a group.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_user_revoke,
  .parent_path = "user",
  .abbrev      = "re",
  .arg_desc    = ad_user_revoke,
  .arg_count   = 2,
};

static const cmd_decl_t show_users_decl = {
  .module      = "userns",
  .name        = "users",
  .usage       = "show users",
  .description = "List users in the working namespace",
  .help_long   = "Renders a colorized table of every user in the working\n"
                 "namespace with their last-seen time and description.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_users,
  .parent_path = "show",
  .abbrev      = "us",
};

static const cmd_decl_t show_user_decl = {
  .module      = "userns",
  .name        = "user",
  .usage       = "show user <username>",
  .description = "Show user details",
  .help_long   =
      "With <username>, displays UUID, description, group memberships,\n"
      "and MFA patterns. For the namespace-wide user list, use\n"
      "/show users. This is the auth record only — for the facts and\n"
      "conversation the bot has accumulated, use show dossier <name>.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_user,
  .parent_path = "show",
  .abbrev      = "u",
  .arg_desc    = ad_show_user,
  .arg_count   = (uint8_t)(sizeof(ad_show_user) / sizeof(ad_show_user[0])),
};

static const cmd_decl_t show_userns_decl = {
  .module      = "userns",
  .name        = "userns",
  .usage       = "show userns",
  .description = "List all namespaces",
  .help_long   = "Lists all defined user namespaces.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_userns,
  .parent_path = "show",
  .abbrev      = "ns",
};

static const cmd_decl_t show_group_decl = {
  .module      = "userns",
  .name        = "group",
  .usage       = "show group [name]",
  .description = "List groups or show group members",
  .help_long   = "Without arguments, lists all groups. With a group name,\n"
                 "shows the members and their privilege levels.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_group,
  .parent_path = "show",
  .abbrev      = "gr",
  .arg_desc    = ad_show_group,
  .arg_count   = 1,
};

static const cmd_decl_t set_user_decl = {
  .module      = "userns",
  .name        = "user",
  .usage       = "set user <username> pass <password>",
  .description = "Set a user's password",
  .help_long   =
      "Resets another user's password without their old password, on\n"
      "the authority of your admin rights. Use this to recover an\n"
      "account whose holder is locked out; users change their own\n"
      "password with user password. Private messages only — the new\n"
      "password appears on the command line.\n"
      "Subcommand: groupdesc",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_set_user,
  .parent_path = "set",
  .abbrev      = "u",
  .arg_desc    = ad_set_user,
  .arg_count   = 3,
};

static const cmd_decl_t set_user_groupdesc_decl = {
  .module      = "userns",
  .name        = "groupdesc",
  .usage       = "set user groupdesc <group> <description>",
  .description = "Set a group's description",
  .help_long   = "Updates the description for an existing group in the\n"
                 "working namespace.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_set_user_groupdesc,
  .parent_path = "set/user",
  .arg_desc    = ad_set_user_groupdesc,
  .arg_count   = 2,
};

void
userns_register_commands(void)
{
  cmd_register(&user_decl);

  // Children of /user.
  cmd_register(&user_cd_decl);
  cmd_register(&user_addns_decl);
  cmd_register(&user_delns_decl);
  cmd_register(&user_add_decl);
  cmd_register(&user_del_decl);

  // Self-service. Every other /user child is admin-gated; permission is
  // checked on the resolved leaf, not the parent chain, so an ordinary
  // user reaches this one and nothing else under /user.
  cmd_register(&user_password_decl);
  cmd_register(&user_addmfa_decl);
  cmd_register(&user_delmfa_decl);
  cmd_register(&user_autoidentify_decl);
  cmd_register(&user_addgroup_decl);
  cmd_register(&user_delgroup_decl);
  cmd_register(&user_grant_decl);
  cmd_register(&user_revoke_decl);

  // /show users — table of all users in the working namespace.
  cmd_register(&show_users_decl);

  // /show user <name> — single-user detail.
  cmd_register(&show_user_decl);
  cmd_register(&show_userns_decl);
  cmd_register(&show_group_decl);

  // /set user — administrative user settings. Unlike the rest of the
  // tree this command carries its own arguments as well as a child:
  // `set user <username> pass <password>` puts a variable in the slot
  // where the resolver looks for a subcommand, so anything that is not
  // the literal "groupdesc" falls through to the argument parser.
  cmd_register(&set_user_decl);
  cmd_register(&set_user_groupdesc_decl);
}
