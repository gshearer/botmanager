// botmanager — MIT
// Cases for irc_parse_line: the tree's one parser whose output an
// authority decision is taken on. A shortened nick, user or host is a
// peer-controlled rewrite of the string userns_mfa_match() reads, so
// the fields it fills and the lines it refuses are both pinned here.
#include "test.h"

#define IRC_INTERNAL
#include "irc.h"

#include <string.h>

// Long enough to overflow any single field, built from a visible unit.
#define N40   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define N300  N40 N40 N40 N40 N40 N40 N40 "aaaaaaaaaaaaaaaaaaaa"
#define N660  N300 N300 N40 "aaaaaaaaaaaaaaaaaaaa"

// A 255-byte host — the longest IRC_HOST_SZ admits, and long enough
// that the whole prefix runs past the 256 bytes this parser clamped at.
#define H62   "b234567890123456789012345678901234567890123456789012345678901."
#define H255  H62 H62 H62 "cdefghijcdefghijcdefghijcdefghijcdefghijcdefghijcdefghijcdefghijcdefg"

static const struct
{
  const char *name;
  const char *line;
  bool        want_rc;      // SUCCESS or FAIL
  const char *want_nick;
  const char *want_user;
  const char *want_host;
  const char *want_command;
  const char *want_params;
  const char *want_trailing;
  bool        want_trail_flag;
} line_cases[] = {
  // The ordinary shapes, pinned so the refusals below cannot be bought
  // by breaking the common path.
  { "channel message", ":nick!user@host.example PRIVMSG #chan :hello there",
    SUCCESS, "nick", "user", "host.example", "PRIVMSG", "#chan",
    "hello there", true },
  { "private message", ":nick!user@host PRIVMSG botname :hi",
    SUCCESS, "nick", "user", "host", "PRIVMSG", "botname", "hi", true },
  { "no prefix", "PING :server1", SUCCESS, "", "", "", "PING", "",
    "server1", true },
  { "command only", "PING", SUCCESS, "", "", "", "PING", "", "", false },
  { "numeric from the server",
    ":irc.example.net 001 bot :Welcome to the network",
    SUCCESS, "irc.example.net", "", "", "001", "bot",
    "Welcome to the network", true },
  { "params without trailing", ":nick!user@host JOIN #chan",
    SUCCESS, "nick", "user", "host", "JOIN", "#chan", "", false },
  { "trailing with no params", ":nick!user@host QUIT :gone",
    SUCCESS, "nick", "user", "host", "QUIT", "", "gone", true },
  { "extra spaces after the prefix", ":nick!user@host  PRIVMSG #c :x",
    SUCCESS, "nick", "user", "host", "PRIVMSG", "#c", "x", true },
  { "colon inside the trailing text",
    ":nick!user@host PRIVMSG #c :see: this", SUCCESS, "nick", "user",
    "host", "PRIVMSG", "#c", "see: this", true },
  { "empty trailing", ":nick!user@host PRIVMSG #c :", SUCCESS, "nick",
    "user", "host", "PRIVMSG", "#c", "", true },
  // A server name may run longer than a nick field. It resolves no
  // identity, so it is shortened rather than costing the line, and the
  // full text stays in .prefix — checked separately below.
  { "server name past the nick field",
    ":" N40 " NOTICE * :hi", SUCCESS,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "", "", "NOTICE", "*", "hi", true },
  // The whole point: a legal long host no longer arrives shortened.
  { "255-byte host survives whole",
    ":nick!user@" H255 " PRIVMSG #c :x", SUCCESS, "nick", "user", H255,
    "PRIVMSG", "#c", "x", true },
  // Refusals. Each field a user prefix carries is checked on its own.
  { "nick past its field", ":" N40 "!user@host PRIVMSG #c :x", FAIL },
  { "user past its field", ":nick!" N40 "@host PRIVMSG #c :x", FAIL },
  // 300 bytes of host: past IRC_HOST_SZ, but the prefix holding it
  // still fits, so this row reaches the host check and not the one
  // below it.
  { "host past its field", ":nick!user@" N300 " PRIVMSG #c :x", FAIL },
  { "prefix past its buffer",
    ":nick!user@" N660 " PRIVMSG #c :x", FAIL },
  { "command past its field", ":nick!user@host " N40 " #c :x", FAIL },
  { "prefix with nothing after it", ":nick!user@host", FAIL },
};

int
main(void)
{
  irc_parsed_msg_t p;

  for(size_t i = 0; i < sizeof(line_cases) / sizeof(line_cases[0]); i++)
  {
    bool rc = irc_parse_line(line_cases[i].line, &p);

    test_check_bool("parse_line", line_cases[i].name,
        line_cases[i].want_rc, rc);

    // A refusal promises nothing about the fields, so the accepted rows
    // are the only ones with expectations to check.
    if(rc != SUCCESS)
      continue;

    test_check_str("nick", line_cases[i].name, line_cases[i].want_nick,
        p.nick);
    test_check_str("user", line_cases[i].name, line_cases[i].want_user,
        p.user);
    test_check_str("host", line_cases[i].name, line_cases[i].want_host,
        p.host);
    test_check_str("command", line_cases[i].name,
        line_cases[i].want_command, p.command);
    test_check_str("params", line_cases[i].name, line_cases[i].want_params,
        p.params);
    test_check_str("trailing", line_cases[i].name,
        line_cases[i].want_trailing, p.trailing);
    test_check_bool("has_trailing", line_cases[i].name,
        line_cases[i].want_trail_flag, p.has_trailing);
  }

  // The shortened server name above keeps its full text in .prefix,
  // which is what every consumer of a server-origin line reads.
  irc_parse_line(":" N40 " NOTICE * :hi", &p);
  test_check_str("prefix", "server name kept whole", N40, p.prefix);

  return(test_report("irc_parse"));
}
