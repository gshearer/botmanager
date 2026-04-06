#define ADMIN_INTERNAL
#include "admin.h"

// Forward declaration.
static void admin_cmd_status(const cmd_ctx_t *ctx);
static void admin_cmd_show_status(const cmd_ctx_t *ctx);

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

  if(gr->count >= ADMIN_SHOW_LIMIT)
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

}

// /show kv [prefix] — display configuration values.
static void
admin_cmd_show_kv(const cmd_ctx_t *ctx)
{
  const char *prefix = (ctx->parsed && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : "";

  admin_show_kv_impl(ctx, prefix);
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
// connected_at: timestamp when instance became AVAILABLE (0 if not)
// data: methodlist_state_t pointer
static void
show_method_inst_cb(const char *name, const char *kind,
    method_state_t state, uint64_t msg_in, uint64_t msg_out,
    uint32_t sub_count, time_t connected_at, void *data)
{
  methodlist_state_t *st = data;
  char line[256];
  char uptime[32];

  // Format uptime as a human-readable duration.
  if(connected_at > 0)
  {
    time_t elapsed = time(NULL) - connected_at;
    uint32_t days  = (uint32_t)(elapsed / 86400);
    uint32_t hours = (uint32_t)((elapsed % 86400) / 3600);
    uint32_t mins  = (uint32_t)((elapsed % 3600) / 60);

    if(days > 0)
      snprintf(uptime, sizeof(uptime), "%ud %uh %um", days, hours, mins);

    else if(hours > 0)
      snprintf(uptime, sizeof(uptime), "%uh %um", hours, mins);

    else
      snprintf(uptime, sizeof(uptime), "%um", mins);
  }

  else
    strncpy(uptime, "-", sizeof(uptime));

  snprintf(line, sizeof(line),
      "  %-16s kind=%-12s state=%-12s in=%-6lu out=%-6lu subs=%u up=%s",
      name, kind, method_state_name(state),
      (unsigned long)msg_in, (unsigned long)msg_out, sub_count, uptime);
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

// -----------------------------------------------------------------------
// /show sockets — list active socket sessions
// -----------------------------------------------------------------------

// Format a duration in seconds to human-readable (e.g., "2h 15m").
// buf: destination buffer
// sz: buffer size
// secs: duration in seconds
static void
admin_fmt_duration(char *buf, size_t sz, time_t secs)
{
  if(secs <= 0)
  {
    snprintf(buf, sz, "-");
    return;
  }

  uint32_t d = (uint32_t)(secs / 86400);
  uint32_t h = (uint32_t)((secs % 86400) / 3600);
  uint32_t m = (uint32_t)((secs % 3600) / 60);

  if(d > 0)
    snprintf(buf, sz, "%ud %uh %um", d, h, m);
  else if(h > 0)
    snprintf(buf, sz, "%uh %um", h, m);
  else if(m > 0)
    snprintf(buf, sz, "%um %us", m, (uint32_t)(secs % 60));
  else
    snprintf(buf, sz, "%us", (uint32_t)secs);
}

// Socket iteration callback for /show sockets: formats one session.
// id: session index
// type: socket type
// state: internal socket state (cast from sock_state_t)
// remote: remote address string
// bytes_in: bytes received
// bytes_out: bytes sent
// tls: whether TLS is enabled
// connected_at: time connection was established
// data: socklist_state_t pointer
static void
show_sock_cb(uint32_t id, sock_type_t type, int state,
    const char *remote, uint64_t bytes_in, uint64_t bytes_out,
    bool tls, time_t connected_at, void *data)
{
  socklist_state_t *st = data;
  char line[512];
  char dur[32];

  if(connected_at > 0)
    admin_fmt_duration(dur, sizeof(dur), time(NULL) - connected_at);
  else
    snprintf(dur, sizeof(dur), "-");

  snprintf(line, sizeof(line),
      "  %-3u %-5s %-12s %-30s in=%-8lu out=%-8lu %s %s",
      id, sock_type_name(type), sock_state_name(state),
      (remote && remote[0]) ? remote : "(local)",
      (unsigned long)bytes_in, (unsigned long)bytes_out,
      tls ? "tls" : "   ", dur);

  cmd_reply(st->ctx, line);
  st->count++;
}

// /show sockets — display active socket sessions.
static void
admin_cmd_show_sockets(const cmd_ctx_t *ctx)
{
  socklist_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx, "socket sessions:");
  sock_iterate(show_sock_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// /show curl — curl subsystem state
// -----------------------------------------------------------------------

// Curl queued request iteration callback: formats one queued request.
// url: request URL
// method: HTTP method
// elapsed_secs: time since submission (0 for queued)
// data: curllist_state_t pointer
static void
show_curl_cb(const char *url, curl_method_t method,
    uint32_t elapsed_secs, void *data)
{
  curllist_state_t *st = data;
  char line[512];

  (void)elapsed_secs;
  snprintf(line, sizeof(line), "  queued  %-6s %s",
      curl_method_name(method), url);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /show curl — display curl subsystem state and statistics.
static void
admin_cmd_show_curl(const cmd_ctx_t *ctx)
{
  curl_stats_t cs;
  char buf[256];

  curl_get_stats(&cs);

  uint64_t avg_ms = cs.total_requests > 0
      ? cs.total_response_ms / cs.total_requests : 0;

  snprintf(buf, sizeof(buf),
      "curl: %u active, %u queued, %lu total requests, "
      "%lu errors, avg %lu ms",
      cs.active, cs.queued,
      (unsigned long)cs.total_requests,
      (unsigned long)cs.total_errors,
      (unsigned long)avg_ms);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  bytes in: %lu  bytes out: %lu",
      (unsigned long)cs.bytes_in, (unsigned long)cs.bytes_out);
  cmd_reply(ctx, buf);

  if(cs.total_requests > 0)
  {
    double err_pct = (double)cs.total_errors /
        (double)cs.total_requests * 100.0;

    snprintf(buf, sizeof(buf), "  error rate: %.1f%%", err_pct);
    cmd_reply(ctx, buf);
  }

  // Show queued requests if any.
  if(cs.queued > 0)
  {
    curllist_state_t st = { .ctx = ctx, .count = 0 };

    cmd_reply(ctx, "queued requests:");
    curl_iterate_active(show_curl_cb, &st);
  }
}

// -----------------------------------------------------------------------
// /show resolve — resolver statistics
// -----------------------------------------------------------------------

// /show resolve — display resolver query statistics.
static void
admin_cmd_show_resolve(const cmd_ctx_t *ctx)
{
  resolve_stats_t rs;
  char buf[256];

  resolve_get_stats(&rs);

  snprintf(buf, sizeof(buf),
      "resolver: %lu queries, %lu failures",
      (unsigned long)rs.queries, (unsigned long)rs.failures);
  cmd_reply(ctx, buf);

  if(rs.queries > 0)
  {
    double fail_pct = (double)rs.failures /
        (double)rs.queries * 100.0;

    snprintf(buf, sizeof(buf), "  failure rate: %.1f%%", fail_pct);
    cmd_reply(ctx, buf);
  }

  cmd_reply(ctx, "  queries by type:");

  static const char *type_names[] = {
    "A", "AAAA", "MX", "TXT", "CNAME", "NS", "PTR", "SRV", "SOA"
  };

  for(uint32_t i = 0; i < RESOLVE_TYPE_COUNT; i++)
  {
    if(rs.by_type[i] > 0)
    {
      snprintf(buf, sizeof(buf), "    %-6s %lu",
          type_names[i], (unsigned long)rs.by_type[i]);
      cmd_reply(ctx, buf);
    }
  }
}

// -----------------------------------------------------------------------
// /show sessions <botname> — per-bot session detail
// -----------------------------------------------------------------------

// Arg descriptor for /show sessions.
static const cmd_arg_desc_t ad_show_sessions[] = {
  { "botname", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
};

// Session iteration callback: formats one active session.
// username: authenticated username
// method_name: method instance the session is on
// auth_time: authentication timestamp
// last_seen: last activity timestamp
// data: sessionlist_state_t pointer
static void
show_session_cb(const char *username, const char *method_name,
    time_t auth_time, time_t last_seen, void *data)
{
  sessionlist_state_t *st = data;
  char line[256];
  char auth_dur[32];
  char idle_dur[32];
  time_t now = time(NULL);

  if(auth_time > 0)
    admin_fmt_duration(auth_dur, sizeof(auth_dur), now - auth_time);

  else
    snprintf(auth_dur, sizeof(auth_dur), "-");

  if(last_seen > 0)
    admin_fmt_duration(idle_dur, sizeof(idle_dur), now - last_seen);

  else
    snprintf(idle_dur, sizeof(idle_dur), "-");

  snprintf(line, sizeof(line),
      "  %-20s method=%-16s authed=%s idle=%s",
      username, method_name, auth_dur, idle_dur);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /show sessions <botname> — list active authenticated sessions for a bot.
static void
admin_cmd_show_sessions(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  bot_inst_t *bot = bot_find(botname);

  if(bot == NULL)
  {
    cmd_reply(ctx, "bot not found");
    return;
  }

  char hdr[128];

  snprintf(hdr, sizeof(hdr), "sessions for '%s' (%u active):",
      botname, bot_session_count(bot));
  cmd_reply(ctx, hdr);

  sessionlist_state_t st = { .ctx = ctx, .count = 0 };

  bot_session_iterate(bot, show_session_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// -----------------------------------------------------------------------
// /show db — database pool details
// -----------------------------------------------------------------------

// DB pool iteration callback: formats one pool slot.
// slot: slot index
// state: connection state
// queries: per-slot query count
// created: when connection was created
// last_used: last query timestamp
// data: dblist_state_t pointer
static void
show_db_cb(uint16_t slot, db_conn_state_t state, uint64_t queries,
    time_t created, time_t last_used, void *data)
{
  dblist_state_t *st = data;
  char line[256];

  const char *state_str;

  switch(state)
  {
    case DB_CONN_IDLE:   state_str = "idle";   break;
    case DB_CONN_ACTIVE: state_str = "active"; break;
    case DB_CONN_FAIL:   state_str = "failed"; break;
    default:             state_str = "?";      break;
  }

  char age[32];

  if(created > 0)
    admin_fmt_duration(age, sizeof(age), time(NULL) - created);
  else
    snprintf(age, sizeof(age), "-");

  char idle[32];

  if(last_used > 0)
    admin_fmt_duration(idle, sizeof(idle), time(NULL) - last_used);
  else
    snprintf(idle, sizeof(idle), "-");

  snprintf(line, sizeof(line),
      "  %-3u %-6s queries=%-8lu age=%-10s idle=%s",
      slot, state_str, (unsigned long)queries, age, idle);
  cmd_reply(st->ctx, line);
  st->count++;
}

// /show db — display database connection pool details.
static void
admin_cmd_show_db(const cmd_ctx_t *ctx)
{
  db_pool_stats_t ds;

  db_get_pool_stats(&ds);

  char buf[256];

  snprintf(buf, sizeof(buf),
      "db pool: %u total, %u idle, %u active, %u failed, "
      "%lu queries, %lu errors",
      ds.total, ds.idle, ds.active, ds.failed,
      (unsigned long)ds.queries, (unsigned long)ds.errors);
  cmd_reply(ctx, buf);

  if(ds.total > 0)
  {
    dblist_state_t st = { .ctx = ctx, .count = 0 };

    cmd_reply(ctx, "connections:");
    db_iterate_pool(show_db_cb, &st);

    if(st.count == 0)
      cmd_reply(ctx, "  (none)");
  }
}

// NOTE: /show parent handler is now built into cmd.c (cmd_builtin_show).
// The admin module only registers subcommands under /show.

// -----------------------------------------------------------------------
// /status — system health dashboard
// -----------------------------------------------------------------------
static void
admin_cmd_status(const cmd_ctx_t *ctx)
{
  char buf[512];

  cmd_reply(ctx, BM_VERSION_STR);

  // Uptime and total messages processed.
  if(bm_start_time > 0)
  {
    time_t elapsed = time(NULL) - bm_start_time;
    uint32_t days  = (uint32_t)(elapsed / 86400);
    uint32_t hours = (uint32_t)((elapsed % 86400) / 3600);
    uint32_t mins  = (uint32_t)((elapsed % 3600) / 60);

    method_stats_t mts_up;

    method_get_stats(&mts_up);

    snprintf(buf, sizeof(buf),
        "uptime: %ud %uh %um — %lu messages processed",
        days, hours, mins,
        (unsigned long)mts_up.total_msg_in);
    cmd_reply(ctx, buf);
  }

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

  cmd_stats_t cmds;

  cmd_get_stats(&cmds);

  snprintf(buf, sizeof(buf),
      "commands: %u registered, %lu dispatches, %lu denials",
      cmds.registered,
      (unsigned long)cmds.dispatches, (unsigned long)cmds.denials);
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

  userns_stats_t uns;

  userns_get_stats(&uns);

  snprintf(buf, sizeof(buf),
      "userns: %u namespaces, %u users, %lu auth, %lu failed, "
      "%lu mfa, %lu discovered",
      uns.namespaces, uns.users,
      (unsigned long)uns.auth_attempts, (unsigned long)uns.auth_failures,
      (unsigned long)uns.mfa_matches, (unsigned long)uns.discoveries);
  cmd_reply(ctx, buf);

  resolve_stats_t rs;

  resolve_get_stats(&rs);

  snprintf(buf, sizeof(buf),
      "resolver: %lu queries, %lu failures, A=%lu AAAA=%lu MX=%lu "
      "TXT=%lu NS=%lu PTR=%lu",
      (unsigned long)rs.queries, (unsigned long)rs.failures,
      (unsigned long)rs.by_type[RESOLVE_A],
      (unsigned long)rs.by_type[RESOLVE_AAAA],
      (unsigned long)rs.by_type[RESOLVE_MX],
      (unsigned long)rs.by_type[RESOLVE_TXT],
      (unsigned long)rs.by_type[RESOLVE_NS],
      (unsigned long)rs.by_type[RESOLVE_PTR]);
  cmd_reply(ctx, buf);

  plugin_stats_t pls;

  plugin_get_stats(&pls);

  snprintf(buf, sizeof(buf),
      "plugins: %u loaded, %u discovered, %u rejected, %u errors",
      pls.loaded, pls.discovered, pls.rejected, pls.load_errors);
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
// Dynamic help generators
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Module lifecycle
// -----------------------------------------------------------------------

// Register all administrative commands and delegate to sub-modules.
// /help, /show, /set are already registered as root commands by cmd_init().
// This function registers subcommands under them plus other admin commands.
void
admin_init(void)
{
  // /set kv <key> <value> — raw KV store write.
  cmd_register("admin", "kv",
      "set kv <key> <value>",
      "Set a configuration value",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_set, NULL, "set", NULL, ad_set, 2);

  // /show subcommands.
  cmd_register("admin", "kv",
      "show kv [prefix]",
      "Show configuration values",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_kv, NULL, "show", "k", ad_show_kv, 1);

  cmd_register("admin", "methods",
      "show methods",
      "List method types and instances",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_methods, NULL, "show", "m", NULL, 0);

  cmd_register("admin", "status",
      "show status",
      "System health dashboard",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_status, NULL, "show", "st", NULL, 0);

  cmd_register("admin", "schema",
      "show schema [plugin [group]]",
      "Show plugin entity schemas",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_schema, NULL, "show", "sc", ad_show_schema, 2);

  cmd_register("admin", "sockets",
      "show sockets",
      "List active socket sessions",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_sockets, NULL, "show", "sock", NULL, 0);

  cmd_register("admin", "curl",
      "show curl",
      "Show curl subsystem state",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_curl, NULL, "show", "cu", NULL, 0);

  cmd_register("admin", "resolve",
      "show resolve",
      "Show resolver statistics",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_resolve, NULL, "show", "dns", NULL, 0);

  cmd_register("admin", "db",
      "show db",
      "Show database pool details",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_db, NULL, "show", "d", NULL, 0);

  cmd_register("admin", "sessions",
      "show sessions <botname>",
      "Show active sessions for a bot",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_show_sessions, NULL, "show", "sess",
      ad_show_sessions, 1);

  // /status — root-level alias for /show status.
  cmd_register("admin", "status",
      "status",
      "Display system health dashboard",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      admin_cmd_status, NULL, NULL, "stat", NULL, 0);

  // Delegate to sub-modules.
  admin_register_user_commands();
  admin_register_bot_commands();

  clam(CLAM_INFO, "admin_init", "administrative commands registered");
}

// -----------------------------------------------------------------------
// KV configuration
// -----------------------------------------------------------------------

// Register admin KV keys.
// returns: void
void
admin_register_config(void)
{
}

// Unregister all administrative commands.
// Note: cmd_exit() frees everything on shutdown, so this is primarily
// for clean ordering during hot-reload scenarios.
void
admin_exit(void)
{
  clam(CLAM_INFO, "admin_exit", "administrative commands unregistered");
}
