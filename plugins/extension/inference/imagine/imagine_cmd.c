// botmanager — MIT
// imagine command-surface plugin: public, stateless text-to-image.
//
//   !imagine <prompt>          generate against the bot's default image model
//   !ig <prompt>               short alias
//   !imagine -m <model> ...    pick a specific model from the per-bot allowlist
//   !show imagine              list the image models available on this bot
//
// The bot owns hosting: the image model is asked for base64 bytes, which
// this command decodes, writes into a KV-configured web directory
// (plugin.imagine.output_dir), and serves back as a public URL
// (plugin.imagine.public_base). Model exposure is governed by a per-bot
// allowlist (`bot.<name>.imagine.allow`, inheriting `plugin.imagine.allow`);
// only image-kind models are ever selectable.
#define IMAGINE_CMD_INTERNAL
#include "imagine_cmd.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#include "colors.h"
#include "bot.h"        // bot_inst_name

// Concurrency guard: image generation is slow and GPU-bound, so we cap the
// number of in-flight requests (plugin.imagine.max_inflight). A reserved
// slot is released either on submit failure or in the async completion.
static uint32_t img_inflight = 0;

// -----------------------------------------------------------------------
// KV schema — registered once under plugin.imagine.*
// -----------------------------------------------------------------------

// The plugin tier is the ROOT of a three-level resolution
// (plugin -> bot -> bot.protocol) for the policy knobs (default model,
// allowlist, image size, style prepend). The hosting knobs (output_dir,
// public_base, max_inflight) are plugin-global infrastructure and are not
// contributed per-bot.
static const plugin_kv_entry_t imagine_kv_schema[] = {
  { "plugin.imagine.default",     KV_STR,    "",
    "Default image model for !imagine when no -m is given" },
  { "plugin.imagine.allow",       KV_STR,    "*",
    "Absolute allowlist of exposable image-model names ('*' = all enabled);"
    " bot / protocol tiers may only narrow this" },
  { "plugin.imagine.size",        KV_STR,    "1024x1024",
    "Requested image dimensions (e.g. 1024x1024); bot / protocol tiers may"
    " override" },
  { "plugin.imagine.output_dir",  KV_STR,    "",
    "Local filesystem directory (web-served) that generated images are"
    " written to" },
  { "plugin.imagine.public_base", KV_STR,    "",
    "Public URL base that maps to output_dir (e.g. https://host/images/ig)" },
  { "plugin.imagine.max_inflight", KV_UINT32, "1",
    "Maximum concurrent image generations before !imagine politely rejects" },
  // Path is relative to the daemon CWD (build/), so reach up to the
  // project-root prompts/ dir; bot / protocol tiers may override.
  { "plugin.imagine.prompt_prepend_file", KV_STR, "",
    "Path to a .txt file whose contents are prepended (as a style preamble)"
    " to every !imagine prompt; bot / protocol tiers may override" },
};

// -----------------------------------------------------------------------
// Per-bot / per-(bot,protocol) KV contributors
// -----------------------------------------------------------------------
//
// Registered with core (bot_kv_contributor_register) so every bot instance
// and every bound protocol grows its own imagine.* override keys, mirroring
// the ask plugin. Only the policy knobs cascade; hosting infrastructure
// stays plugin-global.

// Stable per-plugin address used as the KV-contributor registration cookie.
static const char imagine_kv_cookie;

static void
imagine_kv_bot_cb(const char *botname, void *user)
{
  char        key[KV_KEY_SZ];
  const char *inherit;
  char        def[KV_STR_SZ];

  (void)user;

  inherit = kv_get_str("plugin.imagine.default");
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.imagine.default", botname);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-bot !imagine default model (empty inherits plugin.imagine.default)");

  snprintf(key, sizeof(key), "bot.%s.imagine.allow", botname);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-bot !imagine allowlist ('*' = inherit; else narrows plugin tier)");

  snprintf(key, sizeof(key), "bot.%s.imagine.size", botname);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-bot image size for !imagine (empty inherits plugin.imagine.size)");

  snprintf(key, sizeof(key), "bot.%s.imagine.prompt_prepend_file", botname);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-bot style-prepend path for !imagine (empty inherits plugin default)");
}

static void
imagine_kv_proto_cb(const char *botname, const char *protocol, void *user)
{
  char        key[KV_KEY_SZ];
  const char *inherit;
  char        def[KV_STR_SZ];

  (void)user;

  snprintf(key, sizeof(key), "bot.%s.imagine.default", botname);
  inherit = kv_get_str(key);
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.default", botname, protocol);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-protocol !imagine default model (empty inherits the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.allow", botname, protocol);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-protocol !imagine allowlist ('*' = inherit; else narrows the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.size", botname, protocol);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-protocol image size for !imagine (empty inherits the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.prompt_prepend_file",
      botname, protocol);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-protocol style-prepend path for !imagine (empty inherits)");
}

// -----------------------------------------------------------------------
// Resolution helpers
// -----------------------------------------------------------------------

// The protocol (method kind, e.g. "irc") a command arrived on, or NULL.
static const char *
imagine_proto(const cmd_ctx_t *ctx)
{
  if(ctx->msg != NULL && ctx->msg->inst != NULL)
    return(method_inst_kind(ctx->msg->inst));

  return(NULL);
}

// Read bot.<bot>.imagine.<suffix>, or NULL if bot_name is absent.
static const char *
imagine_kv_bot(const char *bot_name, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(bot_name == NULL)
    return(NULL);

  snprintf(key, sizeof(key), "bot.%s.imagine.%s", bot_name, suffix);
  return(kv_get_str(key));
}

// Read bot.<bot>.<proto>.imagine.<suffix>, or NULL if either scope is absent.
static const char *
imagine_kv_proto(const char *bot_name, const char *proto, const char *suffix)
{
  char key[KV_KEY_SZ];

  if(bot_name == NULL || proto == NULL)
    return(NULL);

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.%s", bot_name, proto, suffix);
  return(kv_get_str(key));
}

// First non-empty of the most-specific-first candidates, or NULL.
static const char *
imagine_first_nonempty(const char *a, const char *b, const char *c)
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
// allowlist tiers (each a membership filter), and the effective size.
typedef struct
{
  const char *def_model;     // most-specific non-empty, or NULL
  const char *allow_plugin;  // absolute list
  const char *allow_bot;     // narrows plugin (NULL/empty/"*" = no-op)
  const char *allow_proto;   // narrows bot    (NULL/empty/"*" = no-op)
  const char *size;          // most-specific non-empty, or NULL
} imagine_scope_t;

// True iff `model` is a whole comma/space-separated token of `csv`.
static bool
imagine_csv_contains(const char *csv, const char *model)
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
imagine_tier_admits(const char *csv, const char *model)
{
  if(csv == NULL || csv[0] == '\0' || strcmp(csv, "*") == 0)
    return(true);

  return(imagine_csv_contains(csv, model));
}

// Populate `s` for a request on (bot_name, proto). Any tier may be absent.
static void
imagine_scope_resolve(const char *bot_name, const char *proto,
    imagine_scope_t *s)
{
  memset(s, 0, sizeof(*s));

  s->def_model = imagine_first_nonempty(
      imagine_kv_proto(bot_name, proto, "default"),
      imagine_kv_bot(bot_name, "default"),
      kv_get_str("plugin.imagine.default"));

  s->allow_plugin = kv_get_str("plugin.imagine.allow");
  s->allow_bot    = imagine_kv_bot(bot_name, "allow");
  s->allow_proto  = imagine_kv_proto(bot_name, proto, "allow");

  s->size = imagine_first_nonempty(
      imagine_kv_proto(bot_name, proto, "size"),
      imagine_kv_bot(bot_name, "size"),
      kv_get_str("plugin.imagine.size"));
}

// Resolve the effective style-prepend path (most-specific non-empty) and
// slurp it into `buf` (NUL-terminated, truncated to `cap`, trailing
// newlines trimmed). Returns SUCCESS with buf populated, else FAIL with
// buf[0] == '\0'. Read fresh each call so file edits take effect live.
static bool
imagine_read_prepend(const char *bot_name, const char *proto,
    char *buf, size_t cap)
{
  const char *path;
  FILE       *fp;
  size_t      n;

  if(cap == 0)
    return(FAIL);

  buf[0] = '\0';

  path = imagine_first_nonempty(
      imagine_kv_proto(bot_name, proto, "prompt_prepend_file"),
      imagine_kv_bot(bot_name, "prompt_prepend_file"),
      kv_get_str("plugin.imagine.prompt_prepend_file"));

  if(path == NULL)
    return(FAIL);

  fp = fopen(path, "r");

  if(fp == NULL)
  {
    clam(CLAM_DEBUG, IMG_CMD_CTX, "prepend file unreadable: '%s'", path);
    return(FAIL);
  }

  n = fread(buf, 1, cap - 1, fp);
  fclose(fp);
  buf[n] = '\0';

  while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = '\0';

  return(n > 0 ? SUCCESS : FAIL);
}

// Gate a model for !imagine: it must exist, be an image model (chat/embed
// models stay unreachable even under "*"), and either be the resolved
// default (always self-allowed) or survive the intersection of all present
// allow tiers.
static bool
imagine_model_ok(const char *model, const imagine_scope_t *s)
{
  llm_kind_t kind;

  if(model == NULL || model[0] == '\0')
    return(false);

  if(!llm_model_exists(model))
    return(false);

  // llm_model_kind uses SUCCESS(=false)/FAIL(=true), unlike llm_model_exists
  // which returns a natural bool — compare against SUCCESS explicitly.
  if(llm_model_kind(model, &kind) != SUCCESS || kind != LLM_KIND_IMAGE)
    return(false);

  if(s->def_model != NULL && strcasecmp(model, s->def_model) == 0)
    return(true);

  return(imagine_tier_admits(s->allow_plugin, model) &&
         imagine_tier_admits(s->allow_bot,    model) &&
         imagine_tier_admits(s->allow_proto,  model));
}

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

// Append one whitespace-separated token to the prompt buffer with a single
// leading space before every token but the first. Silently drops tokens
// that would overflow.
static void
imagine_prompt_append(char *prompt, size_t cap, size_t *len, const char *tok)
{
  int wrote;

  if(*len >= cap)
    return;

  wrote = snprintf(prompt + *len, cap - *len, "%s%s",
      *len > 0 ? " " : "", tok);

  if(wrote > 0)
    *len += (size_t)wrote < cap - *len ? (size_t)wrote : cap - *len - 1;
}

// Split the raw argument string into an optional "-m <model>" selection and
// the residual prompt. "-m" is consumed on its first occurrence; a trailing
// "-m" with no following token is dropped.
static void
imagine_parse_flags(const char *args, char *model, size_t model_cap,
    char *prompt, size_t prompt_cap, bool *have_model)
{
  char   scratch[METHOD_TEXT_SZ];
  char  *save;
  size_t plen = 0;

  snprintf(scratch, sizeof(scratch), "%s", args);
  model[0]    = '\0';
  prompt[0]   = '\0';
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

    imagine_prompt_append(prompt, prompt_cap, &plen, tok);
  }
}

// -----------------------------------------------------------------------
// Async completion
// -----------------------------------------------------------------------

// Write `len` bytes to <dir>/<filename>. Returns SUCCESS or FAIL.
static bool
imagine_write_file(const char *dir, const char *filename,
    const void *bytes, size_t len)
{
  char   path[IMG_FULLPATH_SZ];
  FILE  *fp;
  size_t wrote;

  snprintf(path, sizeof(path), "%s/%s", dir, filename);

  fp = fopen(path, "wb");

  if(fp == NULL)
  {
    clam(CLAM_WARN, IMG_CMD_CTX, "cannot open '%s' for write", path);
    return(FAIL);
  }

  wrote = fwrite(bytes, 1, len, fp);

  if(fclose(fp) != 0 || wrote != len)
  {
    clam(CLAM_WARN, IMG_CMD_CTX, "short write to '%s' (%zu/%zu)",
        path, wrote, len);
    return(FAIL);
  }

  return(SUCCESS);
}

// Runs on a curl worker thread: decode the base64 image, persist it, reply
// with the public URL, then release the in-flight slot and the closure.
static void
imagine_done(const llm_image_response_t *resp)
{
  img_req_t *r = (img_req_t *)resp->user_data;
  cmd_ctx_t  ctx;
  char       line[IMG_CMD_REPLY_SZ];
  char       filename[IMG_FILENAME_SZ];
  uuid_t     uu;
  char       uuid_str[USERNS_UUID_SZ];
  void      *bytes;
  size_t     decoded = 0;

  ctx     = r->ctx;
  ctx.msg = &r->msg;

  if(!resp->ok || resp->b64 == NULL || resp->b64_len == 0)
  {
    snprintf(line, sizeof(line), "imagine: %s",
        resp->error != NULL ? resp->error : "no image returned");
    cmd_reply(&ctx, line);
    goto cleanup;
  }

  // Decode into a buffer sized at the base64 length — the decoded payload is
  // always ~3/4 of that, so this comfortably fits.
  bytes = mem_alloc(IMG_CMD_CTX, "png", resp->b64_len);

  if(util_b64_decode(resp->b64, resp->b64_len, bytes, resp->b64_len,
      &decoded) != SUCCESS || decoded == 0)
  {
    cmd_reply(&ctx, "imagine: could not decode the returned image");
    mem_free(bytes);
    goto cleanup;
  }

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);
  snprintf(filename, sizeof(filename), "imagine-%s.png", uuid_str);

  if(imagine_write_file(r->output_dir, filename, bytes, decoded) != SUCCESS)
  {
    cmd_reply(&ctx, "imagine: could not save the generated image");
    mem_free(bytes);
    goto cleanup;
  }

  mem_free(bytes);

  {
    char url[IMG_FULLPATH_SZ];

    snprintf(url, sizeof(url), "%s/%s", r->public_base, filename);
    cmd_reply(&ctx, url);
  }

cleanup:
  __atomic_sub_fetch(&img_inflight, 1, __ATOMIC_ACQ_REL);
  mem_free(r);
}

// -----------------------------------------------------------------------
// Command handlers
// -----------------------------------------------------------------------

static void
imagine_cmd_handler(const cmd_ctx_t *ctx)
{
  img_req_t          *r;
  const char         *bot_name;
  const char         *proto;
  const char         *model;
  const char         *output_dir;
  const char         *public_base;
  imagine_scope_t     scope;
  char                picked[128];
  char                prompt[IMG_PROMPT_SZ];
  char                prepend[IMG_PREPEND_SZ];
  char                reply[IMG_CMD_REPLY_SZ];
  bool                have_model;
  uint32_t            max_inflight;
  uint32_t            reserved;
  llm_image_params_t  params;

  static const char usage[] =
      "Usage: imagine [-m <model>] <prompt>  ·  !show imagine lists models";

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  imagine_parse_flags(ctx->args, picked, sizeof(picked),
      prompt, sizeof(prompt), &have_model);

  if(prompt[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  bot_name = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  proto    = imagine_proto(ctx);
  imagine_scope_resolve(bot_name, proto, &scope);
  model    = have_model ? picked : scope.def_model;

  if(!imagine_model_ok(model, &scope))
  {
    snprintf(reply, sizeof(reply),
        "unknown or unavailable model '%s' — try !show imagine",
        (model != NULL && model[0] != '\0') ? model : "(none)");
    cmd_reply(ctx, reply);
    return;
  }

  // Hosting must be configured for a URL to be servable.
  output_dir  = kv_get_str("plugin.imagine.output_dir");
  public_base = kv_get_str("plugin.imagine.public_base");

  if(output_dir == NULL || output_dir[0] == '\0'
      || public_base == NULL || public_base[0] == '\0')
  {
    cmd_reply(ctx, "imagine: image hosting is not configured "
        "(set plugin.imagine.output_dir and plugin.imagine.public_base)");
    return;
  }

  // Reserve an in-flight slot; reject politely when the cap is reached.
  max_inflight = (uint32_t)kv_get_uint("plugin.imagine.max_inflight");
  if(max_inflight == 0)
    max_inflight = 1;

  reserved = __atomic_add_fetch(&img_inflight, 1, __ATOMIC_ACQ_REL);

  if(reserved > max_inflight)
  {
    __atomic_sub_fetch(&img_inflight, 1, __ATOMIC_ACQ_REL);
    cmd_reply(ctx, "imagine: busy generating images right now — "
        "please try again shortly");
    return;
  }

  // Fold an optional style preamble into the prompt (most-specific-first).
  // A truncated prompt beats a clobbered buffer, so snprintf caps it.
  if(imagine_read_prepend(bot_name, proto, prepend, sizeof(prepend))
      == SUCCESS)
  {
    char merged[IMG_PROMPT_SZ];

    snprintf(merged, sizeof(merged), "%s %s", prepend, prompt);
    snprintf(prompt, sizeof(prompt), "%s", merged);
  }

  r = mem_alloc(IMG_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  snprintf(r->output_dir,  sizeof(r->output_dir),  "%s", output_dir);
  snprintf(r->public_base, sizeof(r->public_base), "%s", public_base);

  params      = (llm_image_params_t){ 0 };
  params.n    = 1;
  params.size = (scope.size != NULL && scope.size[0] != '\0')
      ? scope.size : NULL;

  // llm_image_submit uses SUCCESS(=false)/FAIL(=true): on a successful
  // enqueue the async request OWNS r and frees it (and releases the slot)
  // via imagine_done. Compare against SUCCESS explicitly — a plain
  // `!llm_image_submit(...)` inverts and would free r out from under the
  // in-flight request.
  if(llm_image_submit(model, &params, prompt, imagine_done, r) != SUCCESS)
  {
    __atomic_sub_fetch(&img_inflight, 1, __ATOMIC_ACQ_REL);
    cmd_reply(ctx, "imagine: failed to submit request (model unavailable?)");
    mem_free(r);
  }
}

// Per-model visitor for !show imagine. Collects one row per enabled image
// model the calling bot may reach.

#define IMG_SHOW_MAX_ROWS 64
#define IMG_SHOW_FIELD_SZ 96

typedef struct
{
  char name[IMG_SHOW_FIELD_SZ];
  char service[IMG_SHOW_FIELD_SZ];
  char model_id[IMG_SHOW_FIELD_SZ];
  bool is_def;
} img_show_row_t;

typedef struct
{
  const cmd_ctx_t       *ctx;
  const imagine_scope_t *scope;
  img_show_row_t         rows[IMG_SHOW_MAX_ROWS];
  size_t                 n_rows;
} img_show_state_t;

static void
imagine_field_copy(char *dst, size_t dst_sz, const char *src)
{
  if(src == NULL)
    src = "";

  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static void
imagine_show_model_cb(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id,
    uint32_t embed_dim, uint32_t max_context, float default_temp,
    bool enabled, void *user)
{
  img_show_state_t *s = (img_show_state_t *)user;
  img_show_row_t   *r;

  (void)embed_dim;
  (void)max_context;
  (void)default_temp;

  if(kind != LLM_KIND_IMAGE || !enabled)
    return;

  if(!imagine_model_ok(name, s->scope))
    return;

  if(s->n_rows >= IMG_SHOW_MAX_ROWS)
    return;

  r = &s->rows[s->n_rows++];

  imagine_field_copy(r->name, sizeof(r->name), name);
  imagine_field_copy(r->service, sizeof(r->service), service_name);
  imagine_field_copy(r->model_id, sizeof(r->model_id), model_id);

  r->is_def = (s->scope->def_model != NULL &&
               strcasecmp(name, s->scope->def_model) == 0);
}

static void
show_imagine_emit(const img_show_state_t *s)
{
  const cmd_ctx_t *ctx    = s->ctx;
  size_t           w_svc  = strlen("service");
  size_t           w_name = strlen("model");
  char             line[IMG_CMD_REPLY_SZ];

  if(s->n_rows == 0)
  {
    cmd_reply(ctx, "  " CLR_GRAY "(no image models available here)" CLR_RESET);
    return;
  }

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

  snprintf(line, sizeof(line),
      "   " CLR_BOLD "%-*s  %-*s  %s" CLR_RESET,
      (int)w_svc, "service", (int)w_name, "model", "model id");
  cmd_reply(ctx, line);

  for(size_t i = 0; i < s->n_rows; i++)
  {
    const img_show_row_t *r = &s->rows[i];

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
show_imagine_handler(const cmd_ctx_t *ctx)
{
  img_show_state_t s;
  imagine_scope_t  scope;
  const char      *bot_name;
  const char      *proto;

  bot_name = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  proto    = imagine_proto(ctx);
  imagine_scope_resolve(bot_name, proto, &scope);

  s.ctx    = ctx;
  s.scope  = &scope;
  s.n_rows = 0;

  cmd_reply(ctx, CLR_BOLD "imagine" CLR_RESET "  ·  image models available here");
  llm_model_iterate(imagine_show_model_cb, &s);
  show_imagine_emit(&s);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static const char imagine_cmd_help[] =
    "Generate an image from a text prompt and print a public URL.\n"
    "\n"
    "  imagine <prompt>          generate with the bot's default image model\n"
    "  ig <prompt>               short alias\n"
    "  imagine -m <model> ...    pick a specific model from the allowlist\n"
    "  show imagine              list the image models available on this bot\n"
    "\n"
    "The bot hosts the result: the image is written to a web-served\n"
    "directory and the reply is a link to it. Requires an operator to have\n"
    "configured plugin.imagine.output_dir and plugin.imagine.public_base.\n"
    "\n"
    "Examples:\n"
    "  !imagine a red panda in a chef hat\n"
    "  !ig -m sdxl a neon city skyline at dusk\n"
    "  !show imagine";

static bool
imagine_cmd_init(void)
{
  if(cmd_register(IMG_CMD_CTX, "imagine", "imagine [-m <model>] <prompt>",
      "Text-to-image generation (stateless)", imagine_cmd_help,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      imagine_cmd_handler, NULL, NULL, "ig", NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(IMG_CMD_CTX, "imagine", "show imagine",
      "List image models available to !imagine on this bot", NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      show_imagine_handler, NULL, "show", NULL, NULL, 0, NULL, NULL)
      != SUCCESS)
  {
    cmd_unregister_path("imagine");
    return(FAIL);
  }

  bot_kv_contributor_register(imagine_kv_bot_cb, imagine_kv_proto_cb,
      (void *)&imagine_kv_cookie);

  clam(CLAM_INFO, IMG_CMD_CTX, "imagine command plugin initialized");
  return(SUCCESS);
}

static void
imagine_cmd_deinit(void)
{
  bot_kv_contributor_unregister((void *)&imagine_kv_cookie);

  cmd_unregister_path("imagine");
  cmd_unregister_path("show/imagine");
  clam(CLAM_INFO, IMG_CMD_CTX, "imagine command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "imagine_cmd",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "imagine",
  .provides        = { { .name = "cmd_imagine" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "method_text" },
    { .name = "inference" },
  },
  .requires_count  = 2,
  .kv_schema       = imagine_kv_schema,
  .kv_schema_count = sizeof(imagine_kv_schema) / sizeof(imagine_kv_schema[0]),
  .init            = imagine_cmd_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = imagine_cmd_deinit,
  .ext             = NULL,
};
