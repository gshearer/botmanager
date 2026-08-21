// botmanager — MIT
// Cases for botmanctl's reply routing (include/botmanctl.h).
//
// The control socket serves many sessions and the answer to a command
// is not always ready when the dispatch returns: every async verb
// deep-copies its message and replies from a pool worker, minutes or
// milliseconds later. Whoever the driver was serving at that instant
// used to receive it. That is silent across a session boundary — an
// operator sees another operator's output in their own terminal, on a
// path that runs with the credential context open — and the same
// pointer read is a SEGV when the window closes underneath the write
// (OBS-29-TRUTH T1/T4).
//
// No daemon and no database: the suite brings up the same subsystems
// the socket needs, registers three test verbs, and drives the real
// driver over real AF_UNIX clients. `slowecho` is the async fixture —
// it copies the message the way core/resolve.c does and answers from a
// worker; `hold` occupies a dispatch window synchronously, so a late
// reply lands while the driver is demonstrably serving someone else.
//
// The last row is the other half of the same delivery: not who the
// line reaches, but where it ends. One reply is one line — and the
// text is a stored value, a fetched title, a model's line, none of
// them framing — so it must not be able to end that line itself.
// `forge` is that fixture.
//
// ⚠ The teardown order here — botmanctl_exit() BEFORE pool_exit() —
// is deliberate and is itself the regression test for OBS-31: it is
// the order main.c does NOT use, so it is the only place in the tree
// that exercises botmanctl_exit()'s own join. Do not "tidy" it.

#include "test.h"

#include "alloc.h"
#include "bot.h"
#include "botmanctl.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "method.h"
#include "pool.h"
#include "task.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SUITE "botmanctl_route"

#define ROUTE_STREAM_SZ  8192   // one client's whole accumulated stream
#define ROUTE_ECHO_MS    200    // how long slowecho takes to answer
#define ROUTE_HOLD_MS    600    // how long hold keeps a dispatch window open

// What `forge` replies, and the single line it has to arrive as. The
// breaks stand in for the ones a real reply carries without choosing
// to: json_unescape decodes them, so a fetched title or a stored value
// reaches cmd_reply holding them.
#define FORGE_TEXT    "FORGE1\nFORGE2\rFORGE3"
#define FORGE_FOLDED  "FORGE1 FORGE2 FORGE3"

// What slowecho carries across the thread boundary: the message by
// value (core/resolve.c:1007-1009's shape — a copy holds no lifetime)
// plus the token to answer with.
typedef struct
{
  method_msg_t msg;
  cmd_ctx_t    ctx;
  char         token[128];
  uint32_t     delay_ms;
} echo_req_t;

static char route_sock[64];

static void
route_sleep_ms(uint32_t ms)
{
  struct timespec ts = {
    .tv_sec  = ms / 1000,
    .tv_nsec = (long)(ms % 1000) * 1000000L,
  };

  nanosleep(&ts, NULL);
}

// The task body: answers long after the dispatch that asked returned.
static void
echo_task(task_t *t)
{
  echo_req_t *r = t->data;

  route_sleep_ms(r->delay_ms);
  cmd_reply(&r->ctx, r->token);
  mem_free(r);

  t->state = TASK_ENDED;
}

static void
echo_cmd(const cmd_ctx_t *ctx)
{
  echo_req_t *r = mem_alloc("test", "echo_req", sizeof(echo_req_t));

  memset(r, 0, sizeof(*r));
  memcpy(&r->msg, ctx->msg, sizeof(r->msg));
  r->ctx.bot      = ctx->bot;
  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->delay_ms     = ROUTE_ECHO_MS;
  strlcpy(r->token, ctx->parsed->argv[0], sizeof(r->token));

  task_add("echo", TASK_THREAD, 0, echo_task, r);
}

// Occupies the dispatch window — and with it the driver's idea of who
// is being served — for as long as it is asked to.
static void
hold_cmd(const cmd_ctx_t *ctx)
{
  route_sleep_ms(ROUTE_HOLD_MS);
  cmd_reply(ctx, "held");
}

// One cmd_reply carrying the bytes that used to draw their own lines.
static void
forge_cmd(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, FORGE_TEXT);
}

static const cmd_arg_desc_t echo_args[] = {
  { .name = "token", .type = CMD_ARG_ALNUM }
};

// Clients

static int
route_connect(void)
{
  struct sockaddr_un addr;
  int                fd;

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0)
    return(-1);

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strlcpy(addr.sun_path, route_sock, sizeof(addr.sun_path));

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    close(fd);
    return(-1);
  }

  return(fd);
}

static bool
route_ask(int fd, const char *line)
{
  char    buf[256];
  size_t  n;
  ssize_t wrote;

  n     = (size_t)snprintf(buf, sizeof(buf), "%s\n", line);
  wrote = write(fd, buf, n);

  return(wrote == (ssize_t)n ? SUCCESS : FAIL);
}

// Drain whatever arrives within budget_ms into stream, NUL-terminated.
// The server's end-of-response delimiter is itself a NUL byte, so it is
// translated to \x01 on the way in: every row here reads the whole
// stream as text, and one of them reads across the delimiter on
// purpose (T9 — a late reply arrives after it, by definition).
#define ROUTE_DELIM  '\x01'

static size_t
route_drain(int fd, char *stream, size_t stream_sz, uint32_t budget_ms)
{
  struct timespec  start;
  size_t           len = 0;

  clock_gettime(CLOCK_MONOTONIC, &start);
  stream[0] = '\0';

  for(;;)
  {
    struct timespec now;
    struct pollfd   pfd = { .fd = fd, .events = POLLIN };
    char            buf[1024];
    ssize_t         got;
    long            spent;

    clock_gettime(CLOCK_MONOTONIC, &now);
    spent = (now.tv_sec - start.tv_sec) * 1000
        + (now.tv_nsec - start.tv_nsec) / 1000000;

    if(spent >= (long)budget_ms)
      break;

    if(poll(&pfd, 1, (int)((long)budget_ms - spent)) <= 0)
      break;

    got = read(fd, buf, sizeof(buf));
    if(got <= 0)
      break;

    for(ssize_t i = 0; i < got && len + 1 < stream_sz; i++)
      stream[len++] = (buf[i] == '\0') ? ROUTE_DELIM : buf[i];

    stream[len] = '\0';
  }

  return(len);
}

// True when every non-empty line in stream is exactly `token`, and at
// least one such line arrived. Delimiters are line breaks here too.
static bool
route_lines_all_are(const char *stream, const char *token)
{
  const char *p    = stream;
  size_t      want = strlen(token);
  bool        seen = false;

  while(*p != '\0')
  {
    size_t n = strcspn(p, "\n\x01");

    if(n > 0)
    {
      if(n != want || strncmp(p, token, want) != 0)
        return(false);

      seen = true;
    }

    p += n;

    if(*p != '\0')
      p++;
  }

  return(seen);
}

// Rows

// The no-regression row: a synchronous verb still answers inside its
// own dispatch, before the end-of-response delimiter.
static void
row_sync_before_delim(void)
{
  char   stream[ROUTE_STREAM_SZ];
  int    a = route_connect();
  size_t delim;
  bool   ok = false;

  if(a < 0)
    return;

  route_ask(a, "version");
  route_drain(a, stream, sizeof(stream), 500);

  delim = strcspn(stream, "\x01");
  ok    = (stream[delim] == ROUTE_DELIM)
      && (strstr(stream, "BotManager") != NULL)
      && ((size_t)(strstr(stream, "BotManager") - stream) < delim);

  test_check_bool(SUITE, "sync_before_delim", true, ok);
  close(a);
}

// The reply exists long after the dispatch returned; it must still find
// the session that asked (T3 — every async reply over the control
// socket was dropped outright).
static void
row_async_reaches_asker(void)
{
  char stream[ROUTE_STREAM_SZ];
  int  a = route_connect();

  if(a < 0)
    return;

  route_ask(a, "slowecho A1");
  route_drain(a, stream, sizeof(stream), ROUTE_ECHO_MS + 800);

  test_check_bool(SUITE, "async_reaches_asker", true,
      strstr(stream, "A1") != NULL);
  close(a);
}

// The one that matters: B asked for nothing but a hold, and must never
// be told about A1. B's dispatch window is deliberately open across the
// whole of A's wait, which is exactly the state that used to make the
// driver hand A's answer to B (T4).
static void
row_async_never_crossfeeds(void)
{
  char stream_a[ROUTE_STREAM_SZ];
  char stream_b[ROUTE_STREAM_SZ];
  int  a = route_connect();
  int  b = route_connect();

  if(a < 0 || b < 0)
    return;

  route_ask(a, "slowecho A1");
  route_sleep_ms(50);
  route_ask(b, "hold");

  route_drain(b, stream_b, sizeof(stream_b), ROUTE_HOLD_MS + 800);
  route_drain(a, stream_a, sizeof(stream_a), 200);

  test_check_bool(SUITE, "async_never_crossfeeds", false,
      strstr(stream_b, "A1") != NULL);

  close(a);
  close(b);
}

// A asks and leaves before the answer exists. The answer has nowhere to
// go, and "nowhere" must not mean "whoever is being served now" — nor a
// pointer to the client record the poll thread has already freed.
static void
row_gone_client_drops(void)
{
  char stream_b[ROUTE_STREAM_SZ];
  int  a = route_connect();
  int  b = route_connect();

  if(a < 0 || b < 0)
    return;

  route_ask(a, "slowecho A2");
  route_sleep_ms(20);
  close(a);

  route_sleep_ms(30);
  route_ask(b, "hold");
  route_drain(b, stream_b, sizeof(stream_b), ROUTE_HOLD_MS + 800);

  test_check_bool(SUITE, "gone_client_drops", false,
      strstr(stream_b, "A2") != NULL);

  close(b);
}

// Two answers land together on two sessions. Each session's stream must
// consist of its own token and nothing else — which states the routing
// and the one-write-per-line rule at once: the newline used to be a
// second write() and two threads' lines interleaved inside one line
// (T5).
static void
row_line_is_one_write(void)
{
  char stream_a[ROUTE_STREAM_SZ];
  char stream_b[ROUTE_STREAM_SZ];
  int  a = route_connect();
  int  b = route_connect();

  if(a < 0 || b < 0)
    return;

  route_ask(a, "slowecho AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  route_ask(b, "slowecho BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");

  route_drain(a, stream_a, sizeof(stream_a), ROUTE_ECHO_MS + 800);
  route_drain(b, stream_b, sizeof(stream_b), 200);

  test_check_bool(SUITE, "line_is_one_write_a", true,
      route_lines_all_are(stream_a,
          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  test_check_bool(SUITE, "line_is_one_write_b", true,
      route_lines_all_are(stream_b,
          "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"));

  close(a);
  close(b);
}

// A reply owns its text and never its framing. What cmd_reply is
// handed is whatever the command had — a stored value, a fetched
// title, a model's line — and a CR or an LF inside one used to reach
// the socket verbatim: further lines of operator output, drawn by the
// reply itself, or a cursor return that overwrites the line already
// printed. One reply is one line, whatever it contains.
static void
row_reply_cannot_forge_lines(void)
{
  char stream[ROUTE_STREAM_SZ];
  int  a = route_connect();

  if(a < 0)
    return;

  route_ask(a, "forge");
  route_drain(a, stream, sizeof(stream), 500);

  test_check_bool(SUITE, "reply_cannot_forge_lines", true,
      route_lines_all_are(stream, FORGE_FOLDED));

  close(a);
}

int
main(void)
{
  int probe;

  // sun_path is 107 bytes and a meson build directory is not, so the
  // socket lives beside the process id and nowhere near the tree.
  snprintf(route_sock, sizeof(route_sock), "/tmp/bm_route_%d.sock",
      (int)getpid());

  clam_init();
  mem_init();
  task_init();
  pool_init();
  method_init();
  bot_init();
  cmd_init();
  kv_init();

  botmanctl_register_config();
  kv_set("core.botmanctl.sockpath", route_sock);

  cmd_register("test", "slowecho", "slowecho <token>",
      "Answer <token> from a pool worker after a delay", NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, echo_cmd, NULL,
      NULL, NULL, echo_args, 1, NULL, NULL);

  cmd_register("test", "hold", "hold",
      "Occupy the dispatch window, then answer", NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, hold_cmd, NULL,
      NULL, NULL, NULL, 0, NULL, NULL);

  cmd_register("test", "forge", "forge",
      "Reply with a token that carries CR and LF", NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, forge_cmd, NULL,
      NULL, NULL, NULL, 0, NULL, NULL);

  botmanctl_register_method();

  probe = route_connect();
  if(probe < 0)
  {
    botmanctl_exit();
    pool_exit();
    task_exit();
    return(test_skip(SUITE, "cannot bind or reach the control socket"));
  }

  close(probe);

  row_sync_before_delim();
  row_async_reaches_asker();
  row_async_never_crossfeeds();
  row_gone_client_drops();
  row_line_is_one_write();
  row_reply_cannot_forge_lines();

  botmanctl_exit();
  pool_exit();
  task_exit();

  return(test_report(SUITE));
}
