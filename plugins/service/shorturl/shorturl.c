// botmanager — MIT
// shorturl: mints short links into the table an off-host FastCGI daemon serves.
//
// Pure mechanism — no command surface, because searxng_cmd requires this
// plugin and a non-leaf service may not register one (PLUGIN.md §Layer Rules,
// Hard Rule 1). Consumers call su_shorten via plugin_dlsym; see shorturl_api.h.
//
// ⚠ Two bool conventions meet in this file and they are OPPOSITES.
// common.h defines SUCCESS as false, so botmanager's own calls test
// `!= SUCCESS`. The three functions ingested from web/shorturl/src —
// token_generate, token_parse, url_valid — are predicates that return true
// for the good outcome, exactly as the daemon calls them, and are tested with
// `!`. Never put one of those behind `!= SUCCESS`: it compiles, it is silent,
// and it means the reverse.
#define SU_INTERNAL
#include "shorturl.h"

#include "src/config.h"
#include "src/sql.h"
#include "src/url.h"

#include "clam.h"
#include "common.h"
#include "db.h"
#include "kv.h"
#include "plugin.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

// KV schema

static const plugin_kv_entry_t su_kv_schema[] = {
  { SU_KV_BASE_URL, KV_STR,    "https://lame.org/p0ada",
    "Public prefix a short link is built on ('?' and the token are appended)" },
  { SU_KV_ENABLED,  KV_BOOL,   "true",
    "Mint short links at all; off renders every URL in full" },
  { SU_KV_MIN_LEN,  KV_UINT32, "50",
    "Only shorten targets longer than this many bytes" },
};

// Schema

// CREATE TABLE IF NOT EXISTS is idempotent on its own; the mutex only keeps
// two concurrent starts from doubling the log noise.
static bool            su_schema_done = false;
static pthread_mutex_t su_schema_lock = PTHREAD_MUTEX_INITIALIZER;

// The table's definition lives here and nowhere else — there is no .sql file
// to drift from. Both CHECK constraints are generated from the same constants
// this plugin validates against and the daemon parses with, so a change to
// SHORTURL_TOKEN_LEN moves the column rule with it.
//
// The two storage parameters are the reason a short link stays cheap: the
// daemon writes hits and last_hit_at on every read, neither is indexed, and
// fillfactor leaves in-page room for that to be a HOT update. The autovacuum
// threshold is cut because a table taking a write on every read reaches the
// default 20% dead-tuple mark long after it should have been vacuumed.
static bool
su_schema_ensure(void)
{
  char sql[1024];
  bool ok = SUCCESS;

  pthread_mutex_lock(&su_schema_lock);

  if(su_schema_done)
  {
    pthread_mutex_unlock(&su_schema_lock);
    return(SUCCESS);
  }

  snprintf(sql, sizeof(sql),
      "CREATE TABLE IF NOT EXISTS " SHORTURL_TABLE " ("
      " token       TEXT        PRIMARY KEY"
      "                         CHECK (char_length(token) = %d),"
      " target      TEXT        NOT NULL"
      "                         CHECK (char_length(target) BETWEEN 1 AND %d)"
      "                         CHECK (target !~ '[[:cntrl:]]'),"
      " hits        BIGINT      NOT NULL DEFAULT 0,"
      " created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),"
      " last_hit_at TIMESTAMPTZ"
      ") WITH (fillfactor = 85, autovacuum_vacuum_scale_factor = 0.02)",
      SHORTURL_TOKEN_LEN, SHORTURL_TARGET_MAX);

  if(db_exec(sql, SU_CTX) != SUCCESS)
    ok = FAIL;

  // What makes a repeat cheap. On md5(target) rather than target, because a
  // 2048-byte column makes a fat btree; deliberately NOT unique, so that two
  // mints racing on the same target produce a harmless duplicate rather than a
  // failed one. It costs the hit counter nothing: a HOT update needs no
  // INDEXED column to change, and target never changes — only hits and
  // last_hit_at do, and neither is indexed.
  if(ok == SUCCESS && db_exec(
      "CREATE INDEX IF NOT EXISTS " SHORTURL_TABLE "_target_md5_idx"
      " ON " SHORTURL_TABLE " (md5(target))", SU_CTX) != SUCCESS)
    ok = FAIL;

  if(ok == SUCCESS)
  {
    su_schema_done = true;
    clam(CLAM_INFO, SU_CTX, "schema ready (table '" SHORTURL_TABLE "')");
  }

  pthread_mutex_unlock(&su_schema_lock);

  return(ok);
}

// Minting

// One round trip, retried only on a token collision. The generated token is a
// CANDIDATE: SQL_MINT answers with the one this target already had if it has
// one, so searching the same URL twice prints the same link and its hits
// accumulate on one row instead of scattering.
static bool
su_mint(const char *target, token_t *out)
{
  int attempt;

  for(attempt = 0; attempt < SU_INSERT_ATTEMPTS; attempt++)
  {
    const char  *params[2];
    db_result_t *res;
    token_t      candidate;

    // Predicate, not SUCCESS/FAIL — see the note at the top of this file.
    if(!token_generate(&candidate))
    {
      clam(CLAM_WARN, SU_CTX, "getrandom refused entropy");
      return(FAIL);
    }

    params[0] = candidate.s;
    params[1] = target;

    res = db_result_alloc();

    if(db_query_params(SQL_MINT, params, 2, res) != SUCCESS || !res->ok)
    {
      clam(CLAM_WARN, SU_CTX, "mint failed: %s",
          (res->error[0] != '\0') ? res->error : "(no driver error)");
      db_result_free(res);
      return(FAIL);
    }

    if(res->rows == 1)
    {
      // The row is another writer's territory — the admin CLI mints too, and
      // the token coming back need not be one this process generated. It
      // re-enters through the same boundary an inbound token crosses, and
      // db_result_get answers NULL out of bounds, which token_parse refuses.
      bool parsed = token_parse(db_result_get(res, 0, 0), out);

      db_result_free(res);

      if(!parsed)
      {
        clam(CLAM_WARN, SU_CTX, "row holds a token that is not one");
        return(FAIL);
      }

      return(SUCCESS);
    }

    // Zero rows: the candidate is taken. Draw another.
    db_result_free(res);
  }

  clam(CLAM_WARN, SU_CTX, "%d token collisions running — keyspace exhausted?",
      SU_INSERT_ATTEMPTS);

  return(FAIL);
}

// A base already ending in '?' means "no separator", which is how the admin
// CLI has printed links since the project's first commit. su_shorten has
// already proved the result fits, so there is no truncation to test here.
static void
su_compose(char *out, size_t cap, const char *base, const token_t *token)
{
  size_t len = strlen(base);

  snprintf(out, cap, "%s%s%s", base, base[len - 1] == '?' ? "" : "?",
      token->s);
}

size_t
su_shorten(const char *const *targets, size_t n, char (*out)[SU_SHORT_URL_SZ])
{
  const char *base;
  size_t      min_len;
  size_t      filled = 0;
  size_t      i;

  for(i = 0; i < n; i++)
    out[i][0] = '\0';

  if(targets == NULL || kv_get_uint(SU_KV_ENABLED) == 0)
    return(0);

  base    = kv_get_str(SU_KV_BASE_URL);
  min_len = (size_t)kv_get_uint(SU_KV_MIN_LEN);

  // A cleared base_url has no sensible substitute — a bare token is not a
  // link — and a base too long to build one from is a configuration error
  // rather than a per-URL one, so both are answered once, here, and neither
  // reaches su_compose.
  if(base == NULL || base[0] == '\0')
    return(0);

  if(strlen(base) + 1 + SHORTURL_TOKEN_LEN + 1 > SU_SHORT_URL_SZ)
  {
    clam(CLAM_WARN, SU_CTX, "%s is too long to build a link from",
        SU_KV_BASE_URL);
    return(0);
  }

  for(i = 0; i < n; i++)
  {
    token_t token;

    // The length test is bounded so that deciding "too short to bother" does
    // not walk a 2 KB URL, and it runs before the validator for the same
    // reason. url_valid is a predicate — see the note at the top of the file.
    if(targets[i] == NULL
        || strnlen(targets[i], min_len + 1) <= min_len
        || !url_valid(targets[i]))
      continue;

    if(su_mint(targets[i], &token) != SUCCESS)
      continue;

    su_compose(out[i], SU_SHORT_URL_SZ, base, &token);
    filled++;
  }

  return(filled);
}

// Lifecycle

static bool
su_init(void)
{
  clam(CLAM_INFO, SU_CTX, "shorturl plugin initialized");
  return(SUCCESS);
}

// The schema is bootstrapped in start(), after kv_load() — init() runs before
// core has read persisted KV. A table we cannot reach is not a reason to
// refuse the plugin: su_shorten simply fills no slots and every caller
// renders its URLs in full.
static bool
su_start(void)
{
  if(su_schema_ensure() != SUCCESS)
    clam(CLAM_WARN, SU_CTX,
        "schema init failed (links will not be minted until it is fixed)");

  return(SUCCESS);
}

static void
su_deinit(void)
{
  clam(CLAM_INFO, SU_CTX, "shorturl plugin deinitialized");
}

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "shorturl",
  .version         = "1.0",
  .type            = PLUGIN_SERVICE,
  .kind            = "shorturl",
  .provides        = { { .name = "service_shorturl" } },
  .provides_count  = 1,
  .requires        = { { .name = "core_db" } },
  .requires_count  = 1,
  .kv_schema       = su_kv_schema,
  .kv_schema_count = sizeof(su_kv_schema) / sizeof(su_kv_schema[0]),
  .init            = su_init,
  .start           = su_start,
  .stop            = NULL,
  .deinit          = su_deinit,
  .ext             = NULL,
};
