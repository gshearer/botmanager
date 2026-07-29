// botmanager — MIT
// ask command-surface plugin: a public, stateless one-shot LLM query.
//
//   !ask <query>          one prompt against the bot's default chat model
//   !a <query>            short alias
//   !ask -m <model> ...   pick a specific model from the per-bot allowlist
//   !show ask             list the models available to !ask on this bot
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
  { "plugin.ask.default",    KV_STR,    "gemini-flash3",
    "Default chat model for !ask when no -m is given" },
  { "plugin.ask.allow",      KV_STR,    "*",
    "Absolute allowlist of exposable chat-model names ('*' = all enabled);"
    " bot / protocol tiers may only narrow this" },
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
  snprintf(key, sizeof(key), "bot.%s.ask.default", botname);
  inherit = kv_get_str(key);
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.default", botname, protocol);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-protocol !ask default model (empty inherits the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.ask.allow", botname, protocol);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-protocol !ask allowlist ('*' = inherit; else narrows the bot tier)");

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

// Resolved per-request scope: the effective default model, the three
// allowlist tiers (each a membership filter), and the reply ceilings.
// Built once per command and threaded through the gate + reply loop.
typedef struct
{
  const char *def_model;     // most-specific non-empty, or NULL
  const char *allow_plugin;  // absolute list
  const char *allow_bot;     // narrows plugin (NULL/empty/"*" = no-op)
  const char *allow_proto;   // narrows bot    (NULL/empty/"*" = no-op)
  uint32_t    max_lines;     // min across present tiers
  uint32_t    max_tokens;    // min across present tiers
  uint32_t    max_cols;      // min across present tiers, buffer-clamped
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

// A single allowlist tier admits `model` when it imposes no restriction
// (unset / empty / "*") or explicitly lists it.
static bool
ask_tier_admits(const char *csv, const char *model)
{
  if(csv == NULL || csv[0] == '\0' || strcmp(csv, "*") == 0)
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

  s->def_model = ask_first_nonempty(
      ask_kv_proto(bot_name, proto, "default"),
      ask_kv_bot(bot_name, "default"),
      kv_get_str("plugin.ask.default"));

  s->allow_plugin = kv_get_str("plugin.ask.allow");
  s->allow_bot    = ask_kv_bot(bot_name, "allow");
  s->allow_proto  = ask_kv_proto(bot_name, proto, "allow");

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

  // The resolved default is always reachable, even if the allowlists omit
  // it — a bot can always run its own configured default.
  if(s->def_model != NULL && strcasecmp(model, s->def_model) == 0)
    return(true);

  // Intersection: every present tier must admit the model.
  return(ask_tier_admits(s->allow_plugin, model) &&
         ask_tier_admits(s->allow_bot,    model) &&
         ask_tier_admits(s->allow_proto,  model));
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

// Split the raw argument string into an optional "-m <model>" selection
// and the residual query. "-m" is consumed on its first occurrence; a
// trailing "-m" with no following token is dropped.
static void
ask_parse_flags(const char *args, char *model, size_t model_cap,
    char *query, size_t query_cap, bool *have_model)
{
  char   scratch[METHOD_TEXT_SZ];
  char  *save;
  size_t qlen = 0;

  snprintf(scratch, sizeof(scratch), "%s", args);
  model[0]    = '\0';
  query[0]    = '\0';
  *have_model = false;

  for(char *tok = strtok_r(scratch, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    if(!*have_model && strcmp(tok, "-m") == 0)
    {
      char *m = strtok_r(NULL, " \t", &save);

      if(m == NULL)
        continue;                    // trailing "-m" with no value: drop

      snprintf(model, model_cap, "%s", m);
      *have_model = true;
      continue;
    }

    ask_query_append(query, query_cap, &qlen, tok);
  }
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
  char        line[ASK_CMD_REPLY_SZ];
  const char *p;
  uint32_t    max_lines;
  size_t      max_cols;
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

  for(p = resp->content; p != NULL && *p != '\0' && !flood_capped; )
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
      size_t take = seg - off;

      if(take > max_cols)
      {
        size_t brk = max_cols;

        // Prefer the last space in the column window; hard-break a single
        // over-long word when there is none.
        while(brk > 0 && p[off + brk] != ' ')
          brk--;

        take = (brk > 0) ? brk : max_cols;
      }

      // Never split a UTF-8 multibyte sequence on a hard break: back off
      // while the byte at the break point is a continuation byte.
      while(take > 1 && ((unsigned char)p[off + take] & 0xC0) == 0x80)
        take--;

      if(emitted >= max_lines)
      {
        cmd_reply(&ctx, "…(truncated)");
        flood_capped = true;
        break;
      }

      snprintf(line, sizeof(line), "%.*s", (int)take, p + off);
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
  ask_scope_t        scope;
  char               picked[128];
  char               query[METHOD_TEXT_SZ];
  char               reply[ASK_CMD_REPLY_SZ];
  char               prepend[ASK_PREPEND_SZ];
  bool               have_model;
  llm_message_t      msgs[2];
  size_t             n;
  llm_chat_params_t  p;

  static const char usage[] =
      "Usage: ask [-m <model>] <query>  ·  !show ask lists models";

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  ask_parse_flags(ctx->args, picked, sizeof(picked),
      query, sizeof(query), &have_model);

  if(query[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  bot_name = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  proto    = ask_proto(ctx);
  ask_scope_resolve(bot_name, proto, &scope);
  model    = have_model ? picked : scope.def_model;

  if(!ask_model_ok(model, &scope))
  {
    snprintf(reply, sizeof(reply),
        "unknown or unavailable model '%s' — try !show ask",
        (model != NULL && model[0] != '\0') ? model : "(none)");
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

// Per-model visitor for !show ask. Collects one row per enabled chat
// model the calling bot may reach; show_ask_handler emits them as an
// aligned, colorized table, starring the default.

#define ASK_SHOW_MAX_ROWS 64
#define ASK_SHOW_FIELD_SZ 96

typedef struct
{
  char name[ASK_SHOW_FIELD_SZ];      // arbitrary model name
  char service[ASK_SHOW_FIELD_SZ];   // arbitrary service name
  char model_id[ASK_SHOW_FIELD_SZ];  // full underlying model id
  bool is_def;
} ask_show_row_t;

typedef struct
{
  const cmd_ctx_t   *ctx;
  const ask_scope_t *scope;
  ask_show_row_t     rows[ASK_SHOW_MAX_ROWS];
  size_t             n_rows;
} ask_show_state_t;

static void
ask_field_copy(char *dst, size_t dst_sz, const char *src)
{
  if(src == NULL)
    src = "";

  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static void
ask_show_model_cb(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user)
{
  ask_show_state_t *s = (ask_show_state_t *)user;
  ask_show_row_t   *r;

  (void)embed_dim;
  (void)max_context;
  (void)default_temp;

  if(kind != LLM_KIND_CHAT || !enabled)
    return;

  if(!ask_model_ok(name, s->scope))
    return;

  if(s->n_rows >= ASK_SHOW_MAX_ROWS)
    return;

  r = &s->rows[s->n_rows++];

  ask_field_copy(r->name, sizeof(r->name), name);
  ask_field_copy(r->service, sizeof(r->service), service_name);
  ask_field_copy(r->model_id, sizeof(r->model_id), model_id);

  r->is_def = (s->scope->def_model != NULL &&
               strcasecmp(name, s->scope->def_model) == 0);
}

static void
show_ask_emit(const ask_show_state_t *s)
{
  const cmd_ctx_t *ctx    = s->ctx;
  size_t           w_svc  = strlen("service");
  size_t           w_name = strlen("model");
  char             line[ASK_CMD_REPLY_SZ];

  if(s->n_rows == 0)
  {
    cmd_reply(ctx, "  " CLR_GRAY "(no models available here)" CLR_RESET);
    return;
  }

  // Widen columns to the longest cell (header labels included).
  for(size_t i = 0; i < s->n_rows; i++)
  {
    size_t l;

    l = strlen(s->rows[i].service);
    if(l > w_svc)
      w_svc = l;

    l = strlen(s->rows[i].name);
    if(l > w_name)
      w_name = l;
  }

  // Header row (three leading spaces align past the star column).
  snprintf(line, sizeof(line),
      "   " CLR_BOLD "%-*s  %-*s  %s" CLR_RESET,
      (int)w_svc, "service", (int)w_name, "model", "model id");
  cmd_reply(ctx, line);

  for(size_t i = 0; i < s->n_rows; i++)
  {
    const ask_show_row_t *r = &s->rows[i];

    snprintf(line, sizeof(line),
        " %s " CLR_CYAN "%-*s" CLR_RESET "  " CLR_WHITE "%-*s" CLR_RESET
        "  " CLR_GRAY "%s" CLR_RESET,
        r->is_def ? CLR_YELLOW "★" CLR_RESET : " ",
        (int)w_svc, r->service,
        (int)w_name, r->name,
        r->model_id);
    cmd_reply(ctx, line);
  }
}

static void
show_ask_handler(const cmd_ctx_t *ctx)
{
  ask_show_state_t s;
  ask_scope_t      scope;
  const char      *bot_name;
  const char      *proto;

  bot_name = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  proto    = ask_proto(ctx);
  ask_scope_resolve(bot_name, proto, &scope);

  s.ctx    = ctx;
  s.scope  = &scope;
  s.n_rows = 0;

  cmd_reply(ctx, CLR_BOLD "ask" CLR_RESET "  ·  models available here");
  llm_model_iterate(ask_show_model_cb, &s);
  show_ask_emit(&s);
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
    "  show ask             list the models available on this bot\n"
    "\n"
    "Answers are STATELESS — no memory is kept and no conversation\n"
    "history is carried between calls (this is not the chat bot).\n"
    "\n"
    "Examples:\n"
    "  !ask why is the sky blue\n"
    "  !a -m gemini-flash3 summarize the CAP theorem\n"
    "  !show ask";

static bool
ask_cmd_init(void)
{
  if(cmd_register(ASK_CMD_CTX, "ask", "ask [-m <model>] <query>",
      "One-shot LLM query (stateless, no memory)", ask_cmd_help,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      ask_cmd_handler, NULL, NULL, "a", NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(ASK_CMD_CTX, "ask", "show ask",
      "List chat models available to !ask on this bot", NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      show_ask_handler, NULL, "show", NULL, NULL, 0, NULL, NULL) != SUCCESS)
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
    { .name = "method_text" },
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
