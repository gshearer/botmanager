// botmanager — MIT
// imagine command-surface plugin: public, stateless text-to-image.
//
//   !imagine <prompt>          generate against the bot's default image model
//   !ig <prompt>               short alias
//   !imagine -m <model> ...    pick a specific model from the per-bot allowlist
//   !show imagine              queue status and the image models available here
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

// -----------------------------------------------------------------------
// KV schema — registered once under plugin.imagine.*
// -----------------------------------------------------------------------

// The plugin tier is the ROOT of a three-level resolution
// (plugin -> bot -> bot.method) for the policy knobs (default model,
// allowlist, image size, style prepend). Hosting and queue shape are
// plugin-global infrastructure and are not contributed per-bot: a fair-use
// quota that differed per bot would not be one.
static const plugin_kv_entry_t imagine_kv_schema[] = {
  { "plugin.imagine.default",     KV_STR,    "",
    "Default image model for !imagine when no -m is given" },
  { "plugin.imagine.allow",       KV_STR,    "*",
    "Absolute allowlist of exposable image-model names ('*' = all enabled);"
    " bot / method tiers may only narrow this" },
  { "plugin.imagine.size",        KV_STR,    "",
    "Requested image dimensions (e.g. 1024x1024); empty asks the provider"
    " for its own default; bot / method tiers may override" },
  { "plugin.imagine.output_dir",  KV_STR,    "",
    "Local filesystem directory (web-served) that generated images are"
    " written to" },
  { "plugin.imagine.public_base", KV_STR,    "",
    "Public URL base that maps to output_dir (e.g. https://host/images/ig)" },
  { "plugin.imagine.max_inflight", KV_UINT32, "1",
    "Image generations running concurrently; the rest wait in the queue" },
  { "plugin.imagine.max_queue",   KV_UINT32, "16",
    "Requests waiting behind the in-flight ones before !imagine refuses"
    " (0 = unbounded)" },
  { "plugin.imagine.max_per_user", KV_UINT32, "1",
    "Queue entries one authenticated user may hold at once, in flight"
    " included" },
  { "plugin.imagine.max_per_anonymous", KV_UINT32, "1",
    "Queue entries shared by ALL unauthenticated callers, on every method,"
    " in flight included" },
  { "plugin.imagine.prompt_prepend_file", KV_STR, "",
    "Path to a .txt file whose contents are prepended (as a style preamble)"
    " to every !imagine prompt; bot / method tiers may override" },
};

// -----------------------------------------------------------------------
// Per-bot / per-(bot,method) KV contributors
// -----------------------------------------------------------------------
//
// Registered with core (bot_kv_contributor_register) so every bot instance
// and every bound method grows its own imagine.* override keys, mirroring
// the ask plugin. Only the policy knobs cascade; hosting and queue shape
// stay plugin-global.

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
imagine_kv_method_cb(const char *botname, const char *method_kind, void *user)
{
  char        key[KV_KEY_SZ];
  const char *inherit;
  char        def[KV_STR_SZ];

  (void)user;

  snprintf(key, sizeof(key), "bot.%s.imagine.default", botname);
  inherit = kv_get_str(key);
  snprintf(def, sizeof(def), "%s", inherit != NULL ? inherit : "");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.default", botname, method_kind);
  kv_register(key, KV_STR, def, NULL, NULL,
      "Per-method !imagine default model (empty inherits the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.allow", botname, method_kind);
  kv_register(key, KV_STR, "*", NULL, NULL,
      "Per-method !imagine allowlist ('*' = inherit; else narrows the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.size", botname, method_kind);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-method image size for !imagine (empty inherits the bot tier)");

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.prompt_prepend_file",
      botname, method_kind);
  kv_register(key, KV_STR, "", NULL, NULL,
      "Per-method style-prepend path for !imagine (empty inherits)");
}

// -----------------------------------------------------------------------
// Resolution helpers
// -----------------------------------------------------------------------

// The method kind (e.g. "irc") a command arrived on, or NULL.
static const char *
imagine_method(const cmd_ctx_t *ctx)
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

// Read bot.<bot>.<method>.imagine.<suffix>, or NULL if either scope is absent.
static const char *
imagine_kv_method(const char *bot_name, const char *method_kind,
    const char *suffix)
{
  char key[KV_KEY_SZ];

  if(bot_name == NULL || method_kind == NULL)
    return(NULL);

  snprintf(key, sizeof(key), "bot.%s.%s.imagine.%s", bot_name, method_kind,
      suffix);
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

// Populate `s` for a request on (bot_name, method_kind). Any tier may be absent.
static void
imagine_scope_resolve(const char *bot_name, const char *method_kind,
    imagine_scope_t *s)
{
  memset(s, 0, sizeof(*s));

  s->def_model = imagine_first_nonempty(
      imagine_kv_method(bot_name, method_kind, "default"),
      imagine_kv_bot(bot_name, "default"),
      kv_get_str("plugin.imagine.default"));

  s->allow_plugin = kv_get_str("plugin.imagine.allow");
  s->allow_bot    = imagine_kv_bot(bot_name, "allow");
  s->allow_method = imagine_kv_method(bot_name, method_kind, "allow");

  s->size = imagine_first_nonempty(
      imagine_kv_method(bot_name, method_kind, "size"),
      imagine_kv_bot(bot_name, "size"),
      kv_get_str("plugin.imagine.size"));
}

// Resolve the effective style-prepend path (most-specific non-empty) and
// slurp it into `buf` (NUL-terminated, truncated to `cap`, trailing
// newlines trimmed). Returns SUCCESS with buf populated, else FAIL with
// buf[0] == '\0'. Read fresh each call so file edits take effect live.
static bool
imagine_read_prepend(const char *bot_name, const char *method_kind,
    char *buf, size_t cap)
{
  const char *path;
  FILE       *fp;
  size_t      n;

  if(cap == 0)
    return(FAIL);

  buf[0] = '\0';

  path = imagine_first_nonempty(
      imagine_kv_method(bot_name, method_kind, "prompt_prepend_file"),
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

  return(imagine_tier_admits(s->allow_plugin,  model) &&
         imagine_tier_admits(s->allow_bot,     model) &&
         imagine_tier_admits(s->allow_method,  model));
}

// -----------------------------------------------------------------------
// The queue
// -----------------------------------------------------------------------
//
// Two lists under one lock: a pending FIFO, and the set of requests
// currently in flight. A request is in exactly one of them from the moment
// the handler enqueues it until its completion frees it, which is what
// makes a per-caller quota simply "count your nodes in both".
//
// Nothing here ever blocks a caller. The handler appends and pumps; the
// pump submits while there are free slots; the engine's curl worker calls
// imagine_done, which replies, unlinks, and pumps again. img_lock is never
// held across a cmd_reply, a KV read, or a submit.

static pthread_mutex_t img_lock      = PTHREAD_MUTEX_INITIALIZER;
static img_req_t      *img_queue_head = NULL;   // next to submit
static img_req_t      *img_queue_tail = NULL;
static img_req_t      *img_active     = NULL;   // in-flight set (unordered)
static uint32_t        img_depth      = 0;      // queued, excluding in flight
static uint32_t        img_active_n   = 0;      // in flight
static uint32_t        img_id_seq     = 0;      // monotonic within a busy period

// A KV knob that must never be zero, since zero would stall the pump or
// deny every caller.
static uint32_t
imagine_kv_count(const char *key, uint32_t fallback)
{
  uint32_t v = (uint32_t)kv_get_uint(key);

  return(v > 0 ? v : fallback);
}

// Reply to the user who made request r. r->ctx.msg already points at the
// embedded method_msg copy, so this is valid on any thread once the
// originating dispatch frame is gone.
static void
imagine_reply(const img_req_t *r, const char *text)
{
  cmd_reply(&r->ctx, text);
}

// The queue identity of a caller: their authenticated username, or the one
// shared anonymous bucket. Callers must copy the result — for the anonymous
// case it is a string literal, for the other it is ctx-lifetime.
static const char *
imagine_owner_of(const cmd_ctx_t *ctx)
{
  if(ctx->username != NULL && ctx->username[0] != '\0')
    return(ctx->username);

  return(IMG_ANON_OWNER);
}

// How many entries `owner` holds across both lists. Caller holds img_lock.
static uint32_t
imagine_owner_count_locked(const char *owner)
{
  uint32_t n = 0;

  for(const img_req_t *r = img_active; r != NULL; r = r->next)
    if(strcasecmp(r->owner, owner) == 0)
      n++;

  for(const img_req_t *r = img_queue_head; r != NULL; r = r->next)
    if(strcasecmp(r->owner, owner) == 0)
      n++;

  return(n);
}

// Unlink r from the in-flight set. Caller must NOT hold img_lock.
static void
imagine_active_drop(img_req_t *r)
{
  img_req_t **link;

  pthread_mutex_lock(&img_lock);

  for(link = &img_active; *link != NULL; link = &(*link)->next)
    if(*link == r)
    {
      *link = r->next;
      r->next = NULL;
      img_active_n--;
      break;
    }

  pthread_mutex_unlock(&img_lock);
}

// Hand r to the inference engine. On SUCCESS the request owns r and
// imagine_done fires exactly once on a curl worker — r must not be touched
// again here. On FAIL no callback will ever fire and r is still ours.
static bool
imagine_submit_one(img_req_t *r)
{
  llm_image_params_t params;

  params      = (llm_image_params_t){ 0 };
  params.n    = 1;
  params.size = (r->size[0] != '\0') ? r->size : NULL;

  r->begin = time(NULL);

  // llm_image_submit uses SUCCESS(=false)/FAIL(=true); a plain
  // `!llm_image_submit(...)` inverts the test and would free r out from
  // under a live request.
  if(llm_image_submit(r->model, &params, r->prompt, imagine_done, r)
      != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Fill every free in-flight slot from the FIFO. Safe to call from any
// thread with img_lock unheld; a request that fails to submit synchronously
// is reported, dropped, and the drain continues.
static void
imagine_pump(void)
{
  for(;;)
  {
    img_req_t *r;
    uint32_t   max_inflight;
    char       line[IMG_CMD_REPLY_SZ];

    // Read the knob outside the lock: KV has a lock of its own and this
    // one is never held across another.
    max_inflight = imagine_kv_count("plugin.imagine.max_inflight", 1);

    pthread_mutex_lock(&img_lock);

    if(img_queue_head == NULL || img_active_n >= max_inflight)
    {
      // Fully idle: restart numbering so a quiet channel sees #1 again
      // rather than #4711.
      if(img_queue_head == NULL && img_active_n == 0)
        img_id_seq = 0;

      pthread_mutex_unlock(&img_lock);
      return;
    }

    r              = img_queue_head;
    img_queue_head = r->next;

    if(img_queue_head == NULL)
      img_queue_tail = NULL;

    img_depth--;

    r->next    = img_active;
    img_active = r;
    img_active_n++;

    pthread_mutex_unlock(&img_lock);

    if(imagine_submit_one(r) == SUCCESS)
      continue;                        // in flight; keep filling slots

    snprintf(line, sizeof(line),
        "imagine #%u: could not submit to the model", r->id);
    imagine_reply(r, line);
    imagine_active_drop(r);
    mem_free(r);
  }
}

// -----------------------------------------------------------------------
// Completion
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

// The served file extension, decided by the bytes themselves.
//
// The response MIME cannot be trusted here: the OpenAI image schema has no
// field for it, so the engine reports a flat "image/png" for every
// provider — and Gemini answers this endpoint with JPEG. Naming a JPEG
// .png leaves browsers to sniff their way out of our mistake, so we sniff
// first and let the magic number name the file. `mime` is consulted only
// when the payload is something none of these signatures cover.
static const char *
imagine_ext_for(const void *bytes, size_t len, const char *mime)
{
  const unsigned char *b = bytes;

  if(len >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
    return("png");

  if(len >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    return("jpg");

  if(len >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
    return("webp");

  if(len >= 6 && memcmp(b, "GIF8", 4) == 0)
    return("gif");

  if(mime != NULL && (strstr(mime, "jpeg") != NULL
      || strstr(mime, "jpg") != NULL))
    return("jpg");

  if(mime != NULL && strstr(mime, "webp") != NULL)
    return("webp");

  return("png");
}

// Runs on a curl worker thread: decode the base64 image, persist it, reply
// with the public URL, then release the slot and pump the next request.
static void
imagine_done(const llm_image_response_t *resp)
{
  img_req_t *r = (img_req_t *)resp->user_data;
  char       line[IMG_CMD_REPLY_SZ];
  char       filename[IMG_FILENAME_SZ];
  uuid_t     uu;
  char       uuid_str[USERNS_UUID_SZ];
  void      *bytes;
  size_t     decoded = 0;
  long       elapsed;

  elapsed = (long)(time(NULL) - r->begin);

  if(!resp->ok || resp->b64 == NULL || resp->b64_len == 0)
  {
    snprintf(line, sizeof(line), "imagine #%u failed: %s", r->id,
        resp->error != NULL ? resp->error : "no image returned");
    imagine_reply(r, line);
    goto cleanup;
  }

  // Decode into a buffer sized at the base64 length — the decoded payload
  // is always ~3/4 of that, so this comfortably fits.
  bytes = mem_alloc(IMG_CMD_CTX, "img", resp->b64_len);

  if(util_b64_decode(resp->b64, resp->b64_len, bytes, resp->b64_len,
      &decoded) != SUCCESS || decoded == 0)
  {
    snprintf(line, sizeof(line),
        "imagine #%u: could not decode the returned image", r->id);
    imagine_reply(r, line);
    mem_free(bytes);
    goto cleanup;
  }

  uuid_generate(uu);
  uuid_unparse_lower(uu, uuid_str);
  snprintf(filename, sizeof(filename), "imagine-%s.%s", uuid_str,
      imagine_ext_for(bytes, decoded, resp->mime));

  if(imagine_write_file(r->output_dir, filename, bytes, decoded) != SUCCESS)
  {
    snprintf(line, sizeof(line),
        "imagine #%u: could not save the generated image", r->id);
    imagine_reply(r, line);
    mem_free(bytes);
    goto cleanup;
  }

  mem_free(bytes);

  {
    // The URL alone can outrun a status line, so it gets a buffer sized
    // for the configured base rather than the reply width.
    char url[IMG_URL_LINE_SZ];

    snprintf(url, sizeof(url), "[#%u %lds] %s/%s",
        r->id, elapsed, r->public_base, filename);
    imagine_reply(r, url);
  }

cleanup:
  imagine_active_drop(r);
  mem_free(r);
  imagine_pump();
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
// Command handlers
// -----------------------------------------------------------------------

static void
imagine_cmd_handler(const cmd_ctx_t *ctx)
{
  img_req_t      *r;
  const char     *bot_name;
  const char     *method_kind;
  const char     *model;
  const char     *output_dir;
  const char     *public_base;
  imagine_scope_t scope;
  char            picked[IMG_MODEL_SZ];
  char            prompt[IMG_PROMPT_SZ];
  char            prepend[IMG_PREPEND_SZ];
  char            reply[IMG_CMD_REPLY_SZ];
  bool            have_model;
  bool            anon;
  uint32_t        quota;
  uint32_t        held;
  uint32_t        max_queue;
  uint32_t        max_inflight;
  uint32_t        ahead = 0;
  bool            waiting;

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

  bot_name    = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  method_kind = imagine_method(ctx);
  imagine_scope_resolve(bot_name, method_kind, &scope);
  model       = have_model ? picked : scope.def_model;

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

  // Fold an optional style preamble into the prompt (most-specific-first).
  // A truncated prompt beats a clobbered buffer, so snprintf caps it.
  if(imagine_read_prepend(bot_name, method_kind, prepend, sizeof(prepend))
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

  snprintf(r->owner,       sizeof(r->owner),       "%s", imagine_owner_of(ctx));
  snprintf(r->model,       sizeof(r->model),       "%s", model);
  snprintf(r->prompt,      sizeof(r->prompt),      "%s", prompt);
  snprintf(r->output_dir,  sizeof(r->output_dir),  "%s", output_dir);
  snprintf(r->public_base, sizeof(r->public_base), "%s", public_base);

  if(scope.size != NULL)
    snprintf(r->size, sizeof(r->size), "%s", scope.size);

  // Every knob this request needs is read before the lock is taken: KV has
  // a lock of its own, and img_lock is never held across it.
  anon         = (strcasecmp(r->owner, IMG_ANON_OWNER) == 0);
  quota        = imagine_kv_count(anon ? "plugin.imagine.max_per_anonymous"
                                       : "plugin.imagine.max_per_user", 1);
  max_queue    = (uint32_t)kv_get_uint("plugin.imagine.max_queue");
  max_inflight = imagine_kv_count("plugin.imagine.max_inflight", 1);

  pthread_mutex_lock(&img_lock);

  held = imagine_owner_count_locked(r->owner);

  if(held >= quota)
  {
    pthread_mutex_unlock(&img_lock);

    if(anon)
      snprintf(reply, sizeof(reply),
          "imagine: %u image%s already running for unidentified users — "
          "identify yourself for your own slot, or try again shortly",
          quota, quota == 1 ? " is" : "s are");
    else
      snprintf(reply, sizeof(reply),
          "imagine: you already have %u image%s in the queue — "
          "let %s finish first",
          quota, quota == 1 ? "" : "s", quota == 1 ? "it" : "them");

    cmd_reply(ctx, reply);
    mem_free(r);
    return;
  }

  if(max_queue > 0 && img_depth >= max_queue)
  {
    pthread_mutex_unlock(&img_lock);
    cmd_reply(ctx, "imagine: too many images queued right now — "
        "try again shortly");
    mem_free(r);
    return;
  }

  r->id = ++img_id_seq;

  if(img_queue_tail != NULL)
    img_queue_tail->next = r;
  else
    img_queue_head = r;

  img_queue_tail = r;
  img_depth++;

  // Everything already committed has to finish before this one starts.
  waiting = (img_active_n + img_depth) > max_inflight;

  if(waiting)
    ahead = img_active_n + img_depth - 1;

  pthread_mutex_unlock(&img_lock);

  if(waiting)
  {
    snprintf(reply, sizeof(reply), "imagine: queued as #%u (%u ahead)",
        r->id, ahead);
    cmd_reply(ctx, reply);
  }

  // r may complete and be freed inside this call — do not touch it after.
  imagine_pump();
}

// -----------------------------------------------------------------------
// !show imagine — queue first, then the model menu
// -----------------------------------------------------------------------

// Copy the first IMG_PREVIEW_CHARS bytes of prompt into out, flattening
// control characters to spaces (prompts are single-line here) and appending
// an ellipsis when the prompt was longer than the window. ASCII-oriented: a
// truncation may land mid-UTF-8, which at worst garbles one preview glyph —
// acceptable for a status line.
static void
imagine_prompt_preview(const char *prompt, char *out, size_t out_sz)
{
  size_t i;

  for(i = 0; i < IMG_PREVIEW_CHARS && prompt[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)prompt[i];

    out[i] = (c < 0x20) ? ' ' : (char)c;
  }

  if(prompt[i] != '\0' && i + 4 <= out_sz)
  {
    memcpy(out + i, "\xe2\x80\xa6", 3);   // U+2026 HORIZONTAL ELLIPSIS
    i += 3;
  }

  out[i] = '\0';
}

static void
imagine_snap(const img_req_t *r, img_snap_t *out)
{
  out->id = r->id;
  snprintf(out->owner, sizeof(out->owner), "%s", r->owner);
  imagine_prompt_preview(r->prompt, out->text, sizeof(out->text));
}

// Per-model visitor: collect one row per enabled image model the calling
// bot may reach.
static void
imagine_model_cb(const char *name, llm_kind_t kind, const char *service_name,
    const char *model_id, uint32_t embed_dim, uint32_t max_context,
    float default_temp, bool enabled, void *user)
{
  img_model_state_t *s = (img_model_state_t *)user;
  img_model_row_t   *row;

  (void)embed_dim;
  (void)max_context;
  (void)default_temp;

  if(kind != LLM_KIND_IMAGE || !enabled)
    return;

  if(!imagine_model_ok(name, s->scope))
    return;

  if(s->n_rows >= IMG_SHOW_MAX)
    return;

  row = &s->rows[s->n_rows++];

  snprintf(row->name,     sizeof(row->name),     "%s", name);
  snprintf(row->service,  sizeof(row->service),  "%s",
      service_name != NULL ? service_name : "");
  snprintf(row->model_id, sizeof(row->model_id), "%s",
      model_id != NULL ? model_id : "");

  row->is_def = (s->scope->def_model != NULL &&
                 strcasecmp(name, s->scope->def_model) == 0);
}

static void
show_imagine_models(const cmd_ctx_t *ctx, const imagine_scope_t *scope)
{
  img_model_state_t s;
  size_t            w_svc  = strlen("service");
  size_t            w_name = strlen("model");
  char              line[IMG_CMD_REPLY_SZ];

  s.scope  = scope;
  s.n_rows = 0;

  llm_model_iterate(imagine_model_cb, &s);

  if(s.n_rows == 0)
  {
    cmd_reply(ctx, "  " CLR_GRAY "(no image models available here)" CLR_RESET);
    return;
  }

  for(size_t i = 0; i < s.n_rows; i++)
  {
    size_t l;

    l = strlen(s.rows[i].service);
    if(l > w_svc)
      w_svc = l;

    l = strlen(s.rows[i].name);
    if(l > w_name)
      w_name = l;
  }

  snprintf(line, sizeof(line), "   " CLR_BOLD "%-*s  %-*s  %s" CLR_RESET,
      (int)w_svc, "service", (int)w_name, "model", "model id");
  cmd_reply(ctx, line);

  for(size_t i = 0; i < s.n_rows; i++)
  {
    const img_model_row_t *row = &s.rows[i];

    snprintf(line, sizeof(line),
        " %s " CLR_CYAN "%-*s" CLR_RESET "  " CLR_WHITE "%-*s" CLR_RESET
        "  " CLR_GRAY "%s" CLR_RESET,
        row->is_def ? CLR_YELLOW "★" CLR_RESET : " ",
        (int)w_svc, row->service,
        (int)w_name, row->name,
        row->model_id);
    cmd_reply(ctx, line);
  }
}

static void
show_imagine_handler(const cmd_ctx_t *ctx)
{
  imagine_scope_t scope;
  const char     *bot_name;
  const char     *method_kind;
  img_snap_t      active[IMG_SHOW_MAX];
  img_snap_t      queued[IMG_SHOW_MAX];
  uint32_t        n_active = 0;
  uint32_t        n_queued = 0;
  uint32_t        depth;
  uint32_t        running;
  uint32_t        max_inflight;
  char            line[IMG_CMD_REPLY_SZ];

  bot_name     = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : NULL;
  method_kind  = imagine_method(ctx);
  max_inflight = imagine_kv_count("plugin.imagine.max_inflight", 1);
  imagine_scope_resolve(bot_name, method_kind, &scope);

  // Snapshot under the lock, emit after releasing it — cmd_reply re-enters
  // the delivery path, so the mutex is never held across a send.
  pthread_mutex_lock(&img_lock);

  running = img_active_n;
  depth   = img_depth;

  for(const img_req_t *r = img_active; r != NULL && n_active < IMG_SHOW_MAX;
      r = r->next)
    imagine_snap(r, &active[n_active++]);

  for(const img_req_t *r = img_queue_head; r != NULL && n_queued < IMG_SHOW_MAX;
      r = r->next)
    imagine_snap(r, &queued[n_queued++]);

  pthread_mutex_unlock(&img_lock);

  cmd_reply(ctx, CLR_BOLD "imagine" CLR_RESET
      "  ·  text-to-image over the inference engine");

  snprintf(line, sizeof(line), "  queue   : %s (%u/%u running, %u waiting)",
      running > 0 ? CLR_YELLOW "busy" CLR_RESET : "idle",
      running, max_inflight, depth);
  cmd_reply(ctx, line);

  for(uint32_t i = 0; i < n_active; i++)
  {
    snprintf(line, sizeof(line), "  render  : " CLR_GREEN "#%u" CLR_RESET
        " %s " CLR_GRAY "(%s)" CLR_RESET,
        active[i].id, active[i].text, active[i].owner);
    cmd_reply(ctx, line);
  }

  for(uint32_t i = 0; i < n_queued; i++)
  {
    snprintf(line, sizeof(line), "    %2u. " CLR_GRAY "#%u" CLR_RESET
        " %s " CLR_GRAY "(%s)" CLR_RESET,
        i + 1, queued[i].id, queued[i].text, queued[i].owner);
    cmd_reply(ctx, line);
  }

  if(depth > n_queued)
  {
    snprintf(line, sizeof(line), "    " CLR_GRAY "… +%u more" CLR_RESET,
        depth - n_queued);
    cmd_reply(ctx, line);
  }

  show_imagine_models(ctx, &scope);
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
    "  show imagine              queue status and the models available here\n"
    "\n"
    "The bot hosts the result: the image is written to a web-served\n"
    "directory and the reply is a link to it. Requires an operator to have\n"
    "configured plugin.imagine.output_dir and plugin.imagine.public_base.\n"
    "\n"
    "Renders run a few at a time and the rest queue, each tagged with an\n"
    "id. You may hold one queue entry at a time; everyone who has not\n"
    "identified shares a single entry between them.\n"
    "\n"
    "Examples:\n"
    "  !imagine a red panda in a chef hat\n"
    "  !ig -m nanoflash a neon city skyline at dusk\n"
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
      "Show the !imagine queue and the image models available here", NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      show_imagine_handler, NULL, "show", NULL, NULL, 0, NULL, NULL)
      != SUCCESS)
  {
    cmd_unregister_path("imagine");
    return(FAIL);
  }

  bot_kv_contributor_register(imagine_kv_bot_cb, imagine_kv_method_cb,
      (void *)&imagine_kv_cookie);

  clam(CLAM_INFO, IMG_CMD_CTX, "imagine command plugin initialized");
  return(SUCCESS);
}

static void
imagine_cmd_deinit(void)
{
  img_req_t *r;

  bot_kv_contributor_unregister((void *)&imagine_kv_cookie);

  cmd_unregister_path("imagine");
  cmd_unregister_path("show/imagine");

  // Drop the queued (not-yet-submitted) closures we still own. An in-flight
  // request is owned by the engine: its callback frees it, and the engine's
  // unmap listener NULLs that callback when this mapping goes away — so the
  // in-flight set is deliberately left alone here. Freeing it would race the
  // window between this deinit and the unload, where a completion can still
  // land in live code holding a freed request.
  pthread_mutex_lock(&img_lock);

  r = img_queue_head;

  while(r != NULL)
  {
    img_req_t *next = r->next;

    mem_free(r);
    r = next;
  }

  img_queue_head = NULL;
  img_queue_tail = NULL;
  img_depth      = 0;
  pthread_mutex_unlock(&img_lock);

  clam(CLAM_INFO, IMG_CMD_CTX, "imagine command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "imagine_cmd",
  .version         = "2.0",
  .type            = PLUGIN_MISC,
  .kind            = "imagine",
  .provides        = { { .name = "cmd_imagine" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "bot_chat" },
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
