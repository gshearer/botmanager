// botmanager — MIT
// whenmoon /bot <name> market start|stop verbs.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "aggregator.h"
#include "market.h"
#include "market_cmds.h"
#include "dl_commands.h"

#include "bot.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <inttypes.h>
#include <math.h>
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
// /show bot <name> indicators <pair> <gran> latest                    //
// ------------------------------------------------------------------ //

// Map a "1m"|"5m"|... token to a wm_gran_t. Returns FAIL on miss.
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

// Format one float slot — NaN renders as "  NaN" so columns line up.
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
  char                pair[64]    = {0};
  char                gran_tok[8] = {0};
  char                tail_tok[16] = {0};
  wm_gran_t           gran;
  wm_candle_full_t    bar;
  uint32_t            n;
  uint32_t            i;
  char                line[256];
  char                a[32];
  char                b[32];
  char                c[32];
  char                d[32];

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

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, pair, sizeof(pair)))
  {
    cmd_reply(ctx,
        "usage: show bot <name> indicators <exch>:<asset>:<cur>"
        " <1m|5m|15m|1h|6h|1d> latest");
    return;
  }

  if(wm_dl_parse_pair(pair, exch, sizeof(exch), base, sizeof(base),
         quote, sizeof(quote), symbol, sizeof(symbol)) != SUCCESS)
  {
    cmd_reply(ctx, "bad pair (expected <exch>:<asset>:<cur>)");
    return;
  }

  if(!wm_dl_next_token(&p, gran_tok, sizeof(gran_tok)))
  {
    cmd_reply(ctx,
        "usage: show bot <name> indicators <exch>:<asset>:<cur>"
        " <1m|5m|15m|1h|6h|1d> latest");
    return;
  }

  if(wm_market_parse_gran(gran_tok, &gran) != SUCCESS)
  {
    cmd_reply(ctx, "bad granularity (expected 1m|5m|15m|1h|6h|1d)");
    return;
  }

  // Optional `latest` keyword for forward-compat — older callers may
  // omit it; future verbs may take a numeric offset.
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
    snprintf(line, sizeof(line),
        "market %s not running on bot %s",
        symbol, st->bot_name);
    cmd_reply(ctx, line);
    return;
  }

  pthread_mutex_lock(&mk->lock);
  n = mk->grain_n[gran];

  if(n == 0 || mk->grain_arr[gran] == NULL)
  {
    pthread_mutex_unlock(&mk->lock);
    snprintf(line, sizeof(line),
        "%s %s: no bars yet", symbol, gran_tok);
    cmd_reply(ctx, line);
    return;
  }

  bar = mk->grain_arr[gran][n - 1];
  pthread_mutex_unlock(&mk->lock);

  snprintf(line, sizeof(line),
      CLR_BOLD "%s %s bar @ ms=%" PRId64 " (closed; bars=%u)" CLR_RESET,
      symbol, gran_tok, bar.ts_close_ms, n);
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
        " Starts: WS subscribe + live-ring 1m backfill."
        " Stops: unsubscribe + drop DB row.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_parent_cb, NULL, "bot", NULL,
        NULL, 0, wm_market_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "start",
        "bot <name> market start <exch>:<asset>:<cur>",
        "Start a market: WS subscribe, backfill the live ring (300 rows"
        " of 1m candles), and persist the assignment so it survives"
        " daemon restarts. History catch-up is not automatic — drive"
        " `/bot <name> download candles ...` if a deeper history is"
        " wanted.",
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

  if(cmd_register("whenmoon", "indicators",
        "show bot <name> indicators <exch>:<asset>:<cur> <gran> latest",
        "Print the latest closed bar's full indicator block for the"
        " named market and granularity (1m|5m|15m|1h|6h|1d). NaN slots"
        " indicate insufficient history for that indicator's window.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_market_cmd_indicators, NULL, "show/bot", NULL,
        NULL, 0, wm_market_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
