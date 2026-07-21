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

static const plugin_kv_entry_t ask_kv_schema[] = {
  { "plugin.ask.model",      KV_STR,    "gemini-flash3",
    "Model used by !ask when no -m is given" },
  { "plugin.ask.allow",      KV_STR,    "*",
    "CSV allowlist of exposable chat-model names ('*' = all enabled)" },
  { "plugin.ask.max_tokens", KV_UINT32, "512",
    "max_tokens sent with each !ask request" },
  { "plugin.ask.max_lines",  KV_UINT32, "6",
    "Reply-line cap per answer (flood guard)" },
  { "plugin.ask.system",     KV_STR,
    "Answer concisely and factually. This is for an IRC channel; keep it short.",
    "One-shot system prompt prepended to every !ask query" },
};

// -----------------------------------------------------------------------
// Resolution helpers
// -----------------------------------------------------------------------

// Effective allowlist CSV for bot `bot_name`: the per-bot override if
// set, else the global default, else the literal "*" (all). Returns a
// pointer into KV storage or a string literal — never NULL.
static const char *
ask_allow_csv(const char *bot_name)
{
  char        key[128];
  const char *v = NULL;

  if(bot_name != NULL)
  {
    snprintf(key, sizeof(key), "bot.%s.ask.allow", bot_name);
    v = kv_get_str(key);
  }

  if(v == NULL || v[0] == '\0')
    v = kv_get_str("plugin.ask.allow");

  if(v == NULL || v[0] == '\0')
    v = "*";

  return(v);
}

// Effective default model for bot `bot_name`: per-bot override if set,
// else the global default. May be NULL/empty in a misconfigured
// deployment — callers treat that as "no default".
static const char *
ask_def_model(const char *bot_name)
{
  char        key[128];
  const char *v = NULL;

  if(bot_name != NULL)
  {
    snprintf(key, sizeof(key), "bot.%s.ask.default", bot_name);
    v = kv_get_str(key);
  }

  if(v == NULL || v[0] == '\0')
    v = kv_get_str("plugin.ask.model");

  return(v);
}

// True iff `model` appears as a whole comma/space-separated token in the
// allowlist CSV, matched case-insensitively. `csv` is left unmodified.
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

// Gate a model for !ask: it must exist, be a chat model (embed models
// stay unreachable even under "*"), and be either the resolved default
// (always self-allowed) or present in the allowlist.
static bool
ask_model_ok(const char *model, const char *def_model, const char *allow_csv)
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

  if(def_model != NULL && strcasecmp(model, def_model) == 0)
    return(true);

  if(strcmp(allow_csv, "*") == 0)
    return(true);

  return(ask_csv_contains(allow_csv, model));
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
  uint32_t    emitted = 0;

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

  max_lines = (uint32_t)kv_get_uint("plugin.ask.max_lines");
  if(max_lines == 0)
    max_lines = 1;

  for(p = resp->content; p != NULL && *p != '\0'; )
  {
    const char *nl  = strchr(p, '\n');
    size_t      seg = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

    // Strip a trailing CR (provider CRLF).
    if(seg > 0 && p[seg - 1] == '\r')
      seg--;

    if(seg > 0)
    {
      if(emitted >= max_lines)
      {
        cmd_reply(&ctx, "…(truncated)");
        break;
      }

      snprintf(line, sizeof(line), "%.*s", (int)seg, p);
      cmd_reply(&ctx, line);
      emitted++;
    }

    if(nl == NULL)
      break;

    p = nl + 1;
  }

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
  const char        *def_model;
  const char        *allow_csv;
  const char        *model;
  char               picked[128];
  char               query[METHOD_TEXT_SZ];
  char               reply[ASK_CMD_REPLY_SZ];
  bool               have_model;
  llm_message_t      msgs[2];
  size_t             n;
  const char        *sys;
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

  bot_name  = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  def_model = ask_def_model(bot_name);
  allow_csv = ask_allow_csv(bot_name);
  model     = have_model ? picked : def_model;

  if(!ask_model_ok(model, def_model, allow_csv))
  {
    snprintf(reply, sizeof(reply),
        "unknown or unavailable model '%s' — try !show ask",
        (model != NULL && model[0] != '\0') ? model : "(none)");
    cmd_reply(ctx, reply);
    return;
  }

  r = mem_alloc(ASK_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  memset(msgs, 0, sizeof(msgs));   // header: blocks ptr must be zeroed
  n   = 0;
  sys = kv_get_str("plugin.ask.system");

  if(sys != NULL && sys[0] != '\0')
  {
    msgs[n].role    = LLM_ROLE_SYSTEM;
    msgs[n].content = sys;
    n++;
  }

  msgs[n].role    = LLM_ROLE_USER;
  msgs[n].content = query;
  n++;

  p            = (llm_chat_params_t){ 0 };
  p.max_tokens = (uint32_t)kv_get_uint("plugin.ask.max_tokens");

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

// Per-model visitor for !show ask. Renders one row per enabled chat
// model the calling bot may reach, starring the default.
typedef struct
{
  const cmd_ctx_t *ctx;
  const char      *def_model;
  const char      *allow_csv;
} ask_show_state_t;

static void
ask_show_model_cb(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user)
{
  ask_show_state_t *s = (ask_show_state_t *)user;
  char              line[ASK_CMD_REPLY_SZ];
  bool              is_def;

  (void)model_id;
  (void)embed_dim;
  (void)max_context;
  (void)default_temp;

  if(kind != LLM_KIND_CHAT || !enabled)
    return;

  if(!ask_model_ok(name, s->def_model, s->allow_csv))
    return;

  is_def = (s->def_model != NULL && strcasecmp(name, s->def_model) == 0);

  snprintf(line, sizeof(line), "  %s %s  " CLR_GRAY "%s" CLR_RESET "%s",
      is_def ? CLR_YELLOW "★" CLR_RESET : "  ",
      name, service_name,
      is_def ? "  " CLR_GRAY "(default)" CLR_RESET : "");
  cmd_reply(s->ctx, line);
}

static void
show_ask_handler(const cmd_ctx_t *ctx)
{
  ask_show_state_t s;
  const char      *bot_name;

  bot_name    = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  s.ctx       = ctx;
  s.def_model = ask_def_model(bot_name);
  s.allow_csv = ask_allow_csv(bot_name);

  cmd_reply(ctx, CLR_BOLD "ask" CLR_RESET "  ·  models available here");
  llm_model_iterate(ask_show_model_cb, &s);
  cmd_reply(ctx, "  -m <model> to pick · answers are one-shot, no memory");
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
    cmd_unregister("ask");
    return(FAIL);
  }

  clam(CLAM_INFO, ASK_CMD_CTX, "ask command plugin initialized");
  return(SUCCESS);
}

static void
ask_cmd_deinit(void)
{
  // Two nodes share the name "ask" (root command + show child). Each
  // cmd_unregister removes the first match in the global list, so two
  // calls clear both.
  cmd_unregister("ask");
  cmd_unregister("ask");
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
    { .name = "method_command" },
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
