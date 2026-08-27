// botmanager — MIT
// Built-in /bot administration commands (add, start, stop, show).
#define BOT_INTERNAL
#define BOT_CMD_INTERNAL
#include "bot.h"
#include "cmd.h"
#include "sig.h"

// Resolve a bot by name from argv[0]. Emits an error reply and returns
// NULL when not found. Every bot-scoped subcommand takes an explicit
// bot name as its first argument -- no session "cd" state.
static bot_inst_t *
resolve_named_bot(const cmd_ctx_t *ctx, const char *name)
{
  bot_inst_t *inst;

  if(name == NULL || name[0] == '\0')
  {
    cmd_reply(ctx, "bot name required");
    return(NULL);
  }

  inst = bot_find(name);
  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];

    snprintf(buf, sizeof(buf), "bot not found: %s", name);
    cmd_reply(ctx, buf);
    return(NULL);
  }

  return(inst);
}

// Argument descriptors

static const cmd_arg_desc_t ad_bot_name_kind[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ - 1,    NULL },
  { "kind", CMD_ARG_NONE,  CMD_ARG_OPTIONAL, PLUGIN_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_bot_name[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_bot_method[] = {
  { "name",   CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ - 1,    NULL },
  { "method", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, PLUGIN_NAME_SZ - 1, NULL },
};

// What a scan of the loaded plugins found on the PLUGIN_BOT axis: the
// first kind, and how many there were to choose from.
typedef struct
{
  char     kind[PLUGIN_NAME_SZ];
  uint32_t count;
} bot_kind_scan_t;

// plugin_iterate callback — records the first PLUGIN_BOT kind and counts
// the rest. plugin_find_type(PLUGIN_BOT, NULL) already yields the first
// one, but it cannot say whether the choice was ambiguous, and an
// unannounced pick is exactly what makes a default confusing later.
static void
bot_kind_scan_cb(const char *name, const char *version, const char *path,
    plugin_type_t type, const char *kind, plugin_state_t state, void *data)
{
  bot_kind_scan_t *scan = data;

  (void)name;
  (void)version;
  (void)path;
  (void)state;

  if(type != PLUGIN_BOT || kind == NULL || kind[0] == '\0')
    return;

  if(scan->count == 0)
    snprintf(scan->kind, sizeof(scan->kind), "%s", kind);

  scan->count++;
}

// /bot add <name> [<kind>]
//
// The bot kind is optional because there is exactly one bot plugin (see
// AGENTS.md §Terminology) — a bot is a name, a set of methods and its
// config, so naming the mind is ceremony. It stays available for the day
// a second one lands.
static void
admin_cmd_bot_add(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  const char *kind = (ctx->parsed->argc >= 2) ? ctx->parsed->argv[1] : NULL;
  bot_kind_scan_t scan = {0};
  const plugin_desc_t *pd;
  const bot_driver_t *drv;
  bot_inst_t *inst;
  bool persisted = false;
  char buf[BOT_NAME_SZ + PLUGIN_NAME_SZ + 64];

  if(bot_find(name) != NULL)
  {
    snprintf(buf, sizeof(buf), "bot already exists: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  if(kind == NULL || kind[0] == '\0')
  {
    plugin_iterate(bot_kind_scan_cb, &scan);

    if(scan.count == 0)
    {
      cmd_reply(ctx, "no bot plugin is loaded — name a kind or load one");
      return;
    }

    if(scan.count > 1)
      clam(CLAM_WARN, "bot_add",
          "%u bot plugins loaded; '%s' defaulted to kind '%s'",
          scan.count, name, scan.kind);

    kind = scan.kind;
  }

  pd = plugin_find_type(PLUGIN_BOT, kind);

  if(pd == NULL || pd->ext == NULL)
  {
    snprintf(buf, sizeof(buf), "unknown bot kind: %s", kind);
    cmd_reply(ctx, buf);
    return;
  }

  drv = (const bot_driver_t *)pd->ext;
  inst = bot_create(drv, name);
  if(inst == NULL)
  {
    cmd_reply(ctx, "failed to create bot instance");
    return;
  }

  // Persist to database. db_escape returns NULL when the pool cannot
  // hand out a connection, so the escapes are freed only where they
  // exist — mem_free aborts on NULL — and the reply tells the operator
  // when the bot lives in memory alone.
  {
    char *e_name = db_escape(name);
    char *e_kind = db_escape(kind);

    if(e_name == NULL || e_kind == NULL)
      clam(CLAM_WARN, "bot_add",
          "escape failed, '%s' not persisted (database unavailable)", name);

    else
    {
      char sql[512];
      db_result_t *r;

      snprintf(sql, sizeof(sql),
          "INSERT INTO bot_instances (name, kind) VALUES ('%s', '%s') "
          "ON CONFLICT (name) DO NOTHING",
          e_name, e_kind);

      r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_add", "DB persist failed: %s", r->error);
      else
        persisted = true;

      db_result_free(r);
    }

    if(e_name != NULL)
      mem_free(e_name);

    if(e_kind != NULL)
      mem_free(e_kind);
  }

  // Register per-instance KV keys declared by the bot driver
  // (e.g., chat's "behavior.personality" → "bot.<name>.behavior.personality").
  bot_register_driver_kv(name, kind);

  snprintf(buf, sizeof(buf), "bot created: %s (kind: %s)%s", name, kind,
      persisted ? "" : " — NOT persisted, will not survive a restart");
  cmd_reply(ctx, buf);
}

// /bot del <name> — destroy a bot
static void
admin_cmd_bot_del(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];

  if(resolve_named_bot(ctx, name) == NULL)
    return;

  if(bot_destroy(name) == SUCCESS)
  {
    // Remove from database (CASCADE deletes bot_methods rows). A row
    // left behind is resurrected by bot_restore on the next start, so
    // a failed delete is worth saying out loud.
    char *e_name = db_escape(name);
    bool  removed = false;
    char  buf[BOT_NAME_SZ + 64];

    if(e_name == NULL)
      clam(CLAM_WARN, "bot_del",
          "escape failed, '%s' left in the database", name);

    else
    {
      char sql[256];
      db_result_t *r;

      snprintf(sql, sizeof(sql),
          "DELETE FROM bot_instances WHERE name = '%s'", e_name);

      r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_del", "DB persist failed: %s", r->error);
      else
        removed = true;

      db_result_free(r);
      mem_free(e_name);
    }

    snprintf(buf, sizeof(buf), "bot destroyed: %s%s", name,
        removed ? "" : " — still in the database, it will return on restart");
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "failed to destroy bot instance");
}

// /bot list

static void
admin_cmd_bot_start(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  bot_inst_t *inst;

  inst = resolve_named_bot(ctx, name);
  if(inst == NULL)
    return;

  if(bot_start(inst) == SUCCESS)
  {
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

// /bot stop <name> — stop a bot
static void
admin_cmd_bot_stop(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  bot_inst_t *inst;

  inst = resolve_named_bot(ctx, name);
  if(inst == NULL)
    return;

  if(bot_stop(inst) == SUCCESS)
  {
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

// /say <bot> <target> <message> — make a running bot emit a line to a
// channel (or nick) over one of its bound methods. The message is the
// rest of the line. `target` is passed to the method driver verbatim
// (IRC: a "#channel" the bot has joined, or a nick for a DM). Intended
// for out-of-band announcements — e.g. the strategy competitors posting
// their scoreboard row to #cabal via the "botman" command bot.
static const cmd_arg_desc_t ad_say[] = {
  { "bot",     CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                BOT_NAME_SZ - 1, NULL },
  { "target",  CMD_ARG_NONE,  CMD_ARG_REQUIRED,                0,               NULL },
  { "message", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,               NULL },
};

static void
admin_cmd_say(const cmd_ctx_t *ctx)
{
  const char     *name    = ctx->parsed->argv[0];
  const char     *target  = ctx->parsed->argv[1];
  const char     *message = ctx->parsed->argv[2];
  bot_inst_t     *inst;
  method_inst_t  *method;
  char            buf[BOT_NAME_SZ + 128];

  inst = resolve_named_bot(ctx, name);
  if(inst == NULL)
    return;

  if(bot_get_state(inst) != BOT_RUNNING)
  {
    snprintf(buf, sizeof(buf),
        "bot not running: %s (state=%s)",
        name, bot_state_name(bot_get_state(inst)));
    cmd_reply(ctx, buf);
    return;
  }

  // First bound method — the announce use-case has a single IRC binding.
  method = bot_first_method(inst);
  if(method == NULL)
  {
    snprintf(buf, sizeof(buf), "bot has no bound method: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  if(!method_send(method, target, message))
  {
    method_release(method);
    clam(CLAM_WARN, "bot_say", "send failed: bot=%s target=%s", name, target);
    snprintf(buf, sizeof(buf), "send failed: %s -> %s", name, target);
    cmd_reply(ctx, buf);
    return;
  }

  method_release(method);

  clam(CLAM_INFO, "bot_say", "bot=%s target=%s", name, target);
  snprintf(buf, sizeof(buf), "sent: %s -> %s", name, target);
  cmd_reply(ctx, buf);
}

// /bot addmethod <name> <method> — add a method to a bot
static void
admin_cmd_bot_bind(const cmd_ctx_t *ctx)
{
  const char *botname     = ctx->parsed->argv[0];
  const char *method_kind = ctx->parsed->argv[1];
  bot_inst_t *inst;
  char inst_name[BOT_NAME_SZ + METHOD_NAME_SZ + 2];

  inst = resolve_named_bot(ctx, botname);
  if(inst == NULL)
    return;

  // Build per-bot method instance name: "<botname>_<kind>"
  snprintf(inst_name, sizeof(inst_name), "%s_%s", botname, method_kind);

  if(bot_bind_method(inst, inst_name, method_kind) == SUCCESS)
  {
    // Persist method binding.
    char *e_bot  = db_escape(botname);
    char *e_kind = db_escape(method_kind);
    bool  persisted = false;
    char  buf[256];

    if(e_bot == NULL || e_kind == NULL)
      clam(CLAM_WARN, "bot_bind",
          "escape failed, %s/%s not persisted (database unavailable)",
          botname, method_kind);

    else
    {
      char sql[512];
      db_result_t *r;

      snprintf(sql, sizeof(sql),
          "INSERT INTO bot_methods (bot_name, method_kind) "
          "VALUES ('%s', '%s') ON CONFLICT DO NOTHING",
          e_bot, e_kind);

      r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_bind", "DB persist failed: %s", r->error);
      else
        persisted = true;

      db_result_free(r);
    }

    if(e_bot != NULL)
      mem_free(e_bot);

    if(e_kind != NULL)
      mem_free(e_kind);

    // Register per-bot method KV keys (bot.<botname>.<kind>.*).
    bot_register_method_kv(botname, method_kind);

    snprintf(buf, sizeof(buf), "%s: added method %s (instance: %s)%s",
        botname, method_kind, inst_name,
        persisted ? "" : " — NOT persisted, will not survive a restart");
    cmd_reply(ctx, buf);
  }

  else
  {
    char buf[128];

    snprintf(buf, sizeof(buf),
        "failed to add %s to %s (duplicate, wrong state, or at limit)",
        method_kind, botname);
    cmd_reply(ctx, buf);
  }
}

// /bot delmethod <name> <method> — remove a method from a bot
static void
admin_cmd_bot_unbind(const cmd_ctx_t *ctx)
{
  const char *botname     = ctx->parsed->argv[0];
  const char *method_kind = ctx->parsed->argv[1];
  bot_inst_t *inst;
  char inst_name[BOT_NAME_SZ + METHOD_NAME_SZ + 2];

  inst = resolve_named_bot(ctx, botname);
  if(inst == NULL)
    return;

  // Build per-bot method instance name: "<botname>_<kind>"
  snprintf(inst_name, sizeof(inst_name), "%s_%s", botname, method_kind);

  if(bot_unbind_method(inst, inst_name) == SUCCESS)
  {
    // Remove method binding from database. A surviving row is re-bound
    // by bot_restore_methods on the next start.
    char *e_bot  = db_escape(botname);
    char *e_kind = db_escape(method_kind);
    bool  removed = false;
    char  buf[160];

    if(e_bot == NULL || e_kind == NULL)
      clam(CLAM_WARN, "bot_unbind",
          "escape failed, %s/%s left in the database", botname, method_kind);

    else
    {
      char sql[512];
      db_result_t *r;

      snprintf(sql, sizeof(sql),
          "DELETE FROM bot_methods WHERE bot_name = '%s' "
          "AND method_kind = '%s'",
          e_bot, e_kind);

      r = db_result_alloc();

      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_unbind", "DB persist failed: %s", r->error);
      else
        removed = true;

      db_result_free(r);
    }

    if(e_bot != NULL)
      mem_free(e_bot);

    if(e_kind != NULL)
      mem_free(e_kind);

    snprintf(buf, sizeof(buf), "%s: removed method %s%s",
        botname, method_kind,
        removed ? "" : " — still in the database, it will return on restart");
    cmd_reply(ctx, buf);
  }

  else
  {
    char buf[128];

    snprintf(buf, sizeof(buf),
        "failed to remove %s from %s (not bound or wrong state)",
        method_kind, botname);
    cmd_reply(ctx, buf);
  }
}

// ---- method-scoped verb resolution -------------------------------------

// True when `inst` may reach `child`. A NULL kind_filter is offered to
// every bot -- that is how the mind's own verbs, and the kind-agnostic
// bot start/stop children, stay universally available. Otherwise the
// filter names method kinds, and one bound method is enough.
static bool
bot_admits_child(const bot_inst_t *inst, const cmd_def_t *child)
{
  const char *const *filter = cmd_kind_filter_of(child);

  if(filter == NULL)
    return(true);

  for(size_t i = 0; i < CMD_KIND_FILTER_MAX && filter[i] != NULL; i++)
    if(bot_has_method_kind(inst, filter[i]))
      return(true);

  return(false);
}

// Per-child callback for cmd_find_child_for_bot.
static void
cmd_find_child_iter_cb(const cmd_def_t *c, void *data)
{
  bot_cmd_child_match_t *w = data;
  const char *cname;
  const char *abbr;

  if(w->exact != NULL || !bot_admits_child(w->inst, c))
    return;

  cname = cmd_get_name(c);
  if(cname != NULL && strncasecmp(cname, w->name, CMD_NAME_SZ) == 0)
  {
    w->exact = c;
    return;
  }

  abbr = cmd_get_abbrev(c);
  if(w->abbrev == NULL && abbr != NULL && abbr[0] != '\0'
      && strncasecmp(abbr, w->name, CMD_NAME_SZ) == 0)
    w->abbrev = c;
}

// Resolve a verb under `parent` against the methods `inst` has bound.
// Name resolution mirrors cmd_find_child(): exact name beats abbrev,
// and among equals the first registered child wins.
static const cmd_def_t *
cmd_find_child_for_bot(const cmd_def_t *parent, const char *name,
    const bot_inst_t *inst)
{
  bot_cmd_child_match_t w = { name, inst, NULL, NULL };

  if(parent == NULL || inst == NULL || name == NULL || name[0] == '\0')
    return(NULL);

  cmd_iterate_children(parent, cmd_find_child_iter_cb, &w);

  return(w.exact != NULL ? w.exact : w.abbrev);
}

// /bot — dispatcher for subcommands
// Parent handler: usage only. Every bot-scoped subcommand takes an
// explicit <name> argument -- no session state.
static void
admin_cmd_bot(const cmd_ctx_t *ctx)
{
  // Subcommand resolution already consumed known children (add, del,
  // start, stop, addmethod, delmethod). Anything left in ctx->args is
  // a name-first invocation: /bot <name> <verb> [args...].
  const char *p = ctx->args;
  char name[BOT_NAME_SZ] = {0};
  size_t n = 0;
  bot_inst_t *inst;
  char verb[CMD_NAME_SZ] = {0};
  size_t vn = 0;
  const cmd_def_t *bot_root;
  const cmd_def_t *child;
  cmd_ctx_t sub;

  if(p == NULL)
  {
    cmd_result_set(ctx, CMD_RESULT_REFUSED);
    cmd_reply(ctx, "usage: bot <subcommand> ...");
    return;
  }

  while(*p == ' ' || *p == '\t') p++;

  if(*p == '\0')
  {
    cmd_result_set(ctx, CMD_RESULT_REFUSED);
    cmd_reply(ctx, "usage: bot <subcommand> ...");
    return;
  }

  while(*p != '\0' && *p != ' ' && *p != '\t' && n + 1 < sizeof(name))
    name[n++] = *p++;

  while(*p == ' ' || *p == '\t') p++;

  inst = bot_find(name);
  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 64];

    snprintf(buf, sizeof(buf),
        "unknown subcommand or bot: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  // Pull the first whitespace-delimited token after <name> as the verb.
  while(*p != '\0' && *p != ' ' && *p != '\t' && vn + 1 < sizeof(verb))
    verb[vn++] = *p++;

  verb[vn] = '\0';

  while(*p == ' ' || *p == '\t') p++;

  if(verb[0] == '\0')
  {
    char buf[PLUGIN_NAME_SZ + BOT_NAME_SZ + 64];

    snprintf(buf, sizeof(buf),
        "usage: bot %s <verb> [args...]", name);
    cmd_reply(ctx, buf);
    return;
  }

  bot_root = cmd_find("bot");
  child    = cmd_find_child_for_bot(bot_root, verb, inst);

  if(child == NULL)
  {
    char buf[CMD_NAME_SZ + BOT_NAME_SZ + 64];

    snprintf(buf, sizeof(buf),
        "unknown verb '%s' for bot %s", verb, name);
    cmd_reply(ctx, buf);
    return;
  }

  sub = *ctx;
  sub.args   = p;
  sub.parsed = NULL;
  sub.bot    = inst;
  cmd_invoke(child, &sub);
}

// /show bots — colorized table of all bot instances

static void
show_bots_cb(const char *name, const char *method_kinds,
    bot_state_t state, uint32_t method_count,
    const char *userns_name, uint64_t cmd_count, time_t last_activity,
    void *data)
{
  bot_cmd_list_state_t *st = data;
  char line[512];
  const char *state_color;

  (void)method_count;
  (void)last_activity;

  switch(state)
  {
    case BOT_RUNNING:  state_color = CLR_GREEN;  break;
    case BOT_CREATED:  state_color = CLR_YELLOW; break;
    default:           state_color = CLR_RED;    break;
  }

  // The METHODS column is padded by byte count, so its placeholder is a
  // plain hyphen — a coloured em dash like the namespace column's would
  // pad the row short by the width of its escape sequence.
  snprintf(line, sizeof(line),
      "  %-16s %-20s %s%-8s" CLR_RESET " %5lu  %s",
      name,
      (method_kinds != NULL && method_kinds[0] != '\0') ? method_kinds : "-",
      state_color, bot_state_name(state),
      (unsigned long)cmd_count,
      userns_name ? userns_name : CLR_GRAY "\xe2\x80\x94" CLR_RESET);

  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_bots(const cmd_ctx_t *ctx)
{
  bot_cmd_list_state_t st = { .ctx = ctx, .count = 0 };

  cmd_reply(ctx,
      "  " CLR_BOLD "NAME             METHODS              STATE     CMDS  NAMESPACE" CLR_RESET);

  bot_iterate(show_bots_cb, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (none)");
  else
  {
    char footer[32];

    snprintf(footer, sizeof(footer), "%u bot%s",
        st.count, st.count == 1 ? "" : "s");
    cmd_reply(ctx, footer);
  }
}

// /show bot <name> [<verb> [args...]] — detailed bot status or a
// method-scoped verb registered under "show/bot" with a kind_filter
// that names a method the bot has bound.

static const cmd_arg_desc_t ad_show_bot[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                BOT_NAME_SZ - 1, NULL },
  { "rest", CMD_ARG_NONE,  CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,               NULL },
};

static const char *
help_ext_next_tok(const char *p, char *buf, size_t sz)
{
  size_t n = 0;

  while(*p == ' ' || *p == '\t') p++;
  while(*p != '\0' && *p != ' ' && *p != '\t' && n + 1 < sz)
    buf[n++] = *p++;
  buf[n] = '\0';
  while(*p == ' ' || *p == '\t') p++;
  return(p);
}

// ---- unified-tree help extender helpers --------------------------------

// Per-child callback for help_list_children_for_bot.
// Emits one line when the child is reachable and its name is
// user-addressable (non-empty, not a ":*" sentinel).
static void
help_list_iter_cb(const cmd_def_t *c, void *data)
{
  help_list_ctx_t *wp = data;
  const char *name = cmd_get_name(c);
  const char *abbr;
  const char *desc;
  char line[256];

  if(name == NULL || name[0] == '\0' || name[0] == ':')
    return;

  // With a bot in hand, list only what that bot's dispatcher would
  // actually reach: two same-named siblings may coexist when their
  // method filters are disjoint, and only one of them ever answers.
  // Without one, the listing is the whole surface and shows both.
  if(wp->inst != NULL
      && cmd_find_child_for_bot(cmd_get_parent(c), name, wp->inst) != c)
    return;

  abbr = cmd_get_abbrev(c);
  desc = cmd_get_description(c);
  snprintf(line, sizeof(line), "  %-14s %-6s %s",
      name, (abbr && abbr[0] != '\0') ? abbr : "-",
      desc ? desc : "");
  cmd_reply(wp->ctx, line);
  wp->count++;
}

// List the children of `parent` that `inst` can reach, one line each,
// via cmd_reply. A NULL `inst` lists every child -- that is the
// bot-less "/help bot" survey, not a bot with no methods. Children with
// empty or ":*" names are skipped (the default handler is not a
// user-addressable verb). Returns the number of verbs listed.
static uint32_t
help_list_children_for_bot(const cmd_ctx_t *ctx,
    const cmd_def_t *parent, const bot_inst_t *inst)
{
  help_list_ctx_t w = { ctx, inst, 0 };

  if(parent == NULL)
    return(0);

  cmd_iterate_children(parent, help_list_iter_cb, &w);

  if(w.count == 0)
    cmd_reply(ctx, "  (no verbs registered for this bot)");
  else
  {
    char line[64];

    snprintf(line, sizeof(line), "%u verb(s)", w.count);
    cmd_reply(ctx, line);
  }

  return(w.count);
}

// Emit long-form help for one verb child. Returns true on success.
static bool
help_one_verb(const cmd_ctx_t *ctx, const cmd_def_t *parent,
    const char *verb, const bot_inst_t *inst)
{
  const cmd_def_t *child;
  const char *usage;
  const char *desc;
  const char *lng;

  child = cmd_find_child_for_bot(parent, verb, inst);
  if(child == NULL)
    return(false);

  usage = cmd_get_usage(child);
  desc  = cmd_get_description(child);
  lng   = cmd_get_help_long(child);

  if(usage != NULL && usage[0] != '\0')
  {
    char line[256];

    snprintf(line, sizeof(line), "usage: %s", usage);
    cmd_reply(ctx, line);
  }

  if(desc != NULL && desc[0] != '\0')
    cmd_reply(ctx, desc);

  if(lng != NULL && lng[0] != '\0')
    cmd_reply(ctx, lng);

  return(true);
}

// /help bot <name> [<verb>] -- mutating verbs under the unified tree.
static void
help_ext_bot(const cmd_ctx_t *ctx, const char *rest)
{
  char name[BOT_NAME_SZ] = {0};
  const cmd_def_t *bot_root;
  bot_inst_t *inst;
  char verb[32] = {0};

  rest = help_ext_next_tok(rest, name, sizeof(name));

  bot_root = cmd_find("bot");
  if(name[0] == '\0')
  {
    cmd_reply(ctx, "");
    cmd_reply(ctx, "available bot <name> verbs (every bot, every method):");
    help_list_children_for_bot(ctx, bot_root, NULL);
    return;
  }

  inst = bot_find(name);
  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];

    snprintf(buf, sizeof(buf), "bot not found: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  help_ext_next_tok(rest, verb, sizeof(verb));

  if(verb[0] == '\0')
  {
    char hdr[BOT_NAME_SZ + 32];

    snprintf(hdr, sizeof(hdr), "verbs for bot %s:", name);
    cmd_reply(ctx, hdr);
    help_list_children_for_bot(ctx, bot_root, inst);
    return;
  }

  if(!help_one_verb(ctx, bot_root, verb, inst))
  {
    char buf[64];

    snprintf(buf, sizeof(buf), "unknown verb: %s", verb);
    cmd_reply(ctx, buf);
  }
}

// /help show bot <name> [<verb>] -- read verbs under the unified tree.
static void
help_ext_show_bot(const cmd_ctx_t *ctx, const char *rest)
{
  char name[BOT_NAME_SZ] = {0};
  const cmd_def_t *show_root;
  const cmd_def_t *show_bot;
  bot_inst_t *inst;
  char verb[32] = {0};

  rest = help_ext_next_tok(rest, name, sizeof(name));

  show_root = cmd_find("show");
  show_bot  = show_root != NULL ? cmd_find_child(show_root, "bot") : NULL;

  if(name[0] == '\0')
  {
    cmd_reply(ctx, "");
    cmd_reply(ctx,
        "available show bot <name> verbs (every bot, every method):");
    help_list_children_for_bot(ctx, show_bot, NULL);
    return;
  }

  inst = bot_find(name);
  if(inst == NULL)
  {
    char buf[BOT_NAME_SZ + 32];

    snprintf(buf, sizeof(buf), "bot not found: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  help_ext_next_tok(rest, verb, sizeof(verb));

  if(verb[0] == '\0')
  {
    char hdr[BOT_NAME_SZ + 32];

    snprintf(hdr, sizeof(hdr), "verbs for show bot %s:", name);
    cmd_reply(ctx, hdr);
    help_list_children_for_bot(ctx, show_bot, inst);
    return;
  }

  if(!help_one_verb(ctx, show_bot, verb, inst))
  {
    char buf[64];

    snprintf(buf, sizeof(buf), "unknown verb: %s", verb);
    cmd_reply(ctx, buf);
  }
}

static void
cmd_show_bot(const cmd_ctx_t *ctx)
{
  const char *name;
  bot_inst_t *inst;
  char line[512];

  if(ctx->parsed == NULL || ctx->parsed->argc < 1
      || ctx->parsed->argv[0][0] == '\0')
  {
    cmd_result_set(ctx, CMD_RESULT_REFUSED);
    cmd_reply(ctx, "usage: show bot <name> [<verb> [args...]]");
    return;
  }

  name = ctx->parsed->argv[0];
  inst = resolve_named_bot(ctx, name);
  if(inst == NULL)
    return;

  // Method-scoped verb dispatch against the unified command tree:
  //   /show bot <name>                -> ":default" (if any) else identity
  //   /show bot <name> <verb> [args]  -> a child under show/bot this bot
  //                                      has the methods to reach
  {
    const cmd_def_t *show_root = cmd_find("show");
    const cmd_def_t *show_bot  =
        show_root != NULL ? cmd_find_child(show_root, "bot") : NULL;

    const char *rest =
        (ctx->parsed->argc >= 2 && ctx->parsed->argv[1] != NULL)
            ? ctx->parsed->argv[1] : "";

    // Pull the first whitespace-delimited token as the verb.
    char verb[CMD_NAME_SZ] = {0};
    size_t vn = 0;

    while(*rest == ' ' || *rest == '\t') rest++;

    while(*rest != '\0' && *rest != ' ' && *rest != '\t'
        && vn + 1 < sizeof(verb))
      verb[vn++] = *rest++;

    verb[vn] = '\0';

    while(*rest == ' ' || *rest == '\t') rest++;

    if(verb[0] == '\0')
    {
      // No verb given. Try this bot's ":default" child first; fall
      // through to the identity render if none is registered.
      const cmd_def_t *dflt =
          cmd_find_child_for_bot(show_bot, ":default", inst);

      if(dflt != NULL)
      {
        cmd_ctx_t sub = *ctx;

        sub.args   = "";
        sub.parsed = NULL;
        sub.bot    = inst;
        cmd_invoke(dflt, &sub);
        return;
      }
      // Fall through to identity render.
    }

    else
    {
      const cmd_def_t *child;
      cmd_ctx_t sub;

      child = cmd_find_child_for_bot(show_bot, verb, inst);
      if(child == NULL)
      {
        char buf[CMD_NAME_SZ + BOT_NAME_SZ + 64];

        snprintf(buf, sizeof(buf),
            "unknown verb '%s' for bot %s", verb, name);
        cmd_reply(ctx, buf);
        return;
      }

      sub = *ctx;
      sub.args   = rest;
      sub.parsed = NULL;
      sub.bot    = inst;
      cmd_invoke(child, &sub);
      return;
    }
  }

  // Header: the bot, then what it can do. The bot kind comes last and in
  // parentheses because it is not this bot's identity — every bot has the
  // same one. bot_driver_name() rather than inst->driver->name: a bot
  // mid-reload has no driver at all.
  {
    char kinds[BOT_METHOD_KINDS_SZ];

    if(bot_method_kinds(inst, kinds, sizeof(kinds)) == 0)
      snprintf(kinds, sizeof(kinds), "no methods");

    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET "  %s  " CLR_GRAY "(%s bot)" CLR_RESET,
        inst->name, kinds, bot_driver_name(inst));
    cmd_reply(ctx, line);
  }

  // State.
  {
    const char *state = bot_state_name(inst->state);
    const char *color;

    switch(inst->state)
    {
      case BOT_RUNNING: color = CLR_GREEN;  break;
      case BOT_CREATED: color = CLR_YELLOW; break;
      default:          color = CLR_RED;    break;
    }

    snprintf(line, sizeof(line),
        "  " CLR_CYAN "State:" CLR_RESET "      %s%s" CLR_RESET,
        color, state);
    cmd_reply(ctx, line);
  }

  // Autostart.
  {
    uint64_t autostart;

    autostart = kv_get_bot_uint(inst->name, "autostart");

    snprintf(line, sizeof(line),
        "  " CLR_CYAN "Autostart:" CLR_RESET "  %s",
        autostart ? "yes" : "no");
    cmd_reply(ctx, line);
  }

  // User namespace.
  {
    const char *ns = inst->userns ? inst->userns->name : NULL;

    snprintf(line, sizeof(line),
        "  " CLR_CYAN "Namespace:" CLR_RESET "  %s",
        (ns && ns[0] != '\0') ? ns : CLR_GRAY "\xe2\x80\x94" CLR_RESET);
    cmd_reply(ctx, line);
  }

  // Stats.
  snprintf(line, sizeof(line),
      "  " CLR_CYAN "Messages:" CLR_RESET "   %lu",
      (unsigned long)inst->msg_count);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  " CLR_CYAN "Commands:" CLR_RESET "   %lu",
      (unsigned long)inst->cmd_count);
  cmd_reply(ctx, line);

  // Methods.
  snprintf(line, sizeof(line),
      "  " CLR_CYAN "Methods:" CLR_RESET "    %u", inst->method_count);
  cmd_reply(ctx, line);

  for(bot_method_t *m = inst->methods; m != NULL; m = m->next)
  {
    const char *mstate = CLR_GRAY "unresolved" CLR_RESET;

    if(m->inst != NULL)
    {
      method_state_t ms;
      const char *mc;
      char tmp[64];

      ms = method_get_state(m->inst);
      switch(ms)
      {
        case METHOD_AVAILABLE:    mc = CLR_GREEN;  break;
        case METHOD_RUNNING:      mc = CLR_YELLOW; break;
        default:                  mc = CLR_RED;    break;
      }

      snprintf(tmp, sizeof(tmp), "%s%s" CLR_RESET,
          mc, method_state_name(ms));
      // tmp is stack-local, copy into line directly below.
      snprintf(line, sizeof(line),
          "    %-20s " CLR_GRAY "(%s)" CLR_RESET "  %s",
          m->method_name, m->method_kind, tmp);
    }

    else
      snprintf(line, sizeof(line),
          "    %-20s " CLR_GRAY "(%s)" CLR_RESET "  %s",
          m->method_name, m->method_kind, mstate);

    cmd_reply(ctx, line);
  }

}

// Ignore filters are bot-level and configured via KV directly:
//   set kv bot.<name>.ignore_nicks alice,bob,*-bot
//   set kv bot.<name>.ignore_regex ^!
// Both apply to every method binding. No dedicated commands -- all
// configuration lives in /set kv / /show kv.

// /quit — graceful shutdown. Routes through sig_request_shutdown() so
// pool_run_parent exits the same way as on SIGTERM and main.c's
// shutdown sequence runs in order. Calling pool_shutdown() here
// instead would set pool_stopping immediately, the curl multi loop
// would exit on pool_shutting_down() before main reached
// curl_begin_shutdown(), and the drain wait would stall ~11s waiting
// on a thread that already left.
//
// The optional reason is the rest of the line, and it is the daemon's
// last word to anyone watching: every method driver is handed it as it
// takes its bot down, and IRC puts it in the QUIT so a channel reads
// why its regular just left instead of a bare "shutting down".
static const cmd_arg_desc_t ad_quit[] = {
  { "reason", CMD_ARG_NONE, CMD_ARG_OPTIONAL | CMD_ARG_REST, 0, NULL },
};

static void
admin_cmd_quit(const cmd_ctx_t *ctx)
{
  const char *reason = ctx->parsed->argc > 0 ? ctx->parsed->argv[0] : NULL;
  char        buf[SIG_REASON_SZ + 64];

  if(reason != NULL)
    snprintf(buf, sizeof(buf), "operator requested shutdown: %s", reason);
  else
    strlcpy(buf, "operator requested shutdown", sizeof(buf));

  cmd_reply(ctx, buf);
  sig_request_shutdown(reason);
}

// NL hints

// NL hint for /show bot <name>. The LLM fills the slot directly; the
// chat plugin's NL bridge forwards the slash line without translation.
static const cmd_nl_slot_t show_bot_slots[] = {
  { .name  = "name",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t show_bot_examples[] = {
  { .utterance  = "tell me about the hands bot",
    .invocation = "/show bot hands" },
  { .utterance  = "what's the status of lessclam?",
    .invocation = "/show bot lessclam" },
};

static const cmd_nl_t show_bot_nl = {
  .when          = "User asks for details or status of a specific bot.",
  .syntax        = "/show bot <name>",
  .slots         = show_bot_slots,
  .slot_count    = (uint8_t)(sizeof(show_bot_slots)
                             / sizeof(show_bot_slots[0])),
  .examples      = show_bot_examples,
  .example_count = (uint8_t)(sizeof(show_bot_examples)
                             / sizeof(show_bot_examples[0])),
};

// Registration

static const cmd_decl_t bot_decl = {
  .module      = "bot",
  .name        = "bot",
  .usage       = "bot <subcommand> ...",
  .description = "Manage bot instances",
  .help_long   = "Manages bot instances.\n"
                 "Subcommands: add del start stop addmethod delmethod",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot,
};

static const cmd_decl_t bot_add_decl = {
  .module      = "bot",
  .name        = "add",
  .usage       = "bot add <name> [<kind>]",
  .description = "Create a bot instance",
  .help_long   = "Creates a new bot instance. Give it methods with\n"
                 "/bot addmethod, then start it.\n"
                 "<kind> names the bot plugin that gives the bot a mind and\n"
                 "defaults to the only one loaded (chat); name it explicitly\n"
                 "only when more than one exists.\n"
                 "Example: bot add mybot",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot_add,
  .parent_path = "bot",
  .arg_desc    = ad_bot_name_kind,
  .arg_count   = 2,
};

static const cmd_decl_t bot_del_decl = {
  .module      = "bot",
  .name        = "del",
  .usage       = "bot del <name>",
  .description = "Destroy a bot instance",
  .help_long   = "Stops (if running) and destroys the named bot.\n"
                 "Removes the bot's KV namespace and database records.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot_del,
  .parent_path = "bot",
  .arg_desc    = ad_bot_name,
  .arg_count   = 1,
};

static const cmd_decl_t bot_start_decl = {
  .module      = "bot",
  .name        = "start",
  .usage       = "bot start <name>",
  .description = "Start a bot",
  .help_long   = "Starts the named bot. Creates method instances and\n"
                 "initiates connections. The bot must have at least one\n"
                 "method added.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot_start,
  .parent_path = "bot",
  .arg_desc    = ad_bot_name,
  .arg_count   = 1,
};

static const cmd_decl_t bot_stop_decl = {
  .module      = "bot",
  .name        = "stop",
  .usage       = "bot stop <name>",
  .description = "Stop a bot",
  .help_long   = "Stops the named bot. Disconnects method instances,\n"
                 "and drains in-flight work.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot_stop,
  .parent_path = "bot",
  .arg_desc    = ad_bot_name,
  .arg_count   = 1,
};

static const cmd_decl_t bot_addmethod_decl = {
  .module      = "bot",
  .name        = "addmethod",
  .usage       = "bot addmethod <name> <method>",
  .description = "Add a method to a bot",
  .help_long   = "Adds a method plugin to a bot. The bot must be in CREATED\n"
                 "state (not running). Configure method settings via set\n"
                 "before starting.\n"
                 "Example: bot addmethod mybot irc",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot_bind,
  .parent_path = "bot",
  .abbrev      = "am",
  .arg_desc    = ad_bot_method,
  .arg_count   = 2,
};

static const cmd_decl_t bot_delmethod_decl = {
  .module      = "bot",
  .name        = "delmethod",
  .usage       = "bot delmethod <name> <method>",
  .description = "Remove a method from a bot",
  .help_long   = "Removes a method from a bot. The bot must be in CREATED\n"
                 "state (not running).",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_bot_unbind,
  .parent_path = "bot",
  .abbrev      = "dm",
  .arg_desc    = ad_bot_method,
  .arg_count   = 2,
};

static const cmd_decl_t show_bots_decl = {
  .module      = "bot",
  .name        = "bots",
  .usage       = "show bots",
  .description = "List all bot instances",
  .help_long   =
      "Shows a colorized table of all bot instances with their bound\n"
      "method kinds, state, command count, and namespace.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_bots,
  .parent_path = "show",
};

static const cmd_decl_t show_bot_decl = {
  .module      = "bot",
  .name        = "bot",
  .usage       = "show bot <name> [<verb> [args...]]",
  .description = "Show bot details or a verb the bot's methods offer",
  .help_long   =
      "With just <name>, renders identity: state, autostart, methods,\n"
      "identities. With a trailing verb, dispatches to the first child of\n"
      "show/bot whose name matches and whose kind_filter is either empty\n"
      "or names a method this bot has bound (every bot: personas,\n"
      "memories, stats, candidates, knowledge, interests, model).",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_bot,
  .parent_path = "show",
  .arg_desc    = ad_show_bot,
  .arg_count   = (uint8_t)(sizeof(ad_show_bot)/sizeof(ad_show_bot[0])),
  .nl          = &show_bot_nl,
};

static const cmd_decl_t say_decl = {
  .module      = "bot",
  .name        = "say",
  .usage       = "say <bot> <target> <message>",
  .description = "Make a bot emit a line to a channel or nick",
  .help_long   =
      "Sends <message> through the named running bot's first bound\n"
      "method to <target> (an IRC #channel the bot has joined, or a\n"
      "nick for a DM). Used for out-of-band announcements.\n"
      "Example: say botman #cabal cp1 [mako] scored avg $/mo=812",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_say,
  .arg_desc    = ad_say,
  .arg_count   = (uint8_t)(sizeof(ad_say) / sizeof(ad_say[0])),
};

static const cmd_decl_t quit_decl = {
  .module      = "bot",
  .name        = "quit",
  .usage       = "quit [reason]",
  .description = "Graceful shutdown",
  .help_long   =
      "Initiates a graceful shutdown of BotManager. All in-flight\n"
      "work is drained, plugins are unloaded in reverse dependency\n"
      "order, and resources are released cleanly.\n"
      "\n"
      "[reason] is the rest of the line and is passed to every method\n"
      "as the bot goes down: on IRC it becomes the QUIT message the\n"
      "channel sees, in place of the default \"shutting down\".\n"
      "Example: quit rebuilding the kraken feed, back in two minutes",
  .group       = USERNS_GROUP_OWNER,
  .level       = USERNS_OWNER_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = admin_cmd_quit,
  .arg_desc    = ad_quit,
  .arg_count   = (uint8_t)(sizeof(ad_quit) / sizeof(ad_quit[0])),
};

// Register bot management commands and /quit.
void
bot_register_commands(void)
{
  // Bot management: parent command + subcommands. Every bot-scoped
  // subcommand takes an explicit <name> argument -- no session state.
  cmd_register(&bot_decl);
  cmd_register(&bot_add_decl);
  cmd_register(&bot_del_decl);
  cmd_register(&bot_start_decl);
  cmd_register(&bot_stop_decl);
  cmd_register(&bot_addmethod_decl);
  cmd_register(&bot_delmethod_decl);

  // /show bots — summary table of all bots.
  cmd_register(&show_bots_decl);

  // /show bot <name> [<verb> ...] -- detailed bot status, or a
  // method-scoped verb registered as a child of "show/bot" via
  // cmd_register with a matching kind_filter.
  cmd_register(&show_bot_decl);

  // Context-sensitive help: /help show bot <name> lists the verbs
  // registered under "show/bot" that this bot's methods admit.
  cmd_set_help_extender("show", "bot", help_ext_show_bot);

  // /help bot <name>: list method-scoped verbs registered under "bot"
  // (e.g. llm personas). Extends /bot (a root command) when /help's
  // tokens don't match a static subcommand.
  cmd_set_help_extender("bot", NULL, help_ext_bot);

  // /say <bot> <target> <message> — out-of-band announce through a
  // running bot's method (IRC channel post). Top-level command.
  cmd_register(&say_decl);
  cmd_register(&quit_decl);
}
