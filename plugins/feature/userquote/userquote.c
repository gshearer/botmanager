// botmanager — MIT
// userquote feature plugin (PLUGIN_FEATURE, kind: userquote).
//
// A cosmetic "quote another user" capability: users capture a memorable
// line into a per-userns quote book and recall it later. The command
// surface (`quote` / `quote add` / `quote del`) is registered globally,
// so it works from any bot on any method.
//
// The `!quote add` no-args shortcut immortalises whoever just spoke by
// reading the dispatching bot's own last-witnessed public line
// (bot_last_public_line) — no message observation lives here.
//
// Successor to the standalone "quotebot" (hedgehogg).

#define USERQUOTE_INTERNAL
#include "userquote.h"

#include "kv.h"

// ------------------------------------------------------------------ //
// KV schema                                                           //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t uq_kv_schema[] = {
  { UQ_KV_TABLE,     KV_STR,    "userquotes",
    "Postgres table name for the quote book (bare SQL identifier)" },
  { UQ_KV_MAX_QUOTE, KV_UINT32, "512",
    "Maximum stored quote length in characters" },
  { UQ_KV_MAX_SAYER, KV_UINT32, "64",
    "Maximum search-key (sayer) length in characters" },
};

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
uq_init(void)
{
  if(uq_commands_register() != SUCCESS)
  {
    clam(CLAM_WARN, UQ_CTX, "command registration failed");
    return(FAIL);
  }

  clam(CLAM_INFO, UQ_CTX, "userquote plugin initialized");
  return(SUCCESS);
}

// Schema bootstrap runs in start(), after kv_load(), so the table name
// reflects the persisted KV rather than the register-time default.
static bool
uq_start(void)
{
  if(uq_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, UQ_CTX,
        "quote schema init failed (recall/add will error until fixed)");

  return(SUCCESS);
}

static void
uq_deinit(void)
{
  uq_commands_unregister();
  clam(CLAM_INFO, UQ_CTX, "userquote plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "userquote",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "userquote",
  .provides             = { { .name = "feature_userquote" } },
  .provides_count       = 1,
  .requires             = { { .name = "method_text" } },
  .requires_count       = 1,
  .kv_schema            = uq_kv_schema,
  .kv_schema_count      = sizeof(uq_kv_schema) / sizeof(uq_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = uq_init,
  .start                = uq_start,
  .stop                 = NULL,
  .deinit               = uq_deinit,
  .ext                  = NULL,
};
