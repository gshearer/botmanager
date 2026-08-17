// botmanager — MIT
// Per-userns stock symbol lists: the storage behind `!stock @name`.
// Symbols are charset-validated before storage (the set deliberately
// excludes the comma that delimits them) and every user-originated value
// is routed through db_escape before it touches a query; the only
// interpolated identifier is the compile-time table name.

#define STOCK_INTERNAL
#include "stock.h"

#include "db.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ----------------------------------------------------------------------
// Validation
// ----------------------------------------------------------------------

// List names are folded to lowercase by the caller; here we only police
// the charset and the length. '@' is excluded so a name can never be
// mistaken for the reference syntax that introduces it.
bool
stock_list_name_ok(const char *name)
{
  size_t i;

  if(name == NULL || name[0] == '\0')
    return(false);

  for(i = 0; name[i] != '\0'; i++)
  {
    const unsigned char c = (unsigned char)name[i];

    if(i + 1 >= STOCK_LIST_NAME_SZ)
      return(false);

    if(!isalnum(c) && c != '_' && c != '-')
      return(false);
  }

  return(true);
}

// Ticker charset as the provider actually uses it: ^GSPC, BRK.B, BTC-USD,
// EURUSD=X. A comma would corrupt the stored CSV, so it is not here.
bool
stock_list_sym_ok(const char *sym)
{
  size_t i;

  if(sym == NULL || sym[0] == '\0')
    return(false);

  for(i = 0; sym[i] != '\0'; i++)
  {
    const unsigned char c = (unsigned char)sym[i];

    if(i + 1 >= STOCK_SYM_SZ)
      return(false);

    if(!isalnum(c) && c != '^' && c != '-' && c != '.' && c != '=')
      return(false);
  }

  return(true);
}

// ----------------------------------------------------------------------
// Schema
// ----------------------------------------------------------------------

// CREATE TABLE IF NOT EXISTS is idempotent, but the mutex keeps concurrent
// callers from racing the bootstrap and doubling the log noise.
static bool            stock_schema_done = false;
static pthread_mutex_t stock_schema_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
stock_run_ddl(const char *sql)
{
  db_result_t *res = db_result_alloc();
  bool         ok  = SUCCESS;

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, STOCK_CTX, "ddl failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  db_result_free(res);
  return(ok);
}

bool
stock_lists_schema_ensure(void)
{
  char sql[512];
  bool ok = SUCCESS;

  pthread_mutex_lock(&stock_schema_lock);

  if(stock_schema_done)
  {
    pthread_mutex_unlock(&stock_schema_lock);
    return(SUCCESS);
  }

  // No class column: this table is the stock class. A crypto list lives
  // in the crypto plugin's own table and never meets these rows.
  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS %s ("
      " ns_id      INTEGER     NOT NULL,"
      " name       VARCHAR(32) NOT NULL,"
      " symbols    TEXT        NOT NULL,"
      " created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " PRIMARY KEY (ns_id, name)"
      ")", STOCK_LIST_TABLE);

  if(stock_run_ddl(sql) != SUCCESS)
    ok = FAIL;

  if(ok == SUCCESS)
  {
    stock_schema_done = true;
    clam(CLAM_INFO, STOCK_CTX, "list schema ready (table '%s')",
        STOCK_LIST_TABLE);
  }

  pthread_mutex_unlock(&stock_schema_lock);
  return(ok);
}

// ----------------------------------------------------------------------
// CSV <-> symbol set
// ----------------------------------------------------------------------

static void
stock_symset_parse(stock_symset_t *set, const char *csv)
{
  const char *p = csv;

  memset(set, 0, sizeof(*set));

  while(p != NULL && *p != '\0' && set->n < STOCK_LIST_MAX)
  {
    size_t n = 0;

    while(*p == ',' || *p == ' ')
      p++;

    while(*p != '\0' && *p != ',' && n + 1 < STOCK_SYM_SZ)
      set->sym[set->n][n++] = *p++;

    set->sym[set->n][n] = '\0';

    while(*p != '\0' && *p != ',')   // drop an over-long remainder
      p++;

    if(set->sym[set->n][0] != '\0')
      set->n++;
  }
}

static void
stock_symset_join(const stock_symset_t *set, char *out, size_t cap)
{
  size_t used = 0;
  uint8_t i;

  out[0] = '\0';

  for(i = 0; i < set->n; i++)
  {
    const int wrote = snprintf(out + used, cap - used, "%s%s",
        (i > 0) ? "," : "", set->sym[i]);

    if(wrote < 0 || (size_t)wrote >= cap - used)
      break;

    used += (size_t)wrote;
  }
}

static bool
stock_symset_has(const stock_symset_t *set, const char *sym)
{
  uint8_t i;

  for(i = 0; i < set->n; i++)
    if(strcasecmp(set->sym[i], sym) == 0)
      return(true);

  return(false);
}

// ----------------------------------------------------------------------
// Row access
// ----------------------------------------------------------------------

// Reads the stored CSV for one list. NOSUCH when the namespace has no
// list by that name.
static stock_list_rc_t
stock_row_read(uint32_t ns_id, const char *name, stock_symset_t *out)
{
  db_result_t *res    = NULL;
  char        *e_name = NULL;
  char         sql[512];
  stock_list_rc_t rc  = STOCK_LIST_ERR;

  memset(out, 0, sizeof(*out));

  e_name = db_escape(name);

  if(e_name == NULL)
    return(STOCK_LIST_ERR);

  snprintf(sql, sizeof(sql),
      "SELECT symbols FROM %s WHERE ns_id = %" PRIu32 " AND name = '%s'",
      STOCK_LIST_TABLE, ns_id, e_name);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    if(res->rows == 1)
    {
      const char *csv = db_result_get(res, 0, 0);

      stock_symset_parse(out, (csv != NULL) ? csv : "");
      rc = STOCK_LIST_OK;
    }

    else
      rc = STOCK_LIST_NOSUCH;
  }

  else
    clam(CLAM_WARN, STOCK_CTX, "list read failed: %s",
        (res->error[0] != '\0')
            ? res->error : "(no driver error)");

  db_result_free(res);
  mem_free(e_name);

  return(rc);
}

static stock_list_rc_t
stock_row_write(uint32_t ns_id, const char *name, const stock_symset_t *set)
{
  db_result_t *res    = NULL;
  char        *e_name = NULL;
  char        *e_csv  = NULL;
  char         csv[STOCK_LIST_CSV_SZ];
  char         sql[1024];
  stock_list_rc_t rc  = STOCK_LIST_ERR;

  stock_symset_join(set, csv, sizeof(csv));

  e_name = db_escape(name);
  e_csv  = db_escape(csv);

  if(e_name == NULL || e_csv == NULL)
    goto out;

  snprintf(sql, sizeof(sql),
      "INSERT INTO %s (ns_id, name, symbols)"
      " VALUES (%" PRIu32 ", '%s', '%s')"
      " ON CONFLICT (ns_id, name) DO UPDATE SET symbols = EXCLUDED.symbols",
      STOCK_LIST_TABLE, ns_id, e_name, e_csv);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
    rc = STOCK_LIST_OK;

  else
    clam(CLAM_WARN, STOCK_CTX, "list write failed: %s",
        (res->error[0] != '\0')
            ? res->error : "(no driver error)");

out:
  db_result_free(res);
  if(e_name != NULL) mem_free(e_name);
  if(e_csv  != NULL) mem_free(e_csv);

  return(rc);
}

static stock_list_rc_t
stock_row_drop(uint32_t ns_id, const char *name)
{
  db_result_t *res    = NULL;
  char        *e_name = NULL;
  char         sql[512];
  stock_list_rc_t rc  = STOCK_LIST_ERR;

  e_name = db_escape(name);

  if(e_name == NULL)
    return(STOCK_LIST_ERR);

  snprintf(sql, sizeof(sql),
      "DELETE FROM %s WHERE ns_id = %" PRIu32 " AND name = '%s'",
      STOCK_LIST_TABLE, ns_id, e_name);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
    rc = STOCK_LIST_OK;

  else
    clam(CLAM_WARN, STOCK_CTX, "list drop failed: %s",
        (res->error[0] != '\0')
            ? res->error : "(no driver error)");

  db_result_free(res);
  mem_free(e_name);

  return(rc);
}

// ----------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------

stock_list_rc_t
stock_lists_get(uint32_t ns_id, const char *name, stock_symset_t *out)
{
  if(!stock_list_name_ok(name))
    return(STOCK_LIST_BADNAME);

  if(stock_lists_schema_ensure() != SUCCESS)
    return(STOCK_LIST_ERR);

  return(stock_row_read(ns_id, name, out));
}

// Create-or-append. Duplicates are ignored rather than rejected, so
// re-adding a symbol is harmless; the cap is checked against the merged
// set so a partial add never silently truncates.
stock_list_rc_t
stock_lists_add(uint32_t ns_id, const char *name, const stock_item_t *syms,
    uint8_t n, uint8_t *added, uint8_t *total)
{
  stock_symset_t  set;
  stock_list_rc_t rc;
  uint8_t         i;

  *added = 0;
  *total = 0;

  if(!stock_list_name_ok(name))
    return(STOCK_LIST_BADNAME);

  for(i = 0; i < n; i++)
    if(!stock_list_sym_ok(syms[i].text))
      return(STOCK_LIST_BADSYM);

  if(stock_lists_schema_ensure() != SUCCESS)
    return(STOCK_LIST_ERR);

  rc = stock_row_read(ns_id, name, &set);

  if(rc != STOCK_LIST_OK && rc != STOCK_LIST_NOSUCH)
    return(rc);

  // The whole batch is rejected on overflow rather than partly applied,
  // so *total must report what is stored — not how far the merge got.
  *total = set.n;

  for(i = 0; i < n; i++)
  {
    if(stock_symset_has(&set, syms[i].text))
      continue;

    if(set.n >= STOCK_LIST_MAX)
    {
      *added = 0;
      return(STOCK_LIST_FULL);
    }

    snprintf(set.sym[set.n], STOCK_SYM_SZ, "%s", syms[i].text);
    set.n++;
    (*added)++;
  }

  *total = set.n;

  if(*added == 0)
    return(STOCK_LIST_OK);   // nothing changed; skip the write

  return(stock_row_write(ns_id, name, &set));
}

// Removes members. When the last symbol goes the row goes with it, which
// is why there is no separate delete-list verb.
stock_list_rc_t
stock_lists_del(uint32_t ns_id, const char *name, const stock_item_t *syms,
    uint8_t n, uint8_t *removed, bool *dropped)
{
  stock_symset_t  set;
  stock_symset_t  keep;
  stock_list_rc_t rc;
  uint8_t         i;
  uint8_t         j;

  *removed = 0;
  *dropped = false;

  if(!stock_list_name_ok(name))
    return(STOCK_LIST_BADNAME);

  if(stock_lists_schema_ensure() != SUCCESS)
    return(STOCK_LIST_ERR);

  rc = stock_row_read(ns_id, name, &set);

  if(rc != STOCK_LIST_OK)
    return(rc);

  memset(&keep, 0, sizeof(keep));

  for(i = 0; i < set.n; i++)
  {
    bool drop = false;

    for(j = 0; j < n && !drop; j++)
      if(strcasecmp(set.sym[i], syms[j].text) == 0)
        drop = true;

    if(drop)
    {
      (*removed)++;
      continue;
    }

    snprintf(keep.sym[keep.n], STOCK_SYM_SZ, "%s", set.sym[i]);
    keep.n++;
  }

  if(*removed == 0)
    return(STOCK_LIST_OK);   // nothing matched; caller reports it

  if(keep.n == 0)
  {
    *dropped = true;
    return(stock_row_drop(ns_id, name));
  }

  return(stock_row_write(ns_id, name, &keep));
}

stock_list_rc_t
stock_lists_names(uint32_t ns_id, char *out, size_t cap, uint32_t *count)
{
  db_result_t    *res  = NULL;
  char            sql[512];
  size_t          used = 0;
  stock_list_rc_t rc   = STOCK_LIST_ERR;

  out[0] = '\0';
  *count = 0;

  if(stock_lists_schema_ensure() != SUCCESS)
    return(STOCK_LIST_ERR);

  snprintf(sql, sizeof(sql),
      "SELECT name, symbols FROM %s WHERE ns_id = %" PRIu32 " ORDER BY name",
      STOCK_LIST_TABLE, ns_id);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    uint32_t r;

    for(r = 0; r < res->rows; r++)
    {
      const char    *name = db_result_get(res, r, 0);
      const char    *csv  = db_result_get(res, r, 1);
      stock_symset_t set;
      int            wrote;

      if(name == NULL)
        continue;

      stock_symset_parse(&set, (csv != NULL) ? csv : "");

      wrote = snprintf(out + used, cap - used, "%s%s (%u)",
          (used > 0) ? ", " : "", name, (unsigned)set.n);

      if(wrote < 0 || (size_t)wrote >= cap - used)
        break;

      used += (size_t)wrote;
      (*count)++;
    }

    rc = STOCK_LIST_OK;
  }

  else
    clam(CLAM_WARN, STOCK_CTX, "list enumerate failed: %s",
        (res->error[0] != '\0')
            ? res->error : "(no driver error)");

  db_result_free(res);
  return(rc);
}
