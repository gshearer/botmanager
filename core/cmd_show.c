// botmanager — MIT
// /show subcommands: render core-subsystem state (KV, methods, curl, db, …).

#include "common.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "curl.h"
#include "db.h"
#include "kv.h"
#include "main.h"
#include "alloc.h"
#include "method.h"
#include "plugin.h"
#include "pool.h"
#include "resolve.h"
#include "sock.h"
#include "task.h"
#include "userns.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Local types

#define SHOW_LIMIT 256

typedef struct
{
  char      key[KV_KEY_SZ];
  char      val[KV_STR_SZ];
  kv_type_t type;
} show_kv_entry_t;

typedef struct
{
  show_kv_entry_t entries[SHOW_LIMIT];
  uint32_t        count;
} show_kv_result_t;

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} show_iter_state_t;

// Argument descriptors

static const cmd_arg_desc_t ad_show_kv[] = {
  { "prefix", CMD_ARG_NONE, CMD_ARG_OPTIONAL, KV_KEY_SZ, NULL },
};

static const cmd_arg_desc_t ad_show_schema[] = {
  { "plugin", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, PLUGIN_NAME_SZ, NULL },
  { "group",  CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, 0,              NULL },
};

static const cmd_arg_desc_t ad_show_sessions[] = {
  { "botname", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
};

// Duration formatter

static void
fmt_duration(char *buf, size_t sz, time_t secs)
{
  uint32_t d, h, m;

  if(secs <= 0)
  {
    snprintf(buf, sz, "-");
    return;
  }

  d = (uint32_t)(secs / 86400);
  h = (uint32_t)((secs % 86400) / 3600);
  m = (uint32_t)((secs % 3600) / 60);

  if(d > 0)
    snprintf(buf, sz, "%ud %uh %um", d, h, m);
  else if(h > 0)
    snprintf(buf, sz, "%uh %um", h, m);
  else if(m > 0)
    snprintf(buf, sz, "%um %us", m, (uint32_t)(secs % 60));
  else
    snprintf(buf, sz, "%us", (uint32_t)secs);
}

// /show kv [prefix]

static void
show_kv_iter_cb(const char *key, kv_type_t type,
    const char *val, void *data)
{
  show_kv_result_t *gr = data;

  if(gr->count >= SHOW_LIMIT)
    return;

  strncpy(gr->entries[gr->count].key, key, KV_KEY_SZ - 1);
  gr->entries[gr->count].key[KV_KEY_SZ - 1] = '\0';
  strncpy(gr->entries[gr->count].val, val, KV_STR_SZ - 1);
  gr->entries[gr->count].val[KV_STR_SZ - 1] = '\0';
  gr->entries[gr->count].type = type;
  gr->count++;
}

static int
show_kv_entry_cmp(const void *a, const void *b)
{
  const show_kv_entry_t *ea = a;
  const show_kv_entry_t *eb = b;

  return(strcmp(ea->key, eb->key));
}

static void
cmd_show_kv(const cmd_ctx_t *ctx)
{
  const char *prefix = (ctx->parsed && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : "";

  show_kv_result_t gr;
  memset(&gr, 0, sizeof(gr));

  kv_iterate_prefix(prefix, show_kv_iter_cb, &gr);

  if(gr.count == 0)
  {
    if(prefix[0] != '\0')
    {
      char buf[KV_KEY_SZ + 32];

      snprintf(buf, sizeof(buf), "no matching keys: %s", prefix);
      cmd_reply(ctx, buf);
    }

    else
      cmd_reply(ctx, "no configuration entries");

    return;
  }

  qsort(gr.entries, gr.count, sizeof(gr.entries[0]), show_kv_entry_cmp);

  for(uint32_t i = 0; i < gr.count; i++)
  {
    char line[KV_KEY_SZ + KV_STR_SZ + 32];

    snprintf(line, sizeof(line), "  %s = %s (%s)",
        gr.entries[i].key, gr.entries[i].val,
        kv_type_name(gr.entries[i].type));
    cmd_reply(ctx, line);
  }
}

// /show methods

static void
show_method_type_cb(const char *name, method_type_t bit,
    const char *desc, void *data)
{
  show_iter_state_t *st = data;
  char line[256];

  (void)bit;
  snprintf(line, sizeof(line), "  %-12s — %s", name, desc);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
show_method_inst_cb(const char *name, const char *kind,
    method_state_t state, uint64_t msg_in, uint64_t msg_out,
    uint32_t sub_count, time_t connected_at, void *data)
{
  show_iter_state_t *st = data;
  char line[256];
  char uptime[32];

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

static void
cmd_show_methods(const cmd_ctx_t *ctx)
{
  show_iter_state_t st = { .ctx = ctx, .count = 0 };

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

// /show schema [plugin [group]]

static void
show_schema_iter_cb(const plugin_desc_t *plugin,
    const plugin_kv_group_t *group, void *data)
{
  const cmd_ctx_t *ctx = data;
  char line[256];

  snprintf(line, sizeof(line), "  %-12s %-12s — %s",
      plugin->name, group->name, group->description);
  cmd_reply(ctx, line);
}

static void
cmd_show_schema(const cmd_ctx_t *ctx)
{
  const char *plugin_name = (ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;
  const char *group_name = (ctx->parsed->argc > 1)
      ? ctx->parsed->argv[1] : NULL;
  const plugin_desc_t *pd;
  const plugin_kv_group_t *g;
  char hdr[256];

  // No arguments: list all schema groups across all plugins.
  if(plugin_name == NULL)
  {
    cmd_reply(ctx, "plugin entity schemas:");
    cmd_reply(ctx, "  plugin       group        description");
    plugin_kv_group_iterate(show_schema_iter_cb, (void *)ctx);
    return;
  }

  // Plugin name only: list groups for that plugin.
  pd = plugin_find(plugin_name);
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
  g = plugin_kv_group_find(plugin_name, group_name);
  if(g == NULL)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s: unknown schema group: %s",
        plugin_name, group_name);
    cmd_reply(ctx, buf);
    return;
  }

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

// /show sockets

static void
show_sock_cb(uint32_t id, sock_type_t type, int state,
    const char *remote, uint64_t bytes_in, uint64_t bytes_out,
    bool tls, time_t connected_at, void *data)
{
  show_iter_state_t *st = data;
  char line[512];
  char dur[32];

  if(connected_at > 0)
    fmt_duration(dur, sizeof(dur), time(NULL) - connected_at);
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

static void
cmd_show_sockets(const cmd_ctx_t *ctx)
{
  show_iter_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx, "socket sessions:");
  sock_iterate(show_sock_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// /show curl

static void
show_curl_cb(const char *url, curl_method_t method,
    uint32_t elapsed_secs, void *data)
{
  show_iter_state_t *st = data;
  char line[512];

  (void)elapsed_secs;
  snprintf(line, sizeof(line), "  queued  %-6s %s",
      curl_method_name(method), url);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_curl(const cmd_ctx_t *ctx)
{
  curl_stats_t cs;
  char buf[256];
  uint64_t avg_ms;

  curl_get_stats(&cs);

  avg_ms = cs.total_requests > 0
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
    show_iter_state_t st = { .ctx = ctx, .count = 0 };

    cmd_reply(ctx, "queued requests:");
    curl_iterate_active(show_curl_cb, &st);
  }
}

// /show resolve

static void
cmd_show_resolve(const cmd_ctx_t *ctx)
{
  static const char *type_names[] = {
    "A", "AAAA", "MX", "TXT", "CNAME", "NS", "PTR", "SRV", "SOA"
  };
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

// /show sessions <botname>

static void
show_session_cb(const char *username, const char *method_name,
    time_t auth_time, time_t last_seen, void *data)
{
  show_iter_state_t *st = data;
  char line[256];
  char auth_dur[32];
  char idle_dur[32];
  time_t now = time(NULL);

  if(auth_time > 0)
    fmt_duration(auth_dur, sizeof(auth_dur), now - auth_time);

  else
    snprintf(auth_dur, sizeof(auth_dur), "-");

  if(last_seen > 0)
    fmt_duration(idle_dur, sizeof(idle_dur), now - last_seen);

  else
    snprintf(idle_dur, sizeof(idle_dur), "-");

  snprintf(line, sizeof(line),
      "  %-20s method=%-16s authed=%s idle=%s",
      username, method_name, auth_dur, idle_dur);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_sessions(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  bot_inst_t *bot;
  char hdr[128];
  show_iter_state_t st = { .ctx = ctx, .count = 0 };

  bot = bot_find(botname);
  if(bot == NULL)
  {
    cmd_reply(ctx, "bot not found");
    return;
  }

  snprintf(hdr, sizeof(hdr), "sessions for '%s' (%u active):",
      botname, bot_session_count(bot));
  cmd_reply(ctx, hdr);

  bot_session_iterate(bot, show_session_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
}

// /show db

static void
show_db_cb(uint16_t slot, db_conn_state_t state, uint64_t queries,
    time_t created, time_t last_used, void *data)
{
  show_iter_state_t *st = data;
  char line[256];
  const char *state_str;
  char age[32];
  char idle[32];

  switch(state)
  {
    case DB_CONN_IDLE:   state_str = "idle";   break;
    case DB_CONN_ACTIVE: state_str = "active"; break;
    case DB_CONN_FAIL:   state_str = "failed"; break;
    default:             state_str = "?";      break;
  }

  if(created > 0)
    fmt_duration(age, sizeof(age), time(NULL) - created);
  else
    snprintf(age, sizeof(age), "-");

  if(last_used > 0)
    fmt_duration(idle, sizeof(idle), time(NULL) - last_used);
  else
    snprintf(idle, sizeof(idle), "-");

  snprintf(line, sizeof(line),
      "  %-3u %-6s queries=%-8lu age=%-10s idle=%s",
      slot, state_str, (unsigned long)queries, age, idle);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_db(const cmd_ctx_t *ctx)
{
  db_pool_stats_t ds;
  char buf[256];

  db_get_pool_stats(&ds);

  snprintf(buf, sizeof(buf),
      "db pool: %u total, %u idle, %u active, %u failed, "
      "%lu queries, %lu errors",
      ds.total, ds.idle, ds.active, ds.failed,
      (unsigned long)ds.queries, (unsigned long)ds.errors);
  cmd_reply(ctx, buf);

  if(ds.total > 0)
  {
    show_iter_state_t st = { .ctx = ctx, .count = 0 };

    cmd_reply(ctx, "connections:");
    db_iterate_pool(show_db_cb, &st);

    if(st.count == 0)
      cmd_reply(ctx, "  (none)");
  }
}

// /show pool — thread pool dashboard

// Append a 10-cell utilization bar (filled blocks for active workers)
// into out. Each filled cell ≈ 10% of max capacity.
static void
fmt_util_bar(char *out, size_t sz, uint32_t active, uint32_t max)
{
  uint32_t filled;
  size_t off = 0;

  if(max == 0) max = 1;
  filled = (active * 10 + max / 2) / max;
  if(filled > 10) filled = 10;

  off += (size_t)snprintf(out + off, sz - off, CLR_GREEN);
  for(uint32_t i = 0; i < filled; i++)
    off += (size_t)snprintf(out + off, sz - off, "\xe2\x96\xb0");  // ▰
  off += (size_t)snprintf(out + off, sz - off, CLR_GRAY);
  for(uint32_t i = filled; i < 10; i++)
    off += (size_t)snprintf(out + off, sz - off, "\xe2\x96\xb1");  // ▱
  snprintf(out + off, sz - off, CLR_RESET);
}

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} pool_persist_state_t;

static void
pool_persist_cb(const char *name, task_state_t state, task_kind_t kind,
    task_type_t type, uint8_t priority, uint32_t run_count,
    uint32_t interval_ms, time_t created, time_t last_run,
    time_t sleep_until, void *data)
{
  pool_persist_state_t *st;
  char age[16];
  char line[256];

  (void)state; (void)type; (void)interval_ms;
  (void)last_run; (void)sleep_until;

  if(kind != TASK_PERSIST) return;

  st = data;
  util_fmt_duration(time(NULL) - created, age, sizeof(age));

  snprintf(line, sizeof(line),
      "    " CLR_PURPLE "%-22s" CLR_RESET
      "  pri=%-3u  runs=%-6u  age=%s",
      name, priority, run_count, age);
  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_pool(const cmd_ctx_t *ctx)
{
  pool_stats_t ps;
  task_stats_t ts;
  uint16_t max_threads;
  uint16_t min_threads;
  uint16_t min_spare;
  uint32_t max_idle_secs;
  uint32_t wait_ms;
  uint32_t active;
  uint32_t pct;
  char bar[160];
  char line[512];
  pool_persist_state_t pst = { .ctx = ctx, .count = 0 };

  pool_get_stats(&ps);
  task_get_stats(&ts);

  max_threads   = (uint16_t)kv_get_uint("core.pool.max_threads");
  min_threads   = (uint16_t)kv_get_uint("core.pool.min_threads");
  min_spare     = (uint16_t)kv_get_uint("core.pool.min_spare");
  max_idle_secs = (uint32_t)kv_get_uint("core.pool.max_idle_secs");
  wait_ms       = (uint32_t)kv_get_uint("core.pool.wait_ms");

  active = (ps.total > ps.idle) ? (ps.total - ps.idle) : 0;
  pct    = max_threads > 0
      ? (uint32_t)(((uint64_t)active * 100) / max_threads)
      : 0;

  // Header.
  cmd_reply(ctx, "");
  cmd_reply(ctx, CLR_BOLD CLR_CYAN
      "Thread Pool" CLR_RESET);
  cmd_reply(ctx, CLR_GRAY
      "  ─────────────────────────────────────────" CLR_RESET);

  fmt_util_bar(bar, sizeof(bar), active, max_threads);

  snprintf(line, sizeof(line),
      "  " CLR_CYAN "workers" CLR_RESET "      %s  "
      CLR_BOLD "%u" CLR_RESET " / %u alive   "
      "(peak " CLR_YELLOW "%u" CLR_RESET ")   "
      "util %s%u%%" CLR_RESET,
      bar, ps.total, max_threads, ps.peak_workers,
      pct >= 80 ? CLR_RED : pct >= 50 ? CLR_YELLOW : CLR_GREEN, pct);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  " CLR_CYAN "active" CLR_RESET
      "       " CLR_GREEN "%u" CLR_RESET
      "        " CLR_CYAN "idle" CLR_RESET "  %u",
      active, ps.idle);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  " CLR_CYAN "lifetime" CLR_RESET
      "     " CLR_BOLD "%lu" CLR_RESET " jobs completed",
      (unsigned long)ps.jobs_completed);
  cmd_reply(ctx, line);

  // Config knobs.
  cmd_reply(ctx, "");
  cmd_reply(ctx, "  " CLR_CYAN "config" CLR_RESET);
  snprintf(line, sizeof(line),
      "    min=%u  spare=%u  max_idle=%us  wait=%ums",
      min_threads, min_spare, max_idle_secs, wait_ms);
  cmd_reply(ctx, line);

  // Persistent threads list.
  cmd_reply(ctx, "");
  snprintf(line, sizeof(line),
      "  " CLR_CYAN "persist" CLR_RESET "      "
      CLR_PURPLE "%u" CLR_RESET " dedicated thread(s)", ps.persist);
  cmd_reply(ctx, line);

  task_iterate(pool_persist_cb, &pst);
  if(pst.count == 0)
    cmd_reply(ctx, "    " CLR_GRAY "(none)" CLR_RESET);

  // Task summary footer.
  cmd_reply(ctx, "");
  snprintf(line, sizeof(line),
      "  " CLR_CYAN "tasks" CLR_RESET "        "
      CLR_BOLD "%u" CLR_RESET " total — "
      "%s%u running" CLR_RESET ", "
      "%s%u waiting" CLR_RESET ", "
      "%s%u sleeping" CLR_RESET ", "
      "%s%u periodic" CLR_RESET,
      ts.total,
      CLR_GREEN,  ts.running,
      CLR_YELLOW, ts.waiting,
      CLR_CYAN,   ts.sleeping,
      CLR_BLUE,   ts.periodic);
  cmd_reply(ctx, line);
  cmd_reply(ctx, "");
}

// /show version

static void
cmd_show_version(const cmd_ctx_t *ctx)
{
  char buf[128];

  snprintf(buf, sizeof(buf), "BotManager v%u.%u (build #%u)",
      (uint32_t)BM_VERSION_MAJOR, (uint32_t)BM_VERSION_MINOR,
      (uint32_t)BM_BUILD_NUM);
  cmd_reply(ctx, buf);
}

// /show status — system health dashboard.

// Color a counter: red if >0, gray if 0. Used for error/denial counters
// where any non-zero value is worth highlighting.
static const char *
err_color(uint64_t n)
{
  return(n > 0 ? CLR_RED : CLR_GRAY);
}

// Section header line (bold cyan title + gray underline rule).
static void
status_section(const cmd_ctx_t *ctx, const char *title)
{
  char line[128];
  snprintf(line, sizeof(line),
      CLR_BOLD CLR_CYAN "%s" CLR_RESET, title);
  cmd_reply(ctx, line);
  cmd_reply(ctx, CLR_GRAY
      "  ─────────────────────────────────────────" CLR_RESET);
}

static void
cmd_show_status(const cmd_ctx_t *ctx)
{
  char buf[512];
  mem_stats_t ms;
  task_stats_t ts;
  pool_stats_t ps;
  sock_stats_t ss;
  method_stats_t mts;
  curl_stats_t cs;
  uint64_t avg_ms;
  resolve_stats_t rs;
  bot_stats_t bs;
  cmd_stats_t cmds;
  userns_stats_t uns;
  db_pool_stats_t ds;
  plugin_stats_t pls;

  // ---- banner ----
  cmd_reply(ctx, "");
  cmd_reply(ctx, CLR_BOLD CLR_GREEN BM_VERSION_STR CLR_RESET);

  if(bm_start_time > 0)
  {
    time_t elapsed = time(NULL) - bm_start_time;
    uint32_t days  = (uint32_t)(elapsed / 86400);
    uint32_t hours = (uint32_t)((elapsed % 86400) / 3600);
    uint32_t mins  = (uint32_t)((elapsed % 3600) / 60);
    method_stats_t mts_up;

    method_get_stats(&mts_up);

    snprintf(buf, sizeof(buf),
        CLR_GRAY "  uptime " CLR_RESET CLR_BOLD "%ud %uh %um" CLR_RESET
        CLR_GRAY "   messages " CLR_RESET CLR_BOLD "%lu" CLR_RESET,
        days, hours, mins,
        (unsigned long)mts_up.total_msg_in);
    cmd_reply(ctx, buf);
  }
  cmd_reply(ctx, "");

  // ---- runtime ----
  status_section(ctx, "Runtime");

  mem_get_stats(&ms);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "memory" CLR_RESET "    "
      CLR_BOLD "%zu" CLR_RESET " bytes  "
      "(peak " CLR_YELLOW "%zu" CLR_RESET ")  "
      "%lu live  %lu lifetime  %lu freelist",
      ms.heap_sz, ms.peak_heap_sz,
      (unsigned long)ms.active, (unsigned long)ms.total_allocs,
      (unsigned long)ms.freelist);
  cmd_reply(ctx, buf);

  task_get_stats(&ts);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "tasks" CLR_RESET "     "
      CLR_BOLD "%u" CLR_RESET " total  "
      "%s%u" CLR_RESET " running  "
      "%s%u" CLR_RESET " waiting  "
      "%s%u" CLR_RESET " sleeping  "
      "%s%u" CLR_RESET " periodic",
      ts.total,
      CLR_GREEN,  ts.running,
      CLR_YELLOW, ts.waiting,
      CLR_CYAN,   ts.sleeping,
      CLR_BLUE,   ts.periodic);
  cmd_reply(ctx, buf);

  pool_get_stats(&ps);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "threads" CLR_RESET "   "
      CLR_BOLD "%u" CLR_RESET " workers  "
      "(peak " CLR_YELLOW "%u" CLR_RESET ")  "
      "%u idle  "
      CLR_PURPLE "%u" CLR_RESET " persist  "
      CLR_GRAY "%lu jobs" CLR_RESET,
      ps.total, ps.peak_workers, ps.idle, ps.persist,
      (unsigned long)ps.jobs_completed);
  cmd_reply(ctx, buf);

  cmd_reply(ctx, "");

  // ---- network ----
  status_section(ctx, "Network");

  sock_get_stats(&ss);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "sockets" CLR_RESET "   "
      CLR_BOLD "%u" CLR_RESET " sessions  "
      CLR_GREEN "%u" CLR_RESET " connected  "
      "%u tls  "
      CLR_GRAY "in=%lu out=%lu" CLR_RESET,
      ss.sessions, ss.connected, ss.tls_sessions,
      (unsigned long)ss.bytes_in, (unsigned long)ss.bytes_out);
  cmd_reply(ctx, buf);

  method_get_stats(&mts);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "methods" CLR_RESET "   "
      CLR_BOLD "%u" CLR_RESET " instances  "
      "%u subscribers  "
      CLR_GRAY "in=%lu out=%lu" CLR_RESET,
      mts.instances, mts.subscribers,
      (unsigned long)mts.total_msg_in, (unsigned long)mts.total_msg_out);
  cmd_reply(ctx, buf);

  curl_get_stats(&cs);
  avg_ms = cs.total_requests > 0
      ? cs.total_response_ms / cs.total_requests : 0;
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "curl" CLR_RESET "      "
      "%u active  %u queued  "
      CLR_BOLD "%lu" CLR_RESET " total  "
      "%s%lu errors" CLR_RESET "  "
      CLR_GRAY "%lums avg" CLR_RESET,
      cs.active, cs.queued,
      (unsigned long)cs.total_requests,
      err_color(cs.total_errors), (unsigned long)cs.total_errors,
      (unsigned long)avg_ms);
  cmd_reply(ctx, buf);

  resolve_get_stats(&rs);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "resolver" CLR_RESET "  "
      CLR_BOLD "%lu" CLR_RESET " queries  "
      "%s%lu failures" CLR_RESET "  "
      CLR_GRAY "A=%lu AAAA=%lu MX=%lu TXT=%lu NS=%lu PTR=%lu" CLR_RESET,
      (unsigned long)rs.queries,
      err_color(rs.failures), (unsigned long)rs.failures,
      (unsigned long)rs.by_type[RESOLVE_A],
      (unsigned long)rs.by_type[RESOLVE_AAAA],
      (unsigned long)rs.by_type[RESOLVE_MX],
      (unsigned long)rs.by_type[RESOLVE_TXT],
      (unsigned long)rs.by_type[RESOLVE_NS],
      (unsigned long)rs.by_type[RESOLVE_PTR]);
  cmd_reply(ctx, buf);

  cmd_reply(ctx, "");

  // ---- application ----
  status_section(ctx, "Application");

  bot_get_stats(&bs);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "bots" CLR_RESET "      "
      CLR_BOLD "%u" CLR_RESET " instances  "
      CLR_GREEN "%u" CLR_RESET " running  "
      "%u methods  %u sessions  "
      CLR_GRAY "cmds=%lu" CLR_RESET "  "
      "%s%lu denied" CLR_RESET,
      bs.instances, bs.running, bs.methods, bs.sessions,
      (unsigned long)bs.cmd_dispatches,
      err_color(bs.cmd_denials), (unsigned long)bs.cmd_denials);
  cmd_reply(ctx, buf);

  cmd_get_stats(&cmds);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "commands" CLR_RESET "  "
      CLR_BOLD "%u" CLR_RESET " registered  "
      CLR_GRAY "%lu dispatches" CLR_RESET "  "
      "%s%lu denials" CLR_RESET,
      cmds.registered,
      (unsigned long)cmds.dispatches,
      err_color(cmds.denials), (unsigned long)cmds.denials);
  cmd_reply(ctx, buf);

  userns_get_stats(&uns);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "userns" CLR_RESET "    "
      CLR_BOLD "%u" CLR_RESET " namespaces  "
      "%u users  "
      CLR_GRAY "auth=%lu mfa=%lu" CLR_RESET "  "
      "%s%lu failed" CLR_RESET,
      uns.namespaces, uns.users,
      (unsigned long)uns.auth_attempts,
      (unsigned long)uns.mfa_matches,
      err_color(uns.auth_failures), (unsigned long)uns.auth_failures);
  cmd_reply(ctx, buf);

  cmd_reply(ctx, "");

  // ---- storage / plugins ----
  status_section(ctx, "Storage");

  db_get_pool_stats(&ds);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "db pool" CLR_RESET "   "
      CLR_BOLD "%u" CLR_RESET " total  "
      "%u idle  %u active  "
      "%s%u failed" CLR_RESET "  "
      CLR_GRAY "%lu queries" CLR_RESET "  "
      "%s%lu errors" CLR_RESET,
      ds.total, ds.idle, ds.active,
      err_color(ds.failed), ds.failed,
      (unsigned long)ds.queries,
      err_color(ds.errors), (unsigned long)ds.errors);
  cmd_reply(ctx, buf);

  plugin_get_stats(&pls);
  snprintf(buf, sizeof(buf),
      "  " CLR_CYAN "plugins" CLR_RESET "   "
      CLR_BOLD "%u" CLR_RESET " loaded  "
      "%u discovered  "
      "%s%u rejected" CLR_RESET "  "
      "%s%u errors" CLR_RESET,
      pls.loaded, pls.discovered,
      err_color(pls.rejected), pls.rejected,
      err_color(pls.load_errors), pls.load_errors);
  cmd_reply(ctx, buf);

  cmd_reply(ctx, "");
}

// /show extract {root,stats} was hosted here until R2; the handlers and
// registration moved into the chat plugin (plugins/bot/chat/show_verbs.c)
// since the extract subsystem itself lives there now.

// Registration

void
cmd_show_register(void)
{
  cmd_register("cmd", "kv",
      "show kv [prefix]",
      "Show configuration values",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_kv, NULL, "show", "k", ad_show_kv, 1, NULL, NULL);

  cmd_register("cmd", "methods",
      "show methods",
      "List method types and instances",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_methods, NULL, "show", "m", NULL, 0, NULL, NULL);

  cmd_register("cmd", "status",
      "show status",
      "System health dashboard",
      NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_status, NULL, "show", "st", NULL, 0, NULL, NULL);

  cmd_register("cmd", "schema",
      "show schema [plugin [group]]",
      "Show plugin entity schemas",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_schema, NULL, "show", "sc", ad_show_schema, 2, NULL, NULL);

  cmd_register("cmd", "sockets",
      "show sockets",
      "List active socket sessions",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_sockets, NULL, "show", "sock", NULL, 0, NULL, NULL);

  cmd_register("cmd", "curl",
      "show curl",
      "Show curl subsystem state",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_curl, NULL, "show", "cu", NULL, 0, NULL, NULL);

  cmd_register("cmd", "resolve",
      "show resolve",
      "Show resolver statistics",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_resolve, NULL, "show", "dns", NULL, 0, NULL, NULL);

  cmd_register("cmd", "db",
      "show db",
      "Show database pool details",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_db, NULL, "show", "d", NULL, 0, NULL, NULL);

  cmd_register("cmd", "sessions",
      "show sessions <botname>",
      "Show active sessions for a bot",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_sessions, NULL, "show", "sess",
      ad_show_sessions, 1, NULL, NULL);

  // /show pool — thread pool dashboard.
  cmd_register("cmd", "pool",
      "show pool",
      "Thread pool dashboard (workers, persist threads, jobs)",
      "Renders worker counts, idle/active breakdown, utilization\n"
      "bar against the configured max, persistent thread list, and\n"
      "lifetime job counter. Config knobs (min, spare, max_idle, wait)\n"
      "live in KV under core.pool.*.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_pool, NULL, "show", NULL, NULL, 0, NULL, NULL);

  cmd_register("cmd", "version",
      "show version",
      "Show botmanager version and build number",
      NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_version, NULL, "show", "ver", NULL, 0, NULL, NULL);

  // /show extract {root,stats} is registered by the chat plugin as of R2.
}
