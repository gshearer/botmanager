// botmanager — MIT
// Cases for the lifetime kv_get_str promises (include/kv.h).
//
// ~205 sites take a `const char *` from kv_get_str and use it after the
// lock is gone. What the header promises them is that the bytes behind
// that pointer never change and the pointer never dies, so the only
// thing that can go wrong is staleness — which is loud, local and the
// caller's business. Both halves of that promise fail SILENTLY when
// they fail: a value written over mid-read is a garbled string nobody
// logs, and an entry freed under a reader is a use-after-free that
// usually returns the right answer anyway. Hence a table.
//
// Every row here is expressed as pointer identity rather than as a
// crash, so the suite is red on a regression under a plain -O2 build
// and does not need a sanitizer to notice.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"

#include <string.h>

#define SUITE "kv_str"

// A value long enough to prove the KV_STR_SZ bound still bites: the
// inline buffer it used to be copied into is gone, and truncation is
// the one behaviour of it that had to survive.
#define LONG_VAL_LEN (KV_STR_SZ + 64)

// The set path used to write over the buffer the reader was holding.
static void
case_value_is_immutable(void)
{
  const char *held;

  kv_register("t.imm", KV_STR, "alpha", NULL, NULL, "");

  held = kv_get_str("t.imm");

  test_check_str(SUITE, "read answers with the value", "alpha", held);

  test_check_bool(SUITE, "set takes",
      SUCCESS, kv_set("t.imm", "omega"));

  test_check_str(SUITE, "a fresh read sees the new value",
      "omega", kv_get_str("t.imm"));

  // The point of the whole change: the pointer handed out before the
  // write still reads what it was handed, because the write installed a
  // different string rather than editing this one.
  test_check_str(SUITE, "the held pointer is not rewritten",
      "alpha", held);

  test_check_bool(SUITE, "the held pointer is not the new value",
      true, held != kv_get_str("t.imm"));
}

// The entry the pointer came out of is freed by kv_unregister and, in
// production, by a plugin unload's kv_reclaim_owned. Identity against a
// value re-interned from a literal is what says the string did not live
// in that entry: two spellings of "beta" are one pointer only if both
// came from the intern table.
static void
case_pointer_outlives_the_entry(void)
{
  const char *held;

  kv_register("t.gone", KV_STR, "beta", NULL, NULL, "");

  held = kv_get_str("t.gone");

  test_check_bool(SUITE, "dropping the key succeeds",
      SUCCESS, kv_unregister("t.gone"));

  test_check_bool(SUITE, "the key really is gone",
      true, kv_get_str("t.gone") == NULL);

  test_check_str(SUITE, "the held pointer still reads its value",
      "beta", held);

  kv_register("t.other", KV_STR, "beta", NULL, NULL, "");

  test_check_bool(SUITE, "and it is storage the registry still owns",
      true, held == kv_get_str("t.other"));
}

// Interning is not a nicety here — it is what makes the two rows above
// cheap, and it makes value equality answerable by pointer.
static void
case_interning(void)
{
  kv_register("t.a", KV_STR, "shared", NULL, NULL, "");
  kv_register("t.b", KV_STR, "shared", NULL, NULL, "");

  test_check_bool(SUITE, "equal values are one string",
      true, kv_get_str("t.a") == kv_get_str("t.b"));

  test_check_bool(SUITE, "repeat reads are one string",
      true, kv_get_str("t.a") == kv_get_str("t.a"));

  test_check_bool(SUITE, "unequal values are not",
      true, kv_get_str("t.a") != kv_get_str("t.imm"));

  // Setting a key to a value another key already holds must reach the
  // same string, not a second copy of it. ("t.other" holds "beta".)
  kv_set("t.b", "beta");

  test_check_bool(SUITE, "a set reaches the existing string",
      true, kv_get_str("t.b") == kv_get_str("t.other"));

  // And setting a key back to what it already holds is not a change.
  test_check_bool(SUITE, "an idempotent set still succeeds",
      SUCCESS, kv_set("t.b", "beta"));

  test_check_str(SUITE, "an idempotent set leaves the value",
      "beta", kv_get_str("t.b"));
}

// KV_STR_SZ used to be the size of the buffer the value was copied
// into. It is now a bound the intern table applies by hand, so the
// truncation it always performed is worth a row.
static void
case_bound(void)
{
  char        val[LONG_VAL_LEN + 1];
  const char *got;

  memset(val, 'x', sizeof(val) - 1);
  val[sizeof(val) - 1] = '\0';

  kv_register("t.long", KV_STR, val, NULL, NULL, "");

  got = kv_get_str("t.long");

  test_check_sz(SUITE, "an over-long value truncates at the bound",
      KV_STR_SZ - 1, strlen(got));

  test_check_bool(SUITE, "and truncates to a prefix of itself",
      true, strncmp(got, val, KV_STR_SZ - 1) == 0);
}

// kv_get_str's other documented answer, and the one a caller is most
// likely to get wrong: NULL means "no string here", never "empty".
static void
case_non_string_keys(void)
{
  kv_register("t.num", KV_UINT32, "7", NULL, NULL, "");
  kv_register("t.empty", KV_STR, "", NULL, NULL, "");

  test_check_bool(SUITE, "a non-string key reads NULL",
      true, kv_get_str("t.num") == NULL);

  test_check_bool(SUITE, "an unregistered key reads NULL",
      true, kv_get_str("t.nope") == NULL);

  test_check_str(SUITE, "an empty value is a string, not NULL",
      "", kv_get_str("t.empty"));
}

int
main(void)
{
  mem_init();
  clam_init();
  kv_init();

  case_value_is_immutable();
  case_pointer_outlives_the_entry();
  case_interning();
  case_bound();
  case_non_string_keys();

  return(test_report(SUITE));
}
