#define ADMIN_INTERNAL
#include "admin.h"

// -----------------------------------------------------------------------
// Argument descriptors
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_bot_name_kind[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,    NULL },
  { "kind", CMD_ARG_NONE,  CMD_ARG_REQUIRED, PLUGIN_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_bot_name[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_bot_method[] = {
  { "bot",    CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,    NULL },
  { "method", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, PLUGIN_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_bot_userns[] = {
  { "bot",       CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,    NULL },
  { "namespace", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_NAME_SZ, NULL },
};

// -----------------------------------------------------------------------
// /bot add <name> <kind>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_add(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  const char *kind = ctx->parsed->argv[1];

  if(bot_find(name) != NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot already exists: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  const plugin_desc_t *pd = plugin_find_type(PLUGIN_BOT, kind);

  if(pd == NULL || pd->ext == NULL)
  {
    char buf[PLUGIN_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "no bot plugin with kind: %s", kind);
    cmd_reply(ctx, buf);
    return;
  }

  const bot_driver_t *drv = (const bot_driver_t *)pd->ext;
  bot_inst_t *inst = bot_create(drv, name);

  if(inst == NULL)
  {
    cmd_reply(ctx, "failed to create bot instance");
    return;
  }

  // Persist to database.
  {
    char *e_name = db_escape(name);
    char *e_kind = db_escape(kind);

    if(e_name != NULL && e_kind != NULL)
    {
      char sql[512];
      snprintf(sql, sizeof(sql),
          "INSERT INTO bot_instances (name, kind) VALUES ('%s', '%s') "
          "ON CONFLICT (name) DO NOTHING",
          e_name, e_kind);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_add", "DB persist failed: %s", r->error);

      db_result_free(r);
    }

    mem_free(e_name);
    mem_free(e_kind);
  }

  char buf[BOT_NAME_SZ + PLUGIN_NAME_SZ + 32];
  snprintf(buf, sizeof(buf), "bot created: %s (kind: %s)", name, kind);
  cmd_reply(ctx, buf);
}

// -----------------------------------------------------------------------
// /bot del <name>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_del(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];

  if(bot_find(name) == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot not found: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  if(bot_destroy(name) == SUCCESS)
  {
    // Remove from database (CASCADE deletes bot_methods rows).
    char *e_name = db_escape(name);

    if(e_name != NULL)
    {
      char sql[256];
      snprintf(sql, sizeof(sql),
          "DELETE FROM bot_instances WHERE name = '%s'", e_name);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_del", "DB persist failed: %s", r->error);

      db_result_free(r);
      mem_free(e_name);
    }

    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot destroyed: %s", name);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "failed to destroy bot instance");
  }
}

// -----------------------------------------------------------------------
// /bot list
// -----------------------------------------------------------------------

// Bot iteration callback: formats and emits one bot instance line.
// returns: void
// name: bot instance name
// driver_name: bot plugin kind (e.g., "command")
// state: current bot state (CREATED, RUNNING, etc.)
// method_count: number of bound method instances
// session_count: number of active sessions
// userns_name: assigned user namespace name, or NULL if none
// data: pointer to botlist_state_t with ctx and running count
static void
botlist_cb(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count, uint32_t session_count,
    const char *userns_name, uint64_t cmd_count, time_t last_activity,
    void *data)
{
  botlist_state_t *st = data;
  char line[256];

  snprintf(line, sizeof(line),
      "  %-16s kind=%-10s state=%-8s methods=%u sessions=%u userns=%s",
      name, driver_name, bot_state_name(state),
      method_count, session_count,
      userns_name ? userns_name : "(none)");

  cmd_reply(st->ctx, line);
  st->count++;
}

// /bot list — list all bot instances with state and binding info.
// returns: void
// ctx: command context
static void
admin_cmd_bot_list(const cmd_ctx_t *ctx)
{
  botlist_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx, "bot instances:");
  bot_iterate(botlist_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// /bot start <name>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_start(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];

  bot_inst_t *inst = bot_find(name);

  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot not found: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  if(bot_start(inst) == SUCCESS)
  {
    // Mark auto_start in database.
    char *e_name = db_escape(name);

    if(e_name != NULL)
    {
      char sql[256];
      snprintf(sql, sizeof(sql),
          "UPDATE bot_instances SET auto_start = TRUE "
          "WHERE name = '%s'", e_name);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_start", "DB persist failed: %s", r->error);

      db_result_free(r);
      mem_free(e_name);
    }

    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot started: %s", name);
    cmd_reply(ctx, buf);
  }
  else
  {
    char buf[BOT_NAME_SZ + 64];
    snprintf(buf, sizeof(buf),
        "failed to start %s (state=%s, check methods)",
        name, bot_state_name(bot_get_state(inst)));
    cmd_reply(ctx, buf);
  }
}

// -----------------------------------------------------------------------
// /bot stop <name>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_stop(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];

  bot_inst_t *inst = bot_find(name);

  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot not found: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  if(bot_stop(inst) == SUCCESS)
  {
    // Clear auto_start in database.
    char *e_name = db_escape(name);

    if(e_name != NULL)
    {
      char sql[256];
      snprintf(sql, sizeof(sql),
          "UPDATE bot_instances SET auto_start = FALSE "
          "WHERE name = '%s'", e_name);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_stop", "DB persist failed: %s", r->error);

      db_result_free(r);
      mem_free(e_name);
    }

    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot stopped: %s", name);
    cmd_reply(ctx, buf);
  }
  else
  {
    char buf[BOT_NAME_SZ + 64];
    snprintf(buf, sizeof(buf),
        "failed to stop %s (state=%s)",
        name, bot_state_name(bot_get_state(inst)));
    cmd_reply(ctx, buf);
  }
}

// -----------------------------------------------------------------------
// /bot bind <botname> <method_kind>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_bind(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  const char *method_kind = ctx->parsed->argv[1];

  bot_inst_t *inst = bot_find(botname);

  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot not found: %s", botname);
    cmd_reply(ctx, buf);
    return;
  }

  // Build per-bot method instance name: "<botname>_<kind>"
  char inst_name[BOT_NAME_SZ + METHOD_NAME_SZ + 2];
  snprintf(inst_name, sizeof(inst_name), "%s_%s", botname, method_kind);

  if(bot_bind_method(inst, inst_name, method_kind) == SUCCESS)
  {
    // Persist method binding.
    char *e_bot = db_escape(botname);
    char *e_kind = db_escape(method_kind);

    if(e_bot != NULL && e_kind != NULL)
    {
      char sql[512];
      snprintf(sql, sizeof(sql),
          "INSERT INTO bot_methods (bot_name, method_kind) "
          "VALUES ('%s', '%s') ON CONFLICT DO NOTHING",
          e_bot, e_kind);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_bind", "DB persist failed: %s", r->error);

      db_result_free(r);
    }

    mem_free(e_bot);
    mem_free(e_kind);

    // Register per-bot method KV keys (bot.<botname>.<kind>.*).
    bot_register_method_kv(botname, method_kind);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: bound method %s (instance: %s)",
        botname, method_kind, inst_name);
    cmd_reply(ctx, buf);
  }
  else
  {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "failed to bind %s to %s (duplicate, wrong state, or at limit)",
        method_kind, botname);
    cmd_reply(ctx, buf);
  }
}

// -----------------------------------------------------------------------
// /bot unbind <botname> <method_kind>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_unbind(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  const char *method_kind = ctx->parsed->argv[1];

  bot_inst_t *inst = bot_find(botname);

  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot not found: %s", botname);
    cmd_reply(ctx, buf);
    return;
  }

  // Build per-bot method instance name: "<botname>_<kind>"
  char inst_name[BOT_NAME_SZ + METHOD_NAME_SZ + 2];
  snprintf(inst_name, sizeof(inst_name), "%s_%s", botname, method_kind);

  if(bot_unbind_method(inst, inst_name) == SUCCESS)
  {
    // Remove method binding from database.
    char *e_bot = db_escape(botname);
    char *e_kind = db_escape(method_kind);

    if(e_bot != NULL && e_kind != NULL)
    {
      char sql[512];
      snprintf(sql, sizeof(sql),
          "DELETE FROM bot_methods WHERE bot_name = '%s' "
          "AND method_kind = '%s'",
          e_bot, e_kind);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_unbind", "DB persist failed: %s", r->error);

      db_result_free(r);
    }

    mem_free(e_bot);
    mem_free(e_kind);

    char buf[128];
    snprintf(buf, sizeof(buf), "%s: unbound method %s", botname, method_kind);
    cmd_reply(ctx, buf);
  }
  else
  {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "failed to unbind %s from %s (not bound or wrong state)",
        method_kind, botname);
    cmd_reply(ctx, buf);
  }
}

// -----------------------------------------------------------------------
// /bot userns <botname> <namespace>
// -----------------------------------------------------------------------
static void
admin_cmd_bot_userns(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  const char *ns_name = ctx->parsed->argv[1];

  bot_inst_t *inst = bot_find(botname);

  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "bot not found: %s", botname);
    cmd_reply(ctx, buf);
    return;
  }

  if(bot_set_userns(inst, ns_name) == SUCCESS)
  {
    // Persist userns assignment.
    char *e_bot = db_escape(botname);
    char *e_ns  = db_escape(ns_name);

    if(e_bot != NULL && e_ns != NULL)
    {
      char sql[256];
      snprintf(sql, sizeof(sql),
          "UPDATE bot_instances SET userns_name = '%s' "
          "WHERE name = '%s'", e_ns, e_bot);

      db_result_t *r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_userns", "DB persist failed: %s", r->error);

      db_result_free(r);
    }

    mem_free(e_bot);
    mem_free(e_ns);

    char buf[BOT_NAME_SZ + USERNS_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "%s: user namespace set to %s",
        botname, ns_name);
    cmd_reply(ctx, buf);
  }
  else
  {
    char buf[BOT_NAME_SZ + 64];
    snprintf(buf, sizeof(buf),
        "failed to set user namespace for %s (wrong state?)", botname);
    cmd_reply(ctx, buf);
  }
}

// -----------------------------------------------------------------------
// /bot — dispatcher for subcommands
// -----------------------------------------------------------------------
// Parent handler: lists available subcommands when no subcommand matched.
static void
admin_cmd_bot(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /bot <subcommand> ...");
}

// -----------------------------------------------------------------------
// /quit — graceful shutdown
// -----------------------------------------------------------------------
static void
admin_cmd_quit(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "operator requested shutdown");
  pool_shutdown();
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

// Register bot management commands and /quit.
void
admin_register_bot_commands(void)
{
  // Bot management: parent command + subcommands.
  cmd_register("admin", "bot",
      "bot <subcommand> ...",
      "Manage bot instances",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot, NULL, NULL, NULL, NULL, 0);

  cmd_register("admin", "add",
      "bot add <name> <kind>",
      "Create a bot instance",
      "Creates a new bot instance with the given name and kind.\n"
      "The kind must match a loaded bot plugin (e.g., command).\n"
      "Example: /bot add mybot command",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_add, NULL, "bot", NULL,
      ad_bot_name_kind, 2);

  cmd_register("admin", "del",
      "bot del <name>",
      "Destroy a bot instance",
      "Stops (if running) and destroys a bot instance.\n"
      "Removes the bot's KV namespace and database records.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_del, NULL, "bot", NULL,
      ad_bot_name, 1);

  cmd_register("admin", "list",
      "bot list",
      "List all bot instances",
      "Shows all bot instances with their state, method count,\n"
      "session count, and user namespace.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_list, NULL, "bot", "ls", NULL, 0);

  cmd_register("admin", "start",
      "bot start <name>",
      "Start a bot instance",
      "Starts a bot instance. Creates method instances and\n"
      "initiates connections. The bot must have at least one\n"
      "method bound.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_start, NULL, "bot", NULL,
      ad_bot_name, 1);

  cmd_register("admin", "stop",
      "bot stop <name>",
      "Stop a bot instance",
      "Stops a running bot instance. Disconnects method\n"
      "instances, clears sessions, and drains in-flight work.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_stop, NULL, "bot", NULL,
      ad_bot_name, 1);

  cmd_register("admin", "bind",
      "bot bind <bot> <method>",
      "Bind a method to a bot",
      "Binds a method plugin to a bot instance. The bot must\n"
      "be in CREATED state (not running). Configure method\n"
      "settings via /set before starting.\n"
      "Example: /bot bind mybot irc",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_bind, NULL, "bot", NULL,
      ad_bot_method, 2);

  cmd_register("admin", "unbind",
      "bot unbind <bot> <method>",
      "Unbind a method from a bot",
      "Unbinds a method from a bot instance. The bot must\n"
      "be in CREATED state (not running).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_unbind, NULL, "bot", NULL,
      ad_bot_method, 2);

  cmd_register("admin", "userns",
      "bot userns <bot> <namespace>",
      "Set user namespace for a bot",
      "Sets the user namespace for a bot instance. The namespace\n"
      "is created if it does not exist. The bot must be in\n"
      "CREATED state (not running).\n"
      "Example: /bot userns mybot default",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_userns, NULL, "bot",
      NULL, ad_bot_userns, 2);

  cmd_register("admin", "quit", "quit",
      "Graceful shutdown",
      "Initiates a graceful shutdown of BotManager. All in-flight\n"
      "work is drained, plugins are unloaded in reverse dependency\n"
      "order, and resources are released cleanly.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_quit, NULL,
      NULL, NULL, NULL, 0);
}
