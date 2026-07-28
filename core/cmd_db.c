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
//   /db orphans              usage
//   /db orphans kv           list persisted KV rows no live entry claims
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
  cmd_reply(ctx, "usage: /db <subcommand> ...  (delete, orphans)");
}

static void
cmd_db_delete(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /db delete <what> ...  (kv <key>)");
}

static void
cmd_db_orphans(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /db orphans <what>  (kv)");
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
// /db orphans kv
// -----------------------------------------------------------------------

// Cap on listed rows, so a pathological table cannot flood a channel. The
// count in the summary is the true total either way.
#define DB_ORPHAN_LIST_MAX  200

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         listed;
} db_orphan_state_t;

static void
db_orphan_cb(const char *key, kv_type_t type, const char *value, void *data)
{
  db_orphan_state_t *st = data;
  const char        *shown = value;
  char               line[KV_KEY_SZ + KV_STR_SZ + 32];

  st->listed++;

  if(st->listed > DB_ORPHAN_LIST_MAX)
    return;

  if(kv_is_secret_key(key) && !kv_admin_context_active())
    shown = KV_REDACTED_VALUE;

  snprintf(line, sizeof(line), "  %s = %s (%s)",
      key, shown, kv_type_name(type));
  cmd_reply(st->ctx, line);
}

// List the persisted KV rows that no live registry entry claims. Purely a
// report: an unloaded plugin's keys are orphans for as long as it is out,
// and they are exactly the rows that must survive so a reload costs no
// reconfiguration. Deciding one is truly dead is the operator's call, and
// /db delete kv is where that decision is executed.
static void
cmd_db_orphans_kv(const cmd_ctx_t *ctx)
{
  db_orphan_state_t st;
  uint32_t          total;
  char              buf[160];

  memset(&st, 0, sizeof(st));
  st.ctx = ctx;

  cmd_reply(ctx, "orphaned kv rows (persisted, no live entry):");
  total = kv_iterate_orphans(db_orphan_cb, &st);

  if(total == 0)
  {
    cmd_reply(ctx, "  none — every persisted row is claimed");
    return;
  }

  if(total > DB_ORPHAN_LIST_MAX)
  {
    snprintf(buf, sizeof(buf), "  ... %u more not shown",
        total - DB_ORPHAN_LIST_MAX);
    cmd_reply(ctx, buf);
  }

  snprintf(buf, sizeof(buf),
      "%u orphan(s) — a row is kept until you drop it: /db delete kv <key>",
      total);
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

  cmd_register("cmd", "orphans",
      "db orphans <what>",
      "Report persisted state nothing claims (kv, ...)",
      NULL,
      USERNS_GROUP_ADMIN, DB_CMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_db_orphans, NULL, "db", NULL, NULL, 0, NULL, NULL);

  cmd_register("cmd", "kv",
      "db orphans kv",
      "List persisted KV rows no live entry claims",
      "Every configuration key a plugin registers is a live binding over a\n"
      "durable database row. Unloading the plugin drops the binding and\n"
      "keeps the row — that is what makes a reload cost no reconfiguration\n"
      "— so its keys appear here until it loads again.\n"
      "\n"
      "A row also lands here when a schema change retires the key. Core\n"
      "cannot tell the two apart and never prunes on its own; when you are\n"
      "sure a key is retired, drop it with /db delete kv <key>.",
      USERNS_GROUP_ADMIN, DB_CMD_LEVEL, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_db_orphans_kv, NULL, "db/orphans", NULL, NULL, 0, NULL, NULL);
}
