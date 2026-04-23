// botmanager — MIT
// /bot <name> hush <duration>: kind-filtered chat-bot mute verb.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Parse a short duration string: "30s", "5m", "2h", "1d", or a bare
// number interpreted as seconds. Returns 0 on parse failure.
static uint64_t
parse_duration_secs(const char *s)
{
  char              *endp;
  unsigned long long n;

  if(s == NULL || s[0] == '\0') return(0);

  endp = NULL;
  n = strtoull(s, &endp, 10);

  if(endp == s) return(0);

  // Skip whitespace between number and unit.
  while(*endp == ' ' || *endp == '\t') endp++;

  if(*endp == '\0' || *endp == 's' || *endp == 'S') return((uint64_t)n);
  if(*endp == 'm' || *endp == 'M')                  return((uint64_t)n * 60ULL);
  if(*endp == 'h' || *endp == 'H')                  return((uint64_t)n * 3600ULL);
  if(*endp == 'd' || *endp == 'D')                  return((uint64_t)n * 86400ULL);
  return(0);
}

// /bot <name> hush <duration>
//
// <name> is resolved by the core dispatcher; this handler sees
// ctx->bot = the resolved bot and ctx->parsed->argv[0] = duration.
// Chat-kind filter on the registration keeps the dispatcher from
// routing non-chat bots here.
static const cmd_arg_desc_t ad_bot_hush[] = {
  { "duration", CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
};

static void
cmd_bot_hush(const cmd_ctx_t *ctx)
{
  uint64_t secs = parse_duration_secs(ctx->parsed->argv[0]);
  char buf[128];
  uint64_t until;
  const char *botname;
  char key[KV_KEY_SZ];

  if(secs == 0)
  {
    cmd_reply(ctx, "bad duration (use e.g. 30s, 5m, 2h, 1d)");
    return;
  }

  botname = bot_inst_name(ctx->bot);

  snprintf(key, sizeof(key), "bot.%s.behavior.mute_until", botname);

  until = (uint64_t)time(NULL) + secs;

  if(kv_set_uint(key, until) != SUCCESS)
  {
    cmd_reply(ctx, "failed to set mute");
    return;
  }

  snprintf(buf, sizeof(buf),
      "muted '%s' for %llu seconds (%s)", botname,
      (unsigned long long)secs, ctx->parsed->argv[0]);
  cmd_reply(ctx, buf);
}

// /bot <name> refresh_prompts
//
// Forces the bot's cached personality / interests state to re-sync with
// the current KV and on-disk personality file. The personality body and
// output contract are already re-read from disk on every reply, so edits
// to those files take effect without this command. What this command
// refreshes is the state that only updates lazily:
//   - bot.<name>.behavior.personality    -> st->active_name
//   - bot.<name>.behavior.contract       -> reported (no cache)
//   - st->topics[] / st->registered_persona so the reactive-topic cache
//     is re-parsed from the personality's `interests:` frontmatter --
//     useful after editing that block without renaming the persona.
static void
cmd_bot_refresh_prompts(const cmd_ctx_t *ctx)
{
  chatbot_state_t *st = bot_get_handle(ctx->bot);
  char buf[256];
  const char *contract;
  const char *persona;
  const char *botname;
  char        key[KV_KEY_SZ];

  if(st == NULL)
  {
    cmd_reply(ctx, "bot has no chatbot state");
    return;
  }

  botname = bot_inst_name(ctx->bot);

  snprintf(key, sizeof(key), "bot.%s.behavior.personality", botname);

  persona = kv_get_str(key);

  if(persona == NULL || persona[0] == '\0')
    persona = kv_get_str("plugin.chat.default_personality");
  if(persona == NULL)
    persona = "";

  snprintf(key, sizeof(key), "bot.%s.behavior.contract", botname);

  contract = kv_get_str(key);

  if(contract == NULL || contract[0] == '\0')
    contract = kv_get_str("plugin.chat.default_contract");
  if(contract == NULL)
    contract = "";

  // Unconditional clear so chatbot_ensure_interests re-parses even when
  // the persona name is unchanged (the author likely edited the
  // `interests:` block in place).
  pthread_rwlock_wrlock(&st->lock);
  snprintf(st->active_name, sizeof(st->active_name), "%s", persona);
  st->registered_persona[0] = '\0';
  pthread_rwlock_unlock(&st->lock);

  chatbot_ensure_interests(st);

  snprintf(buf, sizeof(buf),
      "refreshed '%s': personality='%s' contract='%s'",
      botname,
      persona[0]  ? persona  : "(none)",
      contract[0] ? contract : "(none)");
  cmd_reply(ctx, buf);
}

// Chat-kind filter: static storage, cmd_register keeps the pointer.
static const char *const chat_kind_filter[] = { "chat", NULL };

// NL hint for /hush. The chat plugin's NL bridge forwards the
// LLM-emitted slash line directly; any future translation from bare
// "hush <nick>" into the parent-scoped "/bot <name> hush <duration>"
// happens on the chat plugin side, not via a sponsor callback.
static const cmd_nl_slot_t chat_hush_slots[] = {
  { .name  = "nick",
    .type  = CMD_NL_ARG_NICK,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "duration",
    .type  = CMD_NL_ARG_DURATION,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t chat_hush_examples[] = {
  { .utterance  = "hush yourself for 5 minutes",
    .invocation = "/hush self 5m" },
  { .utterance  = "shut lessclam up for an hour",
    .invocation = "/hush lessclam 1h" },
};

static const cmd_nl_t chat_hush_nl = {
  .when          = "Someone asks you (or a specific nick) to be silent"
                   " for a duration.",
  .syntax        = "/hush <nick|self> <duration>",
  .slots         = chat_hush_slots,
  .slot_count    = (uint8_t)(sizeof(chat_hush_slots)
                             / sizeof(chat_hush_slots[0])),
  .examples      = chat_hush_examples,
  .example_count = (uint8_t)(sizeof(chat_hush_examples)
                             / sizeof(chat_hush_examples[0])),
  .dispatch_text = "bot $bot hush",
};

bool
chatbot_cmds_register(void)
{
  if(cmd_register("chat", "hush",
        "bot <name> hush <duration>",
        "Mute a chat bot's replies for a duration",
        "Suppresses all replies from the named bot for the given\n"
        "duration. Sets bot.<name>.behavior.mute_until to (now + duration).\n"
        "Auto-clears when the deadline passes, or manually via\n"
        "/set kv bot.<name>.behavior.mute_until 0.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_bot_hush, NULL, "bot", NULL,
        ad_bot_hush, (uint8_t)(sizeof(ad_bot_hush) / sizeof(ad_bot_hush[0])),
        chat_kind_filter, &chat_hush_nl) != SUCCESS)
    return(FAIL);

  if(chatbot_dossiersweep_cmd_register() != SUCCESS)
    goto fail_dossiersweep;

  if(chatbot_show_verbs_register() != SUCCESS)
    goto fail_show_verbs;

  if(cmd_register("chat", "refresh_prompts",
        "bot <name> refresh_prompts",
        "Re-sync a chat bot's cached personality + interests",
        "Personality body and output contract are read fresh from disk on\n"
        "every reply, so edits to those files already take effect without\n"
        "this command. Use this after editing the personality file's\n"
        "`interests:` frontmatter block, or any time you want to force\n"
        "bot.<name>.behavior.personality and bot.<name>.behavior.contract to be\n"
        "re-read from KV. Re-parses the interests block into the\n"
        "reactive-topic cache and re-registers topics with the\n"
        "acquisition engine.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_bot_refresh_prompts, NULL, "bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    goto fail_refresh_prompts;

  return(SUCCESS);

fail_refresh_prompts:
  // show verbs / dossiersweep left registered; cmd_unregister only
  // removes leaf defs and there's no per-child unregister helper here.

fail_show_verbs:
  // dossiersweep left registered; cmd_unregister only removes leaf
  // defs and there's no per-child unregister helper here.

fail_dossiersweep:
  cmd_unregister("hush");
  return(FAIL);
}

void
chatbot_cmds_unregister(void)
{
  // Names like "hush" may collide with other modules' commands at the
  // global cmd_unregister level (parent scope is not considered), so
  // we leave them registered -- matches core modules (memory, userns).
}
