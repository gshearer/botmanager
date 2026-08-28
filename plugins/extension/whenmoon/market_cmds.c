// botmanager — MIT
// whenmoon market verbs (state-changing under /whenmoon market;
// observability under /show whenmoon).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "market_cmds.h"
#include "market_engine.h"
#include "numeraire.h"
#include "live.h"
#include "strategy.h"
#include "dl_commands.h"
#include "wm_exch_query.h"

#include "cmd.h"
#include "colors.h"
#include "display.h"
#include "common.h"
#include "userns.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Read one market-id token off ctx->args, parse it, and produce the
// (exch/base/quote) triple plus the coinbase wire-form symbol.
// Replies on the ctx with `usage` on missing token, "bad market id"
// on parse failure, and returns FAIL in both cases.
static bool
wm_market_take_id_arg(const cmd_ctx_t *ctx, const char *usage,
    char *exch,     size_t exch_cap,
    char *base,     size_t base_cap,
    char *quote,    size_t quote_cap,
    char *symbol,   size_t sym_cap,
    char *instance, size_t inst_cap)
{
  const char *p;
  char        id_tok[64] = {0};

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)))
  {
    cmd_reply(ctx, usage);
    return(FAIL);
  }

  // WM-MI-1: accept an optional "@<instance>" suffix; empty when absent.
  if(wm_market_parse_instance_id(id_tok, exch, exch_cap, base, base_cap,
         quote, quote_cap, instance, inst_cap) != SUCCESS)
  {
    cmd_reply(ctx,
        "bad market id (expected <exch>-<base>-<quote>[@<instance>])");
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
  char              instance[WM_INSTANCE_LABEL_SZ] = {0};
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
         "usage: /whenmoon market start <exch>-<base>-<quote>[@<instance>]",
         exch,     sizeof(exch),
         base,     sizeof(base),
         quote,    sizeof(quote),
         symbol,   sizeof(symbol),
         instance, sizeof(instance)) != SUCCESS)
    return;

  wm_market_format_instance_id(exch, base, quote, instance,
      id_str, sizeof(id_str));

  if(wm_market_add(st, exch, base, quote, symbol, instance,
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
  char              instance[WM_INSTANCE_LABEL_SZ] = {0};
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
         "usage: /whenmoon market stop <exch>-<base>-<quote>[@<instance>]",
         exch,     sizeof(exch),
         base,     sizeof(base),
         quote,    sizeof(quote),
         symbol,   sizeof(symbol),
         instance, sizeof(instance)) != SUCCESS)
    return;

  wm_market_format_instance_id(exch, base, quote, instance,
      id_str, sizeof(id_str));

  if(wm_market_remove(st, exch, symbol, instance, true, &was_present,
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

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the mk->lock read below.
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    snprintf(line, sizeof(line), "market %s not running", id_str);
    cmd_reply(ctx, line);
    return;
  }

  pthread_mutex_lock(&mk->lock);
  n = mk->grain_n[gran];

  if(n == 0 || mk->grain_arr[gran] == NULL)
  {
    pthread_mutex_unlock(&mk->lock);
    pthread_rwlock_unlock(&st->markets->arr_lock);
    snprintf(line, sizeof(line),
        "%s %s: no bars yet", id_str, gran_tok);
    cmd_reply(ctx, line);
    return;
  }

  bar = mk->grain_arr[gran][n - 1];
  pthread_mutex_unlock(&mk->lock);
  pthread_rwlock_unlock(&st->markets->arr_lock);

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
  char              instance[WM_INSTANCE_LABEL_SZ] = {0};
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
        "usage: /whenmoon market mode <exch>-<base>-<quote>[@<instance>]"
        " <manual|paper|real>");
    return;
  }

  if(wm_market_parse_instance_id(id_tok, exch, sizeof(exch),
         base, sizeof(base), quote, sizeof(quote),
         instance, sizeof(instance)) != SUCCESS)
  {
    cmd_reply(ctx,
        "bad market id (expected <exch>-<base>-<quote>[@<instance>])");
    return;
  }

  wm_market_format_instance_id(exch, base, quote, instance,
      id_str, sizeof(id_str));

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

  // WM-REAL-CASH-1: switching into real mode is the natural point to bind
  // the cash ledger to actual funds — otherwise real order sizing would
  // deploy a fraction of the seeded $10k placeholder. The market is flat
  // here (set_mode enforces it), so the quote `available` is the full
  // deployable balance. Blocking is fine in this command context. A
  // failure does NOT revert the mode switch — but it is surfaced loudly,
  // and the market card shows "UNSYNCED" until a later sync succeeds.
  if(mode == WM_MARKET_MODE_REAL)
  {
    whenmoon_market_t *mk;
    double             cash = 0.0;
    char               rerr[160] = {0};

    // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the reconcile (which
    // reads/writes the session under mk->lock).
    pthread_rwlock_rdlock(&st->markets->arr_lock);
    mk = wm_market_lookup_by_id(st, id_str);

    if(mk != NULL &&
       wm_market_reconcile_real_cash(mk, &cash, rerr, sizeof(rerr))
           == SUCCESS)
      snprintf(reply, sizeof(reply),
          "  real cash reconciled to %.2f (real sizing now deploys"
          " size_frac of actual funds)", cash);

    else
      snprintf(reply, sizeof(reply),
          "  WARN real cash UNSYNCED: %s — sizing would use the placeholder;"
          " run /whenmoon market sync %s",
          rerr[0] != '\0' ? rerr : "reconcile failed", id_str);

    pthread_rwlock_unlock(&st->markets->arr_lock);
    cmd_reply(ctx, reply);
  }
}

// ------------------------------------------------------------------ //
// /whenmoon market sync <id>  — reconcile real cash (WM-REAL-CASH-1)  //
// ------------------------------------------------------------------ //

static void
wm_market_cmd_sync(const cmd_ctx_t *ctx)
{
  whenmoon_state_t  *st;
  whenmoon_market_t *mk;
  const char        *p;
  char               id_tok[64] = {0};
  char               exch[32];
  char               base[16];
  char               quote[16];
  char               id_str[WM_MARKET_ID_STR_SZ];
  char               err[160] = {0};
  char               reply[224];
  double             cash = 0.0;

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
        "usage: /whenmoon market sync <exch>-<base>-<quote>");
    return;
  }

  if(wm_market_parse_id(id_tok, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the reconcile (mk->lock).
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    snprintf(reply, sizeof(reply), "market %s not running", id_str);
    cmd_reply(ctx, reply);
    return;
  }

  if(wm_market_reconcile_real_cash(mk, &cash, err, sizeof(err)) != SUCCESS)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    snprintf(reply, sizeof(reply),
        "market %s real cash sync FAIL: %s", id_str,
        err[0] != '\0' ? err : "(no detail)");
    cmd_reply(ctx, reply);
    return;
  }

  pthread_rwlock_unlock(&st->markets->arr_lock);

  snprintf(reply, sizeof(reply),
      "market %s real cash reconciled to %.2f", id_str, cash);
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
  char               instance[WM_INSTANCE_LABEL_SZ] = {0};
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
        "usage: /whenmoon market force <exch>-<base>-<quote>[@<instance>]"
        " <buy|sell> <qty> [<px>]");
    return;
  }

  if(wm_dl_next_token(&p, px_tok, sizeof(px_tok)))
    px_override = strtod(px_tok, NULL);

  // WM-FORCE-INST-1: instance-aware like every other market verb, so
  // the treasury's manual order surface (and the tripwire hook below)
  // can address `@instance` markets.
  if(wm_market_parse_instance_id(id_tok, exch, sizeof(exch), base,
         sizeof(base), quote, sizeof(quote), instance,
         sizeof(instance)) != SUCCESS)
  {
    cmd_reply(ctx,
        "bad market id (expected <exch>-<base>-<quote>[@<instance>])");
    return;
  }

  wm_market_format_instance_id(exch, base, quote, instance,
      id_str, sizeof(id_str));

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

  // WM-MKT-ARR-UAF-1: hold rdlock across lookup + the mk->lock force-trade.
  pthread_rwlock_rdlock(&st->markets->arr_lock);
  mk = wm_market_lookup_by_id(st, id_str);

  if(mk == NULL)
  {
    pthread_rwlock_unlock(&st->markets->arr_lock);
    snprintf(reply, sizeof(reply),
        "error: market %s not running", id_str);
    cmd_reply(ctx, reply);
    return;
  }

  ts_ms = wm_now_ms();

  pthread_mutex_lock(&mk->lock);
  mode = mk->session.mode;

  ok = wm_market_engine_force_trade_locked(mk, side_ch, qty, px_override,
      ts_ms, "force-operator", errbuf, sizeof(errbuf));

  pthread_mutex_unlock(&mk->lock);
  pthread_rwlock_unlock(&st->markets->arr_lock);

  // Synth-mode force fills evaluate the treasury tripwire here. Real
  // force trades are checked when the exchange fill lands in
  // wm_market_engine_record_external_fill — submissions don't move the
  // stack, fills do.
  if(ok == SUCCESS && mode != WM_MARKET_MODE_REAL)
    wm_live_treasury_freeze_check(id_str);

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
      "usage: /whenmoon market <start|stop|mode|force|sync> ..."
      " (mode takes <manual|paper|real>; sync reconciles real cash)");
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_decl_t whenmoon_market_decl = {
  .module      = "whenmoon",
  .name        = "market",
  .usage       = "whenmoon market <start|stop|mode|force|sync> ...",
  .description =
      "Add or remove a live market and manage its session."
      " Starts: WS subscribe + live-ring 1m backfill."
      " Stops: unsubscribe + clear enabled flag."
      " Mode: change the market's mode (manual|paper|real)."
      " Force: operator-issued forced trade (manual+paper synth fill"
      " or real-mode submit, all gates honored)."
      " Sync: force a fresh reconcile of real-mode cash + re-anchor the"
      " daily-loss baseline (usually unnecessary — flat markets"
      " auto-reconcile from the balance cache).",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_parent_cb,
  .parent_path = "whenmoon",
};

static const cmd_decl_t whenmoon_market_start_decl = {
  .module      = "whenmoon",
  .name        = "start",
  .usage       = "whenmoon market start <exch>-<base>-<quote>",
  .description =
      "Start a market: WS subscribe, live-ring backfill (300 rows of"
      " 1m candles via REST), and persist (wm_market.enabled=true)"
      " so it survives daemon restarts. History catch-up is not"
      " automatic — drive `/whenmoon download <market>` for a"
      " deeper history.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_cmd_start,
  .parent_path = "whenmoon/market",
};

static const cmd_decl_t whenmoon_market_stop_decl = {
  .module      = "whenmoon",
  .name        = "stop",
  .usage       = "whenmoon market stop <exch>-<base>-<quote>",
  .description = "Stop a market: WS unsubscribe, drop from the live set, and"
                 " flip wm_market.enabled=false so it does not resume on next"
                 " plugin start.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_cmd_stop,
  .parent_path = "whenmoon/market",
};

static const cmd_decl_t whenmoon_market_mode_decl = {
  .module      = "whenmoon",
  .name        = "mode",
  .usage       = "whenmoon market mode <exch>-<base>-<quote>"
                 " <manual|paper|real>",
  .description =
      "Change a market's mode. PAPER = synthetic fills against the"
      " cached mark + paper-stats accumulation. REAL = exchange"
      " submission + risk gates (daily satoshi drawdown, per-order"
      " satoshi cap, pending-cap, mark staleness) + real-stats"
      " accumulation."
      " MANUAL = strategies"
      " still receive ticks and log advice but the market takes no"
      " action; force-trades (WM-MK-4) drive the position. Refused"
      " when the market currently holds a position — flatten first.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_cmd_mode,
  .parent_path = "whenmoon/market",
};

static const cmd_decl_t whenmoon_market_force_decl = {
  .module      = "whenmoon",
  .name        = "force",
  .usage       = "whenmoon market force <exch>-<base>-<quote>[@<instance>]"
                 " <buy|sell> <qty> [<px>]",
  .description =
      "Operator-issued forced trade. Bypasses strategy advisors and"
      " the market's mode gate. Manual + paper modes: synthetic fill"
      " at <px> or last ticker (paper applies synth slippage only on"
      " the fallback path). Real mode: limit order via the exchange"
      " abstraction (credentials, daily satoshi drawdown, pending-cap"
      " and per-order satoshi cap gates apply; fill arrives"
      " asynchronously). Giving"
      " an explicit <px> also waives the mark-staleness gate — the"
      " price is yours, not one inferred from a feed. Same"
      " fill ledger choke point as accepted strategy advice — stats"
      " accumulate in the current mode's ledger. Refused on"
      " sell-against-flat (manual+paper) and any real-mode gate trip.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_cmd_force,
  .parent_path = "whenmoon/market",
};

static const cmd_decl_t whenmoon_market_sync_decl = {
  .module      = "whenmoon",
  .name        = "sync",
  .usage       = "whenmoon market sync <exch>-<base>-<quote>",
  .description =
      "Force a fresh reconcile of the market's REAL-mode cash ledger"
      " against the live quote-currency `available` balance on its bound"
      " exchange, and re-anchor the daily-loss baseline. Real order"
      " sizing is size_frac * cash. Usually unnecessary: a flat market"
      " auto-reconciles from the balance cache (the scheduled poll, an"
      " on-demand `/show whenmoon balances`, or a real fill), and"
      " switching into real mode also reconciles. Use this to force a"
      " refresh after an external deposit/withdrawal. Blocks on an"
      " authenticated account fetch; requires exchange credentials.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_cmd_sync,
  .parent_path = "whenmoon/market",
};

static const cmd_decl_t show_whenmoon_indicators_decl = {
  .module      = "whenmoon",
  .name        = "indicators",
  .usage       = "show whenmoon indicators <exch>-<base>-<quote> <gran>"
                 " latest",
  .description = "Print the latest closed bar's indicator block for the named"
                 " market and granularity (1m|5m|15m|1h|4h|1d). NaN slots"
                 " indicate insufficient history for that indicator's window.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_market_cmd_indicators,
  .parent_path = "show/whenmoon",
};

bool
wm_market_register_verbs(void)
{
  if(cmd_register(&whenmoon_market_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_market_start_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_market_stop_decl) != SUCCESS)
    return(FAIL);

  // WM-MK-2: per-market mode change. Refuses non-flat transitions to
  // keep paper / real ledgers from contaminating each other when the
  // market still holds a position. Persists via market_persist.
  if(cmd_register(&whenmoon_market_mode_decl) != SUCCESS)
    return(FAIL);

  // WM-MK-4: operator-issued forced trade. Bypasses strategy advisors
  // and the MANUAL mode "no auto-action" rule. Same fill-ledger choke
  // points as accepted strategy advice — apply_fill_locked for synth
  // modes, real_submit_locked (with the real-mode gates) for real mode.
  // An explicit px waives mark-staleness alone (OBS-62).
  if(cmd_register(&whenmoon_market_force_decl) != SUCCESS)
    return(FAIL);

  // WM-REAL-CASH-1: on-demand real-cash reconciliation. Binds the real
  // ledger to the live quote-currency balance so real order sizing
  // deploys actual funds; also runs implicitly on switching to real mode.
  if(cmd_register(&whenmoon_market_sync_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&show_whenmoon_indicators_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ====================================================================== //
// WM-MK-OBS-1: /show whenmoon market[s] [sessions|<id>] observability     //
// ====================================================================== //

// ------------------------------------------------------------------ //
// Shared table chrome — bold heading + a gray dash rule beneath       //
// ------------------------------------------------------------------ //
//
// One column descriptor drives both the heading and the rule so the two
// can never drift. Cells concatenate with no explicit separator — each
// column's width carries its own breathing room — so the rule lays
// (width-1) dashes per column, justified like the column, leaving the
// one-space seam that mirrors where the data padding falls.

typedef struct
{
  const char *label;
  int         width;
  bool        rjust;
} wm_tcol_t;

static void
wm_table_head(const cmd_ctx_t *ctx, const wm_tcol_t *cols, uint32_t n)
{
  char     line[512];
  char     cell[64];
  size_t   off;
  uint32_t i;

  off = (size_t)snprintf(line, sizeof(line), CLR_BOLD "  ");

  for(i = 0; i < n; i++)
  {
    snprintf(cell, sizeof(cell), "%s", cols[i].label);
    if(cols[i].rjust) display_align_right(cell, sizeof(cell), cols[i].width);
    else              display_align_left (cell, sizeof(cell), cols[i].width);
    off += (size_t)snprintf(line + off, sizeof(line) - off, "%s", cell);
  }

  snprintf(line + off, sizeof(line) - off, CLR_RESET);
  cmd_reply(ctx, line);

  off = (size_t)snprintf(line, sizeof(line), CLR_GRAY "  ");

  for(i = 0; i < n; i++)
  {
    int d = cols[i].width - 1;
    int j;

    if(d < 1)                       d = 1;
    if(d > (int)sizeof(cell) - 1)   d = (int)sizeof(cell) - 1;

    for(j = 0; j < d; j++)
      cell[j] = '-';
    cell[d] = '\0';

    if(cols[i].rjust) display_align_right(cell, sizeof(cell), cols[i].width);
    else              display_align_left (cell, sizeof(cell), cols[i].width);
    off += (size_t)snprintf(line + off, sizeof(line) - off, "%s", cell);
  }

  snprintf(line + off, sizeof(line) - off, CLR_RESET);
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// Market subscriptions table — `show whenmoon markets`                //
// ------------------------------------------------------------------ //
//
// One row per distinct (exchange, product) subscription — the N
// per-strategy sessions collapse onto their shared product. Each grain
// column is the % move from that grain's latest COMPLETED candle close
// to the live last-trade price.

#define WM_MKT_COL_MARKET   20   // "coinbase-btc-usd" = 16, room to spare
#define WM_MKT_COL_SESS      6    // attached-session count for this product
#define WM_MKT_COL_PRICE    13
#define WM_MKT_COL_GRAIN     9

static const wm_tcol_t wm_mkt_cols[] = {
  { "Market", WM_MKT_COL_MARKET, false },
  { "Sess",   WM_MKT_COL_SESS,   true  },
  { "Price",  WM_MKT_COL_PRICE,  true  },
  { "1m",     WM_MKT_COL_GRAIN,  true  },
  { "5m",     WM_MKT_COL_GRAIN,  true  },
  { "15m",    WM_MKT_COL_GRAIN,  true  },
  { "1h",     WM_MKT_COL_GRAIN,  true  },
  { "4h",     WM_MKT_COL_GRAIN,  true  },
  { "1d",     WM_MKT_COL_GRAIN,  true  },
};

#define WM_SUB_MAX  32

// One accumulated subscription row while the session walk dedups onto
// products. `ref[g]` is the close of the latest completed candle for
// grain g, chosen by the max `ref_ts[g]` across the product's sessions
// (their rings differ only in warmup depth; the newest bar is shared).
typedef struct
{
  char     base_id[WM_MARKET_ID_STR_SZ];  // "<exch>-<base>-<quote>", no @inst
  uint32_t n_sessions;                    // sessions collapsed onto this product
  double   last_px;
  int64_t  last_tick_ms;
  double   ref[WM_GRAN_MAX];
  int64_t  ref_ts[WM_GRAN_MAX];
} wm_sub_row_t;

static void
wm_obs_render_subscriptions(const cmd_ctx_t *ctx, whenmoon_state_t *st)
{
  whenmoon_markets_t *m = st->markets;
  wm_sub_row_t        rows[WM_SUB_MAX];
  uint32_t            n_rows = 0;
  uint32_t            i, g;

  if(m->n_markets == 0)
  {
    cmd_reply(ctx,
        "whenmoon: no markets configured"
        " (use /whenmoon market start <exch>-<base>-<quote>)");
    return;
  }

  // WM-MKT-ARR-UAF-1: rdlock across the dedup walk.
  pthread_rwlock_rdlock(&m->arr_lock);

  for(i = 0; i < m->n_markets; i++)
  {
    whenmoon_market_t *mk = m->arr[i];
    char     base[WM_MARKET_ID_STR_SZ];
    double   px;
    int64_t  tick_ms;
    double   cref[WM_GRAN_MAX];
    int64_t  cref_ts[WM_GRAN_MAX];
    uint32_t r;
    size_t   blen;

    // Product base id = market_id_str truncated at the '@instance' tail.
    blen = strcspn(mk->market_id_str, "@");
    if(blen >= sizeof(base))
      blen = sizeof(base) - 1;
    memcpy(base, mk->market_id_str, blen);
    base[blen] = '\0';

    pthread_mutex_lock(&mk->lock);
    px      = mk->last_px;
    tick_ms = mk->last_tick_ms;

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      uint32_t cn = mk->grain_n[g];

      if(cn > 0)
      {
        cref[g]    = mk->grain_arr[g][cn - 1].close;
        cref_ts[g] = mk->grain_arr[g][cn - 1].ts_close_ms;
      }
      else
      {
        cref[g]    = 0.0;
        cref_ts[g] = INT64_MIN;
      }
    }
    pthread_mutex_unlock(&mk->lock);

    // Find (or start) this product's row.
    for(r = 0; r < n_rows; r++)
      if(strcmp(rows[r].base_id, base) == 0)
        break;

    if(r == n_rows)
    {
      if(n_rows >= WM_SUB_MAX)
        continue;   // unreachable at trial scale; a silent cap is fine here

      snprintf(rows[r].base_id, sizeof(rows[r].base_id), "%s", base);
      rows[r].n_sessions   = 0;
      rows[r].last_px      = 0.0;
      rows[r].last_tick_ms = INT64_MIN;

      for(g = 0; g < WM_GRAN_MAX; g++)
      {
        rows[r].ref[g]    = 0.0;
        rows[r].ref_ts[g] = INT64_MIN;
      }

      n_rows++;
    }

    rows[r].n_sessions++;

    // Freshest tick wins the price; latest completed bar wins each grain.
    if(tick_ms >= rows[r].last_tick_ms)
    {
      rows[r].last_px      = px;
      rows[r].last_tick_ms = tick_ms;
    }

    for(g = 0; g < WM_GRAN_MAX; g++)
      if(cref_ts[g] > rows[r].ref_ts[g])
      {
        rows[r].ref[g]    = cref[g];
        rows[r].ref_ts[g] = cref_ts[g];
      }
  }

  pthread_rwlock_unlock(&m->arr_lock);

  wm_table_head(ctx, wm_mkt_cols,
      (uint32_t)(sizeof(wm_mkt_cols) / sizeof(wm_mkt_cols[0])));

  for(i = 0; i < n_rows; i++)
  {
    const wm_sub_row_t *rw = &rows[i];
    char   line[512];
    char   cell[48];
    char   price[32];
    size_t off;

    snprintf(cell, sizeof(cell), "%-*.*s", WM_MKT_COL_MARKET,
        WM_MKT_COL_MARKET, rw->base_id);
    off = (size_t)snprintf(line, sizeof(line),
        "  " CLR_CYAN "%s" CLR_RESET, cell);

    snprintf(cell, sizeof(cell), "%u", rw->n_sessions);
    display_align_right(cell, sizeof(cell), WM_MKT_COL_SESS);
    off += (size_t)snprintf(line + off, sizeof(line) - off, "%s", cell);

    if(rw->last_px > 0.0)
      snprintf(price, sizeof(price),
          CLR_BOLD CLR_WHITE "%.4f" CLR_RESET, rw->last_px);
    else
      snprintf(price, sizeof(price), CLR_GRAY "—" CLR_RESET);

    display_align_right(price, sizeof(price), WM_MKT_COL_PRICE);
    off += (size_t)snprintf(line + off, sizeof(line) - off, "%s", price);

    for(g = 0; g < WM_GRAN_MAX; g++)
    {
      if(rw->last_px > 0.0 && rw->ref[g] > 0.0)
        wm_fmt_pct((rw->last_px - rw->ref[g]) / rw->ref[g] * 100.0, 2,
            cell, sizeof(cell));
      else
        snprintf(cell, sizeof(cell), CLR_GRAY "—" CLR_RESET);

      display_align_right(cell, sizeof(cell), WM_MKT_COL_GRAIN);
      off += (size_t)snprintf(line + off, sizeof(line) - off, "%s", cell);
    }

    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// Session list table — column geometry                                //
// ------------------------------------------------------------------ //
//
// SESSION is left-justified (it carries the "<exch>-<base>-<quote>@inst"
// identity); every metric column is right-justified so the figures align
// under their heading. Widths are picked so the flagship 9-instance trial
// (e.g. "coinbase-btc-usd@juggernaut") fits without truncation.
#define WM_SES_COL_SESSION  28
#define WM_SES_COL_MODE      8   // "manual" = 6, "paper"/"real" shorter
#define WM_SES_COL_SIDE      6
#define WM_SES_COL_TRADES    7
#define WM_SES_COL_START    11
#define WM_SES_COL_CURRENT  12
#define WM_SES_COL_PL        9
#define WM_SES_COL_ENTRY    11
#define WM_SES_COL_VSENTRY   9

// Append one right-justified cell to `line` at `*off`, padding to width.
static void
wm_ses_append(char *line, size_t line_sz, size_t *off,
    char *cell, size_t cell_sz, int width)
{
  display_align_right(cell, cell_sz, width);
  *off += (size_t)snprintf(line + *off, line_sz - *off, "%s", cell);
}

// The mark price the open position is valued against: the freshest of the
// live ticker and the last strategy mark, falling back to entry so a
// just-restored session with no tick yet reads flat rather than -100%.
static double
wm_ses_mark_px(const wm_market_session_snapshot_t *snap)
{
  if(snap->last_ticker_px > 0.0)
    return(snap->last_ticker_px);
  if(snap->last_mark_px > 0.0)
    return(snap->last_mark_px);

  return(snap->position.avg_entry_px);
}

static const wm_tcol_t wm_ses_cols[] = {
  { "Session", WM_SES_COL_SESSION, false },
  { "Mode",    WM_SES_COL_MODE,    false },
  { "Side",    WM_SES_COL_SIDE,    true  },
  { "Trades",  WM_SES_COL_TRADES,  true  },
  { "Start",   WM_SES_COL_START,   true  },
  { "Current", WM_SES_COL_CURRENT, true  },
  { "P/L",     WM_SES_COL_PL,      true  },
  { "Entry",   WM_SES_COL_ENTRY,   true  },
  { "vsEntry", WM_SES_COL_VSENTRY, true  },
};

// Render the no-arg list-row for one market session. Metrics are drawn
// from the market's ACTIVE mode ledger (paper vs real); "Current" marks
// any open long to market so a session mid-position shows its true worth
// rather than its depleted cash.
static void
wm_obs_render_row(const cmd_ctx_t *ctx,
    const wm_market_session_snapshot_t *snap)
{
  const wm_market_stats_t *st  = &snap->stats[snap->mode];
  bool     is_long = (snap->position.side == WM_MARKET_POS_LONG);
  double   mark    = wm_ses_mark_px(snap);
  double   equity;
  double   pl_pct;
  char     line[512];
  char     cell[48];
  size_t   off;

  equity = st->cash;
  if(is_long)
    equity += snap->position.qty * mark;

  pl_pct = (st->starting_cash > 0.0)
      ? (equity - st->starting_cash) / st->starting_cash * 100.0 : 0.0;

  // Session id (cyan) — left-justified, hard-truncated to the column.
  snprintf(cell, sizeof(cell), "%-*.*s", WM_SES_COL_SESSION,
      WM_SES_COL_SESSION, snap->market_id_str);
  off = (size_t)snprintf(line, sizeof(line),
      "  " CLR_CYAN "%s" CLR_RESET, cell);

  // Mode — real money tints yellow to catch the eye; paper/manual muted.
  snprintf(cell, sizeof(cell), "%-*.*s", WM_SES_COL_MODE, WM_SES_COL_MODE,
      wm_market_mode_name(snap->mode));
  off += (size_t)snprintf(line + off, sizeof(line) - off, "%s%s" CLR_RESET,
      snap->mode == WM_MARKET_MODE_REAL ? CLR_YELLOW : CLR_GRAY, cell);

  // Side — long tints green, flat stays muted.
  if(is_long)
    snprintf(cell, sizeof(cell), CLR_GREEN "long" CLR_RESET);
  else
    snprintf(cell, sizeof(cell), CLR_GRAY "flat" CLR_RESET);
  wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_SIDE);

  snprintf(cell, sizeof(cell), "%u", st->n_trades);
  wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_TRADES);

  snprintf(cell, sizeof(cell), "%.2f", st->starting_cash);
  wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_START);

  snprintf(cell, sizeof(cell), CLR_BOLD CLR_WHITE "%.2f" CLR_RESET, equity);
  wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_CURRENT);

  wm_fmt_pct(pl_pct, 2, cell, sizeof(cell));
  wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_PL);

  // Entry + move-since-entry only carry meaning for an open long.
  if(is_long)
  {
    double vs_entry = (snap->position.avg_entry_px > 0.0)
        ? (mark - snap->position.avg_entry_px)
              / snap->position.avg_entry_px * 100.0 : 0.0;

    snprintf(cell, sizeof(cell), "%.4f", snap->position.avg_entry_px);
    wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_ENTRY);

    wm_fmt_pct(vs_entry, 2, cell, sizeof(cell));
    wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_VSENTRY);
  }
  else
  {
    snprintf(cell, sizeof(cell), CLR_GRAY "—" CLR_RESET);
    wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_ENTRY);
    snprintf(cell, sizeof(cell), CLR_GRAY "—" CLR_RESET);
    wm_ses_append(line, sizeof(line), &off, cell, sizeof(cell), WM_SES_COL_VSENTRY);
  }

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

// OBS-64: the outstanding orders, one line each. The orders are limit
// GTC, so an old row is not by itself a fault — it may be resting at the
// venue exactly as asked. This reports the age and leaves the judgement
// to the reader, which is the only honest split: nothing in the code can
// know what an implausible rest is for a given pair. The lever, when a
// row IS stuck, is `/whenmoon order cancel <exchange> <order-id>` — the
// venue's own CANCELLED event then reaps the row here.
static void
wm_obs_render_pending(const cmd_ctx_t *ctx,
    const wm_market_pending_view_t *rows, uint32_t n)
{
  int64_t  now_ms = wm_now_ms();
  uint32_t i;
  char     line[256];

  for(i = 0; i < n; i++)
  {
    const wm_market_pending_view_t *v = &rows[i];
    char                            age[32];

    if(v->submitted_ms > 0)
      wm_fmt_age(now_ms - v->submitted_ms, age, sizeof(age));
    else
      strlcpy(age, "?", sizeof(age));

    // Precisions on the two `%s` from the view are not decoration: the
    // rows come from a fixed-size array, so the compiler bounds an
    // unqualified `%s` by the whole array rather than one field.
    snprintf(line, sizeof(line),
        "    %-4.4s qty=%-12.8g px=%-10.4f filled=%-12.8g age=%-9s"
        " %s %s=%.63s",
        v->side, v->submitted_qty, v->limit_px, v->filled_qty, age,
        v->gateway_accepted ? "accepted" : "unacked ",
        v->order_id[0] != '\0' ? "ord"       : "coid",
        v->order_id[0] != '\0' ? v->order_id : v->coid);
    cmd_reply(ctx, line);
  }
}

// Render the detail card.
static void
wm_obs_render_card(const cmd_ctx_t *ctx,
    const wm_market_session_snapshot_t *snap,
    const wm_market_pending_view_t *pending, uint32_t n_pending)
{
  const wm_market_stats_t *paper = &snap->stats[WM_MARKET_MODE_PAPER];
  const wm_market_stats_t *real  = &snap->stats[WM_MARKET_MODE_REAL];
  char line[320];

  if(snap->position.side == WM_MARKET_POS_LONG)
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET "  mode=%s  warmup=%s  position: long"
        " qty=%.8g entry=%.4f opened_at_ms=%" PRId64,
        snap->market_id_str, wm_market_mode_name(snap->mode),
        wm_warmup_state_name(snap->warmup_state),
        snap->position.qty, snap->position.avg_entry_px,
        snap->position.opened_at_ms);
  else
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET "  mode=%s  warmup=%s  position: flat",
        snap->market_id_str, wm_market_mode_name(snap->mode),
        wm_warmup_state_name(snap->warmup_state));
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

  // WM-REAL-CASH-1: real cash is a seeded placeholder until reconciled
  // with the exchange. Flag it so the figure above is not mistaken for
  // actual funds; `/whenmoon market sync <id>` (or switching to real mode)
  // reconciles it.
  if(snap->real_cash_synced_ms == 0)
    cmd_reply(ctx,
        "               " CLR_RED "^ real cash UNSYNCED" CLR_RESET
        " — placeholder, not exchange funds (run /whenmoon market sync)");

  else
  {
    snprintf(line, sizeof(line),
        "               " CLR_GRAY "^ real cash reconciled at ms=%"
        PRId64 CLR_RESET, snap->real_cash_synced_ms);
    cmd_reply(ctx, line);
  }

  snprintf(line, sizeof(line),
      "  params:      fee_bps=%.1f slip_bps=%.1f size_frac=%.2f"
      " max_notional_sats=%.0f daily_dd_bps=%.1f pending_cap=%u",
      snap->fee_bps, snap->slip_bps, snap->size_frac,
      snap->max_notional_sats, snap->daily_drawdown_bps,
      snap->pending_cap);
  cmd_reply(ctx, line);

  // WM-NUMERAIRE-1: the book in the office's own unit — the figure the
  // drawdown breaker and the per-order cap are both evaluated against,
  // shown beside the quote-currency ledger above rather than instead of
  // it (CFO.md sec. 2: judge in one numeral, report in two).
  {
    wm_market_mode_t         book = (snap->mode == WM_MARKET_MODE_PAPER)
        ? WM_MARKET_MODE_PAPER : WM_MARKET_MODE_REAL;
    const wm_market_stats_t *bs   = &snap->stats[book];
    double                   pos  =
        (snap->position.side == WM_MARKET_POS_LONG)
            ? snap->position.qty : 0.0;
    // The mark is stamped by a signal or a fill; a market that has only
    // ever ticked has none, and its ticker is the honest valuation.
    double                   px   = (snap->last_mark_px > 0.0)
        ? snap->last_mark_px : snap->last_ticker_px;
    wm_btc_leg_t             leg  = wm_numeraire_leg(snap->product_id);
    double                   sats;

    if(wm_numeraire_stack_sats(leg, px, bs->cash, pos, &sats))
      snprintf(line, sizeof(line), "  stack:       %.0f sats (%s book)",
          sats, wm_market_mode_name(book));

    // Two different failures, and telling them apart is the whole
    // value of the line: one is a market this office cannot measure at
    // all, the other is a price that has not arrived yet.
    else if(leg == WM_BTC_LEG_NONE)
      snprintf(line, sizeof(line),
          "  stack:       " CLR_GRAY "unpriceable — %s has no bitcoin"
          " leg" CLR_RESET, snap->product_id);

    else
      snprintf(line, sizeof(line),
          "  stack:       " CLR_GRAY "unpriced — no mark or ticker"
          " observed yet" CLR_RESET);

    cmd_reply(ctx, line);
  }

  snprintf(line, sizeof(line),
      "  pending:     %u of %u", snap->pending_n, snap->pending_cap);
  cmd_reply(ctx, line);

  wm_obs_render_pending(ctx, pending, n_pending);

  wm_obs_render_fills(ctx, snap, WM_MARKET_MODE_PAPER,
      "  recent paper fills (oldest first):");
  wm_obs_render_fills(ctx, snap, WM_MARKET_MODE_REAL,
      "  recent real fills (oldest first):");
}

// Render the attached strategy for one market (at most one, WM-MI-3)
// with its live counters, last signal, and resolved per-market param
// values. The market-centric companion to /show whenmoon strategy
// <name> — answers "what is advising THIS market and what is each knob
// actually set to here". Param values carry a source tag:
// m=per-market override, g=global, d=schema default.
static void
wm_obs_render_strategies(const cmd_ctx_t *ctx, whenmoon_state_t *st,
    const char *market_id_str)
{
  wm_market_attach_snapshot_t snap;
  const wm_market_attach_snapshot_t *s = &snap;
  char     line[320];
  uint32_t off;
  uint32_t j;
  int      w;

  if(wm_strategy_snapshot_market(st, market_id_str, &snap, 1) == 0)
  {
    cmd_reply(ctx, "  strategy:  (none attached — feed-only)");
    return;
  }

  snprintf(line, sizeof(line),
      "  strategy:  " CLR_BOLD "%.*s" CLR_RESET
      "  bars_seen=%" PRIu64 " signals=%" PRIu64,
      (int)(sizeof(s->strategy_name) - 1),
      s->strategy_name, s->bars_seen, s->signals_emitted);
  cmd_reply(ctx, line);

  if(s->has_last_signal)
  {
    char    reason[WM_STRATEGY_REASON_SZ];
    size_t  rlen;

    rlen = strnlen(s->last_signal.reason, sizeof(reason) - 1);
    memcpy(reason, s->last_signal.reason, rlen);
    reason[rlen] = '\0';

    snprintf(line, sizeof(line),
        "        last_signal: score=%+.4f conf=%.4f reason=%s",
        s->last_signal.score, s->last_signal.confidence,
        reason[0] != '\0' ? reason : "(none)");
    cmd_reply(ctx, line);
  }

  if(s->n_params > 0)
  {
    // Pack resolved params onto one wrapped line: name=value(source).
    off = (uint32_t)snprintf(line, sizeof(line), "        params:");

    for(j = 0; j < s->n_params; j++)
    {
      const wm_market_strat_param_t *p = &s->params[j];

      if(off >= sizeof(line))
        break;

      w = snprintf(line + off, sizeof(line) - off, " %s=%s(%c)",
          p->name, p->value, p->source);

      if(w < 0)
        break;

      off += (uint32_t)w;
    }

    cmd_reply(ctx, line);
  }

  cmd_reply(ctx,
      CLR_GRAY "      param source: m=market-override g=global d=default"
      CLR_RESET);
}

// Render the full trading-session table (every instance, one row each).
static void
wm_obs_render_sessions(const cmd_ctx_t *ctx, whenmoon_markets_t *m)
{
  wm_market_session_snapshot_t snap;
  uint32_t                     i;

  if(m->n_markets == 0)
  {
    cmd_reply(ctx, "whenmoon: (no sessions running)");
    return;
  }

  wm_table_head(ctx, wm_ses_cols,
      (uint32_t)(sizeof(wm_ses_cols) / sizeof(wm_ses_cols[0])));

  // WM-MKT-ARR-UAF-1: rdlock across the list walk.
  pthread_rwlock_rdlock(&m->arr_lock);

  for(i = 0; i < m->n_markets; i++)
  {
    wm_market_session_snapshot(m->arr[i], &snap);
    wm_obs_render_row(ctx, &snap);
  }

  pthread_rwlock_unlock(&m->arr_lock);
}

// One handler behind both `markets` and `market`. The first argument
// selects the view:
//   (none)         → market subscriptions table (dedup onto products)
//   sessions | ses → the trading-session table
//   <id>           → a per-session detail card
static void
wm_show_market_cmd(const cmd_ctx_t *ctx)
{
  const char                  *p;
  char                         tok[WM_MARKET_ID_STR_SZ] = {0};
  whenmoon_state_t            *st;
  whenmoon_markets_t          *m;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  m = st->markets;
  p = ctx->args != NULL ? ctx->args : "";

  // No argument → the subscriptions overview.
  if(!wm_dl_next_token(&p, tok, sizeof(tok)))
  {
    wm_obs_render_subscriptions(ctx, st);
    return;
  }

  // `sessions` / `ses` → the trading-session table.
  if(strcasecmp(tok, "sessions") == 0 || strcasecmp(tok, "ses") == 0)
  {
    wm_obs_render_sessions(ctx, m);
    return;
  }

  // Otherwise treat the token as a session id → detail card.
  {
    wm_market_session_snapshot_t snap;
    wm_market_pending_view_t     pending[WM_MARKET_PENDING_CAP];
    whenmoon_market_t           *mk;
    uint32_t                     n_pending;
    char                         err[128];

    // WM-MKT-ARR-UAF-1: hold rdlock across lookup + snapshot (which reads
    // the session under mk->lock). The render below works off the local
    // `snap` copy + tok, so the lock is released first.
    pthread_rwlock_rdlock(&m->arr_lock);
    mk = wm_market_lookup_by_id(st, tok);

    if(mk == NULL)
    {
      pthread_rwlock_unlock(&m->arr_lock);
      snprintf(err, sizeof(err), "error: market %s not running", tok);
      cmd_reply(ctx, err);
      return;
    }

    wm_market_session_snapshot(mk, &snap);
    n_pending = wm_market_pending_snapshot(mk, pending,
        WM_MARKET_PENDING_CAP);
    pthread_rwlock_unlock(&m->arr_lock);

    wm_obs_render_card(ctx, &snap, pending, n_pending);
    wm_obs_render_strategies(ctx, st, tok);
  }
}

static const cmd_decl_t show_whenmoon_markets_decl = {
  .module      = "whenmoon",
  .name        = "markets",
  .usage       = "show whenmoon markets",
  .description =
      "Market subscriptions: one row per distinct (exchange, product)"
      " with the live last-trade price and, per candle grain (1m…1d),"
      " the % move from that grain's latest completed candle close.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_show_market_cmd,
  .parent_path = "show/whenmoon",
  .abbrev      = "mar",
};

static const cmd_decl_t show_whenmoon_market_decl = {
  .module      = "whenmoon",
  .name        = "market",
  .usage       = "show whenmoon market [sessions|<id>]",
  .description = "No arg: the subscriptions table (as `markets`)."
                 " `sessions` (abbr `ses`): every trading session with side,"
                 " trade count, starting vs current equity, total P/L, and the"
                 " entry price + move-from-entry for open longs."
                 " `<id>`: a per-session detail card plus recent fills tails.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_show_market_cmd,
  .parent_path = "show/whenmoon",
  .abbrev      = "mk",
};

bool
wm_show_market_register_verbs(void)
{
  // Subscriptions overview: `show whenmoon markets` (abbr `mar`).
  if(cmd_register(&show_whenmoon_markets_decl) != SUCCESS)
    return(FAIL);

  // Session table + detail card: `show whenmoon market [sessions|<id>]`
  // (abbr `mk`; `sessions` abbr `ses`). Shares the one handler above, so
  // `market` with no arg mirrors `markets`.
  if(cmd_register(&show_whenmoon_market_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
