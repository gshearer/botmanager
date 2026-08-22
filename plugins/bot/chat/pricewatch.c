// botmanager — MIT
// Price watch: the one chore whose moment is neither a clock nor a
// person, but the market.
//
// "How do you know?" — *you asked me.* A row here exists because
// somebody typed (or said) "tell me if bitcoin drops below 90k", and
// the report goes back to the venue they asked from. That makes it the
// purest ASKED cue in the plugin: the human chose the subject, the bot
// only chose the moment, so quiet hours may delay it and the
// unsolicited budget never touches it (CHATBOT.md §The initiative
// rule, §The voice budget and quiet hours).
//
// Two tables and one fence:
//
//   chat_watchlist   what a bot is WILLING to watch — admin-curated,
//                    per bot, and the only thing a user may set a
//                    threshold on.
//   chat_pricewatch  one person's one threshold on one of those pairs.
//
// The fence (CARE-7 §D6) is that this file reads public market data
// and nothing else: bulk tickers through feature/exchange, no order or
// account API, no whenmoon symbol, no CoinMarketCap credits. A bot may
// tell you what a coin costs; it may not trade one.
//
// The shape is CARE-5's, with the trigger swapped:
//
//   scan the armed rows → one snapshot → claim each crossing once
//     → hand a report to the deferred spine and speak nothing
//
// Delivery, venue, restraint and lateness are therefore CARE-1/2/3's
// and cannot be got wrong here — including the useful half of a quiet
// hour: the crossing is spent the moment it happens, while the report
// it wrote waits for morning as an ordinary pending row.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"
#include "colors.h"
#include "db.h"
#include "exchange_api.h"
#include "util.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define PRICEWATCH_CTX "pricewatch"

// How many pairs one bot may be told to watch, and how many armed
// thresholds one sweep carries. Both bound a heap struct that is built
// fresh every sweep; neither is a policy statement — the watchlist is
// admin-curated and the per-person cap is the real limit on rows.
#define PRICEWATCH_PAIRS_MAX      64
#define PRICEWATCH_ROWS_MAX      128

#define PRICEWATCH_LIST_MAX       20

// Long enough to survive the longest sane quiet window (23→8 is nine
// hours) so a crossing at one minute past eleven is still reported at
// eight; short enough that nobody is told about yesterday's price. The
// report carries the wall-clock time it was observed, so a delayed one
// stays true rather than becoming a lie.
#define PRICEWATCH_REPORT_TTL_SECS  (12 * 3600)

// Which way the price has to move. Stored as the SMALLINT `dir`, so
// the values are part of the schema and may not be renumbered.
typedef enum
{
  PRICEWATCH_BELOW = 0,
  PRICEWATCH_ABOVE = 1,
} pricewatch_dir_t;

// One armed threshold, carried from the scan to the claim. The tuple
// rides whole (the dcfc359 rule) because the report is a synthetic cue
// and a cue missing any of it resolves to no dossier.
typedef struct
{
  int64_t  id;
  int64_t  dossier_id;
  double   threshold;
  double   observed;                            // NAN until the snapshot lands
  uint8_t  dir;
  char     pair       [EXCHANGE_PRODUCT_ID_SZ];
  char     method_name[METHOD_NAME_SZ];
  char     channel    [METHOD_CHANNEL_SZ];
  char     sender     [METHOD_SENDER_SZ];
  chat_identity_t who;
  char     metadata   [METHOD_META_SZ];
} pricewatch_row_t;

// One watchlisted pair and what the last snapshot said about it.
// `seen` false means the exchange did not publish the pair at all —
// the answer to "did that ticker I typed ever exist?", deferred from
// the moment it was added to the moment the market was asked.
typedef struct
{
  char   pair[EXCHANGE_PRODUCT_ID_SZ];
  double price;
  bool   seen;
} pricewatch_quote_t;

// The whole sweep, one allocation. ⚠ If the chat plugin reloads with a
// snapshot airborne the exchange driver suppresses our callback and
// this LEAKS whole — the same accepted lifecycle outcome the weather
// watch documents. The fresh mapping starts with clear in-flight
// flags, so nothing wedges.
typedef struct
{
  soul_sched_t       *sched;
  uint32_t            chore;
  uint32_t            ns_id;
  char                bot_name[BOT_NAME_SZ];
  char                exchange[32];
  uint32_t            n_quotes;
  uint32_t            n_rows;
  pricewatch_quote_t  quotes[PRICEWATCH_PAIRS_MAX];
  pricewatch_row_t    rows  [PRICEWATCH_ROWS_MAX];
} pricewatch_sweep_t;

typedef async_rc_t (*pricewatch_tickers_fn_t)(const char *,
    exchange_done_tickers_cb_t, void *);

static pricewatch_tickers_fn_t pricewatch_tickers_fn;

// ---------- small shared plumbing ----------

static double
pricewatch_col_f64(const db_result_t *res, uint32_t row, uint32_t col)
{
  const char *s = db_result_get(res, row, col);

  return(s != NULL && s[0] != '\0' ? strtod(s, NULL) : NAN);
}

// ---------- the vocabulary: pairs, directions, prices ----------

// A product id as the abstraction spells it: BTC-USD, uppercase,
// hyphenated. Accepts what a human types — btc/usd, and a bare symbol
// meaning the dollar pair — and refuses anything that is not
// alphanumerics and one hyphen, which is also what keeps a stored pair
// safe to interpolate into SQL.
static bool
pricewatch_canon_pair(const char *in, char *dst, size_t cap)
{
  size_t o    = 0;
  bool   dash = false;

  if(in == NULL || cap < EXCHANGE_PRODUCT_ID_SZ)
    return(false);

  for(size_t i = 0; in[i] != '\0'; i++)
  {
    char c = (char)toupper((unsigned char)in[i]);

    if(c == '/')
      c = '-';

    if(c == '-')
    {
      if(dash || o == 0)
        return(false);

      dash = true;
    }

    else if(!isalnum((unsigned char)c))
      return(false);

    if(o + 1 >= cap)
      return(false);

    dst[o++] = c;
  }

  dst[o] = '\0';

  if(o == 0 || dst[o - 1] == '-')
    return(false);

  // "watch BTC" means the dollar pair, because that is what everybody
  // means. Anything else they must spell out.
  if(!dash)
  {
    if(o + 4 >= cap)
      return(false);

    snprintf(dst + o, cap - o, "-USD");
  }

  return(true);
}

static bool
pricewatch_parse_dir(const char *s, pricewatch_dir_t *out)
{
  if(strcasecmp(s, "above") == 0 || strcasecmp(s, "over") == 0
      || strcasecmp(s, "up") == 0 || strcmp(s, ">") == 0)
  {
    *out = PRICEWATCH_ABOVE;
    return(true);
  }

  if(strcasecmp(s, "below") == 0 || strcasecmp(s, "under") == 0
      || strcasecmp(s, "down") == 0 || strcmp(s, "<") == 0)
  {
    *out = PRICEWATCH_BELOW;
    return(true);
  }

  return(false);
}

// "$90,000", "90k", "0.42" — the forms people actually say. Strict
// about the result and forgiving about the spelling: a price that is
// not a finite positive number is refused rather than stored as one.
static bool
pricewatch_parse_price(const char *s, double *out)
{
  char   clean[64];
  char  *endp = NULL;
  size_t o    = 0;
  double v;

  if(s == NULL)
    return(false);

  for(size_t i = 0; s[i] != '\0' && o + 1 < sizeof(clean); i++)
    if(s[i] != '$' && s[i] != ',' && s[i] != '_')
      clean[o++] = s[i];

  clean[o] = '\0';
  v        = strtod(clean, &endp);

  if(endp == clean)
    return(false);

  while(*endp == ' ')
    endp++;

  if(*endp == 'k' || *endp == 'K')
  {
    v *= 1000.0;
    endp++;
  }

  else if(*endp == 'm' || *endp == 'M')
  {
    v *= 1000000.0;
    endp++;
  }

  while(*endp == ' ')
    endp++;

  if(*endp != '\0' || !isfinite(v) || v <= 0.0)
    return(false);

  *out = v;
  return(true);
}

// Crypto spans eight orders of magnitude inside one snapshot, so the
// precision follows the number: nobody wants BTC to four decimals or a
// meme coin rounded to zero. Trailing zeros are trimmed because
// "1.05000000" reads as a machine talking.
static void
pricewatch_fmt_price(double v, char *dst, size_t cap)
{
  double a = fabs(v);
  size_t n;
  int    dp;

  if(!isfinite(v))
  {
    snprintf(dst, cap, "?");
    return;
  }

  if(a >= 1000.0)   dp = 2;
  else if(a >= 1.0) dp = 4;
  else              dp = 8;

  snprintf(dst, cap, "%.*f", dp, v);

  if(strchr(dst, '.') == NULL)
    return;

  n = strlen(dst);

  while(n > 0 && dst[n - 1] == '0')
    n--;

  if(n > 0 && dst[n - 1] == '.')
    n--;

  dst[n] = '\0';
}

static const char *
pricewatch_dir_word(int64_t dir)
{
  return(dir == PRICEWATCH_ABOVE ? "above" : "below");
}

static bool
pricewatch_crossed(double price, int64_t dir, double threshold)
{
  if(!isfinite(price))
    return(false);

  return(dir == PRICEWATCH_ABOVE ? price >= threshold : price <= threshold);
}

// ---------- per-bot configuration ----------

static bool
pricewatch_enabled(const char *bot_name)
{

  return(kv_get_bot_uint(bot_name, "behavior.soul.pricewatch.enabled") != 0);
}

// An empty name is passed through, not defaulted: the schema declares
// "coinbase" and substituting it here would refuse `set kv --clear`.
// Clearing the key idles the sweep — the exact outcome the schema entry
// promises for a name no exchange answers to.
static void
pricewatch_exchange_for(const char *bot_name, char *dst, size_t cap)
{
  const char *v;

  v = kv_get_bot_str(bot_name, "behavior.soul.pricewatch.exchange");

  strlcpy(dst, v != NULL ? v : "", cap);
}

// ---------- schema ----------

void
chatbot_pricewatch_ensure_schema(void)
{
  // The chat DDL discipline (memory_ensure_schema): owner-run
  // idempotent batches at plugin start(), after dossier_register_config
  // so the dossier(id) FK target exists.
  //
  // The watchlist has no ns_id and wants none: a bot belongs to exactly
  // one namespace, and the list is a statement about what THIS bot is
  // willing to watch (§D6), not about who may ask.
  (void)db_exec(
      "CREATE TABLE IF NOT EXISTS chat_watchlist ("
      " bot_name   VARCHAR(64)  NOT NULL,"
      " pair       VARCHAR(32)  NOT NULL,"
      " added_by   VARCHAR(128) NOT NULL DEFAULT '',"
      " added_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " last_price NUMERIC,"
      " last_seen  TIMESTAMPTZ,"
      " PRIMARY KEY (bot_name, pair)"
      ")", PRICEWATCH_CTX);

  (void)db_exec(
      "CREATE TABLE IF NOT EXISTS chat_pricewatch ("
      " id          BIGSERIAL    PRIMARY KEY,"
      " ns_id       INTEGER      NOT NULL REFERENCES userns(id) ON DELETE CASCADE,"
      " dossier_id  BIGINT       REFERENCES dossier(id) ON DELETE SET NULL,"
      " bot_name    VARCHAR(64)  NOT NULL,"
      " sender      VARCHAR(128) NOT NULL,"
      " nickname    VARCHAR(64)  NOT NULL DEFAULT '',"
      " username    VARCHAR(64)  NOT NULL DEFAULT '',"
      " hostname    VARCHAR(128) NOT NULL DEFAULT '',"
      " verified_id VARCHAR(128) NOT NULL DEFAULT '',"
      " metadata    VARCHAR(512) NOT NULL DEFAULT '',"
      " method_name VARCHAR(64)  NOT NULL,"
      " channel     VARCHAR(128) NOT NULL DEFAULT '',"
      " pair        VARCHAR(32)  NOT NULL,"
      " dir         SMALLINT     NOT NULL,"
      " threshold   NUMERIC      NOT NULL,"
      " created_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
      " fired_at    TIMESTAMPTZ"
      ")", PRICEWATCH_CTX);

  // The sweep's only hot query is "what is still armed for this bot".
  (void)db_exec(
      "CREATE INDEX IF NOT EXISTS idx_chat_pricewatch_armed"
      " ON chat_pricewatch(ns_id, bot_name) WHERE fired_at IS NULL", PRICEWATCH_CTX);
}

// ---------- the watchlist ----------

// Is the pair on this bot's watchlist, and what did the last sweep say
// it cost? The price is what lets the ack tell somebody their threshold
// is already met — NAN when the pair has never been priced, which reads
// the same as "no opinion" at every call site.
static bool
pricewatch_on_watchlist(const char *bot_name, const char *pair,
    double *out_price)
{
  db_result_t *res;
  char        *e_bot;
  char         sql[512];
  bool         found = false;

  if(out_price != NULL)
    *out_price = NAN;

  e_bot = db_escape(bot_name);

  if(e_bot == NULL)
    return(false);

  // `pair` is canonical by construction at every call site, so it
  // carries only [A-Z0-9-] and needs no escape of its own.
  snprintf(sql, sizeof(sql),
      "SELECT COALESCE(last_price::TEXT, '') FROM chat_watchlist"
      " WHERE bot_name = '%s' AND pair = '%s'", e_bot, pair);
  mem_free(e_bot);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    found = true;

    if(out_price != NULL)
      *out_price = pricewatch_col_f64(res, 0, 0);
  }

  db_result_free(res);
  return(found);
}

static const cmd_arg_desc_t ad_bot_watchlist[] = {
  { "action", CMD_ARG_NONE, CMD_ARG_REQUIRED, 8,  NULL },
  { "pair",   CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
};

// /bot <name> watchlist <add|del> <PAIR>
//
// One verb rather than two children: `add` and `del` differ by a
// single statement and share every gate, and a grandchild under `bot`
// would buy nothing but a deeper path. Registered under `bot`, so it
// inherits the parent's admin gate (TAXO-4) — which is the
// whole of §D6's "admin-curated".
static void
cmd_bot_watchlist(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  const char  *bot_name = bot_inst_name(ctx->bot);
  const char  *action   = ctx->parsed->argv[0];
  char        *e_bot;
  char        *e_who;
  char         pair[EXCHANGE_PRODUCT_ID_SZ];
  char         sql[768];
  char         ack[192];
  bool         adding;

  if(strcasecmp(action, "add") == 0)
    adding = true;

  else if(strcasecmp(action, "del") == 0 || strcasecmp(action, "rm") == 0)
    adding = false;

  else
  {
    cmd_reply(ctx, "say 'add' or 'del'");
    return;
  }

  if(!pricewatch_canon_pair(ctx->parsed->argv[1], pair, sizeof(pair)))
  {
    cmd_reply(ctx, "that is not a product id — try BTC-USD (or just BTC)");
    return;
  }

  e_bot = db_escape(bot_name);
  e_who = db_escape(ctx->username != NULL ? ctx->username : ctx->msg->sender);

  if(e_bot == NULL || e_who == NULL)
  {
    if(e_bot != NULL) mem_free(e_bot);
    if(e_who != NULL) mem_free(e_who);
    cmd_reply(ctx, "failed to prepare the query");
    return;
  }

  if(adding)
    // ON CONFLICT so re-adding is a no-op rather than an error, and
    // RETURNING so the ack can tell "added" from "already there".
    snprintf(sql, sizeof(sql),
        "INSERT INTO chat_watchlist (bot_name, pair, added_by)"
        " VALUES ('%s', '%s', '%s') ON CONFLICT DO NOTHING RETURNING pair",
        e_bot, pair, e_who);

  else
    // The thresholds people already set on the pair go with it: leaving
    // them armed against a pair the bot no longer watches would be a
    // promise nothing can keep. Reported, never silent.
    snprintf(sql, sizeof(sql),
        "WITH gone AS (DELETE FROM chat_watchlist WHERE bot_name = '%s'"
        " AND pair = '%s' RETURNING pair),"
        " dropped AS (DELETE FROM chat_pricewatch WHERE bot_name = '%s'"
        " AND pair = '%s' AND fired_at IS NULL RETURNING id)"
        " SELECT (SELECT COUNT(*) FROM gone), (SELECT COUNT(*) FROM dropped)",
        e_bot, pair, e_bot, pair);

  mem_free(e_bot);
  mem_free(e_who);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "could not update the watchlist");
    return;
  }

  if(adding)
    snprintf(ack, sizeof(ack), "%s %s to %s's watchlist.",
        res->rows > 0 ? "added" : "already had", pair, bot_name);

  else
  {
    int64_t gone    = db_result_get_i64(res, 0, 0, 0);
    int64_t dropped = db_result_get_i64(res, 0, 1, 0);

    if(gone == 0)
      snprintf(ack, sizeof(ack), "%s was not on %s's watchlist.",
          pair, bot_name);

    else if(dropped > 0)
      snprintf(ack, sizeof(ack),
          "dropped %s from %s's watchlist, and cancelled %" PRId64
          " watch(es) on it.", pair, bot_name, dropped);

    else
      snprintf(ack, sizeof(ack), "dropped %s from %s's watchlist.",
          pair, bot_name);
  }

  clam(CLAM_INFO, PRICEWATCH_CTX, "bot=%s watchlist %s %s by %s",
      bot_name, adding ? "add" : "del", pair,
      ctx->username != NULL ? ctx->username : ctx->msg->sender);

  cmd_reply(ctx, ack);
  db_result_free(res);
}

// /show bot <name> watchlist — everyone-gated on purpose: the refusal a
// user gets from /pricewatch points here, so a surface only an admin
// can read would be a dead end.
static void
cmd_show_bot_watchlist(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  const char  *bot_name = bot_inst_name(ctx->bot);
  char        *e_bot;
  char         exchange[32];
  char         sql[1024];
  char         head[160];

  pricewatch_exchange_for(bot_name, exchange, sizeof(exchange));

  e_bot = db_escape(bot_name);

  if(e_bot == NULL)
  {
    cmd_reply(ctx, "failed to prepare the query");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT w.pair, w.last_price,"
      " GREATEST(0, EXTRACT(EPOCH FROM (NOW() - w.last_seen))::BIGINT),"
      " (w.last_seen IS NOT NULL),"
      " (SELECT COUNT(*) FROM chat_pricewatch p WHERE p.bot_name = w.bot_name"
      "  AND p.pair = w.pair AND p.fired_at IS NULL)"
      " FROM chat_watchlist w WHERE w.bot_name = '%s'"
      " ORDER BY w.pair ASC LIMIT %d", e_bot, PRICEWATCH_PAIRS_MAX);
  mem_free(e_bot);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "query failed");
    return;
  }

  if(res->rows == 0)
  {
    snprintf(head, sizeof(head),
        "%s watches nothing yet — 'bot %s watchlist add BTC-USD'.",
        bot_name, bot_name);
    cmd_reply(ctx, head);
    db_result_free(res);
    return;
  }

  snprintf(head, sizeof(head),
      "%s watches (via " CLR_BOLD "%s" CLR_RESET ")%s:", bot_name, exchange,
      pricewatch_enabled(bot_name) ? "" : " — chore DISABLED");
  cmd_reply(ctx, head);

  for(uint32_t i = 0; i < res->rows; i++)
  {
    const char *seen  = db_result_get(res, i, 3);
    int64_t     armed = db_result_get_i64(res, i, 4, 0);
    char        pair[EXCHANGE_PRODUCT_ID_SZ];
    char        price[48];
    char        age[UTIL_DURATION_SZ];
    char        line[256];

    db_result_copy(pair, sizeof(pair), res, i, 0);

    if(seen != NULL && (seen[0] == 't' || seen[0] == 'T'))
    {
      util_fmt_duration((time_t)db_result_get_i64(res, i, 2, 0), age,
          sizeof(age));
      pricewatch_fmt_price(pricewatch_col_f64(res, i, 1), price,
          sizeof(price));
    }

    else
    {
      // Never seen in a snapshot: either the sweep has not run yet or
      // the exchange does not publish this pair. Deliberately the same
      // line for both, because the operator's next move is the same —
      // wait one sweep, then correct the spelling. ASCII on purpose:
      // the columns are padded by byte count, and an em-dash is three
      // bytes wide and one column wide.
      snprintf(price, sizeof(price), "--");
      snprintf(age,   sizeof(age),   "not priced yet");
    }

    snprintf(line, sizeof(line),
        "  " CLR_BOLD "%-14s" CLR_RESET " %-14s %-14s %" PRId64 " watching",
        pair, price, age, armed);
    cmd_reply(ctx, line);
  }

  db_result_free(res);
}

// ---------- /pricewatch ----------

// The count behind the per-owner watch limit, or -1 when the count could
// not be taken; deferred_pending_for's contract, for the same reason.
static int64_t
pricewatch_pending_for(uint32_t ns_id, const char *owner_pred)
{
  db_result_t *res;
  const char  *cell;
  char         sql[1024];
  int64_t      n = -1;

  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM chat_pricewatch WHERE ns_id = %" PRIu32
      " AND fired_at IS NULL AND %s", ns_id, owner_pred);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0
      && (cell = db_result_get(res, 0, 0)) != NULL)
    n = (int64_t)strtoll(cell, NULL, 10);

  else
    clam(CLAM_WARN, PRICEWATCH_CTX, "pending count failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  return(n);
}

static const cmd_arg_desc_t ad_pricewatch[] = {
  { "pair",      CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
  { "direction", CMD_ARG_NONE, CMD_ARG_REQUIRED, 8,  NULL },
  { "price",     CMD_ARG_NONE, CMD_ARG_REQUIRED, 32, NULL },
};

static void
cmd_pricewatch(const cmd_ctx_t *ctx)
{
  chatbot_state_t  *st;
  userns_t         *ns;
  const char       *bot_name = bot_inst_name(ctx->bot);
  const char       *method_name;
  pricewatch_dir_t  dir;
  double            threshold;
  double            spot;
  uint32_t          cap;
  int64_t           pending;
  char              pair[EXCHANGE_PRODUCT_ID_SZ];
  char              owner_pred[768];
  char              ack[256];
  char              num[48];

  if(!pricewatch_canon_pair(ctx->parsed->argv[0], pair, sizeof(pair)))
  {
    cmd_reply(ctx, "I don't recognise that pair — try BTC-USD (or just BTC)");
    return;
  }

  if(!pricewatch_parse_dir(ctx->parsed->argv[1], &dir))
  {
    cmd_reply(ctx, "say 'above' or 'below'");
    return;
  }

  if(!pricewatch_parse_price(ctx->parsed->argv[2], &threshold))
  {
    cmd_reply(ctx, "that price doesn't parse (try 90000, 90k or $1.25)");
    return;
  }

  // The chore is what makes this a promise rather than a row. Refuse
  // the dead letter up front, exactly as /in refuses a bot that cannot
  // speak.
  if(!pricewatch_enabled(bot_name))
  {
    cmd_reply(ctx, "I'm not watching prices at the moment — an admin has"
        " to turn my price watch on first");
    return;
  }

  if(!pricewatch_on_watchlist(bot_name, pair, &spot))
  {
    char msg[192];

    snprintf(msg, sizeof(msg),
        "%s isn't on my watchlist — 'show bot %s watchlist' is what I"
        " keep an eye on", pair, bot_name);
    cmd_reply(ctx, msg);
    return;
  }

  st = bot_get_handle(ctx->bot);
  ns = bot_get_userns(ctx->bot);

  if(st == NULL || ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace to keep watches in");
    return;
  }

  // The report is persona speech through the deferred spine, so a
  // command-only bot would take the watch and never speak it.
  if(kv_get_bot_uint(bot_name, "behavior.chat.enabled") == 0)
  {
    cmd_reply(ctx, "this bot doesn't speak (chat is disabled) — it would"
        " take this and never tell you");
    return;
  }

  method_name = method_inst_name(ctx->msg->inst);

  if(method_name == NULL || method_name[0] == '\0')
  {
    cmd_reply(ctx, "cannot tell which method to answer on");
    return;
  }

  if(chatbot_row_owner_pred(ctx->msg, owner_pred,
      sizeof(owner_pred)) != SUCCESS)
  {
    cmd_reply(ctx, "failed to prepare the request");
    return;
  }

  // The deferred spine's knob, read here and counted separately: ten
  // pending reminders and ten armed watches are each reasonable, twenty
  // of one is not.
  cap = (uint32_t)kv_get_bot_uint_or_default(bot_name,
      "behavior.soul.deferred.max_pending");

  pending = pricewatch_pending_for(ns->id, owner_pred);

  if(pending < 0)
  {
    cmd_reply(ctx, "I couldn't check how many watches you already have — "
        "try again shortly");
    return;
  }

  if(pending >= (int64_t)cap)
  {
    char msg[160];

    snprintf(msg, sizeof(msg),
        "you already have %u price watches with me — cancel one first"
        " (see 'pricewatch list')", cap);
    cmd_reply(ctx, msg);
    return;
  }

  if(chatbot_pricewatch_insert(ns->id, chatbot_resolve_dossier(st, ctx->msg),
      ctx->msg, bot_name, method_name, pair, (int)dir,
      threshold) != SUCCESS)
  {
    cmd_reply(ctx, "failed to store the watch");
    return;
  }

  pricewatch_fmt_price(threshold, num, sizeof(num));

  clam(CLAM_INFO, PRICEWATCH_CTX,
      "bot=%s watch set by %s: %s %s %s in '%s'", bot_name, ctx->msg->sender,
      pair, pricewatch_dir_word(dir), num, ctx->msg->channel);

  // A threshold the market already satisfies is not an error — it is
  // the honest answer to "tell me if it drops below 90k" when it is at
  // 63k — but silently promising to tell them "once it happens" would
  // be, so the ack says which of the two they just asked for.
  if(pricewatch_crossed(spot, (int64_t)dir, threshold))
  {
    char now[48];

    pricewatch_fmt_price(spot, now, sizeof(now));
    snprintf(ack, sizeof(ack),
        "watching — though %s was at %s when I last looked, already %s"
        " %s, so you'll hear from me on the next sweep.", pair, now,
        pricewatch_dir_word(dir), num);
  }

  else
    snprintf(ack, sizeof(ack),
        "watching — I'll tell you once, when %s goes %s %s.", pair,
        pricewatch_dir_word(dir), num);

  cmd_reply(ctx, ack);
}

// Columns 0..5 are what the renderer reads; the listing query below
// produces exactly this shape.
#define PRICEWATCH_LIST_COLS \
    "id, pair, dir, threshold, channel," \
    " (fired_at IS NOT NULL)"

static void
pricewatch_render(const cmd_ctx_t *ctx, const db_result_t *res)
{
  for(uint32_t i = 0; i < res->rows; i++)
  {
    const char *fired = db_result_get(res, i, 5);
    char        id   [24];
    char        pair [EXCHANGE_PRODUCT_ID_SZ];
    char        venue[METHOD_CHANNEL_SZ];
    char        num  [48];
    char        line [320];

    db_result_copy(id,    sizeof(id),    res, i, 0);
    db_result_copy(pair,  sizeof(pair),  res, i, 1);
    db_result_copy(venue, sizeof(venue), res, i, 4);
    pricewatch_fmt_price(pricewatch_col_f64(res, i, 3), num, sizeof(num));

    snprintf(line, sizeof(line), "  " CLR_BOLD "%s" CLR_RESET "  %s %s %s"
        "  %s%s", id, pair,
        pricewatch_dir_word(db_result_get_i64(res, i, 2, PRICEWATCH_BELOW)),
        num, venue[0] != '\0' ? venue : "DM",
        (fired != NULL && (fired[0] == 't' || fired[0] == 'T'))
            ? "  " CLR_GRAY "(spent)" CLR_RESET : "");

    cmd_reply(ctx, line);
  }
}

static void
cmd_pricewatch_list(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  userns_t    *ns = bot_get_userns(ctx->bot);
  char         pred[768];
  char         sql[1536];

  if(ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace");
    return;
  }

  if(chatbot_row_owner_pred(ctx->msg, pred, sizeof(pred)) != SUCCESS)
  {
    cmd_reply(ctx, "failed to prepare the query");
    return;
  }

  // Armed rows plus the last day's spent ones: "did it fire?" is the
  // question a one-shot watch invites, and answering it from the same
  // list is cheaper than a second verb.
  snprintf(sql, sizeof(sql),
      "SELECT " PRICEWATCH_LIST_COLS " FROM chat_pricewatch"
      " WHERE ns_id = %u AND %s"
      " AND (fired_at IS NULL OR fired_at > NOW() - INTERVAL '1 day')"
      " ORDER BY fired_at NULLS FIRST, id ASC LIMIT %d",
      ns->id, pred, PRICEWATCH_LIST_MAX);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "could not read your price watches");
    return;
  }

  if(res->rows == 0)
    cmd_reply(ctx, "no price watches.");

  else
  {
    cmd_reply(ctx, "price watches:");
    pricewatch_render(ctx, res);
  }

  db_result_free(res);
}

static const cmd_arg_desc_t ad_pricewatch_cancel[] = {
  { "id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 20, NULL },
};

static void
cmd_pricewatch_cancel(const cmd_ctx_t *ctx)
{
  db_result_t *res;
  userns_t    *ns = bot_get_userns(ctx->bot);
  char         pred[768];
  char         sql[1536];
  char         ack[96];
  int64_t      id;

  if(ns == NULL)
  {
    cmd_reply(ctx, "this bot has no namespace");
    return;
  }

  id = (int64_t)strtoll(ctx->parsed->argv[0], NULL, 10);

  if(chatbot_caller_is_admin(ctx, ns))
    snprintf(pred, sizeof(pred), "TRUE");

  else if(chatbot_row_owner_pred(ctx->msg, pred, sizeof(pred)) != SUCCESS)
  {
    cmd_reply(ctx, "failed to prepare the query");
    return;
  }

  // DELETE, like `in cancel`: a cancelled watch is not late work, it is
  // work that never happens, and a tombstone would only confuse the
  // pending count.
  snprintf(sql, sizeof(sql),
      "DELETE FROM chat_pricewatch WHERE id = %" PRId64
      " AND ns_id = %u AND fired_at IS NULL AND %s RETURNING id",
      id, ns->id, pred);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    db_result_free(res);
    cmd_reply(ctx, "could not cancel it");
    return;
  }

  if(res->rows == 0)
    cmd_reply(ctx, "no armed watch with that id that's yours to cancel");

  else
  {
    snprintf(ack, sizeof(ack), "cancelled %" PRId64 ".", id);
    cmd_reply(ctx, ack);
  }

  db_result_free(res);
}

// ---------- the insert ----------

bool
chatbot_pricewatch_insert(uint32_t ns_id, int64_t dossier,
    const method_msg_t *msg, const char *bot_name, const char *method_name,
    const char *pair, int dir, double threshold)
{
  // Escaped in one array so the all-or-nothing guard and the release
  // are each one loop; E_COUNT keeps the two from drifting apart.
  enum
  {
    E_SENDER = 0, E_NICK, E_USER, E_HOST, E_VID, E_META, E_BOT, E_METH,
    E_CHAN, E_COUNT,
  };
  char *e[E_COUNT];
  bool  ok    = FAIL;
  bool  whole = true;

  e[E_SENDER] = db_escape(msg->sender);
  e[E_NICK]   = db_escape(msg->nickname);
  e[E_USER]   = db_escape(msg->username);
  e[E_HOST]   = db_escape(msg->hostname);
  e[E_VID]    = db_escape(msg->verified_id);
  e[E_META]   = db_escape(msg->metadata);
  e[E_BOT]    = db_escape(bot_name);
  e[E_METH]   = db_escape(method_name);
  e[E_CHAN]   = db_escape(msg->channel);

  for(size_t i = 0; i < E_COUNT; i++)
    if(e[i] == NULL)
      whole = false;

  if(whole)
  {
    char dossier_cell[32];
    char sql[2048];

    if(dossier > 0)
      snprintf(dossier_cell, sizeof(dossier_cell), "%" PRId64, dossier);

    else
      snprintf(dossier_cell, sizeof(dossier_cell), "NULL");

    // %.12g rather than a fixed precision: NUMERIC accepts the
    // exponent form, and a threshold on a sub-cent coin needs it.
    snprintf(sql, sizeof(sql),
        "INSERT INTO chat_pricewatch"
        " (ns_id, dossier_id, bot_name, sender, nickname, username,"
        "  hostname, verified_id, metadata, method_name, channel, pair,"
        "  dir, threshold)"
        " VALUES (%" PRIu32 ", %s, '%s', '%s', '%s', '%s', '%s', '%s',"
        " '%s', '%s', '%s', '%s', %d, %.12g)",
        ns_id, dossier_cell, e[E_BOT], e[E_SENDER], e[E_NICK], e[E_USER],
        e[E_HOST], e[E_VID], e[E_META], e[E_METH], e[E_CHAN], pair, dir,
        threshold);

    ok = db_exec(sql, PRICEWATCH_CTX);
  }

  for(size_t i = 0; i < E_COUNT; i++)
    if(e[i] != NULL)
      mem_free(e[i]);

  return(ok);
}

// ---------- the sweep ----------

// Claim one crossing and hand its report to the deferred spine. The
// claim is the guard (the soul's idiom): two sweeps racing over one
// crossing cannot both report it, and a daemon that dies between the
// claim and the insert loses one report rather than repeating it
// forever — which is the direction a one-shot watch should fail in.
static bool
pricewatch_report(const pricewatch_sweep_t *sweep, const pricewatch_row_t *row,
    time_t now)
{
  db_result_t  *res;
  method_msg_t  msg;
  struct tm     tm;
  char          sql[256];
  char          seen[16];
  char          price[48];
  char          limit[48];
  char          body[CMD_ARG_SZ];
  bool          won;

  snprintf(sql, sizeof(sql),
      "UPDATE chat_pricewatch SET fired_at = NOW() WHERE id = %" PRId64
      " AND fired_at IS NULL RETURNING id", row->id);

  res = db_result_alloc();

  won = (db_query(sql, res) == SUCCESS && res->ok && res->rows > 0);
  db_result_free(res);

  if(!won)
    return(false);

  memset(&msg, 0, sizeof(msg));
  msg.timestamp = now;

  snprintf(msg.sender,      sizeof(msg.sender),      "%s", row->sender);
  chat_identity_apply(&row->who, &msg);
  snprintf(msg.metadata,    sizeof(msg.metadata),    "%s", row->metadata);
  snprintf(msg.channel,     sizeof(msg.channel),     "%s", row->channel);

  localtime_r(&now, &tm);
  snprintf(seen, sizeof(seen), "%02d:%02d", tm.tm_hour, tm.tm_min);

  pricewatch_fmt_price(row->observed, price, sizeof(price));
  pricewatch_fmt_price(row->threshold, limit, sizeof(limit));

  // The observation time is in the body on purpose: quiet hours can
  // hold this report until morning, and "at 23:04" stays true where
  // "just now" would have quietly become a lie.
  snprintf(body, sizeof(body),
      "at %s, %s traded at %s — %s the %s they asked you to watch for."
      " That watch was a one-shot and is spent now", seen, row->pair,
      price, pricewatch_dir_word(row->dir), limit);

  if(chatbot_deferred_insert(sweep->ns_id, row->dossier_id, &msg,
      row->method_name, "pricewatch", DEFERRED_KIND_SAY, body, NULL, 0,
      false, PRICEWATCH_REPORT_TTL_SECS) != SUCCESS)
  {
    // The watch is spent and the report is not written, so this
    // crossing is lost. Loud, because it is the one outcome here that
    // is nobody's design.
    clam(CLAM_WARN, PRICEWATCH_CTX,
        "bot=%s claimed watch %" PRId64 " but failed to store the report",
        sweep->bot_name, row->id);
    return(false);
  }

  clam(CLAM_INFO, PRICEWATCH_CTX,
      "bot=%s %s %s %s met at %s for %s in %s — report queued",
      sweep->bot_name, row->pair, pricewatch_dir_word(row->dir), limit,
      price, row->sender, row->channel[0] != '\0' ? row->channel : "DM");

  return(true);
}

// Everything that touches the DB, on a worker. The snapshot has already
// been reduced to the prices this bot cares about, so nothing here
// depends on the exchange plugin still being loaded.
static void
pricewatch_batch_task(task_t *t)
{
  pricewatch_sweep_t *sweep = t->data;
  time_t              now   = time(NULL);
  uint32_t            fired = 0;
  uint32_t            known = 0;
  size_t              off   = 0;
  char                sql[8192];

  // One statement for the whole watchlist refresh: 64 UPDATEs every
  // five minutes would be a round-trip storm for a display column.
  for(uint32_t i = 0; i < sweep->n_quotes; i++)
  {
    const pricewatch_quote_t *q = &sweep->quotes[i];

    if(!q->seen || !isfinite(q->price))
      continue;

    off += (size_t)snprintf(sql + off, sizeof(sql) - off,
        "%s('%s'%s, %.12g%s)", known == 0 ? "" : ",", q->pair,
        known == 0 ? "::VARCHAR" : "", q->price,
        known == 0 ? "::NUMERIC" : "");

    known++;

    if(off >= sizeof(sql) / 2)
      break;
  }

  if(known > 0)
  {
    char *e_bot = db_escape(sweep->bot_name);

    if(e_bot != NULL)
    {
      char stmt[8192 + 256];

      snprintf(stmt, sizeof(stmt),
          "UPDATE chat_watchlist w SET last_price = v.price,"
          " last_seen = NOW() FROM (VALUES %s) AS v(pair, price)"
          " WHERE w.bot_name = '%s' AND w.pair = v.pair", sql, e_bot);
      mem_free(e_bot);
      (void)db_exec(stmt, PRICEWATCH_CTX);
    }
  }

  for(uint32_t i = 0; i < sweep->n_rows; i++)
  {
    const pricewatch_row_t *row = &sweep->rows[i];

    if(!pricewatch_crossed(row->observed, row->dir, row->threshold))
      continue;

    if(pricewatch_report(sweep, row, now))
      fired++;
  }

  clam(CLAM_DEBUG, PRICEWATCH_CTX,
      "bot=%s sweep: %u pair(s) priced, %u watch(es) armed, %u fired",
      sweep->bot_name, known, sweep->n_rows, fired);

  soul_chore_done(sweep->sched, sweep->chore);
  mem_free(sweep);
  t->state = TASK_ENDED;
}

// Curl-worker soil: reduce, hand off, get out (the exchange API's
// threading rule). `snaps` dies with this call, so everything this
// sweep will ever need is copied here.
static void
pricewatch_tickers_cb(bool success, const char *err,
    const exchange_ticker_snapshot_t *snaps, size_t n, void *user)
{
  pricewatch_sweep_t *sweep = user;

  if(!success || snaps == NULL)
  {
    clam(CLAM_WARN, PRICEWATCH_CTX, "bot=%s ticker snapshot failed: %s",
        sweep->bot_name, err != NULL && err[0] != '\0' ? err : "(no error)");
    soul_chore_done(sweep->sched, sweep->chore);
    mem_free(sweep);
    return;
  }

  for(size_t i = 0; i < n; i++)
  {
    const exchange_ticker_snapshot_t *s = &snaps[i];

    for(uint32_t q = 0; q < sweep->n_quotes; q++)
    {
      if(strcmp(sweep->quotes[q].pair, s->product_id) != 0)
        continue;

      sweep->quotes[q].seen  = true;
      sweep->quotes[q].price = s->price;
      break;
    }
  }

  for(uint32_t r = 0; r < sweep->n_rows; r++)
    for(uint32_t q = 0; q < sweep->n_quotes; q++)
      if(strcmp(sweep->rows[r].pair, sweep->quotes[q].pair) == 0)
      {
        sweep->rows[r].observed = sweep->quotes[q].seen
            ? sweep->quotes[q].price : NAN;
        break;
      }

  if(task_add(PRICEWATCH_CTX, TASK_THREAD, 200, pricewatch_batch_task,
      sweep) != NULL)
    return;

  // No worker will ever run the batch. Nothing was claimed yet, so the
  // next sweep simply re-reads the same rows and reports late.
  clam(CLAM_WARN, PRICEWATCH_CTX, "bot=%s batch spawn failed — dropped",
      sweep->bot_name);
  soul_chore_done(sweep->sched, sweep->chore);
  mem_free(sweep);
}

// The exchange is a feature plugin and this is the bot layer, so the
// call is downward and legal (§D3) — but it is resolved tolerantly
// rather than through exchange_api.h's shims, which are FATAL on a
// miss. A bot whose exchange plugin is unloaded should idle, not take
// the daemon with it.
static bool
pricewatch_resolve(void)
{
  union { void *obj; pricewatch_tickers_fn_t fn; } u;

  u.obj = plugin_dlsym_cached("exchange", "exchange_fetch_all_tickers_async",
      (void **)&pricewatch_tickers_fn);
  pricewatch_tickers_fn = u.fn;

  return(pricewatch_tickers_fn != NULL);
}

// Fill the sweep's watchlist half. Returns how many pairs it holds; a
// pair that does not survive canonicalization is skipped rather than
// interpolated, since the sweep's SQL trusts these strings.
static uint32_t
pricewatch_load_pairs(pricewatch_sweep_t *sweep)
{
  db_result_t *res;
  char        *e_bot;
  char         sql[512];

  e_bot = db_escape(sweep->bot_name);

  if(e_bot == NULL)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT pair FROM chat_watchlist WHERE bot_name = '%s'"
      " ORDER BY pair ASC LIMIT %d", e_bot, PRICEWATCH_PAIRS_MAX);
  mem_free(e_bot);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
    for(uint32_t i = 0; i < res->rows && sweep->n_quotes < PRICEWATCH_PAIRS_MAX;
        i++)
    {
      pricewatch_quote_t *q = &sweep->quotes[sweep->n_quotes];
      char                raw[64];
      char                canon[EXCHANGE_PRODUCT_ID_SZ];

      db_result_copy(raw, sizeof(raw), res, i, 0);

      if(!pricewatch_canon_pair(raw, canon, sizeof(canon))
          || strcmp(raw, canon) != 0)
      {
        clam(CLAM_WARN, PRICEWATCH_CTX,
            "bot=%s watchlist row '%s' is not a product id — ignored",
            sweep->bot_name, raw);
        continue;
      }

      snprintf(q->pair, sizeof(q->pair), "%s", canon);
      q->price = NAN;
      q->seen  = false;
      sweep->n_quotes++;
    }

  db_result_free(res);
  return(sweep->n_quotes);
}

static void
pricewatch_load_rows(pricewatch_sweep_t *sweep)
{
  db_result_t *res;
  char        *e_bot;
  char         sql[1024];

  e_bot = db_escape(sweep->bot_name);

  if(e_bot == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT id, COALESCE(dossier_id, 0), pair, dir, threshold,"
      " method_name, channel, sender, nickname, username, hostname,"
      " verified_id, metadata FROM chat_pricewatch"
      " WHERE ns_id = %" PRIu32 " AND bot_name = '%s' AND fired_at IS NULL"
      " ORDER BY id ASC LIMIT %d", sweep->ns_id, e_bot, PRICEWATCH_ROWS_MAX);
  mem_free(e_bot);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
    for(uint32_t i = 0; i < res->rows && sweep->n_rows < PRICEWATCH_ROWS_MAX;
        i++)
    {
      pricewatch_row_t *row = &sweep->rows[sweep->n_rows];

      memset(row, 0, sizeof(*row));
      row->id         = db_result_get_i64(res, i, 0, 0);
      row->dossier_id = db_result_get_i64(res, i, 1, 0);
      row->dir        = (uint8_t)db_result_get_i64(res, i, 3,
                            PRICEWATCH_BELOW);
      row->threshold  = pricewatch_col_f64(res, i, 4);
      row->observed   = NAN;

      db_result_copy(row->pair,        sizeof(row->pair),        res, i, 2);
      db_result_copy(row->method_name, sizeof(row->method_name), res, i, 5);
      db_result_copy(row->channel,     sizeof(row->channel),     res, i, 6);
      db_result_copy(row->sender,      sizeof(row->sender),      res, i, 7);
      db_result_copy(row->who.nickname,    sizeof(row->who.nickname),    res, i, 8);
      db_result_copy(row->who.username,    sizeof(row->who.username),    res, i, 9);
      db_result_copy(row->who.hostname,    sizeof(row->who.hostname),    res, i, 10);
      db_result_copy(row->who.verified_id, sizeof(row->who.verified_id), res, i, 11);
      db_result_copy(row->metadata,    sizeof(row->metadata),    res, i, 12);

      if(isfinite(row->threshold))
        sweep->n_rows++;
    }

  db_result_free(res);
}

bool
chatbot_pricewatch_run(soul_sched_t *sched, uint32_t chore,
    const char *bot_name, uint32_t ns_id)
{
  pricewatch_sweep_t *sweep;

  if(!pricewatch_enabled(bot_name))
    return(false);

  if(!pricewatch_resolve())
  {
    clam(CLAM_DEBUG, PRICEWATCH_CTX,
        "bot=%s price watch idle (exchange plugin not loaded)", bot_name);
    return(false);
  }

  sweep = mem_alloc("chat", "pricewatch_sweep", sizeof(*sweep));

  memset(sweep, 0, sizeof(*sweep));
  sweep->sched = sched;
  sweep->chore = chore;
  sweep->ns_id = ns_id;
  snprintf(sweep->bot_name, sizeof(sweep->bot_name), "%s", bot_name);
  pricewatch_exchange_for(bot_name, sweep->exchange, sizeof(sweep->exchange));

  // An empty watchlist is the whole answer: nothing can be armed
  // against a pair the bot does not watch, so there is nothing to
  // price and no call to make.
  if(pricewatch_load_pairs(sweep) == 0)
  {
    clam(CLAM_DEBUG, PRICEWATCH_CTX, "bot=%s price watch idle (empty"
        " watchlist)", bot_name);
    mem_free(sweep);
    return(false);
  }

  pricewatch_load_rows(sweep);

  // The snapshot runs even with nothing armed: it is what keeps the
  // watchlist's prices — and the answer to "does that pair exist?" —
  // honest between watches.
  //
  // Anything but ASYNC_AIRBORNE means the sweep is no longer ours —
  // pricewatch_tickers_cb has logged the error, released the latch and
  // freed it before this returns. Freeing here as well was a real
  // double-free that killed the daemon on the first unregistered
  // exchange name; the return type is what says so now.
  //
  // false, not true: the callback has already released the latch, so
  // asking the tick to release it again is a harmless repeated store,
  // where claiming to be airborne after a synchronous failure would
  // wedge the chore for good.
  if(pricewatch_tickers_fn(sweep->exchange, pricewatch_tickers_cb, sweep)
      != ASYNC_AIRBORNE)
    return(false);

  // Airborne: the in-flight flag now belongs to the completion path.
  return(true);
}

// ---------- registration ----------

static const cmd_nl_slot_t pricewatch_slots[] = {
  { .name  = "pair",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "direction",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "price",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t pricewatch_examples[] = {
  { .utterance  = "tell me if bitcoin drops below 90k",
    .invocation = "/pricewatch BTC-USD below 90000" },
  { .utterance  = "let me know when ETH gets over 4000",
    .invocation = "/pricewatch ETH-USD above 4000" },
  { .utterance  = "ping me if doge goes above 50 cents",
    .invocation = "/pricewatch DOGE-USD above 0.50" },
};

static const cmd_nl_t pricewatch_nl = {
  // ⚠⚠ `.when` is the ROUTING field — it decides this command against
  // /crypto (which reports a price NOW) and against /remind (which
  // waits out a duration). Say "a price is reached", never "later" or
  // "when": both neighbours are about time, and this one is not.
  .when          = "Someone asks to be told if a coin or trading pair"
                   " reaches a price — a level, not a time.",
  .syntax        = "/pricewatch <PAIR> <above|below> <price>"
                   " — one-shot; the pair must be on the bot's watchlist",
  .slots         = pricewatch_slots,
  .slot_count    = (uint8_t)(sizeof(pricewatch_slots)
                             / sizeof(pricewatch_slots[0])),
  .examples      = pricewatch_examples,
  .example_count = (uint8_t)(sizeof(pricewatch_examples)
                             / sizeof(pricewatch_examples[0])),
  .dispatch_text = NULL,
};

bool
chatbot_pricewatch_register(void)
{
  if(cmd_register("chat", "pricewatch",
        "pricewatch <PAIR> <above|below> <price>",
        "Be told once when a pair crosses a price",
        "Watches a trading pair and tells you, in the bot's own voice,\n"
        "the first time it trades above (or below) the price you name:\n"
        "'pricewatch BTC-USD below 90000'. Set it in a channel and the\n"
        "answer lands there; set it in a DM and it comes back as a DM.\n"
        "\n"
        "It fires ONCE — ask again to re-arm. Prices read like 90000,\n"
        "90k or $1.25, and a bare symbol means the dollar pair, so BTC\n"
        "is BTC-USD. Only pairs on this bot's watchlist can be watched;\n"
        "'show bot <name> watchlist' is the list, and an admin curates\n"
        "it with 'bot <name> watchlist add <PAIR>'.\n"
        "\n"
        "Public market data only — the bot reads prices and never\n"
        "trades. 'pricewatch list' shows yours (spent ones for a day\n"
        "after); 'pricewatch cancel <id>' drops one.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_pricewatch, NULL, NULL, NULL,
        ad_pricewatch,
        (uint8_t)(sizeof(ad_pricewatch) / sizeof(ad_pricewatch[0])),
        NULL, &pricewatch_nl) != SUCCESS)
    return(FAIL);

  if(cmd_register("chat", "list",
        "pricewatch list",
        "List your price watches, armed and recently spent",
        "Everything you have watching, with the id 'pricewatch cancel'\n"
        "takes. A watch that has already fired stays listed as (spent)\n"
        "for a day so you can see that it did.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_pricewatch_list, NULL, "pricewatch", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    goto fail_list;

  if(cmd_register("chat", "cancel",
        "pricewatch cancel <id>",
        "Cancel one armed price watch by id",
        "Drops an armed watch. Yours to cancel means you set it; an\n"
        "admin may cancel any watch in the namespace. A spent watch has\n"
        "nothing left to cancel.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_pricewatch_cancel, NULL, "pricewatch", NULL,
        ad_pricewatch_cancel,
        (uint8_t)(sizeof(ad_pricewatch_cancel)
                  / sizeof(ad_pricewatch_cancel[0])),
        NULL, NULL) != SUCCESS)
    goto fail_cancel;

  if(cmd_register("chat", "watchlist",
        "bot <name> watchlist <add|del> <PAIR>",
        "Curate what a bot is willing to watch prices on",
        "Adds or removes an exchange product id (BTC-USD; a bare symbol\n"
        "means the dollar pair) from this bot's watchlist. Users may set\n"
        "price watches only on listed pairs, which is the whole scope\n"
        "fence: public market data, no trading.\n"
        "\n"
        "A pair is not checked against the exchange here — the next\n"
        "sweep prices it, and 'show bot <name> watchlist' shows what it\n"
        "found, so a pair that never gets a price is one the exchange\n"
        "does not publish. Removing a pair also cancels the armed\n"
        "watches on it, and says how many.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_bot_watchlist, NULL, "bot", NULL,
        ad_bot_watchlist,
        (uint8_t)(sizeof(ad_bot_watchlist) / sizeof(ad_bot_watchlist[0])),
        NULL, NULL) != SUCCESS)
    goto fail_watchlist;

  if(cmd_register("chat", "watchlist",
        "show bot <name> watchlist",
        "What a bot watches prices on, and what they last cost",
        "One line per pair: the last price the sweep saw, how long ago\n"
        "it saw it, and how many watches are armed against it. A pair\n"
        "with no price has not been priced yet — either the sweep has\n"
        "not run or the exchange does not publish that pair.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_show_bot_watchlist, NULL, "show/bot", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    goto fail_show;

  return(SUCCESS);

  // Registration failure is FATAL to the load and the daemon does not
  // come up with a half-registered surface; unwind what we can name.
fail_show:
  cmd_unregister_path("bot/watchlist");
fail_watchlist:
  cmd_unregister_path("pricewatch/cancel");
fail_cancel:
  cmd_unregister_path("pricewatch/list");
fail_list:
  cmd_unregister_path("pricewatch");
  return(FAIL);
}
