// botmanager — MIT
// whenmoon market verbs (state-changing under /whenmoon market;
// observability under /show whenmoon).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "market.h"
#include "market_cmds.h"
#include "dl_commands.h"

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Build the coinbase wire-form symbol "BASE-QUOTE" (uppercase) from
// already-parsed lowercase base/quote tokens.
static void
wm_market_wire_symbol(const char *base, const char *quote,
    char *out, size_t out_sz)
{
  size_t i;
  int    n;

  if(out == NULL || out_sz == 0)
    return;

  n = snprintf(out, out_sz, "%s-%s", base, quote);

  if(n < 0 || (size_t)n >= out_sz)
  {
    out[0] = '\0';
    return;
  }

  for(i = 0; out[i] != '\0'; i++)
    out[i] = (char)toupper((unsigned char)out[i]);
}

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
  char              symbol[COINBASE_PRODUCT_ID_SZ];
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
  char              symbol[COINBASE_PRODUCT_ID_SZ];
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
  if(strcmp(tok, "6h")  == 0) { *out = WM_GRAN_6H;  return(SUCCESS); }
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
  whenmoon_markets_t *m;
  whenmoon_market_t  *mk = NULL;
  const char         *p;
  char                exch[32];
  char                base[16];
  char                quote[16];
  char                symbol[COINBASE_PRODUCT_ID_SZ];
  char                id_tok[64] = {0};
  char                gran_tok[8] = {0};
  char                tail_tok[16] = {0};
  char                id_str[WM_MARKET_ID_STR_SZ];
  wm_gran_t           gran;
  wm_candle_full_t    bar;
  uint32_t            n;
  uint32_t            i;
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
        " <1m|5m|15m|1h|6h|1d> latest");
    return;
  }

  if(wm_market_parse_id(id_tok, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote)) != SUCCESS)
  {
    cmd_reply(ctx, "bad market id (expected <exch>-<base>-<quote>)");
    return;
  }

  wm_market_wire_symbol(base, quote, symbol, sizeof(symbol));
  wm_market_format_id(exch, base, quote, id_str, sizeof(id_str));

  if(!wm_dl_next_token(&p, gran_tok, sizeof(gran_tok)))
  {
    cmd_reply(ctx,
        "usage: /show whenmoon indicators <exch>-<base>-<quote>"
        " <1m|5m|15m|1h|6h|1d> latest");
    return;
  }

  if(wm_market_parse_gran(gran_tok, &gran) != SUCCESS)
  {
    cmd_reply(ctx, "bad granularity (expected 1m|5m|15m|1h|6h|1d)");
    return;
  }

  // Optional `latest` keyword for forward-compat.
  (void)wm_dl_next_token(&p, tail_tok, sizeof(tail_tok));

  m = st->markets;

  for(i = 0; i < m->n_markets; i++)
  {
    if(strncmp(m->arr[i].product_id, symbol,
           COINBASE_PRODUCT_ID_SZ) == 0)
    {
      mk = &m->arr[i];
      break;
    }
  }

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
// Parent stub                                                         //
// ------------------------------------------------------------------ //

static void
wm_market_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon market <start|stop> <exch>-<base>-<quote>");
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

bool
wm_market_register_verbs(void)
{
  if(cmd_register("whenmoon", "market",
        "whenmoon market <start|stop> <exch>-<base>-<quote>",
        "Add or remove a live market."
        " Starts: WS subscribe + live-ring 1m backfill."
        " Stops: unsubscribe + clear enabled flag.",
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
        " automatic — drive `/whenmoon download candles ...` for a"
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

  if(cmd_register("whenmoon", "indicators",
        "show whenmoon indicators <exch>-<base>-<quote> <gran> latest",
        "Print the latest closed bar's indicator block for the named"
        " market and granularity (1m|5m|15m|1h|6h|1d). NaN slots"
        " indicate insufficient history for that indicator's window.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_indicators, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
