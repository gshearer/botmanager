// botmanager — MIT
// Whenmoon downloader DDL bootstrap + market-registry resolver (S1).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "dl_schema.h"

#include "coinbase_api.h"
#include "db.h"

#include "candle_upsample.sql.h"   // xxd-embedded plpgsql function body

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cap on the stack-local product scratch buffer used during the
// create-path cache lookup. coinbase.h reports CB_MAX_PRODUCTS=500 but
// that macro is CB_INTERNAL-only; we allocate a matching scratch buffer
// on the heap at lookup time rather than hardcoding the cap outside the
// plugin. The caller frees before returning.
#define WM_DL_CB_CACHE_CAP   500

// ------------------------------------------------------------------ //
// Core metadata DDL (run once per daemon by wm_dl_init)              //
// ------------------------------------------------------------------ //

static const char *const wm_dl_ddl_core[] = {
  // Registry --------------------------------------------------------
  "CREATE TABLE IF NOT EXISTS wm_market ("
  " id              SERIAL       PRIMARY KEY,"
  " exchange        VARCHAR(32)  NOT NULL,"
  " base_asset      VARCHAR(16)  NOT NULL,"
  " quote_asset     VARCHAR(16)  NOT NULL,"
  " exchange_symbol VARCHAR(32)  NOT NULL,"
  " base_increment  NUMERIC(36,18),"
  " quote_increment NUMERIC(36,18),"
  " status          VARCHAR(16)  NOT NULL DEFAULT 'online',"
  " first_seen      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
  " UNIQUE (exchange, exchange_symbol)"
  ")",

  "CREATE INDEX IF NOT EXISTS idx_wm_market_triple"
  " ON wm_market(exchange, base_asset, quote_asset)",

  // Coverage --------------------------------------------------------
  "CREATE TABLE IF NOT EXISTS wm_trade_coverage ("
  " market_id      INT          NOT NULL REFERENCES wm_market(id),"
  " first_trade_id BIGINT       NOT NULL,"
  " last_trade_id  BIGINT       NOT NULL,"
  " first_ts       TIMESTAMPTZ  NOT NULL,"
  " last_ts        TIMESTAMPTZ  NOT NULL,"
  " source         VARCHAR(16)  NOT NULL,"
  " completed_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
  " PRIMARY KEY (market_id, first_trade_id)"
  ")",

  "CREATE TABLE IF NOT EXISTS wm_candle_coverage ("
  " market_id    INT          NOT NULL REFERENCES wm_market(id),"
  " granularity  INT          NOT NULL,"
  " range_start  TIMESTAMPTZ  NOT NULL,"
  " range_end    TIMESTAMPTZ  NOT NULL,"
  " completed_at TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
  " PRIMARY KEY (market_id, granularity, range_start)"
  ")",

  // Job state -------------------------------------------------------
  "CREATE TABLE IF NOT EXISTS wm_download_job ("
  " id            BIGSERIAL    PRIMARY KEY,"
  " bot_name      VARCHAR(64)  NOT NULL,"
  " market_id     INT          NOT NULL REFERENCES wm_market(id),"
  " kind          VARCHAR(16)  NOT NULL,"
  " granularity   INT,"
  " oldest_ts     TIMESTAMPTZ,"
  " newest_ts     TIMESTAMPTZ,"
  " state         VARCHAR(16)  NOT NULL,"
  " cursor_after  BIGINT,"
  " cursor_end_ts TIMESTAMPTZ,"
  " pages_fetched INT          NOT NULL DEFAULT 0,"
  " rows_written  BIGINT       NOT NULL DEFAULT 0,"
  " last_err      TEXT,"
  " created       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
  " updated       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
  " requested_by  VARCHAR(64)"
  ")",

  "CREATE INDEX IF NOT EXISTS idx_wm_job_state"
  " ON wm_download_job(state, created)",

  // Running-market assignments. A row exists iff the named bot has
  // started the market and not yet stopped it. Rows survive daemon
  // restarts (wm_market_restore re-subscribes + fires a catch-up
  // download at bot start). wm_market_remove(persist=true) deletes.
  "CREATE TABLE IF NOT EXISTS wm_bot_market ("
  " bot_name   VARCHAR(64)  NOT NULL,"
  " market_id  INT          NOT NULL REFERENCES wm_market(id),"
  " added_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),"
  " PRIMARY KEY (bot_name, market_id)"
  ")",

  "CREATE INDEX IF NOT EXISTS idx_wm_bot_market_bot"
  " ON wm_bot_market(bot_name)",
};

// ------------------------------------------------------------------ //
// Per-pair DDL templates                                             //
// ------------------------------------------------------------------ //

static const char wm_trade_table_ddl_fmt[] =
  "CREATE TABLE IF NOT EXISTS %s ("
  " trade_id  BIGINT         PRIMARY KEY,"
  " ts        TIMESTAMPTZ    NOT NULL,"
  " side      CHAR(1)        NOT NULL,"
  " price     NUMERIC(24,10) NOT NULL,"
  " size      NUMERIC(28,10) NOT NULL,"
  " source    SMALLINT       NOT NULL DEFAULT 0"
  ")";

static const char wm_trade_idx_ddl_fmt[] =
  "CREATE INDEX IF NOT EXISTS %s_ts_idx ON %s(ts)";

static const char wm_candle_table_ddl_fmt[] =
  "CREATE TABLE IF NOT EXISTS %s ("
  " ts     TIMESTAMPTZ       PRIMARY KEY,"
  " low    DOUBLE PRECISION  NOT NULL,"
  " high   DOUBLE PRECISION  NOT NULL,"
  " open   DOUBLE PRECISION  NOT NULL,"
  " close  DOUBLE PRECISION  NOT NULL,"
  " volume DOUBLE PRECISION  NOT NULL"
  ")";

// ------------------------------------------------------------------ //
// One-shot DDL guard                                                 //
// ------------------------------------------------------------------ //

// Per-daemon: the core DDL pass runs at most once across every bot
// start. CREATE TABLE IF NOT EXISTS is idempotent at the Postgres level,
// so the mutex is belt-and-braces — it avoids the N-bots-starting case
// where every bot races a DDL batch at boot. Races would succeed but
// double the log noise.
static bool             wm_dl_schema_ensured = false;
static pthread_mutex_t  wm_dl_schema_lock    = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------------ //
// Internal helpers                                                   //
// ------------------------------------------------------------------ //

static bool
wm_dl_run_ddl(const char *sql)
{
  db_result_t *res;
  bool         ok = SUCCESS;

  res = db_result_alloc();

  if(res == NULL)
    return(FAIL);

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, WM_DL_CTX, "ddl failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  db_result_free(res);
  return(ok);
}

// ------------------------------------------------------------------ //
// Candle upsample function installer (WM-S6)                         //
// ------------------------------------------------------------------ //

// The xxd-embedded byte array is not NUL-terminated. Copy it onto a
// heap scratch buffer, terminate, and hand the C string to db_query.
// CREATE OR REPLACE FUNCTION is idempotent; running on every daemon
// boot is cheap and survives old signatures being swapped in-place.
static bool
wm_dl_install_candle_upsample_fn(void)
{
  db_result_t *res = NULL;
  char        *sql = NULL;
  bool         ok  = FAIL;

  sql = mem_alloc(WM_DL_CTX, "upsample_ddl",
      (size_t)wm_dl_candle_upsample_ddl_len + 1);

  if(sql == NULL)
    return(FAIL);

  memcpy(sql, wm_dl_candle_upsample_ddl,
      (size_t)wm_dl_candle_upsample_ddl_len);
  sql[wm_dl_candle_upsample_ddl_len] = '\0';

  res = db_result_alloc();

  if(res == NULL)
  {
    mem_free(sql);
    return(FAIL);
  }

  if(db_query(sql, res) == SUCCESS && res->ok)
    ok = SUCCESS;

  else
    clam(CLAM_WARN, WM_DL_CTX,
        "candle_upsample function install failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");

  db_result_free(res);
  mem_free(sql);

  return(ok);
}

// ------------------------------------------------------------------ //
// Orphan reset — flip any wm_download_job rows this bot left in       //
// 'running' back to 'queued' so the restored scheduler picks them up. //
// Runs from wm_dl_init after the DDL pass.                            //
// ------------------------------------------------------------------ //

static bool
wm_dl_reset_orphan_jobs(const char *bot_name)
{
  db_result_t *res = NULL;
  char        *e_bot = NULL;
  char         sql[512];
  bool         ok = FAIL;
  int          n;

  if(bot_name == NULL || bot_name[0] == '\0')
    return(FAIL);

  e_bot = db_escape(bot_name);

  if(e_bot == NULL)
    goto out;

  n = snprintf(sql, sizeof(sql),
      "UPDATE wm_download_job"
      "   SET state = 'queued', last_err = NULL, updated = NOW()"
      " WHERE bot_name = '%s' AND state = 'running'",
      e_bot);

  if(n < 0 || (size_t)n >= sizeof(sql))
    goto out;

  res = db_result_alloc();

  if(res == NULL)
    goto out;

  if(db_query(sql, res) == SUCCESS && res->ok)
    ok = SUCCESS;

  else
    clam(CLAM_WARN, WM_DL_CTX,
        "orphan reset failed for bot %s: %s", bot_name,
        res->error[0] != '\0' ? res->error : "(no driver error)");

out:
  if(res   != NULL) db_result_free(res);
  if(e_bot != NULL) mem_free(e_bot);

  return(ok);
}

// ------------------------------------------------------------------ //
// Lifecycle                                                          //
// ------------------------------------------------------------------ //

bool
wm_dl_init(whenmoon_state_t *st)
{
  bool ok = SUCCESS;

  if(st == NULL)
    return(FAIL);

  pthread_mutex_lock(&wm_dl_schema_lock);

  if(wm_dl_schema_ensured)
  {
    pthread_mutex_unlock(&wm_dl_schema_lock);
    st->dl_ready = true;
    return(SUCCESS);
  }

  for(size_t i = 0;
      i < sizeof(wm_dl_ddl_core) / sizeof(wm_dl_ddl_core[0]);
      i++)
  {
    if(wm_dl_run_ddl(wm_dl_ddl_core[i]) != SUCCESS)
    {
      ok = FAIL;
      break;
    }
  }

  // WM-S6: install the candle_upsample plpgsql function after the
  // core DDL succeeds. Must run before the per-daemon guard flips so
  // that a bot starting on a schema whose function was manually
  // dropped reinstalls it on next boot.
  if(ok == SUCCESS && wm_dl_install_candle_upsample_fn() != SUCCESS)
    ok = FAIL;

  if(ok == SUCCESS)
    wm_dl_schema_ensured = true;

  pthread_mutex_unlock(&wm_dl_schema_lock);

  if(ok == SUCCESS)
  {
    // Per-bot crash-recovery: flip any rows this bot left in 'running'
    // back to 'queued' before the scheduler loads jobs. Idempotent —
    // every boot runs it even when no rows match.
    wm_dl_reset_orphan_jobs(st->bot_name);

    st->dl_ready = true;
    clam(CLAM_INFO, WM_DL_CTX,
        "bot %s: downloader schema ready", st->bot_name);
  }

  else
    clam(CLAM_WARN, WM_DL_CTX,
        "bot %s: downloader schema init failed", st->bot_name);

  return(ok);
}

// S1 stub. WM-S4 will tear down the per-bot job scheduler here.
void
wm_dl_destroy(whenmoon_state_t *st)
{
  if(st != NULL)
    st->dl_ready = false;
}

// ------------------------------------------------------------------ //
// Table naming                                                       //
// ------------------------------------------------------------------ //

bool
wm_trade_table_name(int32_t market_id, char *out, size_t cap)
{
  int n;

  if(out == NULL || cap == 0 || market_id < 0)
    return(FAIL);

  n = snprintf(out, cap, "wm_trades_%" PRId32, market_id);

  if(n < 0 || (size_t)n >= cap)
    return(FAIL);

  return(SUCCESS);
}

bool
wm_candle_table_name(int32_t market_id, int32_t gran_secs,
    char *out, size_t cap)
{
  int n;

  if(out == NULL || cap == 0 || market_id < 0 || gran_secs <= 0)
    return(FAIL);

  n = snprintf(out, cap, "wm_candles_%" PRId32 "_%" PRId32,
      market_id, gran_secs);

  if(n < 0 || (size_t)n >= cap)
    return(FAIL);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Per-pair table DDL (lazy)                                          //
// ------------------------------------------------------------------ //

bool
wm_trade_table_ensure(int32_t market_id)
{
  char  name[WM_DL_TABLE_SZ];
  char  sql[512];
  int   n;

  if(wm_trade_table_name(market_id, name, sizeof(name)) != SUCCESS)
    return(FAIL);

  n = snprintf(sql, sizeof(sql), wm_trade_table_ddl_fmt, name);

  if(n < 0 || (size_t)n >= sizeof(sql))
    return(FAIL);

  if(wm_dl_run_ddl(sql) != SUCCESS)
    return(FAIL);

  // Secondary index on ts — the primary key covers trade_id lookups;
  // range scans (coverage stitching, newest-first pagination) want ts.
  n = snprintf(sql, sizeof(sql), wm_trade_idx_ddl_fmt, name, name);

  if(n < 0 || (size_t)n >= sizeof(sql))
    return(FAIL);

  return(wm_dl_run_ddl(sql));
}

bool
wm_candle_table_ensure(int32_t market_id, int32_t gran_secs)
{
  char  name[WM_DL_TABLE_SZ];
  char  sql[512];
  int   n;

  if(wm_candle_table_name(market_id, gran_secs, name, sizeof(name))
     != SUCCESS)
    return(FAIL);

  n = snprintf(sql, sizeof(sql), wm_candle_table_ddl_fmt, name);

  if(n < 0 || (size_t)n >= sizeof(sql))
    return(FAIL);

  return(wm_dl_run_ddl(sql));
}

// ------------------------------------------------------------------ //
// Market registry                                                    //
// ------------------------------------------------------------------ //

// Resolve (exchange, base, quote, exchange_symbol) to a wm_market.id.
// On miss, INSERT ... RETURNING id, filling increments from the
// coinbase product cache when available. Never composes SQL with raw
// user-supplied tokens — every string goes through db_escape first.
int32_t
wm_market_lookup_or_create(const char *exchange, const char *base_asset,
    const char *quote_asset, const char *exchange_symbol)
{
  db_result_t         *res       = NULL;
  char                *e_exchange = NULL;
  char                *e_base     = NULL;
  char                *e_quote    = NULL;
  char                *e_symbol   = NULL;
  coinbase_product_t  *cache      = NULL;
  char                 sql[1024];
  int32_t              id        = -1;
  double               base_inc  = 0.0;
  double               quote_inc = 0.0;
  bool                 have_increments = false;

  if(exchange == NULL || base_asset == NULL ||
     quote_asset == NULL || exchange_symbol == NULL)
    return(-1);

  e_exchange = db_escape(exchange);
  e_base     = db_escape(base_asset);
  e_quote    = db_escape(quote_asset);
  e_symbol   = db_escape(exchange_symbol);

  if(e_exchange == NULL || e_base == NULL ||
     e_quote == NULL || e_symbol == NULL)
    goto out;

  // Lookup ----------------------------------------------------------
  snprintf(sql, sizeof(sql),
      "SELECT id FROM wm_market"
      " WHERE exchange = '%s' AND exchange_symbol = '%s'",
      e_exchange, e_symbol);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok &&
     res->rows == 1)
  {
    const char *s = db_result_get(res, 0, 0);

    if(s != NULL)
      id = (int32_t)strtol(s, NULL, 10);
  }

  db_result_free(res);
  res = NULL;

  if(id >= 0)
    goto out;

  // Create path -----------------------------------------------------
  // Pull increments from the coinbase product cache when available.
  // Other exchanges (or an empty/stale cache) fall through to the
  // NULL-increments INSERT; a later cache refresh plus a follow-up
  // refresh command (not in S1) can backfill.
  if(strcmp(exchange, "coinbase") == 0 &&
     coinbase_products_cache_fresh())
  {
    uint32_t n = 0;

    cache = mem_alloc("whenmoon", "dl_cb_cache",
        sizeof(*cache) * WM_DL_CB_CACHE_CAP);

    if(cache != NULL &&
       coinbase_get_products(cache, WM_DL_CB_CACHE_CAP, &n) == SUCCESS)
    {
      for(uint32_t i = 0; i < n; i++)
      {
        if(strncmp(cache[i].product_id, exchange_symbol,
               COINBASE_PRODUCT_ID_SZ) == 0)
        {
          base_inc        = cache[i].base_increment;
          quote_inc       = cache[i].quote_increment;
          have_increments = true;
          break;
        }
      }
    }
  }

  if(have_increments)
    snprintf(sql, sizeof(sql),
        "INSERT INTO wm_market"
        " (exchange, base_asset, quote_asset, exchange_symbol,"
        "  base_increment, quote_increment)"
        " VALUES ('%s', '%s', '%s', '%s', %.18g, %.18g)"
        " RETURNING id",
        e_exchange, e_base, e_quote, e_symbol, base_inc, quote_inc);

  else
    snprintf(sql, sizeof(sql),
        "INSERT INTO wm_market"
        " (exchange, base_asset, quote_asset, exchange_symbol)"
        " VALUES ('%s', '%s', '%s', '%s')"
        " RETURNING id",
        e_exchange, e_base, e_quote, e_symbol);

  res = db_result_alloc();

  if(res != NULL && db_query(sql, res) == SUCCESS && res->ok &&
     res->rows == 1)
  {
    const char *s = db_result_get(res, 0, 0);

    if(s != NULL)
      id = (int32_t)strtol(s, NULL, 10);
  }

  else
    clam(CLAM_WARN, WM_DL_CTX,
        "market create failed for %s:%s:%s (%s): %s",
        exchange, base_asset, quote_asset, exchange_symbol,
        (res != NULL && res->error[0] != '\0')
            ? res->error : "(no driver error)");

  db_result_free(res);
  res = NULL;

out:
  if(cache      != NULL) mem_free(cache);
  if(e_exchange != NULL) mem_free(e_exchange);
  if(e_base     != NULL) mem_free(e_base);
  if(e_quote    != NULL) mem_free(e_quote);
  if(e_symbol   != NULL) mem_free(e_symbol);

  return(id);
}
