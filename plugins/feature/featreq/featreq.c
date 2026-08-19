// botmanager — MIT
// featreq feature plugin (PLUGIN_FEATURE, kind: featreq).
//
// A suggestion box with a global table behind it: anyone registered can
// file a request, anyone can read the board, and the owner moves rows
// along it. No bot driver (`.ext = NULL`) and no per-bot state — the
// board belongs to the daemon, not to a bot.

#define FEATREQ_INTERNAL
#include "featreq.h"

#include "kv.h"

// ------------------------------------------------------------------ //
// KV schema                                                           //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t fr_kv_schema[] = {
  { FR_KV_TABLE,     KV_STR,    "feature_requests",
    "Postgres table name for the request board (bare SQL identifier)" },
  { FR_KV_MAX_DESC,  KV_UINT32, "200",
    "Longest description a request may carry, in characters" },
  { FR_KV_LIST_ROWS, KV_UINT32, "20",
    "Rows one `show feature` draws (0 = the default; 100 is the ceiling)" },
};

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
fr_init(void)
{
  if(fr_commands_register() != SUCCESS || fr_show_register() != SUCCESS)
  {
    clam(CLAM_WARN, FR_CTX, "command registration failed");
    return(FAIL);
  }

  clam(CLAM_INFO, FR_CTX, "featreq plugin initialized");
  return(SUCCESS);
}

// The schema is bootstrapped in start(), after kv_load(), so the table
// name is the persisted one rather than the register-time default. A
// board we cannot reach is not a reason to refuse the plugin: the
// commands are registered and each one says so on the way past.
static bool
fr_start(void)
{
  if(fr_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, FR_CTX,
        "schema init failed (the board will error until it is fixed)");

  return(SUCCESS);
}

static void
fr_deinit(void)
{
  fr_show_unregister();
  fr_commands_unregister();
  clam(CLAM_INFO, FR_CTX, "featreq plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "featreq",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "featreq",
  .provides             = { { .name = "feature_featreq" } },
  .provides_count       = 1,
  .requires             = { { .name = "bot_chat" } },
  .requires_count       = 1,
  .kv_schema            = fr_kv_schema,
  .kv_schema_count      = sizeof(fr_kv_schema) / sizeof(fr_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = fr_init,
  .start                = fr_start,
  .stop                 = NULL,
  .deinit               = fr_deinit,
  .ext                  = NULL,
};
