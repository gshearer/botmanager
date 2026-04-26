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
typedef struct dl_jobtable      dl_jobtable_t;

// Plugin-global singleton state. Allocated in whenmoon_init, freed in
// whenmoon_deinit. Markets, account, and downloader are now plugin-
// scoped (no per-bot binding) following WM-G1.
typedef struct whenmoon_state
{
  whenmoon_markets_t *markets;   // owned; NULL when market init failed
  whenmoon_account_t *account;   // owned; NULL when account init failed

  // Downloader schema + registry readiness. Flipped to true by
  // wm_dl_init() once the core metadata tables exist. WM-S4+ gates all
  // downloader entry points on this bit so a plugin whose init failed
  // cannot enqueue jobs.
  bool                dl_ready;

  // Plugin-global download job table. Spawned in whenmoon_init after
  // wm_dl_init succeeds; torn down from whenmoon_deinit before the DDL
  // readiness flag drops. EX-1: rate limit + retry now owned by
  // feature_exchange; this struct is purely the job list + concurrency
  // cap.
  dl_jobtable_t      *downloader;
} whenmoon_state_t;

// Plugin-global singleton accessor. Returns NULL before whenmoon_init
// runs and after whenmoon_deinit. Used by command callbacks that no
// longer have a per-bot handle (markets/account/downloader are now
// plugin-scoped).
whenmoon_state_t *whenmoon_get_state(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_H
