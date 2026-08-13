// botmanager — MIT
// /whenmoon order verbs (WM-OR-1 part 2).
//
// Ad-hoc order surface against any registered exchange. Routes through
// the feature_exchange capability layer (`exchange_place_order_async`,
// `exchange_cancel_order_async`); the operator never names a strategy
// or a market book — these orders are unrelated to the market
// subsystem and never touch any whenmoon position state.
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
#include "cmd.h"
#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_ORDER_TARGET_SZ   128
#define WM_ORDER_LABEL_SZ     96

// Heap-owned reply target captured at handler entry.
typedef struct
{
  method_inst_t *inst;
  char           target[WM_ORDER_TARGET_SZ];
  char           label[WM_ORDER_LABEL_SZ];   // e.g. "buy coinbase-btc-usd"
} wm_order_async_ctx_t;

static wm_order_async_ctx_t *
wm_order_async_ctx_new(const cmd_ctx_t *ctx, const char *label)
{
  wm_order_async_ctx_t *ac;
  const char           *target;

  if(ctx == NULL || ctx->msg == NULL || ctx->msg->inst == NULL)
    return(NULL);

  ac = mem_alloc(WHENMOON_CTX, "order.async", sizeof(*ac));

  memset(ac, 0, sizeof(*ac));
  ac->inst = ctx->msg->inst;

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
  if(ac == NULL || ac->inst == NULL || ac->target[0] == '\0' || text == NULL)
    return;

  method_send(ac->inst, ac->target, text);
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
    snprintf(reply, sizeof(reply),
        "%s: placed: %s %s %s qty=%.8g px=%.8g",
        ac->label,
        res->order.order_id,
        res->order.product_id,
        res->order.side,
        res->order.size,
        res->order.price);
  }

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
  double                     qty;
  double                     price = 0.0;

  snprintf(usage, sizeof(usage),
      "usage: /whenmoon order %s <exch>-<base>-<quote>"
      " <limit|market> <qty> [<price>]", side);

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

  if(strcmp(type_tok, "limit") == 0)
  {
    is_market = false;

    if(!wm_dl_next_token(&p, px_tok, sizeof(px_tok)))
    {
      cmd_reply(ctx, "limit orders require a price");
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
    cmd_reply(ctx, "type must be 'limit' or 'market'");
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
    req.size  = qty;
    req.price = price;
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

bool
wm_order_register_verbs(void)
{
  // /whenmoon order parent.
  if(cmd_register("whenmoon", "order",
        "whenmoon order <verb> ...",
        "Ad-hoc order management against any registered exchange.",
        "Subcommands: buy <market_id> <limit|market> <qty> [<price>],"
        " sell <market_id> <limit|market> <qty> [<price>],"
        " cancel <exchange> <order_id>.\n"
        "Orders here are independent of the whenmoon market"
        " subsystem — no book, no strategy, no PnL attribution.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_order_parent_cb, NULL, "whenmoon", "or",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "buy",
        "whenmoon order buy <exch>-<base>-<quote>"
        " <limit|market> <qty> [<price>]",
        "Submit a buy order against the named exchange.",
        "limit orders require a <price>; market orders do not."
        " Market-buys interpret <qty> as quote-currency notional"
        " (e.g. USD); market-sells and limits interpret <qty> as"
        " base-currency size.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_order_cmd_buy, NULL, "whenmoon/order", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "sell",
        "whenmoon order sell <exch>-<base>-<quote>"
        " <limit|market> <qty> [<price>]",
        "Submit a sell order against the named exchange.",
        "Symmetric to /whenmoon order buy. <qty> is base-currency"
        " size in every mode (market and limit alike).",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_order_cmd_sell, NULL, "whenmoon/order", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "cancel",
        "whenmoon order cancel <exchange> <order-id>",
        "Cancel a resting order at the named exchange.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_order_cmd_cancel, NULL, "whenmoon/order", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
