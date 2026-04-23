// botmanager — MIT
// CLI for controlling BotManager over its Unix domain socket (one-shot/attach).

#include "botmanctl.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

// Signal flag for clean subscribe mode shutdown.
static volatile sig_atomic_t ctl_interrupted = 0;

static void
ctl_sigint(int sig)
{
  (void)sig;
  ctl_interrupted = 1;
}

// Interactive line editor with up/down-arrow history.
//
// Engaged only when stdin is a TTY. The terminal is put into a lightly raw
// mode (ICANON/ECHO/ISIG off) while a line is being composed, and restored
// to its original settings before each server round-trip and on exit.

#define HIST_MAX 128

// Circular history ring. hist_head points to the slot where the next entry
// will be stored; hist_count is the number of valid entries (<= HIST_MAX).
static char hist_buf[HIST_MAX][CMD_SZ];
static int  hist_head  = 0;
static int  hist_count = 0;

// Push a line into history. Empty lines and exact duplicates of the
// most-recent entry are skipped to keep navigation useful.
static void
hist_push(const char *line)
{
  if(line[0] == '\0')
    return;

  if(hist_count > 0)
  {
    int last = (hist_head - 1 + HIST_MAX) % HIST_MAX;
    if(strcmp(hist_buf[last], line) == 0)
      return;
  }

  snprintf(hist_buf[hist_head], CMD_SZ, "%s", line);
  hist_head = (hist_head + 1) % HIST_MAX;
  if(hist_count < HIST_MAX)
    hist_count++;
}

// Fetch a history entry by age. k=0 is the most-recent entry, k=hist_count-1
// is the oldest. Returns NULL if k is out of range.
static const char *
hist_at(int k)
{
  int idx;

  if(k < 0 || k >= hist_count)
    return(NULL);

  idx = (hist_head - 1 - k + HIST_MAX) % HIST_MAX;
  return(hist_buf[idx]);
}

// Saved terminal state. Raw mode is engaged once on entry to interactive
// editing and held for the entire session; bytes the user types during a
// server round-trip simply buffer in the kernel TTY queue and stream into
// the next line_read_interactive() call byte-by-byte — which is how
// multi-line pastes get dispatched as sequential commands.
static struct termios term_saved;
static bool           term_saved_valid = false;

static void
term_restore(void)
{
  if(term_saved_valid)
    tcsetattr(STDIN_FILENO, TCSANOW, &term_saved);
}

// atexit hook: called on normal process exit to restore the TTY.
static void
term_restore_atexit(void)
{
  term_restore();
}

// Signal handler for kill signals: restore the TTY and re-raise with the
// default disposition so the process exits normally. Without this, SIGTERM
// during raw mode would leave the terminal unusable until `stty sane`.
static void
term_restore_signal(int sig)
{
  term_restore();
  signal(sig, SIG_DFL);
  raise(sig);
}

// Engage raw mode: ICANON/ECHO/ISIG off, one byte per read(). OPOST is left
// alone so \n still expands to CRLF on output. Registers the atexit and
// signal restore hooks on first call.
static bool
term_enter_raw(void)
{
  struct termios t;

  if(!term_saved_valid)
  {
    struct sigaction sa;

    if(tcgetattr(STDIN_FILENO, &term_saved) != 0)
      return(false);

    term_saved_valid = true;
    atexit(term_restore_atexit);

    // Cover the common kill paths so we don't leave the TTY wedged.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = term_restore_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
  }

  t = term_saved;
  t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
  t.c_cc[VMIN]  = 1;
  t.c_cc[VTIME] = 0;
  return(tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0);
}

// Repaint the edit line: return to column 0, print the prompt and buffer,
// erase any trailing leftover chars, then position the cursor at `cur`.
static void
line_redraw(const char *buf, size_t len, size_t cur)
{
  (void)!write(STDOUT_FILENO, "\r", 1);
  (void)!write(STDOUT_FILENO, PROMPT, strlen(PROMPT));
  if(len > 0)
    (void)!write(STDOUT_FILENO, buf, len);
  (void)!write(STDOUT_FILENO, "\033[K", 3);

  if(cur < len)
  {
    char seq[32];
    int  n = snprintf(seq, sizeof(seq), "\033[%zuD", len - cur);
    if(n > 0)
      (void)!write(STDOUT_FILENO, seq, (size_t)n);
  }
}

// Read a single byte from stdin, restarting on EINTR.
// Returns 1 on success, 0 on EOF, -1 on error.
static int
read_byte(char *ch)
{
  for(;;)
  {
    ssize_t n = read(STDIN_FILENO, ch, 1);

    if(n == 1)
      return(1);

    if(n == 0)
      return(0);

    if(errno == EINTR)
      continue;

    return(-1);
  }
}

// Interactive line reader with up/down history, left/right cursor movement,
// Line-editor state, passed to history/escape helpers by pointer so they
// can mutate buffer len/cursor without the outer loop re-plumbing args.
typedef struct {
  char   *out;
  size_t  out_sz;
  size_t  len;
  size_t  cur;
  int     hist_idx;        // -1 = scratch (user's in-progress line)
  char    scratch[CMD_SZ];
  size_t  scratch_len;
} editor_t;

static void
history_prev(editor_t *e)
{
  const char *h;
  size_t      hl;

  if(hist_count == 0 || e->hist_idx >= hist_count - 1)
    return;

  if(e->hist_idx == -1)
  {
    // Stash the in-progress line so Down can restore it.
    memcpy(e->scratch, e->out, e->len);
    e->scratch_len = e->len;
  }

  e->hist_idx++;
  h  = hist_at(e->hist_idx);
  hl = strlen(h);

  if(hl >= e->out_sz)
    hl = e->out_sz - 1;

  memcpy(e->out, h, hl);
  e->out[hl] = '\0';
  e->len = hl;
  e->cur = hl;
  line_redraw(e->out, e->len, e->cur);
}

static void
history_next(editor_t *e)
{
  if(e->hist_idx < 0)
    return;

  e->hist_idx--;

  if(e->hist_idx == -1)
  {
    memcpy(e->out, e->scratch, e->scratch_len);
    e->len = e->scratch_len;
  }

  else
  {
    const char *h  = hist_at(e->hist_idx);
    size_t      hl = strlen(h);

    if(hl >= e->out_sz)
      hl = e->out_sz - 1;

    memcpy(e->out, h, hl);
    e->len = hl;
  }

  e->out[e->len] = '\0';
  e->cur = e->len;
  line_redraw(e->out, e->len, e->cur);
}

// Consume one CSI/SS3 escape sequence and apply its editor effect.
// Returns 0 on success, -1 on EOF while reading the sequence.
static int
line_handle_escape(editor_t *e)
{
  char   intro;
  char   params[32];
  size_t plen = 0;
  char   final_byte = 0;
  char   c;

  if(read_byte(&intro) <= 0)
    return(-1);

  // Lone ESC or unrecognized intro — drop it.
  if(intro != '[' && intro != 'O')
    return(0);

  // Read until the "final byte" (0x40..0x7E).
  for(;;)
  {
    if(read_byte(&c) <= 0)
      return(-1);

    if((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E)
    {
      final_byte = c;
      break;
    }

    if(plen < sizeof(params) - 1)
      params[plen++] = c;
  }

  params[plen] = '\0';

  if(plen == 0)
  {
    switch(final_byte)
    {
      case 'A':  history_prev(e); break;
      case 'B':  history_next(e); break;

      case 'C':  // Right
        if(e->cur < e->len)
        {
          (void)!write(STDOUT_FILENO, "\033[C", 3);
          e->cur++;
        }
        break;

      case 'D':  // Left
        if(e->cur > 0)
        {
          (void)!write(STDOUT_FILENO, "\033[D", 3);
          e->cur--;
        }
        break;

      case 'H':  // Home
        if(e->cur > 0)
        {
          char s[32];
          int  sn = snprintf(s, sizeof(s), "\033[%zuD", e->cur);

          if(sn > 0)
            (void)!write(STDOUT_FILENO, s, (size_t)sn);

          e->cur = 0;
        }
        break;

      case 'F':  // End
        if(e->cur < e->len)
        {
          char s[32];
          int  sn = snprintf(s, sizeof(s), "\033[%zuC", e->len - e->cur);

          if(sn > 0)
            (void)!write(STDOUT_FILENO, s, (size_t)sn);

          e->cur = e->len;
        }
        break;

      default:
        break;
    }
  }

  // Parameterized: ESC [ <params> <final>. DEL (ESC[3~) removes under cursor.
  else
  {
    if(final_byte == '~' && strcmp(params, "3") == 0 && e->cur < e->len)
    {
      memmove(e->out + e->cur, e->out + e->cur + 1, e->len - e->cur - 1);
      e->len--;
      e->out[e->len] = '\0';
      line_redraw(e->out, e->len, e->cur);
    }
    // Everything else (bracketed-paste 200~/201~, etc.) is dropped.
  }

  return(0);
}

// backspace, Home/End, Ctrl-U (clear), Ctrl-C (cancel line), Ctrl-D (EOF on
// empty line). Writes the completed line (NUL-terminated, newline stripped)
// to `out` and returns its length, 0 for an empty line, or -1 on EOF.
static int
line_read_interactive(char *out, size_t out_sz)
{
  editor_t e;

  e.out = out;
  e.out_sz = out_sz;
  e.len = 0;
  e.cur = 0;
  e.hist_idx = -1;
  e.scratch_len = 0;

  out[0] = '\0';

  fflush(stdout);
  (void)!write(STDOUT_FILENO, PROMPT, strlen(PROMPT));

  for(;;)
  {
    char ch;
    int  rc = read_byte(&ch);

    if(rc <= 0)
      return(-1);

    if(ch == '\r' || ch == '\n')
    {
      (void)!write(STDOUT_FILENO, "\n", 1);
      out[e.len] = '\0';
      return((int)e.len);
    }

    // Ctrl-C: abandon current line, redraw empty prompt.
    if(ch == 3)
    {
      (void)!write(STDOUT_FILENO, "^C\n", 3);
      e.len = 0;
      e.cur = 0;
      e.hist_idx = -1;
      out[0] = '\0';
      (void)!write(STDOUT_FILENO, PROMPT, strlen(PROMPT));
      continue;
    }

    // Ctrl-D: EOF on empty line, otherwise ignore.
    if(ch == 4)
    {
      if(e.len == 0)
      {
        (void)!write(STDOUT_FILENO, "\n", 1);
        return(-1);
      }
      continue;
    }

    // Ctrl-U: kill whole line.
    if(ch == 21)
    {
      e.len = 0;
      e.cur = 0;
      out[0] = '\0';
      line_redraw(out, e.len, e.cur);
      continue;
    }

    // Backspace (DEL or BS).
    if(ch == 127 || ch == 8)
    {
      if(e.cur > 0)
      {
        memmove(out + e.cur - 1, out + e.cur, e.len - e.cur);
        e.cur--;
        e.len--;
        out[e.len] = '\0';
        line_redraw(out, e.len, e.cur);
      }
      continue;
    }

    // Escape sequences (CSI/SS3): arrows, Home/End, DEL, bracketed-paste.
    if(ch == 0x1B)
    {
      if(line_handle_escape(&e) < 0)
        return(-1);
      continue;
    }

    // Printable byte (includes UTF-8 continuation bytes; cursor is byte-based).
    if((unsigned char)ch >= 0x20 && e.len + 1 < out_sz)
    {
      bool at_end = (e.cur == e.len);

      if(!at_end)
        memmove(out + e.cur + 1, out + e.cur, e.len - e.cur);

      out[e.cur] = ch;
      e.cur++;
      e.len++;
      out[e.len] = '\0';

      if(at_end)
        (void)!write(STDOUT_FILENO, &ch, 1);
      else
        line_redraw(out, e.len, e.cur);
    }
  }
}

static void
default_sock_path(char *dst, size_t sz)
{
  const char *home = getenv("HOME");

  if(home != NULL)
    snprintf(dst, sz, "%s/.config/botmanager/%s", home, DEFAULT_SOCK_NAME);
  else
    snprintf(dst, sz, "/tmp/%s", DEFAULT_SOCK_NAME);
}

static int
ctl_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;

  if(fd < 0)
  {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    return(-1);
  }

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

static int
ctl_send_recv(int fd, const char *cmd)
{
  // Send command with newline.
  size_t len = strlen(cmd);
  char sendbuf[CMD_SZ + 2];
  ssize_t w;
  char buf[BUF_SZ];

  if(len >= CMD_SZ)
  {
    fprintf(stderr, "command too long\n");
    return(-1);
  }

  memcpy(sendbuf, cmd, len);
  sendbuf[len] = '\n';

  w = write(fd, sendbuf, len + 1);

  if(w < 0)
  {
    fprintf(stderr, "write: %s\n", strerror(errno));
    return(-1);
  }

  // Read response until null byte delimiter.
  for(;;)
  {
    ssize_t n = read(fd, buf, sizeof(buf));

    if(n < 0)
    {
      fprintf(stderr, "read: %s\n", strerror(errno));
      return(-1);
    }

    if(n == 0)
      // Server closed connection.
      return(-1);

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

static int
ctl_subscribe(int fd, int sev, const char *regex)
{
  char cmd[CMD_SZ];
  ssize_t w;
  struct sigaction sa;
  char buf[BUF_SZ];

  if(regex != NULL)
    snprintf(cmd, sizeof(cmd), "SUBSCRIBE %d %s\n", sev, regex);
  else
    snprintf(cmd, sizeof(cmd), "SUBSCRIBE %d\n", sev);

  w = write(fd, cmd, strlen(cmd));

  if(w < 0)
  {
    fprintf(stderr, "write: %s\n", strerror(errno));
    return(1);
  }

  // Install SIGINT handler for clean shutdown.
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ctl_sigint;
  sigaction(SIGINT, &sa, NULL);

  // Read and print until EOF or signal.
  for(;;)
  {
    ssize_t n;

    if(ctl_interrupted)
      break;

    n = read(fd, buf, sizeof(buf));

    if(n <= 0)
      break;

    fwrite(buf, 1, n, stdout);
    fflush(stdout);
  }

  return(0);
}

static void
print_usage(const char *prog)
{
  printf("Usage: %s [options] [command [args...]]\n", prog);
  printf("  -s <path>   Socket path (default: ~/.config/botmanager/%s)\n",
      DEFAULT_SOCK_NAME);
  printf("  -S <sev>    Subscribe to CLAM messages (severity 0-7)\n");
  printf("  -r <regex>  Filter against \"<context> <msg>\" (requires -S)\n");
  printf("  -A <bot>    Attach to a bot: stream its observe-trace\n");
  printf("              (sugar for -S 3 -r 'bot=<bot>\\b')\n");
  printf("  -u <user>   Dispatch commands asserting this username\n");
  printf("              (default: @owner; used to test userns perms)\n");
  printf("  -h          Show this help\n");
  printf("\n");
  printf("Modes:\n");
  printf("  One-shot:    %s status\n", prog);
  printf("  Attach:      %s (interactive, no args)\n", prog);
  printf("  Batch:       %s < commands.txt (piped, session preserved)\n", prog);
  printf("  Subscribe:   %s -S 2\n", prog);
}

// Options parsed from argv. `as_user` / `subscribe_regex` alias into argv
// where possible; `attach_regex` is the in-place buffer for -A synthesis.
typedef struct {
  char        sock_path[256];
  int         subscribe_sev;
  const char *subscribe_regex;
  const char *as_user;
  char        attach_regex[128];
  int         first_cmd_arg;      // optind after parse
} botmanctl_opts_t;

static int
main_parse_args(int argc, char *argv[], botmanctl_opts_t *out)
{
  int opt;

  default_sock_path(out->sock_path, sizeof(out->sock_path));
  out->subscribe_sev    = -1;
  out->subscribe_regex  = NULL;
  out->as_user          = NULL;
  out->attach_regex[0]  = '\0';

  while((opt = getopt(argc, argv, "s:S:r:u:A:h")) != -1)
  {
    switch(opt)
    {
      case 's':
        snprintf(out->sock_path, sizeof(out->sock_path), "%s", optarg);
        break;

      case 'S':
        out->subscribe_sev = atoi(optarg);

        if(out->subscribe_sev < 0 || out->subscribe_sev > 7)
        {
          fprintf(stderr, "severity must be 0-7\n");
          return(1);
        }

        break;

      case 'r':
        out->subscribe_regex = optarg;
        break;

      case 'u':
        out->as_user = optarg;
        break;

      case 'A':
        // Bot names are alphanumeric + underscore (validated server-side), so
        // no regex metachars to escape. Anchor on bot= + word boundary so
        // "pacman" does not match "pacmanpundit".
        snprintf(out->attach_regex, sizeof(out->attach_regex),
            "bot=%s\\b", optarg);
        out->subscribe_sev   = 3;  // CLAM_DEBUG — llm's observe-trace level
        out->subscribe_regex = out->attach_regex;
        break;

      case 'h':
        print_usage(argv[0]);
        return(-1);    // success, but stop

      default:
        print_usage(argv[0]);
        return(1);
    }
  }

  out->first_cmd_arg = optind;
  return(0);
}

// Assert identity up-front so subsequent commands run as `as_user`.
// Drains the ack line so it doesn't mix into interactive output.
static bool
main_apply_as_user(int fd, const char *as_user)
{
  char as_line[256];
  char ack[256];
  int n;
  ssize_t rd;

  n = snprintf(as_line, sizeof(as_line), "AS %s\n", as_user);

  if(n > 0 && write(fd, as_line, (size_t)n) != n)
  {
    fprintf(stderr, "failed to send AS identity\n");
    return(false);
  }

  rd = read(fd, ack, sizeof(ack) - 1);

  if(rd > 0) ack[rd] = '\0';   // discard

  return(true);
}

static int
main_one_shot(int fd, int optind_start, int argc, char *argv[])
{
  char cmd[CMD_SZ];
  size_t off = 0;
  int rc;

  for(int i = optind_start; i < argc; i++)
  {
    size_t arglen;

    if(i > optind_start && off < sizeof(cmd) - 1)
      cmd[off++] = ' ';

    arglen = strlen(argv[i]);

    if(off + arglen >= sizeof(cmd) - 1)
    {
      fprintf(stderr, "command too long\n");
      return(1);
    }

    memcpy(cmd + off, argv[i], arglen);
    off += arglen;
  }

  cmd[off] = '\0';
  rc = ctl_send_recv(fd, cmd);

  return(rc < 0 ? 1 : 0);
}

// Read one input line into `line`. Returns its length (>=0), 0 on empty
// skip, or -1 on EOF. Mode selection: line_edit → raw editor; else fgets.
static int
main_read_line(char *line, size_t line_sz, bool interactive, bool line_edit)
{
  int llen;
  size_t len;

  if(line_edit)
  {
    llen = line_read_interactive(line, line_sz);

    if(llen < 0)
      return(-1);

    return(llen);
  }

  if(interactive)
  {
    fputs(PROMPT, stdout);
    fflush(stdout);
  }

  if(fgets(line, (int)line_sz, stdin) == NULL)
    return(-1);

  len = strlen(line);

  while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    line[--len] = '\0';

  if(len == 0)
    return(0);

  // Skip comment lines in batch mode.
  if(!interactive && line[0] == '#')
    return(0);

  return((int)len);
}

static int
main_interactive_loop(int fd)
{
  char line[CMD_SZ];
  // interactive: stdin is a TTY. line_edit: stdin AND stdout are TTYs
  // (engage raw-mode editor). Batch redirects take the fgets path.
  bool interactive = isatty(STDIN_FILENO);
  bool line_edit   = interactive && isatty(STDOUT_FILENO) && term_enter_raw();

  for(;;)
  {
    int llen = main_read_line(line, sizeof(line), interactive, line_edit);

    if(llen < 0)
      break;

    if(llen == 0)
      continue;

    if(line_edit)
      hist_push(line);

    if(ctl_send_recv(fd, line) < 0)
    {
      fprintf(stderr, "connection lost\n");
      return(1);
    }

    // Exit after quit — daemon is shutting down.
    if(strcasecmp(line, "quit") == 0 || strcasecmp(line, "/quit") == 0)
      break;
  }

  return(0);
}

int
main(int argc, char *argv[])
{
  botmanctl_opts_t opts;
  int fd;
  int rc;

  rc = main_parse_args(argc, argv, &opts);

  if(rc < 0) return(0);         // -h
  if(rc > 0) return(rc);        // parse error

  fd = ctl_connect(opts.sock_path);

  if(fd < 0)
    return(1);

  // AS identity applies only to command clients, not subscribe clients.
  if(opts.as_user != NULL && opts.subscribe_sev < 0)
  {
    if(!main_apply_as_user(fd, opts.as_user))
    {
      close(fd);
      return(1);
    }
  }

  if(opts.subscribe_sev >= 0)
  {
    rc = ctl_subscribe(fd, opts.subscribe_sev, opts.subscribe_regex);
    close(fd);
    return(rc);
  }

  // Remaining args after options → one-shot command.
  if(opts.first_cmd_arg < argc)
  {
    rc = main_one_shot(fd, opts.first_cmd_arg, argc, argv);
    close(fd);
    return(rc);
  }

  rc = main_interactive_loop(fd);
  close(fd);
  return(rc);
}
