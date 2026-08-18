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

// The help string carries the same promise for the same reason (OBS-14):
// "static caller-owned storage" meant static *in the owning .so*, so the
// pointer outlived the mapping it pointed into. Those rows live at the
// bottom of this file.

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

// The pair OBS-37 rests on. Eleven read sites had their
// `if(s == NULL || s[0] == '\0') s = <the declaration>;` fallback deleted,
// and both halves of that deletion are invisible when they break: the
// first arm turning live is a NULL walking into a "%s", and the second
// arm turning live again is `set kv --clear` being silently swallowed by
// a substituted default.
static void
case_declared_default_and_clear(void)
{
  kv_register("t.decl", KV_STR, "en-US", NULL, NULL, "");

  test_check_str(SUITE, "a registered key answers with its declaration",
      "en-US", kv_get_str("t.decl"));

  test_check_bool(SUITE, "and never answers NULL, so a fallback is dead",
      true, kv_get_str("t.decl") != NULL);

  // What `set kv --clear` does. The declaration must NOT come back: it
  // is reachable again only through kv_reset (`set kv --delete`).
  kv_set("t.decl", "");

  test_check_str(SUITE, "a cleared key answers empty, not its declaration",
      "", kv_get_str("t.decl"));

  test_check_bool(SUITE, "a cleared key is still not NULL",
      true, kv_get_str("t.decl") != NULL);

  kv_reset("t.decl");

  test_check_str(SUITE, "and kv_reset is what brings the declaration back",
      "en-US", kv_get_str("t.decl"));
}

// A help string long enough to prove the value bound does NOT apply to it:
// help text is prose and routinely runs past KV_STR_SZ.
#define LONG_HELP_LEN (KV_STR_SZ + 200)

// The row OBS-14 is about. Every registration hands core a `const char *`
// the caller swears is static — and it is, in the caller's own .so, which
// a plugin unload unmaps out from under the one reader in core. There is
// no dlclose to stage in a unit test, so the equivalent is staged here:
// storage the registry does not own, released after the registration.
// Failure is silent either way — a freed buffer usually still reads back
// the right bytes.
static void
case_help_outlives_its_storage(void)
{
  char       *owned = mem_alloc("test", "help", 64);
  const char *held;

  strlcpy(owned, "the caller's storage", 64);

  kv_register("t.help", KV_UINT32, "1", NULL, NULL, owned);

  held = kv_get_help("t.help");

  test_check_str(SUITE, "help reads back what was registered",
      "the caller's storage", held);

  test_check_bool(SUITE, "and it is not the caller's pointer",
      true, held != owned);

  // The caller's storage goes away, exactly as its mapping does.
  memset(owned, 'z', 63);
  owned[63] = '\0';
  mem_free(owned);

  test_check_str(SUITE, "the held pointer survives its storage",
      "the caller's storage", held);

  test_check_str(SUITE, "and so does a fresh read",
      "the caller's storage", kv_get_help("t.help"));
}

// Interning is what makes the row above cheap: the per-bot schemas hand
// one literal to five bots' worth of keys.
static void
case_help_interning(void)
{
  kv_register("t.h1", KV_BOOL, "false", NULL, NULL, "one spelling");
  kv_register("t.h2", KV_BOOL, "false", NULL, NULL, "one spelling");

  test_check_bool(SUITE, "equal help is one string",
      true, kv_get_help("t.h1") == kv_get_help("t.h2"));

  test_check_bool(SUITE, "unequal help is not",
      true, kv_get_help("t.h1") != kv_get_help("t.help"));
}

// NULL help is legal (irc.c registers one) and must not become "".
static void
case_help_absent(void)
{
  kv_register("t.h0", KV_BOOL, "false", NULL, NULL, NULL);

  test_check_bool(SUITE, "NULL help stays NULL",
      true, kv_get_help("t.h0") == NULL);

  test_check_bool(SUITE, "an unregistered key reads NULL help",
      true, kv_get_help("t.nope") == NULL);
}

// KV_STR_SZ bounds a value because a value is serialized into buffers of
// that size. Help is prose and answers only to the renderer, so the bound
// must not reach it — truncating it would be a new silent defect.
static void
case_help_is_not_bounded_like_a_value(void)
{
  char help[LONG_HELP_LEN + 1];

  memset(help, 'h', sizeof(help) - 1);
  help[sizeof(help) - 1] = '\0';

  kv_register("t.hlong", KV_BOOL, "false", NULL, NULL, help);

  test_check_sz(SUITE, "long help is kept whole",
      LONG_HELP_LEN, strlen(kv_get_help("t.hlong")));
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
  case_declared_default_and_clear();

  case_help_outlives_its_storage();
  case_help_interning();
  case_help_absent();
  case_help_is_not_bounded_like_a_value();

  return(test_report(SUITE));
}
