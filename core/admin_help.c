#define ADMIN_INTERNAL
#include "admin.h"

// -----------------------------------------------------------------------
// /help — command reference with access-filtered table output
// -----------------------------------------------------------------------

// Iteration callback: collect command info into state.
// Filters out commands not visible on the caller's method type.
static void
help_cmd_collect_cb(const cmd_def_t *def, void *data)
{
  help_cmd_state_t *st = data;

  if(st->count >= HELP_CMD_MAX)
    return;

  // Skip commands not visible on the caller's method.
  method_type_t mt = cmd_get_methods(def);

  if(st->caller_type != 0 && !(mt & st->caller_type))
    return;

  help_cmd_entry_t *e = &st->entries[st->count];

  const char *name   = cmd_get_name(def);
  const char *abbrev = cmd_get_abbrev(def);
  const char *module = cmd_get_module(def);
  const char *help   = cmd_get_help(def);
  const char *group  = cmd_get_group(def);

  snprintf(e->name, sizeof(e->name), "%s", name ? name : "?");
  snprintf(e->abbrev, sizeof(e->abbrev), "%s",
      (abbrev != NULL && abbrev[0] != '\0') ? abbrev : "");
  snprintf(e->module, sizeof(e->module), "%s",
      (module != NULL && module[0] != '\0') ? module : "core");
  snprintf(e->help, sizeof(e->help), "%s", help ? help : "");
  snprintf(e->group, sizeof(e->group), "%s", group ? group : "");
  e->level = cmd_get_level(def);

  st->count++;
}

// qsort comparator: sort by name.
static int
help_cmd_cmp(const void *a, const void *b)
{
  const help_cmd_entry_t *ea = a;
  const help_cmd_entry_t *eb = b;

  return(strcmp(ea->name, eb->name));
}

// Check if the calling user has access to a command entry.
static bool
help_cmd_accessible(const cmd_ctx_t *ctx, const help_cmd_entry_t *e)
{
  // Console origin: unrestricted.
  if(ctx->msg != NULL && ctx->msg->console_origin)
    return(true);

  // Owner: unrestricted.
  if(ctx->username != NULL && userns_is_owner(ctx->username))
    return(true);

  // "everyone" group at level 0: always accessible.
  if(strcmp(e->group, USERNS_GROUP_EVERYONE) == 0 && e->level == 0)
    return(true);

  // Unauthenticated: no further access.
  if(ctx->username == NULL)
    return(false);

  // Authenticated: check group membership + level.
  userns_t *ns = (ctx->bot != NULL) ? bot_get_userns(ctx->bot) : NULL;

  if(ns == NULL)
    return(false);

  int32_t user_level = userns_member_level(ns, ctx->username, e->group);

  return(user_level >= 0 && (uint16_t)user_level >= e->level);
}

// Emit a help table from a collected + sorted state, filtered by access.
static void
help_cmd_emit_table(const cmd_ctx_t *ctx, help_cmd_state_t *st)
{
  char line[512];

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-20s %-12s %-12s %s" CLR_RESET,
      "COMMAND", "ABBREV", "MODULE", "DESCRIPTION");
  cmd_reply(ctx, line);

  uint32_t shown = 0;

  for(uint32_t i = 0; i < st->count; i++)
  {
    help_cmd_entry_t *e = &st->entries[i];

    if(!help_cmd_accessible(ctx, e))
      continue;

    snprintf(line, sizeof(line), "  %-20s %-12s %-12s %s",
        e->name,
        e->abbrev[0] != '\0' ? e->abbrev : "-",
        e->module,
        e->help);

    cmd_reply(ctx, line);
    shown++;
  }

  snprintf(line, sizeof(line), "%u command(s)", shown);
  cmd_reply(ctx, line);
}

// /help commands — table of all top-level commands.
static void
admin_cmd_help_commands(const cmd_ctx_t *ctx)
{
  help_cmd_state_t st;

  memset(&st, 0, sizeof(st));
  st.caller_type = (ctx->msg != NULL && ctx->msg->inst != NULL)
      ? method_inst_type(ctx->msg->inst) : METHOD_T_ANY;
  cmd_iterate_system(help_cmd_collect_cb, &st);

  qsort(st.entries, st.count, sizeof(help_cmd_entry_t), help_cmd_cmp);
  help_cmd_emit_table(ctx, &st);
}

// /help <command> — parent handler. If the command has children, show
// a subcommand table. Otherwise show verbose help for the command.
static void
admin_cmd_help_parent(const cmd_ctx_t *ctx)
{
  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, "usage: /help commands       — list all commands");
    cmd_reply(ctx, "       /help <command>      — subcommands or "
        "detailed help");
    return;
  }

  // Look up the named command.
  const cmd_def_t *d = cmd_find(ctx->args);

  if(d == NULL)
  {
    char buf[CMD_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "unknown command: %s", ctx->args);
    cmd_reply(ctx, buf);
    return;
  }

  // If the command has children, show a subcommand table.
  if(cmd_has_children(d))
  {
    char hdr[CMD_NAME_SZ + 32];
    const char *name = cmd_get_name(d);

    snprintf(hdr, sizeof(hdr), "subcommands of /%s:", name ? name : "?");
    cmd_reply(ctx, hdr);

    help_cmd_state_t st;

    memset(&st, 0, sizeof(st));
    st.caller_type = (ctx->msg != NULL && ctx->msg->inst != NULL)
        ? method_inst_type(ctx->msg->inst) : METHOD_T_ANY;
    cmd_iterate_children(d, help_cmd_collect_cb, &st);

    qsort(st.entries, st.count, sizeof(help_cmd_entry_t), help_cmd_cmp);
    help_cmd_emit_table(ctx, &st);
    return;
  }

  // No children: show verbose help.
  const char *module    = cmd_get_module(d);
  const char *usage     = cmd_get_usage(d);
  const char *help      = cmd_get_help(d);
  const char *help_long = cmd_get_help_long(d);

  if(usage != NULL)
  {
    char line[CMD_USAGE_SZ + 16];
    snprintf(line, sizeof(line), "usage: /%s", usage);
    cmd_reply(ctx, line);
  }

  if(help != NULL)
    cmd_reply(ctx, help);

  if(help_long != NULL && help_long[0] != '\0')
  {
    char buf[CMD_HELP_LONG_SZ];
    strncpy(buf, help_long, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *text = buf;
    char *nl;

    while((nl = strchr(text, '\n')) != NULL)
    {
      *nl = '\0';
      cmd_reply(ctx, text);
      text = nl + 1;
    }

    if(*text != '\0')
      cmd_reply(ctx, text);
  }

  if(module != NULL && module[0] != '\0')
  {
    char mod_line[CMD_MODULE_SZ + 16];
    snprintf(mod_line, sizeof(mod_line), "module: %s", module);
    cmd_reply(ctx, mod_line);
  }
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

// Register help commands.
void
admin_register_help_commands(void)
{
  // Help: parent command + subcommands.
  cmd_register_system("admin", "help", "help <subcommand|command>",
      "Command reference",
      "Use /help commands to list all accessible commands.\n"
      "Use /help <command> to see subcommands (if any) or\n"
      "detailed help for a specific command.",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_help_parent, NULL, NULL, "h", NULL, 0);

  cmd_register_system("admin", "commands",
      "help commands",
      "List all commands in a table",
      "Displays a table of all top-level system commands that\n"
      "the calling user has access to, showing command name,\n"
      "abbreviation, providing module, and brief description.",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_help_commands, NULL, "help", "com", NULL, 0);
}
