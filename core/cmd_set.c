// botmanager — MIT
// /set subcommands (registered as children of the /set parent).

#define CMD_SET_INTERNAL
#include "cmd_set.h"

#include "common.h"
#include "bot.h"
#include "cmd.h"
#include "kv.h"
#include "plugin.h"   // PLUGIN_NAME_SZ — the bound on a method kind
#include "userns.h"

#include <stdio.h>
#include <string.h>

// Custom validators

// KV key format: alphanumeric, dots, underscores, hyphens, and '@'.
// '@' is the multi-instance market-ID separator ("<exch>-<base>-<quote>@
// <instance>", WM-MI) and appears in per-market KV paths such as
// plugin.whenmoon.market.coinbase-btc-usd@mako.strategy.mako.alpha. The
// KV store and DB accept it; without it here per-instance config is
// unsettable via `set kv`.
static bool
validate_kv_key(const char *str)
{
  if(str == NULL || str[0] == '\0')
    return false;

  for(const char *k = str; *k != '\0'; k++)
  {
    unsigned char ch = (unsigned char)*k;

    if(!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_'
        || ch == '-' || ch == '@'))
      return false;
  }

  return true;
}

// Argument descriptors

static const cmd_arg_desc_t ad_set_kv[] = {
  { "key",   CMD_ARG_CUSTOM, CMD_ARG_REQUIRED,                KV_KEY_SZ - 1, validate_kv_key },
  { "value", CMD_ARG_NONE,   CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,             NULL },
};

// /set kv <key> <value>  ·  /set kv --clear <key>  ·  --delete <key>
//
// An empty value used to be untypeable here: `value` was REQUIRED, so a
// bare key failed arg parsing, and `""` stored the two literal quote
// characters. Four plugins invented a sentinel vocabulary to route
// around that — giphy, urlgrabber and chat's memory each decode
// ""/"\"\""/none/off, and chat's aka list invented a lone "-" — and the
// same word means "broadest allowed" in one of them and "feature off"
// in the others (OBS-50). `--clear` is the verb they were working
// around.
//
// The flag lives INSIDE the subcommand. `set --clear kv <key>` puts it
// ahead of the child, which would need flag handling in core/cmd.c's
// dispatcher and a flag concept in cmd_arg_desc_t — the framework every
// plugin in this tree registers against. That form was measured and
// refused on exactly that blast radius; do not re-propose it.
//
// ⚠ validate_kv_key accepts '-', so "--clear" passes validation as a key
// and arrives in the key position rather than being rejected. The flag
// test is therefore positional and explicit — argv[0], nowhere else —
// never a fallthrough from a lookup that failed.

static const char set_kv_usage[] =
    "usage: set kv <key> <value> | set kv --clear <key> "
    "| set kv --delete <key>";

// Empty a KV_STR key. "" parses for KV_STR alone — str_to_val interns it
// unconditionally there and every other arm fails on `end == str`, so
// the other types self-refuse — but they refuse as "invalid value for
// X", which describes the wrong event when the operator supplied no
// value at all. Name the type instead.
static void
set_kv_clear(const cmd_ctx_t *ctx, const char *key)
{
  const char *type;
  char        buf[KV_KEY_SZ + 128];

  type = kv_get_type_name(key);

  if(type == NULL)
    snprintf(buf, sizeof(buf), "unknown key: %s", key);

  else if(strcmp(type, "STR") != 0)
    snprintf(buf, sizeof(buf),
        "cannot clear %s: it is %s, and only STR holds an empty value",
        key, type);

  else if(kv_set(key, "") != SUCCESS)
    snprintf(buf, sizeof(buf), "could not clear %s", key);

  else
  {
    kv_flush();

    // A secret's reply states the action and never the value. And a
    // cleared credential is not restorable from anything on disk: keys
    // here are installed by hand and live in no file, so say so.
    if(kv_is_secret_key(key))
      snprintf(buf, sizeof(buf),
          "cleared %s — secret; re-enter it by hand to restore", key);
    else
      snprintf(buf, sizeof(buf), "cleared %s (now empty)", key);
  }

  cmd_reply(ctx, buf);
}

// Retire a key's stored value. Two different retirements live here and
// the operator cannot tell them apart from the outside, so the reply
// says which happened: a registered key keeps its binding and goes back
// to what its declaration named, while an unregistered one has no
// declaration to go back to and only its row goes.
//
// ⛔ No kv_flush() on this path. kv_reset leaves the entry clean exactly
// so nothing re-persists the row it just dropped; flushing here would be
// asking the one question this verb exists to answer NO.
static void
set_kv_delete(const cmd_ctx_t *ctx, const char *key)
{
  char buf[KV_KEY_SZ + KV_STR_SZ + 128];

  if(!kv_exists(key))
  {
    kv_delete(key);
    snprintf(buf, sizeof(buf),
        "%s is not registered — dropped any persisted row "
        "(that is db delete kv's job)", key);
  }

  else if(!kv_reset(key))
    snprintf(buf, sizeof(buf), "could not reset %s", key);

  // A secret's reply states the action and never the value, on this arm
  // as on --clear.
  else if(kv_is_secret_key(key))
    snprintf(buf, sizeof(buf),
        "%s reverted to its declared default — secret; value not shown",
        key);

  else
  {
    char val[KV_STR_SZ];

    // ⚠ kv_get_val_str answers with common.h's inverted SUCCESS (false),
    // not with a predicate's true — unlike kv_exists and kv_reset either
    // side of it. A bare `!` here reads every success as a failure, and
    // the only symptom is a reply that says "?".
    if(kv_get_val_str(key, val, sizeof(val)) != SUCCESS)
      strlcpy(val, "?", sizeof(val));

    snprintf(buf, sizeof(buf),
        "%s reverted to its declared default (%s) and its row dropped",
        key, val);
  }

  cmd_reply(ctx, buf);
}

static void
cmd_set_kv(const cmd_ctx_t *ctx)
{
  const char   *first = ctx->parsed->argv[0];
  const char   *rest  = ctx->parsed->argc > 1 ? ctx->parsed->argv[1] : NULL;
  set_verb_t    verb  = SET_VERB_ASSIGN;
  const char   *key;

  if(strcmp(first, "--clear") == 0)
    verb = SET_VERB_CLEAR;
  else if(strcmp(first, "--delete") == 0)
    verb = SET_VERB_DELETE;

  key = (verb == SET_VERB_ASSIGN) ? first : rest;

  if(key == NULL || (verb == SET_VERB_ASSIGN && rest == NULL))
  {
    cmd_reply(ctx, set_kv_usage);
    return;
  }

  // Only argv[0] carries the descriptor's validator, and on a flag the
  // key is argv[1] — which is REST, so it can hold whitespace and
  // anything else the operator typed.
  if(verb != SET_VERB_ASSIGN && !validate_kv_key(key))
  {
    char buf[KV_KEY_SZ + 32];

    snprintf(buf, sizeof(buf), "invalid key: %s", key);
    cmd_reply(ctx, buf);
    return;
  }

  if(verb == SET_VERB_CLEAR)
  {
    set_kv_clear(ctx, key);
    return;
  }

  if(verb == SET_VERB_DELETE)
  {
    set_kv_delete(ctx, key);
    return;
  }

  if(kv_set(key, rest) == SUCCESS)
  {
    char buf[KV_KEY_SZ + KV_STR_SZ + 8];

    snprintf(buf, sizeof(buf), "%s = %s", key, rest);
    cmd_reply(ctx, buf);
    kv_flush();
  }

  else
  {
    char buf[KV_KEY_SZ + 64];

    if(!kv_exists(key))
      snprintf(buf, sizeof(buf), "unknown key: %s", key);
    else
      snprintf(buf, sizeof(buf), "invalid value for %s", key);

    cmd_reply(ctx, buf);
  }
}

// /set bot <bot> [<method>] <key> <value>
//
// Sugar over /set kv that builds the namespaced key for an admin who
// knows the bot but not the full KV path. With three args (bot key
// value), writes bot.<bot>.<key>. With four args (bot method key value),
// writes bot.<bot>.<method>.<key> — the per-method tier, where a method
// plugin's KV schema is copied for each bot that binds it
// (bot_register_method_kv).
//
// Disambiguation between the two forms is bot_has_method_kind(): the
// second token is a method kind when the bot has that method bound. It
// used to be compared against bot_driver_name() instead, which meant the
// four-token form could only ever address the bot plugin's own tier —
// and that tier has no <kind> segment, so it addressed nothing at all
// while every real per-method knob had to be written through raw
// `set kv bot.<n>.<method>.<key>`. Method kinds ("irc", "reachy") don't
// collide with KV key segments, so this stays unambiguous in practice.
//
// That is the SECOND time this disambiguation has been wrong, and OBS-54
// is the third. It also asked "does a value follow the second token?",
// which is not a question about the form at all: with the value absent,
// `set bot karan reachy attention.mode` picked the three-arg form and
// addressed bot.karan.reachy — a different, adjacent key — rather than
// saying the value was missing. Nothing here reads a value to decide a
// form any more; the token count and the method lookup decide it, and a
// missing value is reported as one.

static const char set_bot_usage[] =
    "usage: set bot <bot> [<kind>] <key> <value> | "
    "set bot <bot> --clear|--delete [<kind>] <key>";

static const cmd_arg_desc_t ad_set_bot[] = {
  { "bot",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                BOT_NAME_SZ - 1, NULL },
  { "rest", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,               NULL },
};

// One whitespace-delimited token. Advances *p past the token and the
// whitespace behind it, and answers the token's length — 0 at the end of
// the blob, where *tok is left on the terminator.
static size_t
set_token(const char **p, const char **tok)
{
  const char *s = *p;
  size_t      len;

  *tok = s;

  while(*s != '\0' && *s != ' ' && *s != '\t')
    s++;

  len = (size_t)(s - *tok);

  while(*s == ' ' || *s == '\t')
    s++;

  *p = s;

  return(len);
}

static set_verb_t
set_verb_of(const char *tok, size_t len)
{
  if(len == 7 && memcmp(tok, "--clear", 7) == 0)
    return(SET_VERB_CLEAR);

  if(len == 8 && memcmp(tok, "--delete", 8) == 0)
    return(SET_VERB_DELETE);

  return(SET_VERB_ASSIGN);
}

// A three-arg key carries a '.' or a '_'; a bare word in that position
// followed by two more tokens is almost always a method kind the bot has
// not bound, and saying so beats writing bot.<bot>.<typo>.
static bool
set_looks_like_key(const char *tok, size_t len)
{
  size_t i;

  for(i = 0; i < len; i++)
    if(tok[i] == '.' || tok[i] == '_')
      return(true);

  return(false);
}

set_bot_rc_t
set_bot_parse(const char *botname, const char *rest,
    set_bot_has_kind_fn has_kind, const bot_inst_t *bot,
    set_bot_parse_t *out)
{
  const char *a1;
  size_t      a1_len;
  const char *a2;
  size_t      a2_len;
  const char *tail;
  const char *kind       = NULL;
  size_t      kind_len   = 0;
  const char *suffix     = NULL;
  size_t      suffix_len = 0;
  char        cand[PLUGIN_NAME_SZ];
  bool        four       = false;
  int         n;

  out->verb    = SET_VERB_ASSIGN;
  out->value   = NULL;
  out->key[0]  = '\0';
  out->kind[0] = '\0';

  while(*rest == ' ' || *rest == '\t')
    rest++;

  a1_len = set_token(&rest, &a1);

  if(a1_len == 0)
    return(SET_BOT_USAGE);

  // A flag is recognised in this position and nowhere else, which is the
  // rule `set kv` already keeps (OBS-50): validate_kv_key accepts '-', so
  // a flag-shaped token is a legal key AND a legal value, and only where
  // it sits can tell them apart. Everything past this point is data,
  // "--clear" included.
  out->verb = set_verb_of(a1, a1_len);

  if(out->verb != SET_VERB_ASSIGN)
  {
    a1_len = set_token(&rest, &a1);

    if(a1_len == 0)
      return(SET_BOT_USAGE);
  }

  a2_len = set_token(&rest, &a2);
  tail   = rest;

  // The form question is "is there a second token, and does the first
  // name a method this bot has bound?" — never "is there a value?".
  if(a2_len > 0)
  {
    snprintf(cand, sizeof(cand), "%.*s", (int)a1_len, a1);
    four = has_kind(bot, cand);
  }

  if(out->verb == SET_VERB_ASSIGN)
  {
    if(a2_len == 0)
      return(SET_BOT_USAGE);

    if(four)
    {
      // <kind> <key> and nothing else: the value is missing, and this is
      // the case OBS-54 answered by silently addressing another key.
      if(*tail == '\0')
        return(SET_BOT_USAGE);

      kind       = a1;
      kind_len   = a1_len;
      suffix     = a2;
      suffix_len = a2_len;
      out->value = tail;
    }

    else
    {
      if(*tail != '\0' && !set_looks_like_key(a1, a1_len))
      {
        snprintf(out->kind, sizeof(out->kind), "%.*s", (int)a1_len, a1);
        return(SET_BOT_NO_METHOD);
      }

      suffix     = a1;
      suffix_len = a1_len;
      out->value = a2;   // a2 onward, so a value may hold whitespace
    }
  }

  // A flag form names a key and stops. Two tokens can only be
  // <kind> <key>; anything after them is a value the verb has no use for.
  else if(a2_len == 0)
  {
    suffix     = a1;
    suffix_len = a1_len;
  }

  else if(*tail != '\0')
    return(SET_BOT_USAGE);

  else if(!four)
  {
    // Two tokens after a flag can only be <kind> <key>. If the first
    // does not name a bound method, say which mistake it was: a method
    // this bot lacks, or a value the verb has no use for.
    if(set_looks_like_key(a1, a1_len))
      return(SET_BOT_USAGE);

    snprintf(out->kind, sizeof(out->kind), "%.*s", (int)a1_len, a1);
    return(SET_BOT_NO_METHOD);
  }

  else
  {
    kind       = a1;
    kind_len   = a1_len;
    suffix     = a2;
    suffix_len = a2_len;
  }

  if(kind != NULL)
    n = snprintf(out->key, sizeof(out->key), "bot.%s.%.*s.%.*s",
        botname, (int)kind_len, kind, (int)suffix_len, suffix);
  else
    n = snprintf(out->key, sizeof(out->key), "bot.%s.%.*s",
        botname, (int)suffix_len, suffix);

  // A truncated key is the one silent failure on this path: it lands on
  // a shorter spelling that can be a real registered knob (KV_KEY_SZ's
  // own comment records this tree paying for that once).
  if(n < 0 || (size_t)n >= sizeof(out->key))
    return(SET_BOT_KEY_TOO_LONG);

  return(SET_BOT_OK);
}

static void
cmd_set_bot(const cmd_ctx_t *ctx)
{
  const char     *botname = ctx->parsed->argv[0];
  const char     *rest    = ctx->parsed->argv[1];
  bot_inst_t     *bot;
  set_bot_parse_t p;
  char            buf[KV_KEY_SZ + KV_STR_SZ + 64];

  bot = bot_find(botname);

  if(bot == NULL)
  {
    snprintf(buf, sizeof(buf), "no such bot: %s", botname);
    cmd_reply(ctx, buf);
    return;
  }

  switch(set_bot_parse(botname, rest, bot_has_method_kind, bot, &p))
  {
    case SET_BOT_OK:
      break;

    case SET_BOT_USAGE:
      cmd_reply(ctx, set_bot_usage);
      return;

    case SET_BOT_NO_METHOD:
      snprintf(buf, sizeof(buf), "%s has no %s method", botname, p.kind);
      cmd_reply(ctx, buf);
      return;

    case SET_BOT_KEY_TOO_LONG:
      snprintf(buf, sizeof(buf),
          "key too long: bot.%s.%s overruns %d bytes", botname, rest,
          (int)KV_KEY_SZ);
      cmd_reply(ctx, buf);
      return;
  }

  if(!validate_kv_key(p.key))
  {
    snprintf(buf, sizeof(buf), "invalid key: %s", p.key);
    cmd_reply(ctx, buf);
    return;
  }

  if(!kv_exists(p.key))
  {
    snprintf(buf, sizeof(buf), "unknown configuration key: %s", p.key);
    cmd_reply(ctx, buf);
    return;
  }

  // The two flag arms are `set kv`'s, reached through a key this command
  // composed rather than one the operator spelled out.
  if(p.verb == SET_VERB_CLEAR)
  {
    set_kv_clear(ctx, p.key);
    return;
  }

  if(p.verb == SET_VERB_DELETE)
  {
    set_kv_delete(ctx, p.key);
    return;
  }

  if(kv_set(p.key, p.value) != SUCCESS)
  {
    snprintf(buf, sizeof(buf), "invalid value for %s", p.key);
    cmd_reply(ctx, buf);
    return;
  }

  snprintf(buf, sizeof(buf), "%s = %s", p.key, p.value);
  cmd_reply(ctx, buf);
  kv_flush();
}

// Registration

void
cmd_set_register(void)
{
  cmd_register("cmd", "kv",
      "set kv <key> <value> | set kv --clear|--delete <key>",
      "Set a configuration value",
      "--clear empties a STR key — the empty value `set kv` could not\n"
      "otherwise express, since a bare key fails arg parsing and \"\" is\n"
      "stored as two literal quote characters. Refused on every other\n"
      "type: \"\" parses for STR alone, and an empty number would have to\n"
      "mean zero, which is a value and not an absence.\n"
      "\n"
      "--delete retires a registered key's STORED value: the key reverts\n"
      "to the default its declaration named and its database row goes, so\n"
      "the next boot reads the declaration. The key itself stays\n"
      "registered. This is not db delete kv, which drops a persisted row\n"
      "that no live key claims — that one is still the only tool for an\n"
      "orphan, and it unregisters, which for a live key would make the\n"
      "knob stop existing.\n"
      "\n"
      "The flags go after `kv`, not before it. `set bot` takes the same\n"
      "two, in the same position — after the bot name, ahead of the key\n"
      "(OBS-54). A flag is recognised by where it sits and nowhere else,\n"
      "on both commands, because validate_kv_key accepts '-' and so a\n"
      "flag-shaped token is a legal key and a legal value alike.\n"
      "\n"
      "Examples:\n"
      "  set kv --clear plugin.urlgrabber.crawler_agent\n"
      "  set kv --delete plugin.tmdb.language",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_set_kv, NULL, "set", NULL, ad_set_kv, 2, NULL, NULL);

  cmd_register("cmd", "bot",
      "set bot <bot> [<method>] <key> <value> | "
      "set bot <bot> --clear|--delete [<method>] <key>",
      "Set a per-bot configuration value (sugar over set kv)",
      "Builds the namespaced KV path for a bot. Three-arg form writes\n"
      "bot.<bot>.<key>; four-arg form writes bot.<bot>.<method>.<key>\n"
      "and requires <method> to name a method the bot has bound. Which\n"
      "form you get is decided by the token count and that lookup — never\n"
      "by whether a value follows, which is what made an absent value\n"
      "address a different real key (OBS-54).\n"
      "Refuses keys that are not registered.\n"
      "\n"
      "--clear and --delete mean exactly what they mean on `set kv`, and\n"
      "go after the bot name: this is the tier where an empty value is\n"
      "most wanted, since bot.<n>.behavior.aka is why a `-` sentinel had\n"
      "to be invented to spell one.\n"
      "\n"
      "Examples:\n"
      "  set bot mini reachy attention.mode name\n"
      "  set bot mini --clear behavior.aka\n"
      "  set bot mini --delete reachy tts_voice",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_set_bot, NULL, "set", NULL, ad_set_bot,
      (uint8_t)(sizeof(ad_set_bot) / sizeof(ad_set_bot[0])), NULL, NULL);
}
