// botmanager — MIT
// Dice misc plugin: rolls 1-5 polyhedral dice and says how it went (!dice).
#define DICE_INTERNAL
#include "dice.h"

#include <errno.h>
#include <stdlib.h>

#include "colors.h"
#include "util.h"

// The dice that exist. An allowlist, not a range: these are the solids
// in a dice bag, and a request for anything else is a typo rather than
// an exotic die.
static const uint8_t dice_sides_allowed[] = { 4, 6, 8, 12, 20 };

#define DICE_SIDES_ALLOWED_COUNT \
  (sizeof(dice_sides_allowed) / sizeof(dice_sides_allowed[0]))

// Flavour, for when the toy is speaking for itself — which is the
// default and the common case (include/bot.h §bot_persona_reply). These
// never name a number: one table has to sit behind a d4 and five d20s
// alike, and a line that guesses at the range ages badly the first time
// someone rolls the other one.
static const char *dice_flavour[] = {
  "The dice have spoken. They decline to elaborate.",
  "A perfectly ordinary result, which is most of them.",
  "Nobody is going to write a song about that one.",
  "The dice considered the alternatives and went with this.",
  "That's the number. The dice don't take questions.",
};

#define DICE_FLAVOUR_COUNT (sizeof(dice_flavour) / sizeof(dice_flavour[0]))

// The two bands worth their own line. Indexed by whether more than one
// die was in the cup: "every die came up 1" is a strange thing to say
// about one die.
static const char *dice_flavour_max[] = {
  "A natural maximum. Do that again with witnesses present.",
  "Every die came up maximum. Do that again with witnesses present.",
};

static const char *dice_flavour_min[] = {
  "A natural 1. The dice are not on your side today.",
  "Every die came up 1. The dice are not on your side today.",
};

// Widest faces rendering: five two-digit faces joined by " + ".
#define DICE_FACES_SZ  32
#define DICE_LINE_SZ   512

// Argument handling

// The boundary. argv is bytes from a human however well the command
// layer's CMD_ARG_DIGITS shaped them, and a `const char *` carries no
// proof of range, so the conversion happens once, here, and produces
// the only dice_spec_t any other function will see.
static bool
dice_arg_uint(const char *s, unsigned long *out)
{
  unsigned long  v;
  char          *end;

  if(s == NULL || s[0] == '\0')
    return(false);

  errno = 0;
  v     = strtoul(s, &end, 10);

  if(errno != 0 || end == s || *end != '\0')
    return(false);

  *out = v;

  return(true);
}

static bool
dice_parse(const cmd_args_t *parsed, dice_spec_t *out)
{
  unsigned long  sides = DICE_SIDES_DEFAULT;
  unsigned long  count = DICE_COUNT_DEFAULT;
  bool           known = false;
  size_t         i;

  if(parsed != NULL && parsed->argc > 0
      && !dice_arg_uint(parsed->argv[0], &sides))
    return(false);

  if(parsed != NULL && parsed->argc > 1
      && !dice_arg_uint(parsed->argv[1], &count))
    return(false);

  for(i = 0; i < DICE_SIDES_ALLOWED_COUNT; i++)
    if(sides == dice_sides_allowed[i])
      known = true;

  if(!known || count < DICE_COUNT_MIN || count > DICE_COUNT_MAX)
    return(false);

  *out = (dice_spec_t){ .sides = (uint8_t)sides, .count = (uint8_t)count };

  return(true);
}

// Rolling and rendering

static void
dice_roll(const dice_spec_t *spec, dice_roll_t *out)
{
  uint8_t i;

  *out = (dice_roll_t){ .sides   = spec->sides,
                        .count   = spec->count,
                        .all_max = true,
                        .all_min = true };

  for(i = 0; i < spec->count; i++)
  {
    uint8_t face = (uint8_t)(util_rand(spec->sides) + 1);

    out->face[i]  = face;
    out->total   += face;

    if(face != spec->sides)
      out->all_max = false;

    if(face != 1)
      out->all_min = false;
  }
}

static void
dice_render(const dice_roll_t *roll, char *out, size_t out_sz)
{
  const char *flavour;
  char        faces[DICE_FACES_SZ] = "";
  uint8_t     i;

  if(roll->all_max)
    flavour = dice_flavour_max[roll->count > 1];

  else if(roll->all_min)
    flavour = dice_flavour_min[roll->count > 1];

  else
    flavour = dice_flavour[util_rand(DICE_FLAVOUR_COUNT)];

  // A single die has nothing to add up, so it shows the total alone.
  if(roll->count == 1)
  {
    snprintf(out, out_sz,
        CLR_YELLOW "\xf0\x9f\x8e\xb2" CLR_RESET " d%u: "
        CLR_GREEN "%u" CLR_RESET " \xe2\x80\x94 %s",
        roll->sides, roll->total, flavour);
    return;
  }

  for(i = 0; i < roll->count; i++)
  {
    char one[8];

    snprintf(one, sizeof one, "%s%u", i > 0 ? " + " : "", roll->face[i]);
    strlcat(faces, one, sizeof faces);
  }

  snprintf(out, out_sz,
      CLR_YELLOW "\xf0\x9f\x8e\xb2" CLR_RESET " %ud%u: %s = "
      CLR_GREEN "%u" CLR_RESET " \xe2\x80\x94 %s",
      roll->count, roll->sides, faces, roll->total, flavour);
}

// dice

static void
dice_cmd(const cmd_ctx_t *ctx)
{
  dice_spec_t  spec;
  dice_roll_t  roll;
  char         line[DICE_LINE_SZ];
  char         instruction[256];

  if(!dice_parse(ctx->parsed, &spec))
  {
    cmd_reply(ctx, "Sides must be one of 4, 6, 8, 12 or 20, and you may"
        " roll 1 to 5 of them. Try " CLR_BOLD "!dice 6 2" CLR_RESET ".");
    return;
  }

  dice_roll(&spec, &roll);
  dice_render(&roll, line, sizeof line);

  // What happened, never how to say it — the fixed half of the prompt
  // belongs to the persona_reply slot. The callee copies this before it
  // returns (plugins/bot/chat/persona_reply.c), so the stack buffer is
  // the whole lifetime it needs.
  snprintf(instruction, sizeof instruction,
      "Someone just rolled %u %u-sided %s and the total is %u."
      " Announce the result, in your own voice.",
      roll.count, roll.sides, roll.count == 1 ? "die" : "dice",
      roll.total);

  if(kv_get_uint(DICE_KV_IN_VOICE) != 0
      && bot_persona_reply(ctx, instruction, line))
    return;

  cmd_reply(ctx, line);
}

// Plugin lifecycle

static const plugin_kv_entry_t dice_kv_schema[] = {
  { DICE_KV_IN_VOICE, KV_BOOL, "false",
    "Answer !dice in the bot's persona voice instead of its own flavour"
    " table. Costs one LLM call per roll and needs a bot whose"
    " conversational half is on (bot.<name>.behavior.chat.enabled) — a"
    " command bot ignores it and keeps the table." },
};

static const cmd_arg_desc_t dice_args[] = {
  { "numsides", CMD_ARG_DIGITS, CMD_ARG_OPTIONAL, 2, NULL },
  { "numdice",  CMD_ARG_DIGITS, CMD_ARG_OPTIONAL, 1, NULL },
};

static const cmd_nl_slot_t dice_slots[] = {
  { .name = "numsides", .type = CMD_NL_ARG_INT, .flags = CMD_NL_SLOT_OPTIONAL },
  { .name = "numdice",  .type = CMD_NL_ARG_INT, .flags = CMD_NL_SLOT_OPTIONAL },
};

static const cmd_nl_example_t dice_examples[] = {
  { .utterance = "roll a die", .invocation = "/dice" },
  { .utterance = "roll 2d6", .invocation = "/dice 6 2" },
  { .utterance = "give me a d20", .invocation = "/dice 20" },
};

static const cmd_nl_t dice_nl = {
  .when          = "User wants dice rolled — a d20 by default, or a named"
                   " die size and how many of them.",
  .syntax        = "/dice [numsides [numdice]]",
  .slots         = dice_slots,
  .slot_count    = (uint8_t)(sizeof(dice_slots) / sizeof(dice_slots[0])),
  .examples      = dice_examples,
  .example_count = (uint8_t)(sizeof(dice_examples)
      / sizeof(dice_examples[0])),
};

static bool
dice_init(void)
{
  if(cmd_register(DICE_CTX, "dice",
      "dice [numsides [numdice]]",
      "Roll dice",
      "Roll dice and hear how it went.\n"
      "numsides is one of 4, 6, 8, 12 or 20 — 20 if you don't say.\n"
      "numdice is 1 to 5 — 1 if you don't say.\n"
      "\n"
      "Examples:\n"
      "  !dice        one 20-sided die\n"
      "  !dice 6      one 6-sided die\n"
      "  !dice 6 3    three 6-sided dice, and their total",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, dice_cmd, NULL, NULL,
      "d", dice_args,
      (uint8_t)(sizeof(dice_args) / sizeof(dice_args[0])),
      NULL, &dice_nl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static void
dice_deinit(void)
{
  cmd_unregister_path("dice");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "dice",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "dice",
  .provides        = { { .name = "misc_dice" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = dice_kv_schema,
  .kv_schema_count = sizeof(dice_kv_schema) / sizeof(dice_kv_schema[0]),
  .init            = dice_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = dice_deinit,
  .ext             = NULL,
};
