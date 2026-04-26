// botmanager — MIT
// /crypto command-surface plugin: parses flags, queries the
// coinmarketcap service plugin via its public API, and formats
// user-facing replies (table / verbose / global).
#define CRYPTO_INTERNAL
#include "crypto.h"

#include "colors.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Formatting helpers

static int
crypto_fmt_number(double val, char *buf, size_t sz)
{
  double abs_val = val < 0 ? -val : val;

  if(abs_val >= 1e12) return(snprintf(buf, sz, "$%.1fT", val / 1e12));
  if(abs_val >= 1e9)  return(snprintf(buf, sz, "$%.1fB", val / 1e9));
  if(abs_val >= 1e6)  return(snprintf(buf, sz, "$%.1fM", val / 1e6));
  if(abs_val >= 1e3)  return(snprintf(buf, sz, "$%.0fK", val / 1e3));

  return(snprintf(buf, sz, "$%.0f", val));
}

static int
crypto_fmt_price(double price, char *buf, size_t sz)
{
  if(price >= 10000.0) return(snprintf(buf, sz, "$%.0f", price));
  if(price >= 1.0)     return(snprintf(buf, sz, "$%.2f", price));
  if(price >= 0.01)    return(snprintf(buf, sz, "$%.4f", price));

  return(snprintf(buf, sz, "$%.6f", price));
}

static size_t
crypto_visible_len(const char *s)
{
  size_t n = 0;

  while(*s != '\0')
  {
    if(*s == '\x01' && s[1] != '\0')
    {
      s += 2;
      continue;
    }

    n++;
    s++;
  }

  return(n);
}

static void
crypto_pad(char *buf, size_t sz, int width)
{
  size_t vis = crypto_visible_len(buf);
  size_t raw = strlen(buf);
  int    pad = width - (int)vis;

  if(pad <= 0 || raw + (size_t)pad + 1 > sz)
    return;

  memmove(buf + pad, buf, raw + 1);

  for(int i = 0; i < pad; i++)
    buf[i] = ' ';
}

static int
crypto_fmt_pct(double pct, char *buf, size_t sz)
{
  if(pct > 0.0)
    return(snprintf(buf, sz, CLR_GREEN "%+.1f%%" CLR_RESET, pct));
  if(pct < 0.0)
    return(snprintf(buf, sz, CLR_RED   "%+.1f%%" CLR_RESET, pct));

  return(snprintf(buf, sz, "0.0%%"));
}

static int
crypto_fmt_vol(double val, char *buf, size_t sz)
{
  char num[32];
  double abs_val = val < 0 ? -val : val;

  if(abs_val >= 1e12)      snprintf(num, sizeof(num), "$%.1fT", val / 1e12);
  else if(abs_val >= 1e9)  snprintf(num, sizeof(num), "$%.1fB", val / 1e9);
  else if(abs_val >= 1e6)  snprintf(num, sizeof(num), "$%.1fM", val / 1e6);
  else if(abs_val >= 1e3)  snprintf(num, sizeof(num), "$%.0fK", val / 1e3);
  else                     snprintf(num, sizeof(num), "$%.0f",  val);

  if(val > 0.0)
    return(snprintf(buf, sz, CLR_GREEN "%s" CLR_RESET, num));
  if(val < 0.0)
    return(snprintf(buf, sz, CLR_RED   "%s" CLR_RESET, num));

  return(snprintf(buf, sz, "%s", num));
}

// Argument parsing

static int
crypto_parse_sort_col(const char *s)
{
  if(strcasecmp(s, "rank")   == 0) return(COINMARKETCAP_SORT_RANK);
  if(strcasecmp(s, "symbol") == 0) return(COINMARKETCAP_SORT_SYMBOL);
  if(strcasecmp(s, "price")  == 0) return(COINMARKETCAP_SORT_PRICE);
  if(strcasecmp(s, "cap")    == 0) return(COINMARKETCAP_SORT_CAP);
  if(strcasecmp(s, "1h")     == 0) return(COINMARKETCAP_SORT_1H);
  if(strcasecmp(s, "24h")    == 0) return(COINMARKETCAP_SORT_24H);
  if(strcasecmp(s, "7d")     == 0) return(COINMARKETCAP_SORT_7D);
  if(strcasecmp(s, "vol")    == 0) return(COINMARKETCAP_SORT_VOL);

  return(-1);
}

static bool
crypto_is_digits(const char *s)
{
  if(s[0] == '\0')
    return(false);

  for(int i = 0; s[i] != '\0'; i++)
    if(s[i] < '0' || s[i] > '9')
      return(false);

  return(true);
}

static bool
crypto_parse_piece(const char *piece, crypto_selector_t *sel)
{
  const char *dash = strchr(piece, '-');

  if(dash != NULL && dash != piece)
  {
    const char *right;
    char        left[16] = {0};
    size_t      llen     = (size_t)(dash - piece);

    if(llen >= sizeof(left))
      return(false);

    memcpy(left, piece, llen);
    left[llen] = '\0';

    right = dash + 1;

    if(crypto_is_digits(left) && crypto_is_digits(right))
    {
      sel->kind     = CRYPTO_SEL_RANGE;
      sel->range.lo = atoi(left);
      sel->range.hi = atoi(right);

      if(sel->range.lo > sel->range.hi)
      {
        int32_t tmp   = sel->range.lo;
        sel->range.lo = sel->range.hi;
        sel->range.hi = tmp;
      }

      return(true);
    }
  }

  if(crypto_is_digits(piece))
  {
    sel->kind = CRYPTO_SEL_RANK;
    sel->rank = atoi(piece);
    return(true);
  }

  sel->kind = CRYPTO_SEL_SYMBOL;
  snprintf(sel->symbol, sizeof(sel->symbol), "%s", piece);

  for(int i = 0; sel->symbol[i] != '\0'; i++)
    sel->symbol[i] = (char)toupper((unsigned char)sel->symbol[i]);

  return(true);
}

static bool
crypto_parse_args(const char *args, crypto_req_t *req,
    const cmd_ctx_t *ctx)
{
  char  buf[CRYPTO_REPLY_SZ];
  char *saveptr;
  char *tok;

  req->verbose        = false;
  req->sort_col       = COINMARKETCAP_SORT_RANK;
  req->sort_reverse   = false;
  req->selector_count = 0;

  if(args == NULL || args[0] == '\0')
    return(true);

  snprintf(buf, sizeof(buf), "%s", args);

  saveptr = NULL;
  tok = strtok_r(buf, " \t", &saveptr);

  while(tok != NULL)
  {
    char *sp2;
    char *piece;

    if(strcmp(tok, "-v") == 0 || strcmp(tok, "--verbose") == 0)
    {
      req->verbose = true;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    if(strcmp(tok, "-g") == 0 || strcmp(tok, "--global") == 0)
    {
      req->kind = CRYPTO_REQ_GLOBAL;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    if(strncmp(tok, "-s", 2) == 0 && tok[2] != '\0')
    {
      int idx;
      const char *col = tok + 2;

      if(col[0] == '-')
      {
        req->sort_reverse = true;
        col++;
      }

      idx = crypto_parse_sort_col(col);

      if(idx < 0)
      {
        char err[CRYPTO_REPLY_SZ];

        snprintf(err, sizeof(err),
            "Unknown sort column '%s'. "
            "Valid: rank, symbol, price, cap, 1h, 24h, 7d, vol", col);
        cmd_reply(ctx, err);
        return(false);
      }

      req->sort_col = (uint8_t)idx;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    if(strncmp(tok, "--sort=", 7) == 0)
    {
      int idx;
      const char *col = tok + 7;

      if(col[0] == '-')
      {
        req->sort_reverse = true;
        col++;
      }

      idx = crypto_parse_sort_col(col);

      if(idx < 0)
      {
        char err[CRYPTO_REPLY_SZ];

        snprintf(err, sizeof(err),
            "Unknown sort column '%s'. "
            "Valid: rank, symbol, price, cap, 1h, 24h, 7d, vol", col);
        cmd_reply(ctx, err);
        return(false);
      }

      req->sort_col = (uint8_t)idx;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    sp2 = NULL;
    piece = strtok_r(tok, ",", &sp2);

    while(piece != NULL)
    {
      if(req->selector_count >= COINMARKETCAP_MAX_SELECT)
      {
        cmd_reply(ctx, "Error: too many selectors (max 32)");
        return(false);
      }

      if(!crypto_parse_piece(piece,
          &req->selectors[req->selector_count]))
      {
        char err[CRYPTO_REPLY_SZ];

        snprintf(err, sizeof(err), "Invalid selector: '%s'", piece);
        cmd_reply(ctx, err);
        return(false);
      }

      req->selector_count++;
      piece = strtok_r(NULL, ",", &sp2);
    }

    tok = strtok_r(NULL, " \t", &saveptr);
  }

  if(req->kind == CRYPTO_REQ_GLOBAL)
  {
    if(req->verbose)
    {
      cmd_reply(ctx, "Error: -g and -v cannot be combined.");
      return(false);
    }

    if(req->selector_count > 0)
    {
      cmd_reply(ctx,
          "Error: -g shows global market data and takes no selectors.");
      return(false);
    }

    return(true);
  }

  if(req->verbose && req->selector_count != 1)
  {
    cmd_reply(ctx,
        "Verbose mode requires exactly one cryptocurrency. "
        "Example: !crypto -v btc");
    return(false);
  }

  if(req->verbose && req->selector_count == 1
      && req->selectors[0].kind == CRYPTO_SEL_RANGE)
  {
    cmd_reply(ctx,
        "Verbose mode requires a single symbol or rank, not a range.");
    return(false);
  }

  return(true);
}

// Selector matching against a cached coin

static bool
crypto_coin_matches(const coinmarketcap_coin_t *c,
    const crypto_selector_t *sel)
{
  switch(sel->kind)
  {
    case CRYPTO_SEL_SYMBOL:
      return(strcasecmp(c->symbol, sel->symbol) == 0);

    case CRYPTO_SEL_RANK:
      return(c->cmc_rank == sel->rank);

    case CRYPTO_SEL_RANGE:
      return(c->cmc_rank >= sel->range.lo
          && c->cmc_rank <= sel->range.hi);
  }

  return(false);
}

// Reply formatters (consume typed payloads + cache reads)

static void
crypto_reply_table(const cmd_ctx_t *ctx, crypto_req_t *req)
{
  char     line[CRYPTO_REPLY_SZ];
  coinmarketcap_coin_t raw[COINMARKETCAP_MAX_LISTINGS];
  coinmarketcap_coin_t results[COINMARKETCAP_MAX_SELECT
      > COINMARKETCAP_MAX_LISTINGS
      ? COINMARKETCAP_MAX_SELECT : COINMARKETCAP_MAX_LISTINGS];
  uint32_t raw_count = 0;
  uint32_t count     = 0;
  uint32_t max_results = sizeof(results) / sizeof(results[0]);

  // Pull the whole cache unsorted, sort locally so selector filtering
  // happens against rank-ordered data regardless of the requested sort.
  if(coinmarketcap_get_listings(0, COINMARKETCAP_SORT_RANK, false,
      raw, COINMARKETCAP_MAX_LISTINGS, &raw_count) != SUCCESS
      || raw_count == 0)
  {
    cmd_reply(ctx, "No cryptocurrency data available yet.");
    return;
  }

  if(req->selector_count == 0)
  {
    uint32_t n = req->limit;

    if(n > raw_count)   n = raw_count;
    if(n > max_results) n = max_results;

    memcpy(results, raw, n * sizeof(*results));
    count = n;
  }
  else
  {
    for(uint32_t i = 0; i < raw_count && count < max_results; i++)
    {
      for(uint8_t s = 0; s < req->selector_count; s++)
      {
        if(crypto_coin_matches(&raw[i], &req->selectors[s]))
        {
          results[count++] = raw[i];
          break;
        }
      }
    }
  }

  if(count == 0)
  {
    cmd_reply(ctx, "No matching cryptocurrencies found.");
    return;
  }

  // Sort the selected results. Comparator lives in the service plugin's
  // private state, so we re-sort by calling the API with a small sub-
  // array swap: copy our results back through get_listings would lose
  // the filter, so do a local qsort here via the same column indices.
  {
    uint8_t col = req->sort_col;
    bool    rev = req->sort_reverse;
    // Small selection sort keeps us out of the comparator tls dance.
    for(uint32_t i = 0; i + 1 < count; i++)
    {
      uint32_t best = i;

      for(uint32_t j = i + 1; j < count; j++)
      {
        double av = 0, bv = 0;
        int    cmp = 0;
        const coinmarketcap_coin_t *a = &results[best];
        const coinmarketcap_coin_t *b = &results[j];

        switch(col)
        {
          case COINMARKETCAP_SORT_RANK:
            cmp = (a->cmc_rank > b->cmc_rank) - (a->cmc_rank < b->cmc_rank);
            break;
          case COINMARKETCAP_SORT_SYMBOL:
            cmp = strcasecmp(a->symbol, b->symbol);
            break;
          case COINMARKETCAP_SORT_PRICE: av = a->price;      bv = b->price;      goto num;
          case COINMARKETCAP_SORT_CAP:   av = a->market_cap; bv = b->market_cap; goto num;
          case COINMARKETCAP_SORT_1H:    av = a->pct_1h;     bv = b->pct_1h;     goto num;
          case COINMARKETCAP_SORT_24H:   av = a->pct_24h;    bv = b->pct_24h;    goto num;
          case COINMARKETCAP_SORT_7D:    av = a->pct_7d;     bv = b->pct_7d;     goto num;
          case COINMARKETCAP_SORT_VOL:   av = a->volume_24h; bv = b->volume_24h;
num:        cmp = (av > bv) - (av < bv);
            break;
          default:
            cmp = (a->cmc_rank > b->cmc_rank) - (a->cmc_rank < b->cmc_rank);
            break;
        }

        if(rev) cmp = -cmp;

        if(cmp > 0)
          best = j;
      }

      if(best != i)
      {
        coinmarketcap_coin_t tmp = results[i];
        results[i]    = results[best];
        results[best] = tmp;
      }
    }
  }

  snprintf(line, sizeof(line),
      " %4s  %-6s %-14s %10s %10s %7s %7s %7s %10s",
      "Rank", "Symbol", "Name", "Price", "Mkt Cap",
      "1h", "24h", "7d", "Vol 24h");
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      " %s  %s %s %s %s %s %s %s %s",
      "----", "------", "--------------",
      "----------", "----------",
      "-------", "-------", "-------", "----------");
  cmd_reply(ctx, line);

  for(uint32_t i = 0; i < count; i++)
  {
    const coinmarketcap_coin_t *c = &results[i];
    char price[32], cap[32], vol[64], p1h[48], p24h[48], p7d[48];

    crypto_fmt_price (c->price,       price, sizeof(price));
    crypto_fmt_number(c->market_cap,  cap,   sizeof(cap));
    crypto_fmt_pct   (c->pct_1h,      p1h,   sizeof(p1h));
    crypto_fmt_pct   (c->pct_24h,     p24h,  sizeof(p24h));
    crypto_fmt_pct   (c->pct_7d,      p7d,   sizeof(p7d));
    crypto_fmt_vol   (c->volume_24h,  vol,   sizeof(vol));

    crypto_pad(p1h,  sizeof(p1h),  7);
    crypto_pad(p24h, sizeof(p24h), 7);
    crypto_pad(p7d,  sizeof(p7d),  7);
    crypto_pad(vol,  sizeof(vol), 10);

    snprintf(line, sizeof(line),
        " %4d  " CLR_YELLOW "%-6s" CLR_RESET
        " %-14.14s "
        CLR_BOLD CLR_WHITE "%10s" CLR_RESET
        " %10s %s %s %s %s",
        c->cmc_rank, c->symbol, c->name,
        price, cap, p1h, p24h, p7d, vol);

    cmd_reply(ctx, line);
  }
}

static void
crypto_reply_verbose(const cmd_ctx_t *ctx,
    const coinmarketcap_coin_detail_t *d)
{
  char p1h[48], p24h[48], p7d[48], p30d[48], p60d[48], p90d[48];
  char supply_t[32], supply_m[32];
  char price[32], cap[32], fdcap[32], vol[64], supply_c[32];
  char line[CRYPTO_REPLY_SZ];
  char date_clean[COINMARKETCAP_DATE_SZ];
  char *tpos;
  double cs, ts, ms;

  // Truncate date_added to date only.
  snprintf(date_clean, sizeof(date_clean), "%s", d->date_added);
  tpos = strchr(date_clean, 'T');
  if(tpos != NULL)
    *tpos = '\0';

  crypto_fmt_price (d->base.price,              price, sizeof(price));
  crypto_fmt_number(d->base.market_cap,         cap,   sizeof(cap));
  crypto_fmt_number(d->fully_diluted_market_cap,fdcap, sizeof(fdcap));
  crypto_fmt_vol   (d->base.volume_24h,         vol,   sizeof(vol));
  crypto_fmt_pct   (d->base.pct_1h,             p1h,   sizeof(p1h));
  crypto_fmt_pct   (d->base.pct_24h,            p24h,  sizeof(p24h));
  crypto_fmt_pct   (d->base.pct_7d,             p7d,   sizeof(p7d));
  crypto_fmt_pct   (d->pct_30d,                 p30d,  sizeof(p30d));
  crypto_fmt_pct   (d->pct_60d,                 p60d,  sizeof(p60d));
  crypto_fmt_pct   (d->pct_90d,                 p90d,  sizeof(p90d));

  cs = d->base.circulating_supply;
  ts = d->base.total_supply;
  ms = d->base.max_supply;

  if(cs >= 1e9)      snprintf(supply_c, sizeof(supply_c), "%.1fB", cs / 1e9);
  else if(cs >= 1e6) snprintf(supply_c, sizeof(supply_c), "%.1fM", cs / 1e6);
  else               snprintf(supply_c, sizeof(supply_c), "%.0f",  cs);

  if(ts >= 1e9)      snprintf(supply_t, sizeof(supply_t), "%.1fB", ts / 1e9);
  else if(ts >= 1e6) snprintf(supply_t, sizeof(supply_t), "%.1fM", ts / 1e6);
  else               snprintf(supply_t, sizeof(supply_t), "%.0f",  ts);

  if(ms > 0)
  {
    if(ms >= 1e9)      snprintf(supply_m, sizeof(supply_m), "%.1fB", ms / 1e9);
    else if(ms >= 1e6) snprintf(supply_m, sizeof(supply_m), "%.1fM", ms / 1e6);
    else               snprintf(supply_m, sizeof(supply_m), "%.0f",  ms);
  }
  else
    snprintf(supply_m, sizeof(supply_m), "N/A");

  snprintf(line, sizeof(line),
      CLR_BOLD CLR_WHITE "%s" CLR_RESET
      " (" CLR_YELLOW "%s" CLR_RESET ") "
      CLR_GRAY "#%d" CLR_RESET,
      d->base.name, d->base.symbol, d->base.cmc_rank);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Price: " CLR_BOLD CLR_WHITE "%s" CLR_RESET, price);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Market Cap: %s  Dominance: %.2f%%", cap, d->market_cap_dominance);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "Fully Diluted: %s", fdcap);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Volume (24h): %s  Market Pairs: %d", vol, d->base.num_market_pairs);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Change:  1h: %s  24h: %s  7d: %s", p1h, p24h, p7d);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Change: 30d: %s  60d: %s  90d: %s", p30d, p60d, p90d);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Supply: Circ: %s  Total: %s  Max: %s", supply_c, supply_t, supply_m);
  cmd_reply(ctx, line);

  if(date_clean[0] != '\0')
  {
    snprintf(line, sizeof(line), "Added: %s", date_clean);
    cmd_reply(ctx, line);
  }
}

static void
crypto_reply_global(const cmd_ctx_t *ctx, const coinmarketcap_global_t *g)
{
  char line[CRYPTO_REPLY_SZ];
  char cap[32], vol[64], defi_v[32], defi_c[32];
  char stable_v[32], stable_c[32], deriv_v[32];
  char cap_chg[48], vol_chg[48];

  crypto_fmt_number(g->total_cap,       cap,      sizeof(cap));
  crypto_fmt_vol   (g->total_vol,       vol,      sizeof(vol));
  crypto_fmt_number(g->defi_vol_24h,    defi_v,   sizeof(defi_v));
  crypto_fmt_number(g->defi_cap,        defi_c,   sizeof(defi_c));
  crypto_fmt_number(g->stablecoin_vol,  stable_v, sizeof(stable_v));
  crypto_fmt_number(g->stablecoin_cap,  stable_c, sizeof(stable_c));
  crypto_fmt_number(g->derivatives_vol, deriv_v,  sizeof(deriv_v));

  if(g->total_cap_yest > 0.0)
    crypto_fmt_pct(
        ((g->total_cap - g->total_cap_yest) / g->total_cap_yest) * 100.0,
        cap_chg, sizeof(cap_chg));
  else
    snprintf(cap_chg, sizeof(cap_chg), "N/A");

  if(g->total_vol_yest > 0.0)
    crypto_fmt_pct(
        ((g->total_vol - g->total_vol_yest) / g->total_vol_yest) * 100.0,
        vol_chg, sizeof(vol_chg));
  else
    snprintf(vol_chg, sizeof(vol_chg), "N/A");

  snprintf(line, sizeof(line),
      CLR_BOLD CLR_WHITE "Global Cryptocurrency Market" CLR_RESET);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "Market Cap: %s (%s)", cap, cap_chg);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line), "Volume (24h): %s (%s)", vol, vol_chg);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Dominance:  BTC: " CLR_YELLOW "%.1f%%" CLR_RESET
      "  ETH: " CLR_CYAN "%.1f%%" CLR_RESET,
      g->btc_dom, g->eth_dom);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Active:  Cryptocurrencies: %d  Exchanges: %d",
      g->active_cryptos, g->active_exchanges);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "DeFi:  Market Cap: %s  Volume (24h): %s", defi_c, defi_v);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Stablecoins:  Market Cap: %s  Volume (24h): %s", stable_c, stable_v);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "Derivatives:  Volume (24h): %s", deriv_v);
  cmd_reply(ctx, line);
}

// Request factory — deep-copies the command context so it survives
// beyond the command callback return.
static crypto_req_t *
crypto_req_new(const cmd_ctx_t *ctx)
{
  crypto_req_t *r = mem_alloc(CRYPTO_CTX, "req", sizeof(*r));

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

// Async completion callbacks

static void
crypto_done_listings(const coinmarketcap_listings_result_t *res, void *user)
{
  crypto_req_t *r   = (crypto_req_t *)user;
  cmd_ctx_t     ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
    cmd_reply(&ctx, res->err);
  else
    crypto_reply_table(&ctx, r);

  mem_free(r);
}

static void
crypto_done_detail(const coinmarketcap_detail_result_t *res, void *user)
{
  crypto_req_t *r   = (crypto_req_t *)user;
  cmd_ctx_t     ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
    cmd_reply(&ctx, res->err);
  else
    crypto_reply_verbose(&ctx, &res->detail);

  mem_free(r);
}

static void
crypto_done_global(const coinmarketcap_global_result_t *res, void *user)
{
  crypto_req_t *r   = (crypto_req_t *)user;
  cmd_ctx_t     ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
    cmd_reply(&ctx, res->err);
  else
    crypto_reply_global(&ctx, &res->global);

  mem_free(r);
}

// Command callback

static void
crypto_cmd_crypto(const cmd_ctx_t *ctx)
{
  crypto_req_t  stack_req;
  crypto_req_t *r;

  if(!coinmarketcap_apikey_configured())
  {
    cmd_reply(ctx,
        "Error: CoinMarketCap API key not configured. "
        "Set plugin.coinmarketcap.apikey via /set");
    return;
  }

  // Parse args on-stack first so validation failures don't allocate.
  memset(&stack_req, 0, sizeof(stack_req));
  stack_req.kind = CRYPTO_REQ_TABLE;

  if(!crypto_parse_args(ctx->args, &stack_req, ctx))
    return;

  stack_req.limit = coinmarketcap_default_limit_kv_value();

  // Global mode: serve from cache if fresh, otherwise fetch.
  if(stack_req.kind == CRYPTO_REQ_GLOBAL)
  {
    if(coinmarketcap_global_cache_fresh())
    {
      coinmarketcap_global_t g;

      if(coinmarketcap_get_global(&g) == SUCCESS)
      {
        crypto_reply_global(ctx, &g);
        return;
      }
    }

    r = crypto_req_new(ctx);
    r->kind = CRYPTO_REQ_GLOBAL;

    if(coinmarketcap_fetch_global_async(crypto_done_global, r) != SUCCESS)
    {
      cmd_reply(ctx,
          "Error: failed to submit global request. "
          "Check plugin.coinmarketcap.apikey.");
      mem_free(r);
    }

    return;
  }

  // Verbose: always fetch fresh detail for the single selected coin.
  if(stack_req.verbose)
  {
    const crypto_selector_t *sel = &stack_req.selectors[0];
    const char *symbol = NULL;
    int32_t     rank   = 0;

    if(sel->kind == CRYPTO_SEL_SYMBOL)
      symbol = sel->symbol;
    else if(sel->kind == CRYPTO_SEL_RANK)
      rank = sel->rank;

    r = crypto_req_new(ctx);
    r->kind = CRYPTO_REQ_VERBOSE;

    if(coinmarketcap_fetch_detail_async(symbol, rank,
        crypto_done_detail, r) != SUCCESS)
    {
      cmd_reply(ctx,
          "Error: failed to submit detail request. "
          "Rank lookups require a warm listings cache.");
      mem_free(r);
    }

    return;
  }

  // Table mode: if cache is fresh, format straight from cache; else
  // fetch a refresh and format from the listings callback.
  if(coinmarketcap_listings_cache_fresh())
  {
    // Promote stack_req into ctx-aware render; the listings formatter
    // reads the cache directly, so no async round-trip is needed.
    crypto_req_t tmp = stack_req;

    crypto_reply_table(ctx, &tmp);
    return;
  }

  r = crypto_req_new(ctx);
  r->kind           = CRYPTO_REQ_TABLE;
  r->selector_count = stack_req.selector_count;
  r->sort_col       = stack_req.sort_col;
  r->sort_reverse   = stack_req.sort_reverse;
  r->verbose        = stack_req.verbose;
  r->limit          = stack_req.limit;
  memcpy(r->selectors, stack_req.selectors, sizeof(r->selectors));

  if(coinmarketcap_fetch_listings_async(crypto_done_listings, r) != SUCCESS)
  {
    cmd_reply(ctx,
        "Error: failed to submit listings request. "
        "Check plugin.coinmarketcap.apikey.");
    mem_free(r);
  }
}

// NL hints

static const cmd_nl_slot_t crypto_nl_slots[] = {
  { .name  = "symbol",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t crypto_nl_examples[] = {
  { .utterance  = "what's bitcoin at?",
    .invocation = "/crypto BTC" },
  { .utterance  = "price of ETH",
    .invocation = "/crypto ETH" },
};

static const cmd_nl_t crypto_nl = {
  .when          = "User asks for a cryptocurrency spot price.",
  .syntax        = "/crypto <TICKER>",
  .slots         = crypto_nl_slots,
  .slot_count    = (uint8_t)(sizeof(crypto_nl_slots)
                             / sizeof(crypto_nl_slots[0])),
  .examples      = crypto_nl_examples,
  .example_count = (uint8_t)(sizeof(crypto_nl_examples)
                             / sizeof(crypto_nl_examples[0])),
};

// Plugin lifecycle

static bool
crypto_init(void)
{
  if(cmd_register(CRYPTO_CTX, "crypto",
      "crypto [options] [symbols|ranks|ranges]",
      "Show cryptocurrency market data from CoinMarketCap",
      NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      crypto_cmd_crypto, NULL, NULL, "c",
      NULL, 0, NULL, &crypto_nl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, CRYPTO_CTX, "crypto command plugin initialized");

  return(SUCCESS);
}

static void
crypto_deinit(void)
{
  cmd_unregister("crypto");

  clam(CLAM_INFO, CRYPTO_CTX, "crypto command plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "crypto",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "crypto",
  .provides        = { { .name = "cmd_crypto" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "method_command" },
    { .name = "service_coinmarketcap" },
  },
  .requires_count  = 2,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = crypto_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = crypto_deinit,
  .ext             = NULL,
};
