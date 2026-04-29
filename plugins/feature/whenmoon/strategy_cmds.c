// botmanager — MIT
// Whenmoon strategy admin verbs (state-changing under
// /whenmoon strategy; observability under /show whenmoon strategy).

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "strategy.h"
#include "market.h"
#include "dl_commands.h"

#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Stack-buffer cap for /show whenmoon strategy <name> attachment list.
// Far above any realistic per-strategy attachment count; if hit, the
// view truncates with a continuation note.
#define WM_STRATEGY_SHOW_MAX_ATTACH  64

// ----------------------------------------------------------------------- //
// /whenmoon strategy attach <market_id> <strategy_name>                   //
// ----------------------------------------------------------------------- //

static void
wm_strategy_cmd_attach(const cmd_ctx_t *ctx)
{
  whenmoon_state_t  *st;
  const char        *p;
  char               id_tok[64]   = {0};
  char               name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char               err[160];
  char               reply[224];
  wm_attach_result_t r;

  st = whenmoon_get_state();

  if(st == NULL || st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)) ||
     !wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon strategy attach <market_id> <strategy_name>");
    return;
  }

  err[0] = '\0';
  r = wm_strategy_attach(st, id_tok, name_tok, err, sizeof(err));

  switch(r)
  {
    case WM_ATTACH_OK:
      snprintf(reply, sizeof(reply), "attached %s -> %s",
          name_tok, id_tok);
      cmd_reply(ctx, reply);
      break;

    case WM_ATTACH_NO_STRATEGY:
    case WM_ATTACH_NO_MARKET:
    case WM_ATTACH_DUPLICATE:
    case WM_ATTACH_INIT_FAILED:
    case WM_ATTACH_OOM:
      snprintf(reply, sizeof(reply), "attach failed: %s",
          err[0] != '\0' ? err : "unknown");
      cmd_reply(ctx, reply);
      break;

    case WM_ATTACH_NO_REGISTRY:
      cmd_reply(ctx, "attach failed: strategy registry not ready");
      break;
  }
}

// ----------------------------------------------------------------------- //
// /whenmoon strategy detach <market_id> <strategy_name>                   //
// ----------------------------------------------------------------------- //

static void
wm_strategy_cmd_detach(const cmd_ctx_t *ctx)
{
  whenmoon_state_t  *st;
  const char        *p;
  char               id_tok[64]   = {0};
  char               name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char               reply[192];
  wm_detach_result_t r;

  st = whenmoon_get_state();

  if(st == NULL || st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, id_tok, sizeof(id_tok)) ||
     !wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon strategy detach <market_id> <strategy_name>");
    return;
  }

  r = wm_strategy_detach(st, id_tok, name_tok);

  switch(r)
  {
    case WM_DETACH_OK:
      snprintf(reply, sizeof(reply), "detached %s -> %s",
          name_tok, id_tok);
      cmd_reply(ctx, reply);
      break;

    case WM_DETACH_NOT_FOUND:
      snprintf(reply, sizeof(reply),
          "detach: %s not attached to %s", name_tok, id_tok);
      cmd_reply(ctx, reply);
      break;

    case WM_DETACH_NO_REGISTRY:
      cmd_reply(ctx, "detach failed: strategy registry not ready");
      break;
  }
}

// ----------------------------------------------------------------------- //
// /whenmoon strategy reload <strategy_name>                               //
// ----------------------------------------------------------------------- //
//
// Detach all attachments, dlclose, dlopen, run init/start. The user
// re-attaches afterward; auto-reattach is out of scope for WM-LT-3 to
// keep the dev-iteration loop predictable.

static void
wm_strategy_cmd_reload(const cmd_ctx_t *ctx)
{
  whenmoon_state_t *st;
  const char       *p;
  char              name_tok[WM_STRATEGY_NAME_SZ] = {0};
  char              err[192];
  char              reply[256];
  uint32_t          n_detached = 0;

  st = whenmoon_get_state();

  if(st == NULL || st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, name_tok, sizeof(name_tok)))
  {
    cmd_reply(ctx,
        "usage: /whenmoon strategy reload <strategy_name>");
    return;
  }

  err[0] = '\0';

  if(wm_strategy_reload(st, name_tok, &n_detached, err, sizeof(err))
         != SUCCESS)
  {
    snprintf(reply, sizeof(reply), "reload failed: %s",
        err[0] != '\0' ? err : "unknown");
    cmd_reply(ctx, reply);
    return;
  }

  snprintf(reply, sizeof(reply),
      "reloaded %s (detached %u, dlclose+dlopen ok)",
      name_tok, n_detached);
  cmd_reply(ctx, reply);
  cmd_reply(ctx,
      "  re-attach: /whenmoon strategy attach <market_id> <name>");
}

// ----------------------------------------------------------------------- //
// /show whenmoon strategy [<name>]                                        //
// ----------------------------------------------------------------------- //

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} wm_strategy_list_state_t;

static void
wm_strategy_list_cb(const loaded_strategy_t *ls, void *user)
{
  wm_strategy_list_state_t *s = user;
  char                      line[224];

  snprintf(line, sizeof(line),
      "  %-32s v%-10s grains=0x%04x params=%-3u attachments=%u",
      ls->name, ls->version,
      (unsigned)ls->meta.grains_mask, ls->meta.n_params,
      ls->n_attachments);
  cmd_reply(s->ctx, line);

  s->count++;
}

// Append per-grain history requirements for one strategy (used in the
// detail view). Renders the populated min_history slots only.
static void
wm_strategy_render_min_history(const cmd_ctx_t *ctx,
    const wm_strategy_meta_t *meta)
{
  static const char *const labels[WM_GRAN_MAX] = {
    "1m", "5m", "15m", "1h", "6h", "1d"
  };

  char     line[160];
  uint32_t off = 0;
  uint32_t i;
  int      n;
  bool     any = false;

  n = snprintf(line + off, sizeof(line) - off, "  min_history:");
  if(n < 0)
    return;
  off += (uint32_t)n;

  for(i = 0; i < WM_GRAN_MAX; i++)
  {
    if(meta->min_history[i] == 0)
      continue;

    if(off >= sizeof(line))
      break;

    n = snprintf(line + off, sizeof(line) - off,
        " %s=%u", labels[i], meta->min_history[i]);
    if(n < 0)
      break;
    off += (uint32_t)n;
    any = true;
  }

  if(!any)
    snprintf(line + off, sizeof(line) - off, " (none)");

  cmd_reply(ctx, line);
}

// Param schema view. Keeps each row short; long help text is omitted
// here to keep the IRC line count bounded.
static void
wm_strategy_render_params(const cmd_ctx_t *ctx,
    const loaded_strategy_t *ls)
{
  char     line[224];
  uint32_t i;

  if(ls->meta.n_params == 0 || ls->meta.params == NULL)
  {
    cmd_reply(ctx, "  params: (none)");
    return;
  }

  cmd_reply(ctx, CLR_GRAY "  params:" CLR_RESET);

  for(i = 0; i < ls->meta.n_params; i++)
  {
    const wm_strategy_param_t *p = &ls->meta.params[i];

    switch(p->type)
    {
      case WM_PARAM_INT:
        snprintf(line, sizeof(line),
            "    %-20s int     default=%-12" PRId64
            " range=[%" PRId64 "..%" PRId64 "]",
            p->name, p->default_int, p->min_int, p->max_int);
        break;

      case WM_PARAM_UINT:
        snprintf(line, sizeof(line),
            "    %-20s uint    default=%-12" PRIu64
            " range=[%" PRIu64 "..%" PRIu64 "]",
            p->name, (uint64_t)p->default_int,
            (uint64_t)p->min_int, (uint64_t)p->max_int);
        break;

      case WM_PARAM_DOUBLE:
        snprintf(line, sizeof(line),
            "    %-20s double  default=%-12.6g"
            " range=[%.6g..%.6g] step=%.6g",
            p->name, p->default_dbl, p->min_dbl, p->max_dbl,
            p->step_dbl);
        break;

      case WM_PARAM_STR:
        snprintf(line, sizeof(line),
            "    %-20s str     default=\"%s\"",
            p->name, p->default_str != NULL ? p->default_str : "");
        break;

      default:
        snprintf(line, sizeof(line),
            "    %-20s (unknown type)", p->name);
        break;
    }

    cmd_reply(ctx, line);
  }
}

static void
wm_strategy_render_attachment(const cmd_ctx_t *ctx,
    const wm_attach_snapshot_t *snap)
{
  char    line[256];
  char    market[64];
  size_t  mlen;

  // Bounded copy so snprintf's truncation analysis sees the
  // upper-bound on %s rather than treating the struct field as
  // unbounded. Pattern: format-truncation bounded-copy idiom.
  mlen = strnlen(snap->market_id_str, sizeof(market) - 1);
  memcpy(market, snap->market_id_str, mlen);
  market[mlen] = '\0';

  snprintf(line, sizeof(line),
      "    market=%s bars_seen=%" PRIu64
      " signals=%" PRIu64 " last_bar_ts_ms=%" PRId64,
      market, snap->bars_seen,
      snap->signals_emitted, snap->last_bar_ts_ms);
  cmd_reply(ctx, line);

  if(snap->has_last_signal)
  {
    char    reason[WM_STRATEGY_REASON_SZ];
    size_t  rlen;

    rlen = strnlen(snap->last_signal.reason, sizeof(reason) - 1);
    memcpy(reason, snap->last_signal.reason, rlen);
    reason[rlen] = '\0';

    snprintf(line, sizeof(line),
        "      last_signal: ts=%" PRId64
        " score=%+.4f conf=%.4f reason=%s",
        snap->last_signal.ts_ms,
        snap->last_signal.score,
        snap->last_signal.confidence,
        reason[0] != '\0' ? reason : "(none)");
    cmd_reply(ctx, line);
  }

  else
    cmd_reply(ctx, "      last_signal: (none)");
}

static void
wm_strategy_show_detail(const cmd_ctx_t *ctx, whenmoon_state_t *st,
    const char *strategy_name)
{
  loaded_strategy_t        *ls;
  wm_attach_snapshot_t      snaps[WM_STRATEGY_SHOW_MAX_ATTACH];
  wm_strategy_meta_t        meta_copy;
  char                      name_copy[WM_STRATEGY_NAME_SZ];
  char                      version_copy[WM_STRATEGY_VERSION_SZ];
  char                      path_copy[512];
  char                      header[256];
  uint32_t                  n_attach;
  uint32_t                  i;
  bool                      wants_trade;

  // Snapshot the per-strategy core fields under the registry lock so
  // the rendering pass below can run without holding it.
  pthread_mutex_lock(&st->strategies->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls == NULL)
  {
    pthread_mutex_unlock(&st->strategies->lock);

    {
      char line[160];

      snprintf(line, sizeof(line), "strategy %s not loaded",
          strategy_name);
      cmd_reply(ctx, line);
    }

    return;
  }

  meta_copy = ls->meta;
  snprintf(name_copy, sizeof(name_copy), "%s", ls->name);
  snprintf(version_copy, sizeof(version_copy), "%s", ls->version);
  snprintf(path_copy, sizeof(path_copy), "%s", ls->plugin_path);
  wants_trade = ls->meta.wants_trade_callback;

  pthread_mutex_unlock(&st->strategies->lock);

  snprintf(header, sizeof(header),
      CLR_BOLD "%s" CLR_RESET " v%s",
      name_copy, version_copy);
  cmd_reply(ctx, header);

  if(path_copy[0] != '\0')
  {
    // Path may legitimately be long; print it on its own line and
    // let cmd_reply do its own truncation if necessary.
    cmd_reply(ctx, "  path:");

    {
      char path_line[600];

      snprintf(path_line, sizeof(path_line), "    %s", path_copy);
      cmd_reply(ctx, path_line);
    }
  }

  snprintf(header, sizeof(header),
      "  abi=%u grains_mask=0x%04x trade_callback=%s",
      meta_copy.abi_version, (unsigned)meta_copy.grains_mask,
      wants_trade ? "yes" : "no");
  cmd_reply(ctx, header);

  wm_strategy_render_min_history(ctx, &meta_copy);

  // Re-acquire the lock briefly to render the param schema (params
  // pointer is stable for the strategy's lifetime, but we don't want
  // to assume across a possible reload — copy under the lock).
  pthread_mutex_lock(&st->strategies->lock);

  ls = wm_strategy_find_loaded(st, strategy_name);

  if(ls != NULL)
    wm_strategy_render_params(ctx, ls);

  pthread_mutex_unlock(&st->strategies->lock);

  n_attach = wm_strategy_snapshot_attachments(st, strategy_name,
      snaps, (uint32_t)(sizeof(snaps) / sizeof(snaps[0])));

  if(n_attach == 0)
  {
    cmd_reply(ctx, "  attachments: (none)");
    return;
  }

  cmd_reply(ctx, CLR_GRAY "  attachments:" CLR_RESET);

  for(i = 0; i < n_attach; i++)
    wm_strategy_render_attachment(ctx, &snaps[i]);

  if(n_attach == WM_STRATEGY_SHOW_MAX_ATTACH)
    cmd_reply(ctx,
        "    (truncated; raise WM_STRATEGY_SHOW_MAX_ATTACH to see more)");
}

static void
wm_strategy_cmd_show(const cmd_ctx_t *ctx)
{
  whenmoon_state_t         *st;
  const char               *p;
  char                      name_tok[WM_STRATEGY_NAME_SZ] = {0};
  wm_strategy_list_state_t  ls_state;

  st = whenmoon_get_state();

  if(st == NULL || st->strategies == NULL)
  {
    cmd_reply(ctx, "whenmoon: strategy registry not ready");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(wm_dl_next_token(&p, name_tok, sizeof(name_tok)) &&
     name_tok[0] != '\0')
  {
    wm_strategy_show_detail(ctx, st, name_tok);
    return;
  }

  cmd_reply(ctx, CLR_BOLD "whenmoon strategies" CLR_RESET);

  ls_state.ctx   = ctx;
  ls_state.count = 0;

  wm_strategy_loaded_iterate(st, wm_strategy_list_cb, &ls_state);

  if(ls_state.count == 0)
    cmd_reply(ctx, "  (no strategies loaded)");
}

// ----------------------------------------------------------------------- //
// Parent stub                                                             //
// ----------------------------------------------------------------------- //

static void
wm_strategy_parent_cb(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon strategy <attach|detach|reload> ...");
}

// ----------------------------------------------------------------------- //
// Registration                                                            //
// ----------------------------------------------------------------------- //

bool
wm_strategy_register_verbs(void)
{
  // /whenmoon strategy parent.
  if(cmd_register("whenmoon", "strategy",
        "whenmoon strategy <verb> ...",
        "Trading-strategy registry controls.",
        "Subcommands: attach <market_id> <name>,"
        " detach <market_id> <name>,"
        " reload <name>.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_strategy_parent_cb, NULL, "whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "attach",
        "whenmoon strategy attach <market_id> <strategy_name>",
        "Attach a strategy to a running market."
        " Registers per-attachment KV override slots at"
        " plugin.whenmoon.market.<id>.strategy.<name>.<param>"
        " and runs the strategy's init() callback. Strategy must"
        " already be loaded (visible via /show whenmoon strategy).",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_strategy_cmd_attach, NULL, "whenmoon/strategy", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "detach",
        "whenmoon strategy detach <market_id> <strategy_name>",
        "Detach a strategy from a running market."
        " Calls the strategy's finalize() and frees the per-attachment"
        " context. KV override slots persist for inspection.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_strategy_cmd_detach, NULL, "whenmoon/strategy", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("whenmoon", "reload",
        "whenmoon strategy reload <strategy_name>",
        "Detach all attachments, dlclose the strategy plugin,"
        " dlopen it (picks up a fresh build), and re-init."
        " Re-attach manually after reload.",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_strategy_cmd_reload, NULL, "whenmoon/strategy", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // /show whenmoon strategy
  if(cmd_register("whenmoon", "strategy",
        "show whenmoon strategy [<name>]",
        "List loaded strategies (no arg) or show details for one.",
        "With no argument: one-line per loaded strategy with name,"
        " version, grains_mask, param count, and live attachment"
        " count.\n"
        "With a name: full meta + param schema + per-attachment"
        " bars_seen / signals_emitted / last_signal.",
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        wm_strategy_cmd_show, NULL, "show/whenmoon", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
