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

// The killing blow — strictly after the death line has gone out, never
// before: a KILL severs the connection, and nothing sent afterwards ever
// reaches the fallen (same law as the pit, see plugins/feature/attack).
//
// Deliberately does not settle for a lesser removal. `nick` is only ever
// worth pulling a trigger on when it named a real IRC member (`ctx->msg`
// present, non-empty channel), and the request this answers to is
// specific: a bot that merely holds channel ops has no business KICKing
// a player over a game they opted into by typing !roulette — only a bot
// with genuine operator status, on IRC, gets to make it real. Anything
// short of METHOD_EJECT_SERVER is left alone.
static method_eject_t
roulette_eject(const cmd_ctx_t *ctx, const char *nick)
{
  method_eject_t force;

  if(ctx->msg == NULL || ctx->msg->channel[0] == '\0' || nick == NULL
      || nick[0] == '\0')
    return(METHOD_EJECT_NONE);

  force = method_eject_probe(ctx->msg->inst, ctx->msg->channel, nick);

  if(force != METHOD_EJECT_SERVER)
    return(METHOD_EJECT_NONE);

  method_eject(ctx->msg->inst, ctx->msg->channel, nick, force,
      "lost at Russian roulette");

  clam(CLAM_INFO, ROULETTE_CTX, "%s lost the spin and was KILLed", nick);

  return(force);
}

static void
roulette_cmd(const cmd_ctx_t *ctx)
{
  const char *who;
  char        line[256];
  bool        dead;

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

    snprintf(body, sizeof(body),
        roulette_bang[util_rand(ROULETTE_BANGS)], who);
    snprintf(line, sizeof(line),
        CLR_RED CLR_BOLD "\xf0\x9f\x92\xa5 %s" CLR_RESET, body);
  }

  else
    snprintf(line, sizeof(line),
        CLR_GRAY "\xf0\x9f\x94\xab %s" CLR_RESET,
        roulette_click[util_rand(ROULETTE_CLICKS)]);

  cmd_reply(ctx, line);

  // The door opens only after the line above has gone out, and only for
  // the nick that actually pulled the trigger — never the username
  // fallback, which is not a valid eject target on any method.
  if(dead && ctx->msg != NULL)
    roulette_eject(ctx, ctx->msg->nickname);
}

// Plugin lifecycle

static bool
roulette_init(void)
{
  if(cmd_register(ROULETTE_CTX, "roulette",
      "roulette",
      "Take your turn at Russian roulette",
      "Spin the cylinder and squeeze the trigger. One of six chambers\n"
      "holds a live round; the other five are empty. Every pull is an\n"
      "independent one-in-six gamble \xe2\x80\x94 the revolver keeps no memory.\n"
      "\n"
      "Example:\n"
      "  !roulette",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, roulette_cmd, NULL, NULL,
      "rr", NULL, 0, NULL, NULL) != SUCCESS)
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
  .requires        = { { .name = "method_text" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = roulette_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = roulette_deinit,
  .ext             = NULL,
};
