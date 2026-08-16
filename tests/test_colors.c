// botmanager — MIT
// Cases for color_markup_translate: an LLM's typeable markup becomes
// abstract markers, and generated text can forge none of them.
#include "test.h"
#include "colors.h"

#include <string.h>

// Large enough that no case truncates unless it says so.
#define DST_SZ  64

// A byte no case legitimately produces. Pre-loaded before each call, it
// proves that a refusal wrote nothing rather than writing "".
#define UNTOUCHED  "!"

static const struct
{
  const char *name;
  const char *src;
  size_t      dst_sz;
  size_t      want_len;
  const char *want;
} markup_cases[] = {
  { "plain text is untouched", "1 < 2 && 3", DST_SZ, 10, "1 < 2 && 3" },
  { "null src", NULL, DST_SZ, 0, "" },
  { "bold pair", "**bold**", DST_SZ, 8, CLR_BOLD "bold" CLR_BOLD },
  { "lone bold marker", "**", DST_SZ, 2, CLR_BOLD },
  { "single asterisks are not bold", "*x*", DST_SZ, 3, "*x*" },
  { "colour tag pair", "<red>hi</red>", DST_SZ, 6, CLR_RED "hi" CLR_RESET },
  { "case-insensitive, and grey aliases gray", "<GREY>x</Grey>", DST_SZ, 5,
    CLR_GRAY "x" CLR_RESET },
  { "nested markup", "<red>**hi**</red>", DST_SZ, 10,
    CLR_RED CLR_BOLD "hi" CLR_BOLD CLR_RESET },
  { "unknown tag survives", "<bogus>x", DST_SZ, 8, "<bogus>x" },
  { "a path is not a closing tag", "</dev/null", DST_SZ, 10, "</dev/null" },
  // Forgery: a raw \x01 downstream is a marker, and the byte behind it
  // is the identifier. Text that could type one would pick its own
  // colours and eat its next character, so the byte is dropped.
  { "forged marker dropped", "a\x01Rb", DST_SZ, 3, "aRb" },
  { "lone marker byte dropped", "\x01", DST_SZ, 0, "" },
  { "marker before real markup", "\x01<red>x", DST_SZ, 3, CLR_RED "x" },
  // Truncation: a marker is two bytes and is never split.
  { "cut in text", "hello", 3, 2, "he" },
  { "cut before a marker", "<red>hello", 3, 2, CLR_RED },
  // A marker is emitted whole or not at all — and not at all stops the
  // walk, so the text behind it does not slide forward into its place.
  { "no room for a marker", "<red>x", 2, 0, "" },
  { "no room at all", "<red>x", 1, 0, "" },
  { "zero-size dst", "<red>x", 0, 0, UNTOUCHED },
};

int
main(void)
{
  char dst[DST_SZ];

  for(size_t i = 0; i < sizeof(markup_cases) / sizeof(markup_cases[0]); i++)
  {
    size_t n;

    strlcpy(dst, UNTOUCHED, sizeof(dst));
    n = color_markup_translate(dst, markup_cases[i].dst_sz,
        markup_cases[i].src);

    test_check_sz("markup_translate", markup_cases[i].name,
        markup_cases[i].want_len, n);
    test_check_str("markup_translate", markup_cases[i].name,
        markup_cases[i].want, dst);

    // The header's standing promise, and what lets every caller size
    // dst at strlen(src) + 1: every token is at least two bytes in and
    // becomes exactly two out, so output never grows.
    if(markup_cases[i].src != NULL)
      test_check_bool("markup_translate", "never grows", true,
          n <= strlen(markup_cases[i].src));
  }

  return(test_report("colors"));
}
