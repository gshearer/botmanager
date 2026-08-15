// botmanager — MIT
// /crypto command-surface plugin: parses flags, queries the
// coinmarketcap service plugin via its public API, and formats
// user-facing replies (table / verbose card / whole-market card).
#define CRYPTO_INTERNAL
#define CRYPTO_CMD_UNIT
#include "crypto.h"

#include "colors.h"
#include "userns.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Card layout: a fixed label column, then dot-separated facts. Shared by
// the --mcap and --verbose cards so the two read as one family.
#define CRYPTO_CARD_LABEL  CLR_BOLD CLR_CYAN "%-11s" CLR_RESET
#define CRYPTO_CARD_DOT    "  " CLR_GRAY "·" CLR_RESET "  "

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

// Two-decimal money for the --mcap card, where the whole market moves
// in trillions and one decimal place throws away the interesting digit.
static int
crypto_fmt_usd(double val, char *buf, size_t sz)
{
  double abs_val = val < 0 ? -val : val;

  if(abs_val >= 1e12) return(snprintf(buf, sz, "$%.2fT", val / 1e12));
  if(abs_val >= 1e9)  return(snprintf(buf, sz, "$%.2fB", val / 1e9));
  if(abs_val >= 1e6)  return(snprintf(buf, sz, "$%.2fM", val / 1e6));
  if(abs_val >= 1e3)  return(snprintf(buf, sz, "$%.1fK", val / 1e3));

  return(snprintf(buf, sz, "$%.0f", val));
}

// Copies a run of decimal digits into buf with thousands separators:
// "112304" -> "112,304".
static int
crypto_group_digits(const char *digits, char *buf, size_t sz)
{
  size_t len = strlen(digits);
  size_t out = 0;

  for(size_t i = 0; i < len; i++)
  {
    if(i > 0 && (len - i) % 3 == 0 && out + 1 < sz)
      buf[out++] = ',';

    if(out + 1 < sz)
      buf[out++] = digits[i];
  }

  if(sz > 0)
    buf[out] = '\0';

  return((int)out);
}

static int
crypto_fmt_count(int32_t n, char *buf, size_t sz)
{
  char digits[16];

  if(n <= 0)
    return(snprintf(buf, sz, CLR_GRAY "n/a" CLR_RESET));

  snprintf(digits, sizeof(digits), "%" PRId32, n);

  return(crypto_group_digits(digits, buf, sz));
}

// Full-precision price for the verbose card, grouped and with as many
// decimals as the magnitude deserves: "$63,019.00", "$0.000042".
static void
crypto_fmt_price_wide(double price, char *buf, size_t sz)
{
  char        raw[24];
  char        grouped[32];
  char        out[64];
  const char *frac = "";
  char       *dot;
  int         dec;

  // NaN, infinity or a price no market has ever printed: hand it to the
  // compact formatter rather than grouping nonsense.
  if(!(price >= 0.0 && price < 1e15))
  {
    crypto_fmt_usd(price, buf, sz);
    return;
  }

  dec = price >= 1.0 ? 2 : (price >= 0.01 ? 4 : 6);

  snprintf(raw, sizeof(raw), "%.*f", dec, price);

  dot = strchr(raw, '.');

  if(dot != NULL)
  {
    *dot = '\0';
    frac = dot + 1;
  }

  crypto_group_digits(raw, grouped, sizeof(grouped));
  snprintf(out, sizeof(out), "$%s.%s", grouped, frac);
  snprintf(buf, sz, "%s", out);
}

// Appends to a bounded buffer, keeping *used truthful when the write is
// clipped so a later append can never walk past the end. Every card
// line assembled piece by piece goes through this.
static void crypto_append(char *buf, size_t cap, size_t *used,
    const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void
crypto_append(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
  va_list ap;
  int     n;

  if(*used + 1 >= cap)
    return;

  va_start(ap, fmt);
  n = vsnprintf(buf + *used, cap - *used, fmt, ap);
  va_end(ap);

  if(n < 0)
    return;

  *used = (size_t)n >= cap - *used ? cap - 1 : *used + (size_t)n;
}

// Coin-count magnitudes — supply, not money, so no dollar sign.
static void
crypto_fmt_supply(double n, char *buf, size_t sz)
{
  if(n <= 0.0)       snprintf(buf, sz, CLR_GRAY "n/a" CLR_RESET);
  else if(n >= 1e12) snprintf(buf, sz, "%.2fT", n / 1e12);
  else if(n >= 1e9)  snprintf(buf, sz, "%.2fB", n / 1e9);
  else if(n >= 1e6)  snprintf(buf, sz, "%.2fM", n / 1e6);
  else if(n >= 1e3)  snprintf(buf, sz, "%.1fK", n / 1e3);
  else               snprintf(buf, sz, "%.0f",  n);
}

// Arrowed, coloured percentage move — the card's workhorse. NaN means
// the provider did not give us enough to compute one.
static int
crypto_fmt_move(double pct, char *buf, size_t sz)
{
  if(isnan(pct))
    return(snprintf(buf, sz, CLR_GRAY "n/a" CLR_RESET));
  if(pct > 0.0)
    return(snprintf(buf, sz, CLR_GREEN "▲%.2f%%" CLR_RESET, pct));
  if(pct < 0.0)
    return(snprintf(buf, sz, CLR_RED "▼%.2f%%" CLR_RESET, -pct));

  return(snprintf(buf, sz, CLR_GRAY "flat" CLR_RESET));
}

// Same arrows, but for a move measured in percentage points (dominance),
// where a trailing '%' would claim the wrong unit.
static int
crypto_fmt_points(double pts, char *buf, size_t sz)
{
  if(isnan(pts) || pts == 0.0)
    return(snprintf(buf, sz, CLR_GRAY "flat" CLR_RESET));
  if(pts > 0.0)
    return(snprintf(buf, sz, CLR_GREEN "▲%.2f" CLR_RESET, pts));

  return(snprintf(buf, sz, CLR_RED "▼%.2f" CLR_RESET, -pts));
}

// Age of a snapshot, in the coarsest unit that still reads honestly.
static int
crypto_fmt_age(int64_t secs, char *buf, size_t sz)
{
  if(secs < 0)     secs = 0;
  if(secs < 90)    return(snprintf(buf, sz, "%llds", (long long)secs));
  if(secs < 5400)  return(snprintf(buf, sz, "%lldm", (long long)(secs / 60)));

  return(snprintf(buf, sz, "%lldh%lldm",
      (long long)(secs / 3600), (long long)((secs % 3600) / 60)));
}

// Percentage change, preferring the value the provider computed and
// falling back to yesterday's level. NaN when neither is available.
static double
crypto_pct_change(double now, double yesterday, double reported)
{
  if(reported != 0.0)
    return(reported);

  if(yesterday > 0.0)
    return(((now - yesterday) / yesterday) * 100.0);

  return(NAN);
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

// A market rank straight off the wire: digits only, and small enough for the
// selector's int32_t. Leading sign or space is rejected by the first-digit
// test, trailing garbage by the endptr test, and 20 nines by errno.
static bool
crypto_parse_rank(const char *s, int32_t *out)
{
  char *end;
  long  val;

  if(s[0] < '0' || s[0] > '9')
    return(false);

  errno = 0;
  val   = strtol(s, &end, 10);

  if(errno != 0 || *end != '\0' || val > INT32_MAX)
    return(false);

  *out = (int32_t)val;

  return(true);
}

static bool
crypto_parse_piece(const char *piece, crypto_selector_t *sel)
{
  const char *dash = strchr(piece, '-');
  int32_t     rank = 0;

  if(dash != NULL && dash != piece)
  {
    const char *right;
    char        left[16] = {0};
    size_t      llen     = (size_t)(dash - piece);
    int32_t     lo       = 0;
    int32_t     hi       = 0;

    if(llen >= sizeof(left))
      return(false);

    memcpy(left, piece, llen);
    left[llen] = '\0';

    right = dash + 1;

    if(crypto_parse_rank(left, &lo) && crypto_parse_rank(right, &hi))
    {
      sel->kind     = CRYPTO_SEL_RANGE;
      sel->range.lo = (lo <= hi) ? lo : hi;
      sel->range.hi = (lo <= hi) ? hi : lo;

      return(true);
    }
  }

  if(crypto_parse_rank(piece, &rank))
  {
    sel->kind = CRYPTO_SEL_RANK;
    sel->rank = rank;
    return(true);
  }

  sel->kind = CRYPTO_SEL_SYMBOL;
  snprintf(sel->symbol, sizeof(sel->symbol), "%s", piece);

  for(int i = 0; sel->symbol[i] != '\0'; i++)
    sel->symbol[i] = (char)toupper((unsigned char)sel->symbol[i]);

  return(true);
}

static bool
crypto_parse_args(const char *args, crypto_req_t *req, crypto_largs_t *la,
    const cmd_ctx_t *ctx)
{
  char  buf[CRYPTO_REPLY_SZ];
  char *saveptr;
  char *tok;

  req->verbose        = false;
  req->sort_col       = COINMARKETCAP_SORT_RANK;
  req->sort_reverse   = false;
  req->selector_count = 0;

  memset(la, 0, sizeof(*la));

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

    // --mcap is the whole-market card; "global" is kept as a synonym
    // because that is what the provider calls the same endpoint.
    if(strcmp(tok, "-m") == 0 || strcmp(tok, "--mcap") == 0 ||
       strcmp(tok, "--global") == 0)
    {
      req->kind = CRYPTO_REQ_MCAP;
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

    if(strcmp(tok, "-l") == 0 || strcmp(tok, "--list") == 0)
    {
      la->op = CRYPTO_OP_SHOW;
      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    if(strcmp(tok, "-a") == 0 || strcmp(tok, "--add") == 0 ||
       strcmp(tok, "-d") == 0 || strcmp(tok, "--del") == 0)
    {
      const bool add = (strcmp(tok, "-a") == 0 || strcmp(tok, "--add") == 0);
      char      *name;

      la->op = add ? CRYPTO_OP_ADD : CRYPTO_OP_DEL;
      name   = strtok_r(NULL, " \t", &saveptr);

      if(name == NULL)
      {
        cmd_reply(ctx, add
            ? "Usage: !crypto --add <list> <symbols…>"
            : "Usage: !crypto --del <list> <symbols…>");
        return(false);
      }

      // A leading '@' is how lists are referenced elsewhere; accept it
      // here too rather than making the user remember where it belongs.
      if(name[0] == '@')
        name++;

      for(size_t j = 0; name[j] != '\0' && j + 1 < sizeof(la->list); j++)
        la->list[j] = (char)tolower((unsigned char)name[j]);

      tok = strtok_r(NULL, " \t", &saveptr);
      continue;
    }

    // Bare tokens are held verbatim and turned into selectors only after
    // parsing, once any '@list' among them has been expanded.
    sp2 = NULL;
    piece = strtok_r(tok, ",", &sp2);

    while(piece != NULL)
    {
      if(la->nitems >= COINMARKETCAP_MAX_SELECT)
      {
        cmd_reply(ctx, "Error: too many selectors (max 32)");
        return(false);
      }

      if(piece[0] != '\0')
      {
        crypto_item_t *it = &la->items[la->nitems];
        size_t         j  = 0;

        it->is_list = (piece[0] == '@');

        if(it->is_list)
          piece++;

        for(; piece[j] != '\0' && j + 1 < sizeof(it->text); j++)
          it->text[j] = it->is_list
              ? (char)tolower((unsigned char)piece[j])
              : piece[j];

        it->text[j] = '\0';

        if(it->text[0] != '\0')
          la->nitems++;
      }

      piece = strtok_r(NULL, ",", &sp2);
    }

    tok = strtok_r(NULL, " \t", &saveptr);
  }

  if(req->kind == CRYPTO_REQ_MCAP)
  {
    if(req->verbose)
    {
      cmd_reply(ctx, "Error: --mcap and --verbose cannot be combined.");
      return(false);
    }

    if(la->nitems > 0)
    {
      cmd_reply(ctx,
          "Error: --mcap shows the whole market and takes no symbols.");
      return(false);
    }

    return(true);
  }

  // Selector-shaped validation (verbose arity, ranges) has to wait for
  // crypto_expand — until a '@list' is resolved we do not know how many
  // selectors the line actually means.
  return(true);
}

// ----------------------------------------------------------------------
// Symbol lists
// ----------------------------------------------------------------------

// Turns parsed items into selectors, expanding '@name' along the way, and
// applies the validation that needs the final selector count. Replies and
// returns false on anything the caller should not proceed past.
static bool
crypto_expand(const cmd_ctx_t *ctx, crypto_req_t *req,
    const crypto_largs_t *la)
{
  userns_t *ns = NULL;
  char      err[CRYPTO_REPLY_SZ];
  uint8_t   i;

  for(i = 0; i < la->nitems; i++)
  {
    const crypto_item_t *it = &la->items[i];
    crypto_symset_t      set;
    crypto_list_rc_t     rc;
    uint8_t              j;

    if(!it->is_list)
    {
      if(req->selector_count >= COINMARKETCAP_MAX_SELECT)
      {
        cmd_reply(ctx, "Error: too many selectors (max 32)");
        return(false);
      }

      if(!crypto_parse_piece(it->text,
          &req->selectors[req->selector_count]))
      {
        snprintf(err, sizeof(err), "Invalid selector: '%s'", it->text);
        cmd_reply(ctx, err);
        return(false);
      }

      req->selector_count++;
      continue;
    }

    // A list reference and a single-coin card are different intents;
    // pick the first symbol for the user and we would be guessing.
    if(req->verbose)
    {
      cmd_reply(ctx, "Verbose mode shows one cryptocurrency, not a list. "
          "Example: !crypto -v btc");
      return(false);
    }

    if(ns == NULL && (ns = userns_session_resolve(ctx)) == NULL)
      return(false);   // the resolver already replied

    rc = crypto_lists_get(ns->id, it->text, &set);

    if(rc == CRYPTO_LIST_NOSUCH)
    {
      snprintf(err, sizeof(err),
          "No list called '%s'. Try !crypto --list", it->text);
      cmd_reply(ctx, err);
      return(false);
    }

    if(rc != CRYPTO_LIST_OK)
    {
      cmd_reply(ctx, "Couldn't read your lists right now — sorry.");
      return(false);
    }

    for(j = 0; j < set.n; j++)
    {
      if(req->selector_count >= COINMARKETCAP_MAX_SELECT)
      {
        cmd_reply(ctx, "Error: too many selectors (max 32)");
        return(false);
      }

      if(!crypto_parse_piece(set.sym[j],
          &req->selectors[req->selector_count]))
        continue;   // a stored symbol we can no longer parse: skip it

      req->selector_count++;
    }
  }

  if(req->verbose && req->selector_count != 1)
  {
    cmd_reply(ctx,
        "Verbose mode requires exactly one cryptocurrency. "
        "Example: !crypto -v btc");
    return(false);
  }

  if(req->verbose && req->selectors[0].kind == CRYPTO_SEL_RANGE)
  {
    cmd_reply(ctx,
        "Verbose mode requires a single symbol or rank, not a range.");
    return(false);
  }

  return(true);
}

// --list / --add / --del. Lists are userns-scoped, so every operation
// needs a namespace; mutating one additionally needs a known user.
static void
crypto_list_cmd(const cmd_ctx_t *ctx, const crypto_largs_t *la)
{
  userns_t *ns = userns_session_resolve(ctx);
  char      line[CRYPTO_REPLY_SZ];
  char      names[CRYPTO_REPLY_SZ - 64];   // leaves room for the heading
  uint32_t  count   = 0;
  uint8_t   changed = 0;
  uint8_t   total   = 0;
  bool      dropped = false;
  crypto_list_rc_t rc;

  if(ns == NULL)   // the resolver already replied
    return;

  if(la->op == CRYPTO_OP_SHOW)
  {
    if(crypto_lists_names(ns->id, names, sizeof(names), &count)
        != CRYPTO_LIST_OK)
    {
      cmd_reply(ctx, "Couldn't read your lists right now — sorry.");
      return;
    }

    if(count == 0)
    {
      cmd_reply(ctx, "No crypto lists yet. "
          "Make one: !crypto --add bags BTC ETH SOL");
      return;
    }

    snprintf(line, sizeof(line), CLR_BOLD "Your crypto lists:" CLR_RESET
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

  if(la->list[0] == '\0' || !crypto_list_name_ok(la->list))
  {
    cmd_reply(ctx, "List names are up to 32 letters, digits, '-' or '_'.");
    return;
  }

  if(la->nitems == 0)
  {
    cmd_reply(ctx, (la->op == CRYPTO_OP_ADD)
        ? "Add what? Example: !crypto --add bags BTC ETH"
        : "Remove what? Example: !crypto --del bags ETH");
    return;
  }

  for(uint8_t i = 0; i < la->nitems; i++)
  {
    if(!la->items[i].is_list)
      continue;

    cmd_reply(ctx, "Lists hold symbols, not other lists.");
    return;
  }

  if(la->op == CRYPTO_OP_ADD)
  {
    rc = crypto_lists_add(ns->id, la->list, la->items, la->nitems,
        &changed, &total);

    if(rc == CRYPTO_LIST_FULL)
    {
      snprintf(line, sizeof(line),
          "@%s holds %u of %d symbols — that batch won't fit, so "
          "nothing was added. Free some: !crypto --del %s <symbol>",
          la->list, (unsigned)total, CRYPTO_LIST_MAX, la->list);
      cmd_reply(ctx, line);
      return;
    }

    if(rc == CRYPTO_LIST_BADSYM)
    {
      cmd_reply(ctx, "Lists hold coin symbols, not ranks or ranges. "
          "Example: !crypto --add bags BTC ETH");
      return;
    }

    if(rc != CRYPTO_LIST_OK)
    {
      cmd_reply(ctx, "Couldn't save that list right now — sorry.");
      return;
    }

    snprintf(line, sizeof(line),
        CLR_GREEN "%s" CLR_RESET " @%s — %u symbol%s (view: !crypto @%s)",
        (changed > 0) ? "Saved" : "No change", la->list, (unsigned)total,
        (total == 1) ? "" : "s", la->list);
    cmd_reply(ctx, line);
    return;
  }

  rc = crypto_lists_del(ns->id, la->list, la->items, la->nitems,
      &changed, &dropped);

  if(rc == CRYPTO_LIST_NOSUCH)
  {
    snprintf(line, sizeof(line),
        "No list called '%s'. Try !crypto --list", la->list);
    cmd_reply(ctx, line);
    return;
  }

  if(rc != CRYPTO_LIST_OK)
  {
    cmd_reply(ctx, "Couldn't update that list right now — sorry.");
    return;
  }

  if(changed == 0)
  {
    snprintf(line, sizeof(line), "Nothing in @%s matched.", la->list);
    cmd_reply(ctx, line);
    return;
  }

  if(dropped)
    snprintf(line, sizeof(line), CLR_GREEN "Removed" CLR_RESET
        " the last symbol — @%s is gone.", la->list);
  else
    snprintf(line, sizeof(line), CLR_GREEN "Removed" CLR_RESET
        " %u from @%s.", (unsigned)changed, la->list);

  cmd_reply(ctx, line);
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
    const coinmarketcap_coin_detail_t *d,
    const coinmarketcap_coin_info_t *info)
{
  char   p1h[48], p24h[48], p7d[48], p30d[48], p60d[48], p90d[48];
  char   vol_mv[48];
  char   price[64], cap[32], fdcap[32], vol[32];
  char   supply_c[32], supply_t[32], supply_m[32];
  char   pairs[24];
  char   line[CRYPTO_REPLY_SZ];
  char   tail[CRYPTO_REPLY_SZ];
  char   date_clean[COINMARKETCAP_DATE_SZ];
  char  *tpos;
  double turnover = 0.0;

  // Truncate date_added to date only.
  snprintf(date_clean, sizeof(date_clean), "%s", d->date_added);
  tpos = strchr(date_clean, 'T');

  if(tpos != NULL)
    *tpos = '\0';

  crypto_fmt_price_wide(d->base.price, price, sizeof(price));
  crypto_fmt_usd(d->base.market_cap,          cap,   sizeof(cap));
  crypto_fmt_usd(d->fully_diluted_market_cap, fdcap, sizeof(fdcap));
  crypto_fmt_usd(d->base.volume_24h,          vol,   sizeof(vol));

  crypto_fmt_move(d->base.pct_1h,  p1h,  sizeof(p1h));
  crypto_fmt_move(d->base.pct_24h, p24h, sizeof(p24h));
  crypto_fmt_move(d->base.pct_7d,  p7d,  sizeof(p7d));
  crypto_fmt_move(d->pct_30d,      p30d, sizeof(p30d));
  crypto_fmt_move(d->pct_60d,      p60d, sizeof(p60d));
  crypto_fmt_move(d->pct_90d,      p90d, sizeof(p90d));
  crypto_fmt_move(d->volume_change_24h, vol_mv, sizeof(vol_mv));

  crypto_fmt_supply(d->base.circulating_supply, supply_c, sizeof(supply_c));
  crypto_fmt_supply(d->base.total_supply,       supply_t, sizeof(supply_t));
  crypto_fmt_supply(d->base.max_supply,         supply_m, sizeof(supply_m));
  crypto_fmt_count (d->base.num_market_pairs,   pairs,    sizeof(pairs));

  if(d->base.market_cap > 0.0)
    turnover = (d->base.volume_24h / d->base.market_cap) * 100.0;

  // Header: what it is, before what it costs. Category and tags come
  // from the metadata leg and are simply absent when it did not land.
  tail[0] = '\0';

  if(info != NULL && info->valid)
  {
    size_t used = 0;

    if(info->category[0] != '\0')
      crypto_append(tail, sizeof(tail), &used,
          CRYPTO_CARD_DOT "%s", info->category);

    for(uint8_t i = 0; i < info->tag_count; i++)
      crypto_append(tail, sizeof(tail), &used, "%s%s",
          i == 0 ? CRYPTO_CARD_DOT : ", ", info->tags[i]);
  }

  snprintf(line, sizeof(line),
      CLR_BOLD CLR_WHITE "%s" CLR_RESET
      " (" CLR_YELLOW "%s" CLR_RESET ") "
      CLR_GRAY "#%d" CLR_RESET "%s",
      d->base.name, d->base.symbol, d->base.cmc_rank, tail);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL CLR_BOLD CLR_WHITE "%s" CLR_RESET
      CRYPTO_CARD_DOT "1h %s  24h %s  7d %s  30d %s  60d %s  90d %s",
      "Price", price, p1h, p24h, p7d, p30d, p60d, p90d);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL "cap " CLR_WHITE "%s" CLR_RESET
      " (%.2f%% of all crypto)"
      CRYPTO_CARD_DOT "fully diluted " CLR_WHITE "%s" CLR_RESET,
      "Market", cap, d->market_cap_dominance, fdcap);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL CLR_WHITE "%s" CLR_RESET " in 24h %s"
      CRYPTO_CARD_DOT CLR_WHITE "%s" CLR_RESET " market pairs"
      CRYPTO_CARD_DOT "turnover " CLR_WHITE "%.1f%%" CLR_RESET,
      "Volume", vol, vol_mv, pairs, turnover);
  cmd_reply(ctx, line);

  // Supply reads as a sentence: how much exists, out of how much ever
  // can. An uncapped token says so rather than showing a bare "n/a".
  if(d->base.max_supply > 0.0)
    snprintf(tail, sizeof(tail), " of " CLR_WHITE "%s" CLR_RESET
        " max (%.1f%% issued)", supply_m,
        (d->base.circulating_supply / d->base.max_supply) * 100.0);
  else if(info != NULL && info->valid && info->infinite_supply)
    snprintf(tail, sizeof(tail),
        CRYPTO_CARD_DOT CLR_GRAY "no supply cap" CLR_RESET);
  else
    snprintf(tail, sizeof(tail),
        CRYPTO_CARD_DOT CLR_GRAY "no published maximum" CLR_RESET);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL CLR_WHITE "%s" CLR_RESET " circulating%s"
      CRYPTO_CARD_DOT CLR_WHITE "%s" CLR_RESET " minted in total",
      "Supply", supply_c, tail, supply_t);
  cmd_reply(ctx, line);

  if(info != NULL && info->valid && info->chain_count > 0)
  {
    size_t used = 0;

    tail[0] = '\0';

    for(uint8_t i = 0; i < info->chain_count; i++)
      crypto_append(tail, sizeof(tail), &used, "%s" CLR_WHITE "%s" CLR_RESET,
          i == 0 ? "" : CRYPTO_CARD_DOT, info->chains[i]);

    if(info->chain_total > info->chain_count)
      crypto_append(tail, sizeof(tail), &used,
          CLR_GRAY "  (+%u more)" CLR_RESET,
          (unsigned)(info->chain_total - info->chain_count));

    snprintf(line, sizeof(line), CRYPTO_CARD_LABEL "%s", "Chains", tail);
    cmd_reply(ctx, line);
  }

  // Closing line: where to go next. Assembled piecewise because every
  // part is optional — and skipped entirely when none of them landed.
  {
    const bool meta = (info != NULL && info->valid);
    size_t     used = 0;

    tail[0] = '\0';

    if(meta && info->website[0] != '\0')
      crypto_append(tail, sizeof(tail), &used,
          CLR_WHITE "%s" CLR_RESET, info->website);

    // Handles are expanded to full URLs rather than shown as "r/foo" or
    // "@foo": a chat client linkifies what it can open, and nothing else.
    if(meta && info->subreddit[0] != '\0')
      crypto_append(tail, sizeof(tail), &used, "%shttps://reddit.com/r/%s",
          used > 0 ? CRYPTO_CARD_DOT : "", info->subreddit);

    if(meta && info->twitter[0] != '\0')
      crypto_append(tail, sizeof(tail), &used, "%shttps://x.com/%s",
          used > 0 ? CRYPTO_CARD_DOT : "", info->twitter);

    if(date_clean[0] != '\0')
      crypto_append(tail, sizeof(tail), &used, "%slisted %s",
          used > 0 ? CRYPTO_CARD_DOT : "", date_clean);

    if(tail[0] != '\0')
    {
      snprintf(line, sizeof(line), CRYPTO_CARD_LABEL "%s", "Links", tail);
      cmd_reply(ctx, line);
    }
  }
}

// --mcap card
//
// Six lines, each a fixed label column followed by dot-separated facts.
// Everything here is read-only formatting of one global snapshot plus,
// when it is already warm, the listings cache.


// Advancers, decliners and the day's extremes among the top-ranked
// coins. Leaves out->valid false when the listings cache is cold: the
// card is a global-metrics view and must not spend a listings credit to
// decorate itself.
static void
crypto_breadth_survey(crypto_breadth_t *out)
{
  coinmarketcap_coin_t *rows;
  uint32_t              n = 0;

  memset(out, 0, sizeof(*out));

  if(!coinmarketcap_listings_cache_fresh())
    return;

  rows = mem_alloc(CRYPTO_CTX, "breadth", sizeof(*rows) * CRYPTO_BREADTH_N);

  if(coinmarketcap_get_listings(CRYPTO_BREADTH_N, COINMARKETCAP_SORT_RANK,
      false, rows, CRYPTO_BREADTH_N, &n) != SUCCESS || n == 0)
  {
    mem_free(rows);
    return;
  }

  out->best_pct  = rows[0].pct_24h;
  out->worst_pct = rows[0].pct_24h;
  snprintf(out->best,  sizeof(out->best),  "%s", rows[0].symbol);
  snprintf(out->worst, sizeof(out->worst), "%s", rows[0].symbol);

  for(uint32_t i = 0; i < n; i++)
  {
    const coinmarketcap_coin_t *c = &rows[i];

    if(c->pct_24h > 0.0)      out->up++;
    else if(c->pct_24h < 0.0) out->down++;

    if(c->pct_24h > out->best_pct)
    {
      out->best_pct = c->pct_24h;
      snprintf(out->best, sizeof(out->best), "%s", c->symbol);
    }

    if(c->pct_24h < out->worst_pct)
    {
      out->worst_pct = c->pct_24h;
      snprintf(out->worst, sizeof(out->worst), "%s", c->symbol);
    }
  }

  out->counted = (int32_t)n;
  out->valid   = true;

  mem_free(rows);
}

static void
crypto_reply_mcap(const cmd_ctx_t *ctx, const coinmarketcap_global_t *g)
{
  crypto_breadth_t breadth;
  char             line[CRYPTO_REPLY_SZ];
  char             cap[32], vol[32], vol_rep[32], alt[32];
  char             cap_mv[48], vol_mv[48];
  char             btc_mv[48], eth_mv[48];
  char             defi_c[32], defi_v[32], defi_mv[48];
  char             stable_c[32], stable_v[32], stable_mv[48];
  char             deriv_v[32], deriv_mv[48];
  char             n_active[24], n_total[24], n_exch[24], n_pairs[24];
  char             n_new[24];
  char             age[24];
  double           turnover  = 0.0;
  double           alt_share = 0.0;

  crypto_fmt_usd(g->total_cap,          cap,      sizeof(cap));
  crypto_fmt_usd(g->total_vol,          vol,      sizeof(vol));
  crypto_fmt_usd(g->total_vol_reported, vol_rep,  sizeof(vol_rep));
  crypto_fmt_usd(g->altcoin_cap,        alt,      sizeof(alt));
  crypto_fmt_usd(g->defi_cap,           defi_c,   sizeof(defi_c));
  crypto_fmt_usd(g->defi_vol_24h,       defi_v,   sizeof(defi_v));
  crypto_fmt_usd(g->stablecoin_cap,     stable_c, sizeof(stable_c));
  crypto_fmt_usd(g->stablecoin_vol,     stable_v, sizeof(stable_v));
  crypto_fmt_usd(g->derivatives_vol,    deriv_v,  sizeof(deriv_v));

  crypto_fmt_move(crypto_pct_change(g->total_cap, g->total_cap_yest,
      g->total_cap_chg_24h), cap_mv, sizeof(cap_mv));
  crypto_fmt_move(crypto_pct_change(g->total_vol, g->total_vol_yest,
      g->total_vol_chg_24h), vol_mv, sizeof(vol_mv));

  crypto_fmt_points(g->btc_dom_chg_24h, btc_mv, sizeof(btc_mv));
  crypto_fmt_points(g->eth_dom_chg_24h, eth_mv, sizeof(eth_mv));

  crypto_fmt_move(g->defi_chg_24h,        defi_mv,   sizeof(defi_mv));
  crypto_fmt_move(g->stablecoin_chg_24h,  stable_mv, sizeof(stable_mv));
  crypto_fmt_move(g->derivatives_chg_24h, deriv_mv,  sizeof(deriv_mv));

  crypto_fmt_count(g->active_cryptos,      n_active, sizeof(n_active));
  crypto_fmt_count(g->total_cryptos,       n_total,  sizeof(n_total));
  crypto_fmt_count(g->active_exchanges,    n_exch,   sizeof(n_exch));
  crypto_fmt_count(g->active_market_pairs, n_pairs,  sizeof(n_pairs));
  crypto_fmt_count(g->new_cryptos_24h,     n_new,    sizeof(n_new));

  crypto_fmt_age(g->fetched_at > 0
      ? (int64_t)time(NULL) - g->fetched_at : 0, age, sizeof(age));

  // Turnover — the slice of total capitalisation that changed hands in
  // the last day. The single best one-number read on market heat.
  if(g->total_cap > 0.0)
  {
    turnover  = (g->total_vol / g->total_cap) * 100.0;
    alt_share = (g->altcoin_cap / g->total_cap) * 100.0;
  }

  snprintf(line, sizeof(line),
      CLR_BOLD CLR_WHITE "Global Cryptocurrency Market" CLR_RESET
      "  " CLR_GRAY "as of %s ago" CLR_RESET, age);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL CLR_BOLD CLR_WHITE "%s" CLR_RESET " %s"
      CRYPTO_CARD_DOT "24h volume " CLR_WHITE "%s" CLR_RESET " %s"
      CLR_GRAY " (%s reported)" CLR_RESET
      CRYPTO_CARD_DOT "turnover " CLR_WHITE "%.1f%%" CLR_RESET,
      "Market", cap, cap_mv, vol, vol_mv, vol_rep, turnover);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL CLR_YELLOW "BTC" CLR_RESET " %.1f%% %s"
      CRYPTO_CARD_DOT CLR_CYAN "ETH" CLR_RESET " %.1f%% %s"
      CRYPTO_CARD_DOT "altcoins " CLR_WHITE "%s" CLR_RESET " (%.1f%%)",
      "Dominance", g->btc_dom, btc_mv, g->eth_dom, eth_mv, alt, alt_share);
  cmd_reply(ctx, line);

  // Each sector's percentage belongs to its 24-hour volume, so the cap
  // and the traded figure are stated side by side and the arrow sits
  // against the one it actually describes.
  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL "stablecoins " CLR_WHITE "%s" CLR_RESET " cap, "
      CLR_WHITE "%s" CLR_RESET " traded %s"
      CRYPTO_CARD_DOT "DeFi " CLR_WHITE "%s" CLR_RESET " cap, "
      CLR_WHITE "%s" CLR_RESET " traded %s"
      CRYPTO_CARD_DOT "derivatives " CLR_WHITE "%s" CLR_RESET " traded %s",
      "Sectors", stable_c, stable_v, stable_mv, defi_c, defi_v, defi_mv,
      deriv_v, deriv_mv);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      CRYPTO_CARD_LABEL CLR_WHITE "%s" CLR_RESET " active of "
      CLR_WHITE "%s" CLR_RESET " tracked"
      CRYPTO_CARD_DOT CLR_WHITE "%s" CLR_RESET " exchanges"
      CRYPTO_CARD_DOT CLR_WHITE "%s" CLR_RESET " market pairs"
      CRYPTO_CARD_DOT CLR_WHITE "%s" CLR_RESET " new in 24h",
      "Universe", n_active, n_total, n_exch, n_pairs, n_new);
  cmd_reply(ctx, line);

  crypto_breadth_survey(&breadth);

  if(breadth.valid)
  {
    char best[48], worst[48];

    crypto_fmt_move(breadth.best_pct,  best,  sizeof(best));
    crypto_fmt_move(breadth.worst_pct, worst, sizeof(worst));

    snprintf(line, sizeof(line),
        CRYPTO_CARD_LABEL "top %" PRId32 ": " CLR_GREEN "%" PRId32
        " up" CLR_RESET " / " CLR_RED "%" PRId32 " down" CLR_RESET
        CRYPTO_CARD_DOT "best " CLR_WHITE "%s" CLR_RESET " %s"
        CRYPTO_CARD_DOT "worst " CLR_WHITE "%s" CLR_RESET " %s",
        "Breadth", breadth.counted, breadth.up, breadth.down,
        breadth.best, best, breadth.worst, worst);
    cmd_reply(ctx, line);
  }
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
    crypto_reply_verbose(&ctx, &res->detail, &res->info);

  mem_free(r);
}

static void
crypto_done_mcap(const coinmarketcap_global_result_t *res, void *user)
{
  crypto_req_t *r   = (crypto_req_t *)user;
  cmd_ctx_t     ctx = r->ctx;

  ctx.msg = &r->msg;

  if(res->err[0] != '\0')
    cmd_reply(&ctx, res->err);
  else
    crypto_reply_mcap(&ctx, &res->global);

  mem_free(r);
}

// Command callback

static void
crypto_cmd_crypto(const cmd_ctx_t *ctx)
{
  crypto_req_t   stack_req;
  crypto_largs_t la;
  crypto_req_t  *r;

  // Parse args on-stack first so validation failures don't allocate.
  memset(&stack_req, 0, sizeof(stack_req));
  stack_req.kind = CRYPTO_REQ_TABLE;

  if(!crypto_parse_args(ctx->args, &stack_req, &la, ctx))
    return;

  // List management is pure storage — it needs no API key and no market
  // data, so it is answered before the provider is ever consulted.
  if(la.op != CRYPTO_OP_NONE)
  {
    crypto_list_cmd(ctx, &la);
    return;
  }

  if(!coinmarketcap_apikey_configured())
  {
    cmd_reply(ctx,
        "Error: CoinMarketCap API key not configured. "
        "Set plugin.coinmarketcap.creds.apikey via /set");
    return;
  }

  if(!crypto_expand(ctx, &stack_req, &la))
    return;

  stack_req.limit = coinmarketcap_default_limit_kv_value();

  // --mcap: serve from cache if fresh, otherwise fetch.
  if(stack_req.kind == CRYPTO_REQ_MCAP)
  {
    if(coinmarketcap_global_cache_fresh())
    {
      coinmarketcap_global_t g;

      if(coinmarketcap_get_global(&g) == SUCCESS)
      {
        crypto_reply_mcap(ctx, &g);
        return;
      }
    }

    r = crypto_req_new(ctx);
    r->kind = CRYPTO_REQ_MCAP;

    if(coinmarketcap_fetch_global_async(crypto_done_mcap, r)
        == ASYNC_FAILED_UNDELIVERED)
    {
      cmd_reply(ctx,
          "Error: failed to submit market-wide request. "
          "Check plugin.coinmarketcap.creds.apikey.");
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
        crypto_done_detail, r)
        == ASYNC_FAILED_UNDELIVERED)
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

  if(coinmarketcap_fetch_listings_async(crypto_done_listings, r)
      == ASYNC_FAILED_UNDELIVERED)
  {
    cmd_reply(ctx,
        "Error: failed to submit listings request. "
        "Check plugin.coinmarketcap.creds.apikey.");
    mem_free(r);
  }
}

// NL hints

// The symbol is optional because the market-wide card takes none: a
// question about "the market" must not be answered with an invented
// ticker.
static const cmd_nl_slot_t crypto_nl_slots[] = {
  { .name  = "symbol",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_OPTIONAL },
};

static const cmd_nl_example_t crypto_nl_examples[] = {
  { .utterance  = "what's bitcoin at?",
    .invocation = "/crypto BTC" },
  { .utterance  = "price of ETH",
    .invocation = "/crypto ETH" },
  { .utterance  = "how's the crypto market doing?",
    .invocation = "/crypto --mcap" },
  { .utterance  = "what's the total market cap right now?",
    .invocation = "/crypto --mcap" },
};

static const cmd_nl_t crypto_nl = {
  .when          = "User asks for a cryptocurrency spot price, or for the "
                   "state of the cryptocurrency market as a whole.",
  .syntax        = "/crypto <TICKER> | /crypto --mcap",
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
      "crypto [options] [@list|symbol|rank|range…] | crypto --verbose <symbol>"
      " | crypto --mcap | crypto --list"
      " | crypto --add|--del <list> <symbols…>",
      "Show cryptocurrency market data from CoinMarketCap",
      "Quote coins by symbol, rank or range, or a saved list with @name. "
      "--verbose (-v) gives one coin the full card: price history, "
      "supply, tags, the chains it is deployed on and its links. "
      "--mcap reports the whole market: capitalisation, volume, "
      "dominance, sectors and breadth. "
      "--list shows your lists; --add creates or appends to one and "
      "--del removes symbols (a list disappears when its last symbol "
      "does). Lists are private to your namespace and holding or "
      "changing one requires a known user.",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      crypto_cmd_crypto, NULL, NULL, "c",
      NULL, 0, NULL, &crypto_nl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, CRYPTO_CTX, "crypto command plugin initialized");

  return(SUCCESS);
}

// Schema bootstrap runs in start(), after the DB plugin is up. Every list
// entry point re-ensures it anyway, so a database that arrives later
// still yields working lists without a reload.
static bool
crypto_start(void)
{
  if(crypto_lists_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, CRYPTO_CTX,
        "list schema init failed (lists will error until the DB is up)");

  return(SUCCESS);
}

static void
crypto_deinit(void)
{
  cmd_unregister_path("crypto");

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
    { .name = "bot_chat" },
    { .name = "service_coinmarketcap" },
  },
  .requires_count  = 2,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = crypto_init,
  .start           = crypto_start,
  .stop            = NULL,
  .deinit          = crypto_deinit,
  .ext             = NULL,
};
