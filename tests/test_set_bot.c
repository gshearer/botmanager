// botmanager — MIT
// Cases for `set bot`'s three-arg / four-arg form pick (OBS-54).

// The pick decides which real KV key a write lands on, and it has been
// wrong twice: once comparing against bot_driver_name() (so the
// four-token form addressed nothing at all), and once asking "does a
// value follow the second token?" — under which an absent value made
// `set bot karan reachy attention.mode` address bot.karan.reachy, an
// adjacent registered key, with no error. That is the silent-wrongness
// bar this directory is filtered to: every other failure on the path
// (an unknown key, an invalid value) is reported to the operator.
//
// set_bot_parse is a pure function of its arguments — the method lookup
// arrives as a parameter — so these cases need no bot, no KV store and
// no daemon.

#define CMD_SET_INTERNAL
#include "cmd_set.h"

#include "test.h"

#include <stdio.h>
#include <string.h>

#define SUITE "set_bot"

// The stub bot binds "reachy" and "irc" and nothing else. The parser
// never dereferences the instance pointer; only this predicate sees it.
static bool
stub_has_kind(const bot_inst_t *bot, const char *kind)
{
  (void)bot;

  return(strcmp(kind, "reachy") == 0 || strcmp(kind, "irc") == 0);
}

typedef struct
{
  const char  *name;
  const char  *rest;      // the CMD_ARG_REST blob after <bot>
  set_bot_rc_t want_rc;
  const char  *want_key;  // "" when the row does not reach a key
  set_verb_t   want_verb;
  const char  *want_value;  // NULL for the flag forms and the failures
} row_t;

static const row_t rows[] = {
  // The two forms, and the lookup that separates them.
  { "three-arg form",
    "behavior.aka mini",
    SET_BOT_OK, "bot.tb.behavior.aka", SET_VERB_ASSIGN, "mini" },

  { "four-arg form on a bound method",
    "reachy attention.mode name",
    SET_BOT_OK, "bot.tb.reachy.attention.mode", SET_VERB_ASSIGN, "name" },

  { "an unbound method is not a method",
    "gemini attention.mode name",
    SET_BOT_NO_METHOD, "", SET_VERB_ASSIGN, NULL },

  { "a dotted first token stays a key",
    "behavior.soul.pricewatch.exchange coinbase",
    SET_BOT_OK, "bot.tb.behavior.soul.pricewatch.exchange",
    SET_VERB_ASSIGN, "coinbase" },

  // OBS-54 itself: the value is absent, and that is not a form.
  { "bound method with the value absent is a usage error, not a form",
    "reachy attention.mode",
    SET_BOT_USAGE, "", SET_VERB_ASSIGN, NULL },

  { "three-arg key with the value absent is a usage error",
    "behavior.aka",
    SET_BOT_USAGE, "", SET_VERB_ASSIGN, NULL },

  // A value keeps every byte the operator typed, whitespace included.
  { "a three-arg value may hold whitespace",
    "behavior.contract be nice to people",
    SET_BOT_OK, "bot.tb.behavior.contract",
    SET_VERB_ASSIGN, "be nice to people" },

  { "a four-arg value may hold whitespace",
    "irc chan.botman.announcetext hello world",
    SET_BOT_OK, "bot.tb.irc.chan.botman.announcetext",
    SET_VERB_ASSIGN, "hello world" },

  // Flags: leading position only, on both forms.
  { "--clear on the three-arg form",
    "--clear behavior.aka",
    SET_BOT_OK, "bot.tb.behavior.aka", SET_VERB_CLEAR, NULL },

  { "--clear on the four-arg form",
    "--clear reachy tts_voice",
    SET_BOT_OK, "bot.tb.reachy.tts_voice", SET_VERB_CLEAR, NULL },

  { "--delete on the three-arg form",
    "--delete behavior.personality",
    SET_BOT_OK, "bot.tb.behavior.personality", SET_VERB_DELETE, NULL },

  { "--delete on the four-arg form",
    "--delete irc identtimeout",
    SET_BOT_OK, "bot.tb.irc.identtimeout", SET_VERB_DELETE, NULL },

  { "a flag with no key at all",
    "--clear",
    SET_BOT_USAGE, "", SET_VERB_CLEAR, NULL },

  { "a flag form takes no value",
    "--clear behavior.aka mini",
    SET_BOT_USAGE, "", SET_VERB_CLEAR, NULL },

  { "a flag form with an unbound method names the method",
    "--clear gemini tts_voice",
    SET_BOT_NO_METHOD, "", SET_VERB_CLEAR, NULL },

  { "a flag form takes no value after a method",
    "--clear reachy tts_voice af_heart",
    SET_BOT_USAGE, "", SET_VERB_CLEAR, NULL },

  // A flag anywhere but the leading position is data. This is the
  // measured behaviour the fix deliberately keeps: only position tells a
  // flag from a value, because validate_kv_key accepts '-'.
  { "a trailing flag is a value, not a verb",
    "reachy attention.mode --clear",
    SET_BOT_OK, "bot.tb.reachy.attention.mode", SET_VERB_ASSIGN, "--clear" },

  { "a flag in the key position is a key",
    "--clear --delete",
    SET_BOT_OK, "bot.tb.--delete", SET_VERB_CLEAR, NULL },

  // Shape errors.
  { "an empty blob",
    "",
    SET_BOT_USAGE, "", SET_VERB_ASSIGN, NULL },

  { "whitespace only",
    "   \t  ",
    SET_BOT_USAGE, "", SET_VERB_ASSIGN, NULL },

  { "leading whitespace is skipped",
    "   behavior.aka mini",
    SET_BOT_OK, "bot.tb.behavior.aka", SET_VERB_ASSIGN, "mini" },

  { "runs of whitespace between tokens",
    "reachy \t attention.mode \t name",
    SET_BOT_OK, "bot.tb.reachy.attention.mode", SET_VERB_ASSIGN, "name" },
};

static void
case_table(void)
{
  size_t i;

  for(i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
  {
    const row_t    *r = &rows[i];
    set_bot_parse_t p;
    set_bot_rc_t    rc;

    rc = set_bot_parse("tb", r->rest, stub_has_kind, NULL, &p);

    test_check_sz(SUITE, r->name, (size_t)r->want_rc, (size_t)rc);

    if(rc != SET_BOT_OK || r->want_key[0] == '\0')
      continue;

    test_check_str(SUITE, r->name, r->want_key, p.key);
    test_check_sz(SUITE, r->name, (size_t)r->want_verb, (size_t)p.verb);

    if(r->want_value == NULL)
      test_check_bool(SUITE, r->name, true, p.value == NULL);
    else
      test_check_str(SUITE, r->name, r->want_value, p.value);
  }
}

// A key too long to compose must refuse rather than answer with the
// shorter spelling it truncates onto — the one silent failure left on
// this path, and one KV_KEY_SZ's own comment records this tree paying
// for once already.
static void
case_truncation(void)
{
  char            rest[KV_KEY_SZ * 2];
  char            suffix[KV_KEY_SZ];
  set_bot_parse_t p;
  size_t          i;

  for(i = 0; i < sizeof(suffix) - 1; i++)
    suffix[i] = 'k';

  suffix[sizeof(suffix) - 1] = '\0';

  snprintf(rest, sizeof(rest), "reachy %s value", suffix);

  test_check_sz(SUITE, "an overlong four-arg key refuses",
      (size_t)SET_BOT_KEY_TOO_LONG,
      (size_t)set_bot_parse("tb", rest, stub_has_kind, NULL, &p));

  snprintf(rest, sizeof(rest), "%s value", suffix);

  test_check_sz(SUITE, "an overlong three-arg key refuses",
      (size_t)SET_BOT_KEY_TOO_LONG,
      (size_t)set_bot_parse("tb", rest, stub_has_kind, NULL, &p));
}

int
main(void)
{
  case_table();
  case_truncation();

  return(test_report(SUITE));
}
