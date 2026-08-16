// botmanager — MIT
// Cases for the per-bot KV key grammar (include/kv.h §per-bot keys).
//
// The accessors compose "bot.<name>[.<kind>].<suffix>" for ~110 call
// sites that used to write it by hand. Two things about that are worth
// a test and the rest is not: the two grammars must stay distinct, and
// a key too long to compose must refuse rather than answer with the
// shorter key it truncates onto — the failure KV_KEY_SZ's own comment
// records this tree already paying for once, and the only one here that
// is silent. Everything else fails loudly or not at all.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "kv.h"

#include <stdio.h>
#include <string.h>

#define SUITE "kv_bot"

// A name long enough that "bot." + name + "." + "max_reply_tokens"
// overruns KV_KEY_SZ by a couple of bytes, so the composed key
// truncates onto a spelling this suite then registers. Longer than
// BOT_NAME_SZ allows — the boundary is what is under test here, and the
// production route to it is the deeper grammars (a method kind, a
// plugin's own nesting, IRC's per-channel keys) rather than the name.
#define LONG_NAME_LEN (KV_KEY_SZ - 19)

static void
case_grammars(void)
{
  kv_register("bot.tb.behavior.chat.enabled", KV_BOOL, "true", NULL, NULL, "");
  kv_register("bot.tb.chat_model",  KV_STR,    "sonnet", NULL, NULL, "");
  kv_register("bot.tb.identtimeout", KV_UINT32, "11",    NULL, NULL, "");
  kv_register("bot.tb.irc.identtimeout", KV_UINT32, "7", NULL, NULL, "");

  test_check_sz(SUITE, "driver uint reads its key",
      1, (size_t)kv_get_bot_uint("tb", "behavior.chat.enabled"));

  test_check_str(SUITE, "driver str reads its key",
      "sonnet", kv_get_bot_str("tb", "chat_model"));

  test_check_sz(SUITE, "method uint reads the method key",
      7, (size_t)kv_get_bot_method_uint("tb", "irc", "identtimeout"));

  // The two grammars address different keys with the same suffix; a
  // method read must not fall back to the driver key or the reverse.
  test_check_sz(SUITE, "driver form is not the method form",
      11, (size_t)kv_get_bot_uint("tb", "identtimeout"));

  test_check_sz(SUITE, "absent method kind is not the driver form",
      0, (size_t)kv_get_bot_method_uint("tb", "reachy", "identtimeout"));

  // kv_get_str's own rule travels: a non-KV_STR key reads NULL.
  test_check_bool(SUITE, "str accessor declines a uint key",
      true, kv_get_bot_str("tb", "identtimeout") == NULL);

  test_check_bool(SUITE, "unregistered suffix reads unset",
      true, kv_get_bot_uint("tb", "behavior.nope") == 0);
}

static void
case_absent_scope(void)
{
  test_check_sz(SUITE, "NULL name reads unset",
      0, (size_t)kv_get_bot_uint(NULL, "behavior.chat.enabled"));

  test_check_sz(SUITE, "empty name reads unset",
      0, (size_t)kv_get_bot_uint("", "behavior.chat.enabled"));

  test_check_sz(SUITE, "NULL suffix reads unset",
      0, (size_t)kv_get_bot_uint("tb", NULL));

  test_check_sz(SUITE, "empty suffix reads unset",
      0, (size_t)kv_get_bot_uint("tb", ""));

  test_check_bool(SUITE, "NULL name reads unset (str)",
      true, kv_get_bot_str(NULL, "chat_model") == NULL);

  test_check_bool(SUITE, "empty method kind falls back to the driver form",
      true, kv_get_bot_method_uint("tb", "", "identtimeout") == 11);
}

// The one silent failure the hand-written copies all carried: a name
// and suffix that together overrun KV_KEY_SZ compose a *shorter* key,
// which may well be registered and hold somebody else's value.
static void
case_truncation(void)
{
  char   name[LONG_NAME_LEN + 1];
  char   truncated[KV_KEY_SZ];
  size_t want;

  memset(name, 'b', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';

  // What a hand-written compose into a KV_KEY_SZ buffer produced.
  strlcpy(truncated, "bot.", sizeof(truncated));
  strlcat(truncated, name, sizeof(truncated));
  strlcat(truncated, ".", sizeof(truncated));
  want = strlcat(truncated, "max_reply_tokens", sizeof(truncated));

  test_check_bool(SUITE, "fixture really does overrun the key buffer",
      true, want >= sizeof(truncated));

  // Register the truncated spelling with a value nobody asked for.
  kv_register(truncated, KV_UINT32, "999", NULL, NULL, "");

  test_check_sz(SUITE, "over-long read refuses, not answers the short key",
      0, (size_t)kv_get_bot_uint(name, "max_reply_tokens"));

  test_check_bool(SUITE, "over-long str read refuses",
      true, kv_get_bot_str(name, "max_reply_tokens") == NULL);

  test_check_bool(SUITE, "over-long write refuses",
      FAIL, kv_set_bot_uint(name, "max_reply_tokens", 5));

  // And the value it would have clobbered is untouched.
  test_check_sz(SUITE, "refused write left the short key alone",
      999, (size_t)kv_get_uint(truncated));
}

static void
case_write(void)
{
  kv_register("bot.tb.behavior.fact_extract.hwm", KV_UINT64, "0",
      NULL, NULL, "");

  test_check_bool(SUITE, "write reaches the composed key",
      SUCCESS, kv_set_bot_uint("tb", "behavior.fact_extract.hwm", 4242));

  test_check_sz(SUITE, "written value reads back",
      4242, (size_t)kv_get_bot_uint("tb", "behavior.fact_extract.hwm"));

  test_check_bool(SUITE, "write to an unregistered suffix fails",
      FAIL, kv_set_bot_uint("tb", "behavior.nope", 1));
}

int
main(void)
{
  mem_init();
  clam_init();
  kv_init();

  case_grammars();
  case_absent_scope();
  case_truncation();
  case_write();

  return(test_report(SUITE));
}
