// botmanager — MIT
// Cases for the situation-provider registry: what a mind is told about a
// bot's own circumstances on the reply path (include/bot.h §Situational
// providers, board #88).
//
// Three things here are worth a table rather than a reading, and each
// fails silently if it is wrong.
//
// The JOIN, because the block chat splices is one line per provider and
// a stray separator either swallows a provider's answer into the one
// before it or ends the fenced block with an empty bullet.
//
// The LENGTH, because collect deliberately does not believe a provider's
// return value: snprintf reports what it WANTED to write, so a provider
// that hands its own truncated count straight back would advance the
// cursor past its own NUL and splice the next answer into the slack.
// The buffer is asked instead, and this states that.
//
// The RECLAIM, because the row holds a function pointer into a plugin's
// mapping and the registration outlives an unload that forgets to
// unregister — the same Class-A backstop the KV contributor table has,
// and the same failure if it is missing: a call into freed memory on the
// next reply, from a curl worker, with the daemon's stderr at /dev/null.

#include "test.h"

#include "bot.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SUITE "bot_situation"

// Cookies are addresses; nothing dereferences them.
static const char cookie_a;
static const char cookie_b;

// Says nothing, the way every provider does for every bot that is not
// in a pit.
static size_t
prov_silent(const char *botname, const char *method_name,
    const char *channel, char *out, size_t out_sz, void *user)
{
  (void)botname; (void)method_name; (void)channel;
  (void)out; (void)out_sz; (void)user;
  return(0);
}

static size_t
prov_a(const char *botname, const char *method_name, const char *channel,
    char *out, size_t out_sz, void *user)
{
  (void)method_name; (void)user;
  return((size_t)snprintf(out, out_sz, "A: %s in %s", botname, channel));
}

static size_t
prov_b(const char *botname, const char *method_name, const char *channel,
    char *out, size_t out_sz, void *user)
{
  (void)botname; (void)method_name; (void)channel; (void)user;
  return((size_t)snprintf(out, out_sz, "B"));
}

// Truncates against its bound and reports the length it WANTED, which is
// exactly what snprintf hands every real provider.
static size_t
prov_overlong(const char *botname, const char *method_name,
    const char *channel, char *out, size_t out_sz, void *user)
{
  (void)botname; (void)method_name; (void)channel; (void)user;
  return((size_t)snprintf(out, out_sz,
      "0123456789012345678901234567890123456789"));
}

static void
drop_all(void)
{
  bot_situation_unregister((void *)&cookie_a);
  bot_situation_unregister((void *)&cookie_b);
}

int
main(void)
{
  char buf[64];
  char small[8];

  // Nothing registered: no bytes, and an emptied buffer rather than
  // whatever the caller's stack held.
  strlcpy(buf, "stale", sizeof(buf));
  test_check_sz(SUITE, "empty registry", 0,
      bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
        buf, sizeof(buf)));
  test_check_str(SUITE, "empty registry clears", "", buf);

  // A provider with nothing to say contributes no separator either.
  bot_situation_register(prov_silent, (void *)&cookie_a);
  test_check_sz(SUITE, "silent provider", 0,
      bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
        buf, sizeof(buf)));
  drop_all();

  // One provider: its answer, and no trailing newline for the caller to
  // strip a second time.
  bot_situation_register(prov_a, (void *)&cookie_a);
  bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
      buf, sizeof(buf));
  test_check_str(SUITE, "one provider", "A: hedgehogg in #cabal", buf);

  // Two: joined by exactly one newline.
  bot_situation_register(prov_b, (void *)&cookie_b);
  bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
      buf, sizeof(buf));
  test_check_str(SUITE, "two providers join", "A: hedgehogg in #cabal\nB",
      buf);

  // Unregistration is by cookie and takes one row, not the table.
  bot_situation_unregister((void *)&cookie_a);
  bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
      buf, sizeof(buf));
  test_check_str(SUITE, "unregister by cookie", "B", buf);
  drop_all();

  // A provider whose return value overshoots the buffer: the cursor
  // follows the bytes, so the second provider's answer lands against the
  // truncation rather than past it.
  bot_situation_register(prov_overlong, (void *)&cookie_a);
  test_check_sz(SUITE, "overlong is buffer-bounded",
      sizeof(small) - 1,
      bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
        small, sizeof(small)));
  test_check_str(SUITE, "overlong truncates", "0123456", small);
  drop_all();

  // Reclaim: an unload's address range. Every register call above is in
  // this binary, so the whole-address-space range takes the row and a
  // range naming no code takes none.
  bot_situation_register(prov_a, (void *)&cookie_a);
  test_check_sz(SUITE, "reclaim spares other mappings", 0,
      bot_reclaim_situations_owned(1, 2));
  test_check_sz(SUITE, "reclaim rejects empty range", 0,
      bot_reclaim_situations_owned(4, 4));
  test_check_sz(SUITE, "reclaim takes the row", 1,
      bot_reclaim_situations_owned(1, UINTPTR_MAX));
  test_check_sz(SUITE, "reclaimed row is gone", 0,
      bot_situation_collect("hedgehogg", "hedgehogg_irc", "#cabal",
        buf, sizeof(buf)));

  return(test_report(SUITE));
}
