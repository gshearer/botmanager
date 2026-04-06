#define CMC_INTERNAL
#include "coinmarketcap.h"
#include "colors.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// -----------------------------------------------------------------------
// Freelist helpers
// -----------------------------------------------------------------------

// Allocate a zeroed request from the freelist or fresh heap.
// returns: zeroed request
static cmc_request_t *
cmc_req_alloc(void)
{
  cmc_request_t *r = NULL;

  pthread_mutex_lock(&cmc_free_mu);

  if(cmc_free != NULL)
  {
    r = cmc_free;
    cmc_free = r->next;
  }

  pthread_mutex_unlock(&cmc_free_mu);

  if(r == NULL)
    r = mem_alloc(CMC_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

// Return a request to the freelist.
// r: request to release
static void
cmc_req_release(cmc_request_t *r)
{
  pthread_mutex_lock(&cmc_free_mu);
  r->next = cmc_free;
  cmc_free = r;
  pthread_mutex_unlock(&cmc_free_mu);
}

// -----------------------------------------------------------------------
// Reply helper
// -----------------------------------------------------------------------

// Send a reply using the saved command context.
// r: request carrying the context
// text: reply text
static void
cmc_reply(cmc_request_t *r, const char *text)
{
  if(!r->is_poll)
    cmd_reply(&r->ctx, text);
}

// -----------------------------------------------------------------------
// Number formatting helpers
// -----------------------------------------------------------------------

// Format a large number with T/B/M/K suffix.
// returns: bytes written (excluding NUL)
// val: number to format
// buf: destination buffer
// sz: buffer size
static int
cmc_fmt_number(double val, char *buf, size_t sz)
{
  double abs_val = val < 0 ? -val : val;

  if(abs_val >= 1e12)
    return(snprintf(buf, sz, "$%.1fT", val / 1e12));
  if(abs_val >= 1e9)
    return(snprintf(buf, sz, "$%.1fB", val / 1e9));
  if(abs_val >= 1e6)
    return(snprintf(buf, sz, "$%.1fM", val / 1e6));
  if(abs_val >= 1e3)
    return(snprintf(buf, sz, "$%.0fK", val / 1e3));

  return(snprintf(buf, sz, "$%.0f", val));
}

// Format a price with appropriate decimal places.
// returns: bytes written (excluding NUL)
// price: price to format
// buf: destination buffer
// sz: buffer size
static int
cmc_fmt_price(double price, char *buf, size_t sz)
{
  if(price >= 10000.0)
    return(snprintf(buf, sz, "$%.0f", price));
  if(price >= 1.0)
    return(snprintf(buf, sz, "$%.2f", price));
  if(price >= 0.01)
    return(snprintf(buf, sz, "$%.4f", price));

  return(snprintf(buf, sz, "$%.6f", price));
}

// Count the visible length of a string, skipping \x01X color pairs.
// returns: visible character count
// s: NUL-terminated string
static size_t
cmc_visible_len(const char *s)
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

// Right-align a string by prepending spaces to a target visible width.
// buf: string to pad (modified in place)
// sz: buffer capacity
// width: desired visible width
static void
cmc_pad(char *buf, size_t sz, int width)
{
  size_t vis = cmc_visible_len(buf);
  size_t raw = strlen(buf);
  int    pad = width - (int)vis;

  if(pad <= 0 || raw + (size_t)pad + 1 > sz)
    return;

  memmove(buf + pad, buf, raw + 1);

  for(int i = 0; i < pad; i++)
    buf[i] = ' ';
}

// Format a percentage with color (CLR_GREEN > 0, CLR_RED < 0).
// returns: bytes written (excluding NUL)
// pct: percentage value
// buf: destination buffer
// sz: buffer size
static int
cmc_fmt_pct(double pct, char *buf, size_t sz)
{
  if(pct > 0.0)
    return(snprintf(buf, sz,
        CLR_GREEN "%+.1f%%" CLR_RESET, pct));
  if(pct < 0.0)
    return(snprintf(buf, sz,
        CLR_RED "%+.1f%%" CLR_RESET, pct));

  return(snprintf(buf, sz, "0.0%%"));
}

// Format a volume with color and T/B/M/K suffix.
// returns: bytes written (excluding NUL)
// val: volume value
// buf: destination buffer
// sz: buffer size
static int
cmc_fmt_vol(double val, char *buf, size_t sz)
{
  char num[32];
  double abs_val = val < 0 ? -val : val;

  if(abs_val >= 1e12)
    snprintf(num, sizeof(num), "$%.1fT", val / 1e12);
  else if(abs_val >= 1e9)
    snprintf(num, sizeof(num), "$%.1fB", val / 1e9);
  else if(abs_val >= 1e6)
    snprintf(num, sizeof(num), "$%.1fM", val / 1e6);
  else if(abs_val >= 1e3)
    snprintf(num, sizeof(num), "$%.0fK", val / 1e3);
  else
    snprintf(num, sizeof(num), "$%.0f", val);

  if(val > 0.0)
    return(snprintf(buf, sz,
        CLR_GREEN "%s" CLR_RESET, num));
  if(val < 0.0)
    return(snprintf(buf, sz,
        CLR_RED "%s" CLR_RESET, num));

  return(snprintf(buf, sz, "%s", num));
}

// -----------------------------------------------------------------------
// Argument parser
// -----------------------------------------------------------------------

// Parse a sort column keyword to a column index.
// returns: column index, or -1 on failure
// s: keyword string
static int
cmc_parse_sort_col(const char *s)
{
  if(strcasecmp(s, "rank") == 0)    return(CMC_SORT_RANK);
  if(strcasecmp(s, "symbol") == 0)  return(CMC_SORT_SYMBOL);
  if(strcasecmp(s, "price") == 0)   return(CMC_SORT_PRICE);
  if(strcasecmp(s, "cap") == 0)     return(CMC_SORT_CAP);
  if(strcasecmp(s, "1h") == 0)      return(CMC_SORT_1H);
  if(strcasecmp(s, "24h") == 0)     return(CMC_SORT_24H);
  if(strcasecmp(s, "7d") == 0)      return(CMC_SORT_7D);
  if(strcasecmp(s, "vol") == 0)     return(CMC_SORT_VOL);

  return(-1);
}

// Check whether a string is all digits.
// returns: true if all digits and non-empty
// s: string to check
static bool
cmc_is_digits(const char *s)
{
  if(s[0] == '\0')
    return(false);

  for(int i = 0; s[i] != '\0'; i++)
  {
    if(s[i] < '0' || s[i] > '9')
      return(false);
  }

  return(true);
}

// Parse a single comma-separated piece into a selector.
// returns: true on success
// piece: single selector token (e.g., "btc", "5", "1-10")
// sel: output selector
static bool
cmc_parse_piece(const char *piece, cmc_selector_t *sel)
{
  // Check for range: digits-digits.
  const char *dash = strchr(piece, '-');

  if(dash != NULL && dash != piece)
  {
    // Ensure both sides are digits.
    char left[16] = {0};
    size_t llen = (size_t)(dash - piece);

    if(llen >= sizeof(left))
      return(false);

    memcpy(left, piece, llen);
    left[llen] = '\0';

    const char *right = dash + 1;

    if(cmc_is_digits(left) && cmc_is_digits(right))
    {
      sel->kind = CMC_SEL_RANGE;
      sel->range.lo = atoi(left);
      sel->range.hi = atoi(right);

      if(sel->range.lo > sel->range.hi)
      {
        int32_t tmp = sel->range.lo;

        sel->range.lo = sel->range.hi;
        sel->range.hi = tmp;
      }

      return(true);
    }
  }

  // Check for rank: all digits.
  if(cmc_is_digits(piece))
  {
    sel->kind = CMC_SEL_RANK;
    sel->rank = atoi(piece);
    return(true);
  }

  // Otherwise: symbol name.
  sel->kind = CMC_SEL_SYMBOL;
  snprintf(sel->symbol, sizeof(sel->symbol), "%s", piece);

  // Uppercase.
  for(int i = 0; sel->symbol[i] != '\0'; i++)
    sel->symbol[i] = (char)toupper((unsigned char)sel->symbol[i]);

  return(true);
}

// Parse command arguments into a request.
// returns: true on success (false on parse error, with error reply sent)
// args: raw argument string (may be NULL)
// req: request to populate
// ctx: command context for error replies
static bool
cmc_parse_args(const char *args, cmc_request_t *req,
    const cmd_ctx_t *ctx)
{
  req->verbose      = false;
  req->sort_col     = CMC_SORT_RANK;
  req->sort_reverse = false;
  req->selector_count = 0;

  if(args == NULL || args[0] == '\0')
    return(true);

  // Make a mutable copy to tokenize.
  char buf[CMC_REPLY_SZ];

  snprintf(buf, sizeof(buf), "%s", args);

  // Tokenize by whitespace.
  char *saveptr = NULL;
  char *tok = strtok_r(buf, " \t", &saveptr);

  while(tok != NULL)
  {
    // Verbose flag.
    if(strcmp(tok, "-v") == 0 || strcmp(tok, "--verbose") == 0)
    {
      req->verbose = true;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    // Global market flag.
    if(strcmp(tok, "-g") == 0 || strcmp(tok, "--global") == 0)
    {
      req->type = CMC_REQ_GLOBAL;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    // Sort flag: -s<col> or --sort=<col>.
    if(strncmp(tok, "-s", 2) == 0 && tok[2] != '\0')
    {
      const char *col = tok + 2;

      if(col[0] == '-')
      {
        req->sort_reverse = true;
        col++;
      }

      int idx = cmc_parse_sort_col(col);

      if(idx < 0)
      {
        char err[CMC_REPLY_SZ];

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
      const char *col = tok + 7;

      if(col[0] == '-')
      {
        req->sort_reverse = true;
        col++;
      }

      int idx = cmc_parse_sort_col(col);

      if(idx < 0)
      {
        char err[CMC_REPLY_SZ];

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

    // Selector string: split by commas.
    char *sp2 = NULL;
    char *piece = strtok_r(tok, ",", &sp2);

    while(piece != NULL)
    {
      if(req->selector_count >= CMC_MAX_SELECT)
      {
        cmd_reply(ctx, "Error: too many selectors (max 32)");
        return(false);
      }

      if(!cmc_parse_piece(piece,
          &req->selectors[req->selector_count]))
      {
        char err[CMC_REPLY_SZ];

        snprintf(err, sizeof(err),
            "Invalid selector: '%s'", piece);
        cmd_reply(ctx, err);
        return(false);
      }

      req->selector_count++;
      piece = strtok_r(NULL, ",", &sp2);
    }

    tok = strtok_r(NULL, " \t", &saveptr);
  }

  // Global mode takes no selectors and conflicts with verbose.
  if(req->type == CMC_REQ_GLOBAL)
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

  // Verbose mode requires exactly one selector.
  if(req->verbose && req->selector_count != 1)
  {
    cmd_reply(ctx,
        "Verbose mode requires exactly one cryptocurrency. "
        "Example: !crypto -v btc");
    return(false);
  }

  // Verbose mode does not support range selectors.
  if(req->verbose && req->selector_count == 1
      && req->selectors[0].kind == CMC_SEL_RANGE)
  {
    cmd_reply(ctx,
        "Verbose mode requires a single symbol or rank, not a range.");
    return(false);
  }

  return(true);
}

// -----------------------------------------------------------------------
// Cache helpers
// -----------------------------------------------------------------------

// Check whether the listings cache is still valid.
// returns: true if cache data is fresh
static bool
cmc_cache_valid(void)
{
  if(cmc_cache_count == 0)
    return(false);

  uint32_t ttl = (uint32_t)kv_get_uint("plugin.coinmarketcap.cache_ttl");

  if(ttl == 0)
    ttl = 60;

  return((time(NULL) - cmc_cache_time) < (time_t)ttl);
}

// Extract a double from a JSON object field, defaulting to 0.0.
// returns: double value
// obj: parent JSON object
// key: field name
static double
cmc_json_double(struct json_object *obj, const char *key)
{
  struct json_object *jv = NULL;

  if(!json_object_object_get_ex(obj, key, &jv) || jv == NULL)
    return(0.0);

  return(json_object_get_double(jv));
}

// Extract an int from a JSON object field, defaulting to 0.
// returns: int value
// obj: parent JSON object
// key: field name
static int32_t
cmc_json_int(struct json_object *obj, const char *key)
{
  struct json_object *jv = NULL;

  if(!json_object_object_get_ex(obj, key, &jv) || jv == NULL)
    return(0);

  return((int32_t)json_object_get_int(jv));
}

// Extract a string from a JSON object field, defaulting to "".
// returns: string pointer (valid for lifetime of obj)
// obj: parent JSON object
// key: field name
static const char *
cmc_json_str(struct json_object *obj, const char *key)
{
  struct json_object *jv = NULL;

  if(!json_object_object_get_ex(obj, key, &jv) || jv == NULL)
    return("");

  const char *s = json_object_get_string(jv);

  return(s != NULL ? s : "");
}

// Parse a single coin entry from a JSON object into a cmc_coin_t.
// c: output coin struct
// item: JSON object for one cryptocurrency
static void
cmc_parse_coin(cmc_coin_t *c, struct json_object *item)
{
  c->id              = cmc_json_int(item, "id");
  c->cmc_rank        = cmc_json_int(item, "cmc_rank");
  c->num_market_pairs = cmc_json_int(item, "num_market_pairs");

  snprintf(c->name, sizeof(c->name), "%s", cmc_json_str(item, "name"));
  snprintf(c->symbol, sizeof(c->symbol), "%s",
      cmc_json_str(item, "symbol"));

  c->circulating_supply = cmc_json_double(item, "circulating_supply");
  c->total_supply       = cmc_json_double(item, "total_supply");
  c->max_supply         = cmc_json_double(item, "max_supply");

  // Extract USD quote.
  struct json_object *jquote = NULL, *jusd = NULL;

  if(json_object_object_get_ex(item, "quote", &jquote) && jquote != NULL
      && json_object_object_get_ex(jquote, "USD", &jusd) && jusd != NULL)
  {
    c->price      = cmc_json_double(jusd, "price");
    c->market_cap = cmc_json_double(jusd, "market_cap");
    c->volume_24h = cmc_json_double(jusd, "volume_24h");
    c->pct_1h     = cmc_json_double(jusd, "percent_change_1h");
    c->pct_24h    = cmc_json_double(jusd, "percent_change_24h");
    c->pct_7d     = cmc_json_double(jusd, "percent_change_7d");
  }
}

// Populate the listings cache from a parsed JSON data array.
// Caller must hold the cache write lock.
// data_arr: JSON array of cryptocurrency objects
static void
cmc_cache_populate(struct json_object *data_arr)
{
  int len = (int)json_object_array_length(data_arr);

  if(len > CMC_MAX_LISTINGS)
    len = CMC_MAX_LISTINGS;

  for(int i = 0; i < len; i++)
  {
    struct json_object *item = json_object_array_get_idx(data_arr, i);

    if(item == NULL)
      continue;

    cmc_parse_coin(&cmc_cache[i], item);
  }

  cmc_cache_count = (uint32_t)len;
  cmc_cache_time  = time(NULL);

  clam(CLAM_DEBUG2, CMC_CTX, "cache populated: %u coins", cmc_cache_count);
}

// Check if a coin matches a selector.
// returns: true if the coin matches
// c: coin to check
// sel: selector to match against
static bool
cmc_coin_matches(const cmc_coin_t *c, const cmc_selector_t *sel)
{
  switch(sel->kind)
  {
    case CMC_SEL_SYMBOL:
      return(strcasecmp(c->symbol, sel->symbol) == 0);

    case CMC_SEL_RANK:
      return(c->cmc_rank == sel->rank);

    case CMC_SEL_RANGE:
      return(c->cmc_rank >= sel->range.lo
          && c->cmc_rank <= sel->range.hi);
  }

  return(false);
}

// -----------------------------------------------------------------------
// Sort comparator
// -----------------------------------------------------------------------

// Per-thread sort parameters (set before qsort, read by comparator).
static __thread uint8_t cmc_sort_col_tl;
static __thread bool    cmc_sort_rev_tl;

// Compare two coins for qsort.
// returns: negative, zero, or positive
// a: first coin
// b: second coin
static int
cmc_coin_cmp(const void *a, const void *b)
{
  const cmc_coin_t *ca = (const cmc_coin_t *)a;
  const cmc_coin_t *cb = (const cmc_coin_t *)b;
  int result = 0;

  switch(cmc_sort_col_tl)
  {
    case CMC_SORT_RANK:
      result = (ca->cmc_rank > cb->cmc_rank) - (ca->cmc_rank < cb->cmc_rank);
      break;

    case CMC_SORT_SYMBOL:
      result = strcasecmp(ca->symbol, cb->symbol);
      break;

    case CMC_SORT_PRICE:
      result = (ca->price > cb->price) - (ca->price < cb->price);
      break;

    case CMC_SORT_CAP:
      result = (ca->market_cap > cb->market_cap)
          - (ca->market_cap < cb->market_cap);
      break;

    case CMC_SORT_1H:
      result = (ca->pct_1h > cb->pct_1h) - (ca->pct_1h < cb->pct_1h);
      break;

    case CMC_SORT_24H:
      result = (ca->pct_24h > cb->pct_24h) - (ca->pct_24h < cb->pct_24h);
      break;

    case CMC_SORT_7D:
      result = (ca->pct_7d > cb->pct_7d) - (ca->pct_7d < cb->pct_7d);
      break;

    case CMC_SORT_VOL:
      result = (ca->volume_24h > cb->volume_24h)
          - (ca->volume_24h < cb->volume_24h);
      break;

    default:
      result = (ca->cmc_rank > cb->cmc_rank) - (ca->cmc_rank < cb->cmc_rank);
      break;
  }

  return(cmc_sort_rev_tl ? -result : result);
}

// -----------------------------------------------------------------------
// Table formatter
// -----------------------------------------------------------------------

// Format and send the listings table. Caller must hold the cache read lock.
// req: request with parsed selectors and sort parameters
static void
cmc_reply_table(cmc_request_t *req)
{
  // Build a filtered result set from the cache.
  cmc_coin_t results[CMC_MAX_SELECT > CMC_MAX_LISTINGS
      ? CMC_MAX_SELECT : CMC_MAX_LISTINGS];
  uint32_t count = 0;
  uint32_t max_results = sizeof(results) / sizeof(results[0]);

  if(req->selector_count == 0)
  {
    // No selectors: take the first N coins by rank.
    uint32_t n = req->limit;

    if(n > cmc_cache_count)
      n = cmc_cache_count;
    if(n > max_results)
      n = max_results;

    memcpy(results, cmc_cache, n * sizeof(cmc_coin_t));
    count = n;
  }
  else
  {
    // Filter by selectors.
    for(uint32_t i = 0; i < cmc_cache_count && count < max_results; i++)
    {
      for(uint8_t s = 0; s < req->selector_count; s++)
      {
        if(cmc_coin_matches(&cmc_cache[i], &req->selectors[s]))
        {
          results[count++] = cmc_cache[i];
          break;
        }
      }
    }
  }

  if(count == 0)
  {
    cmc_reply(req, "No matching cryptocurrencies found.");
    return;
  }

  // Sort.
  cmc_sort_col_tl = req->sort_col;
  cmc_sort_rev_tl = req->sort_reverse;
  qsort(results, count, sizeof(cmc_coin_t), cmc_coin_cmp);

  // Header.
  char line[CMC_REPLY_SZ];

  snprintf(line, sizeof(line),
      " %4s  %-6s %-14s %10s %10s %7s %7s %7s %10s",
      "Rank", "Symbol", "Name", "Price", "Mkt Cap",
      "1h", "24h", "7d", "Vol 24h");
  cmc_reply(req, line);

  snprintf(line, sizeof(line),
      " %s  %s %s %s %s %s %s %s %s",
      "----", "------", "--------------",
      "----------", "----------",
      "-------", "-------", "-------", "----------");
  cmc_reply(req, line);

  // Rows.
  for(uint32_t i = 0; i < count; i++)
  {
    const cmc_coin_t *c = &results[i];
    char price[32], cap[32], vol[64], p1h[48], p24h[48], p7d[48];

    cmc_fmt_price(c->price, price, sizeof(price));
    cmc_fmt_number(c->market_cap, cap, sizeof(cap));
    cmc_fmt_pct(c->pct_1h, p1h, sizeof(p1h));
    cmc_fmt_pct(c->pct_24h, p24h, sizeof(p24h));
    cmc_fmt_pct(c->pct_7d, p7d, sizeof(p7d));
    cmc_fmt_vol(c->volume_24h, vol, sizeof(vol));

    cmc_pad(p1h, sizeof(p1h), 7);
    cmc_pad(p24h, sizeof(p24h), 7);
    cmc_pad(p7d, sizeof(p7d), 7);
    cmc_pad(vol, sizeof(vol), 10);

    snprintf(line, sizeof(line),
        " %4d  " CLR_YELLOW "%-6s" CLR_RESET
        " %-14.14s "
        CLR_BOLD CLR_WHITE "%10s" CLR_RESET
        " %10s %s %s %s %s",
        c->cmc_rank, c->symbol, c->name,
        price, cap, p1h, p24h, p7d, vol);

    cmc_reply(req, line);
  }
}

// -----------------------------------------------------------------------
// Verbose formatter
// -----------------------------------------------------------------------

// Format and send verbose detail for a single coin.
// req: request context
// root: parsed JSON root from Quotes Latest response
static void
cmc_reply_verbose(cmc_request_t *req, struct json_object *root)
{
  struct json_object *jdata = NULL;

  if(!json_object_object_get_ex(root, "data", &jdata) || jdata == NULL)
  {
    cmc_reply(req, "Error: unexpected API response format");
    return;
  }

  // The data object is keyed by ID or symbol. Iterate to get the first
  // (and only) entry.
  struct json_object *item = NULL;

  {
    struct json_object_iterator it = json_object_iter_begin(jdata);
    struct json_object_iterator end = json_object_iter_end(jdata);

    if(!json_object_iter_equal(&it, &end))
      item = json_object_iter_peek_value(&it);
  }

  if(item == NULL)
  {
    cmc_reply(req, "Cryptocurrency not found.");
    return;
  }

  // If data value is an array (symbol lookup can return arrays),
  // take the first element.
  if(json_object_is_type(item, json_type_array))
  {
    if(json_object_array_length(item) == 0)
    {
      cmc_reply(req, "Cryptocurrency not found.");
      return;
    }

    item = json_object_array_get_idx(item, 0);
  }

  // Parse into detail struct.
  cmc_coin_detail_t d;

  memset(&d, 0, sizeof(d));
  cmc_parse_coin(&d.base, item);

  struct json_object *jquote = NULL, *jusd = NULL;

  if(json_object_object_get_ex(item, "quote", &jquote) && jquote != NULL
      && json_object_object_get_ex(jquote, "USD", &jusd) && jusd != NULL)
  {
    d.market_cap_dominance   = cmc_json_double(jusd, "market_cap_dominance");
    d.fully_diluted_market_cap = cmc_json_double(jusd,
        "fully_diluted_market_cap");
    d.volume_change_24h      = cmc_json_double(jusd, "volume_change_24h");
    d.pct_30d = cmc_json_double(jusd, "percent_change_30d");
    d.pct_60d = cmc_json_double(jusd, "percent_change_60d");
    d.pct_90d = cmc_json_double(jusd, "percent_change_90d");
  }

  snprintf(d.date_added, sizeof(d.date_added), "%s",
      cmc_json_str(item, "date_added"));

  // Truncate date_added to date only (strip time portion).
  char *tpos = strchr(d.date_added, 'T');

  if(tpos != NULL)
    *tpos = '\0';

  // Format output lines.
  char line[CMC_REPLY_SZ];
  char price[32], cap[32], fdcap[32], vol[64], supply_c[32];
  char supply_t[32], supply_m[32];
  char p1h[48], p24h[48], p7d[48], p30d[48], p60d[48], p90d[48];

  cmc_fmt_price(d.base.price, price, sizeof(price));
  cmc_fmt_number(d.base.market_cap, cap, sizeof(cap));
  cmc_fmt_number(d.fully_diluted_market_cap, fdcap, sizeof(fdcap));
  cmc_fmt_vol(d.base.volume_24h, vol, sizeof(vol));
  cmc_fmt_pct(d.base.pct_1h, p1h, sizeof(p1h));
  cmc_fmt_pct(d.base.pct_24h, p24h, sizeof(p24h));
  cmc_fmt_pct(d.base.pct_7d, p7d, sizeof(p7d));
  cmc_fmt_pct(d.pct_30d, p30d, sizeof(p30d));
  cmc_fmt_pct(d.pct_60d, p60d, sizeof(p60d));
  cmc_fmt_pct(d.pct_90d, p90d, sizeof(p90d));

  // Supply formatting (no $ prefix).
  double cs = d.base.circulating_supply;
  double ts = d.base.total_supply;
  double ms = d.base.max_supply;

  if(cs >= 1e9)      snprintf(supply_c, sizeof(supply_c), "%.1fB", cs / 1e9);
  else if(cs >= 1e6) snprintf(supply_c, sizeof(supply_c), "%.1fM", cs / 1e6);
  else               snprintf(supply_c, sizeof(supply_c), "%.0f", cs);

  if(ts >= 1e9)      snprintf(supply_t, sizeof(supply_t), "%.1fB", ts / 1e9);
  else if(ts >= 1e6) snprintf(supply_t, sizeof(supply_t), "%.1fM", ts / 1e6);
  else               snprintf(supply_t, sizeof(supply_t), "%.0f", ts);

  if(ms > 0)
  {
    if(ms >= 1e9)      snprintf(supply_m, sizeof(supply_m), "%.1fB", ms / 1e9);
    else if(ms >= 1e6) snprintf(supply_m, sizeof(supply_m), "%.1fM", ms / 1e6);
    else               snprintf(supply_m, sizeof(supply_m), "%.0f", ms);
  }
  else
  {
    snprintf(supply_m, sizeof(supply_m), "N/A");
  }

  // Line 1: Name (Symbol) #rank
  snprintf(line, sizeof(line),
      CLR_BOLD CLR_WHITE "%s" CLR_RESET
      " (" CLR_YELLOW "%s" CLR_RESET ") "
      CLR_GRAY "#%d" CLR_RESET,
      d.base.name, d.base.symbol, d.base.cmc_rank);
  cmc_reply(req, line);

  // Line 2: Price
  snprintf(line, sizeof(line),
      "Price: " CLR_BOLD CLR_WHITE "%s" CLR_RESET, price);
  cmc_reply(req, line);

  // Line 3: Market Cap + Dominance
  snprintf(line, sizeof(line),
      "Market Cap: %s  Dominance: %.2f%%", cap, d.market_cap_dominance);
  cmc_reply(req, line);

  // Line 4: Fully Diluted
  snprintf(line, sizeof(line), "Fully Diluted: %s", fdcap);
  cmc_reply(req, line);

  // Line 5: Volume + Market Pairs
  snprintf(line, sizeof(line),
      "Volume (24h): %s  Market Pairs: %d",
      vol, d.base.num_market_pairs);
  cmc_reply(req, line);

  // Line 6: Short-term change
  snprintf(line, sizeof(line),
      "Change:  1h: %s  24h: %s  7d: %s", p1h, p24h, p7d);
  cmc_reply(req, line);

  // Line 7: Long-term change
  snprintf(line, sizeof(line),
      "Change: 30d: %s  60d: %s  90d: %s", p30d, p60d, p90d);
  cmc_reply(req, line);

  // Line 8: Supply
  snprintf(line, sizeof(line),
      "Supply: Circ: %s  Total: %s  Max: %s",
      supply_c, supply_t, supply_m);
  cmc_reply(req, line);

  // Line 9: Date added
  if(d.date_added[0] != '\0')
  {
    snprintf(line, sizeof(line), "Added: %s", d.date_added);
    cmc_reply(req, line);
  }
}

// Format and reply with global cryptocurrency market data.
// req: request context
// root: parsed JSON root from Global Metrics Quotes Latest
static void
cmc_reply_global(cmc_request_t *req, struct json_object *root)
{
  struct json_object *jdata = NULL;

  if(!json_object_object_get_ex(root, "data", &jdata) || jdata == NULL)
  {
    cmc_reply(req, "Error: unexpected API response format");
    return;
  }

  int32_t active_cryptos  = cmc_json_int(jdata, "active_cryptocurrencies");
  int32_t active_exchanges = cmc_json_int(jdata, "active_exchanges");
  double  btc_dom         = cmc_json_double(jdata, "btc_dominance");
  double  eth_dom         = cmc_json_double(jdata, "eth_dominance");
  double  defi_vol_24h    = cmc_json_double(jdata, "defi_volume_24h");
  double  defi_cap        = cmc_json_double(jdata, "defi_market_cap");
  double  stablecoin_vol  = cmc_json_double(jdata,
      "stablecoin_volume_24h");
  double  stablecoin_cap  = cmc_json_double(jdata,
      "stablecoin_market_cap");
  double  derivatives_vol = cmc_json_double(jdata,
      "derivatives_volume_24h");

  // Extract USD quote for total market values.
  double total_cap    = 0.0;
  double total_vol    = 0.0;
  double total_cap_yest = 0.0;
  double total_vol_yest = 0.0;

  struct json_object *jquote = NULL, *jusd = NULL;

  if(json_object_object_get_ex(jdata, "quote", &jquote) && jquote != NULL
      && json_object_object_get_ex(jquote, "USD", &jusd) && jusd != NULL)
  {
    total_cap       = cmc_json_double(jusd, "total_market_cap");
    total_vol       = cmc_json_double(jusd, "total_volume_24h");
    total_cap_yest  = cmc_json_double(jusd,
        "total_market_cap_yesterday");
    total_vol_yest  = cmc_json_double(jusd,
        "total_volume_24h_yesterday");
  }

  // Format values.
  char line[CMC_REPLY_SZ];
  char cap[32], vol[64], defi_v[32], defi_c[32];
  char stable_v[32], stable_c[32], deriv_v[32];
  char cap_chg[48], vol_chg[48];

  cmc_fmt_number(total_cap, cap, sizeof(cap));
  cmc_fmt_vol(total_vol, vol, sizeof(vol));
  cmc_fmt_number(defi_vol_24h, defi_v, sizeof(defi_v));
  cmc_fmt_number(defi_cap, defi_c, sizeof(defi_c));
  cmc_fmt_number(stablecoin_vol, stable_v, sizeof(stable_v));
  cmc_fmt_number(stablecoin_cap, stable_c, sizeof(stable_c));
  cmc_fmt_number(derivatives_vol, deriv_v, sizeof(deriv_v));

  // Compute 24h change percentages from yesterday values.
  if(total_cap_yest > 0.0)
    cmc_fmt_pct(
        ((total_cap - total_cap_yest) / total_cap_yest) * 100.0,
        cap_chg, sizeof(cap_chg));
  else
    snprintf(cap_chg, sizeof(cap_chg), "N/A");

  if(total_vol_yest > 0.0)
    cmc_fmt_pct(
        ((total_vol - total_vol_yest) / total_vol_yest) * 100.0,
        vol_chg, sizeof(vol_chg));
  else
    snprintf(vol_chg, sizeof(vol_chg), "N/A");

  // Line 1: Title
  snprintf(line, sizeof(line),
      CLR_BOLD CLR_WHITE "Global Cryptocurrency Market" CLR_RESET);
  cmc_reply(req, line);

  // Line 2: Total Market Cap + 24h change
  snprintf(line, sizeof(line),
      "Market Cap: %s (%s)", cap, cap_chg);
  cmc_reply(req, line);

  // Line 3: Total Volume + 24h change
  snprintf(line, sizeof(line),
      "Volume (24h): %s (%s)", vol, vol_chg);
  cmc_reply(req, line);

  // Line 4: Dominance
  snprintf(line, sizeof(line),
      "Dominance:  BTC: "
      CLR_YELLOW "%.1f%%" CLR_RESET
      "  ETH: "
      CLR_CYAN "%.1f%%" CLR_RESET,
      btc_dom, eth_dom);
  cmc_reply(req, line);

  // Line 5: Active counts
  snprintf(line, sizeof(line),
      "Active:  Cryptocurrencies: %d  Exchanges: %d",
      active_cryptos, active_exchanges);
  cmc_reply(req, line);

  // Line 6: DeFi
  snprintf(line, sizeof(line),
      "DeFi:  Market Cap: %s  Volume (24h): %s",
      defi_c, defi_v);
  cmc_reply(req, line);

  // Line 7: Stablecoins
  snprintf(line, sizeof(line),
      "Stablecoins:  Market Cap: %s  Volume (24h): %s",
      stable_c, stable_v);
  cmc_reply(req, line);

  // Line 8: Derivatives
  snprintf(line, sizeof(line),
      "Derivatives:  Volume (24h): %s", deriv_v);
  cmc_reply(req, line);
}

// -----------------------------------------------------------------------
// HTTP submit helpers
// -----------------------------------------------------------------------

// Submit a Listings Latest API request.
// returns: SUCCESS or FAIL
// req: request context (carries apikey and reply context)
static bool
cmc_submit_listings(cmc_request_t *req)
{
  char url[CMC_URL_SZ];

  snprintf(url, sizeof(url),
      CMC_LISTINGS_URL "?limit=%u&sort=market_cap&sort_dir=desc&convert=USD",
      CMC_MAX_LISTINGS);

  curl_request_t *cr = curl_request_create(
      CURL_METHOD_GET, url, cmc_listings_done, req);

  if(cr == NULL)
  {
    cmc_reply(req, "Error: failed to create API request");
    cmc_req_release(req);
    return(FAIL);
  }

  char hdr[CMC_HDR_SZ];

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    cmc_reply(req, "Error: failed to submit API request");
    cmc_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// Submit a Quotes Latest API request for a single symbol or rank.
// returns: SUCCESS or FAIL
// req: request context
static bool
cmc_submit_quotes(cmc_request_t *req)
{
  char url[CMC_URL_SZ];

  if(req->selectors[0].kind == CMC_SEL_SYMBOL)
  {
    snprintf(url, sizeof(url),
        CMC_QUOTES_URL "?symbol=%s&convert=USD",
        req->selectors[0].symbol);
  }
  else
  {
    // Rank: need to resolve to an ID from cache or use the rank.
    // Look up the coin ID from cache if available.
    int32_t target_rank = req->selectors[0].rank;
    int32_t coin_id = 0;

    pthread_rwlock_rdlock(&cmc_cache_rwl);

    for(uint32_t i = 0; i < cmc_cache_count; i++)
    {
      if(cmc_cache[i].cmc_rank == target_rank)
      {
        coin_id = cmc_cache[i].id;
        break;
      }
    }

    pthread_rwlock_unlock(&cmc_cache_rwl);

    if(coin_id > 0)
    {
      snprintf(url, sizeof(url),
          CMC_QUOTES_URL "?id=%d&convert=USD", coin_id);
    }
    else
    {
      // No cache entry: fall back to symbol-based lookup won't work.
      // Inform the user.
      cmc_reply(req,
          "Error: rank lookup requires cached data. "
          "Run !crypto first to populate the cache.");
      cmc_req_release(req);
      return(FAIL);
    }
  }

  curl_request_t *cr = curl_request_create(
      CURL_METHOD_GET, url, cmc_quotes_done, req);

  if(cr == NULL)
  {
    cmc_reply(req, "Error: failed to create API request");
    cmc_req_release(req);
    return(FAIL);
  }

  char hdr[CMC_HDR_SZ];

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    cmc_reply(req, "Error: failed to submit API request");
    cmc_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// Submit a request to the Global Metrics Quotes Latest API.
// returns: SUCCESS or FAIL
// req: request context
static bool
cmc_submit_global(cmc_request_t *req)
{
  curl_request_t *cr = curl_request_create(
      CURL_METHOD_GET,
      CMC_GLOBAL_URL "?convert=USD",
      cmc_global_done, req);

  if(cr == NULL)
  {
    cmc_reply(req, "Error: failed to create API request");
    cmc_req_release(req);
    return(FAIL);
  }

  char hdr[CMC_HDR_SZ];

  snprintf(hdr, sizeof(hdr), "X-CMC_PRO_API_KEY: %s", req->apikey);
  curl_request_add_header(cr, hdr);
  curl_request_add_header(cr, "Accept: application/json");

  if(curl_request_submit(cr) != SUCCESS)
  {
    cmc_reply(req, "Error: failed to submit API request");
    cmc_req_release(req);
    return(FAIL);
  }

  return(SUCCESS);
}

// -----------------------------------------------------------------------
// Curl callbacks
// -----------------------------------------------------------------------

// Completion callback for Listings Latest API response.
// resp: curl response
static void
cmc_listings_done(const curl_response_t *resp)
{
  cmc_request_t *r = (cmc_request_t *)resp->user_data;

  if(resp->curl_code != 0)
  {
    char buf[CMC_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "CoinMarketCap API error: %s", resp->error);
    cmc_reply(r, buf);
    cmc_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    cmc_reply(r, "Error: invalid CoinMarketCap API key");
    cmc_req_release(r);
    return;
  }

  if(resp->status == 429)
  {
    cmc_reply(r,
        "Error: CoinMarketCap API rate limit exceeded, try again later");
    cmc_req_release(r);
    return;
  }

  if(resp->status == 402 || resp->status == 403)
  {
    cmc_reply(r,
        "Error: CoinMarketCap plan limit reached or permission denied");
    cmc_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    char buf[CMC_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "CoinMarketCap API returned HTTP %ld", resp->status);
    cmc_reply(r, buf);
    cmc_req_release(r);
    return;
  }

  // Parse JSON.
  struct json_object *root = json_tokener_parse(resp->body);

  if(root == NULL)
  {
    cmc_reply(r, "Error: malformed JSON from CoinMarketCap API");
    cmc_req_release(r);
    return;
  }

  struct json_object *jdata = NULL;

  if(!json_object_object_get_ex(root, "data", &jdata) || jdata == NULL
      || !json_object_is_type(jdata, json_type_array))
  {
    cmc_reply(r, "Error: unexpected CoinMarketCap API response format");
    json_object_put(root);
    cmc_req_release(r);
    return;
  }

  // Populate cache.
  pthread_rwlock_wrlock(&cmc_cache_rwl);
  cmc_cache_populate(jdata);
  pthread_rwlock_unlock(&cmc_cache_rwl);

  // Reply with table (if not a background poll).
  if(!r->is_poll)
  {
    pthread_rwlock_rdlock(&cmc_cache_rwl);
    cmc_reply_table(r);
    pthread_rwlock_unlock(&cmc_cache_rwl);
  }

  json_object_put(root);
  cmc_req_release(r);
}

// Completion callback for Quotes Latest API response.
// resp: curl response
static void
cmc_quotes_done(const curl_response_t *resp)
{
  cmc_request_t *r = (cmc_request_t *)resp->user_data;

  if(resp->curl_code != 0)
  {
    char buf[CMC_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "CoinMarketCap API error: %s", resp->error);
    cmc_reply(r, buf);
    cmc_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    cmc_reply(r, "Error: invalid CoinMarketCap API key");
    cmc_req_release(r);
    return;
  }

  if(resp->status == 429)
  {
    cmc_reply(r,
        "Error: CoinMarketCap API rate limit exceeded, try again later");
    cmc_req_release(r);
    return;
  }

  if(resp->status == 400)
  {
    cmc_reply(r,
        "Error: cryptocurrency not found or invalid request");
    cmc_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    char buf[CMC_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "CoinMarketCap API returned HTTP %ld", resp->status);
    cmc_reply(r, buf);
    cmc_req_release(r);
    return;
  }

  // Parse JSON.
  struct json_object *root = json_tokener_parse(resp->body);

  if(root == NULL)
  {
    cmc_reply(r, "Error: malformed JSON from CoinMarketCap API");
    cmc_req_release(r);
    return;
  }

  cmc_reply_verbose(r, root);

  json_object_put(root);
  cmc_req_release(r);
}

// Completion callback for Global Metrics Quotes Latest API response.
// resp: curl response
static void
cmc_global_done(const curl_response_t *resp)
{
  cmc_request_t *r = (cmc_request_t *)resp->user_data;

  if(resp->curl_code != 0)
  {
    char buf[CMC_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "CoinMarketCap API error: %s", resp->error);
    cmc_reply(r, buf);
    cmc_req_release(r);
    return;
  }

  if(resp->status == 401)
  {
    cmc_reply(r, "Error: invalid CoinMarketCap API key");
    cmc_req_release(r);
    return;
  }

  if(resp->status == 429)
  {
    cmc_reply(r,
        "Error: CoinMarketCap API rate limit exceeded, try again later");
    cmc_req_release(r);
    return;
  }

  if(resp->status != 200)
  {
    char buf[CMC_REPLY_SZ];

    snprintf(buf, sizeof(buf),
        "CoinMarketCap API returned HTTP %ld", resp->status);
    cmc_reply(r, buf);
    cmc_req_release(r);
    return;
  }

  struct json_object *root = json_tokener_parse(resp->body);

  if(root == NULL)
  {
    cmc_reply(r, "Error: malformed JSON from CoinMarketCap API");
    cmc_req_release(r);
    return;
  }

  cmc_reply_global(r, root);

  json_object_put(root);
  cmc_req_release(r);
}

// -----------------------------------------------------------------------
// Polling thread
// -----------------------------------------------------------------------

// Background polling loop. Periodically refreshes the listings cache.
// t: persist task handle
static void
cmc_poll_loop(task_t *t)
{
  clam(CLAM_INFO, CMC_CTX, "polling thread started");

  while(!pool_shutting_down())
  {
    uint32_t ttl = (uint32_t)kv_get_uint("plugin.coinmarketcap.cache_ttl");

    if(ttl == 0)
      ttl = 60;

    // Sleep in 1-second increments, checking for shutdown.
    for(uint32_t i = 0; i < ttl && !pool_shutting_down(); i++)
      sleep(1);

    if(pool_shutting_down())
      break;

    // Check if polling is still enabled.
    if(!(uint8_t)kv_get_uint("plugin.coinmarketcap.poll"))
    {
      clam(CLAM_INFO, CMC_CTX, "polling disabled, thread exiting");
      break;
    }

    const char *apikey = kv_get_str("plugin.coinmarketcap.apikey");

    if(apikey == NULL || apikey[0] == '\0')
    {
      clam(CLAM_DEBUG2, CMC_CTX, "poll: no API key configured, skipping");
      continue;
    }

    // Submit a background listings refresh.
    cmc_request_t *r = cmc_req_alloc();

    r->type    = CMC_REQ_TABLE;
    r->is_poll = true;
    snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

    clam(CLAM_DEBUG2, CMC_CTX, "poll: refreshing listings cache");

    if(cmc_submit_listings(r) != SUCCESS)
      clam(CLAM_WARN, CMC_CTX, "poll: failed to submit listings request");
  }

  t->state = TASK_ENDED;
  clam(CLAM_INFO, CMC_CTX, "polling thread stopped");
}

// -----------------------------------------------------------------------
// Command callback
// -----------------------------------------------------------------------

// !crypto [options] [selection] — show cryptocurrency market data.
// ctx: command context
static void
cmc_cmd_crypto(const cmd_ctx_t *ctx)
{
  // Check API key.
  const char *apikey = kv_get_str("plugin.coinmarketcap.apikey");

  if(apikey == NULL || apikey[0] == '\0')
  {
    cmd_reply(ctx,
        "Error: CoinMarketCap API key not configured. "
        "Set plugin.coinmarketcap.apikey via /set");
    return;
  }

  // Allocate request.
  cmc_request_t *r = cmc_req_alloc();

  // Parse arguments.
  if(!cmc_parse_args(ctx->args, r, ctx))
  {
    cmc_req_release(r);
    return;
  }

  // Copy API key and config.
  snprintf(r->apikey, sizeof(r->apikey), "%s", apikey);

  uint32_t dlimit = (uint32_t)kv_get_uint(
      "plugin.coinmarketcap.default_limit");

  r->limit = (dlimit > 1) ? dlimit : 12;

  // Deep-copy the command context.
  memcpy(&r->msg, ctx->msg, sizeof(r->msg));
  r->ctx.bot      = ctx->bot;
  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;

  // Global mode: fetch from Global Metrics Quotes Latest.
  if(r->type == CMC_REQ_GLOBAL)
  {
    cmc_submit_global(r);
    return;
  }

  // Verbose mode: always fetch fresh from Quotes Latest.
  if(r->verbose)
  {
    r->type = CMC_REQ_VERBOSE;
    cmc_submit_quotes(r);
    return;
  }

  // Table mode: check cache first.
  r->type = CMC_REQ_TABLE;

  pthread_rwlock_rdlock(&cmc_cache_rwl);

  if(cmc_cache_valid())
  {
    cmc_reply_table(r);
    pthread_rwlock_unlock(&cmc_cache_rwl);
    cmc_req_release(r);
    return;
  }

  pthread_rwlock_unlock(&cmc_cache_rwl);

  // Cache miss: fetch from API.
  cmc_submit_listings(r);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

// Initialize the coinmarketcap plugin. Registers commands and starts
// the optional polling thread.
// returns: SUCCESS or FAIL
static bool
cmc_init(void)
{
  pthread_mutex_init(&cmc_free_mu, NULL);
  pthread_rwlock_init(&cmc_cache_rwl, NULL);
  memset(cmc_cache, 0, sizeof(cmc_cache));

  if(cmd_register("crypto", "crypto",
      "crypto [options] [symbols|ranks|ranges]",
      "Show cryptocurrency market data from CoinMarketCap",
      NULL,
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmc_cmd_crypto, NULL, NULL, "c",
      NULL, 0) != SUCCESS)
    return(FAIL);

  // Start polling thread if enabled.
  if((uint8_t)kv_get_uint("plugin.coinmarketcap.poll"))
    cmc_poll_task = task_add_persist("cmc_poll", 200, cmc_poll_loop, NULL);

  clam(CLAM_INFO, CMC_CTX, "coinmarketcap plugin initialized");

  return(SUCCESS);
}

// Tear down the coinmarketcap plugin. Unregisters commands, frees the
// request freelist, and destroys synchronization primitives.
static void
cmc_deinit(void)
{
  cmd_unregister("crypto");

  // Free the request freelist.
  pthread_mutex_lock(&cmc_free_mu);

  while(cmc_free != NULL)
  {
    cmc_request_t *r = cmc_free;

    cmc_free = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&cmc_free_mu);
  pthread_mutex_destroy(&cmc_free_mu);

  pthread_rwlock_destroy(&cmc_cache_rwl);

  clam(CLAM_INFO, CMC_CTX, "coinmarketcap plugin deinitialized");
}

// -----------------------------------------------------------------------
// Plugin descriptor
// -----------------------------------------------------------------------

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "coinmarketcap",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "coinmarketcap",
  .provides        = { { .name = "service_coinmarketcap" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_command" } },
  .requires_count  = 1,
  .kv_schema       = cmc_kv_schema,
  .kv_schema_count = 4,
  .init            = cmc_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = cmc_deinit,
  .ext             = NULL,
};
