// botmanager — MIT
// Cases for the FACT-1 merge ladder: the statement MEM_MERGE_OBSERVE
// sends, replayed against Postgres inside a rolled-back transaction.
// The ladder is memory.c's internal, and this replays it exactly as
// memory.c sends it — so the suite opens the same door memory.c does
// rather than widening the header's public surface for a test.
#define MEMORY_INTERNAL
#include "test.h"
#include "memory.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The ladder is SQL — a CASE table Postgres evaluates, not C — so the
// only honest test of it runs the statement. Two things keep that from
// being a liability: the statement comes from memory.h, so it cannot
// drift from the one production sends, and the fixture creates a TEMP
// dossier_facts that shadows the real one, sets search_path to pg_temp
// so an unqualified name can reach nothing else, and rolls back. When
// the database is unreachable the suite SKIPs.

#define CONF_PATH_SZ   512
#define CONF_VAL_SZ    256
#define SCRIPT_SZ      (192 * 1024)
#define LINE_SZ         512

// Both clocks are seeded here, and both are compared against it after
// the upsert. Equal means the row was left byte-identical — a
// rejection — because every accepted outcome writes NOW() to both.
#define SEED_TS  "TIMESTAMPTZ '2020-01-01 00:00:00+00'"

// One dossier, one key: the ladder resolves within a single conflict
// target, so varying them would test the unique index instead.
#define CASE_DOSSIER  ((int64_t)1)
#define CASE_KIND     1
#define CASE_KEY      "location"

typedef struct
{
  char host[CONF_VAL_SZ];
  char port[CONF_VAL_SZ];
  char name[CONF_VAL_SZ];
  char user[CONF_VAL_SZ];
  char pass[CONF_VAL_SZ];
} db_conf_t;

// stored → incoming → what the row must look like afterwards.
// want_conf is compared as the text of round(confidence::numeric, 2),
// so the expectations read as the confidences the cases were written
// with. want_untouched true is the reject arm.
static const struct
{
  const char *name;
  bool        seeded;
  const char *stored_value;
  const char *stored_source;
  double      stored_conf;
  const char *in_value;
  const char *in_source;
  double      in_conf;
  const char *want_value;
  const char *want_source;
  const char *want_conf;
  bool        want_untouched;
} merge_cases[] = {
  { "insert when nothing conflicts", false, "", "", 0.0,
    "Philadelphia", "user_stated", 0.95,
    "Philadelphia", "user_stated", "0.95", false },

  { "affirm keeps the value and refreshes the clock", true,
    "Philadelphia", "llm_extract", 0.80,
    "Philadelphia", "llm_extract", 0.60,
    "Philadelphia", "llm_extract", "0.80", false },

  { "affirm upgrades the attribution", true,
    "Philadelphia", "llm_extract", 0.80,
    "Philadelphia", "admin_seed", 0.50,
    "Philadelphia", "admin_seed", "0.80", false },

  { "affirm from below never lowers the confidence", true,
    "Philadelphia", "user_stated", 0.50,
    "Philadelphia", "llm_extract", 0.95,
    "Philadelphia", "user_stated", "0.95", false },

  // The West Chester incident: a correction used to lose to a stored
  // 1.0 forever. A replace now takes the confidence AS GIVEN.
  { "higher rank replaces at a lower confidence", true,
    "Philadelphia", "llm_extract", 1.00,
    "West Chester", "user_stated", 0.30,
    "West Chester", "user_stated", "0.30", false },

  { "equal rank replaces on a tie", true,
    "Philadelphia", "user_stated", 0.90,
    "West Chester", "user_stated", 0.90,
    "West Chester", "user_stated", "0.90", false },

  { "equal rank below the stored confidence is rejected", true,
    "Philadelphia", "user_stated", 0.90,
    "West Chester", "user_stated", 0.80,
    "Philadelphia", "user_stated", "0.90", true },

  { "lower rank never overrides a differing value", true,
    "Philadelphia", "user_stated", 0.50,
    "West Chester", "llm_extract", 0.95,
    "Philadelphia", "user_stated", "0.50", true },

  { "an unknown source ranks at the bottom", true,
    "Philadelphia", "user_stated", 0.50,
    "West Chester", "nl_observe", 1.00,
    "Philadelphia", "user_stated", "0.50", true },

  { "admin_seed outranks user_stated", true,
    "Philadelphia", "user_stated", 1.00,
    "West Chester", "admin_seed", 0.60,
    "West Chester", "admin_seed", "0.60", false },
};

// Values are pasted into the shell command line, so each is held to a
// charset that has no meaning to a shell. The password never goes near
// it — psql reads it from the environment.
static bool
conf_value_is_plain(const char *v)
{
  if(v[0] == '\0')
    return(false);

  for(size_t i = 0; v[i] != '\0'; i++)
    if(isalnum((unsigned char)v[i]) == 0 && strchr(".-_", v[i]) == NULL)
      return(false);

  return(true);
}

// Strip surrounding double quotes and any trailing newline, in place.
static void
conf_unquote(char *v)
{
  size_t len = strlen(v);

  while(len > 0 && (v[len - 1] == '\n' || v[len - 1] == '\r'
      || v[len - 1] == ' ' || v[len - 1] == '\t'))
    v[--len] = '\0';

  if(len >= 2 && v[0] == '"' && v[len - 1] == '"')
  {
    memmove(v, v + 1, len - 2);
    v[len - 2] = '\0';
  }
}

// botman.conf is KEY="value", one per line. bconf.c parses it into the
// KV store; this wants five values and no daemon, so it reads them
// directly — and resolves the path the same way bconf.c does.
static bool
conf_load(db_conf_t *c)
{
  char  path[CONF_PATH_SZ];
  char  line[CONF_VAL_SZ * 2];
  const char *base;
  const char *home;
  FILE *f;

  base = getenv("XDG_CONFIG_HOME");
  home = getenv("HOME");

  if(base != NULL && base[0] != '\0')
    snprintf(path, sizeof(path), "%s/botmanager/botman.conf", base);

  else if(home != NULL && home[0] != '\0')
    snprintf(path, sizeof(path), "%s/.config/botmanager/botman.conf", home);

  else
    strlcpy(path, ".config/botmanager/botman.conf", sizeof(path));

  f = fopen(path, "r");

  if(f == NULL)
    return(false);

  while(fgets(line, sizeof(line), f) != NULL)
  {
    char *eq = strchr(line, '=');

    if(eq == NULL)
      continue;

    *eq = '\0';

    if(strcmp(line, "DBHOST") == 0)      strlcpy(c->host, eq + 1, CONF_VAL_SZ);
    else if(strcmp(line, "DBPORT") == 0) strlcpy(c->port, eq + 1, CONF_VAL_SZ);
    else if(strcmp(line, "DBNAME") == 0) strlcpy(c->name, eq + 1, CONF_VAL_SZ);
    else if(strcmp(line, "DBUSER") == 0) strlcpy(c->user, eq + 1, CONF_VAL_SZ);
    else if(strcmp(line, "DBPASS") == 0) strlcpy(c->pass, eq + 1, CONF_VAL_SZ);
  }

  fclose(f);

  conf_unquote(c->host);
  conf_unquote(c->port);
  conf_unquote(c->name);
  conf_unquote(c->user);
  conf_unquote(c->pass);

  return(conf_value_is_plain(c->host) && conf_value_is_plain(c->port)
      && conf_value_is_plain(c->name) && conf_value_is_plain(c->user));
}

// -q silences command tags, -t -A make each SELECT one bare line, and
// ON_ERROR_STOP turns a broken fixture into a non-zero exit rather
// than a partial result set.
static void
psql_command(const db_conf_t *c, char *out, size_t out_cap)
{
  snprintf(out, out_cap,
      "psql -X -q -t -A -P pager=off -v ON_ERROR_STOP=1"
      " -h '%s' -p '%s' -U '%s' -d '%s'",
      c->host, c->port, c->user, c->name);
}

static bool script_addf(char *, size_t, size_t *, const char *, ...)
    __attribute__((format(printf, 4, 5)));

static bool
script_addf(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
  va_list ap;
  int     n;

  if(*pos >= cap)
    return(false);

  va_start(ap, fmt);
  n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
  va_end(ap);

  if(n < 0 || (size_t)n >= cap - *pos)
    return(false);

  *pos += (size_t)n;
  return(true);
}

// One transaction: a temp table that shadows dossier_facts, then per
// case a clean slate, an optional seed row, the shipped upsert, and a
// pipe-separated line naming what the row became.
static bool
script_build(char *buf, size_t cap)
{
  size_t pos = 0;
  bool   ok;

  ok = script_addf(buf, cap, &pos,
      "BEGIN;\n"
      "CREATE TEMP TABLE dossier_facts ("
      " id BIGSERIAL PRIMARY KEY,"
      " dossier_id BIGINT NOT NULL,"
      " kind SMALLINT NOT NULL,"
      " fact_key VARCHAR(128) NOT NULL,"
      " fact_value TEXT NOT NULL,"
      " source VARCHAR(32) NOT NULL DEFAULT 'llm_extract',"
      " channel VARCHAR(128) NOT NULL DEFAULT '',"
      " confidence REAL NOT NULL DEFAULT 0.6,"
      " observed_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " last_seen TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      " UNIQUE (dossier_id, kind, fact_key));\n"
      "SET LOCAL search_path = pg_temp;\n");

  for(size_t i = 0; ok && i < sizeof(merge_cases) / sizeof(merge_cases[0]); i++)
  {
    ok = script_addf(buf, cap, &pos, "DELETE FROM dossier_facts;\n");

    if(ok && merge_cases[i].seeded)
      ok = script_addf(buf, cap, &pos,
          "INSERT INTO dossier_facts (dossier_id, kind, fact_key, fact_value,"
          " source, channel, confidence, observed_at, last_seen)"
          " VALUES (%" PRId64 ", %d, '%s', '%s', '%s', '', %.2f,"
          " " SEED_TS ", " SEED_TS ");\n",
          CASE_DOSSIER, CASE_KIND, CASE_KEY, merge_cases[i].stored_value,
          merge_cases[i].stored_source, merge_cases[i].stored_conf);

    if(ok)
      ok = script_addf(buf, cap, &pos, MEM_FACT_OBSERVE_SQL ";\n",
          CASE_DOSSIER, CASE_KIND, CASE_KEY, merge_cases[i].in_value,
          merge_cases[i].in_source, "", merge_cases[i].in_conf);

    if(ok)
      ok = script_addf(buf, cap, &pos,
          "SELECT '%s' || '|' || fact_value || '|' || source || '|'"
          " || round(confidence::numeric, 2)::text || '|'"
          " || (observed_at = " SEED_TS " AND last_seen = " SEED_TS ")::text"
          " FROM dossier_facts;\n",
          merge_cases[i].name);
  }

  return(ok && script_addf(buf, cap, &pos, "ROLLBACK;\n"));
}

// Split "name|value|source|confidence|untouched" in place. Returns the
// field count, so a short line is reported rather than read past.
static size_t
line_split(char *line, char *field[], size_t max)
{
  size_t n = 0;

  field[n++] = line;

  for(char *p = line; *p != '\0' && n < max; p++)
    if(*p == '|')
    {
      *p = '\0';
      field[n++] = p + 1;
    }

  return(n);
}

static char script[SCRIPT_SZ];
static char fixture_path[CONF_PATH_SZ];

// Spill the script to a file psql can read with -f, and remember where
// so main() can unlink it. mkstemp both names and creates it, so the
// path never exists before we own it.
static bool
fixture_write(const char *sql)
{
  const char *tmp = getenv("TMPDIR");
  int         fd;
  FILE       *f;

  snprintf(fixture_path, sizeof(fixture_path), "%s/bm_fact_merge.XXXXXX",
      (tmp != NULL && tmp[0] == '/') ? tmp : "/tmp");

  fd = mkstemp(fixture_path);

  if(fd < 0)
    return(false);

  f = fdopen(fd, "w");

  if(f == NULL)
  {
    close(fd);
    unlink(fixture_path);
    return(false);
  }

  fputs(sql, f);

  if(fclose(f) != 0)
  {
    unlink(fixture_path);
    return(false);
  }

  return(true);
}

int
main(void)
{
  const size_t ncases = sizeof(merge_cases) / sizeof(merge_cases[0]);
  db_conf_t    conf;
  char         cmd[CONF_VAL_SZ * 6];
  char         run[sizeof(cmd) + sizeof(fixture_path) + 32];
  char         line[LINE_SZ];
  FILE        *p;
  size_t       row = 0;
  int          rc;

  memset(&conf, 0, sizeof(conf));

  if(!conf_load(&conf))
    return(test_skip("fact_merge", "no readable botman.conf DB settings"));

  setenv("PGPASSWORD", conf.pass, 1);
  psql_command(&conf, cmd, sizeof(cmd));

  // Ask first, so an outage reads as SKIP and a broken ladder as FAIL.
  snprintf(run, sizeof(run), "%s -c 'SELECT 1' >/dev/null 2>&1", cmd);

  if(system(run) != 0)
    return(test_skip("fact_merge", "database unreachable"));

  if(!script_build(script, sizeof(script)))
  {
    fprintf(stderr, "FAIL fact_merge: fixture exceeds %zu bytes\n",
        sizeof(script));
    return(1);
  }

  // popen is one-directional, so the script goes via a file rather
  // than down psql's stdin.
  if(!fixture_write(script))
    return(test_skip("fact_merge", "no writable temporary directory"));

  snprintf(run, sizeof(run), "%s -f '%s'", cmd, fixture_path);
  p = popen(run, "r");

  if(p == NULL)
  {
    unlink(fixture_path);
    return(test_skip("fact_merge", "psql not runnable"));
  }

  while(row < ncases && fgets(line, sizeof(line), p) != NULL)
  {
    char  *field[8];
    size_t nf;

    line[strcspn(line, "\r\n")] = '\0';

    if(line[0] == '\0')
      continue;

    nf = line_split(line, field, sizeof(field) / sizeof(field[0]));

    if(nf != 5)
    {
      fprintf(stderr, "FAIL fact_merge: unreadable row \"%s\"\n", field[0]);
      pclose(p);
      unlink(fixture_path);
      return(1);
    }

    test_check_str("fact_merge", merge_cases[row].name,
        merge_cases[row].name, field[0]);
    test_check_str("fact_merge value", merge_cases[row].name,
        merge_cases[row].want_value, field[1]);
    test_check_str("fact_merge source", merge_cases[row].name,
        merge_cases[row].want_source, field[2]);
    test_check_str("fact_merge confidence", merge_cases[row].name,
        merge_cases[row].want_conf, field[3]);
    test_check_str("fact_merge untouched", merge_cases[row].name,
        merge_cases[row].want_untouched ? "true" : "false", field[4]);
    row++;
  }

  rc = pclose(p);
  unlink(fixture_path);

  test_check_sz("fact_merge", "every case answered", ncases, row);
  test_check_bool("fact_merge", "psql exited clean", true, rc == 0);

  return(test_report("fact_merge"));
}
