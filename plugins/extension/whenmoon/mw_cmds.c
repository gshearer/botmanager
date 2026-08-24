// botmanager — MIT
// /whenmoon mw + /show whenmoon mw operator verbs (MW-2).
//
// Wires the mw_* runtime ops into the unified command tree. Reply
// pattern follows the existing wm_show_orders_done shape:
// method_send (thread-safe, callable from the dispatch task thread)
// for multi-line output; cmd_reply for short single-line replies that
// always emit on the dispatch task.

#define WHENMOON_INTERNAL
#include "whenmoon.h"
#include "mw.h"
#include "dl_commands.h"
#include "exchange_api.h"
#include "userns.h"
#include "method.h"
#include "cmd.h"
#include "common.h"

#include <stdio.h>
#include <string.h>

#define MW_CMD_TARGET_SZ  128

static const char *
mw_cmd_target(const cmd_ctx_t *ctx)
{
  if(ctx == NULL || ctx->msg == NULL)
    return(NULL);

  return(ctx->msg->channel[0] != '\0'
      ? ctx->msg->channel
      : ctx->msg->sender);
}

// ------------------------------------------------------------------ //
// /whenmoon mw                                                        //
// ------------------------------------------------------------------ //

static void
wm_cmd_whenmoon_mw_parent(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx,
      "usage: /whenmoon mw <enable|disable|global> <exchange|on|off>");
}

static void
wm_cmd_whenmoon_mw_enable(const cmd_ctx_t *ctx)
{
  const char *p;
  char        exch_tok[EXCHANGE_NAME_SZ] = {0};
  char        line[160];

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)))
  {
    cmd_reply(ctx, "usage: /whenmoon mw enable <exchange>");
    return;
  }

  if(mw_enable_exch(exch_tok) != SUCCESS)
  {
    snprintf(line, sizeof(line),
        "exchange '%s' not registered", exch_tok);
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line),
      "marketwatch enabled for %s", exch_tok);
  cmd_reply(ctx, line);
}

static void
wm_cmd_whenmoon_mw_disable(const cmd_ctx_t *ctx)
{
  const char *p;
  char        exch_tok[EXCHANGE_NAME_SZ] = {0};
  char        line[160];

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)))
  {
    cmd_reply(ctx, "usage: /whenmoon mw disable <exchange>");
    return;
  }

  if(mw_disable_exch(exch_tok) != SUCCESS)
  {
    snprintf(line, sizeof(line),
        "exchange '%s' not registered", exch_tok);
    cmd_reply(ctx, line);
    return;
  }

  snprintf(line, sizeof(line),
      "marketwatch disabled for %s", exch_tok);
  cmd_reply(ctx, line);
}

static void
wm_cmd_whenmoon_mw_global(const cmd_ctx_t *ctx)
{
  const char *p;
  char        arg_tok[16] = {0};
  bool        on;

  p = ctx->args != NULL ? ctx->args : "";

  if(!wm_dl_next_token(&p, arg_tok, sizeof(arg_tok)))
  {
    cmd_reply(ctx, "usage: /whenmoon mw global <on|off>");
    return;
  }

  if(strcmp(arg_tok, "on") == 0 || strcmp(arg_tok, "enable") == 0
      || strcmp(arg_tok, "1") == 0)
    on = true;
  else if(strcmp(arg_tok, "off") == 0 || strcmp(arg_tok, "disable") == 0
      || strcmp(arg_tok, "0") == 0)
    on = false;
  else
  {
    cmd_reply(ctx, "expected 'on' or 'off'");
    return;
  }

  (void)mw_set_global_enabled(on);
  cmd_reply(ctx, on
      ? "marketwatch globally enabled"
      : "marketwatch globally disabled");
}

// ------------------------------------------------------------------ //
// /show whenmoon mw                                                   //
// ------------------------------------------------------------------ //

static void
wm_cmd_show_mw(const cmd_ctx_t *ctx)
{
  const char *p;
  const char *target;
  char        exch_tok[EXCHANGE_NAME_SZ] = {0};
  char        line[160];

  target = mw_cmd_target(ctx);

  if(target == NULL || target[0] == '\0' || ctx->msg->inst == NULL)
  {
    cmd_reply(ctx, "no reply target");
    return;
  }

  p = ctx->args != NULL ? ctx->args : "";

  if(wm_dl_next_token(&p, exch_tok, sizeof(exch_tok)))
  {
    if(mw_render_status_exch(ctx->msg->inst, target, exch_tok) != SUCCESS)
    {
      snprintf(line, sizeof(line),
          "exchange '%s' not registered", exch_tok);
      cmd_reply(ctx, line);
    }
    return;
  }

  mw_render_status(ctx->msg->inst, target);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_decl_t whenmoon_mw_decl = {
  .module      = "whenmoon",
  .name        = "mw",
  .usage       = "whenmoon mw <enable|disable|global> ...",
  .description = "Marketwatch (mw) controls: per-exchange poll enable/disable,"
                 " plus global on/off cascade.",
  .help_long   = "Subverbs: enable <exch>, disable <exch>, global <on|off>."
                 " Persists via plugin.whenmoon.mw.{enabled,<exch>.enabled,"
                 "<exch>.poll_sec}. Global on cascades to per-exchange tasks"
                 " already marked enabled; global off cancels them but leaves"
                 " per-exchange flags intact so a subsequent global on resumes"
                 " without re-enabling individually.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_cmd_whenmoon_mw_parent,
  .parent_path = "whenmoon",
};

static const cmd_decl_t whenmoon_mw_enable_decl = {
  .module      = "whenmoon",
  .name        = "enable",
  .usage       = "whenmoon mw enable <exchange>",
  .description = "Enable marketwatch polling for the named exchange.",
  .help_long   = "Idempotent. Spawns the per-exchange periodic task at the"
                 " configured cadence when the global flag is also on.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_cmd_whenmoon_mw_enable,
  .parent_path = "whenmoon/mw",
};

static const cmd_decl_t whenmoon_mw_disable_decl = {
  .module      = "whenmoon",
  .name        = "disable",
  .usage       = "whenmoon mw disable <exchange>",
  .description = "Disable marketwatch polling for the named exchange.",
  .help_long   = "Idempotent. Cancels the periodic task, clears the pair"
                 " table, and persists the disabled state. Ring memory is"
                 " retained for a fast subsequent re-enable.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_cmd_whenmoon_mw_disable,
  .parent_path = "whenmoon/mw",
};

static const cmd_decl_t whenmoon_mw_global_decl = {
  .module      = "whenmoon",
  .name        = "global",
  .usage       = "whenmoon mw global <on|off>",
  .description = "Globally enable or disable the marketwatch subsystem.",
  .help_long   = "Cascades to every per-exchange flag that is already enabled:"
                 " on spawns missing tasks, off cancels active ones. Per-"
                 " exchange enable flags are NOT cleared, so a later 'global"
                 " on' resumes the same set without per-exchange re-enabling.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_cmd_whenmoon_mw_global,
  .parent_path = "whenmoon/mw",
};

static const cmd_decl_t show_whenmoon_mw_decl = {
  .module      = "whenmoon",
  .name        = "mw",
  .usage       = "show whenmoon mw [<exchange>]",
  .description = "Marketwatch state. No arg = per-exchange summary table."
                 " With arg = per-exchange detail (top-10 by |pct_24h| and"
                 " top-10 by vol_24h_quote when available).",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = wm_cmd_show_mw,
  .parent_path = "show/whenmoon",
};

bool
mw_cmds_register(void)
{
  // /whenmoon mw — state-changing parent.
  if(cmd_register(&whenmoon_mw_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_mw_enable_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_mw_disable_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&whenmoon_mw_global_decl) != SUCCESS)
    return(FAIL);

  // /show whenmoon mw [<exch>] — observability.
  if(cmd_register(&show_whenmoon_mw_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
