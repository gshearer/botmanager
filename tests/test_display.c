// botmanager — MIT
// Cases for display.h: the column arithmetic six renderers share.
//
// Two things make this silent rather than loud. The width count walks
// text that came from outside the process — a nickname out of a VARCHAR,
// a provider's title — so a mis-stepped UTF-8 length reads past the end
// of it (a prior audit found exactly that in one of the copies this
// module replaced); and a miscounted column is a table that merely looks
// wrong, which no error path will ever report. The arithmetic is pure
// and needs no daemon, so it is cheap to state exactly.
#include "test.h"
#include "colors.h"
#include "display.h"

#include <string.h>

#define CELL_SZ  32

// A byte no case legitimately produces, so an untouched buffer is
// distinguishable from one written with "".
#define UNTOUCHED  "!"

static const struct
{
  const char *name;
  const char *in;
  size_t      want;
} vis_cases[] = {
  { "ascii counts itself",           "abc",                    3 },
  { "empty",                         "",                       0 },
  { "a marker is two bytes of none", CLR_GREEN "up",           2 },
  { "markers only",                  CLR_RED CLR_RESET,        0 },
  // An unpaired \x01 is the last byte: there is no code letter to skip,
  // so it counts as the one byte it is rather than eating the NUL.
  { "unpaired marker at the end",    "x\x01",                  2 },
  { "two-byte glyph is one column",  "é",                      1 },
  { "three-byte glyph is one",       "▲",                      1 },
  { "four-byte glyph is one",        "😀",                     1 },
  { "mixed",                         CLR_GREEN "▲1.3%" CLR_RESET, 5 },
  // Continuation bytes with no lead byte — a name byte-sliced by a
  // VARCHAR. They belong to a glyph that is not here, and counting them
  // is what the crypto copy did (OBS-8).
  { "orphan continuation bytes",     "\x80\x80",               0 },
};

static const struct
{
  const char *name;
  const char *in;
  size_t      cap;
  int         width;
  const char *want;
} right_cases[] = {
  { "pads to width",             "7",      CELL_SZ, 4, "   7" },
  { "already at width",          "1234",   CELL_SZ, 4, "1234" },
  { "wider than the column",     "123456", CELL_SZ, 4, "123456" },
  { "zero width",                "x",      CELL_SZ, 0, "x" },
  { "a glyph pads by columns",   "▲",      CELL_SZ, 3, "  ▲" },
  { "markers do not consume it", CLR_RED "9" CLR_RESET, CELL_SZ, 3,
    "  " CLR_RED "9" CLR_RESET },
  // The whole padded cell has to fit or nothing moves: a half-shifted
  // cell would be worse than an unpadded one.
  { "no room for the padding",   "abc",    5,       8, "abc" },
};

static const struct
{
  const char *name;
  const char *in;
  size_t      cap;
  int         width;
  const char *want;
} left_cases[] = {
  { "pads to width",           "ab",   CELL_SZ, 4, "ab  " },
  { "already at width",        "abcd", CELL_SZ, 4, "abcd" },
  { "wider than the column",   "abcde", CELL_SZ, 4, "abcde" },
  { "a glyph pads by columns", "▲",    CELL_SZ, 3, "▲  " },
  // Unlike the right-align, a short buffer pads as far as it can: the
  // cell is already misaligned and stopping short of the terminator
  // would be the only way to make it wrong as well.
  { "padding bounded by cap",  "ab",   5,       8, "ab  " },
};

static const struct
{
  const char *name;
  const char *in;
  size_t      cap;
  int         cols;
  const char *mark;
  const char *want;
} fit_cases[] = {
  { "shorter than the budget", "abc",   CELL_SZ, 8, NULL, "abc" },
  { "exactly the budget",      "abcd",  CELL_SZ, 4, NULL, "abcd" },
  { "hard stop at the budget", "abcdef", CELL_SZ, 4, NULL, "abcd" },
  { "zero columns",            "abc",   CELL_SZ, 0, NULL, "" },
  { "a glyph is never split",  "a▲b",   CELL_SZ, 2, NULL, "a▲" },
  { "four-byte glyph intact",  "😀x",   CELL_SZ, 1, NULL, "😀" },
  { "cap stops it early",      "abcdef", 4,      9, NULL, "abc" },
  // The mark is written only when content was left behind, and its own
  // bytes are reserved before the copy so it can never be the thing
  // that does not fit.
  { "mark on truncation",      "abcdef", CELL_SZ, 3, "…", "abc…" },
  { "no mark when it all fit", "abc",    CELL_SZ, 3, "…", "abc" },
  { "mark reserves its bytes", "abcdef", 8,       9, "…", "abcd…" },
  // The mark's bytes are reserved before the first copy, so a buffer
  // too small to hold the mark at all yields an empty cell rather than
  // a cell with no mark. Every copy this replaced did the same.
  { "no room for the mark",    "abcdef", 3,       9, "…", "" },
};

// A UTF-8 lead byte whose sequence a VARCHAR cut short: the length the
// lead byte claims runs past the terminator. The bytes that ARE there
// are copied — the fragment reaches the client as one replacement
// glyph, which is what all six copies did — but the claim itself is
// never read, and that is the property the out-of-bounds read a prior
// audit found in one of them violated.
static void
fit_truncated_sequence(void)
{
  char dst[CELL_SZ];

  strlcpy(dst, UNTOUCHED, sizeof(dst));
  display_fit("\xe2\x96", 4, dst, sizeof(dst), NULL);
  test_check_str("fit", "a sequence cut short copies what is there",
      "\xe2\x96", dst);

  strlcpy(dst, UNTOUCHED, sizeof(dst));
  display_fit("ab\xf0\x9f\x90", 4, dst, sizeof(dst), NULL);
  test_check_str("fit", "and never reads past the terminator",
      "ab\xf0\x9f\x90", dst);
}

static void
cat_cases(void)
{
  char line[8];

  line[0] = '\0';
  display_cat(line, sizeof(line), "ab");
  display_cat(line, sizeof(line), "cd");
  test_check_str("cat", "cells append in order", "abcd", line);

  display_cat(line, sizeof(line), "efghij");
  test_check_str("cat", "stops at capacity", "abcdefg", line);

  display_cat(line, sizeof(line), "k");
  test_check_str("cat", "a full line takes nothing more", "abcdefg", line);

  line[0] = '\0';
  display_cat(line, sizeof(line), "");
  test_check_str("cat", "an empty cell is a no-op", "", line);
}

int
main(void)
{
  char buf[CELL_SZ];

  for(size_t i = 0; i < sizeof(vis_cases) / sizeof(vis_cases[0]); i++)
    test_check_sz("vis_len", vis_cases[i].name, vis_cases[i].want,
        display_vis_len(vis_cases[i].in));

  for(size_t i = 0; i < sizeof(right_cases) / sizeof(right_cases[0]); i++)
  {
    strlcpy(buf, right_cases[i].in, sizeof(buf));
    display_align_right(buf, right_cases[i].cap, right_cases[i].width);
    test_check_str("align_right", right_cases[i].name,
        right_cases[i].want, buf);
  }

  for(size_t i = 0; i < sizeof(left_cases) / sizeof(left_cases[0]); i++)
  {
    strlcpy(buf, left_cases[i].in, sizeof(buf));
    display_align_left(buf, left_cases[i].cap, left_cases[i].width);
    test_check_str("align_left", left_cases[i].name, left_cases[i].want, buf);
  }

  for(size_t i = 0; i < sizeof(fit_cases) / sizeof(fit_cases[0]); i++)
  {
    strlcpy(buf, UNTOUCHED, sizeof(buf));
    display_fit(fit_cases[i].in, fit_cases[i].cols, buf, fit_cases[i].cap,
        fit_cases[i].mark);
    test_check_str("fit", fit_cases[i].name, fit_cases[i].want, buf);
  }

  fit_truncated_sequence();
  cat_cases();

  return(test_report("display"));
}
