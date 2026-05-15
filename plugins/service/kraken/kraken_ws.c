// botmanager — MIT
// Kraken WebSocket v2 transport.
//
// Owns a single libcurl WebSocket session against `wss://ws.kraken.com/v2`.
// A dedicated persist task runs the recv loop; reconnect is driven by
// exponential backoff with KR_WS_MAX_BACKOFF_MS as the cap and the
// `plugin.kraken.ws_reconnect_ms` KV as the base. Liveness watchdogs
// pair an idle timeout (drop after KR_WS_IDLE_TIMEOUT_MS) with client-
// side pings every KR_WS_PING_INTERVAL_MS.
//
// Raw text frames are reassembled across chunks and forwarded to
// kr_ws_channels_dispatch via kr_ws_dispatch_frame.
//
// The token cache lives here too. Every consumer subscribe path that
// needs an authenticated payload (executions, balances) snapshots the
// cached value via kr_ws_token_snapshot. The cache is monotonic — never
// zeroed past the first successful fetch — so a stale snapshot still
// flushes through Kraken's gateway and fails clean if rejected, while a
// background refresh maintains freshness.

#define KR_INTERNAL
#include "kraken.h"

#include "json.h"
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
  CURL           *easy;
  curl_socket_t   sockfd;
  kr_ws_state_t   state;

  pthread_mutex_t lock;             // guards easy, state, rx_buf, thread_alive

  task_t         *reader;
  bool            exit_requested;
  bool            thread_alive;
  pthread_cond_t  lifecycle_cond;   // signalled when thread_alive clears

  bool            enabled;          // latched copy of plugin.kraken.ws_enabled

  char            url[KR_URL_SZ];
  uint32_t        reconnect_base_ms;
  uint32_t        backoff_ms;
  time_t          backoff_until;
  uint32_t        consec_fails;
  time_t          last_crit_log;

  uint64_t        last_frame_ms;
  uint64_t        last_ping_ms;

  // Reassembly buffer for multi-chunk text frames. Grown on demand;
  // held across reconnects so normal operation never re-allocs.
  char           *rx_buf;
  size_t          rx_len;
  size_t          rx_cap;
} kr_ws_t;

static kr_ws_t kr_ws;

// Set from a KV change callback; cleared by the reader on its next tick
// so the reader — not the caller's thread — owns the state transition.
static _Atomic bool kr_ws_reconfig_req;

// Internal helpers. All *_locked helpers require kr_ws.lock held by caller.

static void     kr_ws_reader              (task_t *t);
static bool     kr_ws_open_locked         (kr_ws_t *w);
static void     kr_ws_close_locked        (kr_ws_t *w);
static void     kr_ws_on_frame_locked     (kr_ws_t *w, const char *data,
                    size_t len, const struct curl_ws_frame *meta);
static bool     kr_ws_send_ping_locked    (kr_ws_t *w);
static bool     kr_ws_send_pong_locked    (kr_ws_t *w, const char *buf,
                    size_t len);
static bool     kr_ws_rx_append_locked    (kr_ws_t *w, const char *data,
                    size_t len);
static void     kr_ws_schedule_reconnect_locked(kr_ws_t *w, const char *why);
static void     kr_ws_set_state_locked    (kr_ws_t *w, kr_ws_state_t s);
static void     kr_ws_reload_url_locked   (kr_ws_t *w);
static void     kr_ws_apply_reconfig_locked(kr_ws_t *w);
static void     kr_ws_kv_cb               (const char *key, void *data);
static uint64_t kr_ws_now_ms              (void);

// ------------------------------------------------------------------ //
// Public surface — state helper, send, dispatch                       //
// ------------------------------------------------------------------ //

const char *
kr_ws_state_name(kr_ws_state_t s)
{
  switch(s)
  {
    case KR_WS_DISCONNECTED: return("DISCONNECTED");
    case KR_WS_CONNECTING:   return("CONNECTING");
    case KR_WS_OPEN:         return("OPEN");
    case KR_WS_RECONNECTING: return("RECONNECTING");
  }

  return("UNKNOWN");
}

void
kr_ws_dispatch_frame(const char *buf, size_t len)
{
  if(buf == NULL || len == 0)
    return;

  kr_ws_channels_dispatch(buf, len);
}

bool
kr_ws_send_text(const char *buf, size_t len)
{
  CURLcode rc;
  size_t   sent = 0;
  bool     ok   = FAIL;

  if(buf == NULL || len == 0)
    return(FAIL);

  pthread_mutex_lock(&kr_ws.lock);

  if(kr_ws.state == KR_WS_OPEN && kr_ws.easy != NULL)
  {
    rc = curl_ws_send(kr_ws.easy, buf, len, &sent, 0, CURLWS_TEXT);

    if(rc == CURLE_OK && sent == len)
    {
      ok = SUCCESS;
      clam(CLAM_DEBUG3, KR_CTX, "ws send (%zu bytes)", len);
    }
    else
      clam(CLAM_WARN, KR_CTX,
          "ws send_text failed rc=%d sent=%zu/%zu",
          (int)rc, sent, len);
  }
  else
    clam(CLAM_DEBUG, KR_CTX, "ws send_text dropped: state=%s",
        kr_ws_state_name(kr_ws.state));

  pthread_mutex_unlock(&kr_ws.lock);

  return(ok);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

void
kr_ws_init(void)
{
  memset(&kr_ws, 0, sizeof(kr_ws));

  pthread_mutex_init(&kr_ws.lock, NULL);
  pthread_cond_init(&kr_ws.lifecycle_cond, NULL);

  kr_ws.state  = KR_WS_DISCONNECTED;
  kr_ws.sockfd = CURL_SOCKET_BAD;

  atomic_store(&kr_ws_reconfig_req, false);

  // Any config knob that materially changes which endpoint we should be
  // attached to triggers a reconnect. The callback is deliberately
  // lock-free — the reader polls kr_ws_reconfig_req at the top of each
  // iteration so there is no risk of the caller's thread taking the
  // session lock while kv_set is already holding the KV registry lock.
  kv_set_cb("plugin.kraken.ws_enabled",     kr_ws_kv_cb, &kr_ws);
  kv_set_cb("plugin.kraken.ws_url_public",  kr_ws_kv_cb, &kr_ws);
  kv_set_cb("plugin.kraken.ws_url_private", kr_ws_kv_cb, &kr_ws);

  // Token cache invalidation when creds change. The channel multiplexer
  // re-fetches lazily on the next private subscribe.
  kv_set_cb("plugin.kraken.creds.api_key",     kr_ws_kv_cb, &kr_ws);
  kv_set_cb("plugin.kraken.creds.private_key", kr_ws_kv_cb, &kr_ws);
}

void
kr_ws_start(void)
{
  kr_ws.enabled = (kv_get_uint("plugin.kraken.ws_enabled") != 0);

  if(kr_ws.reader != NULL)
    return;

  kr_ws.exit_requested = false;
  kr_ws.thread_alive   = true;

  kr_ws.reader = task_add_persist("kraken_ws", 50, kr_ws_reader, &kr_ws);

  if(kr_ws.reader == NULL)
  {
    clam(CLAM_WARN, KR_CTX, "ws: failed to spawn reader task");
    kr_ws.thread_alive = false;
    return;
  }

  clam(CLAM_INFO, KR_CTX, "ws subsystem started (enabled=%s)",
      kr_ws.enabled ? "true" : "false");
}

void
kr_ws_stop(void)
{
  struct timespec deadline;

  if(kr_ws.reader == NULL)
    return;

  pthread_mutex_lock(&kr_ws.lock);
  kr_ws.exit_requested = true;
  pthread_mutex_unlock(&kr_ws.lock);

  // Deadline for a graceful shutdown. Past this, log and press on — we
  // cannot block daemon shutdown indefinitely on a single plugin.
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += (KR_WS_STOP_WAIT_MS / 1000);

  pthread_mutex_lock(&kr_ws.lock);

  while(kr_ws.thread_alive)
  {
    int rc = pthread_cond_timedwait(&kr_ws.lifecycle_cond, &kr_ws.lock,
        &deadline);

    if(rc == ETIMEDOUT)
    {
      clam(CLAM_WARN, KR_CTX,
          "ws stop: reader did not exit within %d ms",
          KR_WS_STOP_WAIT_MS);
      break;
    }
  }

  pthread_mutex_unlock(&kr_ws.lock);

  kr_ws.reader = NULL;

  clam(CLAM_INFO, KR_CTX, "ws subsystem stopped");
}

void
kr_ws_deinit(void)
{
  if(kr_ws.reader != NULL)
    kr_ws_stop();

  pthread_mutex_lock(&kr_ws.lock);

  kr_ws_close_locked(&kr_ws);

  if(kr_ws.rx_buf != NULL)
  {
    mem_free(kr_ws.rx_buf);
    kr_ws.rx_buf = NULL;
    kr_ws.rx_cap = 0;
    kr_ws.rx_len = 0;
  }

  pthread_mutex_unlock(&kr_ws.lock);

  pthread_cond_destroy(&kr_ws.lifecycle_cond);
  pthread_mutex_destroy(&kr_ws.lock);
}

// ------------------------------------------------------------------ //
// Internals — state + reconfig                                        //
// ------------------------------------------------------------------ //

static uint64_t
kr_ws_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return(((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u));
}

static void
kr_ws_set_state_locked(kr_ws_t *w, kr_ws_state_t s)
{
  if(w->state == s)
    return;

  clam(CLAM_DEBUG, KR_CTX, "ws state %s -> %s",
      kr_ws_state_name(w->state), kr_ws_state_name(s));

  w->state = s;
}

static void
kr_ws_reload_url_locked(kr_ws_t *w)
{
  const char *base;
  size_t      blen;

  base = kv_get_str("plugin.kraken.ws_url_public");

  if(base == NULL || base[0] == '\0')
  {
    w->url[0] = '\0';
    return;
  }

  blen = strlen(base);

  if(blen >= sizeof(w->url))
  {
    w->url[0] = '\0';
    return;
  }

  memcpy(w->url, base, blen);
  w->url[blen] = '\0';
}

static void
kr_ws_kv_cb(const char *key, void *data)
{
  (void)data;

  clam(CLAM_INFO, KR_CTX,
      "ws config changed (%s); reconnect scheduled on next tick", key);

  atomic_store(&kr_ws_reconfig_req, true);
}

// Drop any live session and land in DISCONNECTED so the reader's normal
// backoff-and-reopen flow reconnects against the refreshed config on
// the next loop iteration. Called from the reader thread only.
static void
kr_ws_apply_reconfig_locked(kr_ws_t *w)
{
  w->enabled = (kv_get_uint("plugin.kraken.ws_enabled") != 0);

  if(w->state == KR_WS_OPEN || w->state == KR_WS_CONNECTING)
    kr_ws_close_locked(w);

  kr_ws_set_state_locked(w, KR_WS_DISCONNECTED);
  w->backoff_until = 0;
  w->backoff_ms    = 0;
  w->consec_fails  = 0;
}

// ------------------------------------------------------------------ //
// Connect + reconnect                                                 //
// ------------------------------------------------------------------ //

static bool
kr_ws_open_locked(kr_ws_t *w)
{
  CURLcode       rc;
  curl_socket_t  sock = CURL_SOCKET_BAD;

  if(w->easy != NULL)
    kr_ws_close_locked(w);

  kr_ws_reload_url_locked(w);

  if(w->url[0] == '\0')
  {
    clam(CLAM_WARN, KR_CTX, "ws open: no URL configured");
    return(FAIL);
  }

  w->easy = curl_easy_init();

  if(w->easy == NULL)
  {
    clam(CLAM_WARN, KR_CTX, "ws open: curl_easy_init failed");
    return(FAIL);
  }

  kr_ws_set_state_locked(w, KR_WS_CONNECTING);

  // CONNECT_ONLY=2 is the WebSocket handshake mode: curl_easy_perform
  // drives the HTTP Upgrade, then the handle stays open and usable via
  // curl_ws_send / curl_ws_recv. NOSIGNAL matches the rest of the core
  // curl subsystem — we never want SIGPIPE thrown at a worker thread.
  curl_easy_setopt(w->easy, CURLOPT_URL,            w->url);
  curl_easy_setopt(w->easy, CURLOPT_CONNECT_ONLY,   2L);
  curl_easy_setopt(w->easy, CURLOPT_NOSIGNAL,       1L);
  curl_easy_setopt(w->easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(w->easy, CURLOPT_TIMEOUT,        10L);
  curl_easy_setopt(w->easy, CURLOPT_USERAGENT,      "libp0ada/2.0");

  rc = curl_easy_perform(w->easy);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, KR_CTX, "ws open: handshake failed url='%s' rc=%d (%s)",
        w->url, (int)rc, curl_easy_strerror(rc));

    curl_easy_cleanup(w->easy);
    w->easy = NULL;

    return(FAIL);
  }

  if(curl_easy_getinfo(w->easy, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK
      || sock == CURL_SOCKET_BAD)
  {
    clam(CLAM_WARN, KR_CTX,
        "ws open: no socket available after handshake url='%s'", w->url);

    curl_easy_cleanup(w->easy);
    w->easy   = NULL;
    w->sockfd = CURL_SOCKET_BAD;

    return(FAIL);
  }

  w->sockfd        = sock;
  w->last_frame_ms = kr_ws_now_ms();
  w->last_ping_ms  = w->last_frame_ms;
  w->rx_len        = 0;
  w->backoff_ms    = 0;
  w->backoff_until = 0;
  w->consec_fails  = 0;
  w->last_crit_log = 0;

  kr_ws_set_state_locked(w, KR_WS_OPEN);

  clam(CLAM_INFO, KR_CTX, "ws opened url='%s'", w->url);

  return(SUCCESS);
}

static void
kr_ws_close_locked(kr_ws_t *w)
{
  if(w->easy != NULL)
  {
    size_t sent = 0;

    // Courtesy close frame. Errors are expected (session may already be
    // half-closed) and irrelevant — we are tearing down anyway.
    (void)curl_ws_send(w->easy, NULL, 0, &sent, 0, CURLWS_CLOSE);

    curl_easy_cleanup(w->easy);
    w->easy = NULL;
  }

  w->sockfd = CURL_SOCKET_BAD;
  w->rx_len = 0;
}

static void
kr_ws_schedule_reconnect_locked(kr_ws_t *w, const char *why)
{
  uint32_t base;
  time_t   now;

  kr_ws_close_locked(w);

  base = w->reconnect_base_ms;

  if(base == 0)
    base = 2000;

  if(w->backoff_ms == 0)
    w->backoff_ms = base;
  else
    w->backoff_ms *= 2;

  if(w->backoff_ms > KR_WS_MAX_BACKOFF_MS)
    w->backoff_ms = KR_WS_MAX_BACKOFF_MS;

  now              = time(NULL);
  w->backoff_until = now + (time_t)(w->backoff_ms / 1000);

  w->consec_fails++;

  // Flap-storm throttle. Past KR_WS_MAX_CONSEC_FAIL we stop spamming INFO
  // and emit one WARN per minute so the operator sees the trend without
  // drowning in the log.
  if(w->consec_fails >= KR_WS_MAX_CONSEC_FAIL)
  {
    if(now - w->last_crit_log >= 60)
    {
      clam(CLAM_WARN, KR_CTX,
          "ws flapping: %u consecutive failures (last why=%s)",
          w->consec_fails, why != NULL ? why : "?");

      w->last_crit_log = now;
    }
  }
  else
    clam(CLAM_INFO, KR_CTX,
        "ws reconnect in %u ms (why=%s)",
        w->backoff_ms, why != NULL ? why : "?");

  kr_ws_set_state_locked(w, KR_WS_RECONNECTING);
}

// ------------------------------------------------------------------ //
// Ping / pong                                                         //
// ------------------------------------------------------------------ //

static bool
kr_ws_send_ping_locked(kr_ws_t *w)
{
  CURLcode rc;
  size_t   sent = 0;

  if(w->easy == NULL)
    return(FAIL);

  rc = curl_ws_send(w->easy, "p", 1, &sent, 0, CURLWS_PING);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, KR_CTX, "ws ping failed rc=%d", (int)rc);
    return(FAIL);
  }

  w->last_ping_ms = kr_ws_now_ms();

  clam(CLAM_DEBUG3, KR_CTX, "ws ping sent");

  return(SUCCESS);
}

static bool
kr_ws_send_pong_locked(kr_ws_t *w, const char *buf, size_t len)
{
  CURLcode rc;
  size_t   sent = 0;

  if(w->easy == NULL)
    return(FAIL);

  rc = curl_ws_send(w->easy, buf, len, &sent, 0, CURLWS_PONG);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, KR_CTX, "ws pong failed rc=%d", (int)rc);
    return(FAIL);
  }

  clam(CLAM_DEBUG3, KR_CTX, "ws pong sent (%zu bytes)", len);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Frame reassembly                                                    //
// ------------------------------------------------------------------ //

static bool
kr_ws_rx_append_locked(kr_ws_t *w, const char *data, size_t len)
{
  size_t need;

  if(len == 0)
    return(SUCCESS);

  need = w->rx_len + len;

  if(need > KR_WS_ASSEMBLY_CAP)
  {
    clam(CLAM_WARN, KR_CTX, "ws reassembly overflow (%zu > %u)",
        need, KR_WS_ASSEMBLY_CAP);
    return(FAIL);
  }

  if(need > w->rx_cap)
  {
    size_t new_cap = w->rx_cap == 0 ? KR_WS_RECV_BUF_SZ : w->rx_cap;

    while(new_cap < need)
      new_cap *= 2;

    if(new_cap > KR_WS_ASSEMBLY_CAP)
      new_cap = KR_WS_ASSEMBLY_CAP;

    if(w->rx_buf == NULL)
      w->rx_buf = mem_alloc(KR_CTX, "ws_rx", new_cap);
    else
      w->rx_buf = mem_realloc(w->rx_buf, new_cap);

    w->rx_cap = new_cap;
  }

  memcpy(w->rx_buf + w->rx_len, data, len);
  w->rx_len += len;

  return(SUCCESS);
}

static void
kr_ws_on_frame_locked(kr_ws_t *w, const char *data, size_t len,
    const struct curl_ws_frame *meta)
{
  unsigned int flags = (unsigned int)meta->flags;

  // Any byte from the peer is liveness proof: freshen the idle clock
  // and drop the flap-storm counter back to zero.
  w->last_frame_ms = kr_ws_now_ms();
  w->consec_fails  = 0;
  w->backoff_ms    = 0;
  w->backoff_until = 0;

  if(flags & CURLWS_PING)
  {
    kr_ws_send_pong_locked(w, data, len);
    return;
  }

  if(flags & CURLWS_PONG)
  {
    clam(CLAM_DEBUG3, KR_CTX, "ws pong recv (%zu bytes)", len);
    return;
  }

  if(flags & CURLWS_CLOSE)
  {
    kr_ws_schedule_reconnect_locked(w, "peer close");
    return;
  }

  // Kraken v2 never sends binary — drop it and keep going.
  if(flags & CURLWS_BINARY)
  {
    clam(CLAM_DEBUG2, KR_CTX, "ws ignoring binary frame (%zu bytes)", len);
    return;
  }

  // Only TEXT (+ CONT continuations) remains.
  if(!(flags & (CURLWS_TEXT | CURLWS_CONT)))
    return;

  if(kr_ws_rx_append_locked(w, data, len) != SUCCESS)
  {
    kr_ws_schedule_reconnect_locked(w, "reassembly overflow");
    return;
  }

  // Dispatch only when the LOGICAL message is complete:
  //   bytesleft == 0   → current frame's payload fully delivered
  //   !(CURLWS_CONT)   → this is the final fragment of the WS message
  //                      (CURLWS_CONT is set on every fragment except the
  //                      last when a single message is split across multiple
  //                      WS frames). Kraken v2 frames stay small so this
  //                      hasn't surfaced as a parse-error storm here yet;
  //                      bug shape identical to GEM-VERIFY-1 + applied for
  //                      symmetry.
  //
  // Drop the lock during dispatch so a subscriber callback may call
  // back into kr_ws_send_text (e.g. a follow-up subscribe) without
  // self-deadlocking. Safe because this thread is the sole reader, so
  // no concurrent recv can touch rx_buf while dispatch runs.
  if(meta->bytesleft == 0
      && !(flags & CURLWS_CONT)
      && w->rx_len > 0)
  {
    const char *payload = w->rx_buf;
    size_t      plen    = w->rx_len;

    w->rx_len = 0;

    pthread_mutex_unlock(&w->lock);

    kr_ws_dispatch_frame(payload, plen);

    pthread_mutex_lock(&w->lock);
  }
}

// ------------------------------------------------------------------ //
// Reader task — owns curl_ws_recv; holds kr_ws.lock around every op   //
// except poll() (released so kr_ws_send_text can fire) and dispatch   //
// (released so subscribers can reentrantly call kr_ws_send_text).     //
// ------------------------------------------------------------------ //

static void
kr_ws_reader(task_t *t)
{
  kr_ws_t *w = t->data;

  clam(CLAM_DEBUG, KR_CTX, "ws reader thread started");

  while(!pool_shutting_down())
  {
    bool     want_open;
    uint64_t now_ms;

    // Pick up any pending config change posted by the KV callback.
    if(atomic_exchange(&kr_ws_reconfig_req, false))
    {
      pthread_mutex_lock(&w->lock);
      kr_ws_apply_reconfig_locked(w);
      pthread_mutex_unlock(&w->lock);
    }

    pthread_mutex_lock(&w->lock);

    if(w->exit_requested)
    {
      pthread_mutex_unlock(&w->lock);
      break;
    }

    // Refresh the reconnect-base knob on every tick so operators can
    // tune it at runtime without reloading the plugin.
    w->reconnect_base_ms = (uint32_t)kv_get_uint("plugin.kraken.ws_reconnect_ms");

    if(w->reconnect_base_ms == 0)
      w->reconnect_base_ms = 2000;

    want_open = w->enabled;

    // Disabled: ensure session is closed and idle in DISCONNECTED.
    if(!want_open)
    {
      if(w->state != KR_WS_DISCONNECTED)
      {
        kr_ws_close_locked(w);
        kr_ws_set_state_locked(w, KR_WS_DISCONNECTED);
        w->backoff_until = 0;
        w->backoff_ms    = 0;
        w->consec_fails  = 0;
      }

      pthread_mutex_unlock(&w->lock);

      {
        struct timespec ts =
            { .tv_sec = 0, .tv_nsec = (long)KR_WS_POLL_MS * 1000L * 1000L };
        nanosleep(&ts, NULL);
      }

      continue;
    }

    // Reconnect scheduling. Stay in backoff until backoff_until elapses.
    if(w->state == KR_WS_DISCONNECTED || w->state == KR_WS_RECONNECTING)
    {
      time_t now = time(NULL);

      if(w->backoff_until > now)
      {
        pthread_mutex_unlock(&w->lock);

        {
          struct timespec ts =
              { .tv_sec = 0, .tv_nsec = (long)KR_WS_POLL_MS * 1000L * 1000L };
          nanosleep(&ts, NULL);
        }

        continue;
      }

      if(kr_ws_open_locked(w) != SUCCESS)
      {
        kr_ws_schedule_reconnect_locked(w, "handshake failed");
        pthread_mutex_unlock(&w->lock);
        continue;
      }

      // Session just went OPEN — drop the transport lock so the channel
      // multiplexer's resubscribe path can re-enter kr_ws_send_text
      // (which re-acquires the lock). Lock order is kr_ws_ch.mu (outer)
      // → w->lock (inner); holding w->lock while taking kr_ws_ch.mu
      // would invert and deadlock.
      pthread_mutex_unlock(&w->lock);
      kr_ws_channels_on_open();
      pthread_mutex_lock(&w->lock);
    }

    if(w->state != KR_WS_OPEN)
    {
      pthread_mutex_unlock(&w->lock);
      continue;
    }

    // Re-read the wall clock: the open path above can advance
    // last_frame_ms past the now_ms we captured before the handshake,
    // which would wrap the unsigned subtraction below and trip the
    // watchdog on a fresh session.
    now_ms = kr_ws_now_ms();

    // Idle-timeout watchdog. A session that has gone quiet beyond the
    // configured idle window is presumed wedged; drop and reconnect.
    if(now_ms - w->last_frame_ms > KR_WS_IDLE_TIMEOUT_MS)
    {
      clam(CLAM_WARN, KR_CTX,
          "ws idle timeout (%u ms) — forcing reconnect",
          KR_WS_IDLE_TIMEOUT_MS);
      kr_ws_schedule_reconnect_locked(w, "idle timeout");
      pthread_mutex_unlock(&w->lock);
      continue;
    }

    // Keepalive ping. The pong updates last_frame_ms, arresting the
    // idle watchdog on an otherwise-silent connection.
    if(now_ms - w->last_ping_ms > KR_WS_PING_INTERVAL_MS)
      kr_ws_send_ping_locked(w);

    {
      curl_socket_t sock = w->sockfd;

      pthread_mutex_unlock(&w->lock);

      {
        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int           pr;

        pr = poll(&pfd, 1, KR_WS_POLL_MS);

        if(pr < 0 && errno != EINTR)
        {
          clam(CLAM_WARN, KR_CTX, "ws poll error: %s", strerror(errno));

          pthread_mutex_lock(&w->lock);
          kr_ws_schedule_reconnect_locked(w, "poll error");
          pthread_mutex_unlock(&w->lock);

          continue;
        }

        if(pr <= 0)
          continue;   // EINTR or timeout — re-check watchdogs on next tick
      }

      // Socket has data. Drain whatever libcurl has buffered, capped at
      // 64 iterations so a flood can't monopolize the lock indefinitely.
      pthread_mutex_lock(&w->lock);

      for(int i = 0; i < 64; i++)
      {
        char                         buf[KR_WS_RECV_BUF_SZ];
        size_t                       rlen = 0;
        const struct curl_ws_frame  *meta = NULL;
        CURLcode                     rc;

        if(w->easy == NULL || w->state != KR_WS_OPEN)
          break;

        rc = curl_ws_recv(w->easy, buf, sizeof(buf), &rlen, &meta);

        if(rc == CURLE_AGAIN)
          break;

        if(rc != CURLE_OK)
        {
          clam(CLAM_WARN, KR_CTX, "ws recv error rc=%d (%s)",
              (int)rc, curl_easy_strerror(rc));
          kr_ws_schedule_reconnect_locked(w, "recv error");
          break;
        }

        if(meta == NULL)
          break;

        kr_ws_on_frame_locked(w, buf, rlen, meta);
      }

      pthread_mutex_unlock(&w->lock);
    }
  }

  // Thread exit. Drop the session and announce liveness=false so the
  // kr_ws_stop cond_timedwait can release.
  pthread_mutex_lock(&w->lock);
  kr_ws_close_locked(w);
  kr_ws_set_state_locked(w, KR_WS_DISCONNECTED);
  w->thread_alive = false;
  pthread_cond_broadcast(&w->lifecycle_cond);
  pthread_mutex_unlock(&w->lock);

  clam(CLAM_DEBUG, KR_CTX, "ws reader thread exited");

  t->state = TASK_ENDED;
}

// ------------------------------------------------------------------ //
// WS authentication token cache                                       //
//                                                                      //
// Issues `POST /0/private/GetWebSocketsToken`. Coalesces concurrent    //
// fetches: every caller registers a (cb, user) waiter; the first one  //
// drives the curl roundtrip and on completion fans out the result to  //
// every queued waiter.                                                 //
// ------------------------------------------------------------------ //

#define KR_WS_TOKEN_MAX_WAITERS  32

typedef struct
{
  kr_ws_token_done_cb_t cb;
  void                 *user;
} kr_ws_token_waiter_t;

static struct
{
  pthread_mutex_t lock;
  bool            in_flight;

  // Cache. fetched_ms == 0 → no token cached.
  char            value[KR_WS_TOKEN_SZ];
  uint64_t        fetched_ms;

  kr_ws_token_waiter_t waiters[KR_WS_TOKEN_MAX_WAITERS];
  uint32_t             n_waiters;

  bool            initialized;
} kr_token = { .lock = PTHREAD_MUTEX_INITIALIZER };

bool
kr_ws_token_snapshot(char *out, size_t cap)
{
  bool      ok       = FAIL;
  uint64_t  age_ms;
  uint64_t  now_ms;

  if(out == NULL || cap == 0)
    return(FAIL);

  out[0] = '\0';

  pthread_mutex_lock(&kr_token.lock);

  if(kr_token.fetched_ms != 0 && kr_token.value[0] != '\0')
  {
    now_ms = kr_ws_now_ms();
    age_ms = now_ms - kr_token.fetched_ms;

    // Snapshot is valid for the docs' 15-minute lifetime; the channel
    // multiplexer probes kr_ws_token_needs_refresh independently and
    // schedules a refresh past the 14-minute mark.
    if(age_ms < (KR_WS_TOKEN_REFRESH_MS + (60u * 1000u)))
    {
      size_t tlen = strnlen(kr_token.value, sizeof(kr_token.value));

      if(tlen < cap)
      {
        memcpy(out, kr_token.value, tlen);
        out[tlen] = '\0';
        ok = SUCCESS;
      }
    }
  }

  pthread_mutex_unlock(&kr_token.lock);

  return(ok);
}

bool
kr_ws_token_needs_refresh(void)
{
  bool      need = true;
  uint64_t  age_ms;
  uint64_t  now_ms;

  pthread_mutex_lock(&kr_token.lock);

  if(kr_token.fetched_ms != 0 && kr_token.value[0] != '\0')
  {
    now_ms = kr_ws_now_ms();
    age_ms = now_ms - kr_token.fetched_ms;
    need   = (age_ms >= KR_WS_TOKEN_REFRESH_MS);
  }

  pthread_mutex_unlock(&kr_token.lock);

  return(need);
}

// Fan out completion to every waiter, draining the queue.
static void
kr_ws_token_fan_out_locked(bool ok)
{
  kr_ws_token_waiter_t snapshot[KR_WS_TOKEN_MAX_WAITERS];
  uint32_t             n;
  uint32_t             i;

  n = kr_token.n_waiters;

  if(n > 0)
  {
    memcpy(snapshot, kr_token.waiters, sizeof(snapshot[0]) * n);
    kr_token.n_waiters = 0;
  }

  kr_token.in_flight = false;

  pthread_mutex_unlock(&kr_token.lock);

  for(i = 0; i < n; i++)
  {
    if(snapshot[i].cb != NULL)
      snapshot[i].cb(ok, snapshot[i].user);
  }

  pthread_mutex_lock(&kr_token.lock);
}

static void
kr_ws_token_done(const curl_response_t *resp)
{
  kr_resp_kind_t      kind;
  char                errbuf[KRAKEN_ERR_SZ];
  struct json_object *root       = NULL;
  struct json_object *result_obj;
  char                token[KR_WS_TOKEN_SZ] = {0};
  bool                ok         = FAIL;

  kind = kr_classify_curl(resp, errbuf, sizeof(errbuf));

  if(kind != KR_RESP_OK)
  {
    clam(CLAM_WARN, KR_CTX, "ws token: %s", errbuf);
    goto fanout;
  }

  root = json_parse_buf(resp->body, resp->body_len, KR_CTX);

  if(root == NULL)
  {
    clam(CLAM_WARN, KR_CTX,
        "ws token: malformed JSON from GetWebSocketsToken");
    goto fanout;
  }

  result_obj = json_get_obj(root, "result");

  if(result_obj == NULL
      || !json_get_str(result_obj, "token", token, sizeof(token))
      || token[0] == '\0')
  {
    clam(CLAM_WARN, KR_CTX,
        "ws token: missing/empty token in response");
    goto fanout;
  }

  pthread_mutex_lock(&kr_token.lock);
  snprintf(kr_token.value, sizeof(kr_token.value), "%s", token);
  kr_token.fetched_ms = kr_ws_now_ms();
  pthread_mutex_unlock(&kr_token.lock);

  ok = SUCCESS;

  clam(CLAM_INFO, KR_CTX, "ws token refreshed");

fanout:
  if(root != NULL)
    json_object_put(root);

  pthread_mutex_lock(&kr_token.lock);
  kr_ws_token_fan_out_locked(ok);
  pthread_mutex_unlock(&kr_token.lock);
}

bool
kr_ws_token_acquire(kr_ws_token_done_cb_t cb, void *user)
{
  bool fire_request = false;

  pthread_mutex_lock(&kr_token.lock);

  if(cb != NULL)
  {
    if(kr_token.n_waiters >= KR_WS_TOKEN_MAX_WAITERS)
    {
      pthread_mutex_unlock(&kr_token.lock);
      clam(CLAM_WARN, KR_CTX,
          "ws token: waiter queue full (%u)", KR_WS_TOKEN_MAX_WAITERS);

      // Synchronous failure rather than silently dropping the callback.
      cb(false, user);
      return(FAIL);
    }

    kr_token.waiters[kr_token.n_waiters].cb   = cb;
    kr_token.waiters[kr_token.n_waiters].user = user;
    kr_token.n_waiters++;
  }

  if(!kr_token.in_flight)
  {
    kr_token.in_flight = true;
    fire_request       = true;
  }

  pthread_mutex_unlock(&kr_token.lock);

  if(!fire_request)
    return(SUCCESS);

  if(!kr_apikey_configured())
  {
    pthread_mutex_lock(&kr_token.lock);
    kr_ws_token_fan_out_locked(false);
    pthread_mutex_unlock(&kr_token.lock);
    return(FAIL);
  }

  // GetWebSocketsToken takes only the nonce; body is empty.
  // CURL_PRIO_TRANSACTIONAL because every private subscribe blocks on
  // a fresh token; missing this with backfill priority would let bulk
  // candle backfill starve out a live subscribe.
  if(kr_submit_private(NULL, CURL_PRIO_TRANSACTIONAL, "GetWebSocketsToken",
        NULL, 0, kr_ws_token_done) != SUCCESS)
  {
    pthread_mutex_lock(&kr_token.lock);
    kr_ws_token_fan_out_locked(false);
    pthread_mutex_unlock(&kr_token.lock);
    return(FAIL);
  }

  return(SUCCESS);
}
