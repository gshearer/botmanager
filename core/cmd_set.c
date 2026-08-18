// botmanager — MIT
// /set subcommands (registered as children of the /set parent).

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
  { "key",   CMD_ARG_CUSTOM, CMD_ARG_REQUIRED,                KV_KEY_SZ, validate_kv_key },
  { "value", CMD_ARG_NONE,   CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,         NULL },
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

typedef enum
{
  SET_KV_ASSIGN = 0,   // set kv <key> <value>
  SET_KV_CLEAR,        // set kv --clear <key>
  SET_KV_DELETE,       // set kv --delete <key>
} set_kv_verb_t;

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
        "(that is /db delete kv's job)", key);
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
  set_kv_verb_t verb  = SET_KV_ASSIGN;
  const char   *key;

  if(strcmp(first, "--clear") == 0)
    verb = SET_KV_CLEAR;
  else if(strcmp(first, "--delete") == 0)
    verb = SET_KV_DELETE;

  key = (verb == SET_KV_ASSIGN) ? first : rest;

  if(key == NULL || (verb == SET_KV_ASSIGN && rest == NULL))
  {
    cmd_reply(ctx, set_kv_usage);
    return;
  }

  // Only argv[0] carries the descriptor's validator, and on a flag the
  // key is argv[1] — which is REST, so it can hold whitespace and
  // anything else the operator typed.
  if(verb != SET_KV_ASSIGN && !validate_kv_key(key))
  {
    char buf[KV_KEY_SZ + 32];

    snprintf(buf, sizeof(buf), "invalid key: %s", key);
    cmd_reply(ctx, buf);
    return;
  }

  if(verb == SET_KV_CLEAR)
  {
    set_kv_clear(ctx, key);
    return;
  }

  if(verb == SET_KV_DELETE)
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

static const cmd_arg_desc_t ad_set_bot[] = {
  { "bot",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED,                BOT_NAME_SZ, NULL },
  { "rest", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,           NULL },
};

static void
cmd_set_bot(const cmd_ctx_t *ctx)
{
  const char *botname = ctx->parsed->argv[0];
  const char *rest    = ctx->parsed->argv[1];
  bot_inst_t *bot;
  const char *t1;
  size_t      t1_len;
  const char *t2;
  size_t      t2_len;
  char        method[PLUGIN_NAME_SZ];
  bool        four_form;
  char        key[KV_KEY_SZ];
  const char *value;

  bot = bot_find(botname);
  if(bot == NULL)
  {
    char buf[BOT_NAME_SZ + 32];
    snprintf(buf, sizeof(buf), "no such bot: %s", botname);
    cmd_reply(ctx, buf);
    return;
  }

  while(*rest == ' ' || *rest == '\t') rest++;
  t1 = rest;
  while(*rest != '\0' && *rest != ' ' && *rest != '\t') rest++;
  t1_len = (size_t)(rest - t1);
  while(*rest == ' ' || *rest == '\t') rest++;

  if(t1_len == 0 || *rest == '\0')
  {
    cmd_reply(ctx, "usage: set bot <bot> [<kind>] <key> <value>");
    return;
  }

  t2 = rest;
  while(*rest != '\0' && *rest != ' ' && *rest != '\t') rest++;
  t2_len = (size_t)(rest - t2);
  while(*rest == ' ' || *rest == '\t') rest++;

  // If the first token names a method this bot has bound, treat as
  // 4-arg form (bot method key value); else 3-arg form (bot key value).
  snprintf(method, sizeof(method), "%.*s", (int)t1_len, t1);
  four_form = (bot_has_method_kind(bot, method)
      && t2_len > 0 && *rest != '\0');

  // Reject a method-looking-but-wrong second token to catch the common
  // mistake "set bot <name> irc ..." against a bot with no irc binding.
  if(!four_form && t2_len > 0 && *rest != '\0')
  {
    // Heuristic: a 3-form key normally contains a dot. A bare alpha
    // first token followed by another token + value almost certainly
    // means the user typed the wrong method. Fall through to 3-form
    // anyway, but if the bare token doesn't look like a key, error.
    bool looks_like_key = false;
    for(size_t i = 0; i < t1_len; i++)
      if(t1[i] == '.' || t1[i] == '_') { looks_like_key = true; break; }
    if(!looks_like_key)
    {
      char buf[BOT_NAME_SZ + PLUGIN_NAME_SZ + 32];
      snprintf(buf, sizeof(buf), "%s has no %.*s method",
          botname, (int)t1_len, t1);
      cmd_reply(ctx, buf);
      return;
    }
  }

  if(four_form)
  {
    snprintf(key, sizeof(key), "bot.%s.%.*s.%.*s",
        botname, (int)t1_len, t1, (int)t2_len, t2);
    value = rest;
  }

  else
  {
    snprintf(key, sizeof(key), "bot.%s.%.*s", botname, (int)t1_len, t1);
    // If only one token followed (no value), error — we already
    // checked *rest != '\0' for the four-form, so here t2 is the value.
    if(t2_len == 0)
    {
      cmd_reply(ctx, "usage: set bot <bot> [<kind>] <key> <value>");
      return;
    }
    value = t2;
  }

  if(!validate_kv_key(key))
  {
    char buf[KV_KEY_SZ + 32];
    snprintf(buf, sizeof(buf), "invalid key: %s", key);
    cmd_reply(ctx, buf);
    return;
  }

  if(!kv_exists(key))
  {
    char buf[KV_KEY_SZ + 32];
    snprintf(buf, sizeof(buf), "unknown configuration key: %s", key);
    cmd_reply(ctx, buf);
    return;
  }

  if(kv_set(key, value) != SUCCESS)
  {
    char buf[KV_KEY_SZ + 32];
    snprintf(buf, sizeof(buf), "invalid value for %s", key);
    cmd_reply(ctx, buf);
    return;
  }

  {
    char buf[KV_KEY_SZ + KV_STR_SZ + 8];

    snprintf(buf, sizeof(buf), "%s = %s", key, value);
    cmd_reply(ctx, buf);
  }
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
      "registered. This is not /db delete kv, which drops a persisted row\n"
      "that no live key claims — that one is still the only tool for an\n"
      "orphan, and it unregisters, which for a live key would make the\n"
      "knob stop existing.\n"
      "\n"
      "The flags go after `kv`, not before it. They are not available on\n"
      "`set bot` — that command's three-arg/four-arg disambiguation keys\n"
      "on whether a value follows, so a flag inverts it.\n"
      "\n"
      "Examples:\n"
      "  /set kv --clear plugin.urlgrabber.crawler_agent\n"
      "  /set kv --delete plugin.tmdb.language",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_set_kv, NULL, "set", NULL, ad_set_kv, 2, NULL, NULL);

  cmd_register("cmd", "bot",
      "set bot <bot> [<method>] <key> <value>",
      "Set a per-bot configuration value (sugar over /set kv)",
      "Builds the namespaced KV path for a bot. Three-arg form writes\n"
      "bot.<bot>.<key>; four-arg form writes bot.<bot>.<method>.<key>\n"
      "and requires <method> to name a method the bot has bound.\n"
      "Refuses keys that are not registered.\n"
      "Example: /set bot mini reachy attention.mode name",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_set_bot, NULL, "set", NULL, ad_set_bot,
      (uint8_t)(sizeof(ad_set_bot) / sizeof(ad_set_bot[0])), NULL, NULL);
}
