#define ADMIN_INTERNAL
#include "admin.h"

// -----------------------------------------------------------------------
// Custom validators
// -----------------------------------------------------------------------

// Permission level: 1-5 digits, value 0-65535.
static bool
admin_validate_level(const char *str)
{
  if(!validate_digits(str, 1, 5))
    return false;

  unsigned long v = strtoul(str, NULL, 10);

  return v <= 65535;
}

// -----------------------------------------------------------------------
// Argument descriptors
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_useradd[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_NAME_SZ, NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_USER_SZ, NULL },
  { "password",  CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,            NULL },
};

static const cmd_arg_desc_t ad_ns_user[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ, NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ, NULL },
};

static const cmd_arg_desc_t ad_ns[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_passwd[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_NAME_SZ, NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_USER_SZ, NULL },
  { "password",  CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,            NULL },
};

static const cmd_arg_desc_t ad_groupadd[] = {
  { "namespace",   CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_NAME_SZ, NULL },
  { "name",        CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_GROUP_SZ, NULL },
  { "description", CMD_ARG_NONE,  CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,            NULL },
};

static const cmd_arg_desc_t ad_grant[] = {
  { "namespace", CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, USERNS_NAME_SZ,  NULL },
  { "username",  CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, USERNS_USER_SZ,  NULL },
  { "group",     CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, USERNS_GROUP_SZ, NULL },
  { "level",     CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, 0,               admin_validate_level },
};

static const cmd_arg_desc_t ad_revoke[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ,  NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ,  NULL },
  { "group",     CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_GROUP_SZ, NULL },
};

static const cmd_arg_desc_t ad_mfa_add[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_NAME_SZ, NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_USER_SZ, NULL },
  { "pattern",   CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,            NULL },
};

static const cmd_arg_desc_t ad_mfa_list[] = {
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ, NULL },
  { "username",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ, NULL },
};

// -----------------------------------------------------------------------
// /useradd <namespace> <username> <password>
// -----------------------------------------------------------------------
static void
admin_cmd_useradd(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *password = ctx->parsed->argv[2];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_user_create(ns, username, password) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user created: %s", username);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to create user (bad name, password policy, "
        "or duplicate)");
  }
}

// -----------------------------------------------------------------------
// /userdel <namespace> <username>
// -----------------------------------------------------------------------
static void
admin_cmd_userdel(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_user_delete(ns, username) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user deleted: %s", username);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to delete user (not found?)");
  }
}

// -----------------------------------------------------------------------
// /userlist <namespace>
// -----------------------------------------------------------------------


// User iteration callback: formats and emits one user entry.
// returns: void
// username: user's name
// uuid: user's unique ID (unused)
// description: user's description string (may be empty)
// data: pointer to userlist_state_t with ctx and running count
static void
userlist_cb(const char *username, const char *uuid,
    const char *description, void *data)
{
  (void)uuid;
  userlist_state_t *st = data;
  char line[USERNS_USER_SZ + USERNS_DESC_SZ + 16];

  if(description[0] != '\0')
    snprintf(line, sizeof(line), "  %s — %s", username, description);
  else
    snprintf(line, sizeof(line), "  %s", username);

  cmd_reply(st->ctx, line);
  st->count++;
}

// /userlist <namespace> — list all users in a namespace.
// returns: void
// ctx: command context with parsed namespace argument
static void
admin_cmd_userlist(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  userlist_state_t st = { .ctx = ctx, .count = 0 };

  char hdr[USERNS_NAME_SZ + 32];
  snprintf(hdr, sizeof(hdr), "users in %s:", ns_name);
  cmd_reply(ctx, hdr);

  userns_user_iterate(ns, userlist_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// /userinfo <namespace> <username>
// -----------------------------------------------------------------------


// Membership iteration callback: emits one group membership line.
// returns: void
// group: group name the user belongs to
// level: privilege level within the group
// data: pointer to userinfo_state_t with ctx and running count
static void
userinfo_membership_cb(const char *group, uint16_t level, void *data)
{
  userinfo_state_t *st = data;
  char line[USERNS_GROUP_SZ + 32];

  snprintf(line, sizeof(line), "  %s (level %u)", group, (unsigned)level);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /userinfo <namespace> <username> — show user details and memberships.
// returns: void
// ctx: command context with parsed namespace and username arguments
static void
admin_cmd_userinfo(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(!userns_user_exists(ns, username))
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user not found: %s", username);
    cmd_reply(ctx, buf);
    return;
  }

  char hdr[USERNS_NAME_SZ + USERNS_USER_SZ + 16];
  snprintf(hdr, sizeof(hdr), "user %s in %s:", username, ns_name);
  cmd_reply(ctx, hdr);

  cmd_reply(ctx, "groups:");

  userinfo_state_t st = { .ctx = ctx, .count = 0 };

  userns_membership_iterate(ns, username, userinfo_membership_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// /passwd <namespace> <username> <newpassword>
// -----------------------------------------------------------------------
static void
admin_cmd_passwd(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *password = ctx->parsed->argv[2];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_user_reset_password(ns, username, password) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "password reset for %s", username);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to reset password (user not found or "
        "password policy violation)");
  }
}

// -----------------------------------------------------------------------
// /groupadd <namespace> <name> [description]
// -----------------------------------------------------------------------
static void
admin_cmd_groupadd(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];
  const char *group   = ctx->parsed->argv[1];
  const char *desc    = (ctx->parsed->argc > 2) ? ctx->parsed->argv[2] : NULL;

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  bool ok;

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
  {
    cmd_reply(ctx, "failed to create group (bad name or duplicate)");
  }
}

// -----------------------------------------------------------------------
// /groupdel <namespace> <name>
// -----------------------------------------------------------------------
static void
admin_cmd_groupdel(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];
  const char *group   = ctx->parsed->argv[1];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

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
  {
    cmd_reply(ctx, "failed to delete group (not found?)");
  }
}

// -----------------------------------------------------------------------
// /grouplist <namespace>
// -----------------------------------------------------------------------


// Group iteration callback: formats and emits one group entry.
// returns: void
// name: group name
// description: group description (may be empty)
// data: pointer to grouplist_state_t with ctx and running count
static void
grouplist_cb(const char *name, const char *description, void *data)
{
  grouplist_state_t *st = data;
  char line[USERNS_GROUP_SZ + USERNS_DESC_SZ + 16];

  if(description[0] != '\0')
    snprintf(line, sizeof(line), "  %s — %s", name, description);
  else
    snprintf(line, sizeof(line), "  %s", name);

  cmd_reply(st->ctx, line);
  st->count++;
}

// /grouplist <namespace> — list all groups in a namespace.
// returns: void
// ctx: command context with parsed namespace argument
static void
admin_cmd_grouplist(const cmd_ctx_t *ctx)
{
  const char *ns_name = ctx->parsed->argv[0];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  grouplist_state_t st = { .ctx = ctx, .count = 0 };

  char hdr[USERNS_NAME_SZ + 32];
  snprintf(hdr, sizeof(hdr), "groups in %s:", ns_name);
  cmd_reply(ctx, hdr);

  userns_group_iterate(ns, grouplist_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// /grant <namespace> <username> <group> <level>
// -----------------------------------------------------------------------
static void
admin_cmd_grant(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *group    = ctx->parsed->argv[2];
  unsigned long level  = strtoul(ctx->parsed->argv[3], NULL, 10);

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

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
    {
      cmd_reply(ctx, "failed to update level");
    }
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
    {
      cmd_reply(ctx, "failed to grant membership (user or group "
          "not found?)");
    }
  }
}

// -----------------------------------------------------------------------
// /revoke <namespace> <username> <group>
// -----------------------------------------------------------------------
static void
admin_cmd_revoke(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *group    = ctx->parsed->argv[2];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_member_remove(ns, username, group) == SUCCESS)
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s removed from %s", username, group);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to revoke membership (not a member?)");
  }
}

// -----------------------------------------------------------------------
// /mfa add|del|list — MFA pattern management
// -----------------------------------------------------------------------


// MFA iteration callback: emits one MFA pattern line.
// returns: void
// pattern: MFA glob pattern string (handle!user@host format)
// data: pointer to mfa_list_state_t with ctx and running count
static void
mfa_list_cb(const char *pattern, void *data)
{
  mfa_list_state_t *st = data;
  char line[USERNS_MFA_PATTERN_SZ + 8];

  snprintf(line, sizeof(line), "  %s", pattern);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /mfa — parent command. Lists subcommands when invoked directly.
static void
admin_cmd_mfa(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /mfa <subcommand> ...");
}

// /mfa add <namespace> <username> <pattern>
static void
admin_cmd_mfa_add(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *pattern  = ctx->parsed->argv[2];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(!userns_user_exists(ns, username))
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user not found: %s", username);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_user_add_mfa(ns, username, pattern) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "MFA pattern added for %s", username);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to add MFA pattern (invalid format, "
        "duplicate, or limit reached)");
  }
}

// /mfa del <namespace> <username> <pattern>
static void
admin_cmd_mfa_del(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];
  const char *pattern  = ctx->parsed->argv[2];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(userns_user_remove_mfa(ns, username, pattern) == SUCCESS)
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "MFA pattern removed for %s", username);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to remove MFA pattern (not found?)");
  }
}

// /mfa list <namespace> <username>
static void
admin_cmd_mfa_list(const cmd_ctx_t *ctx)
{
  const char *ns_name  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];

  userns_t *ns = userns_find(ns_name);

  if(ns == NULL)
  {
    char buf[USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "namespace not found: %s", ns_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(!userns_user_exists(ns, username))
  {
    char buf[USERNS_USER_SZ + 32];
    snprintf(buf, sizeof(buf), "user not found: %s", username);
    cmd_reply(ctx, buf);
    return;
  }

  mfa_list_state_t st = { .ctx = ctx, .count = 0 };

  char hdr[USERNS_USER_SZ + 32];
  snprintf(hdr, sizeof(hdr), "MFA patterns for %s:", username);
  cmd_reply(ctx, hdr);

  userns_user_list_mfa(ns, username, mfa_list_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

// Register user/group/MFA management commands.
void
admin_register_user_commands(void)
{
  // User management commands.
  cmd_register("admin", "useradd",
      "useradd <namespace> <username> <password>",
      "Create a user in a namespace",
      "Creates a new user in the specified namespace. The password\n"
      "must meet the password policy (length, uppercase, lowercase,\n"
      "digit, and symbol). The user is automatically added to the\n"
      "everyone and user groups at level 0.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_useradd, NULL, NULL, NULL,
      ad_useradd, 3);

  cmd_register("admin", "userdel",
      "userdel <namespace> <username>",
      "Delete a user from a namespace",
      "Deletes a user and all their group memberships from the\n"
      "specified namespace. This action cannot be undone.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_userdel, NULL, NULL, NULL,
      ad_ns_user, 2);

  cmd_register("admin", "userlist",
      "userlist <namespace>",
      "List users in a namespace",
      "Lists all users in the specified namespace with their\n"
      "descriptions (if set).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_userlist, NULL, NULL, NULL,
      ad_ns, 1);

  cmd_register("admin", "userinfo",
      "userinfo <namespace> <username>",
      "Show user details and group memberships",
      "Displays information about a user including all group\n"
      "memberships and their privilege levels.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_userinfo, NULL, NULL, NULL,
      ad_ns_user, 2);

  cmd_register("admin", "passwd",
      "passwd <namespace> <username> <newpassword>",
      "Reset a user's password",
      "Administratively resets a user's password without requiring\n"
      "the old password. The new password must meet the password\n"
      "policy requirements.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_passwd, NULL, NULL, NULL,
      ad_passwd, 3);

  // Group management commands.
  cmd_register("admin", "groupadd",
      "groupadd <namespace> <name> [description]",
      "Create a group in a namespace",
      "Creates a new group in the specified namespace. An optional\n"
      "description can follow the group name. Group names must be\n"
      "alphanumeric (max 30 characters).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_groupadd, NULL, NULL, NULL,
      ad_groupadd, 3);

  cmd_register("admin", "groupdel",
      "groupdel <namespace> <name>",
      "Delete a group from a namespace",
      "Deletes a group and all its memberships from the specified\n"
      "namespace. Built-in groups (owner, admin, user, everyone)\n"
      "cannot be deleted.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_groupdel, NULL, NULL, NULL,
      ad_ns_user, 2);

  cmd_register("admin", "grouplist",
      "grouplist <namespace>",
      "List groups in a namespace",
      "Lists all groups in the specified namespace with their\n"
      "descriptions (if set).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_grouplist, NULL, NULL, NULL,
      ad_ns, 1);

  // Membership commands.
  cmd_register("admin", "grant",
      "grant <namespace> <username> <group> <level>",
      "Grant group membership to a user",
      "Adds a user to a group with the specified privilege level\n"
      "(0-65535). If the user is already a member, their level is\n"
      "updated instead. Higher levels grant access to more commands.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_grant, NULL, NULL, NULL,
      ad_grant, 4);

  cmd_register("admin", "revoke",
      "revoke <namespace> <username> <group>",
      "Revoke group membership from a user",
      "Removes a user from a group. The user will no longer have\n"
      "access to commands that require membership in that group.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_revoke, NULL, NULL, NULL,
      ad_revoke, 3);

  // MFA management: parent command + subcommands.
  cmd_register("admin", "mfa",
      "mfa <add|del|list> ...",
      "Manage user MFA patterns",
      "Manages glob-based MFA patterns in handle!username@hostname\n"
      "format. The handle must have at least 3 non-glob characters\n"
      "and the hostname at least 6. Maximum patterns per user is\n"
      "configurable via core.userns.max_mfa (default 10).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_mfa, NULL, NULL, NULL, NULL, 0);

  cmd_register("admin", "add",
      "mfa add <namespace> <username> <pattern>",
      "Add an MFA pattern to a user",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_mfa_add, NULL, "mfa", NULL,
      ad_mfa_add, 3);

  cmd_register("admin", "del",
      "mfa del <namespace> <username> <pattern>",
      "Remove an MFA pattern from a user",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_mfa_del, NULL, "mfa", NULL,
      ad_mfa_add, 3);

  cmd_register("admin", "list",
      "mfa list <namespace> <username>",
      "List MFA patterns for a user",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_mfa_list, NULL, "mfa", "ls",
      ad_mfa_list, 2);
}
