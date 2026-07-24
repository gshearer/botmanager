// botmanager — MIT
// Temporary text-to-image command backed by the local zimage-service.
#define IMAGINE_ZIMAGE_INTERNAL
#include "imagine_zimage.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

// -----------------------------------------------------------------------
// KV schema — flat plugin.imagine.* (no per-bot tiers; single backend)
// -----------------------------------------------------------------------

static const plugin_kv_entry_t iz_kv_schema[] = {
  { "plugin.imagine.service_url", KV_STR, "",
    "zimage-service /generate endpoint that renders a prompt"
    " (e.g. http://127.0.0.1:8001/generate)" },
  { "plugin.imagine.public_base", KV_STR, "",
    "Public URL base the service's image directory is served under"
    " (e.g. https://host/images/ig)" },
  { "plugin.imagine.auth_header", KV_STR, "",
    "Optional auth header sent to the service, as 'name: value' (empty = none)" },
  { "plugin.imagine.timeout", KV_UINT32, "180",
    "Seconds to wait for one image render before giving up" },
  { "plugin.imagine.max_queue", KV_UINT32, "16",
    "Maximum requests waiting behind the in-flight one before !imagine rejects"
    " (0 = unbounded)" },
};

// -----------------------------------------------------------------------
// Single-flight FIFO
// -----------------------------------------------------------------------
//
// The GPU renders one image at a time, so at most one request is in flight
// to the service; the rest queue. iz_lock guards the whole queue plus
// iz_active. The curl completion callback (iz_done, worker thread) advances
// the queue via iz_pump — the same "callback pumps the next" shape the old
// quotebot used, made thread-safe for this heavily-threaded daemon.

static pthread_mutex_t iz_lock    = PTHREAD_MUTEX_INITIALIZER;
static iz_req_t       *iz_head    = NULL;   // FIFO front (next to submit)
static iz_req_t       *iz_tail    = NULL;
static iz_req_t       *iz_current = NULL;   // the in-flight request, for !show
static bool            iz_active  = false;  // a request is in flight
static uint32_t        iz_depth   = 0;      // queued, excluding the in-flight one
static uint32_t        iz_id_seq  = 0;      // monotonic request id

static void iz_pump(void);
static void iz_done(const curl_response_t *resp);

// -----------------------------------------------------------------------
// Request building + submission
// -----------------------------------------------------------------------

// Reply to the user who made request r. r->ctx.msg already points at the
// embedded method_msg copy, so this is valid on any thread after dispatch.
static void
iz_reply(iz_req_t *r, const char *text)
{
  cmd_reply(&r->ctx, text);
}

// Build the request body {"prompt":"<escaped>"} into buf. Returns the body
// length, or 0 if it would not fit (buf must be generously sized for the
// worst-case JSON escape).
static size_t
iz_build_body(const char *prompt, char *buf, size_t cap)
{
  size_t escaped;
  size_t end;
  int    pre;

  pre = snprintf(buf, cap, "{\"prompt\":\"");

  if(pre < 0 || (size_t)pre >= cap)
    return(0);

  escaped = json_escape(prompt, buf + pre, cap - (size_t)pre);

  if(escaped >= cap - (size_t)pre)     // escaped body was truncated
    return(0);

  end = (size_t)pre + escaped;

  if(end + 3 > cap)                    // room for '"', '}', NUL
    return(0);

  buf[end++] = '"';
  buf[end++] = '}';
  buf[end]   = '\0';

  return(end);
}

// Create and submit the curl POST for r. Sets r->begin. On SUCCESS the
// transfer is owned by the curl subsystem and iz_done fires on a worker
// thread (r stays alive as user_data). On FAIL nothing is in flight, the
// callback will NOT fire, and the caller still owns r (must reply + free).
static bool
iz_submit_one(iz_req_t *r)
{
  curl_request_t *cr;
  char            body[IZ_PROMPT_SZ * 6 + 32];   // worst-case \uXXXX escape
  size_t          body_len;

  body_len = iz_build_body(r->prompt, body, sizeof(body));

  if(body_len == 0)
  {
    clam(CLAM_WARN, IZ_CTX, "#%u: prompt too long to encode", r->id);
    return(FAIL);
  }

  cr = curl_request_create(CURL_METHOD_POST, r->service_url, iz_done, r);

  if(cr == NULL)
    return(FAIL);

  // set_body only fails on a bad request state; the strict allocator aborts
  // on OOM rather than returning, so this path is effectively unreachable.
  if(curl_request_set_body(cr, "application/json", body, body_len) != SUCCESS)
    return(FAIL);

  if(r->auth_header[0] != '\0')
    curl_request_add_header(cr, r->auth_header);

  curl_request_add_header(cr, "Accept: application/json");
  curl_request_set_timeout(cr, r->timeout_secs);
  curl_request_set_prio(cr, CURL_PRIO_BULK);   // long, idempotent

  r->begin = time(NULL);

  // On submit failure the curl core releases cr itself; r is ours to free.
  if(curl_request_submit(cr) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Completion + queue pump
// -----------------------------------------------------------------------

// Runs on a curl worker thread: turn the service response into a channel
// reply (public URL on success, the service's error detail otherwise),
// release r, then advance the queue.
static void
iz_done(const curl_response_t *resp)
{
  iz_req_t *r = (iz_req_t *)resp->user_data;
  char      line[IZ_REPLY_SZ];
  long      elapsed;

  elapsed = (long)(time(NULL) - r->begin);

  if(resp->cancelled)
  {
    clam(CLAM_INFO, IZ_CTX, "#%u cancelled (shutdown drain)", r->id);
    goto done;
  }

  if(resp->status < 200 || resp->status >= 300 || resp->body == NULL)
  {
    char detail[256];

    detail[0] = '\0';

    if(resp->body != NULL && resp->body_len > 0)
    {
      struct json_object *root;

      root = json_parse_buf(resp->body, resp->body_len, IZ_CTX);

      if(root != NULL)
      {
        json_get_str(root, "detail", detail, sizeof(detail));
        json_object_put(root);
      }
    }

    if(detail[0] != '\0')
      snprintf(line, sizeof(line), "imagine #%u failed: %s", r->id, detail);

    else if(resp->error != NULL)
      snprintf(line, sizeof(line), "imagine #%u failed: %s", r->id, resp->error);

    else
      snprintf(line, sizeof(line), "imagine #%u failed (HTTP %ld)",
          r->id, resp->status);

    iz_reply(r, line);
    goto done;
  }

  {
    struct json_object *root;
    char                filename[IZ_FILENAME_SZ];

    filename[0] = '\0';
    root = json_parse_buf(resp->body, resp->body_len, IZ_CTX);

    if(root != NULL)
    {
      json_get_str(root, "filename", filename, sizeof(filename));
      json_object_put(root);
    }

    if(filename[0] == '\0')
    {
      snprintf(line, sizeof(line),
          "imagine #%u: service returned no filename", r->id);
      iz_reply(r, line);
      goto done;
    }

    snprintf(line, sizeof(line), "[#%u %lds] %s/%s",
        r->id, elapsed, r->public_base, filename);
    iz_reply(r, line);
  }

done:
  // r is leaving flight. Clear the display pointer before freeing so a
  // concurrent !show never dereferences it; iz_pump installs the next one.
  pthread_mutex_lock(&iz_lock);

  if(iz_current == r)
    iz_current = NULL;

  pthread_mutex_unlock(&iz_lock);

  mem_free(r);
  iz_pump();
}

// Submit queued requests until one is in flight or the queue drains. Called
// with iz_lock NOT held (it locks internally). A request that fails to submit
// synchronously is reported, dropped, and the drain continues.
static void
iz_pump(void)
{
  for(;;)
  {
    iz_req_t *r;
    char      line[IZ_REPLY_SZ];

    pthread_mutex_lock(&iz_lock);

    r = iz_head;

    if(r == NULL)
    {
      iz_active  = false;
      iz_current = NULL;
      iz_id_seq  = 0;                    // fully drained: restart numbering at #1
      pthread_mutex_unlock(&iz_lock);
      return;
    }

    iz_head = r->next;

    if(iz_head == NULL)
      iz_tail = NULL;

    iz_depth--;
    iz_active  = true;
    iz_current = r;
    pthread_mutex_unlock(&iz_lock);

    if(iz_submit_one(r) == SUCCESS)
      return;                          // in flight; iz_done pumps the next

    snprintf(line, sizeof(line), "imagine #%u failed to submit", r->id);
    iz_reply(r, line);

    pthread_mutex_lock(&iz_lock);

    if(iz_current == r)
      iz_current = NULL;

    pthread_mutex_unlock(&iz_lock);
    mem_free(r);
  }
}

// -----------------------------------------------------------------------
// Command handlers
// -----------------------------------------------------------------------

static void
imagine_zimage_handler(const cmd_ctx_t *ctx)
{
  iz_req_t   *r;
  const char *service_url;
  const char *public_base;
  const char *auth_header;
  uint32_t    timeout;
  uint32_t    max_queue;
  uint32_t    depth;
  bool        go_now;

  static const char usage[] = "Usage: imagine <prompt>  ·  !show imagine for status";

  if(ctx->args == NULL || ctx->args[0] == '\0')
  {
    cmd_reply(ctx, usage);
    return;
  }

  service_url = kv_get_str("plugin.imagine.service_url");
  public_base = kv_get_str("plugin.imagine.public_base");
  auth_header = kv_get_str("plugin.imagine.auth_header");

  if(service_url == NULL || service_url[0] == '\0'
      || public_base == NULL || public_base[0] == '\0')
  {
    cmd_reply(ctx, "imagine: not configured (set plugin.imagine.service_url "
        "and plugin.imagine.public_base)");
    return;
  }

  timeout = (uint32_t)kv_get_uint("plugin.imagine.timeout");

  if(timeout == 0)
    timeout = 180;

  max_queue = (uint32_t)kv_get_uint("plugin.imagine.max_queue");

  r = mem_alloc(IZ_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));

  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  snprintf(r->prompt,      sizeof(r->prompt),      "%s", ctx->args);
  snprintf(r->service_url, sizeof(r->service_url), "%s", service_url);
  snprintf(r->public_base, sizeof(r->public_base), "%s", public_base);
  snprintf(r->auth_header, sizeof(r->auth_header), "%s",
      auth_header != NULL ? auth_header : "");
  r->timeout_secs = timeout;

  // Take the in-flight slot if free; otherwise join the FIFO tail (subject
  // to the depth cap). id is assigned under the lock so it is monotonic.
  pthread_mutex_lock(&iz_lock);

  r->id  = ++iz_id_seq;
  go_now = !iz_active;

  if(go_now)
  {
    iz_active  = true;
    iz_current = r;
  }

  else
  {
    if(max_queue > 0 && iz_depth >= max_queue)
    {
      pthread_mutex_unlock(&iz_lock);
      cmd_reply(ctx, "imagine: too many images queued right now — "
          "try again shortly");
      mem_free(r);
      return;
    }

    r->next = NULL;

    if(iz_tail != NULL)
      iz_tail->next = r;
    else
      iz_head = r;

    iz_tail = r;
    iz_depth++;
  }

  depth = iz_depth;
  pthread_mutex_unlock(&iz_lock);

  if(!go_now)
  {
    char line[IZ_REPLY_SZ];

    snprintf(line, sizeof(line), "imagine: queued as #%u (%u ahead)",
        r->id, depth);
    cmd_reply(ctx, line);
    return;
  }

  if(iz_submit_one(r) != SUCCESS)
  {
    cmd_reply(ctx, "imagine: failed to submit request");

    pthread_mutex_lock(&iz_lock);

    if(iz_current == r)
      iz_current = NULL;

    pthread_mutex_unlock(&iz_lock);

    mem_free(r);
    iz_pump();                         // release the slot / drain any racers
  }
}

// One queue entry captured for display: id plus a short prompt preview.
// text is sized for IZ_PREVIEW_CHARS bytes + a 3-byte "…" + NUL.
typedef struct
{
  uint32_t id;
  char     text[IZ_PREVIEW_CHARS + 4];
} iz_snap_t;

// Copy the first IZ_PREVIEW_CHARS bytes of prompt into out, flattening
// control characters to spaces (prompts are single-line here) and appending
// an ellipsis when the prompt was longer than the window. ASCII-oriented: a
// truncation may land mid-UTF-8, which at worst garbles one preview glyph —
// acceptable for a status line.
static void
iz_prompt_preview(const char *prompt, char *out, size_t out_sz)
{
  size_t i;

  for(i = 0; i < IZ_PREVIEW_CHARS && prompt[i] != '\0'; i++)
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
show_imagine_handler(const cmd_ctx_t *ctx)
{
  const char *service_url;
  const char *public_base;
  const char *auth_header;
  iz_snap_t   queued[IZ_SHOW_MAX];
  iz_snap_t   current;
  uint32_t    n_queued = 0;
  uint32_t    depth;
  bool        active;
  bool        have_current = false;
  char        line[IZ_REPLY_SZ];

  service_url = kv_get_str("plugin.imagine.service_url");
  public_base = kv_get_str("plugin.imagine.public_base");
  auth_header = kv_get_str("plugin.imagine.auth_header");

  // Snapshot the in-flight request and the queued prompts under the lock, then
  // release it before replying — cmd_reply may re-enter the delivery path, so
  // the mutex is never held across an emit (snapshot-then-emit).
  pthread_mutex_lock(&iz_lock);

  active = iz_active;
  depth  = iz_depth;

  if(iz_current != NULL)
  {
    current.id = iz_current->id;
    iz_prompt_preview(iz_current->prompt, current.text, sizeof(current.text));
    have_current = true;
  }

  for(iz_req_t *r = iz_head; r != NULL && n_queued < IZ_SHOW_MAX; r = r->next)
  {
    queued[n_queued].id = r->id;
    iz_prompt_preview(r->prompt, queued[n_queued].text,
        sizeof(queued[n_queued].text));
    n_queued++;
  }

  pthread_mutex_unlock(&iz_lock);

  cmd_reply(ctx, CLR_BOLD "imagine" CLR_RESET
      "  ·  zimage-service backend (temporary)");

  snprintf(line, sizeof(line), "  service : " CLR_CYAN "%s" CLR_RESET,
      (service_url != NULL && service_url[0] != '\0')
      ? service_url : CLR_GRAY "(unset)" CLR_RESET);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "  public  : " CLR_CYAN "%s" CLR_RESET,
      (public_base != NULL && public_base[0] != '\0')
      ? public_base : CLR_GRAY "(unset)" CLR_RESET);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "  auth    : %s",
      (auth_header != NULL && auth_header[0] != '\0') ? "configured" : "none");
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "  queue   : %s, %u waiting",
      active ? CLR_YELLOW "busy" CLR_RESET : "idle", depth);
  cmd_reply(ctx, line);

  if(have_current)
  {
    snprintf(line, sizeof(line), "  render  : " CLR_GREEN "#%u" CLR_RESET
        " %s", current.id, current.text);
    cmd_reply(ctx, line);
  }

  for(uint32_t i = 0; i < n_queued; i++)
  {
    snprintf(line, sizeof(line), "    %2u. " CLR_GRAY "#%u" CLR_RESET " %s",
        i + 1, queued[i].id, queued[i].text);
    cmd_reply(ctx, line);
  }

  if(depth > n_queued)
  {
    snprintf(line, sizeof(line), "    " CLR_GRAY "… +%u more" CLR_RESET,
        depth - n_queued);
    cmd_reply(ctx, line);
  }
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static const char imagine_zimage_help[] =
    "Generate an image from a text prompt and print a public URL.\n"
    "\n"
    "  imagine <prompt>   generate an image (alias: ig)\n"
    "  show imagine       show backend status and queue depth\n"
    "\n"
    "Requests render one at a time on the GPU and are queued when busy;\n"
    "each is tagged with an id. The render service hosts the result and the\n"
    "reply is a link to it. Requires an operator to have set\n"
    "plugin.imagine.service_url and plugin.imagine.public_base.\n"
    "\n"
    "Examples:\n"
    "  !imagine a red panda in a chef hat\n"
    "  !show imagine";

static bool
imagine_zimage_init(void)
{
  if(cmd_register(IZ_CTX, "imagine", "imagine <prompt>",
      "Text-to-image generation (zimage-service backend)", imagine_zimage_help,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      imagine_zimage_handler, NULL, NULL, "ig", NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register(IZ_CTX, "imagine", "show imagine",
      "Show the imagine backend status and queue depth", NULL,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      show_imagine_handler, NULL, "show", NULL, NULL, 0, NULL, NULL) != SUCCESS)
  {
    cmd_unregister("imagine");
    return(FAIL);
  }

  clam(CLAM_INFO, IZ_CTX,
      "imagine (zimage-service) command plugin initialized");
  return(SUCCESS);
}

static void
imagine_zimage_deinit(void)
{
  iz_req_t *r;

  // Two nodes share the name "imagine" (root command + show child); two
  // unregister calls clear both.
  cmd_unregister("imagine");
  cmd_unregister("imagine");

  // Drop any queued (not-yet-submitted) closures we still own. An in-flight
  // request is owned by the curl subsystem; its callback frees itself (or
  // fires cancelled on the shutdown drain, which precedes plugin teardown).
  pthread_mutex_lock(&iz_lock);

  r = iz_head;

  while(r != NULL)
  {
    iz_req_t *next = r->next;

    mem_free(r);
    r = next;
  }

  iz_head    = NULL;
  iz_tail    = NULL;
  iz_depth   = 0;
  iz_active  = false;
  iz_current = NULL;
  pthread_mutex_unlock(&iz_lock);

  clam(CLAM_INFO, IZ_CTX,
      "imagine (zimage-service) command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "imagine_zimage",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "imagine",
  .provides        = { { .name = "cmd_imagine" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "method_text" },
  },
  .requires_count  = 1,
  .kv_schema       = iz_kv_schema,
  .kv_schema_count = sizeof(iz_kv_schema) / sizeof(iz_kv_schema[0]),
  .init            = imagine_zimage_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = imagine_zimage_deinit,
  .ext             = NULL,
};
