// botmanager — MIT
// /set subcommands (registered as children of the /set parent).

#include "common.h"
#include "bot.h"
#include "cmd.h"
#include "kv.h"
#include "userns.h"

#include <stdio.h>
#include <string.h>

// Custom validators

// KV key format: alphanumeric, dots, underscores, hyphens.
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
        || ch == '-'))
      return false;
  }

  return true;
}

// Argument descriptors

static const cmd_arg_desc_t ad_set_kv[] = {
  { "key",   CMD_ARG_CUSTOM, CMD_ARG_REQUIRED,                KV_KEY_SZ, validate_kv_key },
  { "value", CMD_ARG_NONE,   CMD_ARG_REQUIRED | CMD_ARG_REST, 0,         NULL },
};

// /set kv <key> <value>

static void
cmd_set_kv(const cmd_ctx_t *ctx)
{
  const char *key = ctx->parsed->argv[0];
  const char *value = ctx->parsed->argv[1];

  if(kv_set(key, value) == SUCCESS)
  {
    char buf[KV_KEY_SZ + KV_STR_SZ + 8];

    snprintf(buf, sizeof(buf), "%s = %s", key, value);
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

// /set bot <bot> [<kind>] <key> <value>
//
// Sugar over /set kv that builds the namespaced key for an admin who
// knows the bot but not the full KV path. With three args (bot key
// value), writes bot.<bot>.<key>. With four args (bot kind key value),
// the kind must match the bot's driver and writes bot.<bot>.<kind>.<key>.
//
// Disambiguation between the two forms uses the bot's driver name:
// if the second token equals bot_driver_name(bot), it's parsed as the
// kind. Kind strings ("llm", "command") don't collide with KV key
// segments, so this is unambiguous in practice.

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
  const char *driver;
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

  // If the first token matches the bot's driver kind, treat as
  // 4-arg form (bot kind key value); else 3-arg form (bot key value).
  driver = bot_driver_name(bot);
  four_form = (driver != NULL
      && strlen(driver) == t1_len
      && strncmp(driver, t1, t1_len) == 0
      && t2_len > 0 && *rest != '\0');

  // Reject a kind-looking-but-wrong second token to catch the common
  // mistake "set bot <name> llm ..." against a non-llm bot.
  if(!four_form && t2_len > 0 && *rest != '\0')
  {
    // Heuristic: a 3-form key normally contains a dot. A bare alpha
    // first token followed by another token + value almost certainly
    // means the user typed the wrong kind. Fall through to 3-form
    // anyway, but if the bare token doesn't look like a key, error.
    bool looks_like_key = false;
    for(size_t i = 0; i < t1_len; i++)
      if(t1[i] == '.' || t1[i] == '_') { looks_like_key = true; break; }
    if(!looks_like_key)
    {
      char buf[BOT_NAME_SZ + 64];
      snprintf(buf, sizeof(buf), "bot is kind %s, not %.*s",
          driver ? driver : "?", (int)t1_len, t1);
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
      "set kv <key> <value>",
      "Set a configuration value",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_set_kv, NULL, "set", NULL, ad_set_kv, 2, NULL, NULL);

  cmd_register("cmd", "bot",
      "set bot <bot> [<kind>] <key> <value>",
      "Set a per-bot configuration value (sugar over /set kv)",
      "Builds the namespaced KV path for a bot. Three-arg form writes\n"
      "bot.<bot>.<key>; four-arg form writes bot.<bot>.<kind>.<key>\n"
      "and requires <kind> to match the bot's driver. Refuses keys\n"
      "that are not registered.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_set_bot, NULL, "set", NULL, ad_set_bot,
      (uint8_t)(sizeof(ad_set_bot) / sizeof(ad_set_bot[0])), NULL, NULL);
}
