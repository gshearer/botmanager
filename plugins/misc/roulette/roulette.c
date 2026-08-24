// botmanager — MIT
// Roulette misc plugin: a stateless game of Russian roulette (!roulette).
#define ROULETTE_INTERNAL
#include "roulette.h"

#include "colors.h"
#include "method.h"
#include "util.h"

// Every pull spins a fresh cylinder: one live round seated at random
// among ROULETTE_CHAMBERS, then a single trigger squeeze. There is no
// carried chamber index and no per-channel state — reply and forget, as
// a misc toy must. The old quotebot roulette KICK/KILLed the loser, and
// that survives here too: a misc plugin depends on core only, but the
// eject primitives (method_eject_probe / method_eject, include/method.h)
// are core, not IRC. Where the bot holds no operator status the blow is
// pure theatre; where it does, the loser leaves the network for real.

// Flavour tables

static const char *roulette_click[] = {
  "*click*",
  "*click* \xe2\x80\xa6 the hammer falls on an empty chamber.",
  "*click* \xe2\x80\x94 not this time.",
  "*click* \xe2\x80\xa6 a bead of sweat, and nothing more.",
  "*click* \xe2\x80\x94 the cylinder was merciful.",
  "*click* \xe2\x80\xa6 five chambers left. Feeling lucky?",
};

#define ROULETTE_CLICKS (sizeof(roulette_click) / sizeof(roulette_click[0]))

static const char *roulette_bang[] = {
  "BANG! %s crumples to the floor. The cylinder spins on.",
  "BANG! The round finds %s. Silence, then a slow drip.",
  "BANG! %s drew the live chamber. Someone reload for the next fool.",
  "BANG! %s bet against one-in-six and lost everything.",
  "BANG! %s. The revolver was hungry after all.",
};

#define ROULETTE_BANGS (sizeof(roulette_bang) / sizeof(roulette_bang[0]))

// Whether this pull has a real removal waiting behind it, asked before
// a word goes out because the answer decides which register the line is
// written in.
//
// Deliberately does not settle for a lesser removal. The trigger-puller
// is only worth pulling on when they named a real IRC member (`ctx->msg`
// present, non-empty channel and nickname), and the request this answers
// to is specific: a bot that merely holds channel ops has no business
// KICKing a player over a game they opted into by typing !roulette —
// only a bot with genuine operator status, on IRC, gets to make it real.
// Anything short of METHOD_EJECT_SERVER is left alone.
static method_eject_t
roulette_eject_probe(const cmd_ctx_t *ctx)
{
  method_eject_t force;

  if(ctx->msg == NULL || ctx->msg->channel[0] == '\0'
      || ctx->msg->nickname[0] == '\0')
    return(METHOD_EJECT_NONE);

  force = method_eject_probe(ctx->msg->inst, ctx->msg->channel,
      ctx->msg->nickname);

  return(force == METHOD_EJECT_SERVER ? force : METHOD_EJECT_NONE);
}

// The killing blow — strictly after the death line has gone out, never
// before: a KILL severs the connection, and nothing sent afterwards ever
// reaches the fallen (same law as the pit, see plugins/feature/attack).
// Only ever reached with a `force` the probe above already vouched for,
// which is what leaves the guards there and not here.
static void
roulette_eject(const cmd_ctx_t *ctx, method_eject_t force)
{
  method_eject(ctx->msg->inst, ctx->msg->channel, ctx->msg->nickname,
      force, "lost at Russian roulette");

  clam(CLAM_INFO, ROULETTE_CTX, "%s lost the spin and was KILLed",
      ctx->msg->nickname);
}

static void
roulette_cmd(const cmd_ctx_t *ctx)
{
  const char     *who;
  method_eject_t  force = METHOD_EJECT_NONE;
  char            line[256];
  char            instruction[256];
  bool            dead;

  // Identify the trigger-puller for the death line; nickname when the
  // method carries one (IRC), else the authenticated username, else a
  // neutral stand-in so the reply never renders a NULL.
  who = (ctx->msg != NULL && ctx->msg->nickname[0] != '\0')
      ? ctx->msg->nickname
      : (ctx->username != NULL && ctx->username[0] != '\0'
          ? ctx->username : "The gambler");

  dead = (util_rand(ROULETTE_CHAMBERS) == 0);

  if(dead)
  {
    char body[192];

    // Asked before a word goes out, because the answer decides which
    // register the line is written in. The probe only looks.
    force = roulette_eject_probe(ctx);

    snprintf(body, sizeof(body),
        roulette_bang[util_rand(ROULETTE_BANGS)], who);
    snprintf(line, sizeof(line),
        CLR_RED CLR_BOLD "\xf0\x9f\x92\xa5 %s" CLR_RESET, body);
  }

  else
    snprintf(line, sizeof(line),
        CLR_GRAY "\xf0\x9f\x94\xab %s" CLR_RESET,
        roulette_click[util_rand(ROULETTE_CLICKS)]);

  // What happened, never how to say it. The callee copies this before
  // it returns (plugins/bot/chat/persona_reply.c), so the stack buffer
  // is the whole lifetime it needs.
  snprintf(instruction, sizeof instruction,
      "%s just took a turn at Russian roulette and %s. Call it, in your"
      " own voice.", who,
      dead ? "the live round was under the hammer"
           : "the chamber came up empty");

  // The voice is the one thing a real KILL cannot afford to wait for.
  // The mind answers seconds from now — bot_persona_reply is a submit,
  // not a call — and the blow may not be held that long: it is the
  // reply going out first that is the whole ordering law above. So a
  // pull with a genuine removal behind it keeps its own line and stays
  // in the local register. Where the bang is pure theatre nobody is
  // going anywhere, and the mind is welcome to take its time.
  if(force == METHOD_EJECT_NONE
      && kv_get_uint(ROULETTE_KV_IN_VOICE) != 0
      && bot_persona_reply(ctx, instruction, line))
    return;

  cmd_reply(ctx, line);

  if(force != METHOD_EJECT_NONE)
    roulette_eject(ctx, force);
}

// Plugin lifecycle

static const plugin_kv_entry_t roulette_kv_schema[] = {
  { ROULETTE_KV_IN_VOICE, KV_BOOL, "false",
    "Answer !roulette in the bot's persona voice instead of its own"
    " flavour table. Costs one LLM call per pull and needs a bot whose"
    " conversational half is on (bot.<name>.behavior.chat.enabled) — a"
    " command bot ignores it and keeps the table. A pull that ends in a"
    " real KILL keeps the table regardless: the blow cannot wait on the"
    " model, and it must not land before the line." },
};

static const cmd_decl_t roulette_decl = {
  .module      = ROULETTE_CTX,
  .name        = "roulette",
  .usage       = "roulette",
  .description = "Take your turn at Russian roulette",
  .help_long   =
      "Spin the cylinder and squeeze the trigger. One of six chambers\n"
      "holds a live round; the other five are empty. Every pull is an\n"
      "independent one-in-six gamble \xe2\x80\x94 the revolver keeps no memory.\n"
      "\n"
      "Example:\n"
      "  !roulette",
  .group       = "everyone",
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = roulette_cmd,
  .abbrev      = "rr",
};

static bool
roulette_init(void)
{
  if(cmd_register(&roulette_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static void
roulette_deinit(void)
{
  cmd_unregister_path("roulette");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "roulette",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "roulette",
  .provides        = { { .name = "misc_roulette" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = roulette_kv_schema,
  .kv_schema_count = sizeof(roulette_kv_schema)
      / sizeof(roulette_kv_schema[0]),
  .init            = roulette_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = roulette_deinit,
  .ext             = NULL,
};
