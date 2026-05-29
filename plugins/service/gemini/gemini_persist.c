// botmanager — MIT
// Gemini symbol-cache persistence (EXCH-PRIME-1).
//
// Mirrors the in-memory gemini_pairs cache to the `gemini_symbols`
// Postgres table and primes it back on startup. This replaces the
// synchronous network prime that gem_start used to run inside the
// plugin start() path — the operator control socket no longer waits on
// Gemini's N+1 /v1/symbols fan-out. The network refresh fires only when
// the persisted snapshot is missing or older than the staleness window
// (plugin.gemini.symbols_refresh_sec).
#define GEM_INTERNAL
#include "gemini.h"

#include "gemini_persist.h"

#include "db.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------
// Schema
// ------------------------------------------------------------------

// Canonical DDL lives in scripts/schema.sql; this mirror keeps the
// runtime self-bootstrapping (CREATE TABLE IF NOT EXISTS). `abstr` is
// not stored — gem_pairs_add re-derives it from base/quote on load.
static const char gem_symbols_ddl[] =
  "CREATE TABLE IF NOT EXISTS gemini_symbols ("
  " native     VARCHAR(16)  PRIMARY KEY,"
  " base       VARCHAR(8)   NOT NULL,"
  " quote      VARCHAR(8)   NOT NULL,"
  " fetched_at TIMESTAMPTZ  NOT NULL DEFAULT NOW()"
  ")";

// Per-daemon DDL guard. CREATE TABLE IF NOT EXISTS is idempotent at the
// Postgres level, so the latch only suppresses log noise when many bots
// race a DDL batch at boot (mirrors wm_dl_schema_ensured / _lock).
static bool             gem_symbols_ensured     = false;
static pthread_mutex_t  gem_symbols_ensure_lock = PTHREAD_MUTEX_INITIALIZER;

// Initial SQL builder capacity. GEM_SYMS_CAP (1024) rows at ~30 bytes
// each fits comfortably; the grow-buffer doubles if a larger listing
// arrives.
#define GEM_PERSIST_SQL_CAP  8192

// ------------------------------------------------------------------
// Table bootstrap
// ------------------------------------------------------------------

bool
gem_symbols_ensure_table(void)
{
  db_result_t *res;
  bool         ok = SUCCESS;

  pthread_mutex_lock(&gem_symbols_ensure_lock);

  if(gem_symbols_ensured)
  {
    pthread_mutex_unlock(&gem_symbols_ensure_lock);
    return(SUCCESS);
  }

  res = db_result_alloc();

  if(res == NULL)
  {
    pthread_mutex_unlock(&gem_symbols_ensure_lock);
    return(FAIL);
  }

  if(db_query(gem_symbols_ddl, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, GEM_CTX, "symbols: ensure table failed: %s",
        res->error[0] != '\0' ? res->error : "(no driver error)");
    ok = FAIL;
  }

  else
    gem_symbols_ensured = true;

  db_result_free(res);
  pthread_mutex_unlock(&gem_symbols_ensure_lock);

  return(ok);
}

// ------------------------------------------------------------------
// Persist (cache → DB)
// ------------------------------------------------------------------

bool
gem_symbols_persist(void)
{
  gemini_pair_t *snap;
  db_result_t   *dbres;
  char          *sql;
  size_t         cap = GEM_PERSIST_SQL_CAP;
  size_t         len = 0;
  uint32_t       n;
  uint32_t       i;
  int            n_w;
  bool           ok = FAIL;

  snap = mem_alloc(GEM_CTX, "persist.snap",
      (size_t)GEM_SYMS_CAP * sizeof(*snap));

  if(snap == NULL)
    return(FAIL);

  n = gem_pairs_snapshot(snap, GEM_SYMS_CAP);

  sql = mem_alloc(GEM_CTX, "persist.sql", cap);

  if(sql == NULL)
  {
    mem_free(snap);
    return(FAIL);
  }

  // Single transaction: clear the table, then bulk-insert the snapshot.
  // An empty cache still emits BEGIN; DELETE; COMMIT; to clear stale
  // rows.
  len += (size_t)snprintf(sql + len, cap - len,
      "BEGIN; DELETE FROM gemini_symbols;");

  if(n > 0)
    len += (size_t)snprintf(sql + len, cap - len,
        " INSERT INTO gemini_symbols (native, base, quote) VALUES ");

  for(i = 0; i < n; i++)
  {
    if(cap - len < 128)
    {
      size_t new_cap = cap * 2;
      char  *new_sql = mem_realloc(sql, new_cap);

      if(new_sql == NULL)
      {
        mem_free(sql);
        mem_free(snap);
        return(FAIL);
      }

      sql = new_sql;
      cap = new_cap;
    }

    // native/base/quote are constrained tokens — lowercase alnum and
    // uppercase ISO codes written by gem_pairs_add — so no quote
    // characters are possible and a direct single-quoted embed is
    // injection-safe.
    n_w = snprintf(sql + len, cap - len, "%s('%s','%s','%s')",
        i > 0 ? "," : "", snap[i].native, snap[i].base, snap[i].quote);

    if(n_w < 0)
    {
      mem_free(sql);
      mem_free(snap);
      return(FAIL);
    }

    len += (size_t)n_w;
  }

  mem_free(snap);

  if(cap - len < 64)
  {
    size_t new_cap = cap + 64;
    char  *new_sql = mem_realloc(sql, new_cap);

    if(new_sql == NULL)
    {
      mem_free(sql);
      return(FAIL);
    }

    sql = new_sql;
    cap = new_cap;
  }

  // Terminate the INSERT (only emitted when rows were appended), then
  // close the transaction. ON CONFLICT DO NOTHING makes the batch
  // robust to a transient duplicate `native` in the snapshot (e.g. two
  // refresh fan-outs racing on the shared cache) — Postgres skips the
  // dup rather than aborting the whole persist; first occurrence wins.
  len += (size_t)snprintf(sql + len, cap - len, "%s COMMIT;",
      n > 0 ? " ON CONFLICT (native) DO NOTHING;" : "");

  dbres = db_result_alloc();

  if(dbres == NULL)
  {
    mem_free(sql);
    return(FAIL);
  }

  if(db_query(sql, dbres) == SUCCESS && dbres->ok)
  {
    ok = SUCCESS;
    clam(CLAM_INFO, GEM_CTX, "symbols: persisted %u rows", n);
  }

  else
    clam(CLAM_WARN, GEM_CTX, "symbols: persist failed: %s",
        dbres->error[0] != '\0' ? dbres->error : "(no driver error)");

  db_result_free(dbres);
  mem_free(sql);

  return(ok);
}

// ------------------------------------------------------------------
// Prime (DB → cache, refresh-on-stale)
// ------------------------------------------------------------------

// Async load result handler. Takes ownership of `res` (db_cb_t
// contract). Repopulates the in-memory cache from a fresh snapshot;
// fires a background network refresh when the snapshot is missing,
// failed, or older than the staleness window.
static void
gem_symbols_load_cb(db_result_t *res, void *data)
{
  uint32_t refresh_sec;
  int64_t  max_age = 0;
  int64_t  age;
  uint32_t n       = 0;
  uint32_t i;

  (void)data;

  if(res == NULL || !res->ok || res->rows == 0)
  {
    db_result_free(res);
    (void)gemini_symbols_refresh_async(gem_symbols_refresh_persist_cb, NULL);
    return;
  }

  // Cols: 0=native 1=base 2=quote 3=age_sec. Take the oldest row's age
  // as the snapshot age.
  for(i = 0; i < res->rows; i++)
  {
    const char *age_s = db_result_get(res, i, 3);

    if(age_s != NULL)
    {
      age = (int64_t)strtoll(age_s, NULL, 10);

      if(age > max_age)
        max_age = age;
    }
  }

  refresh_sec = (uint32_t)kv_get_uint("plugin.gemini.symbols_refresh_sec");

  // refresh_sec == 0 disables the periodic refresh; treat any existing
  // snapshot as fresh regardless of age (only an empty table refreshes).
  if(refresh_sec > 0 && max_age >= (int64_t)refresh_sec)
  {
    db_result_free(res);
    clam(CLAM_INFO, GEM_CTX,
        "symbols: cache stale (age %llds >= %u s); refreshing",
        (long long)max_age, refresh_sec);
    (void)gemini_symbols_refresh_async(gem_symbols_refresh_persist_cb, NULL);
    return;
  }

  gem_pairs_clear();

  for(i = 0; i < res->rows; i++)
  {
    const char *native = db_result_get(res, i, 0);
    const char *base   = db_result_get(res, i, 1);
    const char *quote  = db_result_get(res, i, 2);

    if(native != NULL && base != NULL && quote != NULL
        && gem_pairs_add(native, base, quote) == SUCCESS)
      n++;
  }

  db_result_free(res);

  clam(CLAM_INFO, GEM_CTX,
      "symbols: loaded %u from cache (age %llds)", n, (long long)max_age);
}

void
gem_symbols_load_or_refresh_async(void)
{
  if(db_query_async(
        "SELECT native, base, quote,"
        " EXTRACT(EPOCH FROM (NOW() - fetched_at))::bigint AS age_sec"
        " FROM gemini_symbols",
        gem_symbols_load_cb, NULL) != SUCCESS)
  {
    // DB unreachable / submit failed → never leave the cache empty;
    // fall through to a direct network refresh.
    clam(CLAM_WARN, GEM_CTX,
        "symbols: cache load submit failed; refreshing from network");
    (void)gemini_symbols_refresh_async(gem_symbols_refresh_persist_cb, NULL);
  }
}

void
gem_symbols_refresh_persist_cb(const gemini_symbols_result_t *res, void *user)
{
  (void)user;

  if(res != NULL && res->err[0] == '\0')
  {
    (void)gem_symbols_persist();
    return;
  }

  // Refresh failed — leave any prior cache (in-memory and DB) intact.
  clam(CLAM_WARN, GEM_CTX, "symbols: refresh failed: %s",
      (res != NULL && res->err[0] != '\0') ? res->err : "(no error string)");
}
