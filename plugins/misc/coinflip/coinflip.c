// botmanager — MIT
// Coinflip misc plugin: a one-bit oracle with opinions (!coinflip).
#define COINFLIP_INTERNAL
#include "coinflip.h"

#include "colors.h"
#include "util.h"

// Flavour tables. One row per face, picked at random — the coin decides
// which row, the die decides which line, and neither is remembered.
//
// This is the default answer and the good one. The persona path below
// replaces it when the operator asks for that; it never improves on it
// unattended.

static const char *coinflip_heads[] = {
  "Heads. The coin thought about it on the way up and committed on the"
    " way down.",
  "Heads \xe2\x80\x94 and it landed flat enough to bore a spirit level.",
  "Heads. Physics had a lot of options and picked the smug one.",
  "Heads, by roughly one rotation. Don't ask me to do that again.",
  "Heads. The universe declines to elaborate.",
};

static const char *coinflip_tails[] = {
  "Tails. The coin spent nine rotations deciding and still looks unsure.",
  "Tails \xe2\x80\x94 the side that never gets its picture taken.",
  "Tails. It bounced twice, considered the edge, and lost its nerve.",
  "Tails, obviously. It was always going to be tails. (It was not always"
    " going to be tails.)",
  "Tails. Somewhere a statistician is writing this down.",
};

#define COINFLIP_HEADS_COUNT \
  (sizeof(coinflip_heads) / sizeof(coinflip_heads[0]))
#define COINFLIP_TAILS_COUNT \
  (sizeof(coinflip_tails) / sizeof(coinflip_tails[0]))

// coinflip

static void
coinflip_cmd(const cmd_ctx_t *ctx)
{
  bool        heads = (util_rand(2) == 0);
  const char *face  = heads
      ? coinflip_heads[util_rand(COINFLIP_HEADS_COUNT)]
      : coinflip_tails[util_rand(COINFLIP_TAILS_COUNT)];
  char        line[512];

  snprintf(line, sizeof(line),
      CLR_YELLOW "\xf0\x9f\xaa\x99" CLR_RESET " %s", face);

  // The whole cost of borrowing the bot's voice. A true return means
  // the mind owes the reply and will make it — including on its own
  // failure, where it sends this very line. False means it declined
  // (no persona, conversational half off, nothing registered) and the
  // answer is ours to give, which is also the default with the knob
  // unset.
  if(kv_get_uint(COINFLIP_KV_IN_VOICE) != 0
      && bot_persona_reply(ctx, COINFLIP_INSTRUCTION, line))
    return;

  cmd_reply(ctx, line);
}

// Plugin lifecycle

static const plugin_kv_entry_t coinflip_kv_schema[] = {
  { COINFLIP_KV_IN_VOICE, KV_BOOL, "false",
    "Answer !coinflip in the bot's persona voice instead of its own"
    " flavour table. Costs one LLM call per flip and needs a bot whose"
    " conversational half is on (bot.<name>.behavior.chat.enabled) — a"
    " command bot ignores it and keeps the table." },
};

static const cmd_nl_example_t coinflip_examples[] = {
  { .utterance = "flip a coin", .invocation = "/coinflip" },
  { .utterance = "heads or tails?", .invocation = "/coinflip" },
  { .utterance = "toss a coin for me", .invocation = "/coinflip" },
};

static const cmd_nl_t coinflip_nl = {
  .when          = "User wants a coin flipped, or a two-way decision"
                   " settled at random — heads or tails.",
  .syntax        = "/coinflip",
  .slots         = NULL,
  .slot_count    = 0,
  .examples      = coinflip_examples,
  .example_count = (uint8_t)(sizeof(coinflip_examples)
      / sizeof(coinflip_examples[0])),
};

static bool
coinflip_init(void)
{
  if(cmd_register(COINFLIP_CTX, "coinflip",
      "coinflip",
      "Flip a coin",
      "Flip a fair coin and hear about it.\n"
      "Takes no arguments; the coin has already made up its mind.",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, coinflip_cmd, NULL, NULL,
      "cf", NULL, 0, NULL, &coinflip_nl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static void
coinflip_deinit(void)
{
  cmd_unregister_path("coinflip");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "coinflip",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "coinflip",
  .provides        = { { .name = "misc_coinflip" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = coinflip_kv_schema,
  .kv_schema_count = sizeof(coinflip_kv_schema)
      / sizeof(coinflip_kv_schema[0]),
  .init            = coinflip_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = coinflip_deinit,
  .ext             = NULL,
};
