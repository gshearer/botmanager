// botmanager — MIT
// Image-vision intercept for the chat bot. Detects image URLs in
// incoming messages, validates + fetches + base64s them, then joins
// the existing reply pipeline via chatbot_reply_submit_vision().

#define CHATBOT_INTERNAL
#include "vision.h"

#include "clam.h"
#include "curl.h"
#include "kv.h"
#include "alloc.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Request context that survives the async curl_get hop. Allocated in
// chatbot_vision_maybe_submit; freed at the end of
// vision_on_fetch_done. On success the base64 payload is handed off
// to chatbot_reply_submit_vision (which takes ownership); the rest of
// the ctx is local to this file.
typedef struct
{
  chatbot_state_t *st;
  method_inst_t   *method;
  char             sender          [METHOD_SENDER_SZ];
  char             sender_metadata [METHOD_META_SZ];
  char             channel         [METHOD_CHANNEL_SZ];
  char             text            [METHOD_TEXT_SZ];
  char             image_url       [1024];
  uint32_t         max_bytes;
  bool             is_action;
} chatbot_vision_ctx_t;

static bool vision_cooldown_hot(chatbot_vision_cd_t *ring,
    const char *key, uint32_t cooldown_secs, time_t now);
static void vision_cooldown_stamp(chatbot_vision_cd_t *ring,
    const char *key, time_t now);
static void vision_url_cd_key(const char *target, const char *url,
    char *out, size_t out_cap);
static bool vision_magic_ok(const char *body, size_t len,
    const char *mime);
static void vision_on_fetch_done(const curl_response_t *resp);

// ----------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------

void
chatbot_vision_state_init(chatbot_state_t *st)
{
  if(st == NULL) return;

  memset(&st->vision_cd,     0, sizeof(st->vision_cd));
  memset(&st->vision_url_cd, 0, sizeof(st->vision_url_cd));
  pthread_mutex_init(&st->vision_cd.mutex,     NULL);
  pthread_mutex_init(&st->vision_url_cd.mutex, NULL);
  pthread_mutex_init(&st->vision_flight_mutex, NULL);
  st->vision_in_flight = 0;
}

void
chatbot_vision_state_destroy(chatbot_state_t *st)
{
  if(st == NULL) return;

  pthread_mutex_destroy(&st->vision_cd.mutex);
  pthread_mutex_destroy(&st->vision_url_cd.mutex);
  pthread_mutex_destroy(&st->vision_flight_mutex);
}

bool
chatbot_vision_maybe_submit(chatbot_state_t *st, const method_msg_t *msg)
{
  const char *botname;
  const char *target;
  char        key[128];
  char        image_url[1024];
  char        url_cd_key[METHOD_CHANNEL_SZ + 32];
  uint32_t    cooldown;
  uint32_t    url_cd;
  uint32_t    max_inflight;
  bool        allow_dm;
  time_t      now;
  chatbot_vision_ctx_t *ctx;
  curl_request_t *req;

  if(st == NULL || msg == NULL) return(false);

  botname = bot_inst_name(st->inst);
  if(botname == NULL || botname[0] == '\0') return(false);

  // Master switch.
  snprintf(key, sizeof(key),
      "bot.%s.behavior.image_vision.enabled", botname);
  if(kv_get_uint(key) == 0) return(false);

  // Public-only gate. Empty channel means DM.
  snprintf(key, sizeof(key),
      "bot.%s.behavior.image_vision.allow_dm", botname);
  allow_dm = (kv_get_uint(key) != 0);
  if(msg->channel[0] == '\0' && !allow_dm) return(false);

  // URL detection.
  if(!util_find_image_url(msg->text, image_url, sizeof(image_url)))
    return(false);

  // SSRF guard — must be safe HTTPS against a public host.
  if(!util_url_is_safe_https(image_url))
  {
    clam(CLAM_WARN, "vision", "rejected url='%s' reason=unsafe-or-http",
         image_url);
    return(true);
  }

  now    = time(NULL);
  target = msg->channel[0] != '\0' ? msg->channel : msg->sender;

  // Channel cooldown.
  snprintf(key, sizeof(key),
      "bot.%s.behavior.image_vision.cooldown_secs", botname);
  cooldown = (uint32_t)kv_get_uint(key);
  if(cooldown == 0) cooldown = 60;

  if(vision_cooldown_hot(&st->vision_cd, target, cooldown, now))
  {
    clam(CLAM_DEBUG, "vision", "channel cooldown hot target='%s'", target);
    return(true);
  }

  // Per-URL cooldown.
  snprintf(key, sizeof(key),
      "bot.%s.behavior.image_vision.url_cooldown_secs", botname);
  url_cd = (uint32_t)kv_get_uint(key);
  if(url_cd == 0) url_cd = 600;

  vision_url_cd_key(target, image_url, url_cd_key, sizeof(url_cd_key));

  if(vision_cooldown_hot(&st->vision_url_cd, url_cd_key, url_cd, now))
  {
    clam(CLAM_DEBUG, "vision", "url cooldown hot url='%s'", image_url);
    return(true);
  }

  // Concurrent-fetch cap.
  snprintf(key, sizeof(key),
      "bot.%s.behavior.image_vision.max_inflight", botname);
  max_inflight = (uint32_t)kv_get_uint(key);
  if(max_inflight == 0) max_inflight = 1;

  pthread_mutex_lock(&st->vision_flight_mutex);
  if(st->vision_in_flight >= max_inflight)
  {
    pthread_mutex_unlock(&st->vision_flight_mutex);
    clam(CLAM_DEBUG, "vision", "inflight cap hit (%u)", max_inflight);
    return(true);
  }
  st->vision_in_flight++;
  pthread_mutex_unlock(&st->vision_flight_mutex);

  // Stamp cooldowns now so a concurrent second identical line can't
  // race us into a double-fetch.
  vision_cooldown_stamp(&st->vision_cd,     target,     now);
  vision_cooldown_stamp(&st->vision_url_cd, url_cd_key, now);

  // Build ctx for the async hop.
  ctx = mem_alloc("vision", "ctx", sizeof(*ctx));
  if(ctx == NULL)
  {
    pthread_mutex_lock(&st->vision_flight_mutex);
    if(st->vision_in_flight > 0) st->vision_in_flight--;
    pthread_mutex_unlock(&st->vision_flight_mutex);
    return(true);
  }

  ctx->st     = st;
  ctx->method = msg->inst;
  snprintf(ctx->sender,          sizeof(ctx->sender),          "%s", msg->sender);
  snprintf(ctx->sender_metadata, sizeof(ctx->sender_metadata), "%s", msg->metadata);
  snprintf(ctx->channel,         sizeof(ctx->channel),         "%s", msg->channel);
  snprintf(ctx->text,            sizeof(ctx->text),            "%s", msg->text);
  snprintf(ctx->image_url,       sizeof(ctx->image_url),       "%s", image_url);
  ctx->is_action = msg->is_action;

  snprintf(key, sizeof(key),
      "bot.%s.behavior.image_vision.max_bytes", botname);
  ctx->max_bytes = (uint32_t)kv_get_uint(key);
  if(ctx->max_bytes == 0) ctx->max_bytes = 8 * 1000 * 1000;

  // Fire the fetch.
  req = curl_request_create(CURL_METHOD_GET, image_url,
      vision_on_fetch_done, ctx);
  if(req == NULL)
  {
    clam(CLAM_WARN, "vision",
        "curl_request_create failed url='%s'", image_url);
    pthread_mutex_lock(&st->vision_flight_mutex);
    if(st->vision_in_flight > 0) st->vision_in_flight--;
    pthread_mutex_unlock(&st->vision_flight_mutex);
    mem_free(ctx);
    return(true);
  }

  curl_request_set_follow_redirects(req, false);

  if(curl_request_submit(req) != SUCCESS)
  {
    clam(CLAM_WARN, "vision",
        "curl_request_submit failed url='%s'", image_url);
    pthread_mutex_lock(&st->vision_flight_mutex);
    if(st->vision_in_flight > 0) st->vision_in_flight--;
    pthread_mutex_unlock(&st->vision_flight_mutex);
    mem_free(ctx);
    return(true);
  }

  clam(CLAM_DEBUG, "vision",
      "fetching url='%s' max_bytes=%u", image_url, ctx->max_bytes);
  return(true);
}

// ----------------------------------------------------------------------
// Cooldown rings (mirror of chatbot_inflight_record_reply)
// ----------------------------------------------------------------------

static bool
vision_cooldown_hot(chatbot_vision_cd_t *ring, const char *key,
    uint32_t cooldown_secs, time_t now)
{
  bool hot = false;

  pthread_mutex_lock(&ring->mutex);

  for(size_t i = 0; i < CHATBOT_VISION_CD_SLOTS; i++)
  {
    if(strcmp(ring->slots[i].key, key) == 0)
    {
      hot = (now - ring->slots[i].last_reply) < (time_t)cooldown_secs;
      break;
    }
  }

  pthread_mutex_unlock(&ring->mutex);
  return(hot);
}

static void
vision_cooldown_stamp(chatbot_vision_cd_t *ring, const char *key, time_t now)
{
  uint32_t slot;

  pthread_mutex_lock(&ring->mutex);

  for(size_t i = 0; i < CHATBOT_VISION_CD_SLOTS; i++)
  {
    if(strcmp(ring->slots[i].key, key) == 0)
    {
      ring->slots[i].last_reply = now;
      pthread_mutex_unlock(&ring->mutex);
      return;
    }
  }

  slot = ring->next % CHATBOT_VISION_CD_SLOTS;
  snprintf(ring->slots[slot].key, sizeof(ring->slots[slot].key), "%s", key);
  ring->slots[slot].last_reply = now;
  ring->next++;

  pthread_mutex_unlock(&ring->mutex);
}

static void
vision_url_cd_key(const char *target, const char *url,
    char *out, size_t out_cap)
{
  // FNV-1a 64-bit over the URL → first 16 hex. Cheap + stable enough
  // for a 16-slot LRU; not crypto.
  uint64_t h = 1469598103934665603ULL;
  const unsigned char *p;

  for(p = (const unsigned char *)url; *p != '\0'; p++)
  {
    h ^= *p;
    h *= 1099511628211ULL;
  }

  snprintf(out, out_cap, "%s:%016llx", target, (unsigned long long)h);
}

// ----------------------------------------------------------------------
// Fetch completion (curl worker thread)
// ----------------------------------------------------------------------

static bool
vision_magic_ok(const char *body, size_t len, const char *mime)
{
  if(body == NULL || len < 8) return(false);

  if(strcmp(mime, "image/jpeg") == 0)
    return((unsigned char)body[0] == 0xff
        && (unsigned char)body[1] == 0xd8
        && (unsigned char)body[2] == 0xff);

  if(strcmp(mime, "image/png") == 0)
    return(memcmp(body, "\x89PNG\r\n\x1a\n", 8) == 0);

  if(strcmp(mime, "image/gif") == 0)
    return(memcmp(body, "GIF87a", 6) == 0 || memcmp(body, "GIF89a", 6) == 0);

  if(strcmp(mime, "image/webp") == 0)
    return(len >= 12
        && memcmp(body, "RIFF", 4) == 0
        && memcmp(body + 8, "WEBP", 4) == 0);

  return(false);
}

static void
vision_on_fetch_done(const curl_response_t *resp)
{
  chatbot_vision_ctx_t *ctx;
  const char *mime_canon = NULL;
  char        *b64       = NULL;
  size_t       b64_cap;
  size_t       b64_written;
  method_msg_t synth;

  if(resp == NULL || resp->user_data == NULL) return;

  ctx = resp->user_data;

  if(resp->status != 200)
  {
    clam(CLAM_WARN, "vision",
        "fetch failed url='%s' status=%ld", ctx->image_url, resp->status);
    goto done;
  }

  if(resp->content_type == NULL
      || strncmp(resp->content_type, "image/", 6) != 0)
  {
    clam(CLAM_WARN, "vision",
        "fetch bad content-type url='%s' type='%s'",
        ctx->image_url,
        resp->content_type != NULL ? resp->content_type : "(null)");
    goto done;
  }

  // Whitelist canonical MIME. Match the prefix so charset params /
  // surrounding whitespace don't defeat us.
  if(strncmp(resp->content_type, "image/jpeg", 10) == 0)
    mime_canon = "image/jpeg";
  else if(strncmp(resp->content_type, "image/png",  9)  == 0)
    mime_canon = "image/png";
  else if(strncmp(resp->content_type, "image/gif",  9)  == 0)
    mime_canon = "image/gif";
  else if(strncmp(resp->content_type, "image/webp", 10) == 0)
    mime_canon = "image/webp";

  if(mime_canon == NULL)
  {
    clam(CLAM_WARN, "vision",
        "fetch unsupported mime url='%s' type='%s'",
        ctx->image_url, resp->content_type);
    goto done;
  }

  if(resp->body == NULL || resp->body_len == 0)
  {
    clam(CLAM_WARN, "vision",
        "fetch empty body url='%s'", ctx->image_url);
    goto done;
  }

  if(resp->body_len > ctx->max_bytes)
  {
    clam(CLAM_WARN, "vision",
        "fetch too large url='%s' bytes=%zu max=%u",
        ctx->image_url, resp->body_len, ctx->max_bytes);
    goto done;
  }

  if(!vision_magic_ok(resp->body, resp->body_len, mime_canon))
  {
    clam(CLAM_WARN, "vision",
        "fetch magic mismatch url='%s' mime='%s'",
        ctx->image_url, mime_canon);
    goto done;
  }

  // Base64-encode. Capacity = ceil(n/3)*4 + 1.
  b64_cap = ((resp->body_len + 2) / 3) * 4 + 1;
  b64     = mem_alloc("vision", "b64", b64_cap);
  if(b64 == NULL) goto done;

  b64_written = util_b64_encode(resp->body, resp->body_len, b64, b64_cap);
  if(b64_written == 0)
  {
    clam(CLAM_WARN, "vision",
        "base64 overflow url='%s'", ctx->image_url);
    mem_free(b64);
    b64 = NULL;
    goto done;
  }

  // Synthesise a method_msg_t matching what chatbot_on_message would
  // have built, so chatbot_reply_submit_vision can populate
  // chatbot_req_t from a single argument.
  memset(&synth, 0, sizeof(synth));
  synth.inst = ctx->method;
  synth.kind = METHOD_MSG_MESSAGE;
  snprintf(synth.sender,   sizeof(synth.sender),   "%s", ctx->sender);
  snprintf(synth.metadata, sizeof(synth.metadata), "%s", ctx->sender_metadata);
  snprintf(synth.channel,  sizeof(synth.channel),  "%s", ctx->channel);
  snprintf(synth.text,     sizeof(synth.text),     "%s", ctx->text);
  synth.is_action = ctx->is_action;
  synth.timestamp = time(NULL);

  // Ownership transfer: b64 is now owned by the reply pipeline.
  chatbot_reply_submit_vision(ctx->st, &synth, ctx->image_url,
      b64, mime_canon);
  b64 = NULL;

done:
  if(b64 != NULL) mem_free(b64);

  pthread_mutex_lock(&ctx->st->vision_flight_mutex);
  if(ctx->st->vision_in_flight > 0) ctx->st->vision_in_flight--;
  pthread_mutex_unlock(&ctx->st->vision_flight_mutex);

  mem_free(ctx);
}
