// botmanager — MIT
// Cases for sig_reason_sanitize: the operator's parting words cross into
// the process here, and the drivers downstream put them on the wire.
#include "test.h"
#include "sig.h"

#include <string.h>

// Wider than any case needs, so truncation only happens where a row
// asks for it by passing a smaller size.
#define DST_SZ  64

// A byte no case legitimately produces. Pre-loaded before each call, so
// a row that writes nothing is distinguishable from one that writes "".
#define UNTOUCHED  "!"

static const struct
{
  const char *name;
  const char *src;
  size_t      dst_sz;
  const char *want;
} reason_cases[] = {
  { "ordinary words pass through", "back in five", DST_SZ, "back in five" },
  { "null src is the empty reason", NULL, DST_SZ, "" },
  { "empty src stays empty", "", DST_SZ, "" },

  // The whole point. On IRC the line ending IS the message boundary, so
  // a CR or LF here would close the QUIT and let everything after it
  // arrive as a command the bot never meant to send — with owner
  // credentials, since /quit is owner-only.
  { "newline cannot end the line", "bye\nJOIN #secret", DST_SZ,
    "bye JOIN #secret" },
  { "carriage return cannot either", "bye\r\nQUIT", DST_SZ, "bye  QUIT" },
  { "a lone CR is folded too", "a\rb", DST_SZ, "a b" },
  { "tab is a control byte", "a\tb", DST_SZ, "a b" },
  { "NUL ends the reason, it does not fold", "a\0b", DST_SZ, "a" },
  { "DEL is folded", "a\x7f" "b", DST_SZ, "a b" },
  { "every low byte folds", "\x01\x02\x1f", DST_SZ, "   " },

  // Folding is by byte value, and a UTF-8 continuation byte is >= 0x80.
  // Read through a signed char those are negative and a naive "< 0x20"
  // test eats them, turning the operator's accents into spaces.
  { "utf-8 survives", "caf\xc3\xa9 \xf0\x9f\x91\x8b", DST_SZ,
    "caf\xc3\xa9 \xf0\x9f\x91\x8b" },

  // Truncation is lossy but never unterminated, and never half a
  // decision: the fold happens before the cut, so a control byte that
  // survives into dst is already a space.
  { "truncates and terminates", "abcdef", 4, "abc" },
  { "truncates a folded byte", "ab\ncdef", 4, "ab " },
  { "one byte of room is the empty reason", "abc", 1, "" },
  { "zero-size dst writes nothing", "abc", 0, UNTOUCHED },
};

int
main(void)
{
  char dst[DST_SZ];

  for(size_t i = 0; i < sizeof(reason_cases) / sizeof(reason_cases[0]); i++)
  {
    strlcpy(dst, UNTOUCHED, sizeof(dst));
    sig_reason_sanitize(dst, reason_cases[i].dst_sz, reason_cases[i].src);

    test_check_str("reason_sanitize", reason_cases[i].name,
        reason_cases[i].want, dst);

    // The promise every driver leans on when it skips a check of its
    // own: whatever came in, what comes out is one line.
    test_check_bool("reason_sanitize", "single line",
        true, strpbrk(dst, "\r\n") == NULL);
  }

  return(test_report("shutdown_reason"));
}
