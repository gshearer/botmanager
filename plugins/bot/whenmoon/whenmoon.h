// whenmoon.h — whenmoon trading bot plugin (kind: whenmoon)
//
// Internal-only header. Subsystem pieces (market, account, …) live in
// sibling headers also gated by WHENMOON_INTERNAL.

#ifndef BM_WHENMOON_H
#define BM_WHENMOON_H

#ifdef WHENMOON_INTERNAL

#include "bot.h"
#include "clam.h"
#include "common.h"
#include "alloc.h"
#include "plugin.h"

#define WHENMOON_CTX  "whenmoon"

// Forward decls — the full structs live in market.h / account.h, both
// WHENMOON_INTERNAL-gated, and are only visible inside the plugin.
typedef struct whenmoon_markets whenmoon_markets_t;
typedef struct whenmoon_account whenmoon_account_t;
typedef struct dl_scheduler     dl_scheduler_t;

// Per-instance state.
typedef struct whenmoon_state
{
  bot_inst_t         *inst;
  char                bot_name[BOT_NAME_SZ];

  whenmoon_markets_t *markets;   // owned; NULL when market init failed
  whenmoon_account_t *account;   // owned; NULL when account init failed

  // Downloader schema + registry readiness. Flipped to true by
  // wm_dl_init() once the core metadata tables exist (CREATE IF NOT
  // EXISTS is per-daemon; the flag is per-bot). Cleared by
  // wm_dl_destroy() at bot teardown. WM-S4+ gate all downloader entry
  // points on this bit so a bot whose init failed cannot enqueue jobs.
  bool                dl_ready;

  // Per-bot download scheduler. Spawned after wm_dl_init succeeds in
  // whenmoon_start_cb; torn down from whenmoon_destroy before the DDL
  // readiness flag drops.
  dl_scheduler_t     *downloader;
} whenmoon_state_t;

// Bot driver vtable — defined in whenmoon.c.
static const bot_driver_t whenmoon_driver;

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_H
