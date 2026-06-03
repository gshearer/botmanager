// botmanager — MIT
// /show whenmoon orders + /show whenmoon exchange (WM-OR-1 part 2).
//
// Cross-exchange observability — both verbs are exchange-discovery
// driven via `exchange_name_list`, so any newly-registered exchange
// surfaces here automatically without code change.
//
// `/show whenmoon orders [exchange]` walks each registered exchange
// (or just the named one) and emits a reply with that exchange's
// open orders. Multi-exchange responses arrive as one reply per
// exchange — the typed callback fires on the curl-multi worker, so a
// formal aggregator (refcounted fan-in into a single consolidated
// reply) waits until a second exchange ships and the visual ordering
// matters.
//
// `/show whenmoon exchange [name]` is sync only: capability snapshot
// plus product-cache counts plus per-exchange active-market count
// derived from the local whenmoon_state.markets walk. Open-order
// counts are deliberately omitted to keep this verb sync; operators
// asking for open orders use `/show whenmoon orders`.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "dl_commands.h"
#include "wm_exch_query.h"
#include "userns.h"
#include "exchange_api.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WM_EXCH_LIST_CAP     16

// Typed completion callback: bridge the async orders result into the
// synchronous waiter (wm_exch_query.h). A NULL result is translated to a
// typed error so the waiter's copy-out carries a message.
static void
wm_show_orders_on_orders(const exchange_orders_result_t *res, void *user)
{
  if(res != NULL)
  {
    wm_sync_fetch_complete(user, res, sizeof(*res));
    return;
  }

  {
    exchange_orders_result_t err;

    memset(&err, 0, sizeof(err));
    snprintf(err.err, sizeof(err.err), "no result delivered");
    wm_sync_fetch_complete(user, &err, sizeof(err));
  }
}

// ------------------------------------------------------------------ //
// /show whenmoon orders [exchange]                                    //
// ------------------------------------------------------------------ //
//
// On-demand authenticated open-orders snapshot. Synchronous for the same
// reason as `/show whenmoon balances`: the control socket drops a reply
// emitted from an async callback once dispatch returns (see
// wm_exch_query.h). Creds-only — no live market or real mode required.

static void
wm_show_orders_render(const cmd_ctx_t *ctx, const char *exchange)
{
  exchange_capabilities_t  caps;
  exchange_orders_result_t res;
  wm_sync_fetch_t         *w;
  char                     header[256];
  char                     line[256];
  uint32_t                 i;

  if(exchange_get_capabilities(exchange, &caps) != SUCCESS)
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET " (not a registered exchange)", exchange);
    cmd_reply(ctx, header);
    return;
  }

  if(!caps.has_credentials)
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET " (no credentials configured)", exchange);
    cmd_reply(ctx, header);
    return;
  }

  w = wm_sync_fetch_begin(sizeof(res));

  if(w == NULL)
  {
    cmd_reply(ctx, "  out of memory");
    return;
  }

  // The callback may run inline on a synchronous FAIL; the bridge
  // tolerates either ordering.
  (void)exchange_list_orders_async(exchange, "OPEN", NULL,
      wm_show_orders_on_orders, w);

  if(!wm_sync_fetch_wait(w, &res, sizeof(res), WM_EXCH_QUERY_WAIT_MS))
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET "  " CLR_RED "FAIL" CLR_RESET
        ": timed out", exchange);
    cmd_reply(ctx, header);
    return;
  }

  if(res.err[0] != '\0')
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET "  " CLR_RED "FAIL" CLR_RESET ": %s",
        exchange, res.err);
    cmd_reply(ctx, header);
    return;
  }

  snprintf(header, sizeof(header),
      CLR_BOLD "  %s" CLR_RESET "  " CLR_GREEN "ok" CLR_RESET
      " — %u open order%s",
      exchange, res.count, res.count == 1 ? "" : "s");
  cmd_reply(ctx, header);

  for(i = 0; i < res.count; i++)
  {
    const exchange_order_t *o = &res.rows[i];
    char                    qbuf[40], fbuf[40], pbuf[40];

    snprintf(line, sizeof(line),
        "    %s %s %s qty=%s filled=%s px=%s status=%s id=%s",
        o->product_id, o->side, o->type,
        wm_fmt_amount(o->size,        qbuf, sizeof(qbuf)),
        wm_fmt_amount(o->filled_size, fbuf, sizeof(fbuf)),
        wm_fmt_amount(o->price,       pbuf, sizeof(pbuf)),
        o->status, o->order_id);
    cmd_reply(ctx, line);
  }
}

static void
wm_cmd_show_orders(const cmd_ctx_t *ctx)
{
  const char *p;
  char        exch_tok[EXCHANGE_NAME_SZ] = {0};
  char        names[WM_EXCH_LIST_CAP][EXCHANGE_NAME_SZ];
  uint32_t    n;
  uint32_t    i;

  p = ctx->args != NULL ? ctx->args : "";

  cmd_reply(ctx, CLR_BOLD "open orders" CLR_RESET);

  // Explicit exchange argument: query just that one.
  if(wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)))
  {
    wm_show_orders_render(ctx, exch_tok);
    return;
  }

  // No-arg: walk every registered exchange.
  if(exchange_name_list(names, WM_EXCH_LIST_CAP, &n) != SUCCESS || n == 0)
  {
    cmd_reply(ctx, "  (no exchanges registered)");
    return;
  }

  if(n > WM_EXCH_LIST_CAP)
    n = WM_EXCH_LIST_CAP;

  for(i = 0; i < n; i++)
    wm_show_orders_render(ctx, names[i]);
}

// ------------------------------------------------------------------ //
// /show whenmoon exchange                                             //
// ------------------------------------------------------------------ //

static uint32_t
wm_count_active_markets(const char *exchange)
{
  whenmoon_state_t *st;
  uint32_t          i;
  uint32_t          n = 0;

  if(exchange == NULL || exchange[0] == '\0')
    return(0);

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return(0);

  for(i = 0; i < st->markets->n_markets; i++)
  {
    const whenmoon_market_t *mk = &st->markets->arr[i];
    size_t                   prefix_len = strnlen(exchange, EXCHANGE_NAME_SZ);

    if(strncmp(mk->market_id_str, exchange, prefix_len) == 0
        && mk->market_id_str[prefix_len] == '-')
      n++;
  }

  return(n);
}

static void
wm_show_exchange_render_row(const cmd_ctx_t *ctx, const char *name)
{
  exchange_capabilities_t caps;
  uint32_t                active_markets;
  char                    line[256];

  if(exchange_get_capabilities(name, &caps) != SUCCESS)
  {
    snprintf(line, sizeof(line),
        "  %-12s  (capabilities unavailable)", name);
    cmd_reply(ctx, line);
    return;
  }

  active_markets = wm_count_active_markets(name);

  snprintf(line, sizeof(line),
      "  %-12s  auth=%-3s  rps=%u/%u  markets=%u",
      name,
      caps.has_credentials  ? "yes" : "no",
      (unsigned)caps.advertised_rps,
      (unsigned)caps.advertised_burst,
      (unsigned)active_markets);
  cmd_reply(ctx, line);
}

static void
wm_cmd_show_exchange(const cmd_ctx_t *ctx)
{
  const char *p;
  char        exch_tok[EXCHANGE_NAME_SZ] = {0};
  char        names[WM_EXCH_LIST_CAP][EXCHANGE_NAME_SZ];
  uint32_t    n;
  uint32_t    i;

  p = ctx->args != NULL ? ctx->args : "";

  if(wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)))
  {
    cmd_reply(ctx, "exchange:");
    wm_show_exchange_render_row(ctx, exch_tok);
    return;
  }

  // No-arg: list every registered exchange.
  if(exchange_name_list(names, WM_EXCH_LIST_CAP, &n) != SUCCESS || n == 0)
  {
    cmd_reply(ctx, "no exchanges registered");
    return;
  }

  if(n > WM_EXCH_LIST_CAP)
    n = WM_EXCH_LIST_CAP;

  cmd_reply(ctx, "exchanges:");

  for(i = 0; i < n; i++)
    wm_show_exchange_render_row(ctx, names[i]);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

bool
wm_exch_register_verbs(void)
{
  if(cmd_register("whenmoon", "orders",
        "show whenmoon orders [exchange]",
        "List open orders across registered exchanges.",
        "Without an argument, fires one async list_orders call per"
        " registered exchange — each exchange's response arrives as a"
        " separate reply. With an argument, queries that single"
        " exchange. Auth-gated: exchanges without configured API keys"
        " reply with 'error: <name>: api keys not configured'.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_cmd_show_orders, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "exchange",
        "show whenmoon exchange [name]",
        "Snapshot of registered exchanges and their capabilities.",
        "Per-exchange row: sandbox flag, auth state, advertised"
        " rps/burst, cached product count (active/total), local"
        " active-market count routed to that exchange.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_cmd_show_exchange, NULL, "show/whenmoon", "exch",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
