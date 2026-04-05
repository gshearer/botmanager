// ircspyctl — CLI client for controlling a running ircspy session.
//
// Usage:
//   ircspyctl [options] [message]
//
// One-shot mode (arguments provided):
//   ircspyctl "!weather 90210"
//   ircspyctl "!forecast -h 10001"
//
// Interactive mode (no arguments):
//   ircspyctl
//   irc> !weather 90210
//   irc> /quit
//
// Connects to ircspy's control socket, sends messages, and prints
// the collected IRC output.  Each command waits for a NUL-delimited
// response (ircspy flushes after a few seconds of channel silence).

#include "ircspyctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Connect to the ircspy control socket.
// path: Unix socket path
// returns: fd on success, -1 on failure
static int
ctl_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if(fd < 0)
  {
    fprintf(stderr, "ircspyctl: socket: %s\n", strerror(errno));
    return(-1);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    fprintf(stderr, "ircspyctl: connect(%s): %s\n", path, strerror(errno));
    close(fd);
    return(-1);
  }

  return(fd);
}

// Send a message and print the collected response.
// fd: connected socket fd
// msg: message to send (without trailing newline)
// returns: 0 on success, -1 on error/disconnect
static int
ctl_send_recv(int fd, const char *msg)
{
  size_t len = strlen(msg);
  char sendbuf[CMD_SZ + 2];

  if(len >= CMD_SZ)
  {
    fprintf(stderr, "ircspyctl: message too long\n");
    return(-1);
  }

  memcpy(sendbuf, msg, len);
  sendbuf[len] = '\n';

  ssize_t w = write(fd, sendbuf, len + 1);

  if(w < 0)
  {
    fprintf(stderr, "ircspyctl: write: %s\n", strerror(errno));
    return(-1);
  }

  // Read response until NUL byte delimiter.
  char buf[BUF_SZ];

  for(;;)
  {
    ssize_t n = read(fd, buf, sizeof(buf));

    if(n < 0)
    {
      fprintf(stderr, "ircspyctl: read: %s\n", strerror(errno));
      return(-1);
    }

    if(n == 0)
      return(-1);

    // Scan for NUL byte delimiter.
    for(ssize_t i = 0; i < n; i++)
    {
      if(buf[i] == '\0')
      {
        if(i > 0)
          fwrite(buf, 1, (size_t)i, stdout);

        fflush(stdout);
        return(0);
      }
    }

    // No NUL yet — print what we have and keep reading.
    fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
  }
}

// print_usage: display command-line usage and examples
// prog: program name (argv[0])
static void
print_usage(const char *prog)
{
  fprintf(stderr,
      "Usage: %s [options] [message]\n"
      "  -s <path>   Socket path (default: %s)\n"
      "  -h          Show this help\n"
      "\n"
      "One-shot:     %s \"!weather 90210\"\n"
      "Interactive:  %s\n",
      prog, DEFAULT_SOCK_PATH, prog, prog);
}

// main: connect to ircspy control socket and send messages
// argc: argument count
// argv: argument vector (optional socket path and message)
// returns: 0 on success, 1 on failure
int
main(int argc, char *argv[])
{
  const char *sock_path = DEFAULT_SOCK_PATH;
  int opt;

  while((opt = getopt(argc, argv, "s:h")) != -1)
  {
    switch(opt)
    {
      case 's':
        sock_path = optarg;
        break;

      case 'h':
        print_usage(argv[0]);
        return(0);

      default:
        print_usage(argv[0]);
        return(1);
    }
  }

  int fd = ctl_connect(sock_path);

  if(fd < 0)
    return(1);

  // One-shot mode: remaining args joined into a single message.
  if(optind < argc)
  {
    char cmd[CMD_SZ];
    size_t off = 0;

    for(int i = optind; i < argc; i++)
    {
      if(i > optind && off < sizeof(cmd) - 1)
        cmd[off++] = ' ';

      size_t arglen = strlen(argv[i]);

      if(off + arglen >= sizeof(cmd) - 1)
      {
        fprintf(stderr, "ircspyctl: message too long\n");
        close(fd);
        return(1);
      }

      memcpy(cmd + off, argv[i], arglen);
      off += arglen;
    }

    cmd[off] = '\0';

    int rc = ctl_send_recv(fd, cmd);

    close(fd);
    return(rc < 0 ? 1 : 0);
  }

  // Interactive mode.
  char line[CMD_SZ];

  for(;;)
  {
    fputs(PROMPT, stdout);
    fflush(stdout);

    if(fgets(line, (int)sizeof(line), stdin) == NULL)
      break;

    size_t len = strlen(line);

    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if(len == 0)
      continue;

    if(ctl_send_recv(fd, line) < 0)
    {
      fprintf(stderr, "ircspyctl: connection lost\n");
      break;
    }
  }

  close(fd);
  return(0);
}
