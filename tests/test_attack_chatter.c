// botmanager — MIT
// Cases for the duelling pit's chatter ring: the narration a combatant
// is handed at prompt time so it can banter about the fight it is
// standing in (board #90).
//
// Three things here are worth a table rather than a reading, and every
// one of them fails silently — into a model's prompt, where a wrong
// answer reads as the persona being odd rather than as a defect.
//
// The COLOUR, because the ring is fed the line the room got, markers and
// all, and a marker that survives comes back out of a model as the code
// letter welded onto a nickname ("Pclaude_test", measured live on the
// voiced-turn path). Stripping is why the ring copies at all rather
// than pointing at the rendered line.
//
// The SLICING, because the buffer is shared with the standing sentence
// and with every other situation provider. A line that will not fit
// whole must take the OLDER lines with it, never itself be cut: half a
// blow is a sentence the pit never said, and the newest line is the one
// the question is about.
//
// The SCOPE, because the ring is global across every room and every
// namespace — one fixed table, no per-round lifecycle — so a busy pit
// next door must cost this round its oldest lines and can never leak a
// line of its own into this round's answer.

#include "test.h"

#define ATTACK_INTERNAL
#include "attack.h"

#include "colors.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SUITE "attack_chatter"

// Round ids nothing else in the table uses. Real ids are BIGSERIAL and
// are never 0, which is what lets 0 mark an unwritten slot.
#define ROUND_A 4001
#define ROUND_B 4002

static void
fill(int64_t round_id, uint32_t n, const char *stem)
{
  char line[64];

  for(uint32_t i = 0; i < n; i++)
  {
    snprintf(line, sizeof(line), "%s%" PRIu32, stem, i);
    atk_chatter_note(round_id, line);
  }
}

// Lines in an answer: separators plus one, and zero for the empty
// answer. The cap is stated in lines, so the test counts them.
static size_t
lines(const char *s)
{
  size_t n = 1;

  if(s[0] == '\0')
    return(0);

  for(; *s != '\0'; s++)
    if(*s == '\n')
      n++;

  return(n);
}

int
main(void)
{
  char out[2048];
  char small[64];

  // ---- an empty ring says nothing, and says it as zero ------------- //
  test_check_sz("quiet", "unknown round",
      0, atk_chatter_recent(ROUND_A, out, sizeof(out)));
  test_check_str("quiet", "buffer cleared", "", out);

  // Round 0 is the ring's empty marker, so it can never be written to a
  // slot and can never match one.
  atk_chatter_note(0, "a line belonging to no round");
  test_check_sz("quiet", "round 0 never stored",
      0, atk_chatter_recent(0, out, sizeof(out)));

  // ---- colour markers are stripped, never translated --------------- //
  atk_chatter_note(ROUND_A, CLR_PURPLE "pops" CLR_RESET " swings");
  atk_chatter_recent(ROUND_A, out, sizeof(out));
  test_check_str("colour", "markers gone", "[pit] pops swings", out);

  // ---- oldest first, newest last ----------------------------------- //
  atk_chatter_note(ROUND_A, "and misses");
  atk_chatter_recent(ROUND_A, out, sizeof(out));
  test_check_str("order", "room order kept",
      "[pit] pops swings\n[pit] and misses", out);

  // ---- another room cannot leak into this one ---------------------- //
  atk_chatter_note(ROUND_B, "a brawl next door");
  atk_chatter_recent(ROUND_A, out, sizeof(out));
  test_check_str("scope", "round B absent",
      "[pit] pops swings\n[pit] and misses", out);

  atk_chatter_recent(ROUND_B, out, sizeof(out));
  test_check_str("scope", "round B alone", "[pit] a brawl next door", out);

  // ---- the show cap holds, and it keeps the NEWEST ------------------ //
  fill(ROUND_A, ATK_CHATTER_SHOW + 4, "line ");
  atk_chatter_recent(ROUND_A, out, sizeof(out));
  test_check_sz("cap", "at most ATK_CHATTER_SHOW lines",
      ATK_CHATTER_SHOW, lines(out));
  test_check_bool("cap", "newest line survives",
      true, strstr(out, "line 11") != NULL);
  test_check_bool("cap", "oldest line evicted",
      false, strstr(out, "line 0\n") != NULL);

  // ---- a short buffer sheds OLD lines and never splits one --------- //
  atk_chatter_recent(ROUND_A, small, sizeof(small));
  test_check_bool("slice", "stays inside the buffer",
      true, strnlen(small, sizeof(small)) < sizeof(small));
  test_check_bool("slice", "keeps the newest whole",
      true, strstr(small, "line 11") != NULL);
  test_check_bool("slice", "sheds the older lines",
      true, lines(small) < ATK_CHATTER_SHOW);

  // A buffer too small for even one line writes nothing rather than a
  // fragment: the provider's separator logic reads that zero.
  test_check_sz("slice", "no room at all",
      0, atk_chatter_recent(ROUND_A, small, 4));
  test_check_str("slice", "cleared when nothing fits", "", small);

  // ---- the ring itself wraps, oldest out first --------------------- //
  fill(ROUND_B, ATK_CHATTER_MAX + 2, "b ");
  atk_chatter_recent(ROUND_A, out, sizeof(out));
  test_check_sz("wrap", "round A pushed out entirely", 0, lines(out));

  atk_chatter_recent(ROUND_B, out, sizeof(out));
  test_check_sz("wrap", "round B still capped at show",
      ATK_CHATTER_SHOW, lines(out));

  return(test_report(SUITE));
}
