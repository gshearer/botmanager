// botmanager — MIT
// note feature plugin (PLUGIN_FEATURE, kind: note).
//
// Leave a message for another user of the same namespace; the next bot to
// witness that user speak announces it and marks it delivered. The
// command surface is registered globally (note_cmds.c), persistence lives
// in note_db.c, and the observers + pending set live in note_deliver.c.
//
// No bot driver (`.ext = NULL`): there is nothing to `/bot add`. Load the
// plugin and it attaches to whatever bots are running.

#define NOTE_INTERNAL
#include "note.h"

#include "kv.h"

// ------------------------------------------------------------------ //
// KV schema                                                           //
// ------------------------------------------------------------------ //

static const plugin_kv_entry_t note_kv_schema[] = {
  { NOTE_KV_TABLE,       KV_STR,    "notes",
    "Postgres table name for notes (bare SQL identifier)" },
  { NOTE_KV_MIN_IDLE,    KV_UINT32, "600",
    "Seconds a recipient must have been idle before a note may be left" },
  { NOTE_KV_MAX_BODY,    KV_UINT32, "400",
    "Maximum stored note length in characters" },
  { NOTE_KV_MAX_PENDING, KV_UINT32, "10",
    "Maximum undelivered notes one recipient may accumulate (0 = no cap)" },
};

// ------------------------------------------------------------------ //
// Lifecycle                                                           //
// ------------------------------------------------------------------ //

static bool
note_init(void)
{
  if(note_commands_register() != SUCCESS)
  {
    clam(CLAM_WARN, NOTE_CTX, "command registration failed");
    return(FAIL);
  }

  clam(CLAM_INFO, NOTE_CTX, "note plugin initialized");
  return(SUCCESS);
}

// Schema bootstrap runs in start(), after kv_load(), so the table name
// reflects the persisted KV rather than the register-time default. The
// pending set is loaded from that table before any observer attaches.
static bool
note_start(void)
{
  if(note_schema_ensure() != SUCCESS)
  {
    clam(CLAM_WARN, NOTE_CTX,
        "note schema init failed (leaving notes will error until fixed)");
    return(SUCCESS);
  }

  return(note_deliver_start());
}

static void
note_deinit(void)
{
  note_deliver_stop();
  note_commands_unregister();
  clam(CLAM_INFO, NOTE_CTX, "note plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "note",
  .version              = "1.0",
  .type                 = PLUGIN_FEATURE,
  .kind                 = "note",
  .provides             = { { .name = "feature_note" } },
  .provides_count       = 1,
  .requires             = { { .name = "bot_chat" } },
  .requires_count       = 1,
  .kv_schema            = note_kv_schema,
  .kv_schema_count      = sizeof(note_kv_schema) / sizeof(note_kv_schema[0]),
  .kv_inst_schema       = NULL,
  .kv_inst_schema_count = 0,
  .init                 = note_init,
  .start                = note_start,
  .stop                 = NULL,
  .deinit               = note_deinit,
  .ext                  = NULL,
};
