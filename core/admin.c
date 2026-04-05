#define ADMIN_INTERNAL
#include "admin.h"

// Forward declaration for show subcommand handler.
static void admin_cmd_status(const cmd_ctx_t *ctx);

// Effective limit for show output (from KV).
static uint32_t admin_show_max = 64;

// -----------------------------------------------------------------------
// Custom validators
// -----------------------------------------------------------------------

// KV key format: alphanumeric, dots, underscores.
static bool
admin_validate_kv_key(const char *str)
{
  if(str == NULL || str[0] == '\0')
    return false;

  for(const char *k = str; *k != '\0'; k++)
  {
    unsigned char ch = (unsigned char)*k;

    if(!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_'
        || ch == '-'))
      return false;
  }

  return true;
}

// -----------------------------------------------------------------------
// Argument descriptors
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_set[] = {
  { "key",   CMD_ARG_CUSTOM, CMD_ARG_REQUIRED,           KV_KEY_SZ,       admin_validate_kv_key },
  { "value", CMD_ARG_NONE,   CMD_ARG_REQUIRED | CMD_ARG_REST, 0,          NULL },
};

static const cmd_arg_desc_t ad_show_kv[] = {
  { "prefix", CMD_ARG_NONE, CMD_ARG_OPTIONAL, KV_KEY_SZ, NULL },
};

static const cmd_arg_desc_t ad_show_schema[] = {
  { "plugin", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, PLUGIN_NAME_SZ, NULL },
  { "group",  CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, 0,              NULL },
};

// -----------------------------------------------------------------------
// /set <key> <value>
// -----------------------------------------------------------------------
static void
admin_cmd_set(const cmd_ctx_t *ctx)
{
  const char *key = ctx->parsed->argv[0];
  const char *value = ctx->parsed->argv[1];

  if(kv_set(key, value) == SUCCESS)
  {
    char buf[KV_KEY_SZ + KV_STR_SZ + 8];

    snprintf(buf, sizeof(buf), "%s = %s", key, value);
    cmd_reply(ctx, buf);
    kv_flush();
  }

  else
  {
    char buf[KV_KEY_SZ + 64];

    if(!kv_exists(key))
      snprintf(buf, sizeof(buf), "unknown key: %s", key);
    else
      snprintf(buf, sizeof(buf), "invalid value for %s", key);

    cmd_reply(ctx, buf);
  }
}

// -----------------------------------------------------------------------
// /show — parent command with subcommands (kv, bots, status)
// -----------------------------------------------------------------------

// KV iteration callback: copies each key/value/type into the result array.
// returns: void
// key: KV key string
// type: KV value type (uint, string, etc.)
// val: KV value as a string
// data: pointer to admin_show_result_t accumulator
static void
admin_show_iter_cb(const char *key, kv_type_t type,
    const char *val, void *data)
{
  admin_show_result_t *gr = data;

  if(gr->count >= admin_show_max)
    return;

  strncpy(gr->entries[gr->count].key, key, KV_KEY_SZ - 1);
  gr->entries[gr->count].key[KV_KEY_SZ - 1] = '\0';
  strncpy(gr->entries[gr->count].val, val, KV_STR_SZ - 1);
  gr->entries[gr->count].val[KV_STR_SZ - 1] = '\0';
  gr->entries[gr->count].type = type;
  gr->count++;
}

// qsort comparator: orders show entries alphabetically by key.
// returns: negative, zero, or positive per strcmp semantics
// a: pointer to first admin_show_entry_t
// b: pointer to second admin_show_entry_t
static int
admin_show_entry_cmp(const void *a, const void *b)
{
  const admin_show_entry_t *ea = a;
  const admin_show_entry_t *eb = b;

  return(strcmp(ea->key, eb->key));
}

// Helper: perform KV prefix browsing and display results.
static void
admin_show_kv_impl(const cmd_ctx_t *ctx, const char *prefix)
{
  admin_show_result_t gr;
  memset(&gr, 0, sizeof(gr));

  kv_iterate_prefix(prefix, admin_show_iter_cb, &gr);

  if(gr.count == 0)
  {
    if(prefix[0] != '\0')
    {
      char buf[KV_KEY_SZ + 32];

      snprintf(buf, sizeof(buf), "no matching keys: %s", prefix);
      cmd_reply(ctx, buf);
    }

    else
    {
      cmd_reply(ctx, "no configuration entries");
    }

    return;
  }

  qsort(gr.entries, gr.count, sizeof(gr.entries[0]), admin_show_entry_cmp);

  for(uint32_t i = 0; i < gr.count; i++)
  {
    char line[KV_KEY_SZ + KV_STR_SZ + 32];

    snprintf(line, sizeof(line), "  %s = %s (%s)",
        gr.entries[i].key, gr.entries[i].val,
        kv_type_name(gr.entries[i].type));
    cmd_reply(ctx, line);
  }

  if(gr.count >= admin_show_max)
    cmd_reply(ctx, "  (truncated)");
}

// /show kv [prefix] — display configuration values.
static void
admin_cmd_show_kv(const cmd_ctx_t *ctx)
{
  const char *prefix = (ctx->parsed && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : "";

  admin_show_kv_impl(ctx, prefix);
}

// Bot iteration callback for /show bots: formats and emits one line.
static void
show_botlist_cb(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count, uint32_t session_count,
    const char *userns_name, void *data)
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

// /show bots — list bot instances.
static void
admin_cmd_show_bots(const cmd_ctx_t *ctx)
{
  botlist_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx, "bot instances:");
  bot_iterate(show_botlist_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// Method type iteration callback for /show methods: formats one type entry.
// name: driver kind name
// bit: method_type_t bitmask
// desc: human-friendly description
// data: methodlist_state_t pointer
static void
show_method_type_cb(const char *name, method_type_t bit,
    const char *desc, void *data)
{
  methodlist_state_t *st = data;
  char line[256];

  (void)bit;
  snprintf(line, sizeof(line), "  %-12s — %s", name, desc);
  cmd_reply(st->ctx, line);
  st->count++;
}

// Method instance iteration callback for /show methods: formats one instance.
// name: instance name
// kind: driver kind name
// state: current instance state
// msg_in: inbound message count
// msg_out: outbound message count
// sub_count: subscriber count
// data: methodlist_state_t pointer
static void
show_method_inst_cb(const char *name, const char *kind,
    method_state_t state, uint64_t msg_in, uint64_t msg_out,
    uint32_t sub_count, void *data)
{
  methodlist_state_t *st = data;
  char line[256];

  snprintf(line, sizeof(line),
      "  %-16s kind=%-12s state=%-12s in=%-6lu out=%-6lu subs=%u",
      name, kind, method_state_name(state),
      (unsigned long)msg_in, (unsigned long)msg_out, sub_count);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /show methods — display method types and active instances.
static void
admin_cmd_show_methods(const cmd_ctx_t *ctx)
{
  methodlist_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx, "method types:");
  method_iterate_types(show_method_type_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");

  st.count = 0;
  cmd_reply(ctx, "method instances:");
  method_iterate_instances(show_method_inst_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// /show status — system health dashboard.
static void
admin_cmd_show_status(const cmd_ctx_t *ctx)
{
  admin_cmd_status(ctx);
}

// Callback for plugin_kv_group_iterate — lists each schema group.
static void
admin_schema_iter_cb(const plugin_desc_t *plugin,
    const plugin_kv_group_t *group, void *data)
{
  const cmd_ctx_t *ctx = data;
  char line[256];

  snprintf(line, sizeof(line), "  %-12s %-12s — %s",
      plugin->name, group->name, group->description);
  cmd_reply(ctx, line);
}

// List all schema groups across all plugins.
static void
admin_show_schema_all(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "plugin entity schemas:");
  cmd_reply(ctx, "  plugin       group        description");
  plugin_kv_group_iterate(admin_schema_iter_cb, (void *)ctx);
}

// /show schema [plugin [group]] — display plugin entity schemas.
static void
admin_cmd_show_schema(const cmd_ctx_t *ctx)
{
  const char *plugin_name = (ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;
  const char *group_name = (ctx->parsed->argc > 1)
      ? ctx->parsed->argv[1] : NULL;

  // No arguments: list all schema groups across all plugins.
  if(plugin_name == NULL)
  {
    admin_show_schema_all(ctx);
    return;
  }

  // Plugin name only: list groups for that plugin.
  const plugin_desc_t *pd = plugin_find(plugin_name);

  if(pd == NULL)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "unknown plugin: %s", plugin_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(pd->kv_groups == NULL || pd->kv_groups_count == 0)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s: no schema groups defined", plugin_name);
    cmd_reply(ctx, buf);
    return;
  }

  if(group_name == NULL)
  {
    // List groups for this plugin.
    char hdr[128];

    snprintf(hdr, sizeof(hdr), "%s schema groups:", plugin_name);
    cmd_reply(ctx, hdr);

    for(uint32_t j = 0; j < pd->kv_groups_count; j++)
    {
      char line[256];

      snprintf(line, sizeof(line), "  %-12s — %s",
          pd->kv_groups[j].name, pd->kv_groups[j].description);
      cmd_reply(ctx, line);
    }

    return;
  }

  // Plugin + group: show full detail.
  const plugin_kv_group_t *g = plugin_kv_group_find(plugin_name, group_name);

  if(g == NULL)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s: unknown schema group: %s",
        plugin_name, group_name);
    cmd_reply(ctx, buf);
    return;
  }

  char hdr[256];

  snprintf(hdr, sizeof(hdr), "schema: %s.%s — %s",
      plugin_name, g->name, g->description);
  cmd_reply(ctx, hdr);

  snprintf(hdr, sizeof(hdr), "  key pattern: %s", g->key_prefix);
  cmd_reply(ctx, hdr);

  snprintf(hdr, sizeof(hdr), "  command: /%s %s", plugin_name, g->cmd_name);
  cmd_reply(ctx, hdr);
  cmd_reply(ctx, "  properties:");

  for(uint32_t j = 0; j < g->schema_count; j++)
  {
    const plugin_kv_entry_t *e = &g->schema[j];
    char line[256];

    snprintf(line, sizeof(line), "    %-16s %-8s default: %s",
        e->key, kv_type_name(e->type),
        (e->default_val && e->default_val[0]) ? e->default_val : "(empty)");
    cmd_reply(ctx, line);
  }
}

// /show parent handler: lists subcommands when no args, falls through
// to KV browsing for backward compatibility when args don't match a
// subcommand (subcommand resolution already ran, so we get here only
// if the first token didn't match a child).
static void
admin_cmd_show(const cmd_ctx_t *ctx)
{
  if(ctx->args[0] != '\0')
  {
    // Backward compatibility: treat unrecognized args as KV prefix.
    admin_show_kv_impl(ctx, ctx->args);
    return;
  }

  cmd_reply(ctx, "usage: /show <subcommand> ...");
  cmd_reply(ctx, "  kv [prefix]  (k)  — show configuration values");
  cmd_reply(ctx, "  bots         (b)  — list bot instances");
  cmd_reply(ctx, "  methods      (m)  — list method types and instances");
  cmd_reply(ctx, "  tasks        (t)  — list active tasks");
  cmd_reply(ctx, "  memory     (mem)  — memory utilization table");
  cmd_reply(ctx, "  status       (st) — system health dashboard");
  cmd_reply(ctx, "  plugin [all|name] (plug) — list or detail plugins");
  cmd_reply(ctx, "  schema [plugin]  (sc) — plugin entity schemas");
  cmd_reply(ctx, "  irc ...               — IRC network/server info");
  cmd_reply(ctx, "");
  cmd_reply(ctx, "backward compatible: /show <kv-prefix>");
}

// -----------------------------------------------------------------------
// /status — system health dashboard
// -----------------------------------------------------------------------
static void
admin_cmd_status(const cmd_ctx_t *ctx)
{
  char buf[512];

  cmd_reply(ctx, BM_VERSION_STR);

  mem_stats_t  ms;
  task_stats_t ts;
  pool_stats_t ps;

  mem_get_stats(&ms);
  task_get_stats(&ts);
  pool_get_stats(&ps);

  snprintf(buf, sizeof(buf),
      "memory: %zu bytes (peak %zu), %lu allocs, %lu total, %lu freelist",
      ms.heap_sz, ms.peak_heap_sz,
      (unsigned long)ms.active, (unsigned long)ms.total_allocs,
      (unsigned long)ms.freelist);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "tasks: %u waiting, %u running, %u sleeping, %u linked, "
      "%u persist, %u periodic",
      ts.waiting, ts.running, ts.sleeping, ts.linked,
      ts.persist, ts.periodic);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "threads: %u workers (peak %u), %u idle, %u persist, %lu jobs",
      ps.total, ps.peak_workers, ps.idle, ps.persist,
      (unsigned long)ps.jobs_completed);
  cmd_reply(ctx, buf);

  sock_stats_t ss;

  sock_get_stats(&ss);

  snprintf(buf, sizeof(buf),
      "sockets: %u sessions, %u connected, %u tls, "
      "%lu conns, %lu disconns, %lu in, %lu out",
      ss.sessions, ss.connected, ss.tls_sessions,
      (unsigned long)ss.connections, (unsigned long)ss.disconnects,
      (unsigned long)ss.bytes_in, (unsigned long)ss.bytes_out);
  cmd_reply(ctx, buf);

  method_stats_t mts;

  method_get_stats(&mts);

  snprintf(buf, sizeof(buf),
      "methods: %u instances, %u subscribers, %lu in, %lu out",
      mts.instances, mts.subscribers,
      (unsigned long)mts.total_msg_in, (unsigned long)mts.total_msg_out);
  cmd_reply(ctx, buf);

  bot_stats_t bs;

  bot_get_stats(&bs);

  snprintf(buf, sizeof(buf),
      "bots: %u instances, %u running, %u methods, %u sessions, "
      "%lu cmds, %lu denied, %u discovered",
      bs.instances, bs.running, bs.methods, bs.sessions,
      (unsigned long)bs.cmd_dispatches, (unsigned long)bs.cmd_denials,
      bs.discovered_users);
  cmd_reply(ctx, buf);

  curl_stats_t cs;

  curl_get_stats(&cs);

  uint64_t avg_ms = cs.total_requests > 0
      ? cs.total_response_ms / cs.total_requests : 0;

  snprintf(buf, sizeof(buf),
      "curl: %u active, %u queued, %lu total, %lu errors, %lu ms avg",
      cs.active, cs.queued,
      (unsigned long)cs.total_requests, (unsigned long)cs.total_errors,
      (unsigned long)avg_ms);
  cmd_reply(ctx, buf);

  db_pool_stats_t ds;

  db_get_pool_stats(&ds);

  snprintf(buf, sizeof(buf),
      "db: %u total, %u idle, %u active, %u failed, "
      "%lu queries, %lu errors",
      ds.total, ds.idle, ds.active, ds.failed,
      (unsigned long)ds.queries, (unsigned long)ds.errors);
  cmd_reply(ctx, buf);
}

// -----------------------------------------------------------------------
// Module lifecycle
// -----------------------------------------------------------------------

// Register all administrative commands (set, show, status) and delegate
// user/group/MFA, bot management, and help commands to sub-modules.
// returns: void
void
admin_init(void)
{
  cmd_register_system("admin", "set", "set <key> <value>",
      "Set a configuration value",
      "Sets the value of a KV configuration key. The key must\n"
      "already exist in the KV store. Changes are persisted to\n"
      "the database immediately. Use /show to browse available keys.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_set, NULL, NULL, NULL, ad_set, 2);

  // Show: parent command + subcommands.
  cmd_register_system("admin", "show",
      "show <subcommand> ...",
      "Show system information",
      "Display configuration, bot instances, system health, and\n"
      "plugin-contributed views. Use /show with no arguments to\n"
      "list available subcommands. For backward compatibility,\n"
      "/show <kv-prefix> still works to browse configuration keys.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_show, NULL, NULL, "sh", NULL, 0);

  cmd_register_system("admin", "kv",
      "show kv [prefix]",
      "Show configuration values",
      "Displays KV configuration entries. If a prefix is given,\n"
      "only keys starting with that prefix are shown (e.g.,\n"
      "show kv core.sock). Results are sorted alphabetically and\n"
      "truncated at the limit set by core.admin.show_max.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_show_kv, NULL, "show", "k",
      ad_show_kv, 1);

  cmd_register_system("admin", "bots",
      "show bots",
      "List bot instances",
      "Shows all bot instances with their state, method count,\n"
      "session count, and user namespace.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_show_bots, NULL, "show", "b", NULL, 0);

  cmd_register_system("admin", "methods",
      "show methods",
      "List method types and instances",
      "Shows all known method types with descriptions and all\n"
      "active method instances with their state, message counts,\n"
      "and subscriber count.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_show_methods, NULL, "show", "m", NULL, 0);

  cmd_register_system("admin", "status",
      "show status",
      "System health dashboard",
      "Shows a summary of system health including memory usage,\n"
      "task counts, thread pool state, method instances, and\n"
      "bot instance statistics.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_show_status, NULL, "show",
      "st", NULL, 0);

  cmd_register_system("admin", "schema",
      "show schema [plugin [group]]",
      "Show plugin entity schemas",
      "Display KV schema groups declared by plugins.\n"
      "  /show schema              — list all groups across plugins\n"
      "  /show schema irc          — list groups for the IRC plugin\n"
      "  /show schema irc channel  — show channel schema details",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_show_schema, NULL, "show",
      "sc", ad_show_schema, 2);

  cmd_register_system("admin", "status", "status",
      "Display system health dashboard",
      "Shows a summary of system health including memory usage,\n"
      "task counts, thread pool state, method instances, and\n"
      "bot instance statistics.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_status, NULL, NULL, "stat", NULL, 0);

  // Delegate to sub-modules.
  admin_register_help_commands();
  admin_register_user_commands();
  admin_register_bot_commands();

  clam(CLAM_INFO, "admin_init", "administrative commands registered");
}

// -----------------------------------------------------------------------
// KV configuration
// -----------------------------------------------------------------------

// Load admin configuration from KV store, clamping show_max to valid range.
// returns: void
static void
admin_load_config(void)
{
  admin_show_max = (uint32_t)kv_get_uint("core.admin.show_max");

  if(admin_show_max < 8)          admin_show_max = 8;
  if(admin_show_max > ADMIN_SHOW_LIMIT) admin_show_max = ADMIN_SHOW_LIMIT;
}

// KV change notification callback: reloads admin config on any change.
// returns: void
// key: changed KV key (unused, we reload all admin keys)
// data: user data (unused)
static void
admin_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  admin_load_config();
}

// Register admin KV keys and load initial values from the store.
// returns: void
void
admin_register_config(void)
{
  kv_register("core.admin.show_max", KV_UINT32, "64", admin_kv_changed, NULL);
  admin_load_config();
}

// Unregister all administrative commands in safe dependency order.
// returns: void
void
admin_exit(void)
{
  cmd_unregister("set");

  // Unregister show subcommands before the parent.
  cmd_unregister("kv");
  cmd_unregister("bots");
  cmd_unregister("status");   // first match: root-level alias
  cmd_unregister("show");     // reparents remaining "status" child
  cmd_unregister("status");   // second match: former show subcommand
  cmd_unregister("quit");
  cmd_unregister("help");

  cmd_unregister("useradd");
  cmd_unregister("userdel");
  cmd_unregister("userlist");
  cmd_unregister("userinfo");
  cmd_unregister("passwd");
  cmd_unregister("groupadd");
  cmd_unregister("groupdel");
  cmd_unregister("grouplist");
  cmd_unregister("grant");
  cmd_unregister("revoke");
  cmd_unregister("mfa");

  // Unregister bot subcommands before the parent.
  cmd_unregister("add");
  cmd_unregister("del");
  cmd_unregister("list");
  cmd_unregister("start");
  cmd_unregister("stop");
  cmd_unregister("bind");
  cmd_unregister("unbind");
  cmd_unregister("userns");
  cmd_unregister("bot");

  // Unregister plugin management subcommands before the parent.
  cmd_unregister("load");
  cmd_unregister("unload");
  cmd_unregister("plugin");   // root-level /plugin
  cmd_unregister("plugin");   // show plugin subcommand (child of show)

  clam(CLAM_INFO, "admin_exit", "administrative commands unregistered");
}
