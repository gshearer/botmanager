// botmanctl — CLI utility for controlling BotManager via Unix domain socket.
//
// Usage:
//   botmanctl [options] [command [args...]]
//
// One-shot mode (arguments provided):
//   botmanctl status
//   botmanctl bot list
//   botmanctl "set core.pool.max_threads 8"
//
// Interactive mode (no arguments):
//   botmanctl
//   > status
//   > bot list
//   > quit

#include "botmanctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Build default socket path (~/.config/botmanager/botman.sock).
// dst: destination buffer
// sz: buffer size
static void
default_sock_path(char *dst, size_t sz)
{
  const char *home = getenv("HOME");

  if(home != NULL)
    snprintf(dst, sz, "%s/.config/botmanager/%s", home, DEFAULT_SOCK_NAME);
  else
    snprintf(dst, sz, "/tmp/%s", DEFAULT_SOCK_NAME);
}

// Connect to the Unix domain socket.
// path: socket file path
// returns: fd on success, -1 on failure
static int
ctl_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if(fd < 0)
  {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    return(-1);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    fprintf(stderr, "connect(%s): %s\n", path, strerror(errno));
    close(fd);
    return(-1);
  }

  return(fd);
}

// Send a command and print the response.
// fd: connected socket fd
// cmd: command string (without trailing newline)
// returns: 0 on success, -1 on error/disconnect
static int
ctl_send_recv(int fd, const char *cmd)
{
  // Send command with newline.
  size_t len = strlen(cmd);
  char sendbuf[CMD_SZ + 2];

  if(len >= CMD_SZ)
  {
    fprintf(stderr, "command too long\n");
    return(-1);
  }

  memcpy(sendbuf, cmd, len);
  sendbuf[len] = '\n';

  ssize_t w = write(fd, sendbuf, len + 1);

  if(w < 0)
  {
    fprintf(stderr, "write: %s\n", strerror(errno));
    return(-1);
  }

  // Read response until null byte delimiter.
  char buf[BUF_SZ];

  for(;;)
  {
    ssize_t n = read(fd, buf, sizeof(buf));

    if(n < 0)
    {
      fprintf(stderr, "read: %s\n", strerror(errno));
      return(-1);
    }

    if(n == 0)
    {
      // Server closed connection.
      return(-1);
    }

    // Scan for null byte delimiter.
    for(ssize_t i = 0; i < n; i++)
    {
      if(buf[i] == '\0')
      {
        // Print everything before the null byte.
        if(i > 0)
          fwrite(buf, 1, i, stdout);

        return(0);
      }
    }

    // No null byte yet — print what we have and keep reading.
    fwrite(buf, 1, n, stdout);
  }
}

// print_usage: display command-line usage and examples
// prog: program name (argv[0])
static void
print_usage(const char *prog)
{
  printf("Usage: %s [options] [command [args...]]\n", prog);
  printf("  -s <path>   Socket path (default: ~/.config/botmanager/%s)\n",
      DEFAULT_SOCK_NAME);
  printf("  -h          Show this help\n");
  printf("\n");
  printf("One-shot mode:   %s status\n", prog);
  printf("Interactive mode: %s\n", prog);
}

// main: connect to botman socket and dispatch commands
// argc: argument count
// argv: argument vector (optional socket path and command)
// returns: 0 on success, 1 on failure
int
main(int argc, char *argv[])
{
  char sock_path[256];
  int  opt;

  default_sock_path(sock_path, sizeof(sock_path));

  while((opt = getopt(argc, argv, "s:h")) != -1)
  {
    switch(opt)
    {
      case 's':
        snprintf(sock_path, sizeof(sock_path), "%s", optarg);
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

  // Remaining args after options = one-shot command.
  if(optind < argc)
  {
    // Join remaining arguments into a single command string.
    char cmd[CMD_SZ];
    size_t off = 0;

    for(int i = optind; i < argc; i++)
    {
      if(i > optind && off < sizeof(cmd) - 1)
        cmd[off++] = ' ';

      size_t arglen = strlen(argv[i]);

      if(off + arglen >= sizeof(cmd) - 1)
      {
        fprintf(stderr, "command too long\n");
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

    if(fgets(line, sizeof(line), stdin) == NULL)
      break;

    // Strip trailing newline.
    size_t len = strlen(line);

    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if(len == 0)
      continue;

    if(ctl_send_recv(fd, line) < 0)
    {
      fprintf(stderr, "connection lost\n");
      break;
    }
  }

  close(fd);
  return(0);
}
