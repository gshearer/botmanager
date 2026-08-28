// botmanager — MIT
// /whenmoon order verbs (WM-OR-1 part 2).
//
// Ad-hoc order surface against any registered exchange. Routes through
// the feature_exchange capability layer (`exchange_place_order_async`,
// `exchange_cancel_order_async`); the operator never names a strategy
// or a market book — these orders are unrelated to the market
// subsystem and never touch any whenmoon position state.
//
// Order types: `market`, `limit`, and `maker` — a limit carrying
// post_only, which the venue rejects rather than crossing. See the
// buy/sell help text for why that rejection is the feature (WM-MAKER-1).
//
// Async reply pattern: the command callback returns before the
// exchange responds, so cmd_ctx_t is gone by the time the typed
// callback fires. The handler captures the reply target into a
// heap-allocated wm_order_async_ctx_t and hands it to the async API
// as `user`. The callback uses method_send directly — bypassing
// cmd_reply, which requires a live ctx.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "dl_commands.h"
#include "userns.h"
#include "exchange_api.h"
#include "method.h"
#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_ORDER_TARGET_SZ   128
#define WM_ORDER_LABEL_SZ     96

// Heap-owned reply target captured at handler entry. The instance is
// captured by NAME: the order round trip outlives the dispatching turn,
// and a `/plugin reload irc` inside it frees whatever the handler saw
// (method.h §method_msg_t).
typedef struct
{
  char inst_name[METHOD_NAME_SZ];
  char target[WM_ORDER_TARGET_SZ];
  char label[WM_ORDER_LABEL_SZ];   // e.g. "buy coinbase-btc-usd"
} wm_order_async_ctx_t;

static wm_order_async_ctx_t *
wm_order_async_ctx_new(const cmd_ctx_t *ctx, const char *label)
{
  wm_order_async_ctx_t *ac;
  const char           *target;

  if(ctx == NULL || ctx->msg == NULL || ctx->msg->inst_name[0] == '\0')
    return(NULL);

  ac = mem_alloc(WHENMOON_CTX, "order.async", sizeof(*ac));

  memset(ac, 0, sizeof(*ac));
  strlcpy(ac->inst_name, ctx->msg->inst_name, sizeof ac->inst_name);

  target = ctx->msg->channel[0] != '\0'
      ? ctx->msg->channel
      : ctx->msg->sender;

  snprintf(ac->target, sizeof(ac->target), "%s",
      target != NULL ? target : "");
  snprintf(ac->label, sizeof(ac->label), "%s",
      label != NULL ? label : "");

  return(ac);
}

static void
wm_order_async_send(wm_order_async_ctx_t *ac, const char *text)
{
  method_inst_t *inst;

  if(ac == NULL || ac->inst_name[0] == '\0' || ac->target[0] == '\0'
      || text == NULL)
    return;

  inst = method_find(ac->inst_name);

  if(inst == NULL)
    return;

  method_send(inst, ac->target, text);
  method_release(inst);
}

// ------------------------------------------------------------------ //
// /whenmoon order buy / sell                                          //
// ------------------------------------------------------------------ //

static void
wm_order_place_done(const exchange_order_result_t *res, void *user)
{
  wm_order_async_ctx_t *ac = user;
  char                  reply[256];

  if(ac == NULL)
    return;

  if(res->err[0] != '\0')
  {
    snprintf(reply, sizeof(reply),
        "%s: error: %s", ac->label, res->err);
  }
  else
  {
    // post_only is echoed from the VENUE's decoded order, not from
    // what we sent: an accepted maker order is the only proof the
    // flag reached the book rather than being dropped en route.
    snprintf(reply, sizeof(reply),
        "%s: placed: %s %s %s qty=%.8g px=%.8g%s",
        ac->label,
        res->order.order_id,
        res->order.product_id,
        res->order.side,
        res->order.size,
        res->order.price,
        res->order.post_only ? " post_only" : "");
  }

  // Log it as well as reply. wm_order_async_send routes through a
  // method instance, so a command issued from botmanctl — or one whose
  // instance went away during the round trip — silently loses the only
  // account the venue ever gives of what happened to real money. The
  // driver does not log a create failure either (it is delivered into
  // `res->err` and nowhere else), so without this line a rejection is
  // recoverable from nothing.
  clam(res->err[0] != '\0' ? CLAM_WARN : CLAM_INFO, WHENMOON_CTX,
      "%s", reply);

  wm_order_async_send(ac, reply);
  mem_free(ac);
}

static void
wm_order_cmd_buysell(const cmd_ctx_t *ctx, const char *side)
{
  const char *p;
  char        id_tok[64]                       = {0};
  char        type_tok[16]                     = {0};
  char        qty_tok[32]                      = {0};
  char        px_tok[32]                       = {0};
  char        exch[32]                         = {0};
  char        base[16]                         = {0};
  char        quote[16]                        = {0};
  char        symbol[EXCHANGE_PRODUCT_ID_SZ]   = {0};
  char        label[WM_ORDER_LABEL_SZ];
  char        usage[160];
  exchange_place_order_req_t req;
  wm_order_async_ctx_t      *ac;
  bool                       is_market;
  bool                       post_only = false;
  double                     qty;
  double                     price = 0.0;

  snprintf(usage, sizeof(usage),
      "usage: /whenmoon order %s <exch>-<base>-<quote>"
      " <limit|maker|market> <qty> [<price>]", side);

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok,   sizeof(id_tok))   ||
     !wm_dl_next_token(&p, type_tok, sizeof(type_tok)) ||
     !wm_dl_next_token(&p, qty_tok,  sizeof(qty_tok)))
  {
    cmd_reply(ctx, usage);
    return;
  }

  if(wm_market_parse_id(id_tok, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx,
        "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  wm_market_wire_symbol(base, quote, symbol, sizeof(symbol));

  if(symbol[0] == '\0')
  {
    cmd_reply(ctx, "market id too long");
    return;
  }

  // `maker` is a limit order carrying post_only, and the distinction is
  // the whole reason this slot has three values instead of two: a limit
  // priced at or through the opposing side CROSSES and pays taker — 120
  // bps against 60 on this venue. post_only asks the book to reject such
  // an order instead of filling it, so `maker` is the type to reach for
  // whenever paying taker would be worse than not trading at all.
  if(strcmp(type_tok, "limit") == 0 || strcmp(type_tok, "maker") == 0)
  {
    is_market = false;
    post_only = (strcmp(type_tok, "maker") == 0);

    if(!wm_dl_next_token(&p, px_tok, sizeof(px_tok)))
    {
      cmd_reply(ctx, "limit and maker orders require a price");
      return;
    }

    price = strtod(px_tok, NULL);

    if(price <= 0.0)
    {
      cmd_reply(ctx, "price must be > 0");
      return;
    }
  }
  else if(strcmp(type_tok, "market") == 0)
  {
    is_market = true;

    if(wm_dl_next_token(&p, px_tok, sizeof(px_tok)))
    {
      cmd_reply(ctx, "market orders do not accept a price");
      return;
    }
  }
  else
  {
    cmd_reply(ctx, "type must be 'limit', 'maker' or 'market'");
    return;
  }

  qty = strtod(qty_tok, NULL);

  if(qty <= 0.0)
  {
    cmd_reply(ctx, "qty must be > 0");
    return;
  }

  memset(&req, 0, sizeof(req));
  snprintf(req.product_id, sizeof(req.product_id), "%s", symbol);
  snprintf(req.side,       sizeof(req.side),       "%s", side);
  snprintf(req.type,       sizeof(req.type),       "%s",
      is_market ? "market" : "limit");

  if(is_market)
  {
    // Market-buy uses quote-ccy notional (`funds`) by Coinbase
    // convention; market-sell uses base-ccy size. Mixing the two
    // is rejected protocol-side.
    if(strcmp(side, "buy") == 0)
      req.funds = qty;
    else
      req.size  = qty;
  }
  else
  {
    snprintf(req.tif, sizeof(req.tif), "GTC");
    req.size      = qty;
    req.price     = price;
    req.post_only = post_only;
  }

  snprintf(label, sizeof(label), "order %s %s", side, id_tok);

  ac = wm_order_async_ctx_new(ctx, label);

  if(ac == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  if(exchange_place_order_async(exch, &req,
        wm_order_place_done, ac) != ASYNC_AIRBORNE)
  {
    // exchange_place_order_async fired wm_order_place_done with
    // the err already — and that callback freed `ac`. Do not
    // touch `ac` from here.
    return;
  }
}

static void
wm_order_cmd_buy(const cmd_ctx_t *ctx)
{
  wm_order_cmd_buysell(ctx, "buy");
}

static void
wm_order_cmd_sell(const cmd_ctx_t *ctx)
{
  wm_order_cmd_buysell(ctx, "sell");
}

// ------------------------------------------------------------------ //
// /whenmoon order cancel                                              //
// ------------------------------------------------------------------ //

static void
wm_order_cancel_done(const exchange_order_result_t *res, void *user)
{
  wm_order_async_ctx_t *ac = user;
  char                  reply[256];

  if(ac == NULL)
    return;

  if(res->err[0] != '\0')
  {
    snprintf(reply, sizeof(reply),
        "%s: error: %s", ac->label, res->err);
  }
  else
  {
    // Cancel ack typically only echoes order_id; leave other
    // fields blank in the reply.
    snprintf(reply, sizeof(reply),
        "%s: cancelled: %s", ac->label,
        res->order.order_id[0] != '\0'
            ? res->order.order_id
            : "(ack)");
  }

  clam(res->err[0] != '\0' ? CLAM_WARN : CLAM_INFO, WHENMOON_CTX,
      "%s", reply);

  wm_order_async_send(ac, reply);
  mem_free(ac);
}

static void
wm_order_cmd_cancel(const cmd_ctx_t *ctx)
{
  const char           *p;
  char                  exch_tok[32]                  = {0};
  char                  oid_tok[EXCHANGE_ORDER_ID_SZ] = {0};
  char                  label[WM_ORDER_LABEL_SZ];
  wm_order_async_ctx_t *ac;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)) ||
     !wm_dl_next_token(&p, oid_tok,  sizeof(oid_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon order cancel <exchange> <order-id>");
    return;
  }

  snprintf(label, sizeof(label), "order cancel %s/%s",
      exch_tok, oid_tok);

  ac = wm_order_async_ctx_new(ctx, label);

  if(ac == NULL)
  {
    cmd_reply(ctx, "out of memory");
    return;
  }

  if(exchange_cancel_order_async(exch_tok, oid_tok,
        wm_order_cancel_done, ac) != ASYNC_AIRBORNE)
  {
    // wm_order_cancel_done fired with err and freed `ac`.
    return;
  }
}

// ------------------------------------------------------------------ //
// /whenmoon order parent (help shell)                                 //
// ------------------------------------------------------------------ //

static void
wm_order_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "/whenmoon order: subcommands: buy, sell, cancel."
      " See /help whenmoon order <verb> for details.");
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_decl_t whenmoon_order_decl = {
  .module      = "whenmoon",
  .name        = "order",
  .usage       = "whenmoon order <verb> ...",
  .description = "Ad-hoc order management against any registered exchange.",
  .help_long   = "Subcommands: buy <market_id> <limit|maker|market> <qty>"
                 " [<price>], sell <market_id> <limit|maker|market> <qty>"
                 " [<price>], cancel <exchange> <order_id>.\n"
                 "Orders here are independent of the whenmoon market"
                 " subsystem — no book, no strategy, no PnL attribution.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_order_parent_cb,
  .parent_path = "whenmoon",
  .abbrev      = "or",
};

static const cmd_decl_t whenmoon_order_buy_decl = {
  .module      = "whenmoon",
  .name        = "buy",
  .usage       = "whenmoon order buy <exch>-<base>-<quote>"
                 " <limit|maker|market> <qty> [<price>]",
  .description = "Submit a buy order against the named exchange.",
  .help_long   = "limit and maker orders require a <price>; market orders"
                 " do not. Market-buys interpret <qty> as quote-currency"
                 " notional (e.g. USD); market-sells, limits and makers"
                 " interpret <qty> as base-currency size.\n"
                 "'maker' is a limit order submitted post_only: the venue REJECTS it rather than filling it if it would cross, which is what buys maker fees (60 bps/side here) instead of taker (120). A rejection is the flag working, not an error — reprice and resubmit.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_order_cmd_buy,
  .parent_path = "whenmoon/order",
};

static const cmd_decl_t whenmoon_order_sell_decl = {
  .module      = "whenmoon",
  .name        = "sell",
  .usage       = "whenmoon order sell <exch>-<base>-<quote>"
                 " <limit|maker|market> <qty> [<price>]",
  .description = "Submit a sell order against the named exchange.",
  .help_long   = "Symmetric to /whenmoon order buy. <qty> is base-currency"
                 " size in every mode (market, limit and maker alike).\n"
                 "'maker' is a limit order submitted post_only: the venue REJECTS it rather than filling it if it would cross, which is what buys maker fees (60 bps/side here) instead of taker (120). A rejection is the flag working, not an error — reprice and resubmit.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_order_cmd_sell,
  .parent_path = "whenmoon/order",
};

static const cmd_decl_t whenmoon_order_cancel_decl = {
  .module      = "whenmoon",
  .name        = "cancel",
  .usage       = "whenmoon order cancel <exchange> <order-id>",
  .description = "Cancel a resting order at the named exchange.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_order_cmd_cancel,
  .parent_path = "whenmoon/order",
};

bool
wm_order_register_verbs(void)
{
  // /whenmoon order parent.
  if(cmd_register(&whenmoon_order_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_order_buy_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_order_sell_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_order_cancel_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
