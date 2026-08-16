// botmanager — MIT
// Cases for json_unescape: the only byte-level decoder in the tree that
// reads outside bytes (an LLM provider's stream) and can be wrong in
// silence — a mis-decoded escape reaches IRC and the fact store as text
// nobody flagged.
#include "test.h"
#include "json.h"

#include <string.h>

// Long enough for every case here; json_unescape needs len + 1.
#define OUT_SZ  256

static const struct
{
  const char *name;
  const char *in;
  const char *want;
} unescape_cases[] = {
  { "no escapes", "plain text", "plain text" },
  { "empty", "", "" },
  { "the two-character escapes",
    "a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti",
    "a\"b\\c/d\be\ff\ng\rh\ti" },
  { "unknown escape drops the backslash", "\\q", "q" },
  { "trailing backslash survives as itself", "ends\\", "ends\\" },
  // BMP code points, one per UTF-8 width.
  { "ascii codepoint", "\\u0041", "A" },
  { "two-byte codepoint", "\\u00e9", "é" },
  { "two-byte codepoint, uppercase hex", "\\u00E9", "é" },
  { "three-byte codepoint", "\\u20ac", "€" },
  { "codepoint among text", "a\\u00e9b", "aéb" },
  // A surrogate pair is one character. Decoding the halves separately
  // is CESU-8 — six bytes of mojibake where four bytes of emoji belong.
  { "surrogate pair", "\\ud83d\\ude00", "\U0001f600" },
  { "surrogate pair at the top of the range", "\\udbff\\udfff",
    "\U0010ffff" },
  { "surrogate pair among text", "hi \\ud83d\\ude00!", "hi \U0001f600!" },
  { "two pairs back to back", "\\ud83d\\ude00\\ud83d\\ude01",
    "\U0001f600\U0001f601" },
  // Everything a C string cannot carry becomes U+FFFD rather than
  // truncating the string or leaving invalid UTF-8 behind.
  { "nul escape cannot truncate the string", "a\\u0000b", "a�b" },
  { "lone high surrogate", "\\ud83d", "�" },
  { "lone low surrogate", "\\ude00", "�" },
  { "high surrogate followed by a plain codepoint", "\\ud83d\\u0041",
    "�A" },
  { "high surrogate followed by text", "\\ud83dxy", "�xy" },
  // A malformed escape is not decoded at all: no code point is invented
  // for it, and in particular no NUL byte from a non-hex digit.
  { "non-hex digit", "\\uZZZZ", "\\uZZZZ" },
  { "one bad digit", "\\u00g1", "\\u00g1" },
  { "cut short", "\\u00", "\\u00" },
  { "bare \\u", "\\u", "\\u" },
  { "malformed escape leaves the next one alone", "\\uZZZZ\\u0041",
    "\\uZZZZA" },
};

int
main(void)
{
  char out[OUT_SZ];

  for(size_t i = 0;
      i < sizeof(unescape_cases) / sizeof(unescape_cases[0]); i++)
  {
    size_t n;

    memset(out, 0, sizeof(out));
    n = json_unescape(unescape_cases[i].in, strlen(unescape_cases[i].in),
        out);

    test_check_str("unescape", unescape_cases[i].name,
        unescape_cases[i].want, out);

    // The return is the promise every caller sizes a buffer against.
    test_check_sz("unescape length", unescape_cases[i].name,
        strlen(unescape_cases[i].want), n);
  }

  return(test_report("json"));
}
