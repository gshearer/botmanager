// botmanager — MIT
// whenmoon trading plugin (PLUGIN_FEATURE, kind: whenmoon).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "account.h"
#include "backtest.h"
#include "market.h"
#include "market_engine.h"
#include "market_persist.h"
#include "dl_schema.h"
#include "dl_jobtable.h"
#include "dl_commands.h"
#include "market_cmds.h"
#include "order_cmds.h"
#include "live.h"
#include "mw.h"
#include "strategy.h"
#include "sweep.h"
#include "warmup.h"
#include "wm_exch_query.h"

#include "cmd.h"
#include "colors.h"
#include "kv.h"
#include "userns.h"
#include "exchange_api.h"
#include "alloc.h"

#include <ta-lib/ta_libc.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

int64_t
wm_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return((int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000));
}

// ------------------------------------------------------------------ //
// Plugin-global KV schema                                             //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t whenmoon_plugin_schema[] = {
  { "plugin.whenmoon.downloader.max_concurrent_jobs", KV_UINT32, "4",
    "Maximum number of jobs in 'running' state at once (1..32). The"
    " feature_exchange token bucket caps effective throughput; higher"
    " values mean more interleaving of pairs rather than more"
    " throughput. EX-1: rate-limit knobs now live under"
    " plugin.exchange.<name>.rate_limit_rps.",
    NULL, NULL },
};

// ------------------------------------------------------------------ //
// /show whenmoon balances [exchange] [fresh]                          //
// ------------------------------------------------------------------ //
//
// Cache-first authenticated balance view. By default this reads the
// per-exchange snapshot cache (account.c) and renders instantly with a
// vintage line — no network wait. A scheduled poll keeps the cache warm
// for real-mode exchanges and real fills fast-forward it; see account.h.
//
// Two paths still do a synchronous blocking /accounts fetch:
//   - `fresh` keyword: the operator forces a live refresh.
//   - cache miss: no snapshot yet (e.g. an all-paper deploy the poll
//     gate never warms). The fallback fetch populates the cache so the
//     next call is instant — this is what fixes the WM-BAL-ONDEMAND-1
//     paper-mode "empty cache shows nothing" regression.
//
// Why the fetch blocks: the control socket (core/botmanctl.c) routes
// command replies through a single global reply target that bctl_dispatch
// clears the instant synchronous dispatch returns — a reply emitted from
// an async curl callback is silently dropped. The fetch therefore waits
// on the dispatch thread (wm_sync_fetch bridge, wm_exch_query.h) and
// formats inline. An explicit operator fetch is fine for any mode.

#define WM_BAL_EXCH_CAP  8

static void
wm_bal_on_accounts(const exchange_accounts_result_t *res, void *user)
{
  if(res != NULL)
  {
    wm_sync_fetch_complete(user, res, sizeof(*res));
    return;
  }

  // Translate a NULL result into a typed error so the waiter's copy-out
  // carries a message rather than a zeroed "ok, 0 currencies".
  {
    exchange_accounts_result_t err;

    memset(&err, 0, sizeof(err));
    snprintf(err.err, sizeof(err.err), "no result delivered");
    wm_sync_fetch_complete(user, &err, sizeof(err));
  }
}

// Render one exchange's balance rows under an "ok" header carrying the
// snapshot vintage. `from_cache` distinguishes a cached read (shows the
// age) from a just-completed live fetch (shows "just fetched").
static void
wm_bal_render_rows(const cmd_ctx_t *ctx, const char *exchange_name,
    const exchange_account_t *rows, uint32_t count, int64_t age_ms,
    bool from_cache)
{
  char     header[256];
  char     line[256];
  char     agebuf[32];
  char     tag[64];
  uint32_t i;
  uint32_t shown = 0;

  if(from_cache)
    snprintf(tag, sizeof(tag), CLR_GRAY "updated %s ago" CLR_RESET,
        wm_fmt_age(age_ms, agebuf, sizeof(agebuf)));
  else
    snprintf(tag, sizeof(tag), CLR_GRAY "just fetched (live)" CLR_RESET);

  snprintf(header, sizeof(header),
      CLR_BOLD "  %s" CLR_RESET "  " CLR_GREEN "ok" CLR_RESET
      " (%u currenc%s)  %s",
      exchange_name, count, count == 1 ? "y" : "ies", tag);
  cmd_reply(ctx, header);

  for(i = 0; i < count; i++)
  {
    char   ccy[EXCHANGE_CURRENCY_SZ];
    char   bbuf[40], hbuf[40], abuf[40];
    size_t clen;

    // Hide fully-zero rows so funded balances stand out.
    if(rows[i].balance == 0.0 && rows[i].hold == 0.0)
      continue;

    clen = strnlen(rows[i].currency, sizeof(ccy) - 1);
    memcpy(ccy, rows[i].currency, clen);
    ccy[clen] = '\0';

    snprintf(line, sizeof(line),
        "    %-8s  balance=%-18s  hold=%-18s  available=%-18s",
        ccy,
        wm_fmt_amount(rows[i].balance,   bbuf, sizeof(bbuf)),
        wm_fmt_amount(rows[i].hold,      hbuf, sizeof(hbuf)),
        wm_fmt_amount(rows[i].available, abuf, sizeof(abuf)));
    cmd_reply(ctx, line);
    shown++;
  }

  if(shown == 0)
    cmd_reply(ctx, "    (all balances zero)");
}

// Render one exchange's balances. Cache-first unless `force` (operator
// `fresh`); a cache miss falls back to a blocking live fetch that then
// populates the cache. Doubles as a credential check: a clean header
// means the key authenticated.
static void
wm_bal_render(const cmd_ctx_t *ctx, const char *exchange_name, bool force)
{
  exchange_capabilities_t    caps;
  exchange_accounts_result_t res;
  char                       header[256];

  if(exchange_get_capabilities(exchange_name, &caps) != SUCCESS)
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET " (not a registered exchange)",
        exchange_name);
    cmd_reply(ctx, header);
    return;
  }

  if(!caps.has_credentials)
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET " (no credentials configured)",
        exchange_name);
    cmd_reply(ctx, header);
    return;
  }

  // Cache-first: render the warm snapshot instantly, no network wait.
  if(!force)
  {
    exchange_account_t cached[WM_ACCOUNT_ROW_CAP];
    uint32_t           n_cached = 0;
    int64_t            age_ms   = 0;

    if(wm_account_get_snapshot(exchange_name, cached, WM_ACCOUNT_ROW_CAP,
           &n_cached, &age_ms))
    {
      wm_bal_render_rows(ctx, exchange_name, cached, n_cached, age_ms, true);
      return;
    }
    // Cache miss (e.g. a paper-only exchange the poll gate never warms):
    // fall through to a one-shot blocking fetch.
  }

  {
    wm_sync_fetch_t *w = wm_sync_fetch_begin(sizeof(res));

    if(w == NULL)
    {
      snprintf(header, sizeof(header),
          CLR_BOLD "  %s" CLR_RESET "  " CLR_RED "FAIL" CLR_RESET
          ": out of memory", exchange_name);
      cmd_reply(ctx, header);
      return;
    }

    // The callback may run inline on a synchronous FAIL; the bridge
    // tolerates either ordering.
    (void)exchange_get_accounts_async(exchange_name, wm_bal_on_accounts, w);

    if(!wm_sync_fetch_wait(w, &res, sizeof(res), WM_EXCH_QUERY_WAIT_MS))
    {
      snprintf(header, sizeof(header),
          CLR_BOLD "  %s" CLR_RESET "  " CLR_RED "FAIL" CLR_RESET
          ": timed out", exchange_name);
      cmd_reply(ctx, header);
      return;
    }
  }

  if(res.err[0] != '\0')
  {
    snprintf(header, sizeof(header),
        CLR_BOLD "  %s" CLR_RESET "  " CLR_RED "FAIL" CLR_RESET ": %s",
        exchange_name, res.err);
    cmd_reply(ctx, header);
    return;
  }

  // Populate the cache so the next read is instant + carries a vintage,
  // then render this result as freshly fetched.
  wm_account_store_snapshot(exchange_name, res.rows, res.count);

  // Auto-reconcile real cash for flat markets on this exchange from the
  // snapshot we just fetched (no extra call) — an explicit balance view
  // also keeps per-market real cash synced.
  wm_live_reconcile_from_accounts(exchange_name, res.rows, res.count);

  wm_bal_render_rows(ctx, exchange_name, res.rows, res.count, 0, false);
}

static void
whenmoon_show_balances_cmd(const cmd_ctx_t *ctx)
{
  const char *p;
  char        tok[EXCHANGE_NAME_SZ] = {0};
  char        want[EXCHANGE_NAME_SZ] = {0};
  bool        force = false;
  char        names[WM_BAL_EXCH_CAP][EXCHANGE_NAME_SZ];
  uint32_t    n = 0;
  uint32_t    i;

  p = (ctx->args != NULL) ? ctx->args : "";

  // Tokens: an optional exchange name and/or the keyword `fresh` (force
  // a live refresh), in either order.
  while(wm_dl_next_token(&p, tok, sizeof(tok)))
  {
    if(strcasecmp(tok, "fresh") == 0 || strcasecmp(tok, "sync") == 0)
      force = true;
    else if(want[0] == '\0')
      snprintf(want, sizeof(want), "%s", tok);
  }

  cmd_reply(ctx, CLR_BOLD "whenmoon balances" CLR_RESET);

  // Explicit exchange argument: query just that one.
  if(want[0] != '\0')
  {
    wm_bal_render(ctx, want, force);
    return;
  }

  // No argument: query every registered exchange.
  if(exchange_name_list(names, WM_BAL_EXCH_CAP, &n) != SUCCESS || n == 0)
  {
    cmd_reply(ctx, "  (no exchanges registered)");
    return;
  }

  if(n > WM_BAL_EXCH_CAP)
    n = WM_BAL_EXCH_CAP;

  for(i = 0; i < n; i++)
    wm_bal_render(ctx, names[i], force);
}

// ------------------------------------------------------------------ //
// /whenmoon and /show whenmoon parents                                //
// ------------------------------------------------------------------ //

static void
whenmoon_root_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon <market|download|strategy|order|backtest|manual>"
      " ... (market start|stop|mode|force,"
      " download <market> [--gaps]|cancel,"
      " strategy attach|detach|reload, order buy|sell|cancel,"
      " backtest run; manual halts every market into MANUAL mode)");
}

// /whenmoon manual — operator halt. Flips every registered market into
// MANUAL mode regardless of position state. Synthetic backtest markets
// are not touched.
static void
whenmoon_manual_cb(const cmd_ctx_t *ctx)
{
  uint32_t visited       = 0;
  uint32_t with_position = 0;
  char     line[160];

  wm_market_halt_all(&visited, &with_position);

  if(visited == 0)
  {
    cmd_reply(ctx, "no markets to halt");
    return;
  }

  snprintf(line, sizeof(line),
      "halted %u market%s (%u with open positions)",
      visited, visited == 1 ? "" : "s", with_position);
  cmd_reply(ctx, line);
}

static void
whenmoon_show_root_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /show whenmoon"
      " <markets|balances|indicators|download|strategy|exchange|market>"
      " ... (download has subverbs: status, candles)");
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

  // /whenmoon manual — operator halt. Flips every market in
  // `whenmoon_state->markets->arr[]` into MANUAL mode, bypassing the
  // flat-position rule that wm_market_set_mode enforces. Open positions
  // freeze (no auto-flatten); subsequent strategy signals are
  // short-circuited at market_engine.c's MANUAL check.
  if(cmd_register("whenmoon", "manual",
        "whenmoon manual",
        "Operator halt: flip every market into MANUAL mode regardless"
        " of position state. Strategies keep emitting advice but no"
        " synthetic or real fills are produced. Synthetic backtest"
        " markets are not touched. Recovery: per-market"
        " `/whenmoon market mode <id> <paper|real>` (still requires"
        " flat position) or `/whenmoon market force` to unwind.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        whenmoon_manual_cb, NULL, "whenmoon", NULL,
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
  // `show whenmoon markets` (subscriptions) and `show whenmoon market
  // [sessions|<id>]` are both registered by wm_show_market_register_verbs
  // in market_cmds.c — they share one handler that owns the session
  // snapshot + detail-card machinery.
  if(cmd_register("whenmoon", "balances",
        "show whenmoon balances [exchange] [fresh]",
        "Account-balance snapshot. Cache-first: reads the per-exchange"
        " cache instantly with a vintage line (the scheduled poll keeps"
        " real-mode exchanges warm; real fills fast-forward it). No arg ="
        " every registered exchange; <exchange> = just that one. Add"
        " `fresh` to force a blocking live refresh. A cache miss (e.g. a"
        " paper-only deploy) also falls back to one blocking fetch."
        " Doubles as a key check (per-currency balance/hold/available, or"
        " the auth error).",
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

  // MW-2: cancel marketwatch periodic tasks before any other teardown
  // so in-flight ticker callbacks observe an explicit per-exchange
  // disable state instead of racing the pair-table free below.
  mw_stop();

  // Teardown order: strategies first (they may hold dispatch
  // references into market state), then live engine, then job table,
  // downloader DDL flag, markets, account. wm_account_destroy cancels
  // its per-slot periodics synchronously before the per-slot locks go.
  wm_strategy_registry_destroy(st);
  wm_live_engine_destroy();
  wm_dl_jobtable_destroy(st);
  wm_dl_destroy(st);
  wm_market_destroy(st);
  wm_account_destroy(st);

  // MW-2: free marketwatch state last (mirrors mw_stop early). Safe
  // when mw_init never ran (mw_g.n_exch == 0 + freshly-zeroed mutex
  // make this a no-op cascade).
  mw_deinit();
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

  if(wm_market_persist_global_init() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "market-persist global init failed");
    TA_Shutdown();
    return(FAIL);
  }

  st = mem_alloc("whenmoon", "state", sizeof(*st));

  if(st == NULL)
  {
    wm_market_persist_global_destroy();
    TA_Shutdown();
    return(FAIL);
  }

  memset(st, 0, sizeof(*st));
  whenmoon_state = st;

  // Order: markets container first (no DB or KV reads), then account,
  // then downloader DDL + scheduler. wm_market_restore runs in
  // whenmoon_start (post-kv_load) so plugin KV reads have settled.
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

  // Real-mode per-market submit path. Per-market risk caps
  // (daily_loss_bps, pending-cap, max-notional) gate the cascade;
  // operator halt is /whenmoon manual which flips every market into
  // MANUAL mode and short-circuits the submit path on the next signal.
  if(wm_live_engine_init() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "wm_live_engine_init failed");
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

  if(wm_show_market_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "show market verb registration failed");
    goto fail;
  }

  if(wm_strategy_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "strategy verb registration failed");
    goto fail;
  }

  if(wm_order_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "order verb registration failed");
    goto fail;
  }

  if(wm_exch_register_verbs() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "exchange show-verb registration failed");
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

  // MW-2: marketwatch substrate. mw_init registers the two global KVs
  // (enable + ring_n); per-exchange KVs are registered lazily in
  // mw_start once the exchange roster is final. Verb registration
  // happens here so /whenmoon mw and /show whenmoon mw are visible
  // before any task fires.
  if(mw_init() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "mw_init failed");
    goto fail;
  }

  if(mw_cmds_register() != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "mw verb registration failed");
    goto fail;
  }

  // Discover strategies that the core loader has already brought up.
  // This is idempotent and safe even when zero strategy plugins are
  // present (the iteration finds no PLUGIN_STRATEGY records).
  wm_strategy_registry_scan(st);

  // WM-BT-5: cap for the sweep default thread count. The default is
  // `max(1, nproc - 2)` so the host keeps two cores for IRC,
  // marketwatch, the live engine, and the OS; this KV further caps
  // that (0 = no cap, [1, 64] pool bounds still apply).
  if(kv_register("plugin.whenmoon.backtest.max_threads",
         KV_UINT64, "64", NULL, NULL,
         "Upper bound on the sweep default worker thread count."
         " The unbounded default is max(1, nproc - 2) so the host"
         " keeps two cores free; this KV caps further (0 = no cap)."
         " Clamped to the sweep pool's [1, 64] bounds regardless."
         " Workers run at nice 19 so a long sweep never starves"
         " the rest of the daemon.") != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "kv_register plugin.whenmoon.backtest.max_threads failed");
    goto fail;
  }

  // WM-BT-6: on-disk artifact root. Empty (the registered default)
  // resolves to $HOME/.local/share/botmanager/backtests at run time.
  // Set explicitly to override (e.g. a project-scoped corpus dir).
  if(kv_register("plugin.whenmoon.backtest.report_path",
         KV_STR, "", NULL, NULL,
         "Root directory under which each /whenmoon backtest run"
         " creates a sweep-id subdirectory (manifest.json +"
         " iterations.jsonl + top-N.txt + charts/). Empty resolves"
         " to $HOME/.local/share/botmanager/backtests at run time."
         ) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "kv_register plugin.whenmoon.backtest.report_path failed");
    goto fail;
  }

  // WM-RIGOR-6: audit trail for backtest runs that touch holdout data
  // (corpus range past the frozen 2025-03-31 research cutoff). Empty
  // resolves beside the other backtest artifacts at run time.
  if(kv_register("plugin.whenmoon.backtest.holdout_log",
         KV_STR, "", NULL, NULL,
         "File the --holdout audit trail appends to: one line at run"
         " submit + one at completion for every backtest run whose"
         " corpus extends past the 2025-03-31 research cutoff"
         " (WM-RIGOR-6 holdout discipline). Empty resolves to"
         " HOLDOUT_LOG.md under the resolved backtest report root."
         ) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "kv_register plugin.whenmoon.backtest.holdout_log failed");
    goto fail;
  }

  // WM-BT-6: chart generation toggle (consumed by WM-BT-8). Registered
  // alongside the other backtest knobs so a single freshstart picks up
  // the entire backtest KV surface.
  if(kv_register("plugin.whenmoon.backtest.charts_enabled",
         KV_BOOL, "false", NULL, NULL,
         "Emit Lightweight Charts HTML for the top-N iterations"
         " (WM-BT-8). Currently parked at false; the runtime"
         " surface is wired but the renderer is not yet shipped."
         ) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "kv_register plugin.whenmoon.backtest.charts_enabled failed");
    goto fail;
  }

  if(kv_register("plugin.whenmoon.backtest.charts_top_n",
         KV_UINT64, "10", NULL, NULL,
         "How many top-ranked iterations get charts when"
         " charts_enabled=true (WM-BT-8). Clamped to the actual"
         " top_n at run time.") != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX,
        "kv_register plugin.whenmoon.backtest.charts_top_n failed");
    goto fail;
  }

  // WM-LT-6: drop any per-iteration KV slots left over from a prior
  // crashed sweep so kv_register on a freshly-allocated synthetic id
  // never trips on a stale row. Safe at boot — no other thread is
  // touching those keys yet.
  wm_bt_sweep_cleanup_stale_kv();

  // WM-TAILFILL-COALESCE-1: one global tail-fill sweep for the plugin
  // (NOT one timer per market session). Needs `st` — hence here, after it
  // exists. Harmless before restore: it skips every non-READY market.
  if(wm_warm_tailfill_global_init(st) != SUCCESS)
  {
    clam(CLAM_INFO, WHENMOON_CTX, "tailfill sweep init failed");
    goto fail;
  }

  clam(CLAM_INFO, WHENMOON_CTX, "whenmoon plugin initialized");
  return(SUCCESS);

fail:
  // Cancel the sweep BEFORE `st` is freed — the task reads through it.
  wm_warm_tailfill_global_destroy();
  whenmoon_subsystems_destroy(st);
  whenmoon_state = NULL;
  mem_free(st);
  wm_market_persist_global_destroy();
  TA_Shutdown();
  return(FAIL);
}

// WM-MR-1: restore runs in start (post-kv_load) so per-plugin KV reads
// see the persisted values rather than the kv_register defaults.
static bool
whenmoon_start(void)
{
  whenmoon_state_t *st = whenmoon_state;

  if(st == NULL)
    return(SUCCESS);

  if(wm_market_restore(st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "wm_market_restore failed (plugin starts with no markets)");

  // WM-MK-2: hydrate per-market sessions immediately after the running
  // set is non-empty. Rows whose market_id is not in the running set
  // are left untouched.
  if(wm_market_persist_restore_all(st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "wm_market_persist_restore_all failed (sessions left at default)");

  // Balance cache: schedule the per-exchange poll now that the exchange
  // roster is settled and per-market session modes are restored (the
  // real-mode poll gate reads them). Runs after restore_all so the
  // start-time initial fetch sees the real running state.
  if(wm_account_start(st) != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "wm_account_start failed (balance cache refresh disabled)");

  // MW-2: marketwatch start runs after the exchange roster is settled
  // (each service plugin's start has already registered with
  // feature_exchange) so mw_start's exchange_name_list sees the final
  // set. Per-exchange KVs are registered + read here; default is
  // disabled so an unconfigured deploy emits no traffic.
  if(mw_start() != SUCCESS)
    clam(CLAM_INFO, WHENMOON_CTX,
        "mw_start failed (marketwatch disabled)");

  // WM-LT-8-B3: schedule the REST /fills safety-net poll + boot
  // reconcile (advisory list of any open orders left resting at the
  // gateway across a prior daemon life). Idempotent.
  wm_live_engine_start();

  return(SUCCESS);
}

// PLIFE-5: whenmoon's Class-B holding that core cannot reason about. A
// running sweep has worker threads inside this plugin's .text and has
// cached strategy function pointers for the length of an iteration —
// no default teardown makes unmapping that safe, so the refusal is
// ours to make. Everything else whenmoon owns (periodic tasks, the
// supervisor tick, marketwatch pairs) is cancelled and drained on the
// deinit path below.
static bool
whenmoon_stop(void)
{
  uint32_t sweeps = wm_bt_sweep_active_count();

  if(sweeps > 0)
  {
    clam(CLAM_WARN, WHENMOON_CTX,
        "stop refused: %u backtest sweep(s) still running", sweeps);
    return(FAIL);
  }

  return(SUCCESS);
}

static void
whenmoon_deinit(void)
{
  whenmoon_state_t *st = whenmoon_state;

  // Cancel the sweep BEFORE `st` is freed — the task reads through it.
  wm_warm_tailfill_global_destroy();

  if(st != NULL)
  {
    whenmoon_subsystems_destroy(st);
    whenmoon_state = NULL;
    mem_free(st);
  }

  wm_market_persist_global_destroy();
  TA_Shutdown();
  clam(CLAM_INFO, WHENMOON_CTX, "whenmoon plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "whenmoon",
  .version              = "0.10-mk3",
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
  .start                = whenmoon_start,
  .stop                 = whenmoon_stop,
  .deinit               = whenmoon_deinit,
  .ext                  = NULL,
};
