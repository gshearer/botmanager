// botmanager — MIT
// In-daemon control-socket command server for the botmanctl CLI.
#define BOTMANCTL_INTERNAL
#include "botmanctl.h"

#include <fcntl.h>
#include <sys/stat.h>

// Severity labels for subscribe output (mirrors clam.h CLAM_INTERNAL).
static const char *bctl_sev_label[] = {
  [CLAM_FATAL]  = "FATAL",
  [CLAM_WARN]   = " WARN",
  [CLAM_INFO]   = " INFO",
  [CLAM_DEBUG]  = "  DBG",
  [CLAM_DEBUG2] = " DBG2",
  [CLAM_DEBUG3] = " DBG3",
  [CLAM_DEBUG4] = " DBG4",
  [CLAM_DEBUG5] = " DBG5",
};

// Color per severity level for subscribe output.
static const char *bctl_sev_color[] = {
  [CLAM_FATAL]  = CON_RED,
  [CLAM_WARN]   = CON_YELLOW,
  [CLAM_INFO]   = CON_GREEN,
  [CLAM_DEBUG]  = CON_CYAN,
  [CLAM_DEBUG2] = CON_PURPLE,
  [CLAM_DEBUG3] = CON_BLUE,
  [CLAM_DEBUG4] = CON_WHITE,
  [CLAM_DEBUG5] = CON_WHITE,
};

// CLAM subscribe callback

// Single shared CLAM subscriber callback for all subscribe-mode clients.
// Runs on the publisher's thread (under clam_mutex). Must be fast.
// Per-client severity and regex filtering is done here.
static void
bctl_clam_cb(const clam_msg_t *m)
{
  bctl_server_t *srv;
  uint8_t sev;
  struct tm tm;
  time_t now;
  char buf[16 + 8 + CLAM_CTX_SZ + 16 + CLAM_MSG_SZ + 2];
  char haystack[CLAM_CTX_SZ + CLAM_MSG_SZ + 2];
  int len;

  srv = bctl_state;
  if(srv == NULL)
    return;

  sev = m->sev;
  if(sev > CLAM_DEBUG5)
    sev = CLAM_DEBUG5;

  now = time(NULL);
  localtime_r(&now, &tm);

  // Format with ANSI colors for terminal display.
  len = snprintf(buf, sizeof(buf) - 1,
      "%02d:%02d:%02d %s%s %-*s%s %s\n",
      tm.tm_hour, tm.tm_min, tm.tm_sec,
      bctl_sev_color[sev], bctl_sev_label[sev],
      CLAM_CTX_SZ, m->context,
      CON_RESET, m->msg);

  if(len <= 0)
    return;

  // Per-client regex runs against "<context> <msg>" so subscribers can
  // filter by subsystem (e.g. "^curl " or "curl|acquire") as well as
  // message body. Built once here rather than per-client since this
  // callback fans out to every subscriber.
  snprintf(haystack, sizeof(haystack), "%s %s", m->context, m->msg);

  // Write to each subscribe client whose severity allows this message.
  pthread_mutex_lock(&srv->client_mutex);

  for(bctl_client_t *c = srv->clients; c != NULL; c = c->next)
  {
    ssize_t wrote;

    if(c->mode != BCTL_MODE_SUBSCRIBE || c->closing)
      continue;

    if(sev > c->clam_sev)
      continue;

    // Per-client regex filtering.
    if(c->has_clam_regex
        && regexec(&c->clam_regex, haystack, 0, NULL, 0) != 0)
      continue;

    // Non-blocking send. A slow/stalled subscriber must never wedge
    // this callback, which runs under clam_mutex → every other thread
    // calling clam() is waiting behind us. MSG_NOSIGNAL avoids SIGPIPE
    // if the peer already closed. EAGAIN drops the line (subscription
    // preserved — the subscriber just missed a message); any other
    // error marks the client for removal.
    wrote = send(c->fd, buf, (size_t)len,
        MSG_DONTWAIT | MSG_NOSIGNAL);

    if(wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK
        && errno != EINTR)
      c->closing = true;
  }

  pthread_mutex_unlock(&srv->client_mutex);
}

// Subscribe handling

static void
bctl_handle_subscribe(bctl_server_t *srv, bctl_client_t *c, char *args)
{
  uint8_t sev = CLAM_DEBUG5;
  char *regex_str = NULL;
  bool need_subscribe;

  if(args != NULL && *args != '\0')
  {
    char *endp;
    long val;

    val = strtol(args, &endp, 10);
    if(endp != args && val >= 0 && val <= 7)
    {
      sev = (uint8_t)val;

      // Skip whitespace to find optional regex.
      while(*endp == ' ' || *endp == '\t')
        endp++;

      if(*endp != '\0')
        regex_str = endp;
    }
  }

  pthread_mutex_lock(&srv->client_mutex);

  c->mode = BCTL_MODE_SUBSCRIBE;
  c->clam_sev = sev;

  if(regex_str != NULL
      && regcomp(&c->clam_regex, regex_str,
          REG_EXTENDED | REG_NOSUB) == 0)
    c->has_clam_regex = true;

  // Check if we need to register the shared CLAM subscriber.
  need_subscribe = !bctl_clam_subscribed;

  if(need_subscribe)
    bctl_clam_subscribed = true;

  pthread_mutex_unlock(&srv->client_mutex);

  // Register outside client_mutex to avoid ABBA deadlock with clam_mutex.
  // (bctl_clam_cb holds clam_mutex → client_mutex; calling clam_subscribe
  // here with client_mutex held would invert to client_mutex → clam_mutex.)
  if(need_subscribe)
    clam_subscribe("botmanctl", CLAM_DEBUG5, NULL, bctl_clam_cb);

  clam(CLAM_DEBUG, "botmanctl",
      "client fd %d subscribed (sev: %u, regex: %s)",
      c->fd, sev, regex_str ? regex_str : "*");
}

// Client management

static bctl_client_t *
bctl_client_add(bctl_server_t *srv, int fd)
{
  bctl_client_t *c;

  if(srv->client_count >= BCTL_MAX_CLIENTS)
  {
    close(fd);
    clam(CLAM_WARN, "botmanctl", "max clients reached, rejecting");
    return(NULL);
  }

  c = mem_alloc("botmanctl", "client", sizeof(bctl_client_t));

  memset(c, 0, sizeof(*c));
  c->fd = fd;
  c->mode = BCTL_MODE_COMMAND;

  // Default asserted identity for unix-socket clients: @owner.
  // Overridable per-connection via the AS session command (wired from
  // `botmanctl --as-user <name>`).
  snprintf(c->as_user, sizeof(c->as_user), "%s", USERNS_OWNER_USER);

  pthread_mutex_lock(&srv->client_mutex);
  c->id = bctl_next_client_id++;
  c->next = srv->clients;
  srv->clients = c;
  srv->client_count++;
  pthread_mutex_unlock(&srv->client_mutex);

  clam(CLAM_DEBUG, "botmanctl", "client connected (fd: %d, total: %u)",
      fd, srv->client_count);
  return(c);
}

static void
bctl_client_remove(bctl_server_t *srv, bctl_client_t *c)
{
  bctl_client_t **pp;
  bool any_subs = false;

  pthread_mutex_lock(&srv->client_mutex);

  // Unlink from list.
  pp = &srv->clients;

  while(*pp != NULL)
  {
    if(*pp == c)
    {
      *pp = c->next;
      break;
    }

    pp = &(*pp)->next;
  }

  srv->client_count--;

  // Free subscribe regex if compiled.
  if(c->mode == BCTL_MODE_SUBSCRIBE && c->has_clam_regex)
    regfree(&c->clam_regex);

  // If no subscribe clients remain, unsubscribe the shared CLAM subscriber.
  for(bctl_client_t *p = srv->clients; p != NULL; p = p->next)
  {
    if(p->mode == BCTL_MODE_SUBSCRIBE)
    {
      any_subs = true;
      break;
    }
  }

  if(!any_subs && bctl_clam_subscribed)
  {
    bctl_clam_subscribed = false;
    pthread_mutex_unlock(&srv->client_mutex);
    clam_unsubscribe("botmanctl");
  }

  else
    pthread_mutex_unlock(&srv->client_mutex);

  close(c->fd);

  clam(CLAM_DEBUG, "botmanctl",
      "client disconnected (fd: %d, total: %u)",
      c->fd, srv->client_count);

  mem_free(c);
}

// Method driver callbacks

// Allocate and initialize botmanctl server state.
static void *
bctl_drv_create(const char *inst_name)
{
  bctl_server_t *srv;

  (void)inst_name;

  srv = mem_alloc("botmanctl", "server", sizeof(bctl_server_t));

  memset(srv, 0, sizeof(*srv));
  srv->listen_fd = -1;
  srv->clients = NULL;
  srv->client_count = 0;
  pthread_mutex_init(&srv->client_mutex, NULL);

  // Stash module-level pointer for the persist task.
  bctl_state = srv;
  return(srv);
}

static void
bctl_drv_destroy(void *handle)
{
  bctl_server_t *srv;

  if(handle == NULL)
    return;

  srv = handle;

  // The reference botmanctl_register_method() stored in srv->inst,
  // given back where the thing that stored it dies.
  method_release(srv->inst);
  pthread_mutex_destroy(&srv->client_mutex);
  mem_free(srv);
}

static bool
bctl_drv_connect(void *handle)
{
  bctl_server_t *srv = handle;
  const char *path;
  char dir[BCTL_SOCK_PATH_SZ];
  char *slash;
  int fd;
  int flags;
  struct sockaddr_un addr;

  if(srv == NULL)
    return(FAIL);

  if(srv->listen_fd >= 0)
    return(SUCCESS);

  // Resolve socket path from KV.
  path = kv_get_str("core.botmanctl.sockpath");
  if(path == NULL || path[0] == '\0')
  {
    clam(CLAM_WARN, "botmanctl", "sockpath not configured");
    return(FAIL);
  }

  snprintf(srv->sock_path, sizeof(srv->sock_path), "%s", path);

  // Ensure parent directory tree exists.
  snprintf(dir, sizeof(dir), "%s", srv->sock_path);

  slash = strrchr(dir, '/');
  if(slash != NULL && slash != dir)
  {
    *slash = '\0';

    // Walk forward creating each component.
    for(char *p = dir + 1; ; p++)
    {
      if(*p == '/' || *p == '\0')
      {
        char saved = *p;

        *p = '\0';

        if(mkdir(dir, 0700) < 0 && errno != EEXIST)
        {
          clam(CLAM_WARN, "botmanctl", "mkdir(%s): %s",
              dir, strerror(errno));
          return(FAIL);
        }

        *p = saved;

        if(saved == '\0')
          break;
      }
    }
  }

  // Remove stale socket file if it exists.
  unlink(srv->sock_path);

  // CLOEXEC: core/proc.c fork+execvp()s helpers (the claude bridge, ninja,
  // git, acquire shells) from other threads, and none of them has any use
  // for the operator's control socket.
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0)
  {
    clam(CLAM_WARN, "botmanctl", "socket(): %s", strerror(errno));
    return(FAIL);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  // sun_path is 108 bytes and sock_path is longer, so a silent truncation
  // would bind a socket nobody is looking for. Refuse instead.
  if(snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", srv->sock_path)
      >= (int)sizeof(addr.sun_path))
  {
    clam(CLAM_WARN, "botmanctl", "socket path too long (max %zu): %s",
        sizeof(addr.sun_path) - 1, srv->sock_path);
    close(fd);
    return(FAIL);
  }

  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "bind(%s): %s",
        srv->sock_path, strerror(errno));
    close(fd);
    return(FAIL);
  }

  // Owner-only permissions on the socket file.
  if(chmod(srv->sock_path, 0600) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "chmod(%s): %s",
        srv->sock_path, strerror(errno));
    close(fd);
    unlink(srv->sock_path);
    return(FAIL);
  }

  if(listen(fd, BCTL_MAX_CLIENTS) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "listen(): %s", strerror(errno));
    close(fd);
    unlink(srv->sock_path);
    return(FAIL);
  }

  // Set non-blocking so poll() works correctly.
  flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "fcntl(O_NONBLOCK): %s", strerror(errno));
    close(fd);
    unlink(srv->sock_path);
    return(FAIL);
  }

  srv->listen_fd = fd;
  clam(CLAM_INFO, "botmanctl", "listening on %s", srv->sock_path);
  return(SUCCESS);
}

static void
bctl_drv_disconnect(void *handle, const char *reason)
{
  bctl_server_t *srv = handle;
  bctl_client_t *c;

  // Nothing to say it to: this tears the listening socket down, and a
  // client still attached is reading a stream that ends, not a line.
  // The operator who typed the reason is on the other end of one of
  // these sockets and already has it in their own scrollback.
  (void)reason;

  if(srv == NULL)
    return;

  // Unsubscribe shared CLAM subscriber if active.
  if(bctl_clam_subscribed)
  {
    bctl_clam_subscribed = false;
    clam_unsubscribe("botmanctl");
  }

  // Close all clients.
  pthread_mutex_lock(&srv->client_mutex);

  c = srv->clients;
  while(c != NULL)
  {
    bctl_client_t *next = c->next;

    close(c->fd);

    if(c->mode == BCTL_MODE_SUBSCRIBE && c->has_clam_regex)
      regfree(&c->clam_regex);

    mem_free(c);
    c = next;
  }

  srv->clients = NULL;
  srv->client_count = 0;

  pthread_mutex_unlock(&srv->client_mutex);

  if(srv->listen_fd >= 0)
  {
    close(srv->listen_fd);
    srv->listen_fd = -1;
  }

  if(srv->sock_path[0] != '\0')
  {
    unlink(srv->sock_path);
    srv->sock_path[0] = '\0';
  }
}

// A route token is 'c' + the decimal client id. Parsed rather than
// trusted: any caller may hand method_send() any string.
static bool
bctl_route_id(const char *target, uint64_t *id)
{
  char     *end;
  uint64_t  val;

  if(target == NULL || target[0] != 'c' || target[1] == '\0')
    return(false);

  errno = 0;
  val   = strtoull(target + 1, &end, 10);

  if(errno != 0 || *end != '\0' || val == 0)
    return(false);

  *id = val;
  return(true);
}

// One write per line: the newline used to be a second write() and two
// threads' lines interleaved inside one line (root TODO.md §OBS-29 T5).
static size_t
bctl_line_build(char *out, size_t out_sz, const char *text)
{
  size_t n = strnlen(text, out_sz - 1);

  if(n == 0)
    return(0);

  memcpy(out, text, n);
  out[n++] = '\n';
  return(n);
}

// The dispatch thread's writer. Blocking, and it must stay blocking: a
// long `show kv` is hundreds of lines and dropping any of them is a
// silent truncation of an operator's output.
static bool
bctl_client_write(bctl_client_t *c, const char *text)
{
  char   out[METHOD_TEXT_SZ + 1];
  size_t n;
  size_t off = 0;

  if(c == NULL || c->fd < 0 || text == NULL)
    return(FAIL);

  n = bctl_line_build(out, sizeof(out), text);
  if(n == 0)
    return(SUCCESS);

  while(off < n)
  {
    ssize_t wrote = write(c->fd, out + off, n - off);

    if(wrote < 0)
    {
      if(errno == EINTR)
        continue;

      return(FAIL);
    }

    off += (size_t)wrote;
  }

  return(SUCCESS);
}

// The foreign thread's writer, called with client_mutex held — which
// bctl_clam_cb takes while holding clam_mutex, so this must never block
// and must never clam(): a stalled reader here stops every clam() in
// the daemon. A late reply is best-effort by that constraint; EAGAIN
// drops the line.
static bool
bctl_client_send_locked(bctl_client_t *c, const char *text)
{
  char    out[METHOD_TEXT_SZ + 1];
  size_t  n;
  ssize_t wrote;

  n = bctl_line_build(out, sizeof(out), text);
  if(n == 0)
    return(SUCCESS);

  wrote = send(c->fd, out, n, MSG_DONTWAIT | MSG_NOSIGNAL);
  return(wrote == (ssize_t)n ? SUCCESS : FAIL);
}

static bool
bctl_drv_send(void *handle, const char *target, const char *text)
{
  bctl_server_t *srv = handle;
  uint64_t       id;
  bool           rc = FAIL;

  if(srv == NULL || text == NULL)
    return(FAIL);

  if(bctl_route_id(target, &id))
  {
    // The dispatch running on THIS thread owns this client, and the
    // sweep that could free it runs on this thread too — so no lock,
    // and the blocking write every long reply depends on.
    if(bctl_dispatch_client != NULL && bctl_dispatch_client->id == id)
      return(bctl_client_write(bctl_dispatch_client, text));

    // Otherwise the answer outlived its dispatch: resolve the client by
    // an id that is never reused, and write it under the list lock so
    // the fd cannot be closed and recycled underneath us.
    pthread_mutex_lock(&srv->client_mutex);

    for(bctl_client_t *c = srv->clients; c != NULL; c = c->next)
    {
      if(c->id != id || c->closing || c->fd < 0)
        continue;

      rc = bctl_client_send_locked(c, text);
      break;
    }

    pthread_mutex_unlock(&srv->client_mutex);

    // A route that resolves to nothing is a client that has gone, and
    // it must NOT fall through to whoever this thread is serving —
    // that fall-through is the misroute rebuilt by hand.
    return(rc);
  }

  // No route: a caller that reached method_send() with a sender or a
  // channel — a plugin replying by hand, the driver's own session
  // replies. It can only mean the dispatch on this thread, and on any
  // other thread it means nobody.
  if(bctl_dispatch_client == NULL)
    return(FAIL);

  return(bctl_client_write(bctl_dispatch_client, text));
}

static bool
bctl_drv_get_context(void *handle, const char *sender,
    char *ctx, size_t ctx_sz)
{
  (void)handle;
  (void)sender;

  strlcpy(ctx, "botmanctl", ctx_sz);
  return(SUCCESS);
}

// ANSI color table for botmanctl terminal output.
static const color_table_t bctl_colors = {
  .red    = CON_RED,
  .green  = CON_GREEN,
  .yellow = CON_YELLOW,
  .blue   = CON_BLUE,
  .purple = CON_PURPLE,
  .cyan   = CON_CYAN,
  .white  = CON_WHITE,
  .orange = CON_ORANGE,
  .gray   = CON_GRAY,
  .bold   = CON_BOLD,
  .reset  = CON_RESET,
};

// Method driver definition.
static const method_driver_t bctl_driver = {
  .name        = "botmanctl",
  .colors      = &bctl_colors,
  .create      = bctl_drv_create,
  .destroy     = bctl_drv_destroy,
  .connect     = bctl_drv_connect,
  .disconnect  = bctl_drv_disconnect,
  .send        = bctl_drv_send,
  .get_context = bctl_drv_get_context,
};

// Command dispatch

static void
bctl_dispatch(bctl_server_t *srv, bctl_client_t *c, char *line)
{
  size_t len;
  char *args;
  const char *as_user;
  char route[METHOD_ROUTE_SZ];
  userns_t *ns = NULL;

  // Strip leading whitespace.
  while(*line == ' ' || *line == '\t')
    line++;

  // Strip trailing whitespace/newline.
  len = strlen(line);
  while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
        || line[len - 1] == ' ' || line[len - 1] == '\t'))
    line[--len] = '\0';

  if(len == 0)
    return;

  // Check for SUBSCRIBE command before stripping '/'.
  if(strncasecmp(line, "SUBSCRIBE", 9) == 0
      && (line[9] == ' ' || line[9] == '\0' || line[9] == '\n'))
  {
    char *sub_args = line + 9;

    while(*sub_args == ' ' || *sub_args == '\t')
      sub_args++;

    bctl_handle_subscribe(srv, c, sub_args);
    return;
  }

  // Session control: "AS <username>" sets the asserted identity for
  // subsequent command dispatches on this connection. Default is
  // @owner (initialized on client connect). Empty resets to default.
  // This is how `botmanctl --as-user <name>` is wired: the tool
  // emits an AS line on connect, then user commands flow normally
  // and the command system evaluates permissions against <name>.
  if(strncasecmp(line, "AS", 2) == 0
      && (line[2] == ' ' || line[2] == '\0' || line[2] == '\n'))
  {
    char *as_args = line + 2;
    char reply[USERNS_USER_SZ + 32];

    while(*as_args == ' ' || *as_args == '\t') as_args++;

    if(as_args[0] == '\0')
      snprintf(c->as_user, sizeof(c->as_user), "%s", USERNS_OWNER_USER);
    else
      snprintf(c->as_user, sizeof(c->as_user), "%s", as_args);

    snprintf(reply, sizeof(reply), "now dispatching as: %s", c->as_user);
    bctl_client_write(c, reply);
    return;
  }

  // Strip optional leading '/'.
  if(line[0] == '/')
    line++;

  // Split command name from arguments.
  args = line;

  while(*args != '\0' && *args != ' ' && *args != '\t')
    args++;

  if(*args != '\0')
  {
    *args = '\0';
    args++;

    while(*args == ' ' || *args == '\t')
      args++;
  }

  // Name this client twice: once for the reply the command body writes
  // on this thread, and once — as a route the message carries — for the
  // reply an async body produces after this call has returned.
  snprintf(route, sizeof(route), "c%" PRIu64, c->id);
  bctl_dispatch_client = c;

  // Dispatch via the unified command system. Identity is asserted
  // from this client's session state — defaulting to @owner at connect
  // time and overridable with an AS <user> session command.
  as_user = (c->as_user[0] != '\0')
      ? c->as_user : USERNS_OWNER_USER;

  // Resolve the botmanctl-side userns. Any userns containing @owner
  // works; we pick the first one loaded. If no userns exists yet
  // (early boot before any bot created one), the default-groups
  // seeding in userns.c hasn't run yet, so we pass NULL and rely on
  // check_permission's synthetic full-membership path for @owner.
  {
    extern userns_t *userns_first(void);
    ns = userns_first();
  }

  if(cmd_dispatch_as(line, args, srv->inst, ns, as_user, route) != SUCCESS)
  {
    // Send error back to client.
    char errmsg[BCTL_INPUT_SZ];

    snprintf(errmsg, sizeof(errmsg), "unknown command: %s (try help)", line);
    bctl_client_write(c, errmsg);
  }

  bctl_dispatch_client = NULL;
}

// Persist task: poll listener and clients

static void
bctl_task_cb(task_t *t)
{
  bctl_server_t *srv;

  srv = bctl_state;
  if(srv == NULL || srv->listen_fd < 0)
  {
    t->state = TASK_ENDED;
    return;
  }

  while(!pool_shutting_down() && !bctl_stopping)
  {
    // Build pollfd array: slot 0 = listener, slots 1..N = clients.
    struct pollfd fds[1 + BCTL_MAX_CLIENTS];
    bctl_client_t *client_map[BCTL_MAX_CLIENTS];
    int nfds = 0;
    int ret;

    fds[0].fd = srv->listen_fd;
    fds[0].events = POLLIN;
    nfds = 1;

    pthread_mutex_lock(&srv->client_mutex);

    for(bctl_client_t *c = srv->clients; c != NULL; c = c->next)
    {
      if(nfds >= 1 + BCTL_MAX_CLIENTS)
        break;

      fds[nfds].fd = c->fd;
      fds[nfds].events = POLLIN;
      client_map[nfds - 1] = c;
      nfds++;
    }

    pthread_mutex_unlock(&srv->client_mutex);

    ret = poll(fds, nfds, 500);
    if(ret <= 0)
      continue;

    // Check listener for new connections.
    if(fds[0].revents & POLLIN)
    {
      int cfd;

      cfd = accept4(srv->listen_fd, NULL, NULL, SOCK_CLOEXEC);
      if(cfd >= 0)
        bctl_client_add(srv, cfd);
    }

    if(fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      clam(CLAM_WARN, "botmanctl", "listener error");
      break;
    }

    // Check each client.
    for(int i = 1; i < nfds; i++)
    {
      bctl_client_t *c = client_map[i - 1];

      if(fds[i].revents & POLLIN)
      {
        char buf[BCTL_INPUT_SZ];
        ssize_t n;

        n = read(c->fd, buf, sizeof(buf) - 1);
        if(n <= 0)
          c->closing = true;

        else
        {
          char *saveptr = NULL;
          char *line;

          buf[n] = '\0';

          // Process each line in the received data.
          line = strtok_r(buf, "\n", &saveptr);

          while(line != NULL)
          {
            if(c->mode == BCTL_MODE_COMMAND)
              bctl_dispatch(srv, c, line);

            // Send end-of-response delimiter (command mode only).
            if(c->mode == BCTL_MODE_COMMAND && !c->closing)
            {
              char nul = '\0';

              if(write(c->fd, &nul, 1) < 0)
              {
                c->closing = true;
                break;
              }
            }

            line = strtok_r(NULL, "\n", &saveptr);
          }
        }
      }

      if(fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
        c->closing = true;
    }

    // Sweep: remove all clients marked for closing.
    // Swept without client_mutex, and correctly: this thread is the
    // list's only mutator (bctl_client_add and bctl_client_remove both
    // run here), and the one foreign mutator — bctl_drv_disconnect —
    // can no longer run while this thread lives (botmanctl_exit joins
    // it first). Taking the lock here would be wrong as well as
    // unnecessary: bctl_client_remove takes it and clam()s under it.
    {
      bctl_client_t *c = srv->clients;

      while(c != NULL)
      {
        bctl_client_t *next = c->next;

        if(c->closing)
          bctl_client_remove(srv, c);

        c = next;
      }
    }
  }

  t->state = TASK_ENDED;
}

// Per-session state accessors

const char *
botmanctl_get_user_ns(void)
{
  if(bctl_dispatch_client == NULL)
    return("");

  return(bctl_dispatch_client->user_ns_cd);
}

void
botmanctl_set_user_ns(const char *name)
{
  if(bctl_dispatch_client == NULL || name == NULL)
    return;

  strlcpy(bctl_dispatch_client->user_ns_cd, name,
      sizeof(bctl_dispatch_client->user_ns_cd));
}

// Public API

// Register KV configuration keys for the botmanctl method.
// Sets up core.botmanctl.sockpath with a default of ~/.config/botmanager/botman.sock.
void
botmanctl_register_config(void)
{
  // Build default path: ~/.config/botmanager/botman.sock
  char defpath[BCTL_SOCK_PATH_SZ];
  const char *home;

  home = getenv("HOME");
  if(home != NULL)
    snprintf(defpath, sizeof(defpath), "%s/.config/botmanager/botman.sock",
        home);
  else
    snprintf(defpath, sizeof(defpath), "/tmp/botman.sock");

  kv_register("core.botmanctl.sockpath", KV_STR, defpath, NULL, NULL,
      "Unix socket path for botmanctl connections");
}

// Register the botmanctl method instance, bind the Unix socket, and
// start the persist task for accepting client connections.
void
botmanctl_register_method(void)
{
  method_inst_t *inst;
  task_handle_t t;

  inst = method_register(&bctl_driver, "botmanctl");
  if(inst == NULL)
  {
    clam(CLAM_WARN, "botmanctl", "failed to register method instance");
    return;
  }

  // bctl_drv_create() was called by method_register() and set bctl_state.
  bctl_state->inst = inst;

  // Connect (bind/listen the socket).
  if(method_connect(inst) != SUCCESS)
  {
    clam(CLAM_WARN, "botmanctl", "failed to start listener");
    method_unregister("botmanctl");
    bctl_state = NULL;
    return;
  }

  // Start the persist task.
  t = task_add_persist("botmanctl", 0, bctl_task_cb, NULL);
  bctl_task = t;

  if(t == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, "botmanctl", "failed to start listener task");
    method_unregister("botmanctl");
    bctl_state = NULL;
    return;
  }

  method_set_state(inst, METHOD_AVAILABLE);
  bctl_active = true;

  clam(CLAM_INFO, "botmanctl", "registered as method instance");
}

// Shut down the botmanctl method. Unregisters the method instance,
// which triggers disconnect (closing sockets and unlinking the socket file).
void
botmanctl_exit(void)
{
  if(!bctl_active)
    return;

  clam(CLAM_INFO, "botmanctl", "shutting down");

  bctl_active   = false;
  bctl_stopping = true;

  // The poll thread is reading the very clients method_unregister is
  // about to free. Refusing to unregister is the safe answer to a join
  // that did not complete: an instance leaked at exit costs a mem_exit
  // line, and freeing it here costs a use-after-free.
  if(!task_persist_join(bctl_task, BCTL_STOP_WAIT_MS))
  {
    clam(CLAM_WARN, "botmanctl",
        "poll thread still running after %d ms; leaving the socket up "
        "(its clients and server block will show in mem_exit)",
        BCTL_STOP_WAIT_MS);
    return;
  }

  bctl_task  = TASK_HANDLE_NONE;
  bctl_state = NULL;

  method_unregister("botmanctl");
}
