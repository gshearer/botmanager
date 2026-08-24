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
//   /db orphans userns       list rows keyed to a userns id that is gone
//
// Everything here is destructive by intent and admin-gated accordingly.

#include "common.h"
#include "cmd.h"
#include "db.h"
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

// A table name read back out of the catalog is ours, but it still reaches
// a statement by interpolation — SQL has no bind parameter for an
// identifier. Anything outside the unquoted-identifier set is refused
// rather than quoted, because no table in this tree needs quoting and a
// name that does is a better thing to report than to run.
static bool
db_validate_table_name(const char *str)
{
  if(str == NULL || str[0] == '\0')
    return(false);

  for(const char *t = str; *t != '\0'; t++)
  {
    unsigned char ch = (unsigned char)*t;

    if(!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '_'))
      return(false);
  }

  return(true);
}

// -----------------------------------------------------------------------
// Argument descriptors
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_db_delete_kv[] = {
  { "key", CMD_ARG_CUSTOM, CMD_ARG_REQUIRED, KV_KEY_SZ - 1, db_validate_kv_key },
};

// -----------------------------------------------------------------------
// Container stubs — invoked when a bare parent is typed with no verb.
// -----------------------------------------------------------------------

static void
cmd_db(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: db <subcommand> ...  (delete, orphans)");
}

static void
cmd_db_delete(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: db delete <what> ...  (kv <key>)");
}

static void
cmd_db_orphans(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: db orphans <what>  (kv, userns)");
}

// -----------------------------------------------------------------------
// /db delete kv <key>
// -----------------------------------------------------------------------

// Drop a single KV key from the live registry and its persisted row. This
// is the escape hatch for orphans a schema change stranded: keys no longer
// in any plugin's schema linger in memory (loaded at startup) and in the
// DB, and nothing else evicts them short of a restart.
//
// ⛔ Not the same operation as `set kv --delete <key>`, and neither
// replaces the other (OBS-50). This one retires an orphaned ROW and
// unregisters whatever claimed it — which is why it must not be pointed
// at a live knob: a key that is not registered is indistinguishable from
// one that never existed, so every read silently takes its fallback.
// `set kv --delete` retires a registered key's stored VALUE, reverting it
// to its declaration with the entry left in place. And this one is still
// the only tool for a DB-only orphan, because kv_reset cannot see a row
// with no live entry.
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
      "%u orphan(s) — a row is kept until you drop it: db delete kv <key>",
      total);
  cmd_reply(ctx, buf);
}

// -----------------------------------------------------------------------
// /db orphans userns
// -----------------------------------------------------------------------

// Postgres caps an identifier at NAMEDATALEN-1; nothing here needs more.
#define DB_TABLE_NAME_SZ  64
#define DB_ORPHAN_SQL_SZ  320

// Every table that keys rows by user namespace carries an `ns_id`, and a
// userns id is not stable across a wipe: bm-wipe.sh drops and recreates
// `userns`, so the ids move underneath everything that stored one. The
// 2026-08-18 wipe moved drow from 4 to 2 and silently stranded 1,771
// quotes that way.
//
// A foreign key is the obvious guard and it does not survive the event it
// guards against: DROP TABLE userns CASCADE removes the constraints that
// point AT userns while leaving the referencing tables in place, after
// which CREATE TABLE IF NOT EXISTS is a no-op and never puts them back.
// Measured 2026-08-20 — four chat tables declare the reference in their
// DDL and carry no constraint in the live database.
//
// So this derives its answer from the rows rather than from a constraint,
// and it finds its own subjects: the catalog names every table with an
// `ns_id` column. A plugin's new table is covered the day it is created,
// core is told nothing about any plugin, and there is no list anywhere to
// fall out of date — which is the whole reason to prefer it.
static const char db_orphan_ns_tables_sql[] =
    "SELECT c.relname,"
    " EXISTS (SELECT 1 FROM pg_constraint k"
    "         WHERE k.conrelid = c.oid AND k.contype = 'f'"
    "         AND a.attnum = ANY(k.conkey)) AS has_fk"
    " FROM pg_class c"
    " JOIN pg_namespace n ON n.oid = c.relnamespace"
    " AND n.nspname = 'public'"
    " JOIN pg_attribute a ON a.attrelid = c.oid"
    " AND a.attname = 'ns_id' AND a.attnum > 0 AND NOT a.attisdropped"
    " WHERE c.relkind = 'r'"
    " ORDER BY c.relname";

// Count one table's stranded rows and name the namespaces they point at.
// The id is what identifies the generation — `ns 4` says which wipe left
// them — so it is worth a column rather than a follow-up query.
static bool
db_orphan_scan_table(const char *table, int64_t *out_rows, char *out_ns,
    size_t out_ns_cap)
{
  db_result_t *res = db_result_alloc();
  char         sql[DB_ORPHAN_SQL_SZ];
  bool         rc  = FAIL;

  snprintf(sql, sizeof(sql),
      "SELECT count(*),"
      " COALESCE(string_agg(DISTINCT t.ns_id::text, ','), '')"
      " FROM public.%s t"
      " WHERE NOT EXISTS (SELECT 1 FROM userns u WHERE u.id = t.ns_id)",
      table);

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    *out_rows = db_result_get_i64(res, 0, 0, 0);
    db_result_copy(out_ns, out_ns_cap, res, 0, 1);
    rc = SUCCESS;
  }

  db_result_free(res);
  return(rc);
}

// Report rows whose namespace is gone. A report only: whether a stranded
// row is repaired onto the surviving namespace or dropped depends on what
// the rows mean, which is the operator's call and not a thing core can
// infer from a count.
static void
cmd_db_orphans_userns(const cmd_ctx_t *ctx)
{
  db_result_t *tables = db_result_alloc();
  char         table[DB_TABLE_NAME_SZ];
  char         has_fk[4];
  char         nslist[96];
  char         line[192];
  int64_t      rows;
  int64_t      total     = 0;
  uint32_t     affected  = 0;
  uint32_t     unguarded = 0;
  uint32_t     i;

  if(db_query(db_orphan_ns_tables_sql, tables) != SUCCESS || !tables->ok)
  {
    cmd_reply(ctx, "cannot read the table catalog — is the database up?");
    db_result_free(tables);
    return;
  }

  cmd_reply(ctx, "rows keyed to a userns id that no longer exists:");

  for(i = 0; i < tables->rows; i++)
  {
    db_result_copy(table, sizeof(table), tables, i, 0);
    db_result_copy(has_fk, sizeof(has_fk), tables, i, 1);

    if(!db_validate_table_name(table))
    {
      cmd_reply(ctx, "  (skipped a table whose name needs quoting)");
      continue;
    }

    // Postgres renders a boolean as "t" / "f".
    if(has_fk[0] != 't')
      unguarded++;

    if(db_orphan_scan_table(table, &rows, nslist, sizeof(nslist)) != SUCCESS)
    {
      snprintf(line, sizeof(line), "  %-22s  unreadable", table);
      cmd_reply(ctx, line);
      continue;
    }

    if(rows == 0)
      continue;

    affected++;
    total += rows;

    snprintf(line, sizeof(line), "  %-22s %7lld  under ns %s%s",
        table, (long long)rows, nslist,
        has_fk[0] == 't' ? "" : "  (no FK)");
    cmd_reply(ctx, line);
  }

  if(total == 0)
    cmd_reply(ctx, "  none — every ns_id resolves to a live namespace");

  else
  {
    snprintf(line, sizeof(line),
        "%lld orphan(s) across %u table(s) — repair onto the surviving"
        " namespace or drop them; neither is core's call",
        (long long)total, affected);
    cmd_reply(ctx, line);
  }

  // Worth saying even when nothing is stranded: an unguarded table is one
  // the next wipe can strand again, and that is the actionable half.
  if(unguarded > 0)
  {
    snprintf(line, sizeof(line),
        "%u of %u ns_id table(s) carry no foreign key to userns",
        unguarded, tables->rows);
    cmd_reply(ctx, line);
  }

  db_result_free(tables);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static const cmd_decl_t db_decl = {
  .module      = "cmd",
  .name        = "db",
  .usage       = "db <subcommand> ...",
  .description = "Admin janitoring (delete kv, ...)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = DB_CMD_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_db,
};

static const cmd_decl_t db_delete_decl = {
  .module      = "cmd",
  .name        = "delete",
  .usage       = "db delete <what> ...",
  .description = "Delete persisted state (kv, ...)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = DB_CMD_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_db_delete,
  .parent_path = "db",
};

static const cmd_decl_t db_delete_kv_decl = {
  .module      = "cmd",
  .name        = "kv",
  .usage       = "db delete kv <key>",
  .description = "Drop one KV key from memory and the database",
  .help_long   =
      "Removes exactly one configuration key — whole-key match, never a\n"
      "prefix — from the live registry and its persisted row. Use it to\n"
      "sweep orphans a schema change stranded (keys no longer owned by any\n"
      "plugin), which otherwise linger in memory until the next restart.\n"
      "\n"
      "This retires an orphaned ROW and unregisters what claimed it. To\n"
      "retire a registered key's stored VALUE — reverting it to its\n"
      "declared default with the key still registered — use\n"
      "/set kv --delete <key> instead. Two operations, not one.\n"
      "\n"
      "Example:\n"
      "  db delete kv plugin.ask.system",
  .group       = USERNS_GROUP_ADMIN,
  .level       = DB_CMD_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_db_delete_kv,
  .parent_path = "db/delete",
  .arg_desc    = ad_db_delete_kv,
  .arg_count   = 1,
};

static const cmd_decl_t db_orphans_decl = {
  .module      = "cmd",
  .name        = "orphans",
  .usage       = "db orphans <what>",
  .description = "Report persisted state nothing claims (kv, userns)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = DB_CMD_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_db_orphans,
  .parent_path = "db",
};

static const cmd_decl_t db_orphans_kv_decl = {
  .module      = "cmd",
  .name        = "kv",
  .usage       = "db orphans kv",
  .description = "List persisted KV rows no live entry claims",
  .help_long   =
      "Every configuration key a plugin registers is a live binding over a\n"
      "durable database row. Unloading the plugin drops the binding and\n"
      "keeps the row — that is what makes a reload cost no reconfiguration\n"
      "— so its keys appear here until it loads again.\n"
      "\n"
      "A row also lands here when a schema change retires the key. Core\n"
      "cannot tell the two apart and never prunes on its own; when you are\n"
      "sure a key is retired, drop it with db delete kv <key>.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = DB_CMD_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_db_orphans_kv,
  .parent_path = "db/orphans",
};

static const cmd_decl_t db_orphans_userns_decl = {
  .module      = "cmd",
  .name        = "userns",
  .usage       = "db orphans userns",
  .description = "List rows keyed to a userns id that no longer exists",
  .help_long   =
      "A user namespace id is not stable. scripts/bm-wipe.sh drops it and\n"
      "the daemon recreates it, so every id can move, and rows elsewhere\n"
      "that stored one are then pointing at nothing. Nothing raises: reads\n"
      "are scoped by ns_id, so a stranded row is simply never selected\n"
      "again and the surface it fed goes quiet.\n"
      "\n"
      "A foreign key does not prevent this, which is the surprising part.\n"
      "DROP TABLE userns CASCADE removes the constraints pointing at it\n"
      "and leaves the referencing tables standing; CREATE TABLE IF NOT\n"
      "EXISTS then finds the table present and never restores them.\n"
      "\n"
      "This reads the rows instead, over every table the catalog says has\n"
      "an ns_id column — so a plugin's new table needs no registration\n"
      "here. Run it after any wipe+restore.",
  .group       = USERNS_GROUP_ADMIN,
  .level       = DB_CMD_LEVEL,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_db_orphans_userns,
  .parent_path = "db/orphans",
};

void
cmd_db_register(void)
{
  cmd_register(&db_decl);
  cmd_register(&db_delete_decl);
  cmd_register(&db_delete_kv_decl);
  cmd_register(&db_orphans_decl);
  cmd_register(&db_orphans_kv_decl);
  cmd_register(&db_orphans_userns_decl);
}
