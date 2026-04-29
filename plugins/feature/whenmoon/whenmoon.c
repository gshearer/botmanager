// botmanager — MIT
// whenmoon trading plugin (PLUGIN_FEATURE, kind: whenmoon).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "backtest.h"
#include "market.h"
#include "account.h"
#include "dl_schema.h"
#include "dl_jobtable.h"
#include "dl_commands.h"
#include "market_cmds.h"
#include "order.h"
#include "strategy.h"
#include "sweep.h"
#include "trade_persist.h"

#include "cmd.h"
#include "colors.h"
#include "kv.h"
#include "userns.h"

#include <ta-lib/ta_libc.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------------ //
// Plugin-global singleton                                             //
// ------------------------------------------------------------------ //

static whenmoon_state_t *whenmoon_state = NULL;

whenmoon_state_t *
whenmoon_get_state(void)
{
  return(whenmoon_state);
}

// ------------------------------------------------------------------ //
// Plugin-global KV schema                                             //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t whenmoon_plugin_schema[] = {
  { "plugin.whenmoon.exchange.coinbase.account.refresh_sec",
    KV_UINT32, "30",
    "Seconds between coinbase /accounts polls. Minimum effective"
    " value is 5. Ignored when no coinbase apikey is configured.",
    NULL, NULL },

  { "plugin.whenmoon.downloader.max_concurrent_jobs", KV_UINT32, "4",
    "Maximum number of jobs in 'running' state at once (1..32). The"
    " feature_exchange token bucket caps effective throughput; higher"
    " values mean more interleaving of pairs rather than more"
    " throughput. EX-1: rate-limit knobs now live under"
    " plugin.exchange.<name>.rate_limit_rps.",
    NULL, NULL },
};

// ------------------------------------------------------------------ //
// /show whenmoon markets                                              //
// ------------------------------------------------------------------ //

static void
whenmoon_show_markets_cmd(const cmd_ctx_t *ctx)
{
  whenmoon_state_t   *st;
  whenmoon_markets_t *m;
  char                line[256];
  uint32_t            i;

  st = whenmoon_get_state();

  if(st == NULL || st->markets == NULL)
  {
    cmd_reply(ctx, "whenmoon: no market state");
    return;
  }

  m = st->markets;

  if(m->n_markets == 0)
  {
    cmd_reply(ctx,
        "whenmoon: no markets configured"
        " (use /whenmoon market start <exch>-<base>-<quote>)");
    return;
  }

  cmd_reply(ctx, CLR_BOLD "whenmoon markets" CLR_RESET);

  for(i = 0; i < m->n_markets; i++)
  {
    whenmoon_market_t *mk = &m->arr[i];
    double   px;
    int64_t  tick_ms;
    uint32_t g1m, g5m, g15m, g1h, g6h, g1d;
    uint32_t cap1m;

    pthread_mutex_lock(&mk->lock);
    px       = mk->last_px;
    tick_ms  = mk->last_tick_ms;
    g1m      = mk->grain_n[WM_GRAN_1M];
    g5m      = mk->grain_n[WM_GRAN_5M];
    g15m     = mk->grain_n[WM_GRAN_15M];
    g1h      = mk->grain_n[WM_GRAN_1H];
    g6h      = mk->grain_n[WM_GRAN_6H];
    g1d      = mk->grain_n[WM_GRAN_1D];
    cap1m    = mk->grain_cap[WM_GRAN_1M];
    pthread_mutex_unlock(&mk->lock);

    snprintf(line, sizeof(line),
        "  %-24s  last_px=%-14.8g  bars 1m=%u/%u 5m=%u 15m=%u"
        " 1h=%u 6h=%u 1d=%u  last_tick_ms=%" PRId64,
        mk->market_id_str, px, g1m, cap1m, g5m, g15m, g1h, g6h, g1d,
        tick_ms);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// /show whenmoon balances                                             //
// ------------------------------------------------------------------ //

static void
whenmoon_show_balances_cmd(const cmd_ctx_t *ctx)
{
  whenmoon_state_t    *st;
  whenmoon_account_t  *acc;
  coinbase_account_t   rows[WM_ACCOUNT_ROW_CAP];
  uint32_t             n;
  time_t               ts;
  char                 err[128];
  char                 line[256];
  uint32_t             i;

  st = whenmoon_get_state();

  if(st == NULL || st->account == NULL)
  {
    cmd_reply(ctx, "whenmoon: no account state");
    return;
  }

  acc = st->account;

  pthread_mutex_lock(&acc->lock);
  n = acc->n_rows;

  if(n > WM_ACCOUNT_ROW_CAP)
    n = WM_ACCOUNT_ROW_CAP;

  memcpy(rows, acc->rows, sizeof(rows[0]) * n);
  ts = acc->last_refresh_ts;
  snprintf(err, sizeof(err), "%s", acc->last_err);
  pthread_mutex_unlock(&acc->lock);

  if(!coinbase_apikey_configured())
  {
    cmd_reply(ctx,
        "whenmoon: account refresh disabled — no coinbase apikey");
    return;
  }

  cmd_reply(ctx, CLR_BOLD "whenmoon balances" CLR_RESET);

  if(err[0] != '\0')
  {
    snprintf(line, sizeof(line),
        "  " CLR_YELLOW "last_err:" CLR_RESET " %s", err);
    cmd_reply(ctx, line);
  }

  if(n == 0)
  {
    cmd_reply(ctx, "  (no rows yet)");
    return;
  }

  snprintf(line, sizeof(line),
      "  refreshed: %ld", (long)ts);
  cmd_reply(ctx, line);

  for(i = 0; i < n; i++)
  {
    char ccy[COINBASE_CURRENCY_SZ];
    size_t clen = strnlen(rows[i].currency, sizeof(ccy) - 1);

    memcpy(ccy, rows[i].currency, clen);
    ccy[clen] = '\0';

    snprintf(line, sizeof(line),
        "  %-8s  balance=%-16.8g  hold=%-16.8g  available=%-16.8g",
        ccy, rows[i].balance, rows[i].hold, rows[i].available);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// /whenmoon and /show whenmoon parents                                //
// ------------------------------------------------------------------ //

static void
whenmoon_root_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon <market|download|strategy|trade|backtest> ..."
      " (market start|stop, download trades|candles|cancel,"
      " strategy attach|detach|reload, trade mode|reset,"
      " backtest run)");
}

static void
whenmoon_show_root_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /show whenmoon"
      " <markets|balances|indicators|download|strategy|trade|backtest>"
      " ... (download has subverbs: status, gaps, candles)");
}

// ------------------------------------------------------------------ //
// Verb registration                                                   //
// ------------------------------------------------------------------ //

static bool
whenmoon_register_root_verbs(void)
{
  // /whenmoon — state-changing parent.
  if(cmd_register("whenmoon", "whenmoon",
        "whenmoon <subcommand> ...",
        "Whenmoon market + downloader + strategy controls.",
        "Subcommands: market, download, strategy.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_root_cb, NULL, NULL, "wm",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // /show whenmoon — observability parent. Parent path "show" already
  // exists (registered by core).
  if(cmd_register("whenmoon", "whenmoon",
        "show whenmoon <subcommand> ...",
        "Whenmoon read-only state.",
        "Subcommands: markets, balances, indicators, download, strategy.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_show_root_cb, NULL, "show", "wm",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static bool
whenmoon_register_show_verbs(void)
{
  if(cmd_register("whenmoon", "markets",
        "show whenmoon markets",
        "Per-market state: id, last ticker px, candle count, last tick ts",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_show_markets_cmd, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "balances",
        "show whenmoon balances",
        "Coinbase /accounts snapshot (per-currency balance/hold/available)",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_show_balances_cmd, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Plugin lifecycle                                                    //
// ------------------------------------------------------------------ //

static void
whenmoon_subsystems_destroy(whenmoon_state_t *st)
{
  if(st == NULL)
    return;

  // Mirror the order from the previous per-bot teardown: strategies
  // first (they may hold dispatch references into market state), then
  // job table, downloader DDL flag, markets, account. The trade engine
  // is plugin-global and outlives subsystems by a hair so any final
  // strategy-detach paths can drop their books cleanly.
  wm_strategy_registry_destroy(st);
  wm_trade_engine_destroy();
  wm_dl_jobtable_destroy(st);
  wm_dl_destroy(st);
  wm_market_destroy(st);
  wm_account_destroy(st);
}

static bool
whenmoon_init(void)
{
  whenmoon_state_t *st;
  TA_RetCode        rc;

  rc = TA_Initialize();

  if(rc != TA_SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "TA_Initialize failed: rc=%d", rc);
    return(FAIL);
  }

  if(wm_trade_persist_global_init() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "trade-persist global init failed");
    TA_Shutdown();
    return(FAIL);
  }

  st = mem_alloc("whenmoon", "state", sizeof(*st));

  if(st == NULL)
  {
    wm_trade_persist_global_destroy();
    TA_Shutdown();
    return(FAIL);
  }

  memset(st, 0, sizeof(*st));
  whenmoon_state = st;

  // Order: markets container first (no DB or KV reads), then account,
  // then downloader DDL + scheduler. wm_market_restore runs last so it
  // can resubscribe over a complete WS handler set.
  if(wm_market_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_market_init failed");
    goto fail;
  }

  if(wm_account_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_account_init failed");
    goto fail;
  }

  if(wm_dl_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_dl_init failed");
    goto fail;
  }

  if(wm_dl_jobtable_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_dl_jobtable_init failed");
    goto fail;
  }

  if(wm_strategy_registry_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_strategy_registry_init failed");
    goto fail;
  }

  // WM-LT-4: trade engine. Plugin-global registry of (market, strategy)
  // trade books. Init order: after the strategy registry so the detach
  // hooks have somewhere to call into; before verb registration so the
  // /whenmoon trade … verbs find an initialized engine.
  if(wm_trade_engine_init() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_trade_engine_init failed");
    goto fail;
  }

  if(whenmoon_register_root_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "root verb registration failed");
    goto fail;
  }

  if(whenmoon_register_show_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "show-verb registration failed");
    goto fail;
  }

  if(wm_dl_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "downloader verb registration failed");
    goto fail;
  }

  if(wm_market_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "market verb registration failed");
    goto fail;
  }

  if(wm_strategy_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "strategy verb registration failed");
    goto fail;
  }

  if(wm_trade_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "trade verb registration failed");
    goto fail;
  }

  // WM-LT-5: backtest verbs. Registered after the trade engine + strategy
  // verbs so cmd_register's parent path ("whenmoon") and sibling
  // resolution see a steady state. The backtest runtime relies on the
  // strategy registry + trade engine + downloader DDL, all of which
  // are already initialised by this point.
  if(wm_backtest_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "backtest verb registration failed");
    goto fail;
  }

  // Discover strategies that the core loader has already brought up.
  // This is idempotent and safe even when zero strategy plugins are
  // present (the iteration finds no PLUGIN_STRATEGY records).
  wm_strategy_registry_scan(st);

  // WM-LT-6: drop any per-iteration KV slots left over from a prior
  // crashed sweep so kv_register on a freshly-allocated synthetic id
  // never trips on a stale row. Safe at boot — no other thread is
  // touching those keys yet.
  wm_bt_sweep_cleanup_stale_kv();

  // Restore last; logs but does not fail init if the DB query errors.
  if(wm_market_restore(st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "wm_market_restore failed (plugin starts with no markets)");

  clam(CLAM_INFO, WHENMOON_CTX, "whenmoon plugin initialized");
  return(SUCCESS);

fail:
  whenmoon_subsystems_destroy(st);
  whenmoon_state = NULL;
  mem_free(st);
  wm_trade_persist_global_destroy();
  TA_Shutdown();
  return(FAIL);
}

static void
whenmoon_deinit(void)
{
  whenmoon_state_t *st = whenmoon_state;

  if(st != NULL)
  {
    whenmoon_subsystems_destroy(st);
    whenmoon_state = NULL;
    mem_free(st);
  }

  wm_trade_persist_global_destroy();
  TA_Shutdown();
  clam(CLAM_INFO, WHENMOON_CTX, "whenmoon plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "whenmoon",
  .version              = "0.9-lt7",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "whenmoon",
  .provides             = { { .name = "feature_whenmoon" } },
  .provides_count       = 1,
  .requires             = {
    { .name = "feature_exchange" },
    { .name = "exchange_coinbase" },
  },
  .requires_count       = 2,
  .kv_schema            = whenmoon_plugin_schema,
  .kv_schema_count      =
      sizeof(whenmoon_plugin_schema) / sizeof(whenmoon_plugin_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = whenmoon_init,
  .start                = NULL,
  .stop                 = NULL,
  .deinit               = whenmoon_deinit,
  .ext                  = NULL,
};
