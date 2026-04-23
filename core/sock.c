// botmanager — MIT
// Unix-domain control socket listener and client session reader.
#define SOCK_INTERNAL
#include "sock.h"

// Send buffer freelist helpers

static sock_sendbuf_t *
sock_sbuf_alloc(const void *data, size_t len)
{
  sock_sendbuf_t *sb = NULL;

  pthread_mutex_lock(&sock_sbuf_mutex);

  if(sock_sbuf_free != NULL)
  {
    sb = sock_sbuf_free;
    sock_sbuf_free = sb->next;
  }

  pthread_mutex_unlock(&sock_sbuf_mutex);

  if(sb == NULL)
    sb = mem_alloc("sock", "sendbuf", sizeof(*sb));

  sb->data = mem_alloc("sock", "sbuf_data", len);
  memcpy(sb->data, data, len);
  sb->len    = len;
  sb->offset = 0;
  sb->next   = NULL;

  return(sb);
}

static void
sock_sbuf_release(sock_sendbuf_t *sb)
{
  mem_free(sb->data);
  sb->data   = NULL;
  sb->len    = 0;
  sb->offset = 0;

  pthread_mutex_lock(&sock_sbuf_mutex);
  sb->next = sock_sbuf_free;
  sock_sbuf_free = sb;
  pthread_mutex_unlock(&sock_sbuf_mutex);
}

// Internal helpers

// Deliver an event to the session's consumer callback.
static void
sock_deliver(sock_session_t *s, sock_event_type_t type,
    const uint8_t *data, size_t len, int err)
{
  sock_event_t ev;

  if(type == SOCK_EVENT_CONNECTED)
    __atomic_add_fetch(&sock_total_conn, 1, __ATOMIC_RELAXED);

  else if(type == SOCK_EVENT_DISCONNECT)
    __atomic_add_fetch(&sock_total_disc, 1, __ATOMIC_RELAXED);

  memset(&ev, 0, sizeof(ev));
  ev.type     = type;
  ev.session  = s;
  ev.data     = data;
  ev.data_len = len;
  ev.err      = err;

  s->cb(&ev, s->cb_data);
}

// Wake the epoll worker so it can check for pending sends.
static void
sock_wake_worker(sock_session_t *s)
{
  uint64_t val = 1;
  sock_worker_t *w = &sock_workers[s->worker_id];

  if(w->wake_fd >= 0)
    (void)write(w->wake_fd, &val, sizeof(val));
}

// Close the session's fd and update state.
static void
sock_session_close_fd(sock_session_t *s)
{
  // Clean up TLS before closing fd.
  sock_tls_cleanup(s);

  if(s->fd >= 0)
  {
    // Remove from epoll (best-effort; may already be removed).
    sock_worker_t *w = &sock_workers[s->worker_id];

    if(w->epoll_fd >= 0)
      epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);

    close(s->fd);
    s->fd = -1;
  }

  s->state = SOCK_STATE_CLOSED;
}

static bool
sock_drain_sendq(sock_session_t *s)
{
  pthread_mutex_lock(&s->send_lock);

  while(s->send_head != NULL)
  {
    sock_sendbuf_t *sb = s->send_head;
    size_t remaining = sb->len - sb->offset;
    ssize_t n;

    if(s->tls != NULL)
    {
      n = SSL_write(s->tls, sb->data + sb->offset, (int)remaining);

      if(n <= 0)
      {
        int ssl_err = SSL_get_error(s->tls, (int)n);

        if(ssl_err == SSL_ERROR_WANT_WRITE ||
            ssl_err == SSL_ERROR_WANT_READ)
        {
          pthread_mutex_unlock(&s->send_lock);
          return(false);
        }

        // Fatal TLS write error.
        pthread_mutex_unlock(&s->send_lock);
        return(false);
      }
    }

    else
    {
      n = send(s->fd, sb->data + sb->offset, remaining, MSG_NOSIGNAL);

      if(n < 0)
      {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
          pthread_mutex_unlock(&s->send_lock);
          return(false);
        }

        // Real error — will be caught by EPOLLERR.
        pthread_mutex_unlock(&s->send_lock);
        return(false);
      }
    }

    sb->offset += (size_t)n;
    s->send_queued -= (uint32_t)n;
    s->bytes_out += (uint64_t)n;

    __atomic_fetch_add(&sock_total_out, (uint64_t)n, __ATOMIC_RELAXED);

    if(sb->offset >= sb->len)
    {
      s->send_head = sb->next;

      if(s->send_head == NULL)
        s->send_tail = NULL;

      sock_sbuf_release(sb);
    }

    else
    {
      // Partial send — stop here, wait for next EPOLLOUT.
      pthread_mutex_unlock(&s->send_lock);
      return(false);
    }
  }

  pthread_mutex_unlock(&s->send_lock);
  return(true);
}

// Arm or disarm EPOLLOUT for a session.
static void
sock_epoll_rearm(sock_worker_t *w, sock_session_t *s, bool want_out)
{
  uint32_t events = EPOLLIN | EPOLLET;
  struct epoll_event ev;

  if(want_out)
    events |= EPOLLOUT;

  ev.events  = events;
  ev.data.ptr = s;

  epoll_ctl(w->epoll_fd, EPOLL_CTL_MOD, s->fd, &ev);
  s->epollout_armed = want_out;
}

// Set TCP keepalive on a connected socket.
static void
sock_set_keepalive(int fd, uint32_t interval)
{
  int yes = 1;
  int cnt = 3;

  if(interval == 0)
    return;

  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &interval, sizeof(interval));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));

  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

// TLS helpers

// Free TLS resources for a session.
static void
sock_tls_cleanup(sock_session_t *s)
{
  if(s->tls != NULL)
  {
    SSL_shutdown(s->tls);
    SSL_free(s->tls);
    s->tls = NULL;
  }

  if(s->tls_ctx != NULL)
  {
    SSL_CTX_free(s->tls_ctx);
    s->tls_ctx = NULL;
  }
}

// Attempt the TLS handshake (non-blocking). Returns true if the
// handshake completed (success or fatal error). Returns false if
// the handshake needs more I/O (WANT_READ/WANT_WRITE).
static bool
sock_tls_handshake(sock_session_t *s)
{
  int rc;
  int err;
  sock_worker_t *w;
  unsigned long ssl_err;
  char errbuf[128];

  rc = SSL_connect(s->tls);
  if(rc == 1)
  {
    // Handshake complete.
    s->state = SOCK_STATE_CONNECTED;
    s->last_activity = time(NULL);
    s->connected_at  = s->last_activity;

    w = &sock_workers[s->worker_id];
    sock_epoll_rearm(w, s, false);

    clam(CLAM_INFO, "sock", "[%s] TLS handshake complete (%s)",
        s->name, SSL_get_version(s->tls));

    sock_deliver(s, SOCK_EVENT_CONNECTED, NULL, 0, 0);
    return(true);
  }

  err = SSL_get_error(s->tls, rc);
  if(err == SSL_ERROR_WANT_READ)
  {
    w = &sock_workers[s->worker_id];
    sock_epoll_rearm(w, s, false);
    return(false);
  }

  if(err == SSL_ERROR_WANT_WRITE)
  {
    w = &sock_workers[s->worker_id];
    sock_epoll_rearm(w, s, true);
    return(false);
  }

  // Fatal handshake error.
  ssl_err = ERR_get_error();
  if(ssl_err != 0)
    ERR_error_string_n(ssl_err, errbuf, sizeof(errbuf));
  else
    snprintf(errbuf, sizeof(errbuf), "SSL_connect returned %d (err=%d)",
        rc, err);

  clam(CLAM_WARN, "sock", "[%s] TLS handshake failed: %s",
      s->name, errbuf);

  sock_tls_cleanup(s);
  sock_session_close_fd(s);
  sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, ECONNABORTED);
  return(true);
}

// Unix domain socket connect task (runs on a pool worker thread)

static void
sock_unix_connect_task(task_t *t)
{
  sock_session_t *s = t->data;
  int fd;
  int rc;
  struct sockaddr_un addr;

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if(fd < 0)
  {
    clam(CLAM_WARN, "sock", "[%s] socket(AF_UNIX) failed: %s",
        s->name, strerror(errno));
    sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, errno);
    s->state = SOCK_STATE_CLOSED;
    t->state = TASK_ENDED;
    return;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->path);

  rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if(rc == 0)
  {
    sock_worker_t *w = &sock_workers[s->worker_id];
    struct epoll_event ev;

    // Immediate connect.
    s->fd = fd;
    s->state = SOCK_STATE_CONNECTED;
    s->last_activity = time(NULL);
    s->connected_at  = s->last_activity;

    // Register with epoll.
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.ptr = s;
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    sock_deliver(s, SOCK_EVENT_CONNECTED, NULL, 0, 0);
  }

  else if(errno == EINPROGRESS)
  {
    sock_worker_t *w = &sock_workers[s->worker_id];
    struct epoll_event ev;

    s->fd = fd;
    s->state = SOCK_STATE_CONNECTING;

    ev.events   = EPOLLOUT | EPOLLET;
    ev.data.ptr = s;
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    sock_wake_worker(s);
  }

  else
  {
    clam(CLAM_WARN, "sock", "[%s] connect(unix:%s) failed: %s",
        s->name, s->path, strerror(errno));
    close(fd);
    sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, errno);
    s->state = SOCK_STATE_CLOSED;
  }

  t->state = TASK_ENDED;
}

// Resolve callback for TCP/UDP connections

static void
sock_resolve_done(const resolve_result_t *result)
{
  sock_session_t *s = result->user_data;
  int socktype;
  int fd = -1;
  int rc = -1;
  sock_worker_t *w;

  if(result->status != 0 || result->count == 0)
  {
    clam(CLAM_WARN, "sock", "[%s] DNS failed for '%s': %s",
        s->name, s->host,
        result->error ? result->error : "no records");
    sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, EHOSTUNREACH);
    s->state = SOCK_STATE_CLOSED;
    return;
  }

  // Determine socket type for socket() call.
  if(s->type == SOCK_TCP)
    socktype = SOCK_STREAM;
  else if(s->type == SOCK_UDP)
    socktype = SOCK_DGRAM;
  else
  {
    clam(CLAM_WARN, "sock", "[%s] ICMP sockets not yet implemented",
        s->name);
    sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, ENOSYS);
    s->state = SOCK_STATE_CLOSED;
    return;
  }

  // Try each resolved address.
  for(uint32_t i = 0; i < result->count; i++)
  {
    const resolve_record_t *rec = &result->records[i];
    const struct sockaddr_storage *sa;
    socklen_t sa_len;
    struct sockaddr_storage sa_copy;

    if(rec->type == RESOLVE_A)
    {
      sa     = &rec->a.sa;
      sa_len = rec->a.sa_len;
    }

    else if(rec->type == RESOLVE_AAAA)
    {
      sa     = &rec->aaaa.sa;
      sa_len = rec->aaaa.sa_len;
    }

    else
      continue;

    // Set port in the sockaddr (resolve doesn't know it).
    memcpy(&sa_copy, sa, sa_len);

    if(sa_copy.ss_family == AF_INET)
      ((struct sockaddr_in *)&sa_copy)->sin_port = htons(s->port);
    else if(sa_copy.ss_family == AF_INET6)
      ((struct sockaddr_in6 *)&sa_copy)->sin6_port = htons(s->port);

    fd = socket(sa_copy.ss_family,
        socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if(fd < 0)
      continue;

    rc = connect(fd, (struct sockaddr *)&sa_copy, sa_len);

    if(rc == 0 || errno == EINPROGRESS)
      break;

    close(fd);
    fd = -1;
  }

  if(fd < 0)
  {
    clam(CLAM_WARN, "sock", "[%s] all addresses failed for %s:%u",
        s->name, s->host, s->port);
    sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, ECONNREFUSED);
    s->state = SOCK_STATE_CLOSED;
    return;
  }

  s->fd = fd;

  // Apply TCP keepalive if configured.
  if(s->type == SOCK_TCP)
    sock_set_keepalive(fd, sock_cfg.keepalive);

  w = &sock_workers[s->worker_id];

  if(rc == 0)
  {
    struct epoll_event ev;

    // Immediate connect.
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.ptr = s;
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    if(s->tls_enabled)
    {
      clam(CLAM_INFO, "sock", "[%s] TCP connected to %s:%u, starting TLS",
          s->name, s->host, s->port);

      SSL_set_fd(s->tls, s->fd);
      SSL_set_tlsext_host_name(s->tls, s->host);
      s->state = SOCK_STATE_TLS_HANDSHAKE;
      sock_tls_handshake(s);
    }

    else
    {
      s->state = SOCK_STATE_CONNECTED;
      s->last_activity = time(NULL);
      s->connected_at  = s->last_activity;

      clam(CLAM_INFO, "sock", "[%s] connected to %s:%u",
          s->name, s->host, s->port);

      sock_deliver(s, SOCK_EVENT_CONNECTED, NULL, 0, 0);
    }
  }

  else
  {
    struct epoll_event ev;

    // EINPROGRESS — register for EPOLLOUT to detect completion.
    s->state = SOCK_STATE_CONNECTING;

    ev.events   = EPOLLOUT | EPOLLET;
    ev.data.ptr = s;
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    sock_wake_worker(s);
  }
}

// Timeout checking (called from epoll loop)

static void
sock_check_timeouts(void)
{
  time_t now;
  sock_session_t *s;

  now = time(NULL);

  pthread_mutex_lock(&sock_mutex);

  s = sock_list;

  while(s != NULL)
  {
    sock_session_t *next = s->next;

    // Connect timeout.
    if((s->state == SOCK_STATE_CONNECTING ||
        s->state == SOCK_STATE_RESOLVING ||
        s->state == SOCK_STATE_TLS_HANDSHAKE) &&
        sock_cfg.connect_timeout > 0 &&
        s->connect_started > 0)
    {
      time_t elapsed = now - s->connect_started;

      if(elapsed > (time_t)sock_cfg.connect_timeout)
      {
        clam(CLAM_WARN, "sock", "[%s] connect timed out after %lds",
            s->name, (long)elapsed);

        // Must unlock sock_mutex before delivering event (callback
        // may call sock_destroy which takes sock_mutex).
        pthread_mutex_unlock(&sock_mutex);
        sock_session_close_fd(s);
        sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, ETIMEDOUT);
        pthread_mutex_lock(&sock_mutex);

        // Restart scan — list may have changed.
        s = sock_list;
        continue;
      }
    }

    // Idle timeout.
    if(s->state == SOCK_STATE_CONNECTED &&
        sock_cfg.idle_timeout > 0 &&
        s->last_activity > 0)
    {
      time_t idle = now - s->last_activity;

      if(idle > (time_t)sock_cfg.idle_timeout)
      {
        clam(CLAM_INFO, "sock", "[%s] idle timeout after %lds",
            s->name, (long)idle);

        pthread_mutex_unlock(&sock_mutex);
        sock_session_close_fd(s);
        sock_deliver(s, SOCK_EVENT_DISCONNECT, NULL, 0, 0);
        pthread_mutex_lock(&sock_mutex);

        s = sock_list;
        continue;
      }
    }

    s = next;
  }

  pthread_mutex_unlock(&sock_mutex);
}

// Epoll worker thread (persistent task)

static void
sock_epoll_task(task_t *t)
{
  sock_worker_t *w = t->data;

  w->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

  if(w->epoll_fd < 0)
  {
    clam(CLAM_FATAL, "sock", "epoll_create1 failed: %s", strerror(errno));
    t->state = TASK_FATAL;
    return;
  }

  w->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

  if(w->wake_fd < 0)
  {
    clam(CLAM_FATAL, "sock", "eventfd failed: %s", strerror(errno));
    close(w->epoll_fd);
    w->epoll_fd = -1;
    t->state = TASK_FATAL;
    return;
  }

  // Register wake_fd with epoll.
  {
    struct epoll_event wake_ev;

    wake_ev.events  = EPOLLIN;
    wake_ev.data.ptr = &w->wake_fd;
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, w->wake_fd, &wake_ev);
  }

  clam(CLAM_INFO, "sock", "epoll worker %u started", w->id);

  {
    struct epoll_event *events;

    events = mem_alloc("sock", "epoll_events",
        sizeof(struct epoll_event) * sock_cfg.epoll_max_events);

  while(!pool_shutting_down())
  {
    int n = epoll_wait(w->epoll_fd, events,
        (int)sock_cfg.epoll_max_events, (int)sock_cfg.epoll_timeout);

    if(n < 0)
    {
      if(errno == EINTR)
        continue;

      clam(CLAM_WARN, "sock", "epoll_wait error: %s", strerror(errno));
      break;
    }

    for(int i = 0; i < n; i++)
    {
      struct epoll_event *ev = &events[i];

      // Wake event — drain eventfd, then scan for pending sends.
      if(ev->data.ptr == &w->wake_fd)
      {
        uint64_t val;
        sock_session_t *s;

        (void)read(w->wake_fd, &val, sizeof(val));

        // Arm EPOLLOUT for any session with pending sends.
        pthread_mutex_lock(&sock_mutex);
        s = sock_list;

        while(s != NULL)
        {
          if(s->worker_id == w->id &&
              s->state == SOCK_STATE_CONNECTED &&
              s->fd >= 0 && !s->epollout_armed)
          {
            bool has_data;

            pthread_mutex_lock(&s->send_lock);
            has_data = (s->send_head != NULL);
            pthread_mutex_unlock(&s->send_lock);

            if(has_data)
              sock_epoll_rearm(w, s, true);
          }

          s = s->next;
        }

        pthread_mutex_unlock(&sock_mutex);
        continue;
      }

      // Session event.
      {
      sock_session_t *s = ev->data.ptr;

      if(s->state == SOCK_STATE_CLOSED || s->fd < 0)
        continue;

      // Error or hangup.
      if(ev->events & (EPOLLERR | EPOLLHUP))
      {
        if(s->state == SOCK_STATE_CONNECTING)
        {
          int err = 0;
          socklen_t errlen = sizeof(err);

          getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);

          clam(CLAM_WARN, "sock", "[%s] connect failed: %s",
              s->name, strerror(err ? err : ECONNREFUSED));
          sock_session_close_fd(s);
          sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0,
              err ? err : ECONNREFUSED);
        }

        else
        {
          clam(CLAM_DEBUG, "sock", "[%s] connection lost", s->name);
          sock_session_close_fd(s);
          sock_deliver(s, SOCK_EVENT_DISCONNECT, NULL, 0, 0);
        }

        continue;
      }

      // Writable.
      if(ev->events & EPOLLOUT)
      {
        if(s->state == SOCK_STATE_CONNECTING)
        {
          // Connect completed — check for error.
          int err = 0;
          socklen_t errlen = sizeof(err);
          getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);

          if(err != 0)
          {
            clam(CLAM_WARN, "sock", "[%s] connect failed: %s",
                s->name, strerror(err));
            sock_session_close_fd(s);
            sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, err);
          }

          else if(s->tls_enabled)
          {
            clam(CLAM_INFO, "sock", "[%s] TCP connected to %s:%u, starting TLS",
                s->name, s->host, s->port);

            SSL_set_fd(s->tls, s->fd);
            SSL_set_tlsext_host_name(s->tls, s->host);
            s->state = SOCK_STATE_TLS_HANDSHAKE;
            sock_epoll_rearm(w, s, false);
            sock_tls_handshake(s);
          }

          else
          {
            s->state = SOCK_STATE_CONNECTED;
            s->last_activity = time(NULL);
            s->connected_at  = s->last_activity;
            sock_epoll_rearm(w, s, false);

            clam(CLAM_INFO, "sock", "[%s] connected to %s:%u",
                s->name, s->host, s->port);

            sock_deliver(s, SOCK_EVENT_CONNECTED, NULL, 0, 0);
          }
        }

        else if(s->state == SOCK_STATE_TLS_HANDSHAKE)
          sock_tls_handshake(s);

        else if(s->state == SOCK_STATE_CONNECTED)
        {
          // Drain send queue.
          bool drained = sock_drain_sendq(s);

          if(drained)
            sock_epoll_rearm(w, s, false);

          s->last_activity = time(NULL);
        }
      }

      // Readable.
      if(ev->events & EPOLLIN)
      {
        if(s->state == SOCK_STATE_TLS_HANDSHAKE)
        {
          sock_tls_handshake(s);
          continue;
        }

        if(s->state != SOCK_STATE_CONNECTED || s->fd < 0)
          continue;

        // Edge-triggered: read until EAGAIN.
        for(;;)
        {
          ssize_t nr;

          if(s->tls != NULL)
          {
            nr = SSL_read(s->tls, s->read_buf, (int)s->read_buf_sz);

            if(nr <= 0)
            {
              int ssl_err = SSL_get_error(s->tls, (int)nr);

              if(ssl_err == SSL_ERROR_WANT_READ ||
                  ssl_err == SSL_ERROR_WANT_WRITE)
                break;

              if(ssl_err == SSL_ERROR_ZERO_RETURN || nr == 0)
              {
                clam(CLAM_DEBUG, "sock", "[%s] TLS peer closed",
                    s->name);
                sock_tls_cleanup(s);
                sock_session_close_fd(s);
                sock_deliver(s, SOCK_EVENT_DISCONNECT, NULL, 0, 0);
                break;
              }

              clam(CLAM_WARN, "sock", "[%s] SSL_read error (%d)",
                  s->name, ssl_err);
              sock_tls_cleanup(s);
              sock_session_close_fd(s);
              sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, EIO);
              break;
            }
          }

          else
          {
            nr = recv(s->fd, s->read_buf, s->read_buf_sz, 0);

            if(nr == 0)
            {
              clam(CLAM_DEBUG, "sock", "[%s] peer closed connection",
                  s->name);
              sock_session_close_fd(s);
              sock_deliver(s, SOCK_EVENT_DISCONNECT, NULL, 0, 0);
              break;
            }

            if(nr < 0)
            {
              if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;

              if(errno == EINTR)
                continue;

              clam(CLAM_WARN, "sock", "[%s] recv error: %s",
                  s->name, strerror(errno));
              sock_session_close_fd(s);
              sock_deliver(s, SOCK_EVENT_ERROR, NULL, 0, errno);
              break;
            }
          }

          s->bytes_in += (uint64_t)nr;
          s->last_activity = time(NULL);

          __atomic_fetch_add(&sock_total_in, (uint64_t)nr,
              __ATOMIC_RELAXED);

          sock_deliver(s, SOCK_EVENT_DATA, s->read_buf, (size_t)nr, 0);

          // Session may have been closed in callback.
          if(s->state != SOCK_STATE_CONNECTED || s->fd < 0)
            break;
        }
      }
      }
    }

    // Check timeouts each iteration.
    sock_check_timeouts();
  }

  mem_free(events);
  }

  close(w->wake_fd);
  w->wake_fd = -1;
  close(w->epoll_fd);
  w->epoll_fd = -1;

  clam(CLAM_INFO, "sock", "epoll worker %u stopped", w->id);
  t->state = TASK_ENDED;
}

// KV configuration

// Read all core.sock.* KV settings into the cached sock_cfg struct
// and apply sanity clamps to keep values within safe bounds.
static void
sock_load_config(void)
{
  sock_cfg.connect_timeout  = (uint32_t)kv_get_uint("core.sock.connect_timeout");
  sock_cfg.read_buf_sz      = (uint32_t)kv_get_uint("core.sock.read_buf_sz");
  sock_cfg.send_queue_max   = (uint32_t)kv_get_uint("core.sock.send_queue_max");
  sock_cfg.idle_timeout     = (uint32_t)kv_get_uint("core.sock.idle_timeout");
  sock_cfg.keepalive        = (uint32_t)kv_get_uint("core.sock.keepalive");
  sock_cfg.max_sessions     = (uint32_t)kv_get_uint("core.sock.max_sessions");
  sock_cfg.epoll_max_events = (uint32_t)kv_get_uint("core.sock.epoll_max_events");
  sock_cfg.epoll_timeout    = (uint32_t)kv_get_uint("core.sock.epoll_timeout");
  sock_cfg.epoll_workers    = (uint32_t)kv_get_uint("core.sock.epoll_workers");

  // Sanity clamps.
  if(sock_cfg.read_buf_sz < 512)
    sock_cfg.read_buf_sz = 512;

  if(sock_cfg.read_buf_sz > 65536)
    sock_cfg.read_buf_sz = 65536;

  if(sock_cfg.epoll_max_events < 1)
    sock_cfg.epoll_max_events = 1;

  if(sock_cfg.epoll_max_events > 1024)
    sock_cfg.epoll_max_events = 1024;

  if(sock_cfg.epoll_timeout < 100)
    sock_cfg.epoll_timeout = 100;

  if(sock_cfg.max_sessions < 1)
    sock_cfg.max_sessions = 1;

  if(sock_cfg.epoll_workers < 1)
    sock_cfg.epoll_workers = 1;

  if(sock_cfg.epoll_workers > SOCK_MAX_WORKERS)
    sock_cfg.epoll_workers = SOCK_MAX_WORKERS;
}

static void
sock_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  sock_load_config();
}

// Register all core.sock.* KV keys with their types, defaults,
// and change callbacks.
static void
sock_register_kv(void)
{
  kv_register("core.sock.connect_timeout",  KV_UINT32, "10",
      sock_kv_changed, NULL, "TCP connect timeout in seconds");
  kv_register("core.sock.read_buf_sz",      KV_UINT32, "4096",
      sock_kv_changed, NULL, "Per-socket read buffer size in bytes");
  kv_register("core.sock.send_queue_max",   KV_UINT32, "65536",
      sock_kv_changed, NULL, "Maximum send queue size in bytes per socket");
  kv_register("core.sock.idle_timeout",     KV_UINT32, "0",
      sock_kv_changed, NULL, "Idle socket timeout in seconds (0=disabled)");
  kv_register("core.sock.keepalive",        KV_UINT32, "0",
      sock_kv_changed, NULL, "TCP keepalive interval in seconds (0=disabled)");
  kv_register("core.sock.max_sessions",     KV_UINT32, "256",
      sock_kv_changed, NULL, "Maximum concurrent socket sessions");
  kv_register("core.sock.epoll_max_events", KV_UINT32, "64",
      sock_kv_changed, NULL, "Maximum events returned per epoll_wait call");
  kv_register("core.sock.epoll_timeout",    KV_UINT32, "500",
      sock_kv_changed, NULL, "Epoll wait timeout in milliseconds");
  kv_register("core.sock.epoll_workers",    KV_UINT32, "1",
      NULL, NULL, "Number of epoll worker threads (read-only at startup)");
}

// Public API

// Create a new socket session. Allocates the session struct, read buffer,
// and assigns a worker via round-robin. Does not connect yet.
// cb: event callback invoked on the epoll worker thread
sock_session_t *
sock_create(const char *name, sock_type_t type,
    sock_cb_t cb, void *user_data)
{
  sock_session_t *s;

  if(cb == NULL || !sock_ready)
    return(NULL);

  if(sock_count >= sock_cfg.max_sessions)
  {
    clam(CLAM_WARN, "sock", "max sessions reached (%u)",
        sock_cfg.max_sessions);
    return(NULL);
  }

  s = mem_alloc("sock", "session", sizeof(*s));
  memset(s, 0, sizeof(*s));

  strncpy(s->name, name ? name : "unnamed", SOCK_NAME_SZ - 1);
  s->type    = type;
  s->state   = SOCK_STATE_CREATED;
  s->fd      = -1;
  s->cb      = cb;
  s->cb_data = user_data;

  // Allocate read buffer using current config.
  s->read_buf_sz = sock_cfg.read_buf_sz;
  s->read_buf    = mem_alloc("sock", "read_buf", s->read_buf_sz);

  pthread_mutex_init(&s->send_lock, NULL);

  // Round-robin assignment across active workers.
  pthread_mutex_lock(&sock_mutex);

  s->worker_id = sock_next_worker;
  sock_next_worker = (sock_next_worker + 1) % sock_worker_count;

  // Add to session list.
  s->next = sock_list;
  sock_list = s;
  sock_count++;
  pthread_mutex_unlock(&sock_mutex);

  clam(CLAM_DEBUG, "sock", "[%s] session created (%s)",
      s->name, sock_type_name(type));

  return(s);
}

// Enable TLS on a session. Must be called after sock_create() and before
// sock_connect(). Creates SSL_CTX and SSL objects; the actual handshake
// occurs after TCP connect completes.
bool
sock_set_tls(sock_session_t *session, bool verify)
{
  SSL_CTX *ctx;
  SSL *ssl;

  if(session == NULL)
    return(FAIL);

  if(session->state != SOCK_STATE_CREATED)
  {
    clam(CLAM_WARN, "sock", "[%s] sock_set_tls called in wrong state",
        session->name);
    return(FAIL);
  }

  if(session->type != SOCK_TCP)
  {
    clam(CLAM_WARN, "sock", "[%s] TLS only supported on TCP sockets",
        session->name);
    return(FAIL);
  }

  // Create an SSL_CTX with a modern TLS method.
  ctx = SSL_CTX_new(TLS_client_method());
  if(ctx == NULL)
  {
    clam(CLAM_WARN, "sock", "[%s] SSL_CTX_new failed", session->name);
    return(FAIL);
  }

  // Set minimum protocol to TLS 1.2.
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  if(verify)
  {
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  }

  else
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

  // Create the SSL object.
  ssl = SSL_new(ctx);
  if(ssl == NULL)
  {
    clam(CLAM_WARN, "sock", "[%s] SSL_new failed", session->name);
    SSL_CTX_free(ctx);
    return(FAIL);
  }

  session->tls_ctx    = ctx;
  session->tls        = ssl;
  session->tls_enabled = true;
  session->tls_verify = verify;

  clam(CLAM_DEBUG, "sock", "[%s] TLS enabled (verify=%s)",
      session->name, verify ? "yes" : "no");

  return(SUCCESS);
}

// Initiate an asynchronous connection. For TCP/UDP, submits a DNS resolve
// request; for UNIX, spawns a direct connect task. The consumer is notified
// via SOCK_EVENT_CONNECTED or SOCK_EVENT_ERROR.
bool
sock_connect(sock_session_t *session, const char *host, uint16_t port,
    const char *path)
{
  if(session == NULL)
    return(FAIL);

  if(session->state != SOCK_STATE_CREATED &&
      session->state != SOCK_STATE_CLOSED)
  {
    clam(CLAM_WARN, "sock", "[%s] connect called in wrong state",
        session->name);
    return(FAIL);
  }

  // Validate parameters.
  if(session->type == SOCK_UNIX)
  {
    if(path == NULL || path[0] == '\0')
    {
      clam(CLAM_WARN, "sock", "[%s] unix socket requires a path",
          session->name);
      return(FAIL);
    }

    strncpy(session->path, path, SOCK_PATH_SZ - 1);
  }

  else
  {
    if(host == NULL || host[0] == '\0')
    {
      clam(CLAM_WARN, "sock", "[%s] host is required", session->name);
      return(FAIL);
    }

    strncpy(session->host, host, SOCK_HOST_SZ - 1);
    session->port = port;
  }

  session->state = SOCK_STATE_RESOLVING;
  session->connect_started = time(NULL);

  if(session->type == SOCK_UNIX)
  {
    // Unix sockets don't need DNS — submit direct connect task.
    task_t *rt = task_add("sock_unix", TASK_THREAD, 100,
        sock_unix_connect_task, session);

    if(rt == NULL)
    {
      clam(CLAM_WARN, "sock", "[%s] failed to submit unix connect task",
          session->name);
      session->state = SOCK_STATE_CLOSED;
      return(FAIL);
    }
  }

  else
  {
    // TCP/UDP — resolve hostname via the generic resolver.
    if(resolve_lookup(session->host, RESOLVE_A,
        sock_resolve_done, session) != SUCCESS)
    {
      clam(CLAM_WARN, "sock", "[%s] failed to submit resolve for '%s'",
          session->name, session->host);
      session->state = SOCK_STATE_CLOSED;
      return(FAIL);
    }
  }

  return(SUCCESS);
}

// Queue data for asynchronous sending. Thread-safe — may be called from
// any thread. Data is copied into a send buffer and drained by the epoll
// worker. Wakes the worker if EPOLLOUT is not already armed.
// returns: SUCCESS or FAIL (not connected, or send queue limit exceeded)
bool
sock_send(sock_session_t *session, const void *buf, size_t len)
{
  sock_sendbuf_t *sb;
  bool need_wake;

  if(session == NULL || buf == NULL || len == 0)
    return(FAIL);

  if(session->state != SOCK_STATE_CONNECTED)
    return(FAIL);

  // Check send queue limit.
  if(sock_cfg.send_queue_max > 0 &&
      session->send_queued + (uint32_t)len > sock_cfg.send_queue_max)
  {
    clam(CLAM_WARN, "sock", "[%s] send queue full (%u bytes)",
        session->name, session->send_queued);
    return(FAIL);
  }

  sb = sock_sbuf_alloc(buf, len);

  pthread_mutex_lock(&session->send_lock);

  if(session->send_tail != NULL)
    session->send_tail->next = sb;

  else
    session->send_head = sb;

  session->send_tail = sb;
  session->send_queued += (uint32_t)len;

  need_wake = !session->epollout_armed;

  pthread_mutex_unlock(&session->send_lock);

  if(need_wake)
    sock_wake_worker(session);

  return(SUCCESS);
}

// Initiate graceful close. Flushes any remaining send queue, closes the
// fd, and delivers SOCK_EVENT_DISCONNECT. Do not call sock_send() after.
// session: session handle (NULL is a safe no-op)
void
sock_close(sock_session_t *session)
{
  if(session == NULL)
    return;

  if(session->state == SOCK_STATE_CLOSED ||
      session->state == SOCK_STATE_CLOSING)
    return;

  session->state = SOCK_STATE_CLOSING;

  // Best-effort: flush remaining send queue.
  if(session->fd >= 0 && session->send_head != NULL)
    sock_drain_sendq(session);

  sock_session_close_fd(session);
  sock_deliver(session, SOCK_EVENT_DISCONNECT, NULL, 0, 0);
}

// Destroy a session and free all resources. Removes from the session
// list, drains the send queue, frees the read buffer, and releases the
// session struct. Must not be called from within a callback for the
// same session. Force-closes the fd if still open.
// session: session handle (NULL is a safe no-op)
void
sock_destroy(sock_session_t *session)
{
  sock_session_t **pp;
  sock_sendbuf_t *sb;

  if(session == NULL)
    return;

  // Force close if still open.
  if(session->state != SOCK_STATE_CLOSED)
    sock_session_close_fd(session);

  // Remove from session list.
  pthread_mutex_lock(&sock_mutex);

  pp = &sock_list;

  while(*pp != NULL)
  {
    if(*pp == session)
    {
      *pp = session->next;
      sock_count--;
      break;
    }

    pp = &(*pp)->next;
  }

  pthread_mutex_unlock(&sock_mutex);

  // Free send queue.
  pthread_mutex_lock(&session->send_lock);
  sb = session->send_head;

  while(sb != NULL)
  {
    sock_sendbuf_t *next = sb->next;

    sock_sbuf_release(sb);
    sb = next;
  }

  session->send_head = NULL;
  session->send_tail = NULL;
  session->send_queued = 0;
  pthread_mutex_unlock(&session->send_lock);

  pthread_mutex_destroy(&session->send_lock);

  // Free read buffer.
  mem_free(session->read_buf);
  session->read_buf = NULL;

  clam(CLAM_DEBUG, "sock", "[%s] session destroyed", session->name);

  mem_free(session);
}

int
sock_get_fd(const sock_session_t *session)
{
  if(session == NULL)
    return(-1);

  return(session->fd);
}

// Collect a thread-safe snapshot of socket subsystem statistics.
void
sock_get_stats(sock_stats_t *out)
{
  sock_session_t *s;

  memset(out, 0, sizeof(*out));

  pthread_mutex_lock(&sock_mutex);

  s = sock_list;

  while(s != NULL)
  {
    out->sessions++;

    if(s->state == SOCK_STATE_CONNECTED)
      out->connected++;

    if(s->tls_enabled)
      out->tls_sessions++;

    s = s->next;
  }

  pthread_mutex_unlock(&sock_mutex);

  out->bytes_in      = __atomic_load_n(&sock_total_in, __ATOMIC_RELAXED);
  out->bytes_out     = __atomic_load_n(&sock_total_out, __ATOMIC_RELAXED);
  out->connections   = __atomic_load_n(&sock_total_conn, __ATOMIC_RELAXED);
  out->disconnects   = __atomic_load_n(&sock_total_disc, __ATOMIC_RELAXED);
  out->workers       = sock_worker_count;
}

const char *
sock_type_name(sock_type_t type)
{
  switch(type)
  {
    case SOCK_TCP:  return("tcp");
    case SOCK_UDP:  return("udp");
    case SOCK_ICMP: return("icmp");
    case SOCK_UNIX: return("unix");
    default:        return("unknown");
  }
}

const char *
sock_event_name(sock_event_type_t type)
{
  switch(type)
  {
    case SOCK_EVENT_CONNECTED:  return("connected");
    case SOCK_EVENT_DATA:       return("data");
    case SOCK_EVENT_DISCONNECT: return("disconnect");
    case SOCK_EVENT_ERROR:      return("error");
    default:                    return("unknown");
  }
}

const char *
sock_state_name(int state)
{
  switch((sock_state_t)state)
  {
    case SOCK_STATE_CREATED:       return("created");
    case SOCK_STATE_RESOLVING:     return("resolving");
    case SOCK_STATE_CONNECTING:    return("connecting");
    case SOCK_STATE_CONNECTED:     return("connected");
    case SOCK_STATE_TLS_HANDSHAKE: return("tls_handshake");
    case SOCK_STATE_CLOSING:       return("closing");
    case SOCK_STATE_CLOSED:        return("closed");
    default:                       return("unknown");
  }
}

// Iterate all active socket sessions under the session list lock.
// Each session's id, type, state, remote address, byte counters,
// TLS flag, and connection timestamp are passed to the callback.
// cb: iteration callback (must be fast — lock is held)
void
sock_iterate(sock_iter_cb_t cb, void *data)
{
  uint32_t id = 0;

  if(cb == NULL)
    return;

  pthread_mutex_lock(&sock_mutex);

  for(sock_session_t *s = sock_list; s != NULL; s = s->next)
  {
    char remote[SOCK_HOST_SZ + 16];

    if(s->type == SOCK_UNIX)
      snprintf(remote, sizeof(remote), "%s", s->path);
    else if(s->host[0] != '\0')
      snprintf(remote, sizeof(remote), "%s:%u", s->host, s->port);
    else
      remote[0] = '\0';

    cb(id++, s->type, (int)s->state, remote,
        s->bytes_in, s->bytes_out, s->tls_enabled,
        s->connected_at, data);
  }

  pthread_mutex_unlock(&sock_mutex);
}

// Subsystem lifecycle

// Initialize the socket subsystem. Sets up mutexes, applies default
// configuration, initializes worker slots, and spawns the initial set
// of epoll worker threads. Must be called after pool_init().
void
sock_init(void)
{
  if(sock_ready)
    return;

  pthread_mutex_init(&sock_mutex, NULL);
  pthread_mutex_init(&sock_sbuf_mutex, NULL);

  // Set defaults before KV may be available.
  sock_cfg.connect_timeout  = SOCK_DEF_CONNECT_TIMEOUT;
  sock_cfg.read_buf_sz      = SOCK_DEF_READ_BUF_SZ;
  sock_cfg.send_queue_max   = SOCK_DEF_SEND_QUEUE_MAX;
  sock_cfg.idle_timeout     = SOCK_DEF_IDLE_TIMEOUT;
  sock_cfg.keepalive        = SOCK_DEF_KEEPALIVE;
  sock_cfg.max_sessions     = SOCK_DEF_MAX_SESSIONS;
  sock_cfg.epoll_max_events = SOCK_DEF_EPOLL_MAX_EVENTS;
  sock_cfg.epoll_timeout    = SOCK_DEF_EPOLL_TIMEOUT;
  sock_cfg.epoll_workers    = SOCK_DEF_EPOLL_WORKERS;

  // Initialize all worker slots.
  memset(sock_workers, 0, sizeof(sock_workers));

  for(uint8_t i = 0; i < SOCK_MAX_WORKERS; i++)
  {
    sock_workers[i].id       = i;
    sock_workers[i].epoll_fd = -1;
    sock_workers[i].wake_fd  = -1;
  }

  // Spawn the configured number of epoll worker threads.
  {
    uint8_t want = (uint8_t)sock_cfg.epoll_workers;

  for(uint8_t i = 0; i < want; i++)
  {
    char name[TASK_NAME_SZ];

    snprintf(name, sizeof(name), "sock_epoll_%u", i);

    sock_workers[i].task = task_add_persist(name, 50,
        sock_epoll_task, &sock_workers[i]);

    if(sock_workers[i].task == NULL)
    {
      clam(CLAM_FATAL, "sock", "failed to spawn epoll worker %u", i);
      return;
    }

    sock_worker_count++;
  }
  }

  sock_ready = true;

  clam(CLAM_INFO, "sock", "socket service initialized (%u epoll worker%s)",
      sock_worker_count, sock_worker_count > 1 ? "s" : "");
}

// Shut down the socket subsystem. Force-closes and destroys all remaining
// sessions, frees the send buffer freelist, and destroys mutexes. The
// epoll worker threads must already be joined (via pool_exit) before this.
void
sock_exit(void)
{
  if(!sock_ready)
    return;

  sock_ready = false;

  // Force close and destroy all remaining sessions.
  while(sock_list != NULL)
  {
    sock_session_t *s = sock_list;
    sock_session_close_fd(s);
    sock_destroy(s);
  }

  // Free send buffer freelist.
  pthread_mutex_lock(&sock_sbuf_mutex);

  while(sock_sbuf_free != NULL)
  {
    sock_sendbuf_t *sb = sock_sbuf_free;
    sock_sbuf_free = sb->next;
    mem_free(sb);
  }

  pthread_mutex_unlock(&sock_sbuf_mutex);

  pthread_mutex_destroy(&sock_mutex);
  pthread_mutex_destroy(&sock_sbuf_mutex);

  sock_worker_count = 0;

  clam(CLAM_INFO, "sock", "socket service shut down");
}

// Called after KV is available to register configuration keys and
// load values from the database. Spawns additional epoll workers
// if the configured count exceeds what sock_init() started.
void
sock_register_config(void)
{
  uint8_t want;

  sock_register_kv();
  sock_load_config();

  // Scale up workers if config asks for more than we started.
  want = (uint8_t)sock_cfg.epoll_workers;

  while(sock_worker_count < want)
  {
    uint8_t i = sock_worker_count;
    char name[TASK_NAME_SZ];

    snprintf(name, sizeof(name), "sock_epoll_%u", i);

    sock_workers[i].task = task_add_persist(name, 50,
        sock_epoll_task, &sock_workers[i]);

    if(sock_workers[i].task == NULL)
    {
      clam(CLAM_WARN, "sock", "failed to spawn epoll worker %u", i);
      break;
    }

    sock_worker_count++;

    clam(CLAM_INFO, "sock", "spawned epoll worker %u (total: %u)",
        i, sock_worker_count);
  }
}
