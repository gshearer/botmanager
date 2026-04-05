#define BOTMANCTL_INTERNAL
#include "botmanctl.h"

#include <fcntl.h>
#include <sys/stat.h>

// -----------------------------------------------------------------------
// Method driver callbacks
// -----------------------------------------------------------------------

// Allocate and initialize botmanctl method instance state.
// returns: pointer to new botmanctl_state_t, or NULL on failure
// inst_name: method instance name (unused)
static void *
bctl_drv_create(const char *inst_name)
{
  (void)inst_name;

  botmanctl_state_t *st = mem_alloc("botmanctl", "state",
      sizeof(botmanctl_state_t));

  if(st == NULL)
    return(NULL);

  st->listen_fd = -1;
  st->client_fd = -1;
  st->sock_path[0] = '\0';
  st->inst = NULL;

  // Stash module-level pointer for the persist task.
  bctl_state = st;
  return(st);
}

// Free the botmanctl method instance state.
// handle: botmanctl_state_t pointer (may be NULL)
static void
bctl_drv_destroy(void *handle)
{
  if(handle != NULL)
    mem_free(handle);
}

// Connect: create the Unix domain socket, bind, and listen.
static bool
bctl_drv_connect(void *handle)
{
  botmanctl_state_t *st = handle;

  if(st == NULL)
    return(FAIL);

  if(st->listen_fd >= 0)
    return(SUCCESS);

  // Resolve socket path from KV.
  const char *path = kv_get_str("core.botmanctl.sockpath");

  if(path == NULL || path[0] == '\0')
  {
    clam(CLAM_WARN, "botmanctl", "sockpath not configured");
    return(FAIL);
  }

  snprintf(st->sock_path, sizeof(st->sock_path), "%s", path);

  // Ensure parent directory tree exists.
  char dir[BCTL_SOCK_PATH_SZ];

  snprintf(dir, sizeof(dir), "%s", st->sock_path);

  char *slash = strrchr(dir, '/');

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
  unlink(st->sock_path);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if(fd < 0)
  {
    clam(CLAM_WARN, "botmanctl", "socket(): %s", strerror(errno));
    return(FAIL);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", st->sock_path);

  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "bind(%s): %s",
        st->sock_path, strerror(errno));
    close(fd);
    return(FAIL);
  }

  // Owner-only permissions on the socket file.
  if(chmod(st->sock_path, 0600) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "chmod(%s): %s",
        st->sock_path, strerror(errno));
    close(fd);
    unlink(st->sock_path);
    return(FAIL);
  }

  if(listen(fd, 1) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "listen(): %s", strerror(errno));
    close(fd);
    unlink(st->sock_path);
    return(FAIL);
  }

  // Set non-blocking so poll() works correctly.
  int flags = fcntl(fd, F_GETFL, 0);

  if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
  {
    clam(CLAM_WARN, "botmanctl", "fcntl(O_NONBLOCK): %s", strerror(errno));
    close(fd);
    unlink(st->sock_path);
    return(FAIL);
  }

  st->listen_fd = fd;
  clam(CLAM_INFO, "botmanctl", "listening on %s", st->sock_path);

  clam(CLAM_INFO, "botmanctl", "listening on %s", st->sock_path);
  return(SUCCESS);
}

// Disconnect: close client and listener sockets and remove the socket file.
// handle: botmanctl_state_t pointer (may be NULL)
static void
bctl_drv_disconnect(void *handle)
{
  botmanctl_state_t *st = handle;

  if(st == NULL)
    return;

  if(st->client_fd >= 0)
  {
    close(st->client_fd);
    st->client_fd = -1;
  }

  if(st->listen_fd >= 0)
  {
    close(st->listen_fd);
    st->listen_fd = -1;
  }

  if(st->sock_path[0] != '\0')
  {
    unlink(st->sock_path);
    st->sock_path[0] = '\0';
  }
}

// Send: write response text to the currently connected client.
static bool
bctl_drv_send(void *handle, const char *target, const char *text)
{
  botmanctl_state_t *st = handle;
  (void)target;

  if(st == NULL || st->client_fd < 0 || text == NULL)
    return(FAIL);

  size_t len = strlen(text);

  if(len == 0)
    return(SUCCESS);

  // Write text followed by newline.
  if(write(st->client_fd, text, len) < 0)
    return(FAIL);

  if(write(st->client_fd, "\n", 1) < 0)
    return(FAIL);

  return(SUCCESS);
}

// Get the context string for botmanctl (always "botmanctl").
// returns: SUCCESS always
// handle: botmanctl_state_t pointer (unused)
// sender: message sender (unused)
// ctx: output buffer for context string
// ctx_sz: size of ctx buffer
static bool
bctl_drv_get_context(void *handle, const char *sender,
    char *ctx, size_t ctx_sz)
{
  (void)handle;
  (void)sender;

  strncpy(ctx, "botmanctl", ctx_sz - 1);
  ctx[ctx_sz - 1] = '\0';
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

// -----------------------------------------------------------------------
// Command dispatch
// -----------------------------------------------------------------------

// Parse and dispatch a command line received from a botmanctl client.
// Strips whitespace and optional leading '/', splits command from args,
// and dispatches via the unified command system.
// st: botmanctl state (used for method instance and error replies)
// line: raw input line (modified in place)
static void
bctl_dispatch(botmanctl_state_t *st, char *line)
{
  // Strip leading whitespace.
  while(*line == ' ' || *line == '\t')
    line++;

  // Strip trailing whitespace/newline.
  size_t len = strlen(line);

  while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
        || line[len - 1] == ' ' || line[len - 1] == '\t'))
    line[--len] = '\0';

  if(len == 0)
    return;

  // Strip optional leading '/'.
  if(line[0] == '/')
    line++;

  // Split command name from arguments.
  char *args = line;

  while(*args != '\0' && *args != ' ' && *args != '\t')
    args++;

  if(*args != '\0')
  {
    *args = '\0';
    args++;

    while(*args == ' ' || *args == '\t')
      args++;
  }

  // Dispatch via unified command system on our method instance.
  if(cmd_dispatch_owner(line, args, st->inst) != SUCCESS)
  {
    // Send error back to client.
    char errmsg[BCTL_INPUT_SZ];
    snprintf(errmsg, sizeof(errmsg), "unknown command: %s (try help)", line);
    bctl_drv_send(st, NULL, errmsg);
  }
}

// -----------------------------------------------------------------------
// Persist task: poll listener and client
// -----------------------------------------------------------------------

// Persist task callback: polls the listener for new connections and the
// client for incoming command lines. Runs until pool shutdown or error.
// t: the persist task
static void
bctl_task_cb(task_t *t)
{
  botmanctl_state_t *st = bctl_state;

  if(st == NULL || st->listen_fd < 0)
  {
    t->state = TASK_ENDED;
    return;
  }

  while(!pool_shutting_down())
  {
    struct pollfd fds[2];
    int nfds = 0;

    // Always poll the listener.
    fds[0].fd = st->listen_fd;
    fds[0].events = POLLIN;
    nfds = 1;

    // Also poll the client if connected.
    if(st->client_fd >= 0)
    {
      fds[1].fd = st->client_fd;
      fds[1].events = POLLIN;
      nfds = 2;
    }

    int ret = poll(fds, nfds, 500);

    if(ret <= 0)
      continue;

    // Check listener for new connections.
    if(fds[0].revents & POLLIN)
    {
      int cfd = accept(st->listen_fd, NULL, NULL);

      if(cfd >= 0)
      {
        // Close existing client if any.
        if(st->client_fd >= 0)
          close(st->client_fd);

        st->client_fd = cfd;

        clam(CLAM_DEBUG, "botmanctl", "client connected");
      }
    }

    if(fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      clam(CLAM_WARN, "botmanctl", "listener error");
      break;
    }

    // Check client for data or disconnect.
    if(nfds == 2 && st->client_fd >= 0)
    {
      if(fds[1].revents & POLLIN)
      {
        char buf[BCTL_INPUT_SZ];
        ssize_t n = read(st->client_fd, buf, sizeof(buf) - 1);

        if(n <= 0)
        {
          // Client disconnected.
          close(st->client_fd);
          st->client_fd = -1;
          clam(CLAM_DEBUG, "botmanctl", "client disconnected");
        }
        else
        {
          buf[n] = '\0';

          // Process each line in the received data.
          char *saveptr = NULL;
          char *line = strtok_r(buf, "\n", &saveptr);

          while(line != NULL)
          {
            bctl_dispatch(st, line);

            // Send end-of-response delimiter.
            if(st->client_fd >= 0)
            {
              char nul = '\0';

              if(write(st->client_fd, &nul, 1) < 0)
              {
                close(st->client_fd);
                st->client_fd = -1;
                clam(CLAM_DEBUG, "botmanctl", "client write error");
                break;
              }
            }

            line = strtok_r(NULL, "\n", &saveptr);
          }
        }
      }

      if(fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
      {
        close(st->client_fd);
        st->client_fd = -1;
        clam(CLAM_DEBUG, "botmanctl", "client disconnected");
      }
    }
  }

  t->state = TASK_ENDED;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Register KV configuration keys for the botmanctl method.
// Sets up core.botmanctl.sockpath with a default of ~/.config/botmanager/botman.sock.
void
botmanctl_register_config(void)
{
  // Build default path: ~/.config/botmanager/botman.sock
  char defpath[BCTL_SOCK_PATH_SZ];
  const char *home = getenv("HOME");

  if(home != NULL)
    snprintf(defpath, sizeof(defpath), "%s/.config/botmanager/botman.sock",
        home);
  else
    snprintf(defpath, sizeof(defpath), "/tmp/botman.sock");

  kv_register("core.botmanctl.sockpath", KV_STR, defpath, NULL, NULL);
}

// Register the botmanctl method instance, bind the Unix socket, and
// start the persist task for accepting client connections.
void
botmanctl_register_method(void)
{
  method_inst_t *inst = method_register(&bctl_driver, "botmanctl");

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
  task_t *t = task_add_persist("botmanctl", 0, bctl_task_cb, NULL);

  if(t == NULL)
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

  bctl_active = false;
  bctl_state = NULL;

  method_unregister("botmanctl");
}
