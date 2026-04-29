// botmanager — MIT
// Whenmoon trade-engine admin verbs (WM-LT-4).
//
//   /whenmoon trade mode  <market_id> <strategy_name>
//                         <off|paper|backtest|live>
//   /whenmoon trade reset <market_id> <strategy_name>
//   /show whenmoon trade  [<market_id> [<strategy_name>]]
//
// Mode set is the entry point that creates the trade book on first
// call (subsequent calls just flip the mode). reset flattens position
// and clears PnL/fills but preserves the book + mode + cached params.
// show with no args lists every registered book; show with a market
// + strategy renders the full detail (position, recent fills, metrics).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "order.h"
#include "dl_commands.h"

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------- //
// Helpers                                                                 //
// ----------------------------------------------------------------------- //

// Find the running market for an id and snapshot its last_px under
// mk->lock. Returns true when the id resolves to a running market and
// last_px is populated.
static bool
wm_trade_lookup_mark(const char *market_id_str, double *out_px,
    int64_t *out_ms)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk;
  uint32_t            i;
  double              px;
  int64_t             ts;
  bool                hit = false;

  if(market_id_str == NULL)
    return(false);

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
    return(false);

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
  {
    mk = &m->arr[i];

    if(strncmp(mk->market_id_str, market_id_str,
           sizeof(mk->market_id_str)) != 0)
      continue;

    pthread_mutex_lock(&mk->lock);
    px  = mk->last_px;
    ts  = mk->last_tick_ms;
    pthread_mutex_unlock(&mk->lock);

    if(px > 0.0)
    {
      if(out_px != NULL)  *out_px = px;
      if(out_ms != NULL)  *out_ms = ts;
      hit = true;
    }

    break;
  }

  return(hit);
}

// ----------------------------------------------------------------------- //
// /whenmoon trade mode                                                    //
// ----------------------------------------------------------------------- //

static void
wm_trade_cmd_mode(const cmd_ctx_t *ctx)
{
  const char       *p;
  char              id_tok[64]                       = {0};
  char              name_tok[WM_STRATEGY_NAME_SZ]    = {0};
  char              mode_tok[16]                     = {0};
  char              reply[224];
  wm_trade_mode_t   mode;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)) ||
     !wm_dl_next_token(&p, name_tok, sizeof(name_tok)) ||
     !wm_dl_next_token(&p, mode_tok, sizeof(mode_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon trade mode <market_id> <strategy_name>"
        " <off|paper|backtest|live>");
    return;
  }

  if(wm_trade_mode_parse(mode_tok, &mode) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "trade mode: unknown mode '%s' (off|paper|backtest|live)",
        mode_tok);
    cmd_reply(ctx, reply);
    return;
  }

  if(wm_trade_book_set_mode(id_tok, name_tok, mode) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "trade mode: failed (engine not ready or oom)");
    cmd_reply(ctx, reply);
    return;
  }

  // Surface the helpful next-step hints. backtest + live are not yet
  // wired (WM-LT-5 / WM-LT-8); the engine accepts the mode but no
  // fills will fire until those chunks land.
  switch(mode)
  {
    case WM_TRADE_MODE_OFF:
      snprintf(reply, sizeof(reply),
          "trade %s/%s: mode=off (signals recorded, no fills)",
          id_tok, name_tok);
      break;

    case WM_TRADE_MODE_PAPER:
      snprintf(reply, sizeof(reply),
          "trade %s/%s: mode=paper (synthetic fills against bar/tick"
          " mark)",
          id_tok, name_tok);
      break;

    case WM_TRADE_MODE_BACKTEST:
      snprintf(reply, sizeof(reply),
          "trade %s/%s: mode=backtest (placeholder; WM-LT-5 wires"
          " fills)",
          id_tok, name_tok);
      break;

    case WM_TRADE_MODE_LIVE:
      snprintf(reply, sizeof(reply),
          "trade %s/%s: mode=live (placeholder; WM-LT-8 wires order"
          " placement)",
          id_tok, name_tok);
      break;
  }

  cmd_reply(ctx, reply);
}

// ----------------------------------------------------------------------- //
// /whenmoon trade reset                                                   //
// ----------------------------------------------------------------------- //

static void
wm_trade_cmd_reset(const cmd_ctx_t *ctx)
{
  const char *p;
  char        id_tok[64]                    = {0};
  char        name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char        reply[192];

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)) ||
     !wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon trade reset <market_id> <strategy_name>");
    return;
  }

  // Verify the book exists before reporting success — reset is a
  // silent no-op when the book is absent (the operator may have
  // mis-typed the strategy name).
  {
    wm_trade_snapshot_t snap;

    if(wm_trade_book_snapshot(id_tok, name_tok, &snap) != SUCCESS)
    {
      snprintf(reply, sizeof(reply),
          "trade reset: no book for %s/%s (use /whenmoon trade mode"
          " first)", id_tok, name_tok);
      cmd_reply(ctx, reply);
      return;
    }
  }

  wm_trade_book_reset(id_tok, name_tok);

  snprintf(reply, sizeof(reply),
      "trade %s/%s: reset (cash restored, position flat, pnl cleared)",
      id_tok, name_tok);
  cmd_reply(ctx, reply);
}

// ----------------------------------------------------------------------- //
// /whenmoon trade parent                                                  //
// ----------------------------------------------------------------------- //

static void
wm_trade_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon trade <mode|reset> ...");
}

// ----------------------------------------------------------------------- //
// /show whenmoon trade — list                                             //
// ----------------------------------------------------------------------- //

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} wm_trade_list_state_t;

static void
wm_trade_list_cb(const wm_trade_snapshot_t *snap, void *user)
{
  wm_trade_list_state_t *s = user;
  char                   line[256];
  char                   pos[48];
  double                 signed_pos;

  signed_pos = (snap->position.side == WM_POS_LONG)  ?  snap->position.qty
             : (snap->position.side == WM_POS_SHORT) ? -snap->position.qty
                                                     :  0.0;

  if(snap->position.side == WM_POS_FLAT)
    snprintf(pos, sizeof(pos), "flat");
  else
    snprintf(pos, sizeof(pos), "%+.6g @ %.6g",
        signed_pos, snap->position.avg_entry_px);

  snprintf(line, sizeof(line),
      "  %-24s %-24s mode=%-8s cash=%-12.2f pos=%-22s"
      " trades=%-3u pnl=%+.2f",
      snap->market_id_str, snap->strategy_name,
      wm_trade_mode_name(snap->mode), snap->cash,
      pos, snap->metrics.n_trades, snap->metrics.realized_pnl);
  cmd_reply(s->ctx, line);

  s->count++;
}

static void
wm_trade_show_list(const cmd_ctx_t *ctx)
{
  wm_trade_list_state_t s;

  cmd_reply(ctx, CLR_BOLD "whenmoon trade books" CLR_RESET);

  s.ctx   = ctx;
  s.count = 0;

  wm_trade_books_iterate(wm_trade_list_cb, &s);

  if(s.count == 0)
    cmd_reply(ctx,
        "  (no books — issue /whenmoon trade mode <mid> <strat> <mode>)");
}

// ----------------------------------------------------------------------- //
// /show whenmoon trade — detail                                           //
// ----------------------------------------------------------------------- //

static void
wm_trade_render_fills(const cmd_ctx_t *ctx,
    const wm_trade_snapshot_t *snap)
{
  char     line[224];
  uint32_t i;

  if(snap->n_fills == 0)
  {
    cmd_reply(ctx, "  fills: (none)");
    return;
  }

  cmd_reply(ctx, CLR_GRAY "  recent fills (newest last):" CLR_RESET);

  for(i = 0; i < snap->n_fills; i++)
  {
    const wm_fill_t *f = &snap->fills[i];
    char             reason[WM_STRATEGY_REASON_SZ];
    size_t           rlen;

    rlen = strnlen(f->reason, sizeof(reason) - 1);
    memcpy(reason, f->reason, rlen);
    reason[rlen] = '\0';

    snprintf(line, sizeof(line),
        "    ts=%" PRId64 " %c qty=%.6g px=%.6g fee=%.4f"
        " realized=%+.4f cash=%.2f pos=%+.6g (%s)",
        f->ts_ms, f->side, f->qty, f->price, f->fee,
        f->realized_pnl, f->cash_after, f->position_after,
        reason[0] != '\0' ? reason : "");
    cmd_reply(ctx, line);
  }
}

static void
wm_trade_show_detail(const cmd_ctx_t *ctx,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_snapshot_t snap;
  char                line[256];
  double              live_px;
  int64_t             live_ms;

  // Refresh the mark from the live market before snapshotting, so the
  // unrealized PnL line reflects the current ticker rather than the
  // last signal-time mark. Skipped if no live ticker is available
  // (e.g. market not running, sandbox WS silence).
  live_px = 0.0;
  live_ms = 0;

  if(wm_trade_lookup_mark(market_id_str, &live_px, &live_ms))
    wm_trade_book_update_mark(market_id_str, strategy_name,
        live_px, live_ms);

  if(wm_trade_book_snapshot(market_id_str, strategy_name, &snap)
         != SUCCESS)
  {
    snprintf(line, sizeof(line),
        "trade: no book for %s/%s", market_id_str, strategy_name);
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line),
      CLR_BOLD "%s / %s" CLR_RESET "  mode=%s",
      snap.market_id_str, snap.strategy_name,
      wm_trade_mode_name(snap.mode));
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  cash=%.2f starting=%.2f equity=%.2f"
      " unrealized=%+.2f mark=%.6g (ts=%" PRId64 ")",
      snap.cash, snap.starting_cash, snap.equity,
      snap.unrealized_pnl, snap.last_mark_px, snap.last_mark_ms);
  cmd_reply(ctx, line);

  if(snap.position.side == WM_POS_FLAT)
    snprintf(line, sizeof(line), "  position: flat");
  else
  {
    const char *side =
        snap.position.side == WM_POS_LONG ? "long" : "short";

    snprintf(line, sizeof(line),
        "  position: %s qty=%.6g entry=%.6g opened_at_ms=%" PRId64,
        side, snap.position.qty, snap.position.avg_entry_px,
        snap.position.opened_at_ms);
  }
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  params: size_frac=%.4f max_position=%.6g"
      " fee_bps=%.2f slip_bps=%.2f",
      snap.size_frac, snap.max_position,
      snap.fee_bps, snap.slip_bps);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  metrics: trades=%u w/l=%u/%u win_rate=%.2f"
      " realized=%+.2f fees=%.2f pf=%.3f maxdd=%.2f"
      " sharpe=%.3f sortino=%.3f",
      snap.metrics.n_trades, snap.metrics.n_wins, snap.metrics.n_losses,
      snap.metrics.win_rate, snap.metrics.realized_pnl,
      snap.metrics.fees_paid, snap.metrics.profit_factor,
      snap.metrics.max_drawdown, snap.metrics.sharpe,
      snap.metrics.sortino);
  cmd_reply(ctx, line);

  if(snap.has_last_signal)
  {
    char    reason[WM_STRATEGY_REASON_SZ];
    size_t  rlen;

    rlen = strnlen(snap.last_signal.reason, sizeof(reason) - 1);
    memcpy(reason, snap.last_signal.reason, rlen);
    reason[rlen] = '\0';

    snprintf(line, sizeof(line),
        "  last_signal: ts=%" PRId64 " score=%+.4f conf=%.4f reason=%s",
        snap.last_signal.ts_ms, snap.last_signal.score,
        snap.last_signal.confidence,
        reason[0] != '\0' ? reason : "(none)");
    cmd_reply(ctx, line);
  }
  else
    cmd_reply(ctx, "  last_signal: (none)");

  snprintf(line, sizeof(line),
      "  fills_total=%" PRIu64, snap.fill_total);
  cmd_reply(ctx, line);

  wm_trade_render_fills(ctx, &snap);
}

// ----------------------------------------------------------------------- //
// /show whenmoon trade reconcile (WM-PT-2)                                //
// ----------------------------------------------------------------------- //

static const char *
wm_trade_reconcile_marker(double delta)
{
  return((delta < 0.0 ? -delta : delta) < WM_TRADE_RECONCILE_EPS
         ? "ok" : "MISMATCH");
}

static void
wm_trade_show_reconcile(const cmd_ctx_t *ctx,
    const char *market_id_str, const char *strategy_name)
{
  wm_trade_reconcile_t rec;
  char                 line[224];
  const char          *fee_mark;

  if(wm_trade_book_reconcile(market_id_str, strategy_name, &rec)
         != SUCCESS)
  {
    snprintf(line, sizeof(line),
        "reconcile: no book for %s/%s",
        market_id_str, strategy_name);
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line),
      CLR_BOLD "reconcile %s/%s" CLR_RESET,
      rec.market_id_str, rec.strategy_name);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  expected_cash     = %15.4f", rec.expected_cash);
  cmd_reply(ctx, line);
  snprintf(line, sizeof(line),
      "  actual_cash       = %15.4f", rec.actual_cash);
  cmd_reply(ctx, line);
  snprintf(line, sizeof(line),
      "  cash_delta        = %15.4f  [%s]",
      rec.cash_delta, wm_trade_reconcile_marker(rec.cash_delta));
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  expected_position = %15.6f", rec.expected_position);
  cmd_reply(ctx, line);
  snprintf(line, sizeof(line),
      "  actual_position   = %15.6f", rec.actual_position);
  cmd_reply(ctx, line);
  snprintf(line, sizeof(line),
      "  position_delta    = %15.6f  [%s]",
      rec.position_delta, wm_trade_reconcile_marker(rec.position_delta));
  cmd_reply(ctx, line);

  if(rec.fees_reconciled)
  {
    fee_mark = wm_trade_reconcile_marker(rec.fee_delta);

    snprintf(line, sizeof(line),
        "  expected_fees     = %15.4f", rec.expected_fees);
    cmd_reply(ctx, line);
    snprintf(line, sizeof(line),
        "  actual_fees       = %15.4f", rec.actual_fees);
    cmd_reply(ctx, line);
    snprintf(line, sizeof(line),
        "  fee_delta         = %15.4f  [%s]",
        rec.fee_delta, fee_mark);
    cmd_reply(ctx, line);
  }
  else
  {
    snprintf(line, sizeof(line),
        "  actual_fees       = %15.4f  [n/a (ring-truncated)]",
        rec.actual_fees);
    cmd_reply(ctx, line);
  }

  if(rec.ring_truncated)
    snprintf(line, sizeof(line),
        "  fills_walked      = %u (of %d-cap ring, lifetime=%" PRIu64 ")",
        rec.fills_walked, WM_FILL_RING_CAP, rec.fills_total);
  else
    snprintf(line, sizeof(line),
        "  fills_walked      = %u (of %d-cap ring)",
        rec.fills_walked, WM_FILL_RING_CAP);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  status            = %s%s",
      rec.ok ? "OK" : "MISMATCH",
      rec.ring_truncated ? "  (mode=ring-truncated)" : "");
  cmd_reply(ctx, line);
}

// ----------------------------------------------------------------------- //
// /show whenmoon trade dispatcher                                         //
// ----------------------------------------------------------------------- //

static void
wm_trade_cmd_show(const cmd_ctx_t *ctx)
{
  const char *p;
  char        first_tok[32]                 = {0};
  char        id_tok[64]                    = {0};
  char        name_tok[WM_STRATEGY_NAME_SZ] = {0};
  bool        have_first;
  bool        have_name;

  p = ctx->args != NULL ? ctx->args : "";

  have_first = wm_dl_next_token(&p, first_tok, sizeof(first_tok));

  // Sub-verb: reconcile <market_id> <strategy_name>.
  if(have_first && strcmp(first_tok, "reconcile") == 0)
  {
    if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)) ||
       !wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
    {
      cmd_reply(ctx,
          "usage: /show whenmoon trade reconcile <market_id>"
          " <strategy_name>");
      return;
    }

    wm_trade_show_reconcile(ctx, id_tok, name_tok);
    return;
  }

  // No sub-verb: first token (if any) is the market_id; second is
  // the strategy name. List view when both are absent; detail view
  // when both are present.
  if(have_first)
  {
    snprintf(id_tok, sizeof(id_tok), "%s", first_tok);
    have_name = wm_dl_next_token(&p, name_tok, sizeof(name_tok));

    if(have_name)
    {
      wm_trade_show_detail(ctx, id_tok, name_tok);
      return;
    }

    cmd_reply(ctx,
        "usage: /show whenmoon trade"
        " [reconcile <market_id> <strategy_name>"
        " | <market_id> <strategy_name>]");
    return;
  }

  wm_trade_show_list(ctx);
}

// ----------------------------------------------------------------------- //
// Registration                                                            //
// ----------------------------------------------------------------------- //

bool
wm_trade_register_verbs(void)
{
  // /whenmoon trade parent.
  if(cmd_register("whenmoon", "trade",
        "whenmoon trade <verb> ...",
        "Trade-engine controls (WM-LT-4).",
        "Subcommands: mode <market_id> <strat>"
        " <off|paper|backtest|live>,"
        " reset <market_id> <strat>.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_trade_parent_cb, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "mode",
        "whenmoon trade mode <market_id> <strategy_name>"
        " <off|paper|backtest|live>",
        "Set the trade-engine mode for a (market, strategy) pair.",
        "off:      signals recorded, no fills.\n"
        "paper:    synthetic fills against bar/tick mark + slip + fee.\n"
        "backtest: placeholder (WM-LT-5 wires snapshot replay).\n"
        "live:     placeholder (WM-LT-8 wires real order placement).\n"
        "First call creates the trade book seeded with starting_cash"
        " from the strategy KV (default 10000).",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_trade_cmd_mode, NULL, "whenmoon/trade", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "reset",
        "whenmoon trade reset <market_id> <strategy_name>",
        "Flatten position, restore cash to starting_cash, clear PnL"
        " accumulators + fills ring. Mode + cached params are"
        " preserved.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_trade_cmd_reset, NULL, "whenmoon/trade", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // /show whenmoon trade
  if(cmd_register("whenmoon", "trade",
        "show whenmoon trade [reconcile] [<market_id> <strategy_name>]",
        "List every trade book (no args), render full detail for one,"
        " or reconcile a book against its fills ring (WM-PT-2).",
        "List view: one row per (market, strategy) book with mode,"
        " cash, position, lifetime trade count, realized PnL.\n"
        "Detail view: refreshes the mark from the live ticker before"
        " rendering so unrealized PnL reflects the current price.\n"
        "Reconcile view: walks the fills ring and recomputes expected"
        " cash + position + fees from first principles, reporting"
        " deltas vs the live book. When the ring has wrapped"
        " (lifetime fills > 256) the walk anchors at the oldest live"
        " fill and reports mode=ring-truncated.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_trade_cmd_show, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
