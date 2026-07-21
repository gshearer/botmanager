// botmanager — MIT
// /db — admin janitoring subsystem.
//
// A small, deliberately privileged (level 1000) command surface for the
// housekeeping that has no home elsewhere: sweeping orphaned KV keys left
// behind by a schema change, and whatever maintenance verbs we add next.
// The tree mirrors the /set and /show containers — a usage-only parent
// with leaf verbs beneath it:
//
//   /db                      usage
//   /db delete               usage
//   /db delete kv <key>      drop one KV key from memory AND its DB row
//
// Everything here is destructive by intent and admin-gated accordingly.

#include "common.h"
#include "cmd.h"
#include "kv.h"
#include "userns.h"

#include <stdio.h>

// Minimum privilege level for every /db verb. Higher than the /set and
// /show containers (100): janitoring can delete persisted state outright.
#define DB_CMD_LEVEL  1000

// -----------------------------------------------------------------------
// Validators
// -----------------------------------------------------------------------

// A KV key as accepted elsewhere in the system: alphanumerics plus the
// path punctuation '.', '_', '-' and the market-instance separator '@'
// (see cmd_set.c validate_kv_key for the '@' rationale). We validate so a
// typo can't be interpreted as a wildcard or injected into the DELETE.
static bool
db_validate_kv_key(const char *str)
{
  if(str == NULL || str[0] == '\0')
    return(false);

  for(const char *k = str; *k != '\0'; k++)
  {
    unsigned char ch = (unsigned char)*k;

    if(!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
         ch == '-' || ch == '@'))
      return(false);
  }

  return(true);
}

// -----------------------------------------------------------------------
// Argument descriptors
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_db_delete_kv[] = {
  { "key", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, KV_KEY_SZ, db_validate_kv_key },
};

// -----------------------------------------------------------------------
// Container stubs — invoked when a bare parent is typed with no verb.
// -----------------------------------------------------------------------

static void
cmd_db(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /db <subcommand> ...  (delete)");
}

static void
cmd_db_delete(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /db delete <what> ...  (kv <key>)");
}

// -----------------------------------------------------------------------
// /db delete kv <key>
// -----------------------------------------------------------------------

// Drop a single KV key from the live registry and its persisted row. This
// is the escape hatch for orphans a schema change stranded: keys no longer
// in any plugin's schema linger in memory (loaded at startup) and in the
// DB, and nothing else evicts them short of a restart.
static void
cmd_db_delete_kv(const cmd_ctx_t *ctx)
{
  const char *key = ctx->parsed->argv[0];
  char        buf[KV_KEY_SZ + 96];

  if(kv_delete(key))
    snprintf(buf, sizeof(buf),
        "deleted kv '%s' — evicted from memory and dropped its DB row", key);
  else
    snprintf(buf, sizeof(buf),
        "no live kv '%s' — dropped any persisted row (nothing in memory)",
        key);

  cmd_reply(ctx, buf);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

void
cmd_db_register(void)
{
  cmd_register("cmd", "db",
      "db <subcommand> ...",
      "Admin janitoring (delete kv, ...)",
      NULL,
      USERNS_GROUP_ADMIN, DB_CMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_db, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  cmd_register("cmd", "delete",
      "db delete <what> ...",
      "Delete persisted state (kv, ...)",
      NULL,
      USERNS_GROUP_ADMIN, DB_CMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_db_delete, NULL, "db", NULL, NULL, 0, NULL, NULL);

  cmd_register("cmd", "kv",
      "db delete kv <key>",
      "Drop one KV key from memory and the database",
      "Removes exactly one configuration key — whole-key match, never a\n"
      "prefix — from the live registry and its persisted row. Use it to\n"
      "sweep orphans a schema change stranded (keys no longer owned by any\n"
      "plugin), which otherwise linger in memory until the next restart.\n"
      "\n"
      "Example:\n"
      "  /db delete kv plugin.ask.system",
      USERNS_GROUP_ADMIN, DB_CMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_db_delete_kv, NULL, "db/delete", NULL,
      ad_db_delete_kv, 1, NULL, NULL);
}
