// botmanager — MIT
// Cases for knowledge ingest idempotency: the statement
// KNOWLEDGE_INSERT_CHUNK_SQL sends, replayed against Postgres inside a
// rolled-back transaction.
//
// The whole fix is one SQL statement and one unique index, so the only
// honest test of it runs them. Two things keep that from being a
// liability: the statement comes from knowledge_priv.h, so it cannot
// drift from the one production sends, and the fixture creates TEMP
// tables that shadow the real ones, sets search_path to pg_temp so an
// unqualified name can reach nothing else, and rolls back. When the
// database is unreachable the suite SKIPs.
//
// The silent wrongness this is filtered for is the reason OBS-16 was a
// row at all: an ingest that quietly inserts a second copy of every
// chunk reports success, costs an embedding per duplicate, and is only
// visible as a row count nobody reads. Two of the cases below are
// regression tests for keys that were PROPOSED and are wrong — case 4
// for a three-column key that would drop a legitimately distinct
// section, and case 6 for sha256(text::bytea), which raises outright on
// a chunk whose text opens "\x" followed by a non-hex byte.

#include "test.h"

#include "knowledge_priv.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUITE "knowledge_dedup"

#define CONF_PATH_SZ   512
#define CONF_VAL_SZ    256
#define SCRIPT_SZ      (64 * 1024)
#define LINE_SZ        1024

// Every case that is meant to land on the SAME row uses these, so
// "did it come back with case 1's id" is a question the fixture can
// ask in SQL rather than one the harness has to remember.
#define BASE_CORPUS   "c1"
#define BASE_URL      "https://example.invalid/p"
#define BASE_HEADING  "h1"
#define BASE_TEXT     "hello world"

typedef struct
{
  char host[CONF_VAL_SZ];
  char port[CONF_VAL_SZ];
  char name[CONF_VAL_SZ];
  char user[CONF_VAL_SZ];
  char pass[CONF_VAL_SZ];
} db_conf_t;

// One presentation of a chunk to the statement, and what the statement
// must answer. `pre` runs first when set — case 3 is case 2's row after
// a vector has been attached, which is the only difference between
// them and the whole of what UNEMBEDDED vs PRESENT turns on.
//
// want_base_id is "the id returned is case 1's row": true means the
// statement resolved to the existing row, false means it created a new
// one. That is the assertion a bare inserted/embedded pair cannot make.
static const struct
{
  const char *name;
  const char *pre;
  const char *corpus;
  const char *source_url;
  const char *heading;
  const char *text;
  const char *want_inserted;
  const char *want_embedded;
  bool        want_base_id;
} dedup_cases[] = {
  { "a new chunk is inserted", NULL,
    BASE_CORPUS, BASE_URL, BASE_HEADING, BASE_TEXT,
    "1", "0", true },

  { "the same chunk again does not insert a second copy", NULL,
    BASE_CORPUS, BASE_URL, BASE_HEADING, BASE_TEXT,
    "0", "0", true },

  // The resume half. Without the embedding this is case 2; with it the
  // caller must skip instead of spending an embed request.
  { "a chunk already stored and already embedded reports embedded",
    "INSERT INTO knowledge_chunk_embeddings SELECT id, 'm', 1, '\\x00'"
    " FROM knowledge_chunks WHERE corpus = '" BASE_CORPUS "'"
    "   AND section_heading = '" BASE_HEADING "'"
    "   AND text = '" BASE_TEXT "';",
    BASE_CORPUS, BASE_URL, BASE_HEADING, BASE_TEXT,
    "0", "1", true },

  // Two archwiki sections legitimately carry the same body. A
  // three-column key (corpus, source_url, md5(text)) drops one of them.
  { "the same text under a different heading is a different chunk", NULL,
    BASE_CORPUS, BASE_URL, "h2", BASE_TEXT,
    "1", "0", false },

  { "the same text in a different corpus is a different chunk", NULL,
    "c2", BASE_URL, BASE_HEADING, BASE_TEXT,
    "1", "0", false },

  // The regression test for sha256(text::bytea): that key raises
  // "invalid hexadecimal digit" on this input rather than storing it,
  // and arch wiki code blocks contain such text. md5 does not
  // reinterpret, so this is an ordinary chunk.
  { "text opening with a backslash-x escape is stored, not reinterpreted",
    NULL,
    BASE_CORPUS, BASE_URL, "h3", "\\xZZ not hexadecimal at all, just text",
    "1", "0", false },

  { "one byte of difference is a different chunk", NULL,
    BASE_CORPUS, BASE_URL, BASE_HEADING, "hello worlD",
    "1", "0", false },
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

// One transaction: temp tables that shadow the two real ones, the
// shipped unique index spelled exactly as knowledge_ensure_tables()
// spells it, then per case an optional pre-step, the shipped statement,
// and a pipe-separated line naming what it answered.
//
// ⚠ The statement CANNOT be wrapped in a subselect to decorate its row:
// Postgres refuses a WITH clause containing a data-modifying statement
// anywhere but the top level. So it runs exactly as production sends
// it and its row is captured with \gset — which is the stronger
// arrangement anyway, since a wrapper would have been a second form of
// the statement that the suite, and only the suite, ever executes.
static bool
script_build(char *buf, size_t cap)
{
  size_t pos = 0;
  bool   ok;

  ok = script_addf(buf, cap, &pos,
      "BEGIN;\n"
      "CREATE TEMP TABLE knowledge_chunks ("
      " id BIGSERIAL PRIMARY KEY,"
      " corpus VARCHAR(64) NOT NULL,"
      " source_url TEXT NOT NULL DEFAULT '',"
      " section_heading TEXT NOT NULL DEFAULT '',"
      " text TEXT NOT NULL,"
      " created TIMESTAMPTZ NOT NULL DEFAULT NOW());\n"
      "CREATE TEMP TABLE knowledge_chunk_embeddings ("
      " chunk_id BIGINT PRIMARY KEY"
      "   REFERENCES knowledge_chunks(id) ON DELETE CASCADE,"
      " model VARCHAR(64) NOT NULL,"
      " dim INT NOT NULL,"
      " vec BYTEA NOT NULL);\n"
      "CREATE UNIQUE INDEX knowledge_chunks_dedup"
      " ON knowledge_chunks(corpus, source_url, section_heading,"
      " md5(text));\n"
      "SET LOCAL search_path = pg_temp;\n");

  for(size_t i = 0; ok && i < sizeof(dedup_cases) / sizeof(dedup_cases[0]); i++)
  {
    if(dedup_cases[i].pre != NULL)
      ok = script_addf(buf, cap, &pos, "%s\n", dedup_cases[i].pre);

    if(ok)
      ok = script_addf(buf, cap, &pos, KNOWLEDGE_INSERT_CHUNK_SQL "\n\\gset r_\n",
          dedup_cases[i].corpus, dedup_cases[i].source_url,
          dedup_cases[i].heading, dedup_cases[i].text);

    if(ok)
      ok = script_addf(buf, cap, &pos,
          "SELECT '%s' || '|' || :r_inserted || '|' || :r_embedded || '|'"
          " || (:r_id = (SELECT MIN(id) FROM knowledge_chunks"
          "               WHERE corpus = '" BASE_CORPUS "'"
          "                 AND section_heading = '" BASE_HEADING "'"
          "                 AND text = '" BASE_TEXT "'))::text;\n",
          dedup_cases[i].name);
  }

  return(ok && script_addf(buf, cap, &pos, "ROLLBACK;\n"));
}

// Split "name|inserted|embedded|base_id" in place. Returns the field
// count, so a short line is reported rather than read past.
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

  snprintf(fixture_path, sizeof(fixture_path), "%s/bm_knowledge_dedup.XXXXXX",
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
  const size_t ncases = sizeof(dedup_cases) / sizeof(dedup_cases[0]);
  db_conf_t    conf;
  char         cmd[CONF_VAL_SZ * 6];
  char         run[sizeof(cmd) + sizeof(fixture_path) + 32];
  char         line[LINE_SZ];
  FILE        *p;
  size_t       row = 0;
  int          rc;

  memset(&conf, 0, sizeof(conf));

  if(!conf_load(&conf))
    return(test_skip(SUITE, "no readable botman.conf DB settings"));

  setenv("PGPASSWORD", conf.pass, 1);
  psql_command(&conf, cmd, sizeof(cmd));

  // Ask first, so an outage reads as SKIP and a broken statement as FAIL.
  snprintf(run, sizeof(run), "%s -c 'SELECT 1' >/dev/null 2>&1", cmd);

  if(system(run) != 0)
    return(test_skip(SUITE, "database unreachable"));

  if(!script_build(script, sizeof(script)))
  {
    fprintf(stderr, "FAIL " SUITE ": fixture exceeds %zu bytes\n",
        sizeof(script));
    return(1);
  }

  // popen is one-directional, so the script goes via a file rather
  // than down psql's stdin.
  if(!fixture_write(script))
    return(test_skip(SUITE, "no writable temporary directory"));

  snprintf(run, sizeof(run), "%s -f '%s'", cmd, fixture_path);
  p = popen(run, "r");

  if(p == NULL)
  {
    unlink(fixture_path);
    return(test_skip(SUITE, "psql not runnable"));
  }

  while(row < ncases && fgets(line, sizeof(line), p) != NULL)
  {
    char  *field[8];
    size_t nf;

    line[strcspn(line, "\r\n")] = '\0';

    if(line[0] == '\0')
      continue;

    nf = line_split(line, field, sizeof(field) / sizeof(field[0]));

    if(nf != 4)
    {
      fprintf(stderr, "FAIL " SUITE ": unreadable row \"%s\"\n", field[0]);
      pclose(p);
      unlink(fixture_path);
      return(1);
    }

    test_check_str(SUITE, dedup_cases[row].name,
        dedup_cases[row].name, field[0]);
    test_check_str(SUITE " inserted", dedup_cases[row].name,
        dedup_cases[row].want_inserted, field[1]);
    test_check_str(SUITE " embedded", dedup_cases[row].name,
        dedup_cases[row].want_embedded, field[2]);
    test_check_str(SUITE " resolved to the existing row",
        dedup_cases[row].name,
        dedup_cases[row].want_base_id ? "true" : "false", field[3]);
    row++;
  }

  rc = pclose(p);
  unlink(fixture_path);

  test_check_sz(SUITE, "every case answered", ncases, row);
  test_check_bool(SUITE, "psql exited clean", true, rc == 0);

  return(test_report(SUITE));
}
