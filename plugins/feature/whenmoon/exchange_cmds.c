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
#include "userns.h"
#include "exchange_api.h"
#include "method.h"
#include "alloc.h"
#include "cmd.h"
#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WM_EXCH_TARGET_SZ   128
#define WM_EXCH_LIST_CAP     16

// Heap-owned reply target carried across the async list_orders call.
typedef struct
{
  method_inst_t *inst;
  char           target[WM_EXCH_TARGET_SZ];
  char           exchange[EXCHANGE_NAME_SZ];
} wm_show_orders_ctx_t;

static wm_show_orders_ctx_t *
wm_show_orders_ctx_new(const cmd_ctx_t *ctx, const char *exchange)
{
  wm_show_orders_ctx_t *sc;
  const char           *target;

  if(ctx == NULL || ctx->msg == NULL || ctx->msg->inst == NULL)
    return(NULL);

  sc = mem_alloc(WHENMOON_CTX, "show.orders", sizeof(*sc));

  if(sc == NULL)
    return(NULL);

  memset(sc, 0, sizeof(*sc));
  sc->inst = ctx->msg->inst;

  target = ctx->msg->channel[0] != '\0'
      ? ctx->msg->channel
      : ctx->msg->sender;

  snprintf(sc->target, sizeof(sc->target), "%s",
      target != NULL ? target : "");
  snprintf(sc->exchange, sizeof(sc->exchange), "%s",
      exchange != NULL ? exchange : "");

  return(sc);
}

static void
wm_show_orders_send(wm_show_orders_ctx_t *sc, const char *text)
{
  if(sc == NULL || sc->inst == NULL || sc->target[0] == '\0' || text == NULL)
    return;

  method_send(sc->inst, sc->target, text);
}

static void
wm_show_orders_done(const exchange_orders_result_t *res, void *user)
{
  wm_show_orders_ctx_t *sc = user;
  char                  line[256];
  uint32_t              i;

  if(sc == NULL)
    return;

  if(res->err[0] != '\0')
  {
    snprintf(line, sizeof(line),
        "open orders on %s: error: %s", sc->exchange, res->err);
    wm_show_orders_send(sc, line);
    mem_free(sc);
    return;
  }

  snprintf(line, sizeof(line),
      "open orders on %s (%u):",
      sc->exchange, (unsigned)res->count);
  wm_show_orders_send(sc, line);

  if(res->count == 0)
  {
    wm_show_orders_send(sc, "  (none)");
    mem_free(sc);
    return;
  }

  for(i = 0; i < res->count; i++)
  {
    const exchange_order_t *o = &res->rows[i];

    snprintf(line, sizeof(line),
        "  %s %s %s qty=%.8g filled=%.8g px=%.8g status=%s id=%s",
        o->product_id, o->side, o->type,
        o->size, o->filled_size, o->price,
        o->status, o->order_id);
    wm_show_orders_send(sc, line);
  }

  mem_free(sc);
}

// ------------------------------------------------------------------ //
// /show whenmoon orders                                               //
// ------------------------------------------------------------------ //

static bool
wm_show_orders_dispatch(const cmd_ctx_t *ctx, const char *exchange)
{
  wm_show_orders_ctx_t *sc;

  sc = wm_show_orders_ctx_new(ctx, exchange);

  if(sc == NULL)
    return(FAIL);

  // exchange_list_orders_async fires wm_show_orders_done on every
  // path (success + every FAIL), and that callback frees `sc`.
  return(exchange_list_orders_async(exchange, "OPEN", NULL,
        wm_show_orders_done, sc));
}

static void
wm_cmd_show_orders(const cmd_ctx_t *ctx)
{
  const char *p;
  char        exch_tok[EXCHANGE_NAME_SZ] = {0};
  char        names[WM_EXCH_LIST_CAP][EXCHANGE_NAME_SZ];
  uint32_t    n;
  uint32_t    i;
  uint32_t    fired = 0;

  p = ctx->args != NULL ? ctx->args : "";

  if(wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)))
  {
    if(wm_show_orders_dispatch(ctx, exch_tok) != SUCCESS)
      cmd_reply(ctx, "out of memory");
    return;
  }

  // No-arg: walk every registered exchange.
  if(exchange_name_list(names, WM_EXCH_LIST_CAP, &n) != SUCCESS || n == 0)
  {
    cmd_reply(ctx, "no exchanges registered");
    return;
  }

  if(n > WM_EXCH_LIST_CAP)
    n = WM_EXCH_LIST_CAP;

  for(i = 0; i < n; i++)
  {
    if(wm_show_orders_dispatch(ctx, names[i]) == SUCCESS)
      fired++;
  }

  if(fired == 0)
    cmd_reply(ctx, "out of memory");
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
      "  %-12s  sandbox=%-3s  auth=%-3s  rps=%u/%u  markets=%u",
      name,
      caps.sandbox          ? "yes" : "no",
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
