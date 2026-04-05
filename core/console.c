#define CONSOLE_INTERNAL
#include "console.h"

#include <unistd.h>

// -----------------------------------------------------------------------
// KV configuration
// -----------------------------------------------------------------------

// Resolve the history file path from KV, expanding ~ to $HOME.
static void
con_resolve_history_path(void)
{
  const char *raw = kv_get_str("core.console.history.file");

  if(raw == NULL || raw[0] == '\0')
    raw = "~/.config/botmanager/history";

  if(raw[0] == '~' && raw[1] == '/')
  {
    const char *home = getenv("HOME");

    if(home != NULL)
    {
      snprintf(con_history_path, sizeof(con_history_path),
          "%s%s", home, raw + 1);
      return;
    }
  }

  strncpy(con_history_path, raw, sizeof(con_history_path) - 1);
  con_history_path[sizeof(con_history_path) - 1] = '\0';
}

// Load cached config values from KV.
static void
con_load_config(void)
{
  con_history_size = (uint32_t)kv_get_uint("core.console.history.size");

  if(con_history_size == 0)
    con_history_size = 1000;

  con_resolve_history_path();

  const char *fmt = kv_get_str("core.console.prompt.format");

  if(fmt != NULL && fmt[0] != '\0')
  {
    strncpy(con_prompt_fmt, fmt, sizeof(con_prompt_fmt) - 1);
    con_prompt_fmt[sizeof(con_prompt_fmt) - 1] = '\0';
  }

  con_prompt_color = (uint8_t)kv_get_uint("core.console.prompt.color");
}

// KV change callback for console settings.
// key: changed KV key
// data: unused
static void
con_kv_changed(const char *key, void *data)
{
  (void)data;
  (void)key;

  con_load_config();
  stifle_history((int)con_history_size);
}

// -----------------------------------------------------------------------
// Argument descriptors
// -----------------------------------------------------------------------

static const cmd_arg_desc_t con_ad_attach[] = {
  { "botname", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ, NULL },
};

static const cmd_arg_desc_t con_ad_associate[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ, NULL },
};

// -----------------------------------------------------------------------
// Console subcommand callbacks
// -----------------------------------------------------------------------

// Parent handler: /console — list available subcommands.
static void
con_cmd_console(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "Console subcommands:");
  cmd_reply(ctx, "  /console attach <botname>  (at)  — attach to a bot");
  cmd_reply(ctx, "  /console unattach          (uat) — detach from bot");
  cmd_reply(ctx, "  /console associate <user>  (as)  — set user identity");
  cmd_reply(ctx, "  /console unassociate       (uas) — reset to @owner");

  if(con_attached_bot != NULL)
  {
    char buf[256];
    snprintf(buf, sizeof(buf), "Attached: %s  User: %s",
        bot_inst_name(con_attached_bot), con_associated_user);
    cmd_reply(ctx, buf);
  }
  else
  {
    cmd_reply(ctx, "Not attached to any bot.");
  }
}

// /console attach <botname>
static void
con_cmd_attach(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];

  bot_inst_t *bot = bot_find(botname);

  if(bot == NULL)
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "Bot '%s' not found.", botname);
    cmd_reply(ctx, buf);
    return;
  }

  if(bot_get_state(bot) != BOT_RUNNING)
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "Bot '%s' is not running.", botname);
    cmd_reply(ctx, buf);
    return;
  }

  con_attached_bot = bot;
  strncpy(con_associated_user, USERNS_OWNER_USER,
      sizeof(con_associated_user) - 1);

  // Ensure per-method prefix KV key exists for console on this bot.
  char pfx_key[KV_KEY_SZ];

  snprintf(pfx_key, sizeof(pfx_key),
      "bot.%s.console.prefix", botname);

  if(!kv_exists(pfx_key))
    kv_register(pfx_key, KV_STR, "/", NULL, NULL);

  char buf[128];
  snprintf(buf, sizeof(buf), "Attached to bot '%s'.", botname);
  cmd_reply(ctx, buf);
}

// /console unattach
static void
con_cmd_unattach(const cmd_ctx_t *ctx)
{
  if(con_attached_bot == NULL)
  {
    cmd_reply(ctx, "Not attached to any bot.");
    return;
  }

  const char *name = bot_inst_name(con_attached_bot);
  char buf[128];
  snprintf(buf, sizeof(buf), "Detached from bot '%s'.", name);

  con_attached_bot = NULL;
  strncpy(con_associated_user, USERNS_OWNER_USER,
      sizeof(con_associated_user) - 1);

  cmd_reply(ctx, buf);
}

// /console associate <username>
static void
con_cmd_associate(const cmd_ctx_t *ctx)
{
  if(con_attached_bot == NULL)
  {
    cmd_reply(ctx, "Not attached to any bot. Use /console attach first.");
    return;
  }

  const char *username = ctx->parsed->argv[0];

  // Validate the user exists in the bot's namespace.
  userns_t *ns = bot_get_userns(con_attached_bot);

  if(ns != NULL && !userns_is_owner(username))
  {
    if(!userns_user_exists(ns, username))
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "User '%s' not found in namespace '%s'.",
          username, ns->name);
      cmd_reply(ctx, buf);
      return;
    }
  }

  strncpy(con_associated_user, username, sizeof(con_associated_user) - 1);
  con_associated_user[sizeof(con_associated_user) - 1] = '\0';

  char buf[128];
  snprintf(buf, sizeof(buf), "Associated as user '%s'.", con_associated_user);
  cmd_reply(ctx, buf);
}

// /console unassociate
static void
con_cmd_unassociate(const cmd_ctx_t *ctx)
{
  if(con_attached_bot == NULL)
  {
    cmd_reply(ctx, "Not attached to any bot. Use /console attach first.");
    return;
  }

  strncpy(con_associated_user, USERNS_OWNER_USER,
      sizeof(con_associated_user) - 1);

  cmd_reply(ctx, "Reset to @owner.");
}

// -----------------------------------------------------------------------
// History command callbacks
// -----------------------------------------------------------------------

static const cmd_arg_desc_t con_ad_history_list[] = {
  { "count", CMD_ARG_DIGITS, CMD_ARG_OPTIONAL, 5, NULL },
};

static const cmd_arg_desc_t con_ad_history_search[] = {
  { "pattern", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

// Parent handler: /history — list available subcommands.
static void
con_cmd_history(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "History subcommands:");
  cmd_reply(ctx, "  /history list [count]       (ls) — show recent entries");
  cmd_reply(ctx, "  /history clear              (cl) — clear all history");
  cmd_reply(ctx, "  /history search <pattern>   (s)  — search history");
}

// /history list [count] — display the last N history entries.
static void
con_cmd_history_list(const cmd_ctx_t *ctx)
{
  int count = 20;

  if(ctx->parsed->argc > 0)
  {
    count = atoi(ctx->parsed->argv[0]);

    if(count <= 0)
      count = 20;
  }

  HISTORY_STATE *hs = history_get_history_state();

  if(hs == NULL || hs->length == 0)
  {
    cmd_reply(ctx, "History is empty.");
    free(hs);
    return;
  }

  int start = hs->length - count;

  if(start < 0)
    start = 0;

  char buf[512];

  for(int i = start; i < hs->length; i++)
  {
    HIST_ENTRY *e = history_get(i + history_base);

    if(e != NULL)
    {
      snprintf(buf, sizeof(buf), "  %4d  %s", i + history_base, e->line);
      cmd_reply(ctx, buf);
    }
  }

  snprintf(buf, sizeof(buf), "%d entries shown (of %d total).",
      hs->length - start, hs->length);
  cmd_reply(ctx, buf);

  free(hs);
}

// /history clear — clear all history and remove the history file.
static void
con_cmd_history_clear(const cmd_ctx_t *ctx)
{
  clear_history();

  if(con_history_path[0] != '\0')
    unlink(con_history_path);

  cmd_reply(ctx, "History cleared.");
}

// /history search <pattern> — search history for entries containing
// the given substring.
static void
con_cmd_history_search(const cmd_ctx_t *ctx)
{
  const char *pattern = ctx->parsed->argv[0];

  HISTORY_STATE *hs = history_get_history_state();

  if(hs == NULL || hs->length == 0)
  {
    cmd_reply(ctx, "History is empty.");
    free(hs);
    return;
  }

  char buf[512];
  int  found = 0;

  for(int i = 0; i < hs->length; i++)
  {
    HIST_ENTRY *e = history_get(i + history_base);

    if(e != NULL && strstr(e->line, pattern) != NULL)
    {
      snprintf(buf, sizeof(buf), "  %4d  %s", i + history_base, e->line);
      cmd_reply(ctx, buf);
      found++;
    }
  }

  if(found == 0)
    cmd_reply(ctx, "No matches found.");
  else
  {
    snprintf(buf, sizeof(buf), "%d match%s found.",
        found, found == 1 ? "" : "es");
    cmd_reply(ctx, buf);
  }

  free(hs);
}

// Dispatch a console input line to the unified command system.
// Tries system commands first, then per-bot dispatch if attached.
// line: raw input line from stdin (modified in place)
static void
con_dispatch(char *line)
{
  // Strip leading whitespace.
  while(*line == ' ' || *line == '\t')
    line++;

  // Strip trailing whitespace/newline.
  size_t len = strlen(line);

  while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
        || line[len - 1] == ' ' || line[len - 1] == '\t'))
    line[--len] = '\0';

  if(len == 0)
    return;

  // Commands must start with '/'.
  if(line[0] != '/')
  {
    clam(CLAM_WARN, "console", "unknown input (try /help)");
    return;
  }

  // Skip the '/'.
  line++;

  // Split command name from arguments.
  char *args = line;

  while(*args != '\0' && *args != ' ' && *args != '\t')
    args++;

  if(*args != '\0')
  {
    *args = '\0';
    args++;

    while(*args == ' ' || *args == '\t')
      args++;
  }

  // Try system commands first — they take priority on the console
  // so that /help, /show, /set etc. always reach the system handlers
  // even when attached to a bot with a same-named built-in.
  if(cmd_dispatch_system(line, args) == SUCCESS)
    return;

  // When attached to a bot, try per-bot dispatch (built-in + enabled
  // commands). The console_origin flag bypasses all permission checks.
  if(con_attached_bot != NULL)
  {
    // Resolve console's per-method prefix for the attached bot.
    const char *prefix = NULL;
    char pfx_key[KV_KEY_SZ];

    snprintf(pfx_key, sizeof(pfx_key),
        "bot.%s.console.prefix",
        bot_inst_name(con_attached_bot));

    const char *kv_pfx = kv_get_str(pfx_key);

    if(kv_pfx != NULL && kv_pfx[0] != '\0')
      prefix = kv_pfx;
    else
      prefix = cmd_get_prefix(con_attached_bot);

    method_msg_t msg;

    memset(&msg, 0, sizeof(msg));
    msg.inst = con_method_inst;
    msg.timestamp = time(NULL);
    msg.console_origin = true;
    snprintf(msg.sender, METHOD_SENDER_SZ, "%s", con_associated_user);

    // Reconstruct the text as <prefix><command> <args>.
    if(args[0] != '\0')
      snprintf(msg.text, METHOD_TEXT_SZ, "%s%s %s", prefix, line, args);
    else
      snprintf(msg.text, METHOD_TEXT_SZ, "%s%s", prefix, line);

    if(cmd_dispatch(con_attached_bot, &msg) == SUCCESS)
      return;
  }

  clam(CLAM_WARN, "console", "unknown command: /%s (try /help)", line);
}

// -----------------------------------------------------------------------
// Readline tab completion engine.
// -----------------------------------------------------------------------

// Reset completion state and free any collected matches.
static void
con_comp_reset(void)
{
  if(con_comp.matches != NULL)
  {
    for(int i = 0; i < con_comp.count; i++)
      free(con_comp.matches[i]);

    free(con_comp.matches);
  }

  con_comp.matches = NULL;
  con_comp.count   = 0;
  con_comp.index   = 0;
}

// Add a match string to the completion state. The string is strdup'd.
// str: match string to add
static void
con_comp_add(const char *str)
{
  if(str == NULL || con_comp.count >= CON_COMP_MAX)
    return;

  if(con_comp.matches == NULL)
  {
    con_comp.matches = malloc(CON_COMP_MAX * sizeof(char *));

    if(con_comp.matches == NULL)
      return;
  }

  con_comp.matches[con_comp.count] = strdup(str);

  if(con_comp.matches[con_comp.count] != NULL)
    con_comp.count++;
}

// Generator function for rl_completion_matches(). Returns the next
// match from the pre-collected con_comp list, or NULL when exhausted.
// text: prefix text (unused — matches are pre-filtered)
// state: 0 on first call, incremented on subsequent calls
// returns: malloc'd match string, or NULL
static char *
con_comp_generator(const char *text, int state)
{
  (void)text;

  if(state == 0)
    con_comp.index = 0;

  if(con_comp.index < con_comp.count)
    return(strdup(con_comp.matches[con_comp.index++]));

  return(NULL);
}

// Callback: collect system command names matching a prefix.
typedef struct
{
  const char *prefix;
  size_t      prefix_len;
  bool        with_slash;    // prepend / to matches
} con_cmd_match_t;

// Iteration callback for system command matching.
// def: command definition
// data: con_cmd_match_t *
static void
con_comp_cmd_cb(const cmd_def_t *def, void *data)
{
  con_cmd_match_t *m = data;
  const char *name   = cmd_get_name(def);
  const char *abbrev = cmd_get_abbrev(def);

  // Only complete commands visible on METHOD_T_CONSOLE.
  method_type_t methods = cmd_get_methods(def);

  if(methods != METHOD_T_ANY && !(methods & METHOD_T_CONSOLE))
    return;

  if(name != NULL && strncasecmp(name, m->prefix, m->prefix_len) == 0)
  {
    if(m->with_slash)
    {
      char buf[CMD_NAME_SZ + 2];
      snprintf(buf, sizeof(buf), "/%s", name);
      con_comp_add(buf);
    }
    else
      con_comp_add(name);
  }

  if(abbrev != NULL && abbrev[0] != '\0'
      && strncasecmp(abbrev, m->prefix, m->prefix_len) == 0)
  {
    if(m->with_slash)
    {
      char buf[CMD_NAME_SZ + 2];
      snprintf(buf, sizeof(buf), "/%s", abbrev);
      con_comp_add(buf);
    }
    else
      con_comp_add(abbrev);
  }
}

// Collect top-level system command names matching a prefix.
// text: prefix to match (with or without leading /)
static void
con_complete_command(const char *text)
{
  con_cmd_match_t m;

  // Strip leading / for matching.
  if(text[0] == '/')
  {
    m.prefix     = text + 1;
    m.with_slash = true;
  }
  else
  {
    m.prefix     = text;
    m.with_slash = true;   // always return /name for system commands
  }

  m.prefix_len = strlen(m.prefix);

  cmd_iterate_system(con_comp_cmd_cb, &m);

  // When attached, also include bot-level commands.
  if(con_attached_bot != NULL)
  {
    con_cmd_match_t bm;
    bm.prefix     = m.prefix;
    bm.prefix_len = m.prefix_len;
    bm.with_slash = true;
    cmd_iterate_bot(con_attached_bot, con_comp_cmd_cb, &bm);
  }
}

// Collect subcommand names matching a prefix under a parent command.
// text: prefix to match
// parent: parent command definition
static void
con_complete_subcommand(const char *text, const cmd_def_t *parent)
{
  con_cmd_match_t m;
  m.prefix     = text;
  m.prefix_len = strlen(text);
  m.with_slash = false;

  cmd_iterate_children(parent, con_comp_cmd_cb, &m);
}

// Iteration callback for bot name completion.
// Collects bot names matching a prefix.
typedef struct
{
  const char *prefix;
  size_t      prefix_len;
  bool        running_only;   // filter to BOT_RUNNING state
} con_bot_match_t;

// bot_iterate callback for collecting matching bot names.
static void
con_comp_bot_cb(const char *name, const char *driver_name,
    bot_state_t state, uint32_t method_count,
    uint32_t session_count, const char *userns_name, void *data)
{
  (void)driver_name;
  (void)method_count;
  (void)session_count;
  (void)userns_name;

  con_bot_match_t *m = data;

  if(m->running_only && state != BOT_RUNNING)
    return;

  if(strncasecmp(name, m->prefix, m->prefix_len) == 0)
    con_comp_add(name);
}

// KV prefix completion callback.
typedef struct
{
  const char *prefix;
  size_t      prefix_len;
  bool        suppress_space;   // set if any match is a partial prefix
} con_kv_match_t;

// kv_iterate_prefix callback for collecting matching KV keys.
static void
con_comp_kv_cb(const char *key, kv_type_t type, const char *value_str,
    void *data)
{
  (void)type;
  (void)value_str;

  con_kv_match_t *m = data;

  if(strncasecmp(key, m->prefix, m->prefix_len) == 0)
    con_comp_add(key);
}

// Namespace iteration callback for name completion.
static void
con_comp_ns_cb(const char *name, void *data)
{
  con_cmd_match_t *m = data;

  if(strncasecmp(name, m->prefix, m->prefix_len) == 0)
    con_comp_add(name);
}

// User iteration callback for username completion.
static void
con_comp_user_cb(const char *username, const char *uuid,
    const char *description, void *data)
{
  (void)uuid;
  (void)description;

  con_cmd_match_t *m = data;

  if(strncasecmp(username, m->prefix, m->prefix_len) == 0)
    con_comp_add(username);
}

// Group iteration callback for group name completion.
static void
con_comp_group_cb(const char *name, const char *description, void *data)
{
  (void)description;

  con_cmd_match_t *m = data;

  if(strncasecmp(name, m->prefix, m->prefix_len) == 0)
    con_comp_add(name);
}

// Plugin iteration callback for kind completion.
// Filters by plugin type stored in the match struct's with_slash field
// (repurposed as type filter: with_slash=false means PLUGIN_METHOD,
// but we use a separate type field instead).
typedef struct
{
  const char    *prefix;
  size_t         prefix_len;
  plugin_type_t  type;
} con_plugin_match_t;

// plugin_iterate callback for collecting matching plugin kinds.
static void
con_comp_plugin_cb(const char *name, const char *version,
    const char *path, plugin_type_t type, const char *kind,
    plugin_state_t state, void *data)
{
  (void)name;
  (void)version;
  (void)path;
  (void)state;

  con_plugin_match_t *m = data;

  if(type != m->type)
    return;

  if(kind != NULL && strncasecmp(kind, m->prefix, m->prefix_len) == 0)
    con_comp_add(kind);
}

// Context-aware argument completion for specific commands.
// text: prefix to match
// cmd_name: resolved command name (after stripping /)
// cmd: command definition (may be NULL)
// arg_pos: 0-based argument position being completed
static void
con_complete_args(const char *text, const char *cmd_name,
    const cmd_def_t *cmd, int arg_pos)
{
  (void)cmd;

  size_t text_len = strlen(text);

  // /console attach <botname> — running bots only
  if(strcasecmp(cmd_name, "attach") == 0 && arg_pos == 0)
  {
    con_bot_match_t bm = { text, text_len, true };
    bot_iterate(con_comp_bot_cb, &bm);
    return;
  }

  // /console associate <username> — users from attached bot's namespace
  if(strcasecmp(cmd_name, "associate") == 0 && arg_pos == 0)
  {
    if(con_attached_bot != NULL)
    {
      const userns_t *ns = bot_get_userns(con_attached_bot);

      if(ns != NULL)
      {
        con_cmd_match_t m = { text, text_len, false };
        userns_user_iterate(ns, con_comp_user_cb, &m);
      }
    }

    return;
  }

  // /bot start|stop|del <botname> — all bots
  if((strcasecmp(cmd_name, "start") == 0
        || strcasecmp(cmd_name, "stop") == 0
        || strcasecmp(cmd_name, "del") == 0) && arg_pos == 0)
  {
    con_bot_match_t bm = { text, text_len, false };
    bot_iterate(con_comp_bot_cb, &bm);
    return;
  }

  // /bot bind <botname> <method_kind>
  // /bot unbind <botname> <method_kind>
  if((strcasecmp(cmd_name, "bind") == 0
        || strcasecmp(cmd_name, "unbind") == 0))
  {
    if(arg_pos == 0)
    {
      con_bot_match_t bm = { text, text_len, false };
      bot_iterate(con_comp_bot_cb, &bm);
      return;
    }

    if(arg_pos == 1)
    {
      con_plugin_match_t pm = { text, text_len, PLUGIN_METHOD };
      plugin_iterate(con_comp_plugin_cb, &pm);
      return;
    }

    return;
  }

  // /bot userns <botname> <namespace>
  if(strcasecmp(cmd_name, "userns") == 0)
  {
    if(arg_pos == 0)
    {
      con_bot_match_t bm = { text, text_len, false };
      bot_iterate(con_comp_bot_cb, &bm);
      return;
    }

    if(arg_pos == 1)
    {
      con_cmd_match_t m = { text, text_len, false };
      userns_iterate(con_comp_ns_cb, &m);
      return;
    }

    return;
  }

  // /set <key> — complete KV keys
  if(strcasecmp(cmd_name, "set") == 0 && arg_pos == 0)
  {
    con_kv_match_t km = { text, text_len, false };
    kv_iterate_prefix("", con_comp_kv_cb, &km);
    return;
  }

  // /show kv [prefix] — complete KV key prefixes
  if(strcasecmp(cmd_name, "kv") == 0 && arg_pos == 0)
  {
    con_kv_match_t km = { text, text_len, false };
    kv_iterate_prefix("", con_comp_kv_cb, &km);
    return;
  }

  // /grant <ns> <user> <group> [level]
  // /revoke <ns> <user> <group>
  if(strcasecmp(cmd_name, "grant") == 0
      || strcasecmp(cmd_name, "revoke") == 0)
  {
    if(arg_pos == 0)
    {
      con_cmd_match_t m = { text, text_len, false };
      userns_iterate(con_comp_ns_cb, &m);
      return;
    }

    // Parse namespace from rl_line_buffer to complete user/group.
    // Tokenize backward from the current command to find the ns arg.
    char lbuf[METHOD_TEXT_SZ];
    snprintf(lbuf, sizeof(lbuf), "%s", rl_line_buffer);

    char *toks[8];
    int   tcnt = 0;
    char *tp   = lbuf;

    while(*tp != '\0' && tcnt < 8)
    {
      while(*tp == ' ' || *tp == '\t')
        tp++;

      if(*tp == '\0')
        break;

      toks[tcnt++] = tp;

      while(*tp != '\0' && *tp != ' ' && *tp != '\t')
        tp++;

      if(*tp != '\0')
        *tp++ = '\0';
    }

    // toks[0] = "/grant", toks[1] = ns, toks[2] = user, toks[3] = group
    if(tcnt >= 2)
    {
      const userns_t *ns = userns_find(toks[1]);

      if(ns != NULL)
      {
        if(arg_pos == 1)
        {
          con_cmd_match_t m = { text, text_len, false };
          userns_user_iterate(ns, con_comp_user_cb, &m);
          return;
        }

        if(arg_pos == 2)
        {
          con_cmd_match_t m = { text, text_len, false };
          userns_group_iterate(ns, con_comp_group_cb, &m);
          return;
        }
      }
    }

    return;
  }

  // /useradd <ns> ... / /userdel <ns> ... / /userlist <ns> ...
  // /userinfo <ns> ... / /passwd <ns> ... / /groupadd <ns> ...
  // /groupdel <ns> ... / /grouplist <ns> ...
  if((strcasecmp(cmd_name, "useradd") == 0
        || strcasecmp(cmd_name, "userdel") == 0
        || strcasecmp(cmd_name, "userlist") == 0
        || strcasecmp(cmd_name, "userinfo") == 0
        || strcasecmp(cmd_name, "passwd") == 0
        || strcasecmp(cmd_name, "groupadd") == 0
        || strcasecmp(cmd_name, "groupdel") == 0
        || strcasecmp(cmd_name, "grouplist") == 0) && arg_pos == 0)
  {
    con_cmd_match_t m = { text, text_len, false };
    userns_iterate(con_comp_ns_cb, &m);
    return;
  }

  // /mfa add|del|list <ns> <user> ...
  if((strcasecmp(cmd_name, "add") == 0
        || strcasecmp(cmd_name, "del") == 0
        || strcasecmp(cmd_name, "list") == 0))
  {
    // Checking if parent is "mfa" to avoid ambiguity.
    if(cmd != NULL)
    {
      const cmd_def_t *p = cmd_get_parent(cmd);

      if(p != NULL && strcasecmp(cmd_get_name(p), "mfa") == 0
          && arg_pos == 0)
      {
        con_cmd_match_t m = { text, text_len, false };
        userns_iterate(con_comp_ns_cb, &m);
        return;
      }
    }
  }

  // /bot add <name> <kind> — kind = bot plugin kinds
  if(strcasecmp(cmd_name, "add") == 0 && arg_pos == 1)
  {
    if(cmd != NULL)
    {
      const cmd_def_t *p = cmd_get_parent(cmd);

      if(p != NULL && strcasecmp(cmd_get_name(p), "bot") == 0)
      {
        con_plugin_match_t pm = { text, text_len, PLUGIN_BOT };
        plugin_iterate(con_comp_plugin_cb, &pm);
        return;
      }
    }
  }
}

// Parse tokens from the line buffer preceding the cursor to determine
// what context we are completing in.
// text: the word being completed
// start: cursor position where text begins in rl_line_buffer
// end: cursor position where text ends
// returns: match array, or NULL for no matches
static char **
con_completion(const char *text, int start, int end)
{
  (void)end;

  // Suppress filename fallback and reset append behavior.
  rl_attempted_completion_over = 1;
  rl_completion_suppress_append = 0;

  con_comp_reset();

  // Determine the effective start (skip leading whitespace).
  int eff_start = 0;

  while(rl_line_buffer[eff_start] == ' '
      || rl_line_buffer[eff_start] == '\t')
    eff_start++;

  // If completing at the start of the line (or only whitespace before
  // cursor), complete command names.
  if(start <= eff_start)
  {
    con_complete_command(text);

    if(con_comp.count == 0)
      return(NULL);

    return(rl_completion_matches(text, con_comp_generator));
  }

  // Parse the preceding tokens to determine command context.
  // Tokenize rl_line_buffer[0..start-1] into words.
  char buf[METHOD_TEXT_SZ];
  int  buf_len = start < (int)sizeof(buf) - 1 ? start : (int)sizeof(buf) - 1;

  memcpy(buf, rl_line_buffer, (size_t)buf_len);
  buf[buf_len] = '\0';

  // Tokenize into up to 8 words.
  #define MAX_TOKENS 8
  char *tokens[MAX_TOKENS];
  int   token_count = 0;
  char *p = buf;

  while(*p != '\0' && token_count < MAX_TOKENS)
  {
    while(*p == ' ' || *p == '\t')
      p++;

    if(*p == '\0')
      break;

    tokens[token_count++] = p;

    while(*p != '\0' && *p != ' ' && *p != '\t')
      p++;

    if(*p != '\0')
      *p++ = '\0';
  }

  if(token_count == 0)
    return(NULL);

  // First token should be the command (with / prefix).
  char *cmd_tok = tokens[0];

  if(cmd_tok[0] == '/')
    cmd_tok++;

  // Look up the command.
  const cmd_def_t *d = cmd_find(cmd_tok);

  if(d == NULL)
    return(NULL);

  // Walk through parent -> child resolution for subcommands.
  // cmd_find() is global but subcommand names are unique, and we
  // verify the parent matches to avoid false positives.
  int tok_idx = 1;

  while(tok_idx < token_count && cmd_has_children(d))
  {
    const cmd_def_t *child = cmd_find(tokens[tok_idx]);

    if(child == NULL || cmd_get_parent(child) != d)
      break;

    d = child;
    tok_idx++;
  }

  // Now: d is the resolved command, tok_idx points at the first
  // argument token (or token_count if all tokens were consumed).

  // If the command has children and we're completing the next token,
  // offer subcommand completion.
  if(cmd_has_children(d))
  {
    con_complete_subcommand(text, d);

    if(con_comp.count > 0)
      return(rl_completion_matches(text, con_comp_generator));

    return(NULL);
  }

  // Argument completion — determine the position.
  int arg_pos = token_count - tok_idx;
  const char *resolved_name = cmd_get_name(d);

  con_complete_args(text, resolved_name, d, arg_pos);

  // For KV key completions, suppress trailing space on partial matches.
  if((strcasecmp(resolved_name, "set") == 0 && arg_pos == 0)
      || (strcasecmp(resolved_name, "kv") == 0 && arg_pos == 0))
  {
    // Check if any match ends with '.' — if so, it's a partial prefix
    // and we don't want a trailing space.
    for(int i = 0; i < con_comp.count; i++)
    {
      size_t len = strlen(con_comp.matches[i]);

      if(len > 0 && con_comp.matches[i][len - 1] == '.')
      {
        rl_completion_suppress_append = 1;
        break;
      }
    }
  }

  if(con_comp.count > 0)
    return(rl_completion_matches(text, con_comp_generator));

  return(NULL);

  #undef MAX_TOKENS
}

// -----------------------------------------------------------------------
// Clear and Ctrl+C handlers.
// -----------------------------------------------------------------------

// /clear — clear the terminal screen and redraw the prompt.
// ctx: command context (unused beyond reply)
static void
con_cmd_clear(const cmd_ctx_t *ctx)
{
  (void)ctx;

  console_output_lock();
  printf("\033[2J\033[H");
  fflush(stdout);

  if(con_readline_active)
    rl_forced_update_display();

  console_output_unlock();
}

// Readline key handler for Ctrl+C (character 0x03). Clears the current
// input line and prints ^C, then redisplays a fresh prompt.
// count: repeat count (unused)
// key: key code (unused)
// returns: 0 (success)
static int
con_ctrl_c_handler(int count, int key)
{
  (void)count;
  (void)key;

  printf("^C\n");
  rl_replace_line("", 0);
  rl_point = 0;
  rl_on_new_line();
  rl_redisplay();

  return(0);
}

// -----------------------------------------------------------------------
// Readline support: prompt builder, shutdown hook, and input loop.
// -----------------------------------------------------------------------

// Build the dynamic prompt string from the KV format template.
// Substitution tokens: {bot} → attached bot name (empty if none),
// {user} → associated username (empty when @owner),
// {time} → current HH:MM:SS.
// When con_prompt_color is enabled, {bot} is green and {user} is cyan.
static void
con_build_prompt(void)
{
  const char *bot_name = "";
  const char *user_name = "";

  if(con_attached_bot != NULL)
    bot_name = bot_inst_name(con_attached_bot);

  if(con_attached_bot != NULL && !userns_is_owner(con_associated_user))
    user_name = con_associated_user;

  char timebuf[16];
  time_t now = time(NULL);
  struct tm tm;

  localtime_r(&now, &tm);
  snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d",
      tm.tm_hour, tm.tm_min, tm.tm_sec);

  // Walk the format string and perform token substitution.
  char  *out = con_prompt;
  size_t rem = sizeof(con_prompt) - 1;
  const char *p = con_prompt_fmt;

  while(*p != '\0' && rem > 0)
  {
    if(*p == '{')
    {
      const char *val  = NULL;
      const char *cstart = "";
      const char *cend   = "";
      size_t skip = 0;

      if(strncmp(p, "{bot}", 5) == 0)
      {
        val  = bot_name;
        skip = 5;

        if(con_prompt_color && val[0] != '\0')
        {
          cstart = CON_GREEN;
          cend   = CON_RESET;
        }
      }
      else if(strncmp(p, "{user}", 6) == 0)
      {
        val  = user_name;
        skip = 6;

        if(con_prompt_color && val[0] != '\0')
        {
          cstart = CON_CYAN;
          cend   = CON_RESET;
        }
      }
      else if(strncmp(p, "{time}", 6) == 0)
      {
        val  = timebuf;
        skip = 6;
      }

      if(val != NULL)
      {
        int n = snprintf(out, rem + 1, "%s%s%s", cstart, val, cend);

        if(n > 0 && (size_t)n <= rem)
        {
          out += n;
          rem -= (size_t)n;
        }

        p += skip;
        continue;
      }
    }

    *out++ = *p++;
    rem--;
  }

  *out = '\0';
}

// Readline event hook: called periodically while readline waits for
// input. Signals readline to return when a graceful shutdown is pending.
// returns: 0 (required by rl_event_hook signature)
static int
con_check_shutdown(void)
{
  if(pool_shutting_down())
    rl_done = 1;

  return(0);
}

// -----------------------------------------------------------------------
// Persistent task callback for console input. Uses GNU Readline for
// line editing and history. Ctrl+D on an empty line prints a hint
// instead of exiting; use /quit for shutdown.
// t: the persist task
// -----------------------------------------------------------------------
static void
con_input_cb(task_t *t)
{
  rl_catch_signals = 0;
  rl_event_hook = con_check_shutdown;

  // Bind Ctrl+C (0x03) to clear the current input line instead of
  // triggering SIGINT (rl_catch_signals=0 prevents readline from
  // installing its own signal handler, so the character reaches us).
  rl_bind_key(0x03, con_ctrl_c_handler);

  while(!pool_shutting_down())
  {
    con_build_prompt();

    con_readline_active = true;
    char *line = readline(con_prompt);
    con_readline_active = false;

    if(line == NULL)
    {
      // Ctrl+D / EOF on empty line — don't exit, inform user.
      printf("\n");
      console_print("Use /quit to exit.");
      continue;
    }

    // Strip leading/trailing whitespace.
    char *p = line;

    while(*p == ' ' || *p == '\t')
      p++;

    if(*p != '\0')
    {
      // History expansion: handle !!, !prefix, ^old^new, etc.
      char *expansion = NULL;
      int   hx_result = history_expand(p, &expansion);

      if(hx_result == -1)
      {
        // Expansion error — print the error and skip dispatch.
        console_print(expansion);
        free(expansion);
        free(line);
        continue;
      }

      if(hx_result == 2)
      {
        // Display only (e.g., !:p) — show but don't dispatch.
        console_print(expansion);
        free(expansion);
        free(line);
        continue;
      }

      // Use expanded line if expansion occurred.
      char *dispatch_line = (hx_result == 1) ? expansion : p;

      if(hx_result == 1)
        console_print(dispatch_line);

      // Add to history if non-empty and not a duplicate of the
      // most recent entry.
      HIST_ENTRY *prev = history_get(history_length);

      if(prev == NULL || strcmp(prev->line, dispatch_line) != 0)
        add_history(dispatch_line);

      con_dispatch(dispatch_line);
      free(expansion);
    }

    free(line);
  }

  t->state = TASK_ENDED;
}

// -----------------------------------------------------------------------
// Method driver callbacks for the console method.
// -----------------------------------------------------------------------

// Sentinel handle — console has no per-instance state but the
// method layer requires a non-NULL return from create().
static char console_sentinel;

// Create instance state (minimal — console is a singleton).
static void *
console_drv_create(const char *inst_name)
{
  (void)inst_name;
  return(&console_sentinel);
}

// Destroy instance state.
static void
console_drv_destroy(void *handle)
{
  (void)handle;
}

// Connect: no-op for console (input reader started in register_method).
static bool
console_drv_connect(void *handle)
{
  (void)handle;
  return(SUCCESS);
}

// Disconnect: the persist task will exit when pool shuts down.
static void
console_drv_disconnect(void *handle)
{
  (void)handle;
}

// Send: write user-facing text directly to the console.
static bool
console_drv_send(void *handle, const char *target, const char *text)
{
  (void)handle;
  (void)target;

  console_print(text);
  return(SUCCESS);
}

// Get context: console is always "console".
static bool
console_drv_get_context(void *handle, const char *sender,
    char *ctx, size_t ctx_sz)
{
  (void)handle;
  (void)sender;

  strncpy(ctx, "console", ctx_sz - 1);
  ctx[ctx_sz - 1] = '\0';
  return(SUCCESS);
}

// ANSI color table for console output.
static const color_table_t console_colors = {
  .red    = CON_RED,
  .green  = CON_GREEN,
  .yellow = CON_YELLOW,
  .blue   = CON_BLUE,
  .purple = CON_PURPLE,
  .cyan   = CON_CYAN,
  .white  = CON_WHITE,
  .orange = CON_ORANGE,
  .gray   = CON_GRAY,
  .bold   = CON_BOLD,
  .reset  = CON_RESET,
};

// Console method driver.
static const method_driver_t console_driver = {
  .name        = "console",
  .colors      = &console_colors,
  .create      = console_drv_create,
  .destroy     = console_drv_destroy,
  .connect     = console_drv_connect,
  .disconnect  = console_drv_disconnect,
  .send        = console_drv_send,
  .get_context = console_drv_get_context,
};

// Register /console subcommands (attach, unattach, associate, unassociate)
// with the command subsystem as system-level commands.
static void
console_register_commands(void)
{
  cmd_register_system("console", "console",
      "console <subcommand> ...",
      "Console attach/associate management",
      "Manage console bot attachment and user association.\n"
      "Use /console attach to route commands to a bot instance.\n"
      "Use /console associate to test commands as a specific user.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_CONSOLE | METHOD_T_BOTMANCTL,
      con_cmd_console, NULL, NULL, "con", NULL, 0);

  cmd_register_system("console", "attach",
      "console attach <botname>",
      "Attach console to a bot instance",
      "Attaches the console to a running bot instance. All subsequent\n"
      "commands are routed to the bot's command dispatcher. System\n"
      "commands remain accessible.\n"
      "Example: /console attach mybot",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_attach, NULL, "console", "at", con_ad_attach, 1);

  cmd_register_system("console", "unattach",
      "console unattach",
      "Detach console from bot",
      "Detaches the console from the currently attached bot.\n"
      "Resets the associated user to @owner.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_unattach, NULL, "console", "uat", NULL, 0);

  cmd_register_system("console", "associate",
      "console associate <username>",
      "Set console user identity",
      "Sets the user identity for console commands dispatched to the\n"
      "attached bot. Only available when attached. The user must exist\n"
      "in the bot's user namespace. Privilege checks are still bypassed.\n"
      "Example: /console associate alice",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_associate, NULL, "console", "as", con_ad_associate, 1);

  cmd_register_system("console", "unassociate",
      "console unassociate",
      "Reset console user to @owner",
      "Resets the console user identity to the built-in @owner user.\n"
      "Only available when attached.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_unassociate, NULL, "console", "uas", NULL, 0);

  // /history parent and subcommands.
  cmd_register_system("console", "history",
      "history <subcommand> ...",
      "Command history management",
      "View, search, and manage console command history.\n"
      "History is saved across sessions.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_CONSOLE,
      con_cmd_history, NULL, NULL, "hist", NULL, 0);

  cmd_register_system("console", "list",
      "history list [count]",
      "Show recent history entries",
      "Displays the last N history entries (default 20).\n"
      "Example: /history list 50",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_history_list, NULL, "history", "ls",
      con_ad_history_list, 1);

  cmd_register_system("console", "clear",
      "history clear",
      "Clear all command history",
      "Clears the in-memory history and removes the history file.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_history_clear, NULL, "history", "cl", NULL, 0);

  cmd_register_system("console", "search",
      "history search <pattern>",
      "Search history for a substring",
      "Searches all history entries for the given substring and\n"
      "displays matches with their history numbers.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_ANY,
      con_cmd_history_search, NULL, "history", "s",
      con_ad_history_search, 1);

  // /clear — clear the terminal screen.
  cmd_register_system("console", "clear",
      "clear",
      "Clear the terminal screen",
      "Clears the terminal screen and redraws the prompt.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL, CMD_SCOPE_ANY,
      METHOD_T_CONSOLE,
      con_cmd_clear, NULL, NULL, "cls", NULL, 0);
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Lock console output for thread-safe readline interleaving.
// Saves and clears the readline prompt/input if readline is active,
// so the caller can write to stdout without corrupting the display.
// Must be paired with console_output_unlock().
void
console_output_lock(void)
{
  if(!console_active)
    return;

  pthread_mutex_lock(&con_output_mutex);

  if(con_readline_active)
  {
    rl_save_prompt();
    rl_replace_line("", 0);
    rl_redisplay();
  }
}

// Unlock console output after writing. Restores the readline prompt
// and input line if readline is active.
void
console_output_unlock(void)
{
  if(!console_active)
    return;

  if(con_readline_active)
  {
    rl_restore_prompt();
    rl_redisplay();
  }

  pthread_mutex_unlock(&con_output_mutex);
}

// Write user-facing output directly to stdout, bypassing CLAM.
// Thread-safe: saves and restores readline state when active.
// text: output text (one line, trailing newline added automatically)
void
console_print(const char *text)
{
  if(text == NULL)
    return;

  console_output_lock();
  printf("%s\n", text);
  console_output_unlock();
}

// Register console KV settings. Must be called after kv_init()/kv_load().
void
console_register_config(void)
{
  kv_register("core.console.history.file", KV_STR,
      "~/.config/botmanager/history", con_kv_changed, NULL);
  kv_register("core.console.history.size", KV_UINT32,
      "1000", con_kv_changed, NULL);
  kv_register("core.console.prompt.format", KV_STR,
      "{bot}:{user}> ", con_kv_changed, NULL);
  kv_register("core.console.prompt.color", KV_UINT8,
      "1", con_kv_changed, NULL);

  con_load_config();
}

// Register the console as a method instance, register /console subcommands,
// load persistent history, and start the stdin input reader persist task.
// start_time: program start time (stored for uptime display)
void
console_register_method(time_t start_time)
{
  con_start_time = start_time;
  console_active = true;

  // Register as a method instance.
  con_method_inst = method_register(&console_driver, "console");

  if(con_method_inst == NULL)
  {
    clam(CLAM_FATAL, "console", "failed to register console method");
    return;
  }

  // Tell cmd subsystem about the console method instance.
  cmd_set_console_inst(con_method_inst);

  // Register /console and /history subcommands.
  console_register_commands();

  // Load persistent history and cap the list size.
  con_resolve_history_path();

  if(con_history_path[0] != '\0')
    read_history(con_history_path);

  stifle_history((int)con_history_size);

  // Install custom tab completion.
  rl_attempted_completion_function = con_completion;
  rl_completer_word_break_characters = " \t";
  rl_completion_suppress_append = 0;

  // Start the input reader persist task and mark as available.
  task_t *t = task_add_persist("console_input", 0, con_input_cb, NULL);

  if(t == NULL)
  {
    clam(CLAM_FATAL, "console", "failed to start input task");
    return;
  }

  method_set_state(con_method_inst, METHOD_AVAILABLE);

  clam(CLAM_INFO, "console", "registered as method instance");

  console_print("Console ready. Type /help for commands. "
      "Tab to complete. Ctrl+R to search history.");
}

// Get the console method instance.
// returns: console method instance, or NULL if not yet registered
method_inst_t *
console_get_inst(void)
{
  return(con_method_inst);
}

// Shut down the console method. Cleans up readline state, destroys
// the output mutex, and resets attach/associate state.
void
console_exit(void)
{
  if(!console_active)
    return;

  clam(CLAM_INFO, "console_exit", "console method shutting down");

  // Persist command history to disk.
  if(con_history_path[0] != '\0')
  {
    write_history(con_history_path);
    history_truncate_file(con_history_path, (int)con_history_size);
  }

  // Restore terminal settings altered by readline.
  rl_cleanup_after_signal();
  clear_history();

  pthread_mutex_destroy(&con_output_mutex);

  con_comp_reset();

  con_attached_bot = NULL;
  strncpy(con_associated_user, USERNS_OWNER_USER,
      sizeof(con_associated_user) - 1);

  console_active = false;
}
