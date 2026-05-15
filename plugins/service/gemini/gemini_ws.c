// botmanager — MIT
// Gemini WebSocket transport — two long-lived libcurl WS sessions.
//
//   GEM_WS_MD: wss://api.gemini.com/v2/marketdata  (public)
//   GEM_WS_OE: wss://api.gemini.com/v1/order/events (private)
//
// Each session owns:
//   * a `task_add_persist` reader thread driving curl_ws_recv,
//   * its own session lock + lifecycle condvar,
//   * an idle watchdog (drop after GEM_WS_IDLE_TIMEOUT_MS),
//   * a keep-alive ping cadence (GEM_WS_PING_INTERVAL_MS),
//   * an exponential reconnect backoff (capped at GEM_WS_MAX_BACKOFF_MS),
//   * a grow-on-demand reassembly buffer (capped at GEM_WS_ASSEMBLY_CAP).
//
// The two sessions do NOT share state with each other. The Order
// Events session adds X-GEMINI-APIKEY / X-GEMINI-PAYLOAD /
// X-GEMINI-SIGNATURE headers to the HTTP handshake (signed against
// `{"request":"/v1/order/events","nonce":<n>}`); no token cache.
//
// KV reconfiguration: any change to plugin.gemini.ws_enabled, either
// URL key, the reconnect cadence, or the creds keys posts an atomic
// flag the readers consume at the top of their next tick. State
// transitions happen on the reader thread — never on the caller's.

#define GEM_INTERNAL
#include "gemini.h"

#include "pool.h"
#include "task.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ------------------------------------------------------------------ //
// Session state                                                       //
// ------------------------------------------------------------------ //

typedef struct
{
  gem_ws_session_id_t  sid;          // GEM_WS_MD or GEM_WS_OE
  const char          *log_ctx;      // "gemini.ws.md" / "gemini.ws.oe"
  const char          *task_name;    // "gem_ws_md" / "gem_ws_oe"
  const char          *url_kv_key;   // plugin.gemini.ws_url_marketdata|...

  CURL                *easy;
  curl_socket_t        sockfd;
  gem_ws_state_t       state;

  pthread_mutex_t      lock;         // guards easy, state, rx_buf, thread_alive

  task_t              *reader;
  bool                 exit_requested;
  bool                 thread_alive;
  pthread_cond_t       lifecycle_cond;

  bool                 enabled;      // latched copy of plugin.gemini.ws_enabled

  char                 url[GEM_URL_SZ];
  uint32_t             reconnect_base_ms;
  uint32_t             backoff_ms;
  time_t               backoff_until;
  uint32_t             consec_fails;
  time_t               last_crit_log;
  // Throttle "OE auth not configured" warnings to one per minute so a
  // persistent unauth state doesn't flood the log.
  time_t               last_auth_warn;

  uint64_t             last_frame_ms;
  uint64_t             last_ping_ms;

  // Reassembly buffer for multi-chunk text frames. Grown on demand;
  // held across reconnects so normal operation never re-allocs.
  char                *rx_buf;
  size_t               rx_len;
  size_t               rx_cap;
} gem_ws_session_t;

static gem_ws_session_t gem_ws_md;
static gem_ws_session_t gem_ws_oe;

// Set from a KV change callback; cleared by the reader on its next
// tick so the reader — not the caller's thread — owns the state
// transition. One flag covers both sessions; both readers check it on
// every iteration so the second one through still sees the reconfig.
static _Atomic uint32_t gem_ws_reconfig_gen;

// Internal helpers — all *_locked require sess->lock held by caller.

static void     gem_ws_reader              (task_t *t);
static bool     gem_ws_open_locked         (gem_ws_session_t *s);
static bool     gem_ws_open_md_locked      (gem_ws_session_t *s);
static bool     gem_ws_open_oe_locked      (gem_ws_session_t *s);
static void     gem_ws_close_locked        (gem_ws_session_t *s);
static void     gem_ws_on_frame_locked     (gem_ws_session_t *s,
                    const char *data, size_t len,
                    const struct curl_ws_frame *meta);
static bool     gem_ws_send_ping_locked    (gem_ws_session_t *s);
static bool     gem_ws_send_pong_locked    (gem_ws_session_t *s,
                    const char *buf, size_t len);
static bool     gem_ws_rx_append_locked    (gem_ws_session_t *s,
                    const char *data, size_t len);
static void     gem_ws_schedule_reconnect_locked(gem_ws_session_t *s,
                    const char *why);
static void     gem_ws_set_state_locked    (gem_ws_session_t *s,
                    gem_ws_state_t st);
static void     gem_ws_reload_url_locked   (gem_ws_session_t *s);
static void     gem_ws_apply_reconfig_locked(gem_ws_session_t *s);
static void     gem_ws_kv_cb               (const char *key, void *data);
static uint64_t gem_ws_now_ms              (void);
static void     gem_ws_oe_normalize_url_locked(gem_ws_session_t *s);

// ------------------------------------------------------------------ //
// Public surface — state helper, send, dispatch                       //
// ------------------------------------------------------------------ //

const char *
gem_ws_state_name(gem_ws_state_t s)
{
  switch(s)
  {
    case GEM_WS_DISCONNECTED: return("DISCONNECTED");
    case GEM_WS_CONNECTING:   return("CONNECTING");
    case GEM_WS_OPEN:         return("OPEN");
    case GEM_WS_RECONNECTING: return("RECONNECTING");
  }

  return("UNKNOWN");
}

static gem_ws_session_t *
gem_ws_session(gem_ws_session_id_t sid)
{
  switch(sid)
  {
    case GEM_WS_MD: return(&gem_ws_md);
    case GEM_WS_OE: return(&gem_ws_oe);
  }
  return(NULL);
}

void
gem_ws_dispatch_frame(gem_ws_session_id_t sid, const char *buf, size_t len)
{
  if(buf == NULL || len == 0)
    return;

  switch(sid)
  {
    case GEM_WS_MD: gem_ws_channels_dispatch_md(buf, len); return;
    case GEM_WS_OE: gem_ws_channels_dispatch_oe(buf, len); return;
  }
}

bool
gem_ws_send_text(gem_ws_session_id_t sid, const char *buf, size_t len)
{
  gem_ws_session_t *s = gem_ws_session(sid);
  CURLcode          rc;
  size_t            sent = 0;
  bool              ok   = FAIL;

  if(s == NULL || buf == NULL || len == 0)
    return(FAIL);

  pthread_mutex_lock(&s->lock);

  if(s->state == GEM_WS_OPEN && s->easy != NULL)
  {
    rc = curl_ws_send(s->easy, buf, len, &sent, 0, CURLWS_TEXT);

    if(rc == CURLE_OK && sent == len)
    {
      ok = SUCCESS;
      clam(CLAM_DEBUG3, s->log_ctx, "send (%zu bytes)", len);
    }
    else
      clam(CLAM_WARN, s->log_ctx,
          "send_text failed rc=%d sent=%zu/%zu",
          (int)rc, sent, len);
  }
  else
    clam(CLAM_DEBUG, s->log_ctx, "send_text dropped: state=%s",
        gem_ws_state_name(s->state));

  pthread_mutex_unlock(&s->lock);

  return(ok);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static void
gem_ws_session_zero(gem_ws_session_t *s, gem_ws_session_id_t sid,
    const char *log_ctx, const char *task_name, const char *url_kv_key)
{
  memset(s, 0, sizeof(*s));
  s->sid        = sid;
  s->log_ctx    = log_ctx;
  s->task_name  = task_name;
  s->url_kv_key = url_kv_key;
  s->state      = GEM_WS_DISCONNECTED;
  s->sockfd     = CURL_SOCKET_BAD;

  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->lifecycle_cond, NULL);
}

void
gem_ws_init(void)
{
  gem_ws_session_zero(&gem_ws_md, GEM_WS_MD,
      GEM_CTX ".ws.md", "gem_ws_md",
      "plugin.gemini.ws_url_marketdata");
  gem_ws_session_zero(&gem_ws_oe, GEM_WS_OE,
      GEM_CTX ".ws.oe", "gem_ws_oe",
      "plugin.gemini.ws_url_order_events");

  atomic_store(&gem_ws_reconfig_gen, 0u);

  // Any config knob that materially changes which endpoint we should
  // be attached to triggers a reconnect on both readers. The callback
  // is deliberately lock-free — readers poll gem_ws_reconfig_gen at
  // the top of each tick so the caller's thread is never blocked
  // waiting for a session lock while kv_set holds the KV registry
  // lock.
  kv_set_cb("plugin.gemini.ws_enabled",          gem_ws_kv_cb, NULL);
  kv_set_cb("plugin.gemini.ws_url_marketdata",   gem_ws_kv_cb, NULL);
  kv_set_cb("plugin.gemini.ws_url_order_events", gem_ws_kv_cb, NULL);
  kv_set_cb("plugin.gemini.ws_reconnect_ms",     gem_ws_kv_cb, NULL);

  // Cred-key changes invalidate the OE handshake so the next reconnect
  // re-signs against the refreshed key.
  kv_set_cb("plugin.gemini.creds.api_key",       gem_ws_kv_cb, NULL);
  kv_set_cb("plugin.gemini.creds.private_key",   gem_ws_kv_cb, NULL);

  clam(CLAM_DEBUG, GEM_CTX, "ws: subsystem initialized");
}

static void
gem_ws_start_one(gem_ws_session_t *s)
{
  s->enabled = (kv_get_uint("plugin.gemini.ws_enabled") != 0);

  if(s->reader != NULL)
    return;

  s->exit_requested = false;
  s->thread_alive   = true;

  s->reader = task_add_persist(s->task_name, 50, gem_ws_reader, s);

  if(s->reader == NULL)
  {
    clam(CLAM_WARN, s->log_ctx, "failed to spawn reader task");
    s->thread_alive = false;
    return;
  }

  clam(CLAM_INFO, s->log_ctx, "reader started (enabled=%s)",
      s->enabled ? "true" : "false");
}

void
gem_ws_start(void)
{
  gem_ws_start_one(&gem_ws_md);
  gem_ws_start_one(&gem_ws_oe);
}

static void
gem_ws_stop_one(gem_ws_session_t *s)
{
  struct timespec deadline;

  if(s->reader == NULL)
    return;

  pthread_mutex_lock(&s->lock);
  s->exit_requested = true;
  pthread_mutex_unlock(&s->lock);

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += (GEM_WS_STOP_WAIT_MS / 1000);

  pthread_mutex_lock(&s->lock);

  while(s->thread_alive)
  {
    int rc = pthread_cond_timedwait(&s->lifecycle_cond, &s->lock,
        &deadline);

    if(rc == ETIMEDOUT)
    {
      clam(CLAM_WARN, s->log_ctx,
          "stop: reader did not exit within %d ms",
          GEM_WS_STOP_WAIT_MS);
      break;
    }
  }

  pthread_mutex_unlock(&s->lock);

  s->reader = NULL;

  clam(CLAM_INFO, s->log_ctx, "reader stopped");
}

void
gem_ws_stop(void)
{
  gem_ws_stop_one(&gem_ws_md);
  gem_ws_stop_one(&gem_ws_oe);
}

static void
gem_ws_deinit_one(gem_ws_session_t *s)
{
  if(s->reader != NULL)
    gem_ws_stop_one(s);

  pthread_mutex_lock(&s->lock);

  gem_ws_close_locked(s);

  if(s->rx_buf != NULL)
  {
    mem_free(s->rx_buf);
    s->rx_buf = NULL;
    s->rx_cap = 0;
    s->rx_len = 0;
  }

  pthread_mutex_unlock(&s->lock);

  pthread_cond_destroy(&s->lifecycle_cond);
  pthread_mutex_destroy(&s->lock);
}

void
gem_ws_deinit(void)
{
  gem_ws_deinit_one(&gem_ws_md);
  gem_ws_deinit_one(&gem_ws_oe);
}

// ------------------------------------------------------------------ //
// Internals — state + reconfig                                        //
// ------------------------------------------------------------------ //

static uint64_t
gem_ws_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return(((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u));
}

static void
gem_ws_set_state_locked(gem_ws_session_t *s, gem_ws_state_t st)
{
  if(s->state == st)
    return;

  clam(CLAM_DEBUG, s->log_ctx, "state %s -> %s",
      gem_ws_state_name(s->state), gem_ws_state_name(st));

  s->state = st;
}

static void
gem_ws_reload_url_locked(gem_ws_session_t *s)
{
  const char *base;
  size_t      blen;

  base = kv_get_str(s->url_kv_key);

  if(base == NULL || base[0] == '\0')
  {
    s->url[0] = '\0';
    return;
  }

  blen = strlen(base);

  if(blen >= sizeof(s->url))
  {
    s->url[0] = '\0';
    return;
  }

  memcpy(s->url, base, blen);
  s->url[blen] = '\0';

  // OE adds the `?heartbeat=true` query string at connect time, only
  // if the operator-set URL doesn't already carry one. Preserves any
  // customisation (e.g. a sandbox URL with its own params).
  if(s->sid == GEM_WS_OE)
    gem_ws_oe_normalize_url_locked(s);
}

static void
gem_ws_oe_normalize_url_locked(gem_ws_session_t *s)
{
  const char *q;
  size_t      cur_len;
  size_t      need;

  if(s->url[0] == '\0')
    return;

  q = strchr(s->url, '?');

  if(q != NULL)
    return;   // operator-supplied query string — leave alone

  cur_len = strlen(s->url);
  need    = cur_len + sizeof("?heartbeat=true") - 1;

  if(need >= sizeof(s->url))
  {
    clam(CLAM_WARN, s->log_ctx,
        "URL overflow appending ?heartbeat=true (cur=%zu cap=%zu)",
        cur_len, sizeof(s->url));
    return;
  }

  memcpy(s->url + cur_len, "?heartbeat=true",
      sizeof("?heartbeat=true") - 1);
  s->url[need] = '\0';
}

static void
gem_ws_kv_cb(const char *key, void *data)
{
  (void)data;

  clam(CLAM_INFO, GEM_CTX,
      "ws config changed (%s); reconnect scheduled on next tick", key);

  atomic_fetch_add(&gem_ws_reconfig_gen, 1u);
}

// Drop any live session and land in DISCONNECTED so the reader's
// normal backoff-and-reopen flow reconnects against the refreshed
// config on the next loop iteration. Called from the reader thread
// only.
static void
gem_ws_apply_reconfig_locked(gem_ws_session_t *s)
{
  s->enabled = (kv_get_uint("plugin.gemini.ws_enabled") != 0);

  if(s->state == GEM_WS_OPEN || s->state == GEM_WS_CONNECTING)
    gem_ws_close_locked(s);

  gem_ws_set_state_locked(s, GEM_WS_DISCONNECTED);
  s->backoff_until = 0;
  s->backoff_ms    = 0;
  s->consec_fails  = 0;
}

// ------------------------------------------------------------------ //
// Connect + reconnect                                                 //
// ------------------------------------------------------------------ //

// Common close + curl init + handshake. Returns SUCCESS on a usable
// OPEN session; FAIL leaves the session in CONNECTING (caller schedules
// a reconnect).
static bool
gem_ws_open_md_locked(gem_ws_session_t *s)
{
  CURLcode      rc;
  curl_socket_t sock = CURL_SOCKET_BAD;

  s->easy = curl_easy_init();

  if(s->easy == NULL)
  {
    clam(CLAM_WARN, s->log_ctx, "open: curl_easy_init failed");
    return(FAIL);
  }

  gem_ws_set_state_locked(s, GEM_WS_CONNECTING);

  curl_easy_setopt(s->easy, CURLOPT_URL,            s->url);
  curl_easy_setopt(s->easy, CURLOPT_CONNECT_ONLY,   2L);
  curl_easy_setopt(s->easy, CURLOPT_NOSIGNAL,       1L);
  curl_easy_setopt(s->easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(s->easy, CURLOPT_TIMEOUT,        10L);
  curl_easy_setopt(s->easy, CURLOPT_USERAGENT,      "libp0ada/2.0");

  rc = curl_easy_perform(s->easy);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, s->log_ctx, "open: handshake failed url='%s' rc=%d (%s)",
        s->url, (int)rc, curl_easy_strerror(rc));

    curl_easy_cleanup(s->easy);
    s->easy = NULL;

    return(FAIL);
  }

  if(curl_easy_getinfo(s->easy, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK
      || sock == CURL_SOCKET_BAD)
  {
    clam(CLAM_WARN, s->log_ctx,
        "open: no socket available after handshake url='%s'", s->url);

    curl_easy_cleanup(s->easy);
    s->easy   = NULL;
    s->sockfd = CURL_SOCKET_BAD;

    return(FAIL);
  }

  s->sockfd = sock;

  return(SUCCESS);
}

// Order Events open path. Mints a fresh nonce + HMAC headers each
// reconnect so the gateway sees a strictly monotonic nonce stream.
// Re-uses gem_sign_request — the same primitive REST private uses.
static bool
gem_ws_open_oe_locked(gem_ws_session_t *s)
{
  CURLcode            rc;
  curl_socket_t       sock        = CURL_SOCKET_BAD;
  struct curl_slist  *hdrs        = NULL;
  uint64_t            nonce       = 0;
  char                payload[256];
  int                 plen;
  char                payload_b64[512];
  char                sig_hex[128];
  char                api_key_hdr[288];
  char                payload_hdr[576];
  char                sig_hdr[192];
  const char         *api_key;
  time_t              now;

  if(!gem_apikey_configured())
  {
    // Throttle to once per minute so a long unauthed run doesn't
    // flood the log.
    now = time(NULL);
    if(now - s->last_auth_warn >= 60)
    {
      clam(CLAM_INFO, s->log_ctx,
          "api keys not configured; private stream disabled");
      s->last_auth_warn = now;
    }
    return(FAIL);
  }

  if(gem_next_nonce(&nonce) != SUCCESS)
  {
    clam(CLAM_WARN, s->log_ctx, "open: nonce mint failed");
    return(FAIL);
  }

  plen = snprintf(payload, sizeof(payload),
      "{\"request\":\"/v1/order/events\",\"nonce\":%" PRIu64 "}", nonce);

  if(plen < 0 || (size_t)plen >= sizeof(payload))
  {
    clam(CLAM_WARN, s->log_ctx, "open: handshake payload overflow");
    return(FAIL);
  }

  if(gem_sign_request(payload, (size_t)plen,
        payload_b64, sizeof(payload_b64),
        sig_hex,     sizeof(sig_hex)) != SUCCESS)
  {
    clam(CLAM_WARN, s->log_ctx, "open: handshake sign failed");
    return(FAIL);
  }

  kv_admin_context_set(true);
  api_key = kv_get_str("plugin.gemini.creds.api_key");
  kv_admin_context_set(false);

  if(api_key == NULL || api_key[0] == '\0')
  {
    clam(CLAM_WARN, s->log_ctx, "open: api_key empty");
    return(FAIL);
  }

  if(snprintf(api_key_hdr, sizeof(api_key_hdr),
        "X-GEMINI-APIKEY: %s", api_key) >= (int)sizeof(api_key_hdr))
  {
    clam(CLAM_WARN, s->log_ctx, "open: api_key header overflow");
    return(FAIL);
  }

  if(snprintf(payload_hdr, sizeof(payload_hdr),
        "X-GEMINI-PAYLOAD: %s", payload_b64) >= (int)sizeof(payload_hdr))
  {
    clam(CLAM_WARN, s->log_ctx, "open: payload header overflow");
    return(FAIL);
  }

  if(snprintf(sig_hdr, sizeof(sig_hdr),
        "X-GEMINI-SIGNATURE: %s", sig_hex) >= (int)sizeof(sig_hdr))
  {
    clam(CLAM_WARN, s->log_ctx, "open: signature header overflow");
    return(FAIL);
  }

  hdrs = curl_slist_append(hdrs, api_key_hdr);
  hdrs = curl_slist_append(hdrs, payload_hdr);
  hdrs = curl_slist_append(hdrs, sig_hdr);
  hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache");

  if(hdrs == NULL)
  {
    clam(CLAM_WARN, s->log_ctx, "open: curl_slist build failed");
    return(FAIL);
  }

  s->easy = curl_easy_init();

  if(s->easy == NULL)
  {
    curl_slist_free_all(hdrs);
    clam(CLAM_WARN, s->log_ctx, "open: curl_easy_init failed");
    return(FAIL);
  }

  gem_ws_set_state_locked(s, GEM_WS_CONNECTING);

  curl_easy_setopt(s->easy, CURLOPT_URL,            s->url);
  curl_easy_setopt(s->easy, CURLOPT_CONNECT_ONLY,   2L);
  curl_easy_setopt(s->easy, CURLOPT_NOSIGNAL,       1L);
  curl_easy_setopt(s->easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(s->easy, CURLOPT_TIMEOUT,        10L);
  curl_easy_setopt(s->easy, CURLOPT_USERAGENT,      "libp0ada/2.0");
  curl_easy_setopt(s->easy, CURLOPT_HTTPHEADER,     hdrs);

  // Body lengths only — values are secrets, never logged.
  clam(CLAM_DEBUG, s->log_ctx,
      "open: handshake nonce=%" PRIu64 " payload_b64_len=%zu sig_len=%zu",
      nonce, strlen(payload_b64), strlen(sig_hex));

  rc = curl_easy_perform(s->easy);

  // The HTTPHEADER list must live until the easy handle is freed. We
  // tear it down right after the handshake completes either way.
  curl_easy_setopt(s->easy, CURLOPT_HTTPHEADER, NULL);
  curl_slist_free_all(hdrs);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, s->log_ctx, "open: handshake failed url='%s' rc=%d (%s)",
        s->url, (int)rc, curl_easy_strerror(rc));

    curl_easy_cleanup(s->easy);
    s->easy = NULL;

    return(FAIL);
  }

  if(curl_easy_getinfo(s->easy, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK
      || sock == CURL_SOCKET_BAD)
  {
    clam(CLAM_WARN, s->log_ctx,
        "open: no socket available after handshake url='%s'", s->url);

    curl_easy_cleanup(s->easy);
    s->easy   = NULL;
    s->sockfd = CURL_SOCKET_BAD;

    return(FAIL);
  }

  s->sockfd = sock;

  return(SUCCESS);
}

static bool
gem_ws_open_locked(gem_ws_session_t *s)
{
  bool ok;

  if(s->easy != NULL)
    gem_ws_close_locked(s);

  gem_ws_reload_url_locked(s);

  if(s->url[0] == '\0')
  {
    clam(CLAM_WARN, s->log_ctx, "open: no URL configured");
    return(FAIL);
  }

  if(s->sid == GEM_WS_OE)
    ok = gem_ws_open_oe_locked(s);
  else
    ok = gem_ws_open_md_locked(s);

  if(ok != SUCCESS)
    return(FAIL);

  s->last_frame_ms = gem_ws_now_ms();
  s->last_ping_ms  = s->last_frame_ms;
  s->rx_len        = 0;
  s->backoff_ms    = 0;
  s->backoff_until = 0;
  s->consec_fails  = 0;
  s->last_crit_log = 0;

  gem_ws_set_state_locked(s, GEM_WS_OPEN);

  clam(CLAM_INFO, s->log_ctx, "opened url='%s'", s->url);

  return(SUCCESS);
}

static void
gem_ws_close_locked(gem_ws_session_t *s)
{
  if(s->easy != NULL)
  {
    size_t sent = 0;

    // Courtesy close frame. Errors are expected (session may already
    // be half-closed) and irrelevant — we are tearing down anyway.
    (void)curl_ws_send(s->easy, NULL, 0, &sent, 0, CURLWS_CLOSE);

    curl_easy_cleanup(s->easy);
    s->easy = NULL;
  }

  s->sockfd = CURL_SOCKET_BAD;
  s->rx_len = 0;
}

static void
gem_ws_schedule_reconnect_locked(gem_ws_session_t *s, const char *why)
{
  uint32_t base;
  time_t   now;

  gem_ws_close_locked(s);

  base = s->reconnect_base_ms;

  if(base == 0)
    base = 2000;

  if(s->backoff_ms == 0)
    s->backoff_ms = base;
  else
    s->backoff_ms *= 2;

  if(s->backoff_ms > GEM_WS_MAX_BACKOFF_MS)
    s->backoff_ms = GEM_WS_MAX_BACKOFF_MS;

  now              = time(NULL);
  s->backoff_until = now + (time_t)(s->backoff_ms / 1000);

  s->consec_fails++;

  // Flap-storm throttle. Past GEM_WS_MAX_CONSEC_FAIL we stop spamming
  // INFO and emit one WARN per minute so the operator sees the trend
  // without drowning in the log.
  if(s->consec_fails >= GEM_WS_MAX_CONSEC_FAIL)
  {
    if(now - s->last_crit_log >= 60)
    {
      clam(CLAM_WARN, s->log_ctx,
          "flapping: %u consecutive failures (last why=%s)",
          s->consec_fails, why != NULL ? why : "?");

      s->last_crit_log = now;
    }
  }
  else
    clam(CLAM_INFO, s->log_ctx,
        "reconnect in %u ms (why=%s)",
        s->backoff_ms, why != NULL ? why : "?");

  gem_ws_set_state_locked(s, GEM_WS_RECONNECTING);
}

// ------------------------------------------------------------------ //
// Ping / pong                                                         //
// ------------------------------------------------------------------ //

static bool
gem_ws_send_ping_locked(gem_ws_session_t *s)
{
  CURLcode rc;
  size_t   sent = 0;

  if(s->easy == NULL)
    return(FAIL);

  rc = curl_ws_send(s->easy, "p", 1, &sent, 0, CURLWS_PING);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, s->log_ctx, "ping failed rc=%d", (int)rc);
    return(FAIL);
  }

  s->last_ping_ms = gem_ws_now_ms();

  clam(CLAM_DEBUG3, s->log_ctx, "ping sent");

  return(SUCCESS);
}

static bool
gem_ws_send_pong_locked(gem_ws_session_t *s, const char *buf, size_t len)
{
  CURLcode rc;
  size_t   sent = 0;

  if(s->easy == NULL)
    return(FAIL);

  rc = curl_ws_send(s->easy, buf, len, &sent, 0, CURLWS_PONG);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, s->log_ctx, "pong failed rc=%d", (int)rc);
    return(FAIL);
  }

  clam(CLAM_DEBUG3, s->log_ctx, "pong sent (%zu bytes)", len);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Frame reassembly                                                    //
// ------------------------------------------------------------------ //

static bool
gem_ws_rx_append_locked(gem_ws_session_t *s, const char *data, size_t len)
{
  size_t need;

  if(len == 0)
    return(SUCCESS);

  need = s->rx_len + len;

  if(need > GEM_WS_ASSEMBLY_CAP)
  {
    clam(CLAM_WARN, s->log_ctx, "reassembly overflow (%zu > %u)",
        need, GEM_WS_ASSEMBLY_CAP);
    return(FAIL);
  }

  if(need > s->rx_cap)
  {
    size_t new_cap = s->rx_cap == 0 ? GEM_WS_RECV_BUF_SZ : s->rx_cap;

    while(new_cap < need)
      new_cap *= 2;

    if(new_cap > GEM_WS_ASSEMBLY_CAP)
      new_cap = GEM_WS_ASSEMBLY_CAP;

    if(s->rx_buf == NULL)
      s->rx_buf = mem_alloc(s->log_ctx, "rx", new_cap);
    else
      s->rx_buf = mem_realloc(s->rx_buf, new_cap);

    s->rx_cap = new_cap;
  }

  memcpy(s->rx_buf + s->rx_len, data, len);
  s->rx_len += len;

  return(SUCCESS);
}

static void
gem_ws_on_frame_locked(gem_ws_session_t *s, const char *data, size_t len,
    const struct curl_ws_frame *meta)
{
  unsigned int flags = (unsigned int)meta->flags;

  // Any byte from the peer is liveness proof: freshen the idle clock
  // and drop the flap-storm counter back to zero.
  s->last_frame_ms = gem_ws_now_ms();
  s->consec_fails  = 0;
  s->backoff_ms    = 0;
  s->backoff_until = 0;

  if(flags & CURLWS_PING)
  {
    gem_ws_send_pong_locked(s, data, len);
    return;
  }

  if(flags & CURLWS_PONG)
  {
    clam(CLAM_DEBUG3, s->log_ctx, "pong recv (%zu bytes)", len);
    return;
  }

  if(flags & CURLWS_CLOSE)
  {
    gem_ws_schedule_reconnect_locked(s, "peer close");
    return;
  }

  // Gemini never sends binary frames — drop them and keep going.
  if(flags & CURLWS_BINARY)
  {
    clam(CLAM_DEBUG2, s->log_ctx,
        "ignoring binary frame (%zu bytes)", len);
    return;
  }

  // Only TEXT (+ CONT continuations) remains.
  if(!(flags & (CURLWS_TEXT | CURLWS_CONT)))
    return;

  if(gem_ws_rx_append_locked(s, data, len) != SUCCESS)
  {
    gem_ws_schedule_reconnect_locked(s, "reassembly overflow");
    return;
  }

  // bytesleft == 0 means the reassembled frame is complete. Drop the
  // lock during dispatch so a subscriber callback may call back into
  // gem_ws_send_text (e.g. a follow-up subscribe) without self-
  // deadlocking. Safe because this thread is the sole reader, so no
  // concurrent recv can touch rx_buf while dispatch runs.
  if(meta->bytesleft == 0 && s->rx_len > 0)
  {
    const char *payload = s->rx_buf;
    size_t      plen    = s->rx_len;

    s->rx_len = 0;

    pthread_mutex_unlock(&s->lock);

    gem_ws_dispatch_frame(s->sid, payload, plen);

    pthread_mutex_lock(&s->lock);
  }
}

// ------------------------------------------------------------------ //
// Reader task — owns curl_ws_recv; holds sess->lock around every op   //
// except poll() (released so gem_ws_send_text can fire) and dispatch  //
// (released so subscribers can reentrantly call gem_ws_send_text).    //
// ------------------------------------------------------------------ //

static void
gem_ws_reader(task_t *t)
{
  gem_ws_session_t *s = t->data;
  uint32_t          last_reconfig_gen;

  clam(CLAM_DEBUG, s->log_ctx, "reader thread started");

  last_reconfig_gen = atomic_load(&gem_ws_reconfig_gen);

  while(!pool_shutting_down())
  {
    bool     want_open;
    uint64_t now_ms;
    uint32_t cur_gen;

    // Pick up any pending config change posted by the KV callback.
    // Compare generations rather than exchange so the *other* reader
    // also gets a chance to see this generation on its tick.
    cur_gen = atomic_load(&gem_ws_reconfig_gen);

    if(cur_gen != last_reconfig_gen)
    {
      last_reconfig_gen = cur_gen;
      pthread_mutex_lock(&s->lock);
      gem_ws_apply_reconfig_locked(s);
      pthread_mutex_unlock(&s->lock);
    }

    pthread_mutex_lock(&s->lock);

    if(s->exit_requested)
    {
      pthread_mutex_unlock(&s->lock);
      break;
    }

    // Refresh the reconnect-base knob on every tick so operators can
    // tune it at runtime without reloading the plugin.
    s->reconnect_base_ms = (uint32_t)kv_get_uint("plugin.gemini.ws_reconnect_ms");

    if(s->reconnect_base_ms == 0)
      s->reconnect_base_ms = 2000;

    want_open = s->enabled;

    // Disabled: ensure session is closed and idle in DISCONNECTED.
    if(!want_open)
    {
      if(s->state != GEM_WS_DISCONNECTED)
      {
        gem_ws_close_locked(s);
        gem_ws_set_state_locked(s, GEM_WS_DISCONNECTED);
        s->backoff_until = 0;
        s->backoff_ms    = 0;
        s->consec_fails  = 0;
      }

      pthread_mutex_unlock(&s->lock);

      {
        struct timespec ts =
            { .tv_sec = 0, .tv_nsec = (long)GEM_WS_POLL_MS * 1000L * 1000L };
        nanosleep(&ts, NULL);
      }

      continue;
    }

    // Reconnect scheduling. Stay in backoff until backoff_until elapses.
    if(s->state == GEM_WS_DISCONNECTED || s->state == GEM_WS_RECONNECTING)
    {
      time_t now = time(NULL);

      if(s->backoff_until > now)
      {
        pthread_mutex_unlock(&s->lock);

        {
          struct timespec ts =
              { .tv_sec = 0, .tv_nsec = (long)GEM_WS_POLL_MS * 1000L * 1000L };
          nanosleep(&ts, NULL);
        }

        continue;
      }

      if(gem_ws_open_locked(s) != SUCCESS)
      {
        gem_ws_schedule_reconnect_locked(s, "handshake failed");
        pthread_mutex_unlock(&s->lock);
        continue;
      }

      // Session just went OPEN — drop the transport lock so the
      // channel multiplexer's resubscribe path can re-enter
      // gem_ws_send_text (which re-acquires the lock). Lock order is
      // gem_ws_ch.mu (outer) → s->lock (inner); holding s->lock while
      // taking gem_ws_ch.mu would invert and deadlock.
      pthread_mutex_unlock(&s->lock);
      gem_ws_channels_on_open(s->sid);
      pthread_mutex_lock(&s->lock);
    }

    if(s->state != GEM_WS_OPEN)
    {
      pthread_mutex_unlock(&s->lock);
      continue;
    }

    // Re-read the wall clock: the open path above can advance
    // last_frame_ms past the now_ms we captured before the handshake,
    // which would wrap the unsigned subtraction below and trip the
    // watchdog on a fresh session.
    now_ms = gem_ws_now_ms();

    // Idle-timeout watchdog. A session that has gone quiet beyond the
    // configured idle window is presumed wedged; drop and reconnect.
    if(now_ms - s->last_frame_ms > GEM_WS_IDLE_TIMEOUT_MS)
    {
      clam(CLAM_WARN, s->log_ctx,
          "idle timeout (%u ms) — forcing reconnect",
          GEM_WS_IDLE_TIMEOUT_MS);
      gem_ws_schedule_reconnect_locked(s, "idle timeout");
      pthread_mutex_unlock(&s->lock);
      continue;
    }

    // Keepalive ping. The pong updates last_frame_ms, arresting the
    // idle watchdog on an otherwise-silent connection.
    if(now_ms - s->last_ping_ms > GEM_WS_PING_INTERVAL_MS)
      gem_ws_send_ping_locked(s);

    {
      curl_socket_t sock = s->sockfd;

      pthread_mutex_unlock(&s->lock);

      {
        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int           pr;

        pr = poll(&pfd, 1, GEM_WS_POLL_MS);

        if(pr < 0 && errno != EINTR)
        {
          clam(CLAM_WARN, s->log_ctx, "poll error: %s", strerror(errno));

          pthread_mutex_lock(&s->lock);
          gem_ws_schedule_reconnect_locked(s, "poll error");
          pthread_mutex_unlock(&s->lock);

          continue;
        }

        if(pr <= 0)
          continue;   // EINTR or timeout — re-check watchdogs on next tick
      }

      // Socket has data. Drain whatever libcurl has buffered, capped
      // at 64 iterations so a flood can't monopolize the lock
      // indefinitely.
      pthread_mutex_lock(&s->lock);

      for(int i = 0; i < 64; i++)
      {
        char                         buf[GEM_WS_RECV_BUF_SZ];
        size_t                       rlen = 0;
        const struct curl_ws_frame  *meta = NULL;
        CURLcode                     rc;

        if(s->easy == NULL || s->state != GEM_WS_OPEN)
          break;

        rc = curl_ws_recv(s->easy, buf, sizeof(buf), &rlen, &meta);

        if(rc == CURLE_AGAIN)
          break;

        if(rc != CURLE_OK)
        {
          clam(CLAM_WARN, s->log_ctx, "recv error rc=%d (%s)",
              (int)rc, curl_easy_strerror(rc));
          gem_ws_schedule_reconnect_locked(s, "recv error");
          break;
        }

        if(meta == NULL)
          break;

        gem_ws_on_frame_locked(s, buf, rlen, meta);
      }

      pthread_mutex_unlock(&s->lock);
    }
  }

  // Thread exit. Drop the session and announce liveness=false so the
  // gem_ws_stop cond_timedwait can release.
  pthread_mutex_lock(&s->lock);
  gem_ws_close_locked(s);
  gem_ws_set_state_locked(s, GEM_WS_DISCONNECTED);
  s->thread_alive = false;
  pthread_cond_broadcast(&s->lifecycle_cond);
  pthread_mutex_unlock(&s->lock);

  clam(CLAM_DEBUG, s->log_ctx, "reader thread exited");

  t->state = TASK_ENDED;
}
