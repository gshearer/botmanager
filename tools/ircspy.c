// ircspy — Minimal IRC client for AI agent observation and debugging.
//
// Usage:
//   ircspy [options]
//
// Connects to an IRC server, joins a channel, and relays traffic to stdout.
// Designed for non-interactive use via stdin/stdout pipes.
//
// Examples:
//   ircspy                                          # defaults
//   ircspy -n claude -c "#test"                     # custom nick/channel
//   echo -e "!help\n/quit" | timeout 15 ircspy      # piped commands
//   ircspy -r > /tmp/raw.log 2>&1 &                 # raw protocol log

#define IRCSPY_INTERNAL
#include "ircspy.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/err.h>

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------

static void
sig_handler(int sig)
{
  (void)sig;
  g_quit = 1;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

// Write len bytes to the IRC connection.
// returns: number of bytes written, or -1 on error.
static int
irc_write(const void *buf, int len)
{
  if(g_ssl != NULL)
    return(SSL_write(g_ssl, buf, len));

  return((int)write(g_fd, buf, (size_t)len));
}

// Read up to len bytes from the IRC connection.
// returns: number of bytes read, 0 on EOF, or -1 on error.
static int
irc_read(void *buf, int len)
{
  if(g_ssl != NULL)
    return(SSL_read(g_ssl, buf, len));

  return((int)read(g_fd, buf, (size_t)len));
}

// Send a formatted IRC protocol line (appends \r\n).
static void
irc_send(const char *fmt, ...)
{
  char buf[CMD_SZ];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);

  if(n < 0)
    return;

  buf[n++] = '\r';
  buf[n++] = '\n';

  if(irc_write(buf, n) < 0)
    fprintf(stderr, "irc_send: write error\n");
}

// ---------------------------------------------------------------------------
// TCP connection
// ---------------------------------------------------------------------------

// Connect to host:port via TCP.
// returns: socket fd on success, -1 on failure.
static int
tcp_connect(const char *host, uint16_t port)
{
  char portstr[8];
  snprintf(portstr, sizeof(portstr), "%u", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;

  if(getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
  {
    fprintf(stderr, "ircspy: cannot resolve %s:%u\n", host, port);
    return(-1);
  }

  int fd = -1;

  for(struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

    if(fd < 0)
      continue;

    if(connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
      break;

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);

  if(fd < 0)
    fprintf(stderr, "ircspy: connect to %s:%u failed: %s\n",
            host, port, strerror(errno));

  return(fd);
}

// ---------------------------------------------------------------------------
// TLS setup
// ---------------------------------------------------------------------------

// Initialize TLS on an existing socket fd.
// returns: 0 on success, -1 on failure.
static int
tls_setup(int fd, int verify)
{
  g_ctx = SSL_CTX_new(TLS_client_method());

  if(g_ctx == NULL)
  {
    fprintf(stderr, "ircspy: SSL_CTX_new failed\n");
    return(-1);
  }

  SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);

  if(!verify)
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);

  g_ssl = SSL_new(g_ctx);

  if(g_ssl == NULL)
  {
    fprintf(stderr, "ircspy: SSL_new failed\n");
    return(-1);
  }

  SSL_set_fd(g_ssl, fd);

  if(SSL_connect(g_ssl) <= 0)
  {
    fprintf(stderr, "ircspy: TLS handshake failed\n");
    ERR_print_errors_fp(stderr);
    return(-1);
  }

  return(0);
}

// ---------------------------------------------------------------------------
// IRC message parsing helpers
// ---------------------------------------------------------------------------

// Extract the nick portion from a :nick!user@host prefix.
// src: full prefix (without leading colon)
// dst: destination buffer
// sz: buffer size
static void
parse_nick(const char *src, char *dst, size_t sz)
{
  size_t i = 0;

  while(i < sz - 1 && src[i] != '\0' && src[i] != '!')
  {
    dst[i] = src[i];
    i++;
  }

  dst[i] = '\0';
}

// ---------------------------------------------------------------------------
// Control socket helpers
// ---------------------------------------------------------------------------

// Create and bind the control socket.
// path: Unix socket path
// returns: 0 on success, -1 on failure.
static int
ctl_setup(const char *path)
{
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if(fd < 0)
  {
    fprintf(stderr, "ircspy: ctl socket: %s\n", strerror(errno));
    return(-1);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    fprintf(stderr, "ircspy: ctl bind(%s): %s\n", path, strerror(errno));
    close(fd);
    return(-1);
  }

  if(listen(fd, 1) < 0)
  {
    fprintf(stderr, "ircspy: ctl listen: %s\n", strerror(errno));
    close(fd);
    unlink(path);
    return(-1);
  }

  snprintf(g_ctl_path, sizeof(g_ctl_path), "%s", path);
  g_ctl_listen = fd;
  return(0);
}

// Close and unlink the control socket.
static void
ctl_cleanup(void)
{
  if(g_ctl_client >= 0)
  {
    close(g_ctl_client);
    g_ctl_client = -1;
  }

  if(g_ctl_listen >= 0)
  {
    close(g_ctl_listen);
    g_ctl_listen = -1;
  }

  if(g_ctl_path[0] != '\0')
  {
    unlink(g_ctl_path);
    g_ctl_path[0] = '\0';
  }
}

// Send a line to the control client (if one is collecting).
// text: NUL-terminated string (newline is appended)
static void
ctl_send_line(const char *text)
{
  if(g_ctl_client < 0 || g_ctl_deadline == 0)
    return;

  if(write(g_ctl_client, text, strlen(text)) < 0 ||
     write(g_ctl_client, "\n", 1) < 0)
  {
    close(g_ctl_client);
    g_ctl_client = -1;
    g_ctl_deadline = 0;
    return;
  }

  // Reset idle deadline — more output may follow.
  g_ctl_deadline = time(NULL) + CTL_IDLE_SEC;
}

// Flush end-of-response to control client (NUL delimiter).
static void
ctl_flush(void)
{
  if(g_ctl_client < 0)
    return;

  char nul = '\0';

  if(write(g_ctl_client, &nul, 1) < 0)
  {
    close(g_ctl_client);
    g_ctl_client = -1;
    g_ctl_deadline = 0;
  }
  g_ctl_deadline = 0;
}

// ---------------------------------------------------------------------------
// IRC formatting stripping (for terminal display)
// ---------------------------------------------------------------------------

// Strip mIRC formatting codes (\003NN,NN colors, \002 bold, etc.)
// from text in-place for clean terminal output.
// text: NUL-terminated string (modified in-place)
static void
strip_irc_fmt(char *text)
{
  char *r = text, *w = text;

  while(*r != '\0')
  {
    if(*r == '\003')
    {
      r++;

      if(isdigit((unsigned char)*r)) r++;
      if(isdigit((unsigned char)*r)) r++;

      if(*r == ',')
      {
        r++;
        if(isdigit((unsigned char)*r)) r++;
        if(isdigit((unsigned char)*r)) r++;
      }

      continue;
    }

    // Bold (\002), reset (\017), reverse (\026), underline (\037).
    if(*r == '\002' || *r == '\017' || *r == '\026' || *r == '\037')
    {
      r++;
      continue;
    }

    *w++ = *r++;
  }

  *w = '\0';
}

// ---------------------------------------------------------------------------
// Handle a complete IRC protocol line from the server
// ---------------------------------------------------------------------------

// Handle numeric 001 (registration complete).
// returns: 1 if handled, 0 otherwise.
static int
handle_numeric_001(bool raw_mode)
{
  g_registered = true;

  if(!raw_mode)
  {
    printf("--- Registered as %s\n", g_nick);
    fflush(stdout);
  }

  return(1);
}

// Handle numeric 433 (nick in use) by retrying with a suffix.
// returns: 1 if handled, 0 otherwise.
static int
handle_numeric_433(bool raw_mode)
{
  if(g_nick_suffix < NICK_RETRY_MAX)
  {
    g_nick_suffix++;
    snprintf(g_nick, sizeof(g_nick), "%s%d",
             DEFAULT_NICK, g_nick_suffix);
    irc_send("NICK %s", g_nick);

    if(!raw_mode)
    {
      printf("--- Nick in use, trying %s\n", g_nick);
      fflush(stdout);
    }
  }
  else
  {
    fprintf(stderr, "ircspy: all nicks exhausted\n");
    g_quit = 1;
  }

  return(1);
}

// Handle PRIVMSG display in normal mode.
static void
handle_privmsg(const char *params, const char *nick)
{
  const char *target = params;
  const char *sp     = strchr(params, ' ');

  if(sp == NULL)
    return;

  char chan[CMD_SZ];
  size_t tlen = (size_t)(sp - target);

  if(tlen >= sizeof(chan))
    tlen = sizeof(chan) - 1;

  memcpy(chan, target, tlen);
  chan[tlen] = '\0';

  const char *text = sp + 1;

  if(text[0] == ':')
    text++;

  // Copy and strip IRC formatting for display.
  char clean[BUF_SZ];
  snprintf(clean, sizeof(clean), "%s", text);
  strip_irc_fmt(clean);

  char fmtline[BUF_SZ];

  snprintf(fmtline, sizeof(fmtline), "[%s] <%s> %s", chan, nick, clean);
  printf("%s\n", fmtline);
  fflush(stdout);
  ctl_send_line(fmtline);
}

// Handle NOTICE display in normal mode.
static void
handle_notice(const char *params, const char *nick)
{
  const char *text = strchr(params, ':');

  if(text != NULL)
    text++;
  else
    text = params;

  char clean[BUF_SZ];
  snprintf(clean, sizeof(clean), "%s", text);
  strip_irc_fmt(clean);

  char fmtline[BUF_SZ];

  snprintf(fmtline, sizeof(fmtline), "--- Notice(%s): %s", nick, clean);
  printf("%s\n", fmtline);
  fflush(stdout);
  ctl_send_line(fmtline);
}

// Handle JOIN display in normal mode.
static void
handle_join(const char *params, const char *nick)
{
  const char *chan = params;

  if(chan[0] == ':')
    chan++;

  if(strcmp(nick, g_nick) == 0)
  {
    printf("--- Joined %s\n", chan);

    // Track current channel.
    snprintf(g_channel, sizeof(g_channel), "%s", chan);
  }
  else
  {
    printf("--- %s has joined %s\n", nick, chan);
  }

  fflush(stdout);
}

// Handle NICK change display in normal mode.
static void
handle_nick_change(const char *params, const char *nick)
{
  const char *newnick = params;

  if(newnick[0] == ':')
    newnick++;

  if(strcmp(nick, g_nick) == 0)
    snprintf(g_nick, sizeof(g_nick), "%s", newnick);

  printf("--- %s is now known as %s\n", nick, newnick);
  fflush(stdout);
}

// Handle QUIT message display in normal mode.
static void
handle_quit_msg(const char *params, const char *nick)
{
  const char *msg = params;

  if(msg != NULL && msg[0] == ':')
    msg++;

  printf("--- %s has quit (%s)\n", nick, msg != NULL ? msg : "");
  fflush(stdout);
}

static void
handle_server_line(const char *line, bool raw_mode)
{
  if(raw_mode)
  {
    printf("%s\n", line);
    fflush(stdout);
  }

  // PING handling (always, regardless of mode).
  if(strncmp(line, "PING ", 5) == 0)
  {
    irc_send("PONG %s", line + 5);
    return;
  }

  // Parse prefix and command.
  // Format: [:prefix] command [params...]
  const char *prefix = NULL;
  const char *cmd    = line;

  if(line[0] == ':')
  {
    prefix = line + 1;
    cmd = strchr(line, ' ');

    if(cmd == NULL)
      return;

    cmd++;
  }

  // Skip to parameters.
  const char *params = strchr(cmd, ' ');

  // Identify the command token length.
  size_t cmdlen = params != NULL ? (size_t)(params - cmd) : strlen(cmd);

  if(params != NULL)
    params++;

  // Numeric: 001 — registration complete.
  if(cmdlen == 3 && strncmp(cmd, "001", 3) == 0)
  {
    handle_numeric_001(raw_mode);
    return;
  }

  // Numeric: 433 — nick in use.
  if(cmdlen == 3 && strncmp(cmd, "433", 3) == 0)
  {
    handle_numeric_433(raw_mode);
    return;
  }

  // In raw mode we already printed; skip formatted output.
  if(raw_mode)
    return;

  // Suppress most numerics in normal mode (except 4xx/5xx errors).
  if(cmdlen == 3 && isdigit((unsigned char)cmd[0]))
  {
    int num = (cmd[0] - '0') * 100 + (cmd[1] - '0') * 10 + (cmd[2] - '0');

    if(num >= 400 && num < 600 && params != NULL)
    {
      printf("--- Error %d: %s\n", num, params);
      fflush(stdout);
    }

    return;
  }

  char nick[CMD_SZ];

  if(prefix != NULL)
    parse_nick(prefix, nick, sizeof(nick));
  else
    nick[0] = '\0';

  // PRIVMSG
  if(cmdlen == 7 && strncmp(cmd, "PRIVMSG", 7) == 0 && params != NULL)
  {
    handle_privmsg(params, nick);
    return;
  }

  // NOTICE
  if(cmdlen == 6 && strncmp(cmd, "NOTICE", 6) == 0 && params != NULL)
  {
    handle_notice(params, nick);
    return;
  }

  // JOIN
  if(cmdlen == 4 && strncmp(cmd, "JOIN", 4) == 0 && params != NULL)
  {
    handle_join(params, nick);
    return;
  }

  // PART
  if(cmdlen == 4 && strncmp(cmd, "PART", 4) == 0 && params != NULL)
  {
    printf("--- %s has left %s\n", nick, params);
    fflush(stdout);
    return;
  }

  // QUIT
  if(cmdlen == 4 && strncmp(cmd, "QUIT", 4) == 0)
  {
    handle_quit_msg(params, nick);
    return;
  }

  // KICK
  if(cmdlen == 4 && strncmp(cmd, "KICK", 4) == 0 && params != NULL)
  {
    printf("--- KICK: %s\n", params);
    fflush(stdout);
    return;
  }

  // NICK change
  if(cmdlen == 4 && strncmp(cmd, "NICK", 4) == 0 && params != NULL)
  {
    handle_nick_change(params, nick);
    return;
  }
}

// ---------------------------------------------------------------------------
// Handle user input from stdin
// ---------------------------------------------------------------------------

static void
handle_user_input(const char *line)
{
  // Skip empty lines.
  if(line[0] == '\0')
    return;

  // Commands start with '/'.
  if(line[0] == '/')
  {
    const char *arg = line + 1;

    if(strncmp(arg, "quit", 4) == 0)
    {
      const char *msg = arg + 4;

      while(*msg == ' ')
        msg++;

      irc_send("QUIT :%s", *msg != '\0' ? msg : "leaving");
      g_quit = 1;
      return;
    }

    if(strncmp(arg, "join ", 5) == 0)
    {
      const char *chan = arg + 5;

      while(*chan == ' ')
        chan++;

      irc_send("JOIN %s", chan);
      return;
    }

    if(strncmp(arg, "part ", 5) == 0)
    {
      const char *chan = arg + 5;

      while(*chan == ' ')
        chan++;

      irc_send("PART %s", chan);
      return;
    }

    if(strncmp(arg, "msg ", 4) == 0)
    {
      const char *rest = arg + 4;

      while(*rest == ' ')
        rest++;

      // Target is first word, text is the remainder.
      const char *sp = strchr(rest, ' ');

      if(sp != NULL)
        irc_send("PRIVMSG %.*s :%s", (int)(sp - rest), rest, sp + 1);

      return;
    }

    if(strncmp(arg, "nick ", 5) == 0)
    {
      const char *nn = arg + 5;

      while(*nn == ' ')
        nn++;

      snprintf(g_nick, sizeof(g_nick), "%s", nn);
      irc_send("NICK %s", nn);
      return;
    }

    if(strncmp(arg, "raw ", 4) == 0)
    {
      irc_send("%s", arg + 4);
      return;
    }

    // Unknown command — send as raw.
    irc_send("%s", arg);
    return;
  }

  // Bare text — send to current channel.
  if(g_channel[0] != '\0')
    irc_send("PRIVMSG %s :%s", g_channel, line);
  else
    fprintf(stderr, "ircspy: no channel joined, use /join first\n");
}

// ---------------------------------------------------------------------------
// Process buffered data from the IRC socket
// ---------------------------------------------------------------------------

static void
process_lines(bool raw_mode)
{
  // Scan for complete \r\n-terminated lines.
  for(;;)
  {
    char *crlf = NULL;

    for(int i = 0; i < g_lineoff - 1; i++)
    {
      if(g_linebuf[i] == '\r' && g_linebuf[i + 1] == '\n')
      {
        crlf = &g_linebuf[i];
        break;
      }
    }

    if(crlf == NULL)
      break;

    *crlf = '\0';

    handle_server_line(g_linebuf, raw_mode);

    // Shift remainder forward.
    int consumed = (int)(crlf - g_linebuf) + 2;
    g_lineoff -= consumed;

    if(g_lineoff > 0)
      memmove(g_linebuf, crlf + 2, (size_t)g_lineoff);
  }
}

// ---------------------------------------------------------------------------
// Main loop — poll set construction and per-source event handlers
// ---------------------------------------------------------------------------

// Poll index tracker for the dynamic poll set.
struct poll_idx
{
  int irc;
  int stdin_fd;
  int ctl_listen;
  int ctl_client;
  int nfds;
};

// Build the dynamic poll set for the main loop.
// fds: pollfd array (must hold at least 4 entries)
// returns: populated poll_idx with per-source indices (-1 if absent).
static struct poll_idx
build_poll_set(struct pollfd *fds)
{
  struct poll_idx idx = { -1, -1, -1, -1, 0 };

  idx.irc = idx.nfds;
  fds[idx.nfds].fd     = g_fd;
  fds[idx.nfds].events = POLLIN;
  idx.nfds++;

  if(g_stdin_open)
  {
    idx.stdin_fd = idx.nfds;
    fds[idx.nfds].fd     = STDIN_FILENO;
    fds[idx.nfds].events = POLLIN;
    idx.nfds++;
  }

  if(g_ctl_listen >= 0 && g_ctl_client < 0)
  {
    idx.ctl_listen = idx.nfds;
    fds[idx.nfds].fd     = g_ctl_listen;
    fds[idx.nfds].events = POLLIN;
    idx.nfds++;
  }

  if(g_ctl_client >= 0)
  {
    idx.ctl_client = idx.nfds;
    fds[idx.nfds].fd     = g_ctl_client;
    fds[idx.nfds].events = POLLIN;
    idx.nfds++;
  }

  return(idx);
}

// Read and process data from the IRC socket.
static void
handle_irc_data(struct pollfd *fds, const struct poll_idx *idx, bool raw_mode)
{
  if(idx->irc >= 0 && (fds[idx->irc].revents & POLLIN))
  {
    for(;;)
    {
      int space = (int)sizeof(g_linebuf) - g_lineoff;

      if(space <= 0)
      {
        g_lineoff = 0;
        break;
      }

      int n = irc_read(g_linebuf + g_lineoff, space);

      if(n <= 0)
      {
        if(n == 0)
          fprintf(stderr, "ircspy: server closed connection\n");
        else
          fprintf(stderr, "ircspy: read error\n");

        g_quit = 1;
        break;
      }

      g_lineoff += n;
      process_lines(raw_mode);

      if(g_ssl != NULL && SSL_pending(g_ssl) > 0)
        continue;

      break;
    }
  }

  if(idx->irc >= 0 && (fds[idx->irc].revents & (POLLERR | POLLHUP)))
  {
    fprintf(stderr, "ircspy: connection lost\n");
    g_quit = 1;
  }
}

// Close stdin and quit or detach depending on control socket state.
static void
stdin_eof(void)
{
  if(g_ctl_listen < 0)
  {
    irc_send("QUIT :leaving");
    g_quit = 1;
  }
  else
  {
    g_stdin_open = false;
  }
}

// Read and process data from stdin.
static void
handle_stdin_data(struct pollfd *fds, const struct poll_idx *idx)
{
  if(idx->stdin_fd >= 0 && (fds[idx->stdin_fd].revents & POLLIN))
  {
    char input[CMD_SZ];

    if(fgets(input, (int)sizeof(input), stdin) != NULL)
    {
      size_t len = strlen(input);

      while(len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r'))
        input[--len] = '\0';

      handle_user_input(input);
    }
    else
    {
      stdin_eof();
    }
  }

  if(idx->stdin_fd >= 0
      && (fds[idx->stdin_fd].revents & (POLLERR | POLLHUP)))
  {
    stdin_eof();
  }
}

// Accept a new control client and read commands from an existing one.
static void
handle_ctl_events(struct pollfd *fds, const struct poll_idx *idx)
{
  // Accept new client.
  if(idx->ctl_listen >= 0 && (fds[idx->ctl_listen].revents & POLLIN))
  {
    int cfd = accept(g_ctl_listen, NULL, NULL);

    if(cfd >= 0)
      g_ctl_client = cfd;
  }

  // Read command from connected client.
  if(idx->ctl_client >= 0 && (fds[idx->ctl_client].revents & POLLIN))
  {
    char input[CMD_SZ];
    ssize_t n = read(g_ctl_client, input, sizeof(input) - 1);

    if(n <= 0)
    {
      close(g_ctl_client);
      g_ctl_client = -1;
      g_ctl_deadline = 0;
    }
    else
    {
      input[n] = '\0';

      while(n > 0 && (input[n - 1] == '\n' || input[n - 1] == '\r'))
        input[--n] = '\0';

      handle_user_input(input);
      g_ctl_deadline = time(NULL) + CTL_IDLE_SEC;
    }
  }

  if(idx->ctl_client >= 0
      && (fds[idx->ctl_client].revents & (POLLERR | POLLHUP)))
  {
    close(g_ctl_client);
    g_ctl_client = -1;
    g_ctl_deadline = 0;
  }
}

static void
irc_loop(const struct irc_cfg *cfg)
{
  while(!g_quit)
  {
    struct pollfd fds[4];
    struct poll_idx idx = build_poll_set(fds);

    int timeout = (g_ctl_deadline > 0) ? 500 : POLL_TIMEOUT_MS;
    int ret = poll(fds, (nfds_t)idx.nfds, timeout);

    if(ret < 0)
    {
      if(errno == EINTR)
        continue;

      break;
    }

    if(ret == 0)
    {
      if(g_ctl_deadline > 0 && time(NULL) >= g_ctl_deadline)
        ctl_flush();
      else if(g_ctl_deadline == 0)
        irc_send("PING :ircspy");

      continue;
    }

    handle_irc_data(fds, &idx, cfg->raw_mode);
    handle_stdin_data(fds, &idx);
    handle_ctl_events(fds, &idx);

    if(g_ctl_deadline > 0 && time(NULL) >= g_ctl_deadline)
      ctl_flush();
  }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

static void
cleanup(void)
{
  if(g_ssl != NULL)
  {
    SSL_shutdown(g_ssl);
    SSL_free(g_ssl);
    g_ssl = NULL;
  }

  if(g_ctx != NULL)
  {
    SSL_CTX_free(g_ctx);
    g_ctx = NULL;
  }

  if(g_fd >= 0)
  {
    close(g_fd);
    g_fd = -1;
  }
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void
print_usage(void)
{
  fprintf(stderr,
    "Usage: ircspy [options]\n"
    "  -s <host>    Server (default: " DEFAULT_HOST ")\n"
    "  -p <port>    Port (default: %d)\n"
    "  -n <nick>    Nickname (default: " DEFAULT_NICK ")\n"
    "  -c <channel> Channel (default: " DEFAULT_CHANNEL ")\n"
    "  -C <path>    Control socket path (default: " CTL_SOCK_PATH ")\n"
    "  -T           Disable TLS\n"
    "  -V           Enable TLS certificate verification\n"
    "  -r           Raw mode (show all protocol lines)\n"
    "  -h           Show this help\n",
    DEFAULT_PORT);
}

// ---------------------------------------------------------------------------
// Main — argument parsing, connection setup, registration
// ---------------------------------------------------------------------------

// Parse command-line arguments into cfg.
// returns: 0 on success, 1 if help was printed, -1 on error.
static int
parse_args(int argc, char *argv[], struct irc_cfg *cfg)
{
  cfg->host       = DEFAULT_HOST;
  cfg->port       = DEFAULT_PORT;
  cfg->nick       = DEFAULT_NICK;
  cfg->user       = DEFAULT_USER;
  cfg->realname   = DEFAULT_REAL;
  cfg->channel    = DEFAULT_CHANNEL;
  cfg->ctl_path   = CTL_SOCK_PATH;
  cfg->use_tls    = true;
  cfg->tls_verify = false;
  cfg->raw_mode   = false;

  int opt;

  while((opt = getopt(argc, argv, "s:p:n:c:C:TVrh")) != -1)
  {
    switch(opt)
    {
      case 's': cfg->host       = optarg;                 break;
      case 'p': cfg->port       = (uint16_t)atoi(optarg); break;
      case 'n': cfg->nick       = optarg;                 break;
      case 'c': cfg->channel    = optarg;                 break;
      case 'C': cfg->ctl_path   = optarg;                 break;
      case 'T': cfg->use_tls    = false;                    break;
      case 'V': cfg->tls_verify = true;                    break;
      case 'r': cfg->raw_mode   = true;                    break;
      case 'h':
        print_usage();
        return(1);
      default:
        print_usage();
        return(-1);
    }
  }

  return(0);
}

// Establish the TCP (and optionally TLS) connection and print status.
// returns: 0 on success, -1 on failure.
static int
irc_connect(const struct irc_cfg *cfg)
{
  g_fd = tcp_connect(cfg->host, cfg->port);

  if(g_fd < 0)
    return(-1);

  if(cfg->use_tls)
  {
    if(tls_setup(g_fd, cfg->tls_verify) != 0)
    {
      cleanup();
      return(-1);
    }

    if(!cfg->raw_mode)
    {
      printf("--- Connected to %s:%u (TLS%s)\n",
             cfg->host, cfg->port,
             cfg->tls_verify ? "" : ", no verify");
      fflush(stdout);
    }
  }
  else
  {
    if(!cfg->raw_mode)
    {
      printf("--- Connected to %s:%u (plaintext)\n", cfg->host, cfg->port);
      fflush(stdout);
    }
  }

  return(0);
}

// Send NICK/USER and wait for 001 (registration complete).
// returns: 0 on success, -1 on failure or timeout.
static int
wait_for_registration(const struct irc_cfg *cfg)
{
  irc_send("NICK %s", g_nick);
  irc_send("USER %s 0 * :%s", cfg->user, cfg->realname);

  while(!g_registered && !g_quit)
  {
    struct pollfd pfd;
    pfd.fd     = g_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 30000);

    if(ret <= 0)
    {
      fprintf(stderr, "ircspy: registration timeout\n");
      return(-1);
    }

    int space = (int)sizeof(g_linebuf) - g_lineoff;
    int n     = irc_read(g_linebuf + g_lineoff, space);

    if(n <= 0)
    {
      fprintf(stderr, "ircspy: connection lost during registration\n");
      return(-1);
    }

    g_lineoff += n;
    process_lines(cfg->raw_mode);
  }

  if(g_quit)
    return(-1);

  return(0);
}

int
main(int argc, char *argv[])
{
  struct irc_cfg cfg;

  int pa = parse_args(argc, argv, &cfg);

  if(pa != 0)
    return(pa < 0 ? 1 : 0);

  // Initialize nick tracking.
  snprintf(g_nick, sizeof(g_nick), "%s", cfg.nick);
  g_channel[0] = '\0';

  // Install signal handlers.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sig_handler;
  sigaction(SIGINT,  &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  // Control socket.
  if(cfg.ctl_path != NULL && cfg.ctl_path[0] != '\0')
  {
    if(ctl_setup(cfg.ctl_path) != 0)
      fprintf(stderr, "ircspy: control socket disabled\n");
  }

  // Establish connection.
  if(irc_connect(&cfg) != 0)
  {
    ctl_cleanup();
    return(1);
  }

  // Register and wait for server acknowledgment.
  if(wait_for_registration(&cfg) != 0)
  {
    cleanup();
    ctl_cleanup();
    return(1);
  }

  // Join channel.
  if(cfg.channel != NULL && cfg.channel[0] != '\0')
    irc_send("JOIN %s", cfg.channel);

  // Main loop.
  irc_loop(&cfg);

  ctl_cleanup();
  cleanup();
  return(0);
}
