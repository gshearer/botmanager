// botmanager — MIT
// Built-in /bot administration commands (add, start, stop, show).
#define BOT_INTERNAL
#define BOT_CMD_INTERNAL
#include "bot.h"
#include "cmd.h"

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
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,    NULL },
  { "kind", CMD_ARG_NONE,  CMD_ARG_REQUIRED, PLUGIN_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_bot_name[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
};

static const cmd_arg_desc_t ad_bot_method[] = {
  { "name",   CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,    NULL },
  { "method", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, PLUGIN_NAME_SZ, NULL },
};

// /bot add <name> <kind>
static void
admin_cmd_bot_add(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  const char *kind = ctx->parsed->argv[1];
  const plugin_desc_t *pd;
  const bot_driver_t *drv;
  bot_inst_t *inst;
  char buf[BOT_NAME_SZ + PLUGIN_NAME_SZ + 32];

  if(bot_find(name) != NULL)
  {
    snprintf(buf, sizeof(buf), "bot already exists: %s", name);
    cmd_reply(ctx, buf);
    return;
  }

  pd = plugin_find_type(PLUGIN_BOT, kind);
  if(pd == NULL || pd->ext == NULL)
  {
    snprintf(buf, sizeof(buf), "no bot plugin with kind: %s", kind);
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

  // Persist to database.
  {
    char *e_name = db_escape(name);
    char *e_kind = db_escape(kind);

    if(e_name != NULL && e_kind != NULL)
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

      db_result_free(r);
    }

    mem_free(e_name);
    mem_free(e_kind);
  }

  // Register per-instance KV keys declared by the bot driver
  // (e.g., chat's "behavior.personality" → "bot.<name>.behavior.personality").
  bot_register_driver_kv(name, kind);

  snprintf(buf, sizeof(buf), "bot created: %s (kind: %s)", name, kind);
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
    // Remove from database (CASCADE deletes bot_methods rows).
    char *e_name = db_escape(name);
    char buf[BOT_NAME_SZ + 32];

    if(e_name != NULL)
    {
      char sql[256];
      db_result_t *r;

      snprintf(sql, sizeof(sql),
          "DELETE FROM bot_instances WHERE name = '%s'", e_name);

      r = db_result_alloc();
      if(db_query(sql, r) != SUCCESS)
        clam(CLAM_WARN, "bot_del", "DB persist failed: %s", r->error);

      db_result_free(r);
      mem_free(e_name);
    }

    snprintf(buf, sizeof(buf), "bot destroyed: %s", name);
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
    char *e_bot = db_escape(botname);
    char *e_kind = db_escape(method_kind);
    char buf[256];

    if(e_bot != NULL && e_kind != NULL)
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

      db_result_free(r);
    }

    mem_free(e_bot);
    mem_free(e_kind);

    // Register per-bot method KV keys (bot.<botname>.<kind>.*).
    bot_register_method_kv(botname, method_kind);

    snprintf(buf, sizeof(buf), "%s: added method %s (instance: %s)",
        botname, method_kind, inst_name);
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
    // Remove method binding from database.
    char *e_bot = db_escape(botname);
    char *e_kind = db_escape(method_kind);
    char buf[128];

    if(e_bot != NULL && e_kind != NULL)
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

      db_result_free(r);
    }

    mem_free(e_bot);
    mem_free(e_kind);

    snprintf(buf, sizeof(buf), "%s: removed method %s", botname, method_kind);
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

// /bot — dispatcher for subcommands
// Parent handler: usage only. Every bot-scoped subcommand takes an
// explicit <name> argument -- no session state.
static void
admin_cmd_bot(const cmd_ctx_t *ctx)
{
  // Subcommand resolution already consumed known children (add, del,
  // start, stop, addmethod, delmethod). Anything left in ctx->args is
  // a name-first invocation: /bot <name> <kind> <verb> [args...].
  const char *p = ctx->args;
  char name[BOT_NAME_SZ] = {0};
  size_t n = 0;
  bot_inst_t *inst;
  const char *kind;
  char verb[CMD_NAME_SZ] = {0};
  size_t vn = 0;
  const cmd_def_t *bot_root;
  const cmd_def_t *child;
  cmd_ctx_t sub;

  if(p == NULL) { cmd_reply(ctx, "usage: /bot <subcommand> ..."); return; }
  while(*p == ' ' || *p == '\t') p++;

  if(*p == '\0')
  {
    cmd_reply(ctx, "usage: /bot <subcommand> ...");
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

  kind = bot_driver_name(inst);

  // Pull the first whitespace-delimited token after <name> as the verb.
  while(*p != '\0' && *p != ' ' && *p != '\t' && vn + 1 < sizeof(verb))
    verb[vn++] = *p++;

  verb[vn] = '\0';

  while(*p == ' ' || *p == '\t') p++;

  if(verb[0] == '\0')
  {
    char buf[PLUGIN_NAME_SZ + BOT_NAME_SZ + 64];

    snprintf(buf, sizeof(buf),
        "usage: /bot %s <verb> [args...]", name);
    cmd_reply(ctx, buf);
    return;
  }

  bot_root = cmd_find("bot");
  child    = cmd_find_child_for_kind(bot_root, verb, kind);

  if(child == NULL)
  {
    char buf[CMD_NAME_SZ + PLUGIN_NAME_SZ + 64];

    snprintf(buf, sizeof(buf),
        "unknown verb '%s' for bot kind '%s'", verb, kind);
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
show_bots_cb(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count, uint32_t session_count,
    const char *userns_name, uint64_t cmd_count, time_t last_activity,
    void *data)
{
  bot_cmd_list_state_t *st = data;
  char line[512];
  const char *state_color;

  (void)last_activity;

  switch(state)
  {
    case BOT_RUNNING:  state_color = CLR_GREEN;  break;
    case BOT_CREATED:  state_color = CLR_YELLOW; break;
    default:           state_color = CLR_RED;    break;
  }

  snprintf(line, sizeof(line),
      "  %-16s %-10s %s%-8s" CLR_RESET " %3u methods  %3u sessions  %5lu cmds  ns=%s",
      name, driver_name,
      state_color, bot_state_name(state),
      method_count, session_count,
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
      "  " CLR_BOLD "NAME             KIND       STATE    METHODS  SESSIONS   CMDS  NAMESPACE" CLR_RESET);

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
// kind-scoped verb registered under "show/bot" via cmd_register(, NULL) with
// a kind_filter that names the bot driver kind.

static const cmd_arg_desc_t ad_show_bot[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,            BOT_NAME_SZ, NULL },
  { "rest", CMD_ARG_NONE,  CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,       NULL },
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

// State passed to the help-list iteration callback.
typedef struct
{
  const cmd_ctx_t *ctx;
  const char      *kind;      // bot kind to filter against (NULL = agnostic)
  uint32_t         count;
} help_list_ctx_t;

// Per-child callback for help_list_children_for_kind.
// Emits one line when the child's kind_filter admits ctx->kind and the
// child name is user-addressable (non-empty, not a ":*" sentinel).
static void
help_list_iter_cb(const cmd_def_t *c, void *data)
{
  help_list_ctx_t *wp = data;
  const char *name = cmd_get_name(c);
  const cmd_def_t *parent;
  const cmd_def_t *match;
  const char *abbr;
  const char *desc;
  char line[256];

  if(name == NULL || name[0] == '\0' || name[0] == ':')
    return;

  // Re-resolve through cmd_find_child_for_kind: accept this child only
  // if the kind filter matches. A distinct child may share the same
  // name under different kinds; the first-match rule in the resolver
  // is acceptable for listing since we visit each child exactly once.
  parent = cmd_get_parent(c);
  match  = cmd_find_child_for_kind(parent, name, wp->kind);

  if(match != c)
    return;

  abbr = cmd_get_abbrev(c);
  desc = cmd_get_description(c);
  snprintf(line, sizeof(line), "  %-14s %-6s %s",
      name, (abbr && abbr[0] != '\0') ? abbr : "-",
      desc ? desc : "");
  cmd_reply(wp->ctx, line);
  wp->count++;
}

// List children of `parent` whose kind_filter admits `kind`. Emits one
// line per child via cmd_reply. Children with empty or ":*" names are
// skipped (the default handler is not a user-addressable verb).
// Returns the number of verbs listed.
static uint32_t
help_list_children_for_kind(const cmd_ctx_t *ctx,
    const cmd_def_t *parent, const char *kind)
{
  help_list_ctx_t w = { ctx, kind, 0 };

  if(parent == NULL)
    return(0);

  cmd_iterate_children(parent, help_list_iter_cb, &w);

  if(w.count == 0)
    cmd_reply(ctx, "  (no verbs registered for this bot kind)");
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
    const char *verb, const char *kind)
{
  const cmd_def_t *child;
  const char *usage;
  const char *desc;
  const char *lng;

  child = cmd_find_child_for_kind(parent, verb, kind);
  if(child == NULL)
    return(false);

  usage = cmd_get_usage(child);
  desc  = cmd_get_description(child);
  lng   = cmd_get_help_long(child);

  if(usage != NULL && usage[0] != '\0')
  {
    char line[256];

    snprintf(line, sizeof(line), "usage: /%s", usage);
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
  const char *kind;
  char verb[32] = {0};

  rest = help_ext_next_tok(rest, name, sizeof(name));

  bot_root = cmd_find("bot");
  if(name[0] == '\0')
  {
    cmd_reply(ctx, "");
    cmd_reply(ctx, "available /bot <name> verbs (kind-agnostic + all kinds):");
    help_list_children_for_kind(ctx, bot_root, NULL);
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

  kind = bot_driver_name(inst);
  help_ext_next_tok(rest, verb, sizeof(verb));

  if(verb[0] == '\0')
  {
    char hdr[BOT_NAME_SZ + PLUGIN_NAME_SZ + 64];

    snprintf(hdr, sizeof(hdr), "verbs for /bot %s (%s):", name, kind);
    cmd_reply(ctx, hdr);
    help_list_children_for_kind(ctx, bot_root, kind);
    return;
  }

  if(!help_one_verb(ctx, bot_root, verb, kind))
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
  const char *kind;
  char verb[32] = {0};

  rest = help_ext_next_tok(rest, name, sizeof(name));

  show_root = cmd_find("show");
  show_bot  = show_root != NULL ? cmd_find_child(show_root, "bot") : NULL;

  if(name[0] == '\0')
  {
    cmd_reply(ctx, "");
    cmd_reply(ctx, "available /show bot <name> verbs (kind-agnostic + all kinds):");
    help_list_children_for_kind(ctx, show_bot, NULL);
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

  kind = bot_driver_name(inst);
  help_ext_next_tok(rest, verb, sizeof(verb));

  if(verb[0] == '\0')
  {
    char hdr[BOT_NAME_SZ + PLUGIN_NAME_SZ + 64];

    snprintf(hdr, sizeof(hdr),
        "verbs for /show bot %s (%s):", name, kind);
    cmd_reply(ctx, hdr);
    help_list_children_for_kind(ctx, show_bot, kind);
    return;
  }

  if(!help_one_verb(ctx, show_bot, verb, kind))
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
    cmd_reply(ctx, "usage: /show bot <name> [<verb> [args...]]");
    return;
  }

  name = ctx->parsed->argv[0];
  inst = resolve_named_bot(ctx, name);
  if(inst == NULL)
    return;

  // Kind-scoped verb dispatch against the unified command tree:
  //   /show bot <name>                -> ":default" (if any) else identity
  //   /show bot <name> <verb> [args]  -> kind-filtered child under show/bot
  {
    const char *kind = bot_driver_name(inst);
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
      // No verb given. Try the per-kind ":default" child first; fall
      // through to the identity render if none is registered.
      const cmd_def_t *dflt =
          cmd_find_child_for_kind(show_bot, ":default", kind);

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

      child = cmd_find_child_for_kind(show_bot, verb, kind);
      if(child == NULL)
      {
        char buf[CMD_NAME_SZ + PLUGIN_NAME_SZ + 64];

        snprintf(buf, sizeof(buf),
            "unknown verb '%s' for bot kind '%s'", verb, kind);
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

  // Header.
  snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET " " CLR_GRAY "(%s)" CLR_RESET,
      inst->name, inst->driver->name);
  cmd_reply(ctx, line);

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
    char key[KV_KEY_SZ];
    uint64_t autostart;

    snprintf(key, sizeof(key), "bot.%s.autostart", inst->name);
    autostart = kv_get_uint(key);

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

  // Sessions.
  snprintf(line, sizeof(line),
      "  " CLR_CYAN "Sessions:" CLR_RESET "   %u", inst->session_count);
  cmd_reply(ctx, line);

  if(inst->session_count > 0)
  {
    for(bot_session_t *s = inst->sessions; s != NULL; s = s->next)
    {
      char age[32] = "n/a";

      if(s->last_seen > 0)
      {
        time_t elapsed;

        elapsed = time(NULL) - s->last_seen;
        if(elapsed < 60)
          snprintf(age, sizeof(age), "%lds ago", (long)elapsed);
        else if(elapsed < 3600)
          snprintf(age, sizeof(age), "%ldm ago", (long)(elapsed / 60));
        else
          snprintf(age, sizeof(age), "%ldh ago", (long)(elapsed / 3600));
      }

      {
      const char *mname = s->method ? method_inst_name(s->method) : "?";

      snprintf(line, sizeof(line),
          "    %-20s on %-16s  last seen: %s",
          s->username, mname, age);
      cmd_reply(ctx, line);
      }
    }
  }
}

// Ignore filters are bot-level and configured via KV directly:
//   set kv bot.<name>.ignore_nicks alice,bob,*-bot
//   set kv bot.<name>.ignore_regex ^!
// Both apply to every method binding. No dedicated commands -- all
// configuration lives in /set kv / /show kv.

// /quit — graceful shutdown
static void
admin_cmd_quit(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "operator requested shutdown");
  pool_shutdown();
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

// Register bot management commands and /quit.
void
bot_register_commands(void)
{
  // Bot management: parent command + subcommands. Every bot-scoped
  // subcommand takes an explicit <name> argument -- no session state.
  cmd_register("bot", "bot",
      "bot <subcommand> ...",
      "Manage bot instances",
      "Manages bot instances.\n"
      "Subcommands: add del start stop addmethod delmethod",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  cmd_register("bot", "add",
      "bot add <name> <kind>",
      "Create a bot instance",
      "Creates a new bot instance with the given name and kind.\n"
      "The kind must match a loaded bot plugin (e.g., command).\n"
      "Example: /bot add mybot command",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_add, NULL, "bot", NULL,
      ad_bot_name_kind, 2, NULL, NULL);

  cmd_register("bot", "del",
      "bot del <name>",
      "Destroy a bot instance",
      "Stops (if running) and destroys the named bot.\n"
      "Removes the bot's KV namespace and database records.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_del, NULL, "bot", NULL,
      ad_bot_name, 1, NULL, NULL);

  cmd_register("bot", "start",
      "bot start <name>",
      "Start a bot",
      "Starts the named bot. Creates method instances and\n"
      "initiates connections. The bot must have at least one\n"
      "method added.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_start, NULL, "bot", NULL,
      ad_bot_name, 1, NULL, NULL);

  cmd_register("bot", "stop",
      "bot stop <name>",
      "Stop a bot",
      "Stops the named bot. Disconnects method instances,\n"
      "clears sessions, and drains in-flight work.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_stop, NULL, "bot", NULL,
      ad_bot_name, 1, NULL, NULL);

  cmd_register("bot", "addmethod",
      "bot addmethod <name> <method>",
      "Add a method to a bot",
      "Adds a method plugin to a bot. The bot must be in CREATED\n"
      "state (not running). Configure method settings via /set\n"
      "before starting.\n"
      "Example: /bot addmethod mybot irc",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_bind, NULL, "bot", "am",
      ad_bot_method, 2, NULL, NULL);

  cmd_register("bot", "delmethod",
      "bot delmethod <name> <method>",
      "Remove a method from a bot",
      "Removes a method from a bot. The bot must be in CREATED\n"
      "state (not running).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_bot_unbind, NULL, "bot", "dm",
      ad_bot_method, 2, NULL, NULL);

  // /show bots — summary table of all bots.
  cmd_register("bot", "bots",
      "show bots",
      "List all bot instances",
      "Shows a colorized table of all bot instances with state,\n"
      "method count, session count, commands, and namespace.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_show_bots, NULL, "show", NULL,
      NULL, 0, NULL, NULL);

  // /show bot <name> [<verb> ...] -- detailed bot status, or a
  // kind-scoped verb registered as a child of "show/bot" via
  // cmd_register(, NULL) with a matching kind_filter.
  cmd_register("bot", "bot",
      "show bot <name> [<verb> [args...]]",
      "Show bot details or a kind-specific verb",
      "With just <name>, renders identity: state, autostart, methods,\n"
      "sessions. With a trailing verb, dispatches to the first child of\n"
      "show/bot whose name matches and whose kind_filter admits the\n"
      "bot's driver kind (llm: personas, memories, stats, candidates,\n"
      "knowledge, interests).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY, cmd_show_bot, NULL, "show", NULL,
      ad_show_bot, (uint8_t)(sizeof(ad_show_bot)/sizeof(ad_show_bot[0])), NULL, &show_bot_nl);

  // Context-sensitive help: /help show bot <name> lists kind-scoped
  // verbs registered under "show/bot" with a matching kind_filter.
  cmd_set_help_extender("show", "bot", help_ext_show_bot);

  // /help bot <name>: list kind-scoped verbs registered under "bot"
  // (e.g. llm personas). Extends /bot (a root command) when /help's
  // tokens don't match a static subcommand.
  cmd_set_help_extender("bot", NULL, help_ext_bot);

  cmd_register("bot", "quit", "quit",
      "Graceful shutdown",
      "Initiates a graceful shutdown of BotManager. All in-flight\n"
      "work is drained, plugins are unloaded in reverse dependency\n"
      "order, and resources are released cleanly.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY, admin_cmd_quit, NULL,
      NULL, NULL, NULL, 0, NULL, NULL);
}
