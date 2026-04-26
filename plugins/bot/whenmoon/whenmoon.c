// botmanager — MIT
// whenmoon trading bot plugin (kind: whenmoon).
//
// CB6: per-bot market + account state, read-only admin commands.
// No order / strategy surface yet — those land in later chunks.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "market.h"
#include "account.h"
#include "dl_schema.h"
#include "dl_scheduler.h"
#include "dl_commands.h"
#include "market_cmds.h"

#include "cmd.h"
#include "colors.h"
#include "kv.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------------ //
// KV schemas                                                          //
// ------------------------------------------------------------------ //

// Per-instance schema: keys registered as bot.<name>.whenmoon.<suffix>
// at bot-create time. See plugin.h for the kv_inst_schema contract.
//
// Running markets live in the `wm_bot_market` DB table (not a KV).
// Use `/bot <name> market start|stop` to change the live set.
static const plugin_kv_entry_t whenmoon_inst_schema[] = {
  { "whenmoon.account.refresh_sec", KV_UINT32, "30",
    "Seconds between coinbase /accounts polls for this bot. Minimum"
    " effective value is 5. Ignored when no coinbase apikey is"
    " configured.", NULL, NULL },

  { "whenmoon.downloader.enabled", KV_BOOL, "true",
    "Enable the trade/candle history downloader engine for this bot.",
    NULL, NULL },
  { "whenmoon.downloader.max_concurrent_jobs", KV_UINT32, "4",
    "Maximum number of jobs in 'running' state at once (1..32). Token"
    " bucket caps effective throughput regardless; higher values mean"
    " more interleaving of pairs rather than more throughput.",
    NULL, NULL },
  { "whenmoon.downloader.rate_limit_rps", KV_UINT32, "8",
    "Sustained requests/sec budget for this bot's downloader (1..10)."
    " Coinbase public cap is 10; default 8 leaves headroom for the"
    " bot's non-downloader traffic. Bucket depth is 1.5x this value"
    " to absorb bursts.",
    NULL, NULL },
};

// ------------------------------------------------------------------ //
// Bot driver callbacks                                                //
// ------------------------------------------------------------------ //

static void *
whenmoon_create(bot_inst_t *inst)
{
  whenmoon_state_t *st;
  const char *name;

  st = mem_alloc("whenmoon", "state", sizeof(*st));

  if(st == NULL)
    return(NULL);

  memset(st, 0, sizeof(*st));
  st->inst = inst;

  name = bot_inst_name(inst);

  if(name != NULL)
    snprintf(st->bot_name, sizeof(st->bot_name), "%s", name);

  // Market + account init is deferred to whenmoon_start_cb so that KVs
  // set by the same config batch land before we read them. `bot add`
  // fires create; the per-bot KVs often land after. Starting the
  // subsystems early would observe an empty markets list.
  return(st);
}

static void
whenmoon_destroy(void *handle)
{
  whenmoon_state_t *st = handle;

  if(st == NULL)
    return;

  wm_dl_scheduler_destroy(st);
  wm_dl_destroy(st);
  wm_market_destroy(st);
  wm_account_destroy(st);

  mem_free(st);
}

static bool
whenmoon_start_cb(void *handle)
{
  whenmoon_state_t *st = handle;

  if(st == NULL)
    return(FAIL);

  if(st->markets == NULL && wm_market_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "bot %s: wm_market_init failed",
        st->bot_name);
    return(FAIL);
  }

  if(st->account == NULL && wm_account_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "bot %s: wm_account_init failed",
        st->bot_name);
    return(FAIL);
  }

  if(!st->dl_ready && wm_dl_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "bot %s: wm_dl_init failed",
        st->bot_name);
    return(FAIL);
  }

  if(st->downloader == NULL && wm_dl_scheduler_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "bot %s: wm_dl_scheduler_init failed", st->bot_name);
    return(FAIL);
  }

  // Restore the running-market set from the DB. Each add() here fires
  // a WS subscribe and a live-ring backfill (300 rows of 1m candles
  // via REST). History coverage for trading is the strategy layer's
  // responsibility (WM-LT-3); operators can drive the
  // `/bot <name> download candles ...` verbs explicitly when a
  // deeper history is wanted. Restore never blocks start on DB
  // contents: a restore failure is logged but the bot still runs.
  if(wm_market_restore(st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "bot %s: wm_market_restore failed (bot starts with no markets)",
        st->bot_name);

  return(SUCCESS);
}

static void
whenmoon_stop_cb(void *handle)
{
  (void)handle;
}

static void
whenmoon_on_message(void *handle, const method_msg_t *msg)
{
  // Command dispatch / strategy routing lands in later chunks. CB6
  // keeps the bot passive: markets + account are maintained in the
  // background, but on-wire messages are not consumed yet.
  (void)handle;
  (void)msg;
}

// ------------------------------------------------------------------ //
// /show bot <name> markets                                            //
// ------------------------------------------------------------------ //

static void
whenmoon_show_markets_cmd(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  whenmoon_markets_t *m;
  char line[256];
  uint32_t i;

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

  m = st->markets;

  if(m->n_markets == 0)
  {
    cmd_reply(ctx,
        "whenmoon: no markets configured"
        " (use /bot <name> market start <exch>:<asset>:<cur>)");
    return;
  }

  cmd_reply(ctx, CLR_BOLD "whenmoon markets" CLR_RESET);

  for(i = 0; i < m->n_markets; i++)
  {
    whenmoon_market_t *mk = &m->arr[i];
    double   px;
    int64_t  tick_ms;
    uint32_t ncandles;

    pthread_mutex_lock(&mk->lock);
    px       = mk->last_px;
    tick_ms  = mk->last_tick_ms;
    ncandles = mk->n_candles;
    pthread_mutex_unlock(&mk->lock);

    snprintf(line, sizeof(line),
        "  %-12s  last_px=%-14.8g  candles=%-4u  last_tick_ms=%" PRId64,
        mk->product_id, px, ncandles, tick_ms);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// /show bot <name> balances                                           //
// ------------------------------------------------------------------ //

static void
whenmoon_show_balances_cmd(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  whenmoon_account_t *acc;
  coinbase_account_t rows[WM_ACCOUNT_ROW_CAP];
  uint32_t n;
  time_t   ts;
  char     err[128];
  char     line[256];
  uint32_t i;

  if(ctx->bot == NULL)
  {
    cmd_reply(ctx, "no bot context");
    return;
  }

  st = bot_get_handle(ctx->bot);

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
// Driver and descriptor                                               //
// ------------------------------------------------------------------ //

static const bot_driver_t whenmoon_driver = {
  .name       = "whenmoon",
  .create     = whenmoon_create,
  .destroy    = whenmoon_destroy,
  .start      = whenmoon_start_cb,
  .stop       = whenmoon_stop_cb,
  .on_message = whenmoon_on_message,
};

// ------------------------------------------------------------------ //
// Plugin lifecycle                                                    //
// ------------------------------------------------------------------ //

static const char *const whenmoon_kind_filter[] = { "whenmoon", NULL };

static bool
whenmoon_register_show_verbs(void)
{
  if(cmd_register("whenmoon", "markets",
        "show bot <name> markets",
        "Per-market state: product id, last ticker px, candle count,"
        " last tick ts",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_show_markets_cmd, NULL, "show/bot", NULL,
        NULL, 0, whenmoon_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "balances",
        "show bot <name> balances",
        "Coinbase /accounts snapshot (per-currency balance/hold/available)",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_show_balances_cmd, NULL, "show/bot", NULL,
        NULL, 0, whenmoon_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static bool
whenmoon_init(void)
{
  if(whenmoon_register_show_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "show-verb registration failed");
    return(FAIL);
  }

  if(wm_dl_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "downloader verb registration failed");
    return(FAIL);
  }

  if(wm_market_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "market verb registration failed");
    return(FAIL);
  }

  clam(CLAM_INFO, WHENMOON_CTX, "whenmoon plugin initialized");
  return(SUCCESS);
}

static void
whenmoon_deinit(void)
{
  clam(CLAM_INFO, WHENMOON_CTX, "whenmoon plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "whenmoon",
  .version              = "0.2-cb6",
  .type                 = PLUGIN_BOT,
  .kind                 = "whenmoon",
  .provides             = { { .name = "bot_whenmoon" } },
  .provides_count       = 1,
  .requires             = { { .name = "service_coinbase" } },
  .requires_count       = 1,
  .kv_schema            = NULL,
  .kv_schema_count      = 0,
  .kv_inst_schema       = whenmoon_inst_schema,
  .kv_inst_schema_count =
      sizeof(whenmoon_inst_schema) / sizeof(whenmoon_inst_schema[0]),
  .init                 = whenmoon_init,
  .start                = NULL,
  .stop                 = NULL,
  .deinit               = whenmoon_deinit,
  .ext                  = &whenmoon_driver,
};
