// botmanager — MIT
// CLI client for controlling a running ircspy session over its control socket.

#include "ircspyctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int
ctl_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;

  if(fd < 0)
  {
    fprintf(stderr, "ircspyctl: socket: %s\n", strerror(errno));
    return(-1);
  }

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

static int
ctl_send_recv(int fd, const char *msg)
{
  size_t len = strlen(msg);
  char sendbuf[CMD_SZ + 2];
  ssize_t w;
  char buf[BUF_SZ];

  if(len >= CMD_SZ)
  {
    fprintf(stderr, "ircspyctl: message too long\n");
    return(-1);
  }

  memcpy(sendbuf, msg, len);
  sendbuf[len] = '\n';

  w = write(fd, sendbuf, len + 1);

  if(w < 0)
  {
    fprintf(stderr, "ircspyctl: write: %s\n", strerror(errno));
    return(-1);
  }

  // Read response until NUL byte delimiter.
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

int
main(int argc, char *argv[])
{
  const char *sock_path = DEFAULT_SOCK_PATH;
  int opt;
  int fd;
  char line[CMD_SZ];

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

  fd = ctl_connect(sock_path);

  if(fd < 0)
    return(1);

  // One-shot mode: remaining args joined into a single message.
  if(optind < argc)
  {
    char cmd[CMD_SZ];
    size_t off = 0;
    int rc;

    for(int i = optind; i < argc; i++)
    {
      size_t arglen;

      if(i > optind && off < sizeof(cmd) - 1)
        cmd[off++] = ' ';

      arglen = strlen(argv[i]);

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

    rc = ctl_send_recv(fd, cmd);

    close(fd);
    return(rc < 0 ? 1 : 0);
  }

  // Interactive mode.
  for(;;)
  {
    size_t len;

    fputs(PROMPT, stdout);
    fflush(stdout);

    if(fgets(line, (int)sizeof(line), stdin) == NULL)
      break;

    len = strlen(line);

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
