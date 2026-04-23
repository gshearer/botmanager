// botmanager — MIT
// Coinbase Exchange: WebSocket transport (connect, framing, reconnect).
//
// Owns a single long-lived libcurl WebSocket session against the Exchange
// feed. A dedicated persist task runs the recv loop; reconnect is driven
// by exponential backoff with a 60 s cap that resets the moment any
// frame arrives. Liveness watchdogs pair idle-timeout (drop after
// CB_WS_IDLE_TIMEOUT_MS without a frame) with client-side pings (sent
// every CB_WS_PING_INTERVAL_MS). Raw text frames are reassembled across
// chunks and forwarded to cb_ws_dispatch_frame — CB5 replaces that stub
// with the channel multiplexer.

#define CB_INTERNAL
#include "coinbase.h"

#include "pool.h"
#include "task.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
  CURL           *easy;
  curl_socket_t   sockfd;
  cb_ws_state_t   state;

  pthread_mutex_t lock;             // guards easy, state, rx_buf, thread_alive

  task_t         *reader;
  bool            exit_requested;
  bool            thread_alive;
  pthread_cond_t  lifecycle_cond;   // signalled when thread_alive clears

  bool            enabled;          // latched copy of plugin.coinbase.ws_enabled

  char            url[CB_URL_SZ];
  uint32_t        reconnect_base_ms;
  uint32_t        backoff_ms;
  time_t          backoff_until;
  uint32_t        consec_fails;
  time_t          last_crit_log;

  uint64_t        last_frame_ms;
  uint64_t        last_ping_ms;

  // Reassembly buffer for multi-chunk text frames. Grown on demand; held
  // across reconnects so normal operation never re-allocs.
  char           *rx_buf;
  size_t          rx_len;
  size_t          rx_cap;
} cb_ws_t;

static cb_ws_t cb_ws;

// Set from a KV change callback; cleared by the reader on its next tick
// so the reader — not the caller's thread — owns the state transition.
static _Atomic bool cb_ws_reconfig_req;

// Internal helpers. All *_locked helpers require cb_ws.lock held by caller.

static void     cb_ws_reader         (task_t *t);
static bool     cb_ws_open_locked    (cb_ws_t *w);
static void     cb_ws_close_locked   (cb_ws_t *w);
static void     cb_ws_on_frame_locked(cb_ws_t *w, const char *data, size_t len,
                    const struct curl_ws_frame *meta);
static bool     cb_ws_send_ping_locked(cb_ws_t *w);
static bool     cb_ws_send_pong_locked(cb_ws_t *w, const char *buf, size_t len);
static bool     cb_ws_rx_append_locked(cb_ws_t *w, const char *data, size_t len);
static void     cb_ws_schedule_reconnect_locked(cb_ws_t *w, const char *why);
static void     cb_ws_set_state_locked(cb_ws_t *w, cb_ws_state_t s);
static void     cb_ws_reload_url_locked(cb_ws_t *w);
static void     cb_ws_apply_reconfig_locked(cb_ws_t *w);
static void     cb_ws_kv_cb          (const char *key, void *data);
static uint64_t cb_ws_now_ms         (void);

// Public surface — state helper, send, dispatch stub.

const char *
cb_ws_state_name(cb_ws_state_t s)
{
  switch(s)
  {
    case CB_WS_DISCONNECTED: return("DISCONNECTED");
    case CB_WS_CONNECTING:   return("CONNECTING");
    case CB_WS_OPEN:         return("OPEN");
    case CB_WS_RECONNECTING: return("RECONNECTING");
  }

  return("UNKNOWN");
}

// Hand the reassembled text frame off to the channel multiplexer
// (CB5). The preview-only stub used by CB4 is gone; the multiplexer
// owns per-frame JSON parse, sequence-gap accounting, and subscriber
// fanout.
void
cb_ws_dispatch_frame(const char *buf, size_t len)
{
  if(buf == NULL || len == 0)
    return;

  cb_ws_channels_dispatch(buf, len);
}

bool
cb_ws_send_json(const char *buf, size_t len)
{
  CURLcode rc;
  size_t   sent = 0;
  bool     ok   = FAIL;

  if(buf == NULL || len == 0)
    return(FAIL);

  pthread_mutex_lock(&cb_ws.lock);

  if(cb_ws.state == CB_WS_OPEN && cb_ws.easy != NULL)
  {
    rc = curl_ws_send(cb_ws.easy, buf, len, &sent, 0, CURLWS_TEXT);

    if(rc == CURLE_OK && sent == len)
    {
      ok = SUCCESS;
      clam(CLAM_DEBUG3, CB_CTX, "ws send (%zu bytes)", len);
    }

    else
      clam(CLAM_WARN, CB_CTX,
          "ws send_json failed rc=%d sent=%zu/%zu",
          (int)rc, sent, len);
  }

  else
    clam(CLAM_WARN, CB_CTX, "ws send_json dropped: state=%s",
        cb_ws_state_name(cb_ws.state));

  pthread_mutex_unlock(&cb_ws.lock);

  return(ok);
}

// Lifecycle

void
cb_ws_init(void)
{
  memset(&cb_ws, 0, sizeof(cb_ws));

  pthread_mutex_init(&cb_ws.lock, NULL);
  pthread_cond_init(&cb_ws.lifecycle_cond, NULL);

  cb_ws.state  = CB_WS_DISCONNECTED;
  cb_ws.sockfd = CURL_SOCKET_BAD;

  atomic_store(&cb_ws_reconfig_req, false);

  // Any config knob that materially changes which endpoint we should be
  // attached to triggers a reconnect. The callback is deliberately
  // lock-free — the reader polls cb_ws_reconfig_req at the top of each
  // iteration so there is no risk of the caller's thread taking the
  // session lock while kv_set is already holding the KV registry lock.
  kv_set_cb("plugin.coinbase.ws_enabled",     cb_ws_kv_cb, &cb_ws);
  kv_set_cb("plugin.coinbase.sandbox",        cb_ws_kv_cb, &cb_ws);
  kv_set_cb("plugin.coinbase.ws_url_prod",    cb_ws_kv_cb, &cb_ws);
  kv_set_cb("plugin.coinbase.ws_url_sandbox", cb_ws_kv_cb, &cb_ws);
}

void
cb_ws_start(void)
{
  cb_ws.enabled = (kv_get_uint("plugin.coinbase.ws_enabled") != 0);

  if(cb_ws.reader != NULL)
    return;

  cb_ws.exit_requested = false;
  cb_ws.thread_alive   = true;

  cb_ws.reader = task_add_persist("coinbase_ws", 50, cb_ws_reader, &cb_ws);

  if(cb_ws.reader == NULL)
  {
    clam(CLAM_WARN, CB_CTX, "ws: failed to spawn reader task");
    cb_ws.thread_alive = false;
    return;
  }

  clam(CLAM_INFO, CB_CTX, "ws subsystem started (enabled=%s)",
      cb_ws.enabled ? "true" : "false");
}

void
cb_ws_stop(void)
{
  struct timespec deadline;

  if(cb_ws.reader == NULL)
    return;

  pthread_mutex_lock(&cb_ws.lock);
  cb_ws.exit_requested = true;
  pthread_mutex_unlock(&cb_ws.lock);

  // Deadline for a graceful shutdown. Past this, log and press on — we
  // cannot block daemon shutdown indefinitely on a single plugin.
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += (CB_WS_STOP_WAIT_MS / 1000);

  pthread_mutex_lock(&cb_ws.lock);

  while(cb_ws.thread_alive)
  {
    int rc = pthread_cond_timedwait(&cb_ws.lifecycle_cond, &cb_ws.lock,
        &deadline);

    if(rc == ETIMEDOUT)
    {
      clam(CLAM_WARN, CB_CTX,
          "ws stop: reader did not exit within %d ms",
          CB_WS_STOP_WAIT_MS);
      break;
    }
  }

  pthread_mutex_unlock(&cb_ws.lock);

  cb_ws.reader = NULL;

  clam(CLAM_INFO, CB_CTX, "ws subsystem stopped");
}

void
cb_ws_deinit(void)
{
  if(cb_ws.reader != NULL)
    cb_ws_stop();

  pthread_mutex_lock(&cb_ws.lock);

  cb_ws_close_locked(&cb_ws);

  if(cb_ws.rx_buf != NULL)
  {
    mem_free(cb_ws.rx_buf);
    cb_ws.rx_buf = NULL;
    cb_ws.rx_cap = 0;
    cb_ws.rx_len = 0;
  }

  pthread_mutex_unlock(&cb_ws.lock);

  pthread_cond_destroy(&cb_ws.lifecycle_cond);
  pthread_mutex_destroy(&cb_ws.lock);
}

// Internals — state + reconfig

static uint64_t
cb_ws_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return(((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u));
}

static void
cb_ws_set_state_locked(cb_ws_t *w, cb_ws_state_t s)
{
  if(w->state == s)
    return;

  clam(CLAM_DEBUG, CB_CTX, "ws state %s -> %s",
      cb_ws_state_name(w->state), cb_ws_state_name(s));

  w->state = s;
}

static void
cb_ws_reload_url_locked(cb_ws_t *w)
{
  char url[CB_URL_SZ];

  if(cb_ws_base_url(url, sizeof(url)) != SUCCESS)
  {
    w->url[0] = '\0';
    return;
  }

  snprintf(w->url, sizeof(w->url), "%s", url);
}

static void
cb_ws_kv_cb(const char *key, void *data)
{
  (void)data;

  clam(CLAM_INFO, CB_CTX,
      "ws config changed (%s); reconnect scheduled on next tick", key);

  atomic_store(&cb_ws_reconfig_req, true);
}

// Drop any live session and land in DISCONNECTED so the reader's normal
// backoff-and-reopen flow reconnects against the refreshed config on
// the next loop iteration. Called from the reader thread only.
static void
cb_ws_apply_reconfig_locked(cb_ws_t *w)
{
  w->enabled = (kv_get_uint("plugin.coinbase.ws_enabled") != 0);

  if(w->state == CB_WS_OPEN || w->state == CB_WS_CONNECTING)
    cb_ws_close_locked(w);

  cb_ws_set_state_locked(w, CB_WS_DISCONNECTED);
  w->backoff_until = 0;
  w->backoff_ms    = 0;
  w->consec_fails  = 0;
}

// Connect + reconnect

static bool
cb_ws_open_locked(cb_ws_t *w)
{
  CURLcode       rc;
  curl_socket_t  sock = CURL_SOCKET_BAD;

  if(w->easy != NULL)
    cb_ws_close_locked(w);

  cb_ws_reload_url_locked(w);

  if(w->url[0] == '\0')
  {
    clam(CLAM_WARN, CB_CTX, "ws open: no URL configured");
    return(FAIL);
  }

  w->easy = curl_easy_init();

  if(w->easy == NULL)
  {
    clam(CLAM_WARN, CB_CTX, "ws open: curl_easy_init failed");
    return(FAIL);
  }

  cb_ws_set_state_locked(w, CB_WS_CONNECTING);

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
    clam(CLAM_WARN, CB_CTX, "ws open: handshake failed url='%s' rc=%d (%s)",
        w->url, (int)rc, curl_easy_strerror(rc));

    curl_easy_cleanup(w->easy);
    w->easy = NULL;

    return(FAIL);
  }

  if(curl_easy_getinfo(w->easy, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK
      || sock == CURL_SOCKET_BAD)
  {
    clam(CLAM_WARN, CB_CTX,
        "ws open: no socket available after handshake url='%s'", w->url);

    curl_easy_cleanup(w->easy);
    w->easy   = NULL;
    w->sockfd = CURL_SOCKET_BAD;

    return(FAIL);
  }

  w->sockfd        = sock;
  w->last_frame_ms = cb_ws_now_ms();
  w->last_ping_ms  = w->last_frame_ms;
  w->rx_len        = 0;
  w->backoff_ms    = 0;
  w->backoff_until = 0;
  w->consec_fails  = 0;
  w->last_crit_log = 0;

  cb_ws_set_state_locked(w, CB_WS_OPEN);

  clam(CLAM_INFO, CB_CTX, "ws opened url='%s'", w->url);

  return(SUCCESS);
}

static void
cb_ws_close_locked(cb_ws_t *w)
{
  if(w->easy != NULL)
  {
    size_t sent = 0;

    // Courtesy close frame. Errors are expected (session may already be
    // half-closed) and are irrelevant — we are tearing down anyway.
    (void)curl_ws_send(w->easy, NULL, 0, &sent, 0, CURLWS_CLOSE);

    curl_easy_cleanup(w->easy);
    w->easy = NULL;
  }

  w->sockfd = CURL_SOCKET_BAD;
  w->rx_len = 0;
}

static void
cb_ws_schedule_reconnect_locked(cb_ws_t *w, const char *why)
{
  uint32_t base;
  time_t   now;

  cb_ws_close_locked(w);

  base = w->reconnect_base_ms;

  if(base == 0)
    base = 2000;

  if(w->backoff_ms == 0)
    w->backoff_ms = base;
  else
    w->backoff_ms *= 2;

  if(w->backoff_ms > CB_WS_MAX_BACKOFF_MS)
    w->backoff_ms = CB_WS_MAX_BACKOFF_MS;

  now              = time(NULL);
  w->backoff_until = now + (time_t)(w->backoff_ms / 1000);

  w->consec_fails++;

  // Flap-storm throttle. Past CB_WS_MAX_CONSEC_FAIL we stop spamming INFO
  // and emit one WARN per minute so the operator sees the trend without
  // drowning in the log.
  if(w->consec_fails >= CB_WS_MAX_CONSEC_FAIL)
  {
    if(now - w->last_crit_log >= 60)
    {
      clam(CLAM_WARN, CB_CTX,
          "ws flapping: %u consecutive failures (last why=%s)",
          w->consec_fails, why != NULL ? why : "?");

      w->last_crit_log = now;
    }
  }

  else
    clam(CLAM_INFO, CB_CTX,
        "ws reconnect in %u ms (why=%s)",
        w->backoff_ms, why != NULL ? why : "?");

  cb_ws_set_state_locked(w, CB_WS_RECONNECTING);
}

// Ping / pong

static bool
cb_ws_send_ping_locked(cb_ws_t *w)
{
  CURLcode rc;
  size_t   sent = 0;

  if(w->easy == NULL)
    return(FAIL);

  rc = curl_ws_send(w->easy, "p", 1, &sent, 0, CURLWS_PING);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, CB_CTX, "ws ping failed rc=%d", (int)rc);
    return(FAIL);
  }

  w->last_ping_ms = cb_ws_now_ms();

  clam(CLAM_DEBUG3, CB_CTX, "ws ping sent");

  return(SUCCESS);
}

static bool
cb_ws_send_pong_locked(cb_ws_t *w, const char *buf, size_t len)
{
  CURLcode rc;
  size_t   sent = 0;

  if(w->easy == NULL)
    return(FAIL);

  rc = curl_ws_send(w->easy, buf, len, &sent, 0, CURLWS_PONG);

  if(rc != CURLE_OK)
  {
    clam(CLAM_WARN, CB_CTX, "ws pong failed rc=%d", (int)rc);
    return(FAIL);
  }

  clam(CLAM_DEBUG3, CB_CTX, "ws pong sent (%zu bytes)", len);

  return(SUCCESS);
}

// Frame reassembly

static bool
cb_ws_rx_append_locked(cb_ws_t *w, const char *data, size_t len)
{
  size_t need;

  if(len == 0)
    return(SUCCESS);

  need = w->rx_len + len;

  if(need > CB_WS_ASSEMBLY_CAP)
  {
    clam(CLAM_WARN, CB_CTX, "ws reassembly overflow (%zu > %u)",
        need, CB_WS_ASSEMBLY_CAP);
    return(FAIL);
  }

  if(need > w->rx_cap)
  {
    size_t new_cap = w->rx_cap == 0 ? CB_WS_RECV_BUF_SZ : w->rx_cap;

    while(new_cap < need)
      new_cap *= 2;

    if(new_cap > CB_WS_ASSEMBLY_CAP)
      new_cap = CB_WS_ASSEMBLY_CAP;

    if(w->rx_buf == NULL)
      w->rx_buf = mem_alloc(CB_CTX, "ws_rx", new_cap);
    else
      w->rx_buf = mem_realloc(w->rx_buf, new_cap);

    w->rx_cap = new_cap;
  }

  memcpy(w->rx_buf + w->rx_len, data, len);
  w->rx_len += len;

  return(SUCCESS);
}

static void
cb_ws_on_frame_locked(cb_ws_t *w, const char *data, size_t len,
    const struct curl_ws_frame *meta)
{
  unsigned int flags = (unsigned int)meta->flags;

  // Any byte from the peer is liveness proof: freshen the idle clock
  // and drop the flap-storm counter back to zero.
  w->last_frame_ms = cb_ws_now_ms();
  w->consec_fails  = 0;
  w->backoff_ms    = 0;
  w->backoff_until = 0;

  if(flags & CURLWS_PING)
  {
    cb_ws_send_pong_locked(w, data, len);
    return;
  }

  if(flags & CURLWS_PONG)
  {
    clam(CLAM_DEBUG3, CB_CTX, "ws pong recv (%zu bytes)", len);
    return;
  }

  if(flags & CURLWS_CLOSE)
  {
    cb_ws_schedule_reconnect_locked(w, "peer close");
    return;
  }

  // Coinbase Exchange never sends binary — drop it and keep going.
  if(flags & CURLWS_BINARY)
  {
    clam(CLAM_DEBUG2, CB_CTX, "ws ignoring binary frame (%zu bytes)", len);
    return;
  }

  // Only TEXT (+ CONT continuations) remains.
  if(!(flags & (CURLWS_TEXT | CURLWS_CONT)))
    return;

  if(cb_ws_rx_append_locked(w, data, len) != SUCCESS)
  {
    cb_ws_schedule_reconnect_locked(w, "reassembly overflow");
    return;
  }

  // bytesleft == 0 means the reassembled frame is complete. Drop the
  // lock during dispatch so a subscriber callback may call back into
  // cb_ws_send_json (e.g. CB5 issuing a follow-up subscribe) without
  // self-deadlocking. Safe because this thread is the sole reader, so
  // no concurrent recv can touch rx_buf while dispatch runs.
  if(meta->bytesleft == 0 && w->rx_len > 0)
  {
    const char *payload = w->rx_buf;
    size_t      plen    = w->rx_len;

    w->rx_len = 0;

    pthread_mutex_unlock(&w->lock);

    cb_ws_dispatch_frame(payload, plen);

    pthread_mutex_lock(&w->lock);
  }
}

// Reader task — owns curl_ws_recv; holds cb_ws.lock around every op
// except poll() (released so cb_ws_send_json can fire) and dispatch
// (released so CB5 subscribers can reentrantly call cb_ws_send_json).

static void
cb_ws_reader(task_t *t)
{
  cb_ws_t *w = t->data;

  clam(CLAM_DEBUG, CB_CTX, "ws reader thread started");

  while(!pool_shutting_down())
  {
    bool     want_open;
    uint64_t now_ms;

    // Pick up any pending config change posted by the KV callback.
    if(atomic_exchange(&cb_ws_reconfig_req, false))
    {
      pthread_mutex_lock(&w->lock);
      cb_ws_apply_reconfig_locked(w);
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
    w->reconnect_base_ms = (uint32_t)kv_get_uint("plugin.coinbase.ws_reconnect_ms");

    if(w->reconnect_base_ms == 0)
      w->reconnect_base_ms = 2000;

    want_open = w->enabled;
    now_ms    = cb_ws_now_ms();

    // Disabled: ensure session is closed and idle in DISCONNECTED.
    if(!want_open)
    {
      if(w->state != CB_WS_DISCONNECTED)
      {
        cb_ws_close_locked(w);
        cb_ws_set_state_locked(w, CB_WS_DISCONNECTED);
        w->backoff_until = 0;
        w->backoff_ms    = 0;
        w->consec_fails  = 0;
      }

      pthread_mutex_unlock(&w->lock);

      {
        struct timespec ts =
            { .tv_sec = 0, .tv_nsec = (long)CB_WS_POLL_MS * 1000L * 1000L };
        nanosleep(&ts, NULL);
      }

      continue;
    }

    // Reconnect scheduling. Stay in backoff until backoff_until elapses.
    if(w->state == CB_WS_DISCONNECTED || w->state == CB_WS_RECONNECTING)
    {
      time_t now = time(NULL);

      if(w->backoff_until > now)
      {
        pthread_mutex_unlock(&w->lock);

        {
          struct timespec ts =
              { .tv_sec = 0, .tv_nsec = (long)CB_WS_POLL_MS * 1000L * 1000L };
          nanosleep(&ts, NULL);
        }

        continue;
      }

      if(cb_ws_open_locked(w) != SUCCESS)
      {
        cb_ws_schedule_reconnect_locked(w, "handshake failed");
        pthread_mutex_unlock(&w->lock);
        continue;
      }

      // Session just went OPEN — drop the transport lock so the
      // channel multiplexer's resubscribe path can re-enter
      // cb_ws_send_json (which re-acquires the lock). Lock order is
      // cb_ws_ch.mu (outer) → w->lock (inner); holding w->lock while
      // taking cb_ws_ch.mu would invert and deadlock.
      pthread_mutex_unlock(&w->lock);
      cb_ws_channels_on_open();
      pthread_mutex_lock(&w->lock);
    }

    if(w->state != CB_WS_OPEN)
    {
      pthread_mutex_unlock(&w->lock);
      continue;
    }

    // Idle-timeout watchdog. A session that has gone quiet beyond the
    // configured idle window is presumed wedged; drop and reconnect.
    if(now_ms - w->last_frame_ms > CB_WS_IDLE_TIMEOUT_MS)
    {
      clam(CLAM_WARN, CB_CTX,
          "ws idle timeout (%u ms) — forcing reconnect",
          CB_WS_IDLE_TIMEOUT_MS);
      cb_ws_schedule_reconnect_locked(w, "idle timeout");
      pthread_mutex_unlock(&w->lock);
      continue;
    }

    // Keepalive ping. The pong updates last_frame_ms, arresting the
    // idle watchdog on an otherwise-silent connection.
    if(now_ms - w->last_ping_ms > CB_WS_PING_INTERVAL_MS)
      cb_ws_send_ping_locked(w);

    {
      curl_socket_t sock = w->sockfd;

      pthread_mutex_unlock(&w->lock);

      {
        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int           pr;

        pr = poll(&pfd, 1, CB_WS_POLL_MS);

        if(pr < 0 && errno != EINTR)
        {
          clam(CLAM_WARN, CB_CTX, "ws poll error: %s", strerror(errno));

          pthread_mutex_lock(&w->lock);
          cb_ws_schedule_reconnect_locked(w, "poll error");
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
        char                         buf[CB_WS_RECV_BUF_SZ];
        size_t                       rlen = 0;
        const struct curl_ws_frame  *meta = NULL;
        CURLcode                     rc;

        if(w->easy == NULL || w->state != CB_WS_OPEN)
          break;

        rc = curl_ws_recv(w->easy, buf, sizeof(buf), &rlen, &meta);

        if(rc == CURLE_AGAIN)
          break;

        if(rc != CURLE_OK)
        {
          clam(CLAM_WARN, CB_CTX, "ws recv error rc=%d (%s)",
              (int)rc, curl_easy_strerror(rc));
          cb_ws_schedule_reconnect_locked(w, "recv error");
          break;
        }

        if(meta == NULL)
          break;

        cb_ws_on_frame_locked(w, buf, rlen, meta);
      }

      pthread_mutex_unlock(&w->lock);
    }
  }

  // Thread exit. Drop the session and announce liveness=false so the
  // cb_ws_stop cond_timedwait can release.
  pthread_mutex_lock(&w->lock);
  cb_ws_close_locked(w);
  cb_ws_set_state_locked(w, CB_WS_DISCONNECTED);
  w->thread_alive = false;
  pthread_cond_broadcast(&w->lifecycle_cond);
  pthread_mutex_unlock(&w->lock);

  clam(CLAM_DEBUG, CB_CTX, "ws reader thread exited");

  t->state = TASK_ENDED;
}
