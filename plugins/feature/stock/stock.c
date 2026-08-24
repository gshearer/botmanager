// botmanager — MIT
// !stock command-surface plugin: parses flags, fetches quotes through the
// provider-neutral "stock_quotes" capability, and renders colorized
// tables, verbose cards (gauge + sparkline), and symbol searches.
#define STOCK_INTERNAL
#define STOCK_CMD_UNIT
#include "stock.h"

#include "colors.h"
#include "display.h"
#include "userns.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------------
// Column / string helpers (color-marker + UTF-8 aware)
// ----------------------------------------------------------------------

// ----------------------------------------------------------------------
// Value formatters
// ----------------------------------------------------------------------

// Price honoring the provider's decimal hint, else scaling precision to
// magnitude. Caller guarantees `p` is finite.
static void
stock_fmt_price(double p, uint8_t dec, char *buf, size_t sz)
{
  double a = p < 0.0 ? -p : p;

  if(dec > 0 && dec <= 8)
    snprintf(buf, sz, "%.*f", (int)dec, p);
  else if(a >= 1.0)
    snprintf(buf, sz, "%.2f", p);
  else if(a >= 0.01)
    snprintf(buf, sz, "%.4f", p);
  else if(a > 0.0)
    snprintf(buf, sz, "%.6f", p);
  else
    snprintf(buf, sz, "%.2f", p);
}

// Share volume with a K/M/B/T suffix (unit-agnostic; no currency).
static void
stock_fmt_vol(double v, char *buf, size_t sz)
{
  double a = v < 0.0 ? -v : v;

  if(a >= 1e12)     snprintf(buf, sz, "%.2fT", v / 1e12);
  else if(a >= 1e9) snprintf(buf, sz, "%.2fB", v / 1e9);
  else if(a >= 1e6) snprintf(buf, sz, "%.2fM", v / 1e6);
  else if(a >= 1e3) snprintf(buf, sz, "%.1fK", v / 1e3);
  else              snprintf(buf, sz, "%.0f",  v);
}

// Colored, arrowed percentage change for the table's Change column.
static void
stock_fmt_pct(double pct, char *buf, size_t sz)
{
  if(isnan(pct))
    snprintf(buf, sz, CLR_GRAY "—" CLR_RESET);
  else if(pct > 0.0)
    snprintf(buf, sz, CLR_GREEN "▲%.2f%%" CLR_RESET, pct);
  else if(pct < 0.0)
    snprintf(buf, sz, CLR_RED "▼%.2f%%" CLR_RESET, -pct);
  else
    snprintf(buf, sz, "0.00%%");
}

// Colored, arrowed absolute-and-percent delta for the verbose card,
// e.g. "▲+1.23 (+0.45%)". Used for both regular and extended sessions.
static void
stock_fmt_delta(double abs_chg, double pct, char *buf, size_t sz)
{
  const char *clr;
  const char *arrow;
  char        cbuf[24];
  char        pbuf[24];
  bool        up;
  bool        down;

  if(isnan(abs_chg) && isnan(pct))
  {
    snprintf(buf, sz, CLR_GRAY "—" CLR_RESET);
    return;
  }

  up   = (!isnan(abs_chg) && abs_chg > 0.0) || (!isnan(pct) && pct > 0.0);
  down = (!isnan(abs_chg) && abs_chg < 0.0) || (!isnan(pct) && pct < 0.0);

  if(up)        { clr = CLR_GREEN; arrow = "▲"; }
  else if(down) { clr = CLR_RED;   arrow = "▼"; }
  else          { clr = CLR_WHITE; arrow = "";  }

  if(!isnan(abs_chg)) snprintf(cbuf, sizeof(cbuf), "%+.2f", abs_chg);
  else                cbuf[0] = '\0';

  if(!isnan(pct)) snprintf(pbuf, sizeof(pbuf), "%+.2f%%", pct);
  else            pbuf[0] = '\0';

  if(cbuf[0] != '\0' && pbuf[0] != '\0')
    snprintf(buf, sz, "%s%s%s (%s)" CLR_RESET, clr, arrow, cbuf, pbuf);
  else
    snprintf(buf, sz, "%s%s%s%s" CLR_RESET, clr, arrow, cbuf, pbuf);
}

// "lo–hi" price range (en-dash), or a dim marker when either bound is
// absent.
static void
stock_fmt_range(double lo, double hi, uint8_t dec, char *buf, size_t sz)
{
  char lbuf[24];
  char hbuf[24];

  if(isnan(lo) || isnan(hi))
  {
    snprintf(buf, sz, CLR_GRAY "—" CLR_RESET);
    return;
  }

  stock_fmt_price(lo, dec, lbuf, sizeof(lbuf));
  stock_fmt_price(hi, dec, hbuf, sizeof(hbuf));
  snprintf(buf, sz, "%s–%s", lbuf, hbuf);
}

static void
stock_klass_str(quote_class_t k, char *buf, size_t sz)
{
  const char *s;

  switch(k)
  {
    case QUOTE_CLASS_EQUITY: s = "stock";  break;
    case QUOTE_CLASS_ETF:    s = "ETF";    break;
    case QUOTE_CLASS_FUND:   s = "fund";   break;
    case QUOTE_CLASS_INDEX:  s = "index";  break;
    case QUOTE_CLASS_FX:     s = "FX";     break;
    case QUOTE_CLASS_CRYPTO: s = "crypto"; break;
    case QUOTE_CLASS_FUTURE: s = "future"; break;
    default:                 s = "";       break;
  }

  snprintf(buf, sz, "%s", s);
}

// Human note for a per-symbol non-OK status (auth failures read as a
// generic outage rather than leaking the enrichment tier's mechanics).
static const char *
stock_status_note(quote_status_t st)
{
  switch(st)
  {
    case QUOTE_NOT_FOUND:    return("no data");
    case QUOTE_RATE_LIMITED: return("rate-limited");
    case QUOTE_OK:           return("");
    case QUOTE_AUTH:
    case QUOTE_TRANSPORT:
    case QUOTE_UNAVAILABLE:  return("unavailable");
  }

  return("unavailable");
}

// ----------------------------------------------------------------------
// Request factory — deep-copies the command context so it survives the
// async round-trip (copied from crypto_req_new).
// ----------------------------------------------------------------------

static stock_req_t *
stock_req_new(const cmd_ctx_t *ctx)
{
  stock_req_t *r = mem_alloc(STOCK_CTX, "req", sizeof(*r));

  memset(r, 0, sizeof(*r));
  r->ctx = *ctx;

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;

  return(r);
}

// ----------------------------------------------------------------------
// Table renderer
// ----------------------------------------------------------------------

static void
stock_row(const cmd_ctx_t *ctx, const quote_t *q)
{
  char line[STOCK_REPLY_SZ];
  char sym[32];
  char name[96];
  char price[48];
  char chg[64];
  char dayr[80];
  char yearr[88];
  char vol[40];

  if(q->status != QUOTE_OK)
  {
    snprintf(line, sizeof(line),
        " " CLR_YELLOW "%-8s" CLR_RESET " " CLR_GRAY "— %s" CLR_RESET,
        q->symbol, stock_status_note(q->status));
    cmd_reply(ctx, line);
    return;
  }

  snprintf(sym, sizeof(sym), CLR_YELLOW "%s" CLR_RESET, q->symbol);
  display_align_left(sym, sizeof(sym), 8);

  display_fit(q->name[0] != '\0' ? q->name : q->symbol, 18, name, sizeof(name), NULL);
  display_align_left(name, sizeof(name), 18);

  if(isnan(q->price))
    snprintf(price, sizeof(price), CLR_GRAY "—" CLR_RESET);
  else
  {
    char tmp[24];

    stock_fmt_price(q->price, q->price_decimals, tmp, sizeof(tmp));
    snprintf(price, sizeof(price), CLR_BOLD CLR_WHITE "%s" CLR_RESET, tmp);
  }
  display_align_right(price, sizeof(price), 10);

  stock_fmt_pct(q->change_pct, chg, sizeof(chg));
  display_align_right(chg, sizeof(chg), 11);

  stock_fmt_range(q->day_low, q->day_high, q->price_decimals,
      dayr, sizeof(dayr));
  display_align_right(dayr, sizeof(dayr), 19);

  stock_fmt_range(q->year_low, q->year_high, q->price_decimals,
      yearr, sizeof(yearr));
  display_align_right(yearr, sizeof(yearr), 21);

  if(isnan(q->volume))
    snprintf(vol, sizeof(vol), CLR_GRAY "—" CLR_RESET);
  else
    stock_fmt_vol(q->volume, vol, sizeof(vol));
  display_align_right(vol, sizeof(vol), 9);

  snprintf(line, sizeof(line), " %s %s %s %s %s %s %s",
      sym, name, price, chg, dayr, yearr, vol);
  cmd_reply(ctx, line);
}

static void
stock_reply_table(const cmd_ctx_t *ctx, const quote_batch_t *batch)
{
  static const char dashes[] = "----------------------------------------";
  char line[STOCK_REPLY_SZ];

  snprintf(line, sizeof(line),
      CLR_GRAY " %-8s %-18s %10s %11s %19s %21s %9s" CLR_RESET,
      "Symbol", "Name", "Price", "Change",
      "Day Range", "52-Week Range", "Volume");
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CLR_GRAY " %-8.8s %-18.18s %10.10s %11.11s %19.19s %21.21s %9.9s"
      CLR_RESET,
      dashes, dashes, dashes, dashes, dashes, dashes, dashes);
  cmd_reply(ctx, line);

  for(uint8_t i = 0; i < batch->n; i++)
    stock_row(ctx, &batch->quotes[i]);
}

// ----------------------------------------------------------------------
// Verbose card: identity, price, 52-week gauge, sparkline, day/volume,
// then caps-gated fundamentals + extended hours (dark until STOCK-4).
// ----------------------------------------------------------------------

// 52-week position gauge: a 20-cell bar filled to (price-low)/(high-low),
// green in the upper third / red in the lower / yellow between, with a
// cyan marker at the fill edge and the range printed on either side.
static void
stock_gauge(const cmd_ctx_t *ctx, const quote_t *q)
{
  char        bar[256] = "";
  char        line[STOCK_REPLY_SZ];
  char        lo[24];
  char        hi[24];
  const char *barclr;
  double      frac;
  int         filled;

  if(isnan(q->year_low) || isnan(q->year_high) || isnan(q->price)
      || q->year_high <= q->year_low)
    return;

  frac = (q->price - q->year_low) / (q->year_high - q->year_low);
  frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);

  filled = (int)(frac * STOCK_GAUGE_CELLS + 0.5);

  barclr = frac >= (2.0 / 3.0) ? CLR_GREEN
         : frac <= (1.0 / 3.0) ? CLR_RED
         : CLR_YELLOW;

  display_cat(bar, sizeof(bar), barclr);

  for(int i = 0; i < STOCK_GAUGE_CELLS; i++)
  {
    if(i + 1 == filled)
      display_cat(bar, sizeof(bar), CLR_CYAN);
    else if(i == filled)
      display_cat(bar, sizeof(bar), CLR_GRAY);

    display_cat(bar, sizeof(bar), i < filled ? "█" : "░");
  }

  stock_fmt_price(q->year_low,  q->price_decimals, lo, sizeof(lo));
  stock_fmt_price(q->year_high, q->price_decimals, hi, sizeof(hi));

  snprintf(line, sizeof(line),
      CLR_GRAY "%s" CLR_RESET " [%s" CLR_RESET "] " CLR_GRAY "%s  52w"
      CLR_RESET,
      lo, bar, hi);
  cmd_reply(ctx, line);
}

// Intraday sparkline: spark[] mapped to eight block levels over its own
// min..max, tinted by the session's direction.
static void
stock_sparkline(const cmd_ctx_t *ctx, const quote_t *q)
{
  static const char *const glyph[8] = {
    "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"
  };
  char        spark[8 * 32 + 8] = "";
  char        line[STOCK_REPLY_SZ];
  const char *clr;
  float       lo;
  float       hi;
  uint16_t    n;

  if(q->spark_n == 0)
    return;

  // Clamp the provider-supplied count to the inline array's capacity — a
  // misbehaving provider must never index past spark[].
  n = q->spark_n;
  if(n > (uint16_t)(sizeof(q->spark) / sizeof(q->spark[0])))
    n = (uint16_t)(sizeof(q->spark) / sizeof(q->spark[0]));

  lo = hi = q->spark[0];

  for(uint16_t i = 1; i < n; i++)
  {
    if(q->spark[i] < lo) lo = q->spark[i];
    if(q->spark[i] > hi) hi = q->spark[i];
  }

  for(uint16_t i = 0; i < n; i++)
  {
    int lvl = 0;

    if(hi > lo)
      lvl = (int)(((q->spark[i] - lo) / (hi - lo)) * 7.0f + 0.5f);

    if(lvl < 0) lvl = 0;
    if(lvl > 7) lvl = 7;

    display_cat(spark, sizeof(spark), glyph[lvl]);
  }

  clr = ((!isnan(q->change) && q->change < 0.0)
      || (!isnan(q->change_pct) && q->change_pct < 0.0))
      ? CLR_RED : CLR_GREEN;

  snprintf(line, sizeof(line), "%s%s" CLR_RESET, clr, spark);
  cmd_reply(ctx, line);
}

static void
stock_card_fundamentals(const cmd_ctx_t *ctx, const quote_t *q, uint32_t caps)
{
  char line[STOCK_REPLY_SZ];
  char cap[24];
  char eps[24];
  char pe[48];
  char div[24];
  bool any;

  // The provider advertises this block whenever its enrichment tier is
  // switched on, which is not the same as having reached it — a session
  // it could not mint leaves every field absent. Drawing "Mkt Cap —
  // P/E —/— EPS — Div —" says nothing, so draw nothing; the depth line
  // below already guards itself the same way.
  any = !isnan(q->market_cap) || !isnan(q->pe_trailing)
      || !isnan(q->pe_forward) || !isnan(q->eps_ttm)
      || !isnan(q->dividend_yield);

  if(any)
  {
    char t[16];
    char f[16];

    if(!isnan(q->market_cap)) stock_fmt_vol(q->market_cap, cap, sizeof(cap));
    else                      snprintf(cap, sizeof(cap), "—");

    if(!isnan(q->eps_ttm)) snprintf(eps, sizeof(eps), "%.2f", q->eps_ttm);
    else                   snprintf(eps, sizeof(eps), "—");

    if(!isnan(q->pe_trailing)) snprintf(t, sizeof(t), "%.1f", q->pe_trailing);
    else                       snprintf(t, sizeof(t), "—");

    if(!isnan(q->pe_forward)) snprintf(f, sizeof(f), "%.1f", q->pe_forward);
    else                      snprintf(f, sizeof(f), "—");

    snprintf(pe, sizeof(pe), "%s/%s", t, f);

    if(!isnan(q->dividend_yield))
      snprintf(div, sizeof(div), "%.2f%%", q->dividend_yield);
    else
      snprintf(div, sizeof(div), "—");

    snprintf(line, sizeof(line),
        CLR_GRAY "Mkt Cap" CLR_RESET " %s   " CLR_GRAY "P/E" CLR_RESET " %s   "
        CLR_GRAY "EPS" CLR_RESET " %s   " CLR_GRAY "Div" CLR_RESET " %s",
        cap, pe, eps, div);
    cmd_reply(ctx, line);
  }

  if((caps & QUOTE_CAP_DEPTH) && (!isnan(q->bid) || !isnan(q->ask)))
  {
    char bid[24];
    char ask[24];

    if(!isnan(q->bid)) stock_fmt_price(q->bid, q->price_decimals, bid, sizeof(bid));
    else               snprintf(bid, sizeof(bid), "—");

    if(!isnan(q->ask)) stock_fmt_price(q->ask, q->price_decimals, ask, sizeof(ask));
    else               snprintf(ask, sizeof(ask), "—");

    snprintf(line, sizeof(line),
        CLR_GRAY "Bid" CLR_RESET " %s   " CLR_GRAY "Ask" CLR_RESET " %s",
        bid, ask);
    cmd_reply(ctx, line);
  }
}

static void
stock_card_exthours(const cmd_ctx_t *ctx, const quote_t *q)
{
  char        line[STOCK_REPLY_SZ];
  char        pbuf[24];
  char        cbuf[48];
  const char *label;

  if(q->market_state != MARKET_STATE_PRE && q->market_state != MARKET_STATE_POST)
    return;

  if(isnan(q->session_price))
    return;

  label = q->market_state == MARKET_STATE_PRE ? "Pre-market" : "After-hours";

  stock_fmt_price(q->session_price, q->price_decimals, pbuf, sizeof(pbuf));
  stock_fmt_delta(q->session_change, q->session_change_pct, cbuf, sizeof(cbuf));

  snprintf(line, sizeof(line),
      CLR_GRAY "%s" CLR_RESET " " CLR_BOLD CLR_WHITE "%s" CLR_RESET " %s",
      label, pbuf, cbuf);
  cmd_reply(ctx, line);
}

static void
stock_reply_card(const cmd_ctx_t *ctx, const quote_t *q)
{
  char     line[STOCK_REPLY_SZ];
  char     klass[16];
  uint32_t caps;

  if(q->status != QUOTE_OK)
  {
    snprintf(line, sizeof(line), "%s — %s",
        q->symbol, stock_status_note(q->status));
    cmd_reply(ctx, line);
    return;
  }

  caps = stockquote_provider_caps();
  stock_klass_str(q->klass, klass, sizeof(klass));

  // line 1 — identity
  {
    char meta[80];

    if(q->exchange[0] != '\0' && klass[0] != '\0')
      snprintf(meta, sizeof(meta), " %s · %s", q->exchange, klass);
    else if(q->exchange[0] != '\0')
      snprintf(meta, sizeof(meta), " %s", q->exchange);
    else if(klass[0] != '\0')
      snprintf(meta, sizeof(meta), " %s", klass);
    else
      meta[0] = '\0';

    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET " (" CLR_YELLOW "%s" CLR_RESET ")"
        CLR_GRAY "%s" CLR_RESET,
        q->name[0] != '\0' ? q->name : q->symbol, q->symbol, meta);
    cmd_reply(ctx, line);
  }

  // line 2 — price + change
  {
    char pbuf[24];
    char cbuf[48];

    if(isnan(q->price))
      snprintf(pbuf, sizeof(pbuf), "—");
    else
      stock_fmt_price(q->price, q->price_decimals, pbuf, sizeof(pbuf));

    stock_fmt_delta(q->change, q->change_pct, cbuf, sizeof(cbuf));

    snprintf(line, sizeof(line),
        "Price: " CLR_BOLD CLR_WHITE "%s" CLR_RESET "%s%s  %s",
        pbuf, q->currency[0] != '\0' ? " " : "", q->currency, cbuf);
    cmd_reply(ctx, line);
  }

  stock_gauge(ctx, q);
  stock_sparkline(ctx, q);

  // day range + volume
  {
    char dayr[64];
    char vol[24];

    stock_fmt_range(q->day_low, q->day_high, q->price_decimals,
        dayr, sizeof(dayr));

    if(isnan(q->volume))
      snprintf(vol, sizeof(vol), "—");
    else
      stock_fmt_vol(q->volume, vol, sizeof(vol));

    snprintf(line, sizeof(line),
        CLR_GRAY "Day" CLR_RESET " %s   " CLR_GRAY "Vol" CLR_RESET " %s",
        dayr, vol);
    cmd_reply(ctx, line);
  }

  if(caps & QUOTE_CAP_FUNDAMENTALS)
    stock_card_fundamentals(ctx, q, caps);

  if(caps & QUOTE_CAP_EXTHOURS)
    stock_card_exthours(ctx, q);
}

// ----------------------------------------------------------------------
// Search renderer
// ----------------------------------------------------------------------

static void
stock_reply_search(const cmd_ctx_t *ctx, const quote_search_res_t *res,
    const char *query)
{
  char line[STOCK_REPLY_SZ];

  if(res->status == QUOTE_RATE_LIMITED)
  {
    cmd_reply(ctx, CLR_ORANGE
        "Quotes are rate-limited right now — try again shortly." CLR_RESET);
    return;
  }

  if(res->status != QUOTE_OK)
  {
    cmd_reply(ctx, "Symbol search is unavailable right now — sorry.");
    return;
  }

  if(res->n == 0)
  {
    snprintf(line, sizeof(line), "No matches for '%s'.", query);
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line), CLR_BOLD "Matches for '%s':" CLR_RESET, query);
  cmd_reply(ctx, line);

  for(uint8_t i = 0; i < res->n; i++)
  {
    const quote_hit_t *h = &res->hits[i];
    char               sym[32];
    char               name[96];
    char               klass[16];
    char               meta[96];

    snprintf(sym, sizeof(sym), CLR_YELLOW "%s" CLR_RESET, h->symbol);
    display_align_left(sym, sizeof(sym), 8);

    display_fit(h->name[0] != '\0' ? h->name : "—", 28, name, sizeof(name), NULL);
    display_align_left(name, sizeof(name), 28);

    stock_klass_str(h->klass, klass, sizeof(klass));

    if(klass[0] != '\0' && h->exchange[0] != '\0')
      snprintf(meta, sizeof(meta), "%s · %s", klass, h->exchange);
    else if(klass[0] != '\0')
      snprintf(meta, sizeof(meta), "%s", klass);
    else if(h->exchange[0] != '\0')
      snprintf(meta, sizeof(meta), "%s", h->exchange);
    else
      meta[0] = '\0';

    if(meta[0] != '\0')
      snprintf(line, sizeof(line),
          "  %s %s " CLR_GRAY "(%s)" CLR_RESET, sym, name, meta);
    else
      snprintf(line, sizeof(line), "  %s %s", sym, name);

    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// Async completion callbacks (fire on the curl worker thread)
// ----------------------------------------------------------------------

static void
stock_batch_done(const quote_batch_t *batch, void *user)
{
  stock_req_t *r   = (stock_req_t *)user;
  cmd_ctx_t    ctx = r->ctx;

  ctx.msg = &r->msg;

  // batch->status is dispatch-level; a well-behaved provider reports real
  // outcomes per-symbol. These branches defend against a provider that
  // signals a whole-batch failure up front.
  if(batch->status == QUOTE_RATE_LIMITED)
    cmd_reply(&ctx, CLR_ORANGE
        "Quotes are rate-limited right now — try again shortly." CLR_RESET);
  else if(batch->status != QUOTE_OK)
    cmd_reply(&ctx, "The quote service is unavailable right now — sorry.");
  else if(r->verbose && batch->n == 1)
    stock_reply_card(&ctx, &batch->quotes[0]);
  else
    stock_reply_table(&ctx, batch);

  mem_free(r);
}

static void
stock_search_done(const quote_search_res_t *res, void *user)
{
  stock_req_t *r   = (stock_req_t *)user;
  cmd_ctx_t    ctx = r->ctx;

  ctx.msg = &r->msg;

  stock_reply_search(&ctx, res, r->query);
  mem_free(r);
}

// ----------------------------------------------------------------------
// Argument parsing
// ----------------------------------------------------------------------

static bool
stock_parse(const cmd_ctx_t *ctx, const char *args, stock_args_t *out)
{
  char  buf[STOCK_REPLY_SZ];
  char *save = NULL;
  char *tok;

  memset(out, 0, sizeof(*out));

  if(args == NULL || args[0] == '\0')
    return(true);

  snprintf(buf, sizeof(buf), "%s", args);

  tok = strtok_r(buf, " \t", &save);

  while(tok != NULL)
  {
    char *sp2;
    char *piece;

    if(strcmp(tok, "-v") == 0 || strcmp(tok, "--verbose") == 0)
    {
      out->verbose = true;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(strcmp(tok, "-s") == 0 || strcmp(tok, "--search") == 0)
    {
      const char *rest = save;   // everything after -s is the free-text query

      out->search = true;

      while(rest != NULL && (*rest == ' ' || *rest == '\t'))
        rest++;

      if(rest != NULL)
        snprintf(out->query, sizeof(out->query), "%s", rest);

      break;
    }

    if(strcmp(tok, "-l") == 0 || strcmp(tok, "--list") == 0)
    {
      out->op = STOCK_OP_SHOW;
      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    if(strcmp(tok, "-a") == 0 || strcmp(tok, "--add") == 0 ||
       strcmp(tok, "-d") == 0 || strcmp(tok, "--del") == 0)
    {
      const bool add = (strcmp(tok, "-a") == 0 || strcmp(tok, "--add") == 0);
      char      *name;

      out->op = add ? STOCK_OP_ADD : STOCK_OP_DEL;
      name    = strtok_r(NULL, " \t", &save);

      if(name == NULL)
      {
        cmd_reply(ctx, add
            ? "Usage: !stock --add <list> <symbols…>"
            : "Usage: !stock --del <list> <symbols…>");
        return(false);
      }

      // A leading '@' is how lists are referenced elsewhere; accept it
      // here too rather than making the user remember where it belongs.
      if(name[0] == '@')
        name++;

      for(size_t j = 0; name[j] != '\0' && j + 1 < sizeof(out->list); j++)
        out->list[j] = (char)tolower((unsigned char)name[j]);

      tok = strtok_r(NULL, " \t", &save);
      continue;
    }

    // bare token: one or more comma-separated symbols or '@list' refs
    sp2   = NULL;
    piece = strtok_r(tok, ",", &sp2);

    while(piece != NULL)
    {
      if(out->nitems >= STOCK_MAX_SYMS)
      {
        cmd_reply(ctx, "Too many symbols (max 24).");
        return(false);
      }

      if(piece[0] != '\0')
      {
        stock_item_t *it = &out->items[out->nitems];
        size_t        j  = 0;

        it->is_list = (piece[0] == '@');

        if(it->is_list)
          piece++;

        // List names fold down, symbols fold up; both are bounded by the
        // destination, which is the wider of the two.
        for(; piece[j] != '\0' && j + 1 < sizeof(it->text); j++)
          it->text[j] = it->is_list
              ? (char)tolower((unsigned char)piece[j])
              : (char)toupper((unsigned char)piece[j]);

        it->text[j] = '\0';

        if(it->text[0] != '\0')
          out->nitems++;
      }

      piece = strtok_r(NULL, ",", &sp2);
    }

    tok = strtok_r(NULL, " \t", &save);
  }

  return(true);
}

// ----------------------------------------------------------------------
// Symbol lists
// ----------------------------------------------------------------------

// Resolves items into out->syms, expanding '@name' in place so the
// rendered order follows the order the user typed. Replies and returns
// false on anything the caller should not proceed past.
static bool
stock_expand(const cmd_ctx_t *ctx, stock_args_t *a)
{
  userns_t *ns = NULL;
  char      line[STOCK_REPLY_SZ];
  uint8_t   i;

  for(i = 0; i < a->nitems; i++)
  {
    const stock_item_t *it = &a->items[i];
    stock_symset_t      set;
    stock_list_rc_t     rc;
    uint8_t             j;

    if(!it->is_list)
    {
      if(a->nsyms >= STOCK_MAX_SYMS)
      {
        cmd_reply(ctx, "Too many symbols (max 24).");
        return(false);
      }

      snprintf(a->syms[a->nsyms], STOCK_SYM_SZ, "%s", it->text);
      a->nsyms++;
      continue;
    }

    // A list reference and a single-symbol card are different intents;
    // pick the first symbol for the user and we would be guessing.
    if(a->verbose)
    {
      cmd_reply(ctx, "Verbose mode shows one symbol, not a list. "
          "Example: !stock -v NVDA");
      return(false);
    }

    if(ns == NULL && (ns = userns_session_resolve(ctx)) == NULL)
      return(false);   // the resolver already replied

    rc = stock_lists_get(ns->id, it->text, &set);

    if(rc == STOCK_LIST_NOSUCH)
    {
      snprintf(line, sizeof(line),
          "No list called '%s'. Try !stock --list", it->text);
      cmd_reply(ctx, line);
      return(false);
    }

    if(rc != STOCK_LIST_OK)
    {
      cmd_reply(ctx, "Couldn't read your lists right now — sorry.");
      return(false);
    }

    for(j = 0; j < set.n; j++)
    {
      if(a->nsyms >= STOCK_MAX_SYMS)
      {
        cmd_reply(ctx, "Too many symbols (max 24).");
        return(false);
      }

      snprintf(a->syms[a->nsyms], STOCK_SYM_SZ, "%s", set.sym[j]);
      a->nsyms++;
    }
  }

  return(true);
}

// --list / --add / --del. Lists are userns-scoped, so every operation
// needs a namespace; mutating one additionally needs a known user.
static void
stock_list_cmd(const cmd_ctx_t *ctx, const stock_args_t *a)
{
  userns_t *ns = userns_session_resolve(ctx);
  char      line[STOCK_REPLY_SZ];
  char      names[STOCK_REPLY_SZ - 64];   // leaves room for the heading
  uint32_t  count   = 0;
  uint8_t   changed = 0;
  uint8_t   total   = 0;
  bool      dropped = false;
  stock_list_rc_t rc;

  if(ns == NULL)   // the resolver already replied
    return;

  if(a->op == STOCK_OP_SHOW)
  {
    if(stock_lists_names(ns->id, names, sizeof(names), &count)
        != STOCK_LIST_OK)
    {
      cmd_reply(ctx, "Couldn't read your lists right now — sorry.");
      return;
    }

    if(count == 0)
    {
      cmd_reply(ctx, "No stock lists yet. "
          "Make one: !stock --add tech AAPL MSFT NVDA");
      return;
    }

    snprintf(line, sizeof(line), CLR_BOLD "Your stock lists:" CLR_RESET
        " %s", names);
    cmd_reply(ctx, line);
    return;
  }

  if(ctx->username == NULL || ctx->username[0] == '\0')
  {
    cmd_reply(ctx, "Lists belong to a user — identify first, "
        "then you can change one.");
    return;
  }

  if(a->list[0] == '\0' || !stock_list_name_ok(a->list))
  {
    cmd_reply(ctx, "List names are up to 32 letters, digits, '-' or '_'.");
    return;
  }

  if(a->nitems == 0)
  {
    cmd_reply(ctx, (a->op == STOCK_OP_ADD)
        ? "Add what? Example: !stock --add tech AAPL MSFT"
        : "Remove what? Example: !stock --del tech MSFT");
    return;
  }

  for(uint8_t i = 0; i < a->nitems; i++)
  {
    if(!a->items[i].is_list)
      continue;

    cmd_reply(ctx, "Lists hold symbols, not other lists.");
    return;
  }

  if(a->op == STOCK_OP_ADD)
  {
    rc = stock_lists_add(ns->id, a->list, a->items, a->nitems,
        &changed, &total);

    if(rc == STOCK_LIST_FULL)
    {
      snprintf(line, sizeof(line),
          "@%s holds %u of %d symbols — that batch won't fit, so "
          "nothing was added. Free some: !stock --del %s <symbol>",
          a->list, (unsigned)total, STOCK_LIST_MAX, a->list);
      cmd_reply(ctx, line);
      return;
    }

    if(rc == STOCK_LIST_BADSYM)
    {
      cmd_reply(ctx, "That doesn't look like a ticker. "
          "Letters, digits and ^ - . = only.");
      return;
    }

    if(rc != STOCK_LIST_OK)
    {
      cmd_reply(ctx, "Couldn't save that list right now — sorry.");
      return;
    }

    snprintf(line, sizeof(line),
        CLR_GREEN "%s" CLR_RESET " @%s — %u symbol%s (view: !stock @%s)",
        (changed > 0) ? "Saved" : "No change", a->list, (unsigned)total,
        (total == 1) ? "" : "s", a->list);
    cmd_reply(ctx, line);
    return;
  }

  rc = stock_lists_del(ns->id, a->list, a->items, a->nitems,
      &changed, &dropped);

  if(rc == STOCK_LIST_NOSUCH)
  {
    snprintf(line, sizeof(line),
        "No list called '%s'. Try !stock --list", a->list);
    cmd_reply(ctx, line);
    return;
  }

  if(rc != STOCK_LIST_OK)
  {
    cmd_reply(ctx, "Couldn't update that list right now — sorry.");
    return;
  }

  if(changed == 0)
  {
    snprintf(line, sizeof(line), "Nothing in @%s matched.", a->list);
    cmd_reply(ctx, line);
    return;
  }

  if(dropped)
    snprintf(line, sizeof(line), CLR_GREEN "Removed" CLR_RESET
        " the last symbol — @%s is gone.", a->list);
  else
    snprintf(line, sizeof(line), CLR_GREEN "Removed" CLR_RESET
        " %u from @%s.", (unsigned)changed, a->list);

  cmd_reply(ctx, line);
}

// ----------------------------------------------------------------------
// Command callback
// ----------------------------------------------------------------------

static void
stock_cmd(const cmd_ctx_t *ctx)
{
  stock_args_t  a;
  const char   *symv[STOCK_MAX_SYMS];
  stock_req_t  *r;
  uint32_t      caps;

  if(!stock_parse(ctx, ctx->args, &a))
    return;

  // List management never touches the quote provider.
  if(a.op != STOCK_OP_NONE)
  {
    stock_list_cmd(ctx, &a);
    return;
  }

  caps = stockquote_provider_caps();

  // Free-text symbol search.
  if(a.search)
  {
    if(a.query[0] == '\0')
    {
      cmd_reply(ctx, "Search for what? Example: !stock -s tesla");
      return;
    }

    if(!(caps & QUOTE_CAP_SEARCH))
    {
      cmd_reply(ctx, "Symbol search isn't available right now.");
      return;
    }

    r = stock_req_new(ctx);
    snprintf(r->query, sizeof(r->query), "%s", a.query);

    if(stockquote_search_async(a.query, stock_search_done, r)
        == ASYNC_FAILED_UNDELIVERED)
    {
      cmd_reply(ctx,
          "Couldn't reach the quote service — try again shortly.");
      mem_free(r);
    }

    return;
  }

  if(!stock_expand(ctx, &a))
    return;

  if(a.nsyms == 0)
  {
    cmd_reply(ctx,
        "Usage: !stock [-v] <@list|symbol…>  |  !stock -s <search words>"
        "  |  !stock --list");
    return;
  }

  if(caps == 0)
  {
    cmd_reply(ctx, "No quote provider is loaded right now.");
    return;
  }

  if(a.verbose && a.nsyms != 1)
  {
    cmd_reply(ctx, "Verbose mode shows one symbol. Example: !stock -v NVDA");
    return;
  }

  for(uint8_t i = 0; i < a.nsyms; i++)
    symv[i] = a.syms[i];

  r = stock_req_new(ctx);
  r->verbose = a.verbose;

  if(stockquote_fetch_async(symv, a.nsyms, stock_batch_done, r)
      == ASYNC_FAILED_UNDELIVERED)
  {
    cmd_reply(ctx, "Couldn't reach the quote service — try again shortly.");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// NL hints
// ----------------------------------------------------------------------

static const cmd_nl_slot_t stock_nl_slots[] = {
  { .name  = "symbol",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t stock_nl_examples[] = {
  { .utterance  = "how's apple stock",
    .invocation = "/stock AAPL" },
  { .utterance  = "s&p 500 right now",
    .invocation = "/stock ^GSPC" },
  { .utterance  = "quote tsla and nvda",
    .invocation = "/stock TSLA NVDA" },
};

static const cmd_nl_t stock_nl = {
  .when          = "User asks for a stock / ETF / index / fund / FX / "
                   "commodity price or ticker quote.",
  .syntax        = "/stock <symbols…>",
  .slots         = stock_nl_slots,
  .slot_count    = (uint8_t)(sizeof(stock_nl_slots)
                             / sizeof(stock_nl_slots[0])),
  .examples      = stock_nl_examples,
  .example_count = (uint8_t)(sizeof(stock_nl_examples)
                             / sizeof(stock_nl_examples[0])),
};

// ----------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------

static const cmd_decl_t stock_decl = {
  .module      = STOCK_CTX,
  .name        = "stock",
  .usage       = "stock [-v] <@list|symbol…> | stock -s <words> | stock --list"
                 " | stock --add|--del <list> <symbols…>",
  .description = "Stock, ETF, fund, index, FX and commodity quotes",
  .help_long   =
      "Quote one or more symbols, or a saved list with @name. "
      "--list shows your lists; --add creates or appends to one and "
      "--del removes symbols (a list disappears when its last symbol "
      "does). Lists are private to your namespace and holding or "
      "changing one requires a known user.",
  .group       = "everyone",
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = stock_cmd,
  .abbrev      = "$",
  .nl          = &stock_nl,
};

static bool
stock_init(void)
{
  if(cmd_register(&stock_decl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, STOCK_CTX, "stock command plugin initialized");

  return(SUCCESS);
}

// Schema bootstrap runs in start(), after the DB plugin is up. Every list
// entry point re-ensures it anyway, so a database that arrives later
// still yields working lists without a reload.
static bool
stock_start(void)
{
  if(stock_lists_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, STOCK_CTX,
        "list schema init failed (lists will error until the DB is up)");

  return(SUCCESS);
}

static void
stock_deinit(void)
{
  cmd_unregister_path("stock");

  clam(CLAM_INFO, STOCK_CTX, "stock command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "stock",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "stock",
  .provides        = { { .name = "cmd_stock" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" },
                       { .name = "stock_quotes" } },
  .requires_count  = 2,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = stock_init,
  .start           = stock_start,
  .stop            = NULL,
  .deinit          = stock_deinit,
  .ext             = NULL,
};
