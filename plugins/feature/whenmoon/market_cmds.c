// botmanager — MIT
// whenmoon market verbs (state-changing under /whenmoon market;
// observability under /show whenmoon).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "market_cmds.h"
#include "market_engine.h"
#include "dl_commands.h"

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Read one market-id token off ctx->args, parse it, and produce the
// (exch/base/quote) triple plus the coinbase wire-form symbol.
// Replies on the ctx with `usage` on missing token, "bad market id"
// on parse failure, and returns FAIL in both cases.
static bool
wm_market_take_id_arg(const cmd_ctx_t *ctx, const char *usage,
    char *exch,   size_t exch_cap,
    char *base,   size_t base_cap,
    char *quote,  size_t quote_cap,
    char *symbol, size_t sym_cap)
{
  const char *p;
  char        id_tok[64] = {0};

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)))
  {
    cmd_reply(ctx, usage);
    return(FAIL);
  }

  if(wm_market_parse_id(id_tok, exch, exch_cap, base, base_cap,
         quote, quote_cap) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return(FAIL);
  }

  wm_market_wire_symbol(base, quote, symbol, sym_cap);

  if(symbol[0] == '\0')
  {
    cmd_reply(ctx, "market id too long");
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// /whenmoon market start|stop                                         //
// ------------------------------------------------------------------ //

static void
wm_market_cmd_start(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  char              exch[32];
  char              base[16];
  char              quote[16];
  char              symbol[WM_PRODUCT_ID_SZ];
  char              err[128] = {0};
  char              reply[192];
  char              id_str[WM_MARKET_ID_STR_SZ];

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  if(wm_market_take_id_arg(ctx,
         "usage: /whenmoon market start <exch>-<base>-<quote>",
         exch,   sizeof(exch),
         base,   sizeof(base),
         quote,  sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
    return;

  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  if(wm_market_add(st, exch, base, quote, symbol,
         true, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "market start failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  snprintf(reply, sizeof(reply),
      "market %s started (live + persisted)", id_str);
  cmd_reply(ctx, reply);
}

static void
wm_market_cmd_stop(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  char              exch[32];
  char              base[16];
  char              quote[16];
  char              symbol[WM_PRODUCT_ID_SZ];
  char              err[128] = {0};
  char              reply[192];
  char              id_str[WM_MARKET_ID_STR_SZ];
  bool              was_present = false;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  if(wm_market_take_id_arg(ctx,
         "usage: /whenmoon market stop <exch>-<base>-<quote>",
         exch,   sizeof(exch),
         base,   sizeof(base),
         quote,  sizeof(quote),
         symbol, sizeof(symbol)) != SUCCESS)
    return;

  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  if(wm_market_remove(st, exch, symbol, true, &was_present,
         err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "market stop failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  if(was_present)
    snprintf(reply, sizeof(reply),
        "market %s stopped (dropped from live + DB)", id_str);
  else
    snprintf(reply, sizeof(reply),
        "market %s not running (nothing to stop)", id_str);

  cmd_reply(ctx, reply);
}

// ------------------------------------------------------------------ //
// /show whenmoon indicators <id> <gran> latest                        //
// ------------------------------------------------------------------ //

static bool
wm_market_parse_gran(const char *tok, wm_gran_t *out)
{
  if(tok == NULL || out == NULL)
    return(FAIL);

  if(strcmp(tok, "1m")  == 0) { *out = WM_GRAN_1M;  return(SUCCESS); }
  if(strcmp(tok, "5m")  == 0) { *out = WM_GRAN_5M;  return(SUCCESS); }
  if(strcmp(tok, "15m") == 0) { *out = WM_GRAN_15M; return(SUCCESS); }
  if(strcmp(tok, "1h")  == 0) { *out = WM_GRAN_1H;  return(SUCCESS); }
  if(strcmp(tok, "4h")  == 0) { *out = WM_GRAN_4H;  return(SUCCESS); }
  if(strcmp(tok, "1d")  == 0) { *out = WM_GRAN_1D;  return(SUCCESS); }

  return(FAIL);
}

// Format one float slot — NaN renders as "NaN" so columns line up.
static void
wm_fmt_ind(char *buf, size_t cap, float v)
{
  if(isnan((double)v))
    snprintf(buf, cap, "NaN");
  else
    snprintf(buf, cap, "%.6g", (double)v);
}

static void
wm_market_cmd_indicators(const cmd_ctx_t *ctx)
{
  whenmoon_state_t   *st;
  whenmoon_market_t  *mk = NULL;
  const char         *p;
  char                exch[32];
  char                base[16];
  char                quote[16];
  char                id_tok[64] = {0};
  char                gran_tok[8] = {0};
  char                tail_tok[16] = {0};
  char                id_str[WM_MARKET_ID_STR_SZ];
  wm_gran_t           gran;
  wm_candle_full_t    bar;
  uint32_t            n;
  char                line[256];
  char                a[32];
  char                b[32];
  char                c[32];
  char                d[32];

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)))
  {
    cmd_reply(ctx,
        "usage: /show whenmoon indicators <exch>-<base>-<quote>"
        " <1m|5m|15m|1h|4h|1d> latest");
    return;
  }

  if(wm_market_parse_id(id_tok, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  if(!wm_dl_next_token(&p, gran_tok, sizeof(gran_tok)))
  {
    cmd_reply(ctx,
        "usage: /show whenmoon indicators <exch>-<base>-<quote>"
        " <1m|5m|15m|1h|4h|1d> latest");
    return;
  }

  if(wm_market_parse_gran(gran_tok, &gran) != SUCCESS)
  {
    cmd_reply(ctx, "bad granularity (expected 1m|5m|15m|1h|4h|1d)");
    return;
  }

  // Optional `latest` keyword for forward-compat.
  (void)wm_dl_next_token(&p, tail_tok, sizeof(tail_tok));

  mk = wm_market_lookup_by_id(st, id_str);

  if(mk == NULL)
  {
    snprintf(line, sizeof(line), "market %s not running", id_str);
    cmd_reply(ctx, line);
    return;
  }

  pthread_mutex_lock(&mk->lock);
  n = mk->grain_n[gran];

  if(n == 0 || mk->grain_arr[gran] == NULL)
  {
    pthread_mutex_unlock(&mk->lock);
    snprintf(line, sizeof(line),
        "%s %s: no bars yet", id_str, gran_tok);
    cmd_reply(ctx, line);
    return;
  }

  bar = mk->grain_arr[gran][n - 1];
  pthread_mutex_unlock(&mk->lock);

  snprintf(line, sizeof(line),
      CLR_BOLD "%s %s bar @ ms=%" PRId64 " (closed; bars=%u)" CLR_RESET,
      id_str, gran_tok, bar.ts_close_ms, n);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  OHLC  o=%.8g h=%.8g l=%.8g c=%.8g  v=%.8g",
      bar.open, bar.high, bar.low, bar.close, bar.volume);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_SMA_20]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_SMA_50]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_SMA_200]);
  snprintf(line, sizeof(line),
      "  SMA   20=%-12s  50=%-12s  200=%s", a, b, c);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_EMA_9]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_EMA_12]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_EMA_20]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_EMA_26]);
  snprintf(line, sizeof(line),
      "  EMA    9=%-10s  12=%-10s  20=%-10s  26=%s",
      a, b, c, d);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_EMA_50]);
  snprintf(line, sizeof(line), "  EMA   50=%s", a);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_MACD]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_MACD_SIGNAL]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_MACD_HIST]);
  snprintf(line, sizeof(line),
      "  MACD  =%-12s  sig=%-12s  hist=%s", a, b, c);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_RSI_14]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_STOCH_K]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_STOCH_D]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_CCI_20]);
  snprintf(line, sizeof(line),
      "  RSI14=%-10s  STOCH K=%-10s D=%-10s  CCI20=%s",
      a, b, c, d);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_BB_UPPER]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_BB_MIDDLE]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_BB_LOWER]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_BB_PCTB]);
  snprintf(line, sizeof(line),
      "  BB    U=%-10s  M=%-10s  L=%-10s  %%B=%s",
      a, b, c, d);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_VWAP]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_OBV]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_MFI_14]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_VPT]);
  snprintf(line, sizeof(line),
      "  VOL   VWAP=%-10s  OBV=%-12s  MFI14=%-8s  VPT=%s",
      a, b, c, d);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_ATR_14]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_TR]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_NATR_14]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_ADX_14]);
  snprintf(line, sizeof(line),
      "  VOL2  ATR14=%-10s  TR=%-10s  NATR14=%-8s  ADX14=%s",
      a, b, c, d);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_ROC_10]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_MOM_10]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_WILLR_14]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_PSAR]);
  snprintf(line, sizeof(line),
      "  TRND  ROC10=%-10s  MOM10=%-10s  WILLR14=%-8s  PSAR=%s",
      a, b, c, d);
  cmd_reply(ctx, line);

  wm_fmt_ind(a, sizeof(a), bar.ind[WM_IND_BAR_RANGE_PCT]);
  wm_fmt_ind(b, sizeof(b), bar.ind[WM_IND_BODY_PCT]);
  wm_fmt_ind(c, sizeof(c), bar.ind[WM_IND_UPPER_WICK_PCT]);
  wm_fmt_ind(d, sizeof(d), bar.ind[WM_IND_LOWER_WICK_PCT]);
  snprintf(line, sizeof(line),
      "  BAR   range%%=%-8s  body%%=%-8s  uw%%=%-8s  lw%%=%s",
      a, b, c, d);
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// /whenmoon market mode <id> <manual|paper|real>  (WM-MK-2)           //
// ------------------------------------------------------------------ //

static void
wm_market_cmd_mode(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              id_tok[64] = {0};
  char              mode_tok[16] = {0};
  char              exch[32];
  char              base[16];
  char              quote[16];
  char              id_str[WM_MARKET_ID_STR_SZ];
  char              err[160] = {0};
  char              reply[200];
  wm_market_mode_t  mode;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)) ||
     !wm_dl_next_token(&p, mode_tok, sizeof(mode_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon market mode <exch>-<base>-<quote>"
        " <manual|paper|real>");
    return;
  }

  if(wm_market_parse_id(id_tok, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  if(wm_market_mode_parse(mode_tok, &mode) != SUCCESS)
  {
    cmd_reply(ctx, "bad mode (expected manual|paper|real)");
    return;
  }

  if(wm_market_set_mode(id_str, mode, err, sizeof(err)) != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "market %s mode change FAIL: %s", id_str,
        err[0] != '\0' ? err : "(no detail)");
    cmd_reply(ctx, reply);
    return;
  }

  snprintf(reply, sizeof(reply),
      "market %s mode -> %s", id_str, wm_market_mode_name(mode));
  cmd_reply(ctx, reply);
}

// ------------------------------------------------------------------ //
// /whenmoon market force <id> <buy|sell> <qty> [<px>]  (WM-MK-4)      //
// ------------------------------------------------------------------ //

static void
wm_market_cmd_force(const cmd_ctx_t *ctx)
{
  whenmoon_state_t  *st;
  whenmoon_market_t *mk;
  const char        *p;
  char               id_tok[64]   = {0};
  char               side_tok[8]  = {0};
  char               qty_tok[32]  = {0};
  char               px_tok[32]   = {0};
  char               exch[32];
  char               base[16];
  char               quote[16];
  char               id_str[WM_MARKET_ID_STR_SZ];
  char               errbuf[192]  = {0};
  char               reply[256];
  double             qty;
  double             px_override = 0.0;
  char               side_ch;
  wm_market_mode_t   mode;
  int64_t            ts_ms;
  bool               ok;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok,   sizeof(id_tok))   ||
     !wm_dl_next_token(&p, side_tok, sizeof(side_tok)) ||
     !wm_dl_next_token(&p, qty_tok,  sizeof(qty_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon market force <exch>-<base>-<quote>"
        " <buy|sell> <qty> [<px>]");
    return;
  }

  if(wm_dl_next_token(&p, px_tok, sizeof(px_tok)))
    px_override = strtod(px_tok, NULL);

  if(wm_market_parse_id(id_tok, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  if(strcmp(side_tok, "buy") == 0)
    side_ch = 'b';
  else if(strcmp(side_tok, "sell") == 0)
    side_ch = 's';
  else
  {
    cmd_reply(ctx, "force: side must be buy|sell");
    return;
  }

  qty = strtod(qty_tok, NULL);

  if(qty <= 0.0)
  {
    cmd_reply(ctx, "force: qty must be > 0");
    return;
  }

  mk = wm_market_lookup_by_id(st, id_str);

  if(mk == NULL)
  {
    snprintf(reply, sizeof(reply),
        "error: market %s not running", id_str);
    cmd_reply(ctx, reply);
    return;
  }

  ts_ms = (int64_t)time(NULL) * 1000;

  pthread_mutex_lock(&mk->lock);
  mode = mk->session.mode;

  ok = wm_market_engine_force_trade_locked(mk, side_ch, qty, px_override,
      ts_ms, "force-operator", errbuf, sizeof(errbuf));

  pthread_mutex_unlock(&mk->lock);

  if(ok != SUCCESS)
  {
    snprintf(reply, sizeof(reply),
        "force %s %s mode=%s FAIL: %s",
        side_tok, id_str, wm_market_mode_name(mode),
        errbuf[0] != '\0' ? errbuf : "(no detail)");
    cmd_reply(ctx, reply);
    return;
  }

  if(mode == WM_MARKET_MODE_REAL)
    snprintf(reply, sizeof(reply),
        "force %s submitted to %s mode=real: qty=%.10g (awaiting fill)",
        side_tok, id_str, qty);
  else
    snprintf(reply, sizeof(reply),
        "force %s applied to %s mode=%s: qty=%.10g",
        side_tok, id_str, wm_market_mode_name(mode), qty);

  cmd_reply(ctx, reply);
}

// ------------------------------------------------------------------ //
// Parent stub                                                         //
// ------------------------------------------------------------------ //

static void
wm_market_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon market <start|stop|mode|force> ..."
      " (mode takes <manual|paper|real>)");
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

bool
wm_market_register_verbs(void)
{
  if(cmd_register("whenmoon", "market",
        "whenmoon market <start|stop|mode|force> ...",
        "Add or remove a live market and manage its session."
        " Starts: WS subscribe + live-ring 1m backfill."
        " Stops: unsubscribe + clear enabled flag."
        " Mode: change the market's mode (manual|paper|real)."
        " Force: operator-issued forced trade (manual+paper synth fill"
        " or real-mode submit, all gates honored).",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_parent_cb, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "start",
        "whenmoon market start <exch>-<base>-<quote>",
        "Start a market: WS subscribe, live-ring backfill (300 rows of"
        " 1m candles via REST), and persist (wm_market.enabled=true)"
        " so it survives daemon restarts. History catch-up is not"
        " automatic — drive `/whenmoon download <market>` for a"
        " deeper history.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_start, NULL, "whenmoon/market", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "stop",
        "whenmoon market stop <exch>-<base>-<quote>",
        "Stop a market: WS unsubscribe, drop from the live set, and"
        " flip wm_market.enabled=false so it does not resume on next"
        " plugin start.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_stop, NULL, "whenmoon/market", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // WM-MK-2: per-market mode change. Refuses non-flat transitions to
  // keep paper / real ledgers from contaminating each other when the
  // market still holds a position. Persists via market_persist.
  if(cmd_register("whenmoon", "mode",
        "whenmoon market mode <exch>-<base>-<quote>"
        " <manual|paper|real>",
        "Change a market's mode. PAPER = synthetic fills against the"
        " cached mark + paper-stats accumulation. REAL = exchange"
        " submission + risk gates (daily-loss bps, max-notional,"
        " pending-cap) + real-stats accumulation. MANUAL = strategies"
        " still receive ticks and log advice but the market takes no"
        " action; force-trades (WM-MK-4) drive the position. Refused"
        " when the market currently holds a position — flatten first.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_mode, NULL, "whenmoon/market", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // WM-MK-4: operator-issued forced trade. Bypasses strategy advisors
  // and the MANUAL mode "no auto-action" rule. Same fill-ledger choke
  // points as accepted strategy advice — apply_fill_locked for synth
  // modes, real_submit_locked (with the four real-mode gates) for real
  // mode.
  if(cmd_register("whenmoon", "force",
        "whenmoon market force <exch>-<base>-<quote>"
        " <buy|sell> <qty> [<px>]",
        "Operator-issued forced trade. Bypasses strategy advisors and"
        " the market's mode gate. Manual + paper modes: synthetic fill"
        " at <px> or last ticker (paper applies synth slippage only on"
        " the fallback path). Real mode: limit order via the exchange"
        " abstraction (credentials, daily-loss, pending-cap, and"
        " max-notional gates apply; fill arrives asynchronously). Same"
        " fill ledger choke point as accepted strategy advice — stats"
        " accumulate in the current mode's ledger. Refused on"
        " sell-against-flat (manual+paper) and any real-mode gate trip.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_force, NULL, "whenmoon/market", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "indicators",
        "show whenmoon indicators <exch>-<base>-<quote> <gran> latest",
        "Print the latest closed bar's indicator block for the named"
        " market and granularity (1m|5m|15m|1h|4h|1d). NaN slots"
        " indicate insufficient history for that indicator's window.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_indicators, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ====================================================================== //
// WM-MK-OBS-1: /show whenmoon market [<id>] per-market session view       //
// ====================================================================== //

// Render the no-arg list-row for one market.
static void
wm_obs_render_row(const cmd_ctx_t *ctx,
    const wm_market_session_snapshot_t *snap)
{
  const wm_market_stats_t *paper = &snap->stats[WM_MARKET_MODE_PAPER];
  const wm_market_stats_t *real  = &snap->stats[WM_MARKET_MODE_REAL];
  char line[320];

  if(snap->position.side == WM_MARKET_POS_LONG)
    snprintf(line, sizeof(line),
        "  %-24s  mode=%-6s side=long   qty=%-12.8g avg=%-10.4f"
        " paper:cash=%-9.2f realized=%-+9.2f"
        " real:cash=%-9.2f realized=%-+9.2f"
        " pending=%u/%u",
        snap->market_id_str,
        wm_market_mode_name(snap->mode),
        snap->position.qty,
        snap->position.avg_entry_px,
        paper->cash, paper->realized_pnl_lifetime,
        real->cash,  real->realized_pnl_lifetime,
        snap->pending_n, snap->pending_cap);
  else
    snprintf(line, sizeof(line),
        "  %-24s  mode=%-6s side=flat"
        "                                    "
        " paper:cash=%-9.2f realized=%-+9.2f"
        " real:cash=%-9.2f realized=%-+9.2f"
        " pending=%u/%u",
        snap->market_id_str,
        wm_market_mode_name(snap->mode),
        paper->cash, paper->realized_pnl_lifetime,
        real->cash,  real->realized_pnl_lifetime,
        snap->pending_n, snap->pending_cap);

  cmd_reply(ctx, line);
}

// Render the recent-fills tail for one mode (paper or real). The
// snapshot stores fills oldest→newest; we render in that order.
static void
wm_obs_render_fills(const cmd_ctx_t *ctx,
    const wm_market_session_snapshot_t *snap, wm_market_mode_t mode,
    const char *header)
{
  uint32_t i;
  char     line[256];

  cmd_reply(ctx, header);

  if(snap->recent_fills_n[mode] == 0)
  {
    cmd_reply(ctx, "    (none)");
    return;
  }

  for(i = 0; i < snap->recent_fills_n[mode]; i++)
  {
    const wm_market_fill_t *f = &snap->recent_fills[mode][i];

    snprintf(line, sizeof(line),
        "    ts=%-13" PRId64 "  %c qty=%-12.8g px=%-10.4f"
        " fee=%-7.4f realized=%-+8.2f cash=%-10.2f pos=%-+8.6g",
        f->ts_ms, f->side, f->qty, f->price, f->fee,
        f->realized_pnl, f->cash_after, f->position_after);
    cmd_reply(ctx, line);
  }
}

// Render the detail card.
static void
wm_obs_render_card(const cmd_ctx_t *ctx,
    const wm_market_session_snapshot_t *snap)
{
  const wm_market_stats_t *paper = &snap->stats[WM_MARKET_MODE_PAPER];
  const wm_market_stats_t *real  = &snap->stats[WM_MARKET_MODE_REAL];
  char line[320];

  if(snap->position.side == WM_MARKET_POS_LONG)
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET "  mode=%s  position: long"
        " qty=%.8g entry=%.4f opened_at_ms=%" PRId64,
        snap->market_id_str, wm_market_mode_name(snap->mode),
        snap->position.qty, snap->position.avg_entry_px,
        snap->position.opened_at_ms);
  else
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET "  mode=%s  position: flat",
        snap->market_id_str, wm_market_mode_name(snap->mode));
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  last_mark:    px=%-10.4f ts_ms=%" PRId64
      "    last_ticker: px=%-10.4f ts_ms=%" PRId64,
      snap->last_mark_px, snap->last_mark_ms,
      snap->last_ticker_px, snap->last_ticker_ms);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  paper_stats: cash=%.2f starting=%.2f"
      " realized_lifetime=%+.2f realized_today=%+.2f"
      " fees=%.4f fills=%" PRIu64 " last_fill_ms=%" PRId64,
      paper->cash, paper->starting_cash,
      paper->realized_pnl_lifetime, paper->realized_pnl_today,
      paper->lifetime_fees, paper->lifetime_fills_count,
      paper->last_fill_ms);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  real_stats:  cash=%.2f starting=%.2f"
      " realized_lifetime=%+.2f realized_today=%+.2f"
      " fees=%.4f fills=%" PRIu64 " last_fill_ms=%" PRId64,
      real->cash, real->starting_cash,
      real->realized_pnl_lifetime, real->realized_pnl_today,
      real->lifetime_fees, real->lifetime_fills_count,
      real->last_fill_ms);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  params:      fee_bps=%.1f slip_bps=%.1f size_frac=%.2f"
      " max_notional=%.2f daily_loss_bps=%.1f pending_cap=%u",
      snap->fee_bps, snap->slip_bps, snap->size_frac,
      snap->max_notional, snap->daily_loss_bps, snap->pending_cap);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  pending:     %u", snap->pending_n);
  cmd_reply(ctx, line);

  wm_obs_render_fills(ctx, snap, WM_MARKET_MODE_PAPER,
      "  recent paper fills (oldest first):");
  wm_obs_render_fills(ctx, snap, WM_MARKET_MODE_REAL,
      "  recent real fills (oldest first):");
}

static void
wm_show_market_cmd(const cmd_ctx_t *ctx)
{
  const char                  *p;
  char                         id_tok[WM_MARKET_ID_STR_SZ] = {0};
  whenmoon_state_t            *st;
  whenmoon_markets_t          *m;
  wm_market_session_snapshot_t snap;
  uint32_t                     i;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  m = st->markets;
  p = ctx->args != NULL ? ctx->args : "";

  // Detail-arg form.
  if(wm_dl_next_token(&p, id_tok, sizeof(id_tok)))
  {
    whenmoon_market_t *mk = wm_market_lookup_by_id(st, id_tok);
    char err[128];

    if(mk == NULL)
    {
      snprintf(err, sizeof(err),
          "error: market %s not running", id_tok);
      cmd_reply(ctx, err);
      return;
    }

    wm_market_session_snapshot(mk, &snap);
    wm_obs_render_card(ctx, &snap);
    return;
  }

  // No-arg list view.
  if(m->n_markets == 0)
  {
    cmd_reply(ctx, "whenmoon: (no markets running)");
    return;
  }

  cmd_reply(ctx, CLR_BOLD "whenmoon market sessions" CLR_RESET);

  for(i = 0; i < m->n_markets; i++)
  {
    wm_market_session_snapshot(&m->arr[i], &snap);
    wm_obs_render_row(ctx, &snap);
  }
}

bool
wm_show_market_register_verbs(void)
{
  // /show whenmoon market [<id>]  — alias /show whenmoon mk
  if(cmd_register("whenmoon", "market",
        "show whenmoon market [<id>]",
        "Per-market session: mode, position, paper+real stats,"
        " pending count. With <id>: a detail card mirroring the legacy"
        " `/show whenmoon trade` layout, plus recent fills tails for"
        " both paper and real ledgers.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_show_market_cmd, NULL, "show/whenmoon", "mk",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
