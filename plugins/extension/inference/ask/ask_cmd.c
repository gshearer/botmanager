// botmanager — MIT
// ask command-surface plugin: a public, stateless one-shot LLM query.
//
//   !ask <query>          one prompt against the bot's default chat model
//   !a <query>            short alias
//   !ask -m <model> ...   pick a specific model from the per-bot allowlist
//   !show ask             the request !ask would make, and under what limits
//
// There is deliberately NO conversation memory and no context carry —
// each call is an independent request (this is not the `chat` method,
// which is the personality bot). Model exposure is governed by a
// per-bot allowlist (`bot.<name>.ask.allow`, inheriting the global
// `plugin.ask.allow`); embed models are never selectable.
#define ASK_CMD_INTERNAL
#include "ask_cmd.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#include "colors.h"
#include "display.h"   // DISPLAY_COLS, display_fit
#include "bot.h"        // bot_inst_name

// -----------------------------------------------------------------------
// KV schema — registered once under plugin.ask.*
// -----------------------------------------------------------------------

// The plugin tier is the ROOT of a three-level resolution
// (plugin -> bot -> bot.protocol). allow is an absolute list the lower
// tiers may only narrow (intersection); max_tokens / max_lines / max_cols
// are hard ceilings the lower tiers may only lower (min); default model and
// prompt_prepend_file cascade most-specific-non-empty-wins. The per-bot
// and per-(bot,protocol) keys are contributed dynamically at bot-create /
// method-bind time — see ask_kv_bot_cb / ask_kv_proto_cb.
static const plugin_kv_entry_t ask_kv_schema[] = {
  { "plugin.ask.default",    KV_STR,    "gfll",
    "Default chat model for !ask when no -m is given" },
  { "plugin.ask.allow",      KV_STR,    "*",
    "Absolute allowlist of exposable chat-model names ('*' = all enabled);"
    " bot / protocol tiers may only narrow this" },
  // Effort is a membership test, never a ceiling: there is no order to
  // clamp along (q38nv takes 'xhigh' and refuses 'high'), so the lower
  // tiers narrow this list rather than lowering a number. Empty default =
  // send nothing, which leaves llm.service.<name>.reasoning_effort in
  // charge — the tuning every service already carries.
  { "plugin.ask.effort",       KV_STR, "",
    "Default reasoning_effort for !ask when no -e is given"
    " (none|minimal|low|medium|high|xhigh). Empty = send nothing and let"
    " the service's own llm.service.<name>.reasoning_effort decide" },
  { "plugin.ask.effort_allow", KV_STR, "*",
    "Absolute allowlist of reasoning_effort values !ask may be asked for"
    " ('*' = any the model declares); bot / protocol tiers may only narrow"
    " this. A raised effort spends the operator's paid quota" },
  { "plugin.ask.max_tokens", KV_UINT32, "1024",
    "Hard ceiling on max_tokens per !ask; bot / protocol tiers may only"
    " lower it" },
  { "plugin.ask.max_lines",  KV_UINT32, "30",
    "Hard ceiling on reply lines per answer (flood guard); bot / protocol"
    " tiers may only lower it" },
  // Keep in step with ASK_WRAP_COLS, the emit-loop fallback.
  { "plugin.ask.max_cols",   KV_UINT32, "100",
    "Hard ceiling on reply columns before word-wrap; each wrapped fragment"
    " counts against max_lines. Bot / protocol tiers may only lower it" },
  // Path is relative to the daemon CWD (build/), so reach up to the
  // project-root prompts/ dir; bot / protocol tiers may override.
  { "plugin.ask.prompt_prepend_file", KV_STR, "../prompts/ask_default.txt",
    "Path to a .txt file whose contents are prepended (as a system prompt)"
    " to every !ask query; bot / protocol tiers may override" },
};

// -----------------------------------------------------------------------
// Per-bot / per-(bot,protocol) KV contributors
// -----------------------------------------------------------------------
//
// Registered with core (bot_kv_contributor_register) so every bot instance
// and every bound protocol grows its own ask.* override keys. Core invokes
// these at bot-create / method-bind time and back-fills existing bots when
// the plugin (re)loads. Keys default to "inherit" values: allow="*" (no
// narrowing), max_*="0" (no lowering), prompt_prepend_file="" (fall through
// to the parent tier). The default model is materialised from the parent
// tier at instantiation so `show kv` reveals the effective choice.

// Stable per-plugin address used as the KV-contributor registration
// cookie (register / unregister match on it).
static const char ask_kv_cookie;

static void
ask_kv_bot_cb(const char *botname, void *user)
{
  char        key[KV_KEY_SZ];
  const char *inherit;
  char        def[KV_STR_SZ];

  (void)user;

  // Snapshot the plugin default so the per-bot key advertises the model it
  // inherited at creation (task: "inherited by bot-specific keys when
  // instantiated"). Operators can override; clearing it falls back to the
  // plugin default at resolution time.
  inherit = kv_get_str("plugin.ask.default");
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.ask.default", botname);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-bot !ask default model (empty inherits plugin.ask.default)");

  snprintf(key, sizeof(key), "bot.%s.ask.allow", botname);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-bot !ask allowlist ('*' = inherit; else narrows plugin.ask.allow)");

  // Same materialisation as the default model above, and it is safe for
  // the same reason: the plugin tier ships empty, so a bot created today
  // inherits "unset" and keeps deferring to its service.
  inherit = kv_get_str("plugin.ask.effort");
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.ask.effort", botname);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-bot !ask default reasoning_effort (empty inherits"
      " plugin.ask.effort; empty everywhere = the service decides)");

  snprintf(key, sizeof(key), "bot.%s.ask.effort_allow", botname);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-bot !ask effort allowlist ('*' = inherit; else narrows"
      " plugin.ask.effort_allow)");

  snprintf(key, sizeof(key), "bot.%s.ask.max_tokens", botname);
  kv_register(key, KV_UINT32, "0", NULL, NULL,
      "Per-bot max_tokens cap for !ask (0 = inherit plugin ceiling)");

  snprintf(key, sizeof(key), "bot.%s.ask.max_lines", botname);
  kv_register(key, KV_UINT32, "0", NULL, NULL,
      "Per-bot reply-line cap for !ask (0 = inherit plugin ceiling)");

  snprintf(key, sizeof(key), "bot.%s.ask.max_cols", botname);
  kv_register(key, KV_UINT32, "0", NULL, NULL,
      "Per-bot wrap-column cap for !ask (0 = inherit plugin ceiling)");

  snprintf(key, sizeof(key), "bot.%s.ask.prompt_prepend_file", botname);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-bot prepend-file path for !ask (empty inherits plugin default)");
}

static void
ask_kv_proto_cb(const char *botname, const char *protocol, void *user)
{
  char        key[KV_KEY_SZ];
  const char *inherit;
  char        def[KV_STR_SZ];

  (void)user;

  // Materialise the protocol default from the bot tier it was created under.
  inherit = kv_get_bot_str(botname, "ask.default");
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.default", botname, protocol);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-protocol !ask default model (empty inherits the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.allow", botname, protocol);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-protocol !ask allowlist ('*' = inherit; else narrows the bot tier)");

  inherit = kv_get_bot_str(botname, "ask.effort");
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.effort", botname, protocol);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-protocol !ask default reasoning_effort (empty inherits the bot"
      " tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.effort_allow", botname, protocol);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-protocol !ask effort allowlist ('*' = inherit; else narrows the"
      " bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.max_tokens", botname, protocol);
  kv_register(key, KV_UINT32, "0", NULL, NULL,
      "Per-protocol max_tokens cap for !ask (0 = inherit)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.max_lines", botname, protocol);
  kv_register(key, KV_UINT32, "0", NULL, NULL,
      "Per-protocol reply-line cap for !ask (0 = inherit)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.max_cols", botname, protocol);
  kv_register(key, KV_UINT32, "0", NULL, NULL,
      "Per-protocol wrap-column cap for !ask (0 = inherit)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.prompt_prepend_file",
      botname, protocol);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-protocol prepend-file path for !ask (empty inherits)");
}

// -----------------------------------------------------------------------
// Resolution helpers
// -----------------------------------------------------------------------

// The protocol (method kind, e.g. "irc") a command arrived on, or NULL
// when it can't be determined — the third resolution tier keys off this.
static const char *
ask_proto(const cmd_ctx_t *ctx)
{
  if(ctx->msg != NULL && ctx->msg->inst != NULL)
    return(method_inst_kind(ctx->msg->inst));

  return(NULL);
}

// Read bot.<bot>.ask.<suffix>, or NULL if bot_name is absent.
static const char *
ask_kv_bot(const char *bot_name, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(bot_name == NULL)
    return(NULL);

  snprintf(key, sizeof(key), "bot.%s.ask.%s", bot_name, suffix);
  return(kv_get_str(key));
}

// Read bot.<bot>.<proto>.ask.<suffix>, or NULL if either scope is absent.
static const char *
ask_kv_proto(const char *bot_name, const char *proto, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(bot_name == NULL || proto == NULL)
    return(NULL);

  snprintf(key, sizeof(key), "bot.%s.%s.ask.%s", bot_name, proto, suffix);
  return(kv_get_str(key));
}

// First non-empty of the most-specific-first candidates, or NULL.
static const char *
ask_first_nonempty(const char *a, const char *b, const char *c)
{
  if(a != NULL && a[0] != '\0')
    return(a);
  if(b != NULL && b[0] != '\0')
    return(b);
  if(c != NULL && c[0] != '\0')
    return(c);

  return(NULL);
}

// One cascading knob, resolved: the winning value together with the full
// KV key that produced it. ask_first_nonempty above returns the value and
// throws the key away, which is why a `show ask` disagreeing with
// `show kv plugin.ask.default` has no way to say which tier won.
typedef struct
{
  const char *value;           // NULL when no tier is set
  char        key[KV_KEY_SZ];  // "" when value is NULL
} ask_tiered_t;

// Resolve one cascading knob across all three tiers, most-specific first,
// recording both halves. The keys are spelled here rather than borrowed
// from ask_kv_bot / ask_kv_proto because the winner has to survive the
// call, and those build theirs on the stack.
static void
ask_tier_resolve(const char *bot_name, const char *proto, const char *suffix,
    ask_tiered_t *out)
{
  const char *v;

  out->value  = NULL;
  out->key[0] = '\0';

  if(bot_name != NULL && proto != NULL)
  {
    snprintf(out->key, sizeof(out->key), "bot.%s.%s.ask.%s", bot_name, proto,
        suffix);
    v = kv_get_str(out->key);

    if(v != NULL && v[0] != '\0')
    {
      out->value = v;
      return;
    }
  }

  if(bot_name != NULL)
  {
    snprintf(out->key, sizeof(out->key), "bot.%s.ask.%s", bot_name, suffix);
    v = kv_get_str(out->key);

    if(v != NULL && v[0] != '\0')
    {
      out->value = v;
      return;
    }
  }

  snprintf(out->key, sizeof(out->key), "plugin.ask.%s", suffix);
  v = kv_get_str(out->key);

  if(v != NULL && v[0] != '\0')
  {
    out->value = v;
    return;
  }

  out->key[0] = '\0';
}

// Resolved per-request scope: the effective default model, the three
// allowlist tiers (each a membership filter), and the reply ceilings.
// Built once per command and threaded through the gate + reply loop.
typedef struct
{
  ask_tiered_t  def_model;     // most-specific non-empty, or .value == NULL
  const char   *allow_plugin;  // absolute list
  const char   *allow_bot;     // narrows plugin (NULL/empty/"*" = no-op)
  const char   *allow_proto;   // narrows bot    (NULL/empty/"*" = no-op)
  ask_tiered_t  def_effort;    // most-specific non-empty, or .value == NULL
  const char   *effort_plugin; // the same three tiers, for -e
  const char   *effort_bot;
  const char   *effort_proto;
  uint32_t      max_lines;     // min across present tiers
  uint32_t      max_tokens;    // min across present tiers
  uint32_t      max_cols;      // min across present tiers, buffer-clamped
} ask_scope_t;

// True iff `model` is a whole comma/space-separated token of `csv`
// (case-insensitive). `csv` is left unmodified.
static bool
ask_csv_contains(const char *csv, const char *model)
{
  char  scratch[KV_STR_SZ];
  char *save;

  snprintf(scratch, sizeof(scratch), "%s", csv);

  for(char *tok = strtok_r(scratch, ", \t", &save); tok != NULL;
      tok = strtok_r(NULL, ", \t", &save))
    if(strcasecmp(tok, model) == 0)
      return(true);

  return(false);
}

// A tier that imposes no restriction at all: unset, empty, or "*". The
// card asks this directly — an `allowed` line under three open tiers is a
// catalogue of everything, which is what `!show llm models chat` is for.
static bool
ask_tier_open(const char *csv)
{
  return(csv == NULL || csv[0] == '\0' || strcmp(csv, "*") == 0);
}

// A single allowlist tier admits `model` when it imposes no restriction
// or explicitly lists it.
static bool
ask_tier_admits(const char *csv, const char *model)
{
  if(ask_tier_open(csv))
    return(true);

  return(ask_csv_contains(csv, model));
}

// One ceiling resolved as the minimum of the plugin value and any lower
// tier that opts in (a tier value of 0 means "inherit, don't lower").
// Never returns below `floor`.
static uint32_t
ask_ceiling(const char *bot_name, const char *proto, const char *suffix,
    uint32_t floor)
{
  char     key[KV_KEY_SZ];
  uint32_t eff;
  uint32_t tier;

  snprintf(key, sizeof(key), "plugin.ask.%s", suffix);
  eff = (uint32_t)kv_get_uint(key);

  if(bot_name != NULL)
  {
    snprintf(key, sizeof(key), "bot.%s.ask.%s", bot_name, suffix);
    tier = (uint32_t)kv_get_uint(key);
    if(tier > 0 && (eff == 0 || tier < eff))
      eff = tier;
  }

  if(bot_name != NULL && proto != NULL)
  {
    snprintf(key, sizeof(key), "bot.%s.%s.ask.%s", bot_name, proto, suffix);
    tier = (uint32_t)kv_get_uint(key);
    if(tier > 0 && (eff == 0 || tier < eff))
      eff = tier;
  }

  return(eff < floor ? floor : eff);
}

// Populate `s` for a request on (bot_name, proto). Any tier may be absent.
static void
ask_scope_resolve(const char *bot_name, const char *proto, ask_scope_t *s)
{
  memset(s, 0, sizeof(*s));

  ask_tier_resolve(bot_name, proto, "default", &s->def_model);

  s->allow_plugin = kv_get_str("plugin.ask.allow");
  s->allow_bot    = ask_kv_bot(bot_name, "allow");
  s->allow_proto  = ask_kv_proto(bot_name, proto, "allow");

  ask_tier_resolve(bot_name, proto, "effort", &s->def_effort);

  s->effort_plugin = kv_get_str("plugin.ask.effort_allow");
  s->effort_bot    = ask_kv_bot(bot_name, "effort_allow");
  s->effort_proto  = ask_kv_proto(bot_name, proto, "effort_allow");

  s->max_lines  = ask_ceiling(bot_name, proto, "max_lines",  1);
  s->max_tokens = ask_ceiling(bot_name, proto, "max_tokens", 1);
  s->max_cols   = ask_ceiling(bot_name, proto, "max_cols",
      ASK_WRAP_COLS_MIN);

  // Clamp to what the emit buffer can carry: a wider setting would leave
  // ask_done's snprintf truncating each line instead of wrapping it.
  if(s->max_cols > ASK_WRAP_COLS_MAX)
    s->max_cols = ASK_WRAP_COLS_MAX;
}

// Resolve the effective prepend-file path (most-specific non-empty) and
// slurp it into `buf` (NUL-terminated, truncated to `cap`, trailing
// newlines trimmed). Returns SUCCESS with buf populated, else FAIL with
// buf[0] == '\0'. Read fresh each call so file edits take effect live.
static bool
ask_read_prepend(const char *bot_name, const char *proto,
    char *buf, size_t cap)
{
  const char *path;
  FILE       *fp;
  size_t      n;

  if(cap == 0)
    return(FAIL);

  buf[0] = '\0';

  path = ask_first_nonempty(
      ask_kv_proto(bot_name, proto, "prompt_prepend_file"),
      ask_kv_bot(bot_name, "prompt_prepend_file"),
      kv_get_str("plugin.ask.prompt_prepend_file"));

  if(path == NULL)
    return(FAIL);

  fp = fopen(path, "r");

  if(fp == NULL)
  {
    // Debug, not warn: this is on the per-request path and a missing file
    // simply means "no system prompt" — logging every !ask would flood.
    clam(CLAM_DEBUG, ASK_CMD_CTX, "prepend file unreadable: '%s'", path);
    return(FAIL);
  }

  n = fread(buf, 1, cap - 1, fp);
  fclose(fp);
  buf[n] = '\0';

  while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = '\0';

  return(n > 0 ? SUCCESS : FAIL);
}

// The allowlist half of the gate, for a model already known to exist and
// to be of the right kind. It is split out because llm_model_iterate runs
// its callback under the registry's read lock, so a row collected there
// must be gated without calling back into the engine.
static bool
ask_allowed(const char *model, const ask_scope_t *s)
{
  // The resolved default is always reachable, even if the allowlists omit
  // it — a bot can always run its own configured default.
  if(s->def_model.value != NULL && strcasecmp(model, s->def_model.value) == 0)
    return(true);

  // Intersection: every present tier must admit the model.
  return(ask_tier_admits(s->allow_plugin, model) &&
         ask_tier_admits(s->allow_bot,    model) &&
         ask_tier_admits(s->allow_proto,  model));
}

// Gate a model for !ask: it must exist, be a chat model (embed models stay
// unreachable even under "*"), and either be the resolved default (always
// self-allowed) or survive the intersection of all present allow tiers.
static bool
ask_model_ok(const char *model, const ask_scope_t *s)
{
  llm_kind_t kind;

  if(model == NULL || model[0] == '\0')
    return(false);

  if(!llm_model_exists(model))
    return(false);

  // llm_model_kind uses the SUCCESS(=false)/FAIL(=true) convention, unlike
  // llm_model_exists which returns a natural bool — compare against SUCCESS
  // explicitly. A plain `!llm_model_kind(...)` inverts and rejects every
  // real model (found → SUCCESS → !false → true → "unavailable").
  if(llm_model_kind(model, &kind) != SUCCESS || kind != LLM_KIND_CHAT)
    return(false);

  return(ask_allowed(model, s));
}


// Gate an effort for !ask. Two independent membership tests and no
// ordering anywhere: the caller's allowlist says what this bot may ASK
// for, the model's declaration says what the provider will ACCEPT.
// `why` is filled only on refusal.
static bool
ask_effort_ok(llm_effort_t e, const char *model, const ask_scope_t *s,
    char *why, size_t why_sz)
{
  const char *wire = llm_effort_wire(e);
  bool        is_default;
  char        key[KV_KEY_SZ];
  const char *declared;

  if(e == LLM_EFFORT_UNSET)
    return(true);                 // nothing is sent; nothing to gate

  // The resolved default is always self-allowed, exactly as the resolved
  // default model is — a bot can always run its own configuration.
  is_default = s->def_effort.value != NULL
      && strcasecmp(wire, s->def_effort.value) == 0;

  if(!is_default
      && (!ask_tier_admits(s->effort_plugin, wire)
          || !ask_tier_admits(s->effort_bot,   wire)
          || !ask_tier_admits(s->effort_proto, wire)))
  {
    snprintf(why, why_sz, "effort '%s' is not allowed here", wire);
    return(false);
  }

  snprintf(key, sizeof(key), "llm.model.%s.efforts", model);
  declared = kv_get_str(key);

  // llm_effort_set_admits answers true on an EMPTY set — undeclared means
  // unmeasured, not forbidden (include/kv.h, the read-site rule) — so
  // there is no empty-check to write here, and writing one would refuse
  // every model whose set nobody has filled in yet.
  if(!llm_effort_set_admits(declared, e))
  {
    snprintf(why, why_sz, "%s accepts: %s", model, declared);
    return(false);
  }

  return(true);
}

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

// Append one whitespace-separated token to the query buffer with a
// single leading space before every token but the first. Silently drops
// tokens that would overflow — a truncated query beats a clobbered stack.
static void
ask_query_append(char *query, size_t cap, size_t *len, const char *tok)
{
  int wrote;

  if(*len >= cap)
    return;

  wrote = snprintf(query + *len, cap - *len, "%s%s",
      *len > 0 ? " " : "", tok);

  if(wrote > 0)
    *len += (size_t)wrote < cap - *len ? (size_t)wrote : cap - *len - 1;
}

// Split the raw argument string into the optional "-m <model>" and
// "-e <effort>" selections and the residual query. Each flag is consumed
// on its first occurrence; a trailing flag with no following token is
// dropped. Anything else is query text, "-x" included — !ask is a natural
// -language surface and refusing an unknown dash-word would eat the
// question.
static void
ask_parse_flags(const char *args, ask_flags_t *f, char *query,
    size_t query_cap)
{
  char   scratch[METHOD_TEXT_SZ];
  char  *save;
  size_t qlen = 0;

  snprintf(scratch, sizeof(scratch), "%s", args);
  memset(f, 0, sizeof(*f));
  query[0] = '\0';

  for(char *tok = strtok_r(scratch, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    if(!f->have_model && strcmp(tok, "-m") == 0)
    {
      char *m = strtok_r(NULL, " \t", &save);

      if(m == NULL)
        continue;                    // trailing "-m" with no value: drop

      strlcpy(f->model, m, sizeof(f->model));
      f->have_model = true;
      continue;
    }

    if(!f->have_effort && strcmp(tok, "-e") == 0)
    {
      char *e = strtok_r(NULL, " \t", &save);

      if(e == NULL)
        continue;                    // trailing "-e" with no value: drop

      strlcpy(f->effort, e, sizeof(f->effort));
      f->have_effort = true;
      continue;
    }

    ask_query_append(query, query_cap, &qlen, tok);
  }
}

// -----------------------------------------------------------------------
// Wrapped emission
// -----------------------------------------------------------------------
//
// The model cannot emit raw control bytes. Asked for "\033[1m" or "\x02"
// it reproduces the *notation* as literal text, which is what lands in the
// channel. So !ask advertises markup the model can actually type — the
// Markdown bold it already produces unprompted, plus <colour> tags — and
// color_markup_translate rewrites it into the method-agnostic markers from
// colors.h before anything below measures or emits a line. method_send()
// then translates those to the driver's native codes, so one answer
// renders correctly on IRC and on the botmanctl console alike.

// Formatting carried across wrapped lines. Every method drops formatting
// at a message boundary, so a continuation line must re-open whatever was
// still active when its predecessor broke.
typedef struct
{
  bool bold;
  char color;   // abstract marker id, or 0 for none
} ask_fmt_t;

static void
ask_fmt_apply(ask_fmt_t *fmt, char id)
{
  if(id == 'b')
    fmt->bold = !fmt->bold;

  else if(id == 'X')
  {
    fmt->bold  = false;
    fmt->color = 0;
  }

  else
    fmt->color = id;
}

// Append to a line under construction. Markers are cosmetic, so a full
// buffer drops them silently rather than truncating the answer.
static void
ask_line_put(char *line, size_t line_sz, size_t *li, const char *s)
{
  size_t n = strlen(s);

  if(*li + n >= line_sz)
    return;

  memcpy(line + *li, s, n);
  *li += n;
}

// Write `fmt` as a canonical marker sequence. Drivers have no "clear one
// attribute" code, so a state change resets and re-states everything.
static void
ask_fmt_emit(char *line, size_t line_sz, size_t *li, const ask_fmt_t *fmt)
{
  char m[3];

  ask_line_put(line, line_sz, li, "\x01X");

  if(fmt->color != 0)
  {
    m[0] = '\x01';
    m[1] = fmt->color;
    m[2] = '\0';
    ask_line_put(line, line_sz, li, m);
  }

  if(fmt->bold)
    ask_line_put(line, line_sz, li, "\x01" "b");
}

// Byte length of the next line: at most `cols` *visible* characters.
// Markers cost no columns, a multi-byte UTF-8 sequence counts as one, and
// the break prefers the last space in the window.
static size_t
ask_wrap_take(const char *s, size_t len, size_t cols)
{
  size_t i        = 0;
  size_t vis      = 0;
  size_t brk      = 0;
  bool   have_brk = false;

  while(i < len)
  {
    if(s[i] == '\x01' && i + 1 < len)
    {
      i += 2;
      continue;
    }

    if(vis == cols)
      break;

    if(s[i] == ' ')
    {
      brk      = i;
      have_brk = true;
    }

    i++;

    while(i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
      i++;

    vis++;
  }

  if(i >= len)
    return(len);

  // brk == 0 would make no progress: hard-break the over-long word instead.
  return((have_brk && brk > 0) ? brk : i);
}

// -----------------------------------------------------------------------
// Async completion
// -----------------------------------------------------------------------

// Runs on a curl worker thread: emit the answer line-by-line (flood-
// capped) and free the per-request closure. Light work only.
static void
ask_done(const llm_chat_response_t *resp)
{
  ask_req_t  *r = (ask_req_t *)resp->user_data;
  cmd_ctx_t   ctx;
  char        line[ASK_LINE_SZ];
  char       *text;
  size_t      text_sz;
  const char *p;
  uint32_t    max_lines;
  size_t      max_cols;
  ask_fmt_t   fmt          = { false, 0 };
  uint32_t    emitted      = 0;
  bool        flood_capped = false;

  ctx     = r->ctx;
  ctx.msg = &r->msg;

  if(!resp->ok || resp->content == NULL)
  {
    snprintf(line, sizeof(line), "ask: %s",
        resp->error != NULL ? resp->error : "no response");
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  max_lines = r->max_lines;
  if(max_lines == 0)
    max_lines = 1;

  max_cols = r->max_cols;
  if(max_cols == 0)
    max_cols = ASK_WRAP_COLS;

  else if(max_cols > ASK_WRAP_COLS_MAX)
    max_cols = ASK_WRAP_COLS_MAX;

  // Rewrite the model's markup into abstract markers up front, so the wrap
  // below measures the answer as the channel will see it rather than
  // counting tag bytes nobody renders.
  text_sz = strlen(resp->content) + 1;
  text    = mem_alloc(ASK_CMD_CTX, "markup", text_sz);
  color_markup_translate(text, text_sz, resp->content);

  for(p = text; *p != '\0' && !flood_capped; )
  {
    const char *nl  = strchr(p, '\n');
    size_t      seg = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
    size_t      off;

    // Strip a trailing CR (provider CRLF).
    if(seg > 0 && p[seg - 1] == '\r')
      seg--;

    // Word-wrap the logical line to max_cols so a long model line becomes
    // several readable IRC lines, each counting toward the flood cap. An
    // empty line (seg == 0) is skipped by the loop condition.
    for(off = 0; off < seg && !flood_capped; )
    {
      size_t take = ask_wrap_take(p + off, seg - off, max_cols);
      size_t li   = 0;

      if(emitted >= max_lines)
      {
        cmd_reply(&ctx, "…(truncated)");
        flood_capped = true;
        break;
      }

      // Re-open formatting the previous line broke in the middle of.
      if(fmt.bold || fmt.color != 0)
        ask_fmt_emit(line, sizeof(line), &li, &fmt);

      for(size_t i = 0; i < take; )
      {
        if(p[off + i] == '\x01' && i + 1 < take)
        {
          ask_fmt_apply(&fmt, p[off + i + 1]);
          ask_fmt_emit(line, sizeof(line), &li, &fmt);
          i += 2;
          continue;
        }

        if(li + 1 < sizeof(line))
          line[li++] = p[off + i];

        i++;
      }

      // Close the line explicitly: a client that carried formatting past a
      // message boundary would tint everything the bot says afterwards.
      if(fmt.bold || fmt.color != 0)
        ask_line_put(line, sizeof(line), &li, "\x01X");

      line[li] = '\0';
      cmd_reply(&ctx, line);
      emitted++;

      off += take;

      // Skip whitespace we broke on so the next line has no leading space.
      while(off < seg && p[off] == ' ')
        off++;
    }

    if(nl == NULL)
      break;

    p = nl + 1;
  }

  // The model stopped because it hit its token ceiling, not because the
  // answer was complete: signal it so the user can tell a short answer from
  // a guillotined one. Skip when we already cut the reply for IRC flood
  // (that path prints its own "…(truncated)" — a different cause).
  if(!flood_capped && resp->finish_reason != NULL
      && strcasecmp(resp->finish_reason, "length") == 0)
    cmd_reply(&ctx, "⋯ (answer cut off at the model's token limit — ask a"
        " narrower question or raise plugin.ask.max_tokens)");

  mem_free(text);
  mem_free(r);
}

// -----------------------------------------------------------------------
// Command handlers
// -----------------------------------------------------------------------

static void
ask_cmd_handler(const cmd_ctx_t *ctx)
{
  ask_req_t         *r;
  const char        *bot_name;
  const char        *proto;
  const char        *model;
  const char        *want_effort;
  ask_scope_t        scope;
  ask_flags_t        flags;
  llm_effort_t       effort;
  char               query[METHOD_TEXT_SZ];
  char               reply[ASK_CMD_REPLY_SZ];
  char               prepend[ASK_PREPEND_SZ];
  llm_message_t      msgs[2];
  size_t             n;
  llm_chat_params_t  p;

  static const char usage[] =
      "Usage: ask [-m <model>] [-e <effort>] <query>"
      "  ·  !show ask reports the defaults";

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  ask_parse_flags(ctx->args, &flags, query, sizeof(query));

  if(query[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  bot_name = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  proto    = ask_proto(ctx);
  ask_scope_resolve(bot_name, proto, &scope);
  model    = flags.have_model ? flags.model : scope.def_model.value;

  if(!ask_model_ok(model, &scope))
  {
    snprintf(reply, sizeof(reply),
        "unknown or unavailable model '%s' — try !show ask",
        (model != NULL && model[0] != '\0') ? model : "(none)");
    cmd_reply(ctx, reply);
    return;
  }

  // Effort is gated AFTER the model: "gpl accepts: low,medium,high" is
  // nonsense advice about a model the caller cannot reach anyway.
  want_effort = flags.have_effort ? flags.effort : scope.def_effort.value;

  if(llm_effort_from_str(want_effort, &effort) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "unknown effort '%s' — one of: none minimal low medium high xhigh",
        want_effort);
    cmd_reply(ctx, reply);
    return;
  }

  if(!ask_effort_ok(effort, model, &scope, reply, sizeof(reply)))
  {
    cmd_reply(ctx, reply);
    return;
  }

  r = mem_alloc(ASK_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx       = *ctx;
  r->max_lines = scope.max_lines;
  r->max_cols  = scope.max_cols;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  memset(msgs, 0, sizeof(msgs));   // header: blocks ptr must be zeroed
  n = 0;

  // Prepend-file contents become the one-shot system prompt. Resolved
  // most-specific-first (protocol -> bot -> plugin) and read fresh so
  // edits to the file take effect without a reload. prepend outlives the
  // submit call (llm_chat_submit copies internally).
  if(ask_read_prepend(bot_name, proto, prepend, sizeof(prepend)) == SUCCESS)
  {
    msgs[n].role    = LLM_ROLE_SYSTEM;
    msgs[n].content = prepend;
    n++;
  }

  msgs[n].role    = LLM_ROLE_USER;
  msgs[n].content = query;
  n++;

  p            = (llm_chat_params_t){ 0 };
  p.max_tokens = scope.max_tokens;
  p.effort     = effort;

  // model/msgs/query are caller-owned only until submit returns
  // (the callee copies internally) — all live here for the call.
  //
  // llm_chat_submit uses SUCCESS(=false)/FAIL(=true): on a successful
  // enqueue the async request OWNS r and frees it via ask_done. A plain
  // `!llm_chat_submit(...)` inverts that — it took the failure branch on
  // SUCCESS, freeing r out from under the in-flight request (use-after-
  // free + double-free in ask_done). Compare against SUCCESS explicitly.
  if(llm_chat_submit(model, &p, msgs, n, ask_done, NULL, r) != SUCCESS)
  {
    cmd_reply(ctx, "ask: failed to submit query (model unavailable?)");
    mem_free(r);
  }
}

// -----------------------------------------------------------------------
// !show ask — one model, so a card and not a table
//
// A column whose value is identical on every row carries no information
// (include/display.h). The menu that stood here had three — service,
// model, model id — and every one of them was the registry restated,
// identical for every caller, describing N models when !ask will use
// exactly one. What it never said was which model, chosen by which tier,
// at what effort, under what ceilings. That is a card.
//
// `show imagine` keeps its table because the same rule comes out the
// other way there: its rows carry per-model counters and they differ.
// -----------------------------------------------------------------------

#define ASK_CARD_INDENT      2
#define ASK_CARD_LABEL       8
#define ASK_CARD_VALUE_COLS  (DISPLAY_COLS - ASK_CARD_INDENT \
                              - ASK_CARD_LABEL - 2)   // ": "
#define ASK_CARD_FIELD_SZ    128    // a registry model id, at its widest
#define ASK_CARD_CELL_SZ     256    // a fitted cell: 90 columns of UTF-8
#define ASK_CARD_ALLOWED_SZ  512
#define ASK_CARD_EFFORTS_SZ  128    // a declared effort set is a short CSV

// One card line, its value already coloured and already fitted.
static void
ask_card(const cmd_ctx_t *ctx, const char *label, const char *value)
{
  char line[ASK_LINE_SZ];

  snprintf(line, sizeof(line), "%*s%-*s: %s", ASK_CARD_INDENT, "",
      ASK_CARD_LABEL, label, value);
  cmd_reply(ctx, line);
}

// A card line whose value is one raw string: fit, then colour. Never the
// other way round — display_fit measures columns and a colour marker is
// two bytes of none (include/display.h).
static void
ask_card_text(const cmd_ctx_t *ctx, const char *label, const char *color,
    const char *text)
{
  char cell [ASK_CARD_CELL_SZ];
  char value[ASK_CMD_REPLY_SZ];

  display_fit(text, ASK_CARD_VALUE_COLS - 1, cell, sizeof(cell), "…");
  snprintf(value, sizeof(value), "%s%s" CLR_RESET, color, cell);
  ask_card(ctx, label, value);
}

// What the card needs out of the registry, gathered in the single pass
// llm_model_iterate gives us.
//
// ⚠ The callback below runs under the registry's read lock. It touches
// neither the engine nor KV — both take locks of their own, and the one
// that matters here is the read lock a nested llm_model_exists would take
// recursively. Everything else this card prints is read after the walk.
typedef struct
{
  const ask_scope_t *scope;
  bool               found;                          // default is registered
  char               service [ASK_CARD_FIELD_SZ];
  char               model_id[ASK_CARD_FIELD_SZ];
  char               allowed [ASK_CARD_ALLOWED_SZ];  // space-separated names
  uint32_t           n_allowed;
} ask_card_state_t;

static void
ask_card_model_cb(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user)
{
  ask_card_state_t  *st = (ask_card_state_t *)user;
  const ask_scope_t *s  = st->scope;

  (void)embed_dim;
  (void)max_context;
  (void)default_temp;

  if(!st->found && s->def_model.value != NULL
      && strcasecmp(name, s->def_model.value) == 0)
  {
    st->found = true;
    strlcpy(st->service,  service_name != NULL ? service_name : "",
        sizeof st->service);
    strlcpy(st->model_id, model_id != NULL ? model_id : "",
        sizeof st->model_id);
  }

  if(kind != LLM_KIND_CHAT || !enabled || !ask_allowed(name, s))
    return;

  st->n_allowed++;

  if(st->allowed[0] != '\0')
    strlcat(st->allowed, " ", sizeof st->allowed);

  strlcat(st->allowed, name, sizeof st->allowed);
}

// `model` — the name, the id it stands for, and the service that serves
// it. An unregistered default is the case worth spelling out: !ask falls
// through to llm.default_chat_model without a word, which is how a
// deleted gf35 stayed configured on three bots for months.
static void
ask_card_model(const cmd_ctx_t *ctx, const ask_card_state_t *st,
    const char *model)
{
  char cell [ASK_CARD_FIELD_SZ];
  char value[ASK_CMD_REPLY_SZ];
  int  left;

  if(model == NULL)
  {
    ask_card(ctx, "model", CLR_RED "(none configured)" CLR_RESET);
    return;
  }

  if(!st->found)
  {
    snprintf(value, sizeof(value), CLR_WHITE "%s" CLR_RESET "   " CLR_YELLOW
        "⚠ not registered — !ask will fall back" CLR_RESET, model);
    ask_card(ctx, "model", value);
    return;
  }

  // The id is the elastic cell and pays for whatever the name and the
  // service spend; three spaces and the separator are the rest.
  left = ASK_CARD_VALUE_COLS - (int)display_vis_len(model) - 3 - 5
      - (int)display_vis_len(st->service);

  display_fit(st->model_id, left > 8 ? left : 8, cell, sizeof(cell), "…");

  snprintf(value, sizeof(value), CLR_WHITE "%s" CLR_RESET "   " CLR_GRAY
      "%s" CLR_RESET "  ·  " CLR_CYAN "%s" CLR_RESET, model, cell,
      st->service);
  ask_card(ctx, "model", value);
}

// `accepts` — the model's declared effort set, and the warning that comes
// with a set refusing `none`: omitting the field on a thinking-only model
// returns a 200 with empty content, which is the failure that went
// unnoticed for eleven hours. One derivation, two renderers — the test is
// llm_effort_set_admits, never a second copy of it.
static void
ask_card_accepts(const cmd_ctx_t *ctx, const char *model)
{
  char        key[KV_KEY_SZ];
  char        set[ASK_CARD_EFFORTS_SZ];
  char        value[ASK_CMD_REPLY_SZ];
  const char *csv;

  snprintf(key, sizeof(key), "llm.model.%s.efforts", model);
  csv = kv_get_str(key);

  // ⚠ Absent means UNDECLARED, which admits everything. A reader shown a
  // blank draws the opposite conclusion (include/kv.h, the read-site rule).
  if(csv == NULL || csv[0] == '\0')
  {
    ask_card(ctx, "accepts", CLR_GRAY "undeclared — nobody has measured this"
        " model, not \"none accepted\"" CLR_RESET);
    return;
  }

  // Space-separated so it reads as a set rather than a CSV to paste back,
  // and otherwise verbatim: an unrecognised token is the operator's typo,
  // and swallowing it here hides the refusal `!ask -e` will hand back.
  strlcpy(set, csv, sizeof set);

  for(char *p = set; *p != '\0'; p++)
    if(*p == ',')
      *p = ' ';

  snprintf(value, sizeof(value), CLR_WHITE "%s" CLR_RESET "%s", set,
      llm_effort_set_admits(csv, LLM_EFFORT_NONE) ? ""
          : "   " CLR_YELLOW "⚠ thinking-only" CLR_RESET);
  ask_card(ctx, "accepts", value);
}

static void
show_ask_handler(const cmd_ctx_t *ctx)
{
  ask_card_state_t  st;
  ask_scope_t       scope;
  const char       *bot_name;
  const char       *proto;
  const char       *model;
  char              value[ASK_CMD_REPLY_SZ];

  bot_name = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  proto    = ask_proto(ctx);
  ask_scope_resolve(bot_name, proto, &scope);

  memset(&st, 0, sizeof(st));
  st.scope = &scope;
  llm_model_iterate(ask_card_model_cb, &st);

  model = scope.def_model.value;

  cmd_reply(ctx, CLR_BOLD "ask" CLR_RESET
      "  ·  one-shot LLM query, no memory");

  ask_card_model(ctx, &st, model);

  if(model != NULL)
    ask_card_text(ctx, "from", CLR_GRAY, scope.def_model.key);

  // ⛔ No value when nothing is set: printing one the plugin would not
  // send is worse than printing none. The service's own
  // llm.service.<name>.reasoning_effort decides in that case.
  if(scope.def_effort.value == NULL)
    ask_card(ctx, "effort", CLR_GRAY "unset — the service decides" CLR_RESET);

  else
  {
    snprintf(value, sizeof(value), CLR_WHITE "%s" CLR_RESET "   " CLR_GRAY
        "from %s" CLR_RESET, scope.def_effort.value, scope.def_effort.key);
    ask_card(ctx, "effort", value);
  }

  if(st.found)
    ask_card_accepts(ctx, model);

  // Ceilings a caller hits without ever being told the number.
  snprintf(value, sizeof(value), "%u tokens  ·  %u lines  ·  %u columns",
      scope.max_tokens, scope.max_lines, scope.max_cols);
  ask_card(ctx, "limits", value);

  // The constraint, not a catalogue. With every tier open there is nothing
  // here a caller does not already have from `!show llm models chat`, so
  // the line says nothing at all — which is the behaviour that makes it
  // worth having the day an operator narrows one.
  if(!ask_tier_open(scope.allow_plugin) || !ask_tier_open(scope.allow_bot)
      || !ask_tier_open(scope.allow_proto))
  {
    if(st.n_allowed == 0)
      ask_card(ctx, "allowed", CLR_RED "(nothing — every tier excludes"
          " every model)" CLR_RESET);
    else
      ask_card_text(ctx, "allowed", CLR_WHITE, st.allowed);
  }

  ask_card_text(ctx, "others", CLR_CYAN, "!show llm models chat");
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static const char ask_cmd_help[] =
    "Ask a chat model a single question and print the answer.\n"
    "\n"
    "  ask <query>          one-shot query against the bot's default model\n"
    "  a <query>            short alias\n"
    "  ask -m <model> ...   pick a specific model from the allowlist\n"
    "  ask -e <effort> ...  how hard that model thinks before answering\n"
    "  show ask             the request !ask would make on this bot\n"
    "\n"
    "Either flag may appear anywhere in the line; everything left over is\n"
    "the query, so a question may safely begin with a dash.\n"
    "\n"
    "Effort is one of none, minimal, low, medium, high, xhigh — but each\n"
    "model declares which of those it accepts, and the set is NOT a range:\n"
    "a model offering low and high does not thereby offer medium. Read the\n"
    "set before you are refused by it:\n"
    "\n"
    "  show ask               the set for the model THIS bot would use\n"
    "  show llm models chat   every model, on its own \"thinking:\" line\n"
    "\n"
    "A model with no thinking line has declared nothing, which admits\n"
    "every effort — it does not refuse them all.\n"
    "\n"
    "Answers are STATELESS — no memory is kept and no conversation\n"
    "history is carried between calls (this is not the chat bot).\n"
    "\n"
    "Examples:\n"
    "  !ask why is the sky blue\n"
    "  !a -m gfll summarize the CAP theorem\n"
    "  !ask -e high derive the CAP theorem from first principles\n"
    "  !show ask";

static const cmd_decl_t ask_decl = {
  .module      = ASK_CMD_CTX,
  .name        = "ask",
  .usage       = "ask [-m <model>] [-e <effort>] <query>",
  .description = "One-shot LLM query (stateless, no memory)",
  .help_long   = ask_cmd_help,
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = ask_cmd_handler,
  .abbrev      = "a",
};

static const cmd_decl_t show_ask_decl = {
  .module      = ASK_CMD_CTX,
  .name        = "ask",
  .usage       = "show ask",
  .description = "Report the model, effort and limits !ask would use here",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = show_ask_handler,
  .parent_path = "show",
};

static bool
ask_cmd_init(void)
{
  if(cmd_register(&ask_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&show_ask_decl) != SUCCESS)
  {
    cmd_unregister_path("ask");
    return(FAIL);
  }

  // Grow every bot / bound protocol its own ask.* override tiers. Core
  // back-fills existing bots immediately, so a hot-reload re-attaches the
  // keys. The plugin_desc pointer is the unregister cookie.
  bot_kv_contributor_register(ask_kv_bot_cb, ask_kv_proto_cb,
      (void *)&ask_kv_cookie);

  clam(CLAM_INFO, ASK_CMD_CTX, "ask command plugin initialized");
  return(SUCCESS);
}

static void
ask_cmd_deinit(void)
{
  bot_kv_contributor_unregister((void *)&ask_kv_cookie);

  cmd_unregister_path("ask");
  cmd_unregister_path("show/ask");
  clam(CLAM_INFO, ASK_CMD_CTX, "ask command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "ask_cmd",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "ask",
  .provides        = { { .name = "cmd_ask" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "bot_chat" },
    { .name = "inference" },
  },
  .requires_count  = 2,
  .kv_schema       = ask_kv_schema,
  .kv_schema_count = sizeof(ask_kv_schema) / sizeof(ask_kv_schema[0]),
  .init            = ask_cmd_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = ask_cmd_deinit,
  .ext             = NULL,
};
