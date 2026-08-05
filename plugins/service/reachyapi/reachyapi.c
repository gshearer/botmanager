// botmanager — MIT
// Reachy Mini service plugin: the mechanism half of the robot's body.
// Speaks the reachy-mini-daemon REST surface — the 84-move emotion
// library, wake/sleep, motor modes, speaker volume, sound upload and
// playback, daemon-side face tracking, head wobbling, and the
// microphone array's direction of arrival — and seals the daemon's
// conventions (the percent-encoded dataset id, its stringly enums, its
// nullable face target) behind reachyapi_api.h. Pure connectivity: no
// commands, no state, no threads. Presentation belongs to reachycmd,
// conversation to the reachy method driver.
#define REACHYAPI_INTERNAL
#include "reachyapi.h"

// ----------------------------------------------------------------------
// KV schema
// ----------------------------------------------------------------------

// One robot, addressed plugin-wide. Per-bot policy (voice, attention,
// volume at connect) is instance KV owned by the method driver — a
// second robot is what would move these two rows down there, and that
// day is not today.
static const plugin_kv_entry_t reachyapi_kv_schema[] = {
  { REACHYAPI_KV_BASE_URL, KV_STR,
    "http://reachy.iot.hiigara.shearer.tech:8000",
    "Base URL of the reachy-mini-daemon HTTP API (no trailing slash)" },

  // Nothing in this plugin reads this one: the ear is polled by the
  // reachy method driver, and a plugin that speaks to the robot has
  // no business speaking to the bridge. It lives here because the two
  // addresses are one robot and belong in one place.
  { REACHYAPI_KV_BRIDGE_URL, KV_STR,
    "http://reachy.iot.hiigara.shearer.tech:8090",
    "Base URL of earbridge, the robot's microphone bridge (no trailing "
    "slash)" },

  { REACHYAPI_KV_TIMEOUT,  KV_UINT32, "15",
    "Per-request timeout in seconds; the robot is on WiFi, so keep it "
    "generous" },
};

// ----------------------------------------------------------------------
// URL assembly and input validation
// ----------------------------------------------------------------------

// Copy the configured base URL into `out`, shorn of trailing slashes so
// it concatenates cleanly with every path below.
static bool
reachy_base(char *out, size_t cap)
{
  const char *url = kv_get_str(REACHYAPI_KV_BASE_URL);
  size_t      n;

  if(url == NULL || url[0] == '\0')
  {
    clam(CLAM_WARN, REACHYAPI_CTX, REACHYAPI_KV_BASE_URL " is unset");
    return(FAIL);
  }

  n = (size_t)snprintf(out, cap, "%s", url);

  if(n >= cap)
    return(FAIL);

  while(n > 0 && out[n - 1] == '/')
    out[--n] = '\0';

  return(n > 0 ? SUCCESS : FAIL);
}

static bool reachy_url(char *, size_t, const char *, ...)
    __attribute__((format(printf, 3, 4)));

static bool
reachy_url(char *out, size_t cap, const char *fmt, ...)
{
  char    base[KV_STR_SZ];
  va_list ap;
  size_t  len;
  int     n;

  if(reachy_base(base, sizeof(base)) != SUCCESS)
    return(FAIL);

  len = (size_t)snprintf(out, cap, "%s", base);

  if(len >= cap)
    return(FAIL);

  va_start(ap, fmt);
  n = vsnprintf(out + len, cap - len, fmt, ap);
  va_end(ap);

  if(n < 0 || (size_t)n >= cap - len)
  {
    clam(CLAM_WARN, REACHYAPI_CTX, "assembled URL exceeds %zu bytes", cap);
    return(FAIL);
  }

  return(SUCCESS);
}

// The emotion library's alphabet, measured against the shipped set:
// lowercase, digits, '-' and '_' (welcoming1, yes_sad1, toc-toc-toc,
// mini-deep-sleep). Anything else is refused rather than escaped — a
// name outside this set is a caller bug, not a user's unlucky phrasing,
// and refusing it keeps the URL splice below provably safe.
static bool
reachy_move_ok(const char *move)
{
  size_t n;

  if(move == NULL || move[0] == '\0')
    return(FAIL);

  for(n = 0; move[n] != '\0'; n++)
  {
    char c = move[n];

    if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_'))
      return(FAIL);
  }

  return(n < REACHY_MOVE_NAME_SZ ? SUCCESS : FAIL);
}

// A bare basename with an extension — never a path. The daemon writes
// the upload to /tmp/reachy_mini_sounds/<filename> and content-probes
// it, but a traversal attempt should never leave this process.
static bool
reachy_file_ok(const char *name)
{
  size_t n;

  if(name == NULL || name[0] == '\0' || name[0] == '.')
    return(FAIL);

  for(n = 0; name[n] != '\0'; n++)
  {
    char c = name[n];

    if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.'))
      return(FAIL);
  }

  return(n < REACHY_FILE_NAME_SZ ? SUCCESS : FAIL);
}

static bool
reachy_mode_ok(const char *mode)
{
  if(mode == NULL)
    return(FAIL);

  if(strcmp(mode, "enabled") == 0 || strcmp(mode, "disabled") == 0
      || strcmp(mode, "gravity_compensation") == 0)
    return(SUCCESS);

  return(FAIL);
}

// ----------------------------------------------------------------------
// The in-flight registry — every entry point borrows a foreign callback
// ----------------------------------------------------------------------

// Each call below stores its caller's completion in a heap context of
// ours and hands core's curl layer reachy_curl_done instead. That is one
// indirection more than core can see: plugin_quiesce and plugin_audit
// both range-test curl_iter_req_t.cb, which for our transfers points at
// THIS mapping, never at the caller's. Unload reachycmd — or the reachy
// driver, whose mouth uploads a WAV per spoken reply — with a robot
// request airborne and the stored pointer aims into freed .text.
//
// So every live request is filed here, and plugin_unmap_notify_register
// tells us when a mapping is about to go away in time to null the
// pointers that name it. A request whose caller left still completes
// normally; it simply delivers to nobody. `llm.c` holds the identical
// posture for the identical reason — the two are the tree's only
// listeners, and the pattern is written up in PLUGIN.md.
//
// The caller's `user_data` is dropped with the callback and whatever it
// points at is leaked. Nothing else is possible: only the caller knows
// how to free its own context, and the caller is precisely what is no
// longer there. A bounded leak on an operator action beats a SIGSEGV.
static pthread_mutex_t reachy_active_mutex = PTHREAD_MUTEX_INITIALIZER;
static reachy_req_t   *reachy_active_head  = NULL;
static uint32_t        reachy_active_count = 0;

static void
reachy_req_track(reachy_req_t *r)
{
  pthread_mutex_lock(&reachy_active_mutex);

  r->next_active     = reachy_active_head;
  reachy_active_head = r;
  reachy_active_count++;

  pthread_mutex_unlock(&reachy_active_mutex);
}

// Unlink `r`, copy it to `out` and free it. The copy is taken under the
// lock so a completion reads the caller's callback in the same critical
// section the unmap sweep would null it in — read it afterwards and the
// two interleave, which is the whole bug.
static void
reachy_req_retire(reachy_req_t *r, reachy_req_t *out)
{
  reachy_req_t **pp;

  pthread_mutex_lock(&reachy_active_mutex);

  for(pp = &reachy_active_head; *pp != NULL; pp = &(*pp)->next_active)
  {
    if(*pp != r)
      continue;

    *pp = r->next_active;
    reachy_active_count--;
    break;
  }

  *out = *r;

  pthread_mutex_unlock(&reachy_active_mutex);

  out->next_active = NULL;
  mem_free(r);
}

// A mapping is going away (core is between the plugin's deinit() and its
// residual audit, so nothing of it runs any more). Drop every callback
// that lives inside it.
//
// Residual race, the same one the reachy driver documents: a completion
// that has already retired its request holds the callback on its stack
// and is a few instructions from calling it. The window is bounded above
// by the quiescence poll plus the audit that follow this broadcast, and
// below by two stores — against an operator-timescale unload. Closing it
// would need core to wait on a lock a curl worker holds.
static void
reachyapi_unmap_cb(uintptr_t lo, uintptr_t hi, void *data)
{
  uint32_t orphaned = 0;

  (void)data;

  pthread_mutex_lock(&reachy_active_mutex);

  for(reachy_req_t *r = reachy_active_head; r != NULL; r = r->next_active)
  {
    uintptr_t cb = 0;

    switch(r->kind)
    {
      case REACHY_REQ_DOA:    cb = (uintptr_t)fn_addr(&r->cb.doa);    break;
      case REACHY_REQ_STATUS: cb = (uintptr_t)fn_addr(&r->cb.status); break;
      case REACHY_REQ_MOVES:  cb = (uintptr_t)fn_addr(&r->cb.moves);  break;
      case REACHY_REQ_DONE:   cb = (uintptr_t)fn_addr(&r->cb.done);   break;
    }

    if(cb == 0 || cb < lo || cb >= hi)
      continue;

    // memset rather than one arm's NULL: the arms are a union, and
    // all-bits-zero is the null test every delivery path makes.
    memset(&r->cb, 0, sizeof(r->cb));
    r->user_data = NULL;
    orphaned++;
  }

  pthread_mutex_unlock(&reachy_active_mutex);

  if(orphaned > 0)
    clam(CLAM_WARN, REACHYAPI_CTX, "%u robot request(s) lost their caller "
        "to an unload; they will complete and deliver nothing", orphaned);
}

// ----------------------------------------------------------------------
// Completion delivery — one arm per reply shape
// ----------------------------------------------------------------------

static reachy_status_t
reachy_status_of_http(long http, int curl_code)
{
  if(curl_code != 0)
    return(REACHY_TRANSPORT);

  if(http >= 200 && http < 300)
    return(REACHY_OK);

  return(REACHY_HTTP);
}

// Each arm takes a RETIRED request — a stack copy the completion owns,
// already unlinked from the in-flight list, whose callback is either the
// caller's or NULL because the caller was unloaded mid-flight.

static void
reachy_deliver_done(const reachy_req_t *r, reachy_status_t st,
    const curl_response_t *cresp)
{
  reachy_result_t out = {
    .status    = st,
    .http      = cresp->status,
    .user_data = r->user_data,
  };

  if(r->cb.done != NULL)
    r->cb.done(&out);
}

static void
reachy_deliver_doa(const reachy_req_t *r, reachy_status_t st,
    const curl_response_t *cresp)
{
  reachy_doa_t        out  = { .status = st, .user_data = r->user_data };
  struct json_object *root = NULL;

  if(st == REACHY_OK)
  {
    root = json_parse_buf(cresp->body, cresp->body_len, REACHYAPI_CTX);

    if(root == NULL || !json_get_double(root, "angle", &out.angle))
      out.status = REACHY_MALFORMED;

    else
      json_get_bool(root, "speech_detected", &out.speech);
  }

  if(r->cb.doa != NULL)
    r->cb.doa(&out);

  if(root != NULL)
    json_object_put(root);
}

static void
reachy_deliver_status(const reachy_req_t *r, reachy_status_t st,
    const curl_response_t *cresp)
{
  reachy_robot_status_t out  = { .status = st, .user_data = r->user_data };
  struct json_object   *root = NULL;

  if(st == REACHY_OK)
  {
    root = json_parse_buf(cresp->body, cresp->body_len, REACHYAPI_CTX);

    if(root == NULL
        || !json_get_str(root, "state", out.state, sizeof(out.state)))
      out.status = REACHY_MALFORMED;

    else
    {
      struct json_object *backend;
      struct json_object *face;

      json_get_bool(root, "media_released", &out.media_released);
      json_get_bool(root, "no_media",       &out.no_media);
      json_get_str (root, "version", out.version, sizeof(out.version));

      // Whether the robot has any torque in it. Left empty rather than
      // guessed at if the daemon ever stops reporting it: "unknown" is
      // a state a caller can render, and "disabled" is one it would act
      // on by heaving the robot to its feet.
      backend = json_get_obj(root, "backend_status");

      if(backend != NULL)
        json_get_str(backend, "motor_control_mode", out.motors,
            sizeof(out.motors));

      // face_target is always present; its x/y are null until the
      // tracker has a target, and json_get_double leaves them at zero.
      face = json_get_obj(root, "face_target");

      if(face != NULL)
      {
        json_get_bool  (face, "detected", &out.face_detected);
        json_get_double(face, "x", &out.face_x);
        json_get_double(face, "y", &out.face_y);
      }
    }
  }

  if(r->cb.status != NULL)
    r->cb.status(&out);

  if(root != NULL)
    json_object_put(root);
}

// The dataset listing is a bare JSON array of strings. Rows are copied
// into one flat block so the caller sees a lifetime-free char[][] for
// the duration of its callback.
static void
reachy_deliver_moves(const reachy_req_t *r, reachy_status_t st,
    const curl_response_t *cresp)
{
  reachy_moves_t      out  = { .status = st, .user_data = r->user_data };
  struct json_object *root = NULL;
  char              (*names)[REACHY_MOVE_NAME_SZ] = NULL;
  size_t              kept = 0;

  if(st == REACHY_OK)
  {
    root = json_parse_buf(cresp->body, cresp->body_len, REACHYAPI_CTX);

    if(root == NULL || !json_object_is_type(root, json_type_array))
      out.status = REACHY_MALFORMED;

    else
    {
      size_t len = (size_t)json_object_array_length(root);

      if(len > REACHY_MOVE_MAX)
      {
        clam(CLAM_WARN, REACHYAPI_CTX,
            "move library holds %zu entries, keeping %u", len,
            (unsigned)REACHY_MOVE_MAX);
        len = REACHY_MOVE_MAX;
      }

      if(len > 0)
        names = mem_alloc(REACHYAPI_CTX, "moves",
            len * REACHY_MOVE_NAME_SZ);

      for(size_t i = 0; i < len; i++)
      {
        struct json_object *item = json_object_array_get_idx(root, (int)i);
        const char         *s;

        if(item == NULL || !json_object_is_type(item, json_type_string))
          continue;

        s = json_object_get_string(item);

        if(s == NULL || s[0] == '\0')
          continue;

        snprintf(names[kept], REACHY_MOVE_NAME_SZ, "%s", s);
        kept++;
      }

      out.moves   = (const char (*)[REACHY_MOVE_NAME_SZ])names;
      out.n_moves = kept;
    }
  }

  if(r->cb.moves != NULL)
    r->cb.moves(&out);

  if(names != NULL)
    mem_free(names);

  if(root != NULL)
    json_object_put(root);
}

static void
reachy_curl_done(const curl_response_t *cresp)
{
  reachy_req_t    r;
  reachy_status_t st = reachy_status_of_http(cresp->status,
      cresp->curl_code);

  reachy_req_retire((reachy_req_t *)cresp->user_data, &r);

  if(st == REACHY_OK)
    clam(CLAM_DEBUG2, REACHYAPI_CTX, "kind %d ok (http %ld, %zu bytes)",
        (int)r.kind, cresp->status, cresp->body_len);

  else
    clam(CLAM_WARN, REACHYAPI_CTX, "request failed: %s (http %ld)",
        reachy_status_str(st), cresp->status);

  switch(r.kind)
  {
    case REACHY_REQ_DOA:
      reachy_deliver_doa(&r, st, cresp);
      break;

    case REACHY_REQ_STATUS:
      reachy_deliver_status(&r, st, cresp);
      break;

    case REACHY_REQ_MOVES:
      reachy_deliver_moves(&r, st, cresp);
      break;

    case REACHY_REQ_DONE:
    default:
      reachy_deliver_done(&r, st, cresp);
      break;
  }
}

// ----------------------------------------------------------------------
// Submission
// ----------------------------------------------------------------------

static reachy_req_t *
reachy_req_new(reachy_req_kind_t kind, void *user_data)
{
  reachy_req_t *r = mem_alloc(REACHYAPI_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));
  r->kind      = kind;
  r->user_data = user_data;

  return(r);
}

// Takes ownership of `r` on every path: it is freed here on refusal, and
// by the completion otherwise. That is what makes "FAIL ⇒ no callback"
// true for every entry point below.
static bool
reachy_submit(curl_method_t method, const char *url,
    const char *content_type, const void *body, size_t body_len,
    reachy_req_t *r)
{
  curl_request_t *cr;
  reachy_req_t    dead;
  uint32_t        timeout;

  cr = curl_request_create(method, url, reachy_curl_done, r);

  if(cr == NULL)
  {
    mem_free(r);
    return(FAIL);
  }

  if(content_type != NULL)
    curl_request_set_body(cr, content_type, (const char *)body, body_len);

  timeout = (uint32_t)kv_get_uint(REACHYAPI_KV_TIMEOUT);

  if(timeout > 0)
    curl_request_set_timeout(cr, timeout);

  curl_request_add_header(cr, "Accept: application/json");

  // File it before submitting, never after: the completion can run on a
  // curl worker before submit has even returned here, and it retires
  // what it finds.
  reachy_req_track(r);

  if(curl_request_submit(cr) != SUCCESS)
  {
    reachy_req_retire(r, &dead);
    return(FAIL);
  }

  return(SUCCESS);
}

// Every fire-and-forget verb funnels through here. A body is always
// attached, even for the endpoints that declare none: CURLOPT_POST with
// no POSTFIELDS falls back to libcurl's default read callback, and the
// daemon ignores a body it did not ask for.
static bool
reachy_post(const char *url, const char *json, reachy_done_cb_t cb,
    void *user_data)
{
  reachy_req_t *r    = reachy_req_new(REACHY_REQ_DONE, user_data);
  const char   *body = json != NULL ? json : "{}";

  r->cb.done = cb;

  return(reachy_submit(CURL_METHOD_POST, url, "application/json",
      body, strlen(body), r));
}

// ----------------------------------------------------------------------
// Multipart — the one thing core's curl layer does not build for us
// ----------------------------------------------------------------------

// Assemble an RFC-7578 body carrying a single `file` part. Returns a
// mem_alloc'd buffer the caller frees, writes its length to *out_len,
// and fills `boundary` with the delimiter the Content-Type header must
// repeat. NULL on a filename that will not fit its header line.
static char *
reachy_multipart(const char *filename, const void *payload, size_t len,
    char *boundary, size_t boundary_cap, size_t *out_len)
{
  char   head[REACHY_FILE_NAME_SZ + REACHY_BOUNDARY_SZ + 128];
  char   tail[REACHY_BOUNDARY_SZ + 16];
  char  *buf;
  size_t head_len;
  size_t tail_len;

  snprintf(boundary, boundary_cap, "----botman%04x%04x%04x%04x",
      (unsigned)util_rand(0x10000), (unsigned)util_rand(0x10000),
      (unsigned)util_rand(0x10000), (unsigned)util_rand(0x10000));

  head_len = (size_t)snprintf(head, sizeof(head),
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
      "Content-Type: audio/wav\r\n"
      "\r\n",
      boundary, filename);

  tail_len = (size_t)snprintf(tail, sizeof(tail), "\r\n--%s--\r\n",
      boundary);

  if(head_len >= sizeof(head) || tail_len >= sizeof(tail))
    return(NULL);

  buf = mem_alloc(REACHYAPI_CTX, "multipart", head_len + len + tail_len);
  memcpy(buf, head, head_len);
  memcpy(buf + head_len, payload, len);
  memcpy(buf + head_len + len, tail, tail_len);

  *out_len = head_len + len + tail_len;
  return(buf);
}

// ----------------------------------------------------------------------
// Provider API — the mechanism contract
// ----------------------------------------------------------------------

bool
reachy_get_status(reachy_status_cb_t cb, void *user_data)
{
  char          url[REACHY_URL_SZ];
  reachy_req_t *r;

  if(reachy_url(url, sizeof(url), "/api/daemon/status") != SUCCESS)
    return(FAIL);

  r = reachy_req_new(REACHY_REQ_STATUS, user_data);
  r->cb.status = cb;

  return(reachy_submit(CURL_METHOD_GET, url, NULL, NULL, 0, r));
}

bool
reachy_get_doa(reachy_doa_cb_t cb, void *user_data)
{
  char          url[REACHY_URL_SZ];
  reachy_req_t *r;

  if(reachy_url(url, sizeof(url), "/api/state/doa") != SUCCESS)
    return(FAIL);

  r = reachy_req_new(REACHY_REQ_DOA, user_data);
  r->cb.doa = cb;

  return(reachy_submit(CURL_METHOD_GET, url, NULL, NULL, 0, r));
}

bool
reachy_list_moves(reachy_moves_cb_t cb, void *user_data)
{
  char          url[REACHY_URL_SZ];
  reachy_req_t *r;

  // The dataset id travels as an argument, never spliced into the
  // format: it carries a percent-encoded slash.
  if(reachy_url(url, sizeof(url),
      "/api/move/recorded-move-datasets/list/%s", REACHY_MOVE_DATASET)
      != SUCCESS)
    return(FAIL);

  r = reachy_req_new(REACHY_REQ_MOVES, user_data);
  r->cb.moves = cb;

  return(reachy_submit(CURL_METHOD_GET, url, NULL, NULL, 0, r));
}

bool
reachy_play_move(const char *move, reachy_done_cb_t cb, void *user_data)
{
  char url[REACHY_URL_SZ];

  if(reachy_move_ok(move) != SUCCESS)
  {
    clam(CLAM_WARN, REACHYAPI_CTX, "refusing implausible move name");
    return(FAIL);
  }

  if(reachy_url(url, sizeof(url),
      "/api/move/play/recorded-move-dataset/%s/%s", REACHY_MOVE_DATASET,
      move) != SUCCESS)
    return(FAIL);

  return(reachy_post(url, NULL, cb, user_data));
}

bool
reachy_wake(reachy_done_cb_t cb, void *user_data)
{
  char url[REACHY_URL_SZ];

  if(reachy_url(url, sizeof(url), "/api/move/play/wake_up") != SUCCESS)
    return(FAIL);

  return(reachy_post(url, NULL, cb, user_data));
}

bool
reachy_sleep_move(reachy_done_cb_t cb, void *user_data)
{
  char url[REACHY_URL_SZ];

  if(reachy_url(url, sizeof(url), "/api/move/play/goto_sleep") != SUCCESS)
    return(FAIL);

  return(reachy_post(url, NULL, cb, user_data));
}

bool
reachy_motors_mode(const char *mode, reachy_done_cb_t cb, void *user_data)
{
  char url[REACHY_URL_SZ];

  if(reachy_mode_ok(mode) != SUCCESS)
    return(FAIL);

  if(reachy_url(url, sizeof(url), "/api/motors/set_mode/%s", mode)
      != SUCCESS)
    return(FAIL);

  return(reachy_post(url, NULL, cb, user_data));
}

bool
reachy_set_volume(uint8_t pct, reachy_done_cb_t cb, void *user_data)
{
  char url [REACHY_URL_SZ];
  char body[REACHY_BODY_SZ];

  if(pct > 100)
    return(FAIL);

  if(reachy_url(url, sizeof(url), "/api/volume/set") != SUCCESS)
    return(FAIL);

  snprintf(body, sizeof(body), "{\"volume\":%u}", (unsigned)pct);
  return(reachy_post(url, body, cb, user_data));
}

bool
reachy_upload_sound(const char *filename, const void *wav, size_t wav_len,
    reachy_done_cb_t cb, void *user_data)
{
  char          url[REACHY_URL_SZ];
  char          boundary[REACHY_BOUNDARY_SZ];
  char          ctype[REACHY_BOUNDARY_SZ + 40];
  char         *body;
  size_t        body_len = 0;
  reachy_req_t *r;
  bool          ok;

  if(reachy_file_ok(filename) != SUCCESS || wav == NULL || wav_len == 0
      || wav_len > REACHY_UPLOAD_MAX)
    return(FAIL);

  if(reachy_url(url, sizeof(url), "/api/media/sounds/upload") != SUCCESS)
    return(FAIL);

  body = reachy_multipart(filename, wav, wav_len, boundary,
      sizeof(boundary), &body_len);

  if(body == NULL)
    return(FAIL);

  snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s",
      boundary);

  r = reachy_req_new(REACHY_REQ_DONE, user_data);
  r->cb.done = cb;

  // curl copies the body at set_body time, so our assembly buffer is
  // dead the moment submit returns either way.
  ok = reachy_submit(CURL_METHOD_POST, url, ctype, body, body_len, r);
  mem_free(body);

  return(ok);
}

bool
reachy_play_sound(const char *filename, reachy_done_cb_t cb, void *user_data)
{
  char url [REACHY_URL_SZ];
  char body[REACHY_FILE_NAME_SZ + 16];

  if(reachy_file_ok(filename) != SUCCESS)
    return(FAIL);

  if(reachy_url(url, sizeof(url), "/api/media/play_sound") != SUCCESS)
    return(FAIL);

  snprintf(body, sizeof(body), "{\"file\":\"%s\"}", filename);
  return(reachy_post(url, body, cb, user_data));
}

bool
reachy_stop_sound(reachy_done_cb_t cb, void *user_data)
{
  char url[REACHY_URL_SZ];

  if(reachy_url(url, sizeof(url), "/api/media/stop_sound") != SUCCESS)
    return(FAIL);

  return(reachy_post(url, NULL, cb, user_data));
}

bool
reachy_tracking(bool on, double weight, reachy_done_cb_t cb, void *user_data)
{
  char url [REACHY_URL_SZ];
  char body[REACHY_BODY_SZ];

  if(reachy_url(url, sizeof(url), "/api/media/tracking/%s",
      on ? "enable" : "disable") != SUCCESS)
    return(FAIL);

  if(!on)
    return(reachy_post(url, NULL, cb, user_data));

  if(weight < 0.0)
    weight = 0.0;

  if(weight > 1.0)
    weight = 1.0;

  snprintf(body, sizeof(body), "{\"weight\":%.3f}", weight);
  return(reachy_post(url, body, cb, user_data));
}

bool
reachy_wobbling(bool on, reachy_done_cb_t cb, void *user_data)
{
  char url[REACHY_URL_SZ];

  if(reachy_url(url, sizeof(url), "/api/media/wobbling/%s",
      on ? "enable" : "disable") != SUCCESS)
    return(FAIL);

  return(reachy_post(url, NULL, cb, user_data));
}

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

// No commands, no tasks, no clam subscriptions. The one thing that does
// outlive a call is the in-flight registry, and the unmap listener that
// keeps it honest is the single registration deinit() has to mirror.
// suspend/resume being absent stays honest: a request in flight owns its
// own heap and its completion runs in this mapping.
static bool
reachyapi_init(void)
{
  char base[KV_STR_SZ];

  if(reachy_base(base, sizeof(base)) != SUCCESS)
    clam(CLAM_WARN, REACHYAPI_CTX,
        "no robot configured: set " REACHYAPI_KV_BASE_URL);

  plugin_unmap_notify_register(reachyapi_unmap_cb, NULL);

  clam(CLAM_INFO, REACHYAPI_CTX, "reachyapi plugin initialized");
  return(SUCCESS);
}

static void
reachyapi_deinit(void)
{
  uint32_t stranded;

  plugin_unmap_notify_unregister(reachyapi_unmap_cb);

  pthread_mutex_lock(&reachy_active_mutex);
  stranded = reachy_active_count;
  pthread_mutex_unlock(&reachy_active_mutex);

  // Nothing to free: those requests belong to curl, and their
  // completions live in the mapping now going away. Core's residual
  // audit sees them — reachy_curl_done is curl_iter_req_t.cb for every
  // one — so it is the audit that refuses the dlclose, not us. Naming
  // the count here is what makes that refusal legible.
  if(stranded > 0)
    clam(CLAM_WARN, REACHYAPI_CTX,
        "%u robot request(s) still in flight at deinit", stranded);

  clam(CLAM_INFO, REACHYAPI_CTX, "reachyapi plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = REACHYAPI_CTX,
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = REACHYAPI_CTX,
  .provides        = { { .name = "service_reachyapi" } },
  .provides_count  = 1,
  .requires_count  = 0,
  .kv_schema       = reachyapi_kv_schema,
  .kv_schema_count = sizeof(reachyapi_kv_schema)
                     / sizeof(reachyapi_kv_schema[0]),
  .init            = reachyapi_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = reachyapi_deinit,
  .ext             = NULL,
};
