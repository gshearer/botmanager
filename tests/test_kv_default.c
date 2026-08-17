// botmanager — MIT
// Cases for kv_get_uint_or_default: the knob whose 0 means "the default".
//
// 28 read sites across 9 plugins spelled this by hand —
// `v = kv_get_uint(k); if(v == 0) v = 10;` — where the 10 was already
// in the key's own declaration (OBS-13). Every pair agreed, so nothing was
// broken; what was broken was that the declaration had stopped being the
// declaration. Editing a schema default moved nothing, because the
// literal downstream of it won.
//
// That failure is silent in the worst way: the schema is what `show kv`
// prints, what the help text quotes, and what an operator reads before
// tuning. A drifted pair answers with a number that appears nowhere the
// operator looked.
//
// case_the_declaration_is_what_moves is the row that states it. The rest
// bound the substitution so it cannot start answering for a knob where 0
// is a value.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"

#define SUITE "kv_default"

// The escape hatch itself: an operator who types 0 is asking for the
// shipped value back, and does not have to know what it is.
static void
case_zero_reads_the_declaration(void)
{
  kv_register("t.knob", KV_UINT32, "10", NULL, NULL, "");

  test_check_bool(SUITE, "an explicit 0 takes",
      SUCCESS, kv_set_uint("t.knob", 0));

  test_check_sz(SUITE, "the plain read answers with the 0 that is stored",
      0, (size_t)kv_get_uint("t.knob"));

  test_check_sz(SUITE, "and this read answers with the declaration",
      10, (size_t)kv_get_uint_or_default("t.knob"));
}

// The substitution must reach only the 0. A tuned value is the answer
// however close to 0 it is.
static void
case_a_set_value_wins(void)
{
  test_check_bool(SUITE, "a tuned value takes",
      SUCCESS, kv_set_uint("t.knob", 7));

  test_check_sz(SUITE, "and is what both reads answer",
      7, (size_t)kv_get_uint_or_default("t.knob"));

  test_check_bool(SUITE, "1 is not 0", SUCCESS, kv_set_uint("t.knob", 1));

  test_check_sz(SUITE, "so 1 survives the substitution",
      1, (size_t)kv_get_uint_or_default("t.knob"));
}

// A registered key already answers with its default (include/kv.h), so
// on the untouched key the two reads are the same read. This is the
// state almost every knob in the tree is in almost all of the time.
static void
case_an_untouched_key_is_already_its_default(void)
{
  kv_register("t.untouched", KV_UINT32, "45", NULL, NULL, "");

  test_check_sz(SUITE, "the plain read answers with the default",
      45, (size_t)kv_get_uint("t.untouched"));

  test_check_sz(SUITE, "and so does this one",
      45, (size_t)kv_get_uint_or_default("t.untouched"));
}

// ⭑ The OBS-13 row. Two knobs, two declarations, both set to 0. The
// hand-written idiom is stood in for by the literal it used — one
// number, serving both keys the way a copied read site serves the key it
// was copied onto. It answers 10 for the knob declared 3, which is the
// whole finding: the read site tracks the literal, not the declaration.
static void
case_the_declaration_is_what_moves(void)
{
  const size_t read_site_literal = 10;   // what the old idiom wrote

  kv_register("t.decl.ten",   KV_UINT32, "10", NULL, NULL, "");
  kv_register("t.decl.three", KV_UINT32, "3",  NULL, NULL, "");

  kv_set_uint("t.decl.ten",   0);
  kv_set_uint("t.decl.three", 0);

  test_check_sz(SUITE, "the old idiom answers for the key it agrees with",
      10, (size_t)kv_get_uint("t.decl.ten") == 0
          ? read_site_literal : (size_t)kv_get_uint("t.decl.ten"));

  test_check_sz(SUITE, "and answers 10 for the key declared 3",
      10, (size_t)kv_get_uint("t.decl.three") == 0
          ? read_site_literal : (size_t)kv_get_uint("t.decl.three"));

  test_check_sz(SUITE, "this read answers 10 for the key declared 10",
      10, (size_t)kv_get_uint_or_default("t.decl.ten"));

  test_check_sz(SUITE, "and 3 for the key declared 3",
      3, (size_t)kv_get_uint_or_default("t.decl.three"));
}

// The bound in the other direction: where 0 IS the declaration, there is
// nothing to substitute and the escape hatch is not one. Several keys
// beside the converted ones are declared this way on purpose —
// acquired_corpus_ttl_days is "0 = never expire".
static void
case_a_declared_zero_stays_zero(void)
{
  kv_register("t.zero", KV_UINT32, "0", NULL, NULL, "");

  test_check_sz(SUITE, "an untouched declared-0 key reads 0",
      0, (size_t)kv_get_uint_or_default("t.zero"));

  kv_set_uint("t.zero", 0);

  test_check_sz(SUITE, "and an explicitly-0 one reads 0",
      0, (size_t)kv_get_uint_or_default("t.zero"));
}

// An undeclared key has no declaration to answer with, and the caller
// gets the same 0 the plain read gives it.
static void
case_an_unregistered_key_has_no_default(void)
{
  test_check_sz(SUITE, "an unregistered key reads 0",
      0, (size_t)kv_get_uint_or_default("t.nope"));

  test_check_sz(SUITE, "an uncomposable per-bot key reads 0",
      0, (size_t)kv_get_bot_uint_or_default(NULL, "behavior.nope"));
}

// The default is stored as the union member its type names, so every
// width the converted sites use has to widen back out of it — a wrong
// member reads as a plausible number rather than as a failure.
static void
case_every_width_widens(void)
{
  kv_register("t.w8",   KV_UINT8,  "200",        NULL, NULL, "");
  kv_register("t.w16",  KV_UINT16, "600",        NULL, NULL, "");
  kv_register("t.w32",  KV_UINT32, "86400",      NULL, NULL, "");
  kv_register("t.w64",  KV_UINT64, "4294967296", NULL, NULL, "");
  kv_register("t.wb",   KV_BOOL,   "true",       NULL, NULL, "");

  kv_set_uint("t.w8",  0);
  kv_set_uint("t.w16", 0);
  kv_set_uint("t.w32", 0);
  kv_set_uint("t.w64", 0);
  kv_set_uint("t.wb",  0);

  test_check_sz(SUITE, "uint8 default widens",
      200, (size_t)kv_get_uint_or_default("t.w8"));
  test_check_sz(SUITE, "uint16 default widens",
      600, (size_t)kv_get_uint_or_default("t.w16"));
  test_check_sz(SUITE, "uint32 default widens",
      86400, (size_t)kv_get_uint_or_default("t.w32"));
  test_check_sz(SUITE, "uint64 default widens past 32 bits",
      (size_t)4294967296ULL, (size_t)kv_get_uint_or_default("t.w64"));
  test_check_sz(SUITE, "a bool default widens to 1",
      1, (size_t)kv_get_uint_or_default("t.wb"));
}

// The per-bot accessor is the composed-key form of the same read, and
// the composition is the half with a silent failure of its own
// (test_kv_bot owns that); this only states that the substitution
// survives it.
static void
case_the_per_bot_form(void)
{
  kv_register("bot.t.behavior.cap", KV_UINT32, "4", NULL, NULL, "");

  test_check_sz(SUITE, "an untouched per-bot knob reads its default",
      4, (size_t)kv_get_bot_uint_or_default("t", "behavior.cap"));

  test_check_bool(SUITE, "0 takes on the composed key",
      SUCCESS, kv_set_bot_uint("t", "behavior.cap", 0));

  test_check_sz(SUITE, "the plain per-bot read answers 0",
      0, (size_t)kv_get_bot_uint("t", "behavior.cap"));

  test_check_sz(SUITE, "and this one answers with the declaration",
      4, (size_t)kv_get_bot_uint_or_default("t", "behavior.cap"));
}

int
main(void)
{
  mem_init();
  clam_init();
  kv_init();

  case_zero_reads_the_declaration();
  case_a_set_value_wins();
  case_an_untouched_key_is_already_its_default();
  case_the_declaration_is_what_moves();
  case_a_declared_zero_stays_zero();
  case_an_unregistered_key_has_no_default();
  case_every_width_widens();
  case_the_per_bot_form();

  return(test_report(SUITE));
}
