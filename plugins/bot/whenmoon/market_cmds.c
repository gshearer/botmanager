// botmanager — MIT
// whenmoon /bot <name> market start|stop verbs.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "market_cmds.h"
#include "dl_commands.h"

#include "bot.h"
#include "cmd.h"
#include "common.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Shared with dl_commands.c — every whenmoon verb is kind-filtered to
// ensure non-whenmoon bots don't see these verbs under /bot or /show.
static const char *const wm_market_kind_filter[] = { "whenmoon", NULL };

// ------------------------------------------------------------------ //
// Handlers                                                            //
// ------------------------------------------------------------------ //

static bool
wm_market_parse_pair_arg(const cmd_ctx_t *ctx, const char *usage,
    char *exch,   size_t exch_cap,
    char *base,   size_t base_cap,
    char *quote,  size_t quote_cap,
    char *symbol, size_t sym_cap)
{
  const char *p;
  char        pair[64] = {0};

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair, sizeof(pair)))
  {
    cmd_reply(ctx, usage);
    return(FAIL);
  }

  if(wm_dl_parse_pair(pair, exch, exch_cap, base, base_cap,
         quote, quote_cap, symbol, sym_cap) != SUCCESS)
  {
    cmd_reply(ctx, "bad pair (expected <exch>:<asset>:<cur>)");
    return(FAIL);
  }

  return(SUCCESS);
}

static void
wm_market_cmd_start(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  char              exch[32];
  char              base[16];
  char              quote[16];
  char              symbol[COINBASE_PRODUCT_ID_SZ];
  char              err[128] = {0};
  char              reply[192];

  if(ctx->bot == NULL)
  {
    cmd_reply(ctx, "no bot context");
    return;
  }

  st = bot_get_handle(ctx->bot);

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  if(wm_market_parse_pair_arg(ctx,
         "usage: /bot <name> market start <exch>:<asset>:<cur>",
         exch,   sizeof(exch),
         base,   sizeof(base),
         quote,  sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
    return;

  if(wm_market_add(st, exch, base, quote, symbol,
         true, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "market start failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  snprintf(reply, sizeof(reply),
      "market %s started (live + persisted)", symbol);
  cmd_reply(ctx, reply);
}

static void
wm_market_cmd_stop(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  char              exch[32];
  char              base[16];
  char              quote[16];
  char              symbol[COINBASE_PRODUCT_ID_SZ];
  char              err[128] = {0};
  char              reply[192];
  bool              was_present = false;

  if(ctx->bot == NULL)
  {
    cmd_reply(ctx, "no bot context");
    return;
  }

  st = bot_get_handle(ctx->bot);

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  if(wm_market_parse_pair_arg(ctx,
         "usage: /bot <name> market stop <exch>:<asset>:<cur>",
         exch,   sizeof(exch),
         base,   sizeof(base),
         quote,  sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
    return;

  if(wm_market_remove(st, symbol, true, &was_present,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "market stop failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  if(was_present)
    snprintf(reply, sizeof(reply),
        "market %s stopped (dropped from live + DB)", symbol);
  else
    snprintf(reply, sizeof(reply),
        "market %s not running (nothing to stop)", symbol);

  cmd_reply(ctx, reply);
}

// ------------------------------------------------------------------ //
// Parent walker                                                       //
// ------------------------------------------------------------------ //

static void
wm_market_parent_cb(const cmd_ctx_t *ctx)
{
  const cmd_def_t *bot_root;
  const cmd_def_t *market;
  const cmd_def_t *child;
  const char      *p;
  char             verb[CMD_NAME_SZ] = {0};
  cmd_ctx_t        sctx;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, verb, sizeof(verb)))
  {
    cmd_reply(ctx, "usage: /bot <name> market <start|stop> <pair>");
    return;
  }

  bot_root = cmd_find("bot");
  market   = bot_root != NULL
      ? cmd_find_child_for_kind(bot_root, "market", "whenmoon") : NULL;

  if(market == NULL)
  {
    cmd_reply(ctx, "market parent not registered");
    return;
  }

  child = cmd_find_child_for_kind(market, verb, "whenmoon");

  if(child == NULL)
  {
    char buf[128];

    snprintf(buf, sizeof(buf), "unknown market verb '%s'", verb);
    cmd_reply(ctx, buf);
    return;
  }

  sctx = *ctx;
  sctx.args   = p;
  sctx.parsed = NULL;
  cmd_invoke(child, &sctx);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

bool
wm_market_register_verbs(void)
{
  if(cmd_register("whenmoon", "market",
        "bot <name> market <start|stop> <exch>:<asset>:<cur>",
        "Add or remove a live market assignment for this bot."
        " Starts: WS subscribe + candle backfill + catch-up download."
        " Stops: unsubscribe + drop DB row.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_parent_cb, NULL, "bot", NULL,
        NULL, 0, wm_market_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "start",
        "bot <name> market start <exch>:<asset>:<cur>",
        "Start a market: WS subscribe, backfill the live ring, enqueue"
        " a catch-up candles download (if the local history is populated),"
        " and persist the assignment so it survives daemon restarts.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_start, NULL, "bot/market", NULL,
        NULL, 0, wm_market_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "stop",
        "bot <name> market stop <exch>:<asset>:<cur>",
        "Stop a market: WS unsubscribe, drop from the live set, and"
        " DELETE the wm_bot_market row so it does not resume on next"
        " restart.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_stop, NULL, "bot/market", NULL,
        NULL, 0, wm_market_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
