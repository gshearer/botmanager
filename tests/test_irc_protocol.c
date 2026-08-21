// botmanager — MIT
// Cases for the IRC wire boundary: what irc_send_raw hands the socket.
// On this protocol the line ending IS the message boundary, so a CR or
// an LF that survives the format ends the command early and the server
// runs the rest as a further one — from a bot that holds OPER, and with
// nothing on the path reporting it. These rows pin the fold, and pin
// the bytes that must come through it: colour, bold, reset and CTCP are
// put in on purpose by color_translate and irc_send_emote.
#include "test.h"

#define IRC_INTERNAL
#include "irc.h"

#include <string.h>

// The socket the driver believes it has. irc_send_raw only passes the
// pointer to sock_send, which is ours, so the address is all it needs.
static int                   fake_session;
static sock_session_t *const FAKE_SESSION = (sock_session_t *)&fake_session;

#define CAP_MAX 8

static char   cap[CAP_MAX][IRC_LINE_SZ + 1];
static size_t cap_n;

// The four externals irc_protocol.c reaches for. Stubbing them is what
// lets this suite compile that translation unit on its own and read the
// bytes at the point they would leave the process.
void
clam(uint8_t sev, const char *context, const char *fmt, ...)
{
  (void)sev;
  (void)context;
  (void)fmt;
}

sock_session_t *
irc_session_ref(irc_state_t *st)
{
  (void)st;
  return(FAKE_SESSION);
}

void
sock_release(sock_session_t *session)
{
  (void)session;
}

bool
sock_send(sock_session_t *session, const void *buf, size_t len)
{
  (void)session;

  if(cap_n < CAP_MAX && len < sizeof(cap[0]))
  {
    memcpy(cap[cap_n], buf, len);
    cap[cap_n][len] = '\0';
    cap_n++;
  }

  return(SUCCESS);
}

// Every captured line ends in exactly one CRLF and carries no other
// break. Stated as a property rather than as expected text because the
// split path emits a line count the case cannot name in advance.
static bool
cap_framed(void)
{
  if(cap_n == 0)
    return(false);

  for(size_t i = 0; i < cap_n; i++)
  {
    size_t len = strlen(cap[i]);

    if(len < 2 || cap[i][len - 2] != '\r' || cap[i][len - 1] != '\n')
      return(false);

    if(strcspn(cap[i], "\r\n") != len - 2)
      return(false);
  }

  return(true);
}

// A body wide enough to force irc_send_privmsg past its 510-byte limit,
// with a forged QUIT sitting in the middle of the second chunk.
#define A64  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define LONG A64 A64 A64 A64 A64 A64 A64 A64 "\r\nQUIT :bye" A64

// One %s argument, formatted into a PRIVMSG envelope — the shape every
// reply the daemon makes arrives in.
static const struct
{
  const char *name;
  const char *text;
  const char *want;
} raw_cases[] = {
  { "ordinary line is untouched",
    "hello there",
    "PRIVMSG #botman :hello there\r\n" },

  { "CRLF in the body cannot forge a command",
    "look\r\nKILL nick :bye",
    "PRIVMSG #botman :look  KILL nick :bye\r\n" },

  { "a lone LF is folded",
    "look\nKILL nick :bye",
    "PRIVMSG #botman :look KILL nick :bye\r\n" },

  { "a lone CR is folded",
    "look\rKILL nick :bye",
    "PRIVMSG #botman :look KILL nick :bye\r\n" },

  { "a leading break cannot empty the verb",
    "\r\nJOIN #secret",
    "PRIVMSG #botman :  JOIN #secret\r\n" },

  { "colour and bold survive the fold",
    "\002bold\00304red\017plain",
    "PRIVMSG #botman :\002bold\00304red\017plain\r\n" },

  { "the CTCP markers survive the fold",
    "\001ACTION waves\001",
    "PRIVMSG #botman :\001ACTION waves\001\r\n" },
};

// The split path builds its own buffer per chunk, so it reaches the
// boundary once per line rather than once per call.
static const struct
{
  const char *name;
  const char *text;
  size_t      want_lines;
} privmsg_cases[] = {
  { "a short body is one line",              "hi",  1 },
  { "a break inside a split body is folded",  LONG, 2 },
};

int
main(void)
{
  static irc_state_t st;

  st.connected = true;

  for(size_t i = 0; i < sizeof(raw_cases) / sizeof(raw_cases[0]); i++)
  {
    cap_n = 0;
    irc_send_raw(&st, "PRIVMSG #botman :%s", raw_cases[i].text);
    test_check_str("send_raw", raw_cases[i].name,
        raw_cases[i].want, cap[0]);
  }

  for(size_t i = 0; i < sizeof(privmsg_cases) / sizeof(privmsg_cases[0]); i++)
  {
    cap_n = 0;
    irc_send_privmsg(&st, "#botman", privmsg_cases[i].text);
    test_check_sz("send_privmsg", privmsg_cases[i].name,
        privmsg_cases[i].want_lines, cap_n);
    test_check_bool("send_privmsg framing", privmsg_cases[i].name,
        true, cap_framed());
  }

  return(test_report("irc_protocol"));
}
