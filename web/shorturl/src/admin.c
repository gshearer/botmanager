// botmanager — MIT
// shorturl: administrative CLI — create, list and remove short links.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "db.h"
#include "token.h"
#include "url.h"
#include "version_str.h"

// A collision in a 62^8 keyspace is vanishingly unlikely. This bound exists so
// that a genuinely exhausted keyspace fails loudly rather than spinning.
#define INSERT_ATTEMPTS 8

typedef struct
{
  unsigned long count;
} list_state_t;

static void usage(FILE *);
static void print_short_url(const token_t *);
static void print_row(const db_row_t *, void *);
static int  cmd_add(const char *);
static int  cmd_list(void);
static int  cmd_delete(const char *);

static void
usage(FILE *stream)
{
  fprintf(stream,
          "%s\n"
          "\n"
          "usage: shorturl <command> [argument]\n"
          "\n"
          "  add <url>       create a short link for <url> and print it\n"
          "  list            list every link, most visited first\n"
          "  delete <token>  remove a link\n"
          "  version         print the version and build number\n"
          "  help            print this message\n"
          "\n"
          "Database settings come from " SHORTURL_ENV_DB_HOST ", "
          SHORTURL_ENV_DB_PORT ", " SHORTURL_ENV_DB_NAME ",\n"
          SHORTURL_ENV_DB_USER " and " SHORTURL_ENV_DB_PASS ". "
          SHORTURL_ENV_BASE_URL " sets the prefix\n"
          "that 'add' prints, for example https://example.org/p0ada\n",
          SHORTURL_VERSION_STRING);
}

static void
print_short_url(const token_t *token)
{
  const char *base = getenv(SHORTURL_ENV_BASE_URL);
  const char *separator;
  size_t length;

  if(!base || !*base)
  {
    printf("%s\n", token->s);
    return;
  }

  length = strlen(base);
  separator = base[length - 1] == '?' ? "" : "?";

  printf("%s%s%s\n", base, separator, token->s);
}

static void
print_row(const db_row_t *row, void *context)
{
  list_state_t *state = context;

  if(state->count == 0)
    printf("%-8s  %10s  %-10s  %-10s  %s\n",
           "TOKEN", "HITS", "CREATED", "LAST HIT", "TARGET");

  state->count++;

  printf("%-8s  %10s  %-10s  %-10s  %s\n",
         row->token, row->hits, row->created_at, row->last_hit_at, row->target);
}

static int
cmd_add(const char *target)
{
  token_t token;
  int attempt;

  // Checked before connecting so a bad argument costs nothing.
  if(!url_valid(target))
  {
    fprintf(stderr,
            "shorturl: destination must be 1 to %d bytes and free of control characters\n",
            SHORTURL_TARGET_MAX);
    return(1);
  }

  if(!db_open())
    return(1);

  for(attempt = 0; attempt < INSERT_ATTEMPTS; attempt++)
  {
    db_result_t result;

    if(!token_generate(&token))
    {
      perror("shorturl: getrandom");
      db_close();
      return(1);
    }

    result = db_insert(&token, target);

    if(result == DB_OK)
    {
      print_short_url(&token);
      db_close();
      return(0);
    }

    if(result != DB_CONFLICT)
    {
      db_close();
      return(1);
    }

    // DB_CONFLICT — that token is already taken. Draw another.
  }

  fprintf(stderr, "shorturl: gave up after %d token collisions\n", INSERT_ATTEMPTS);
  db_close();

  return(1);
}

static int
cmd_list(void)
{
  list_state_t state = { .count = 0 };
  db_result_t result;

  if(!db_open())
    return(1);

  result = db_list(print_row, &state);
  db_close();

  if(result == DB_NOT_FOUND)
  {
    printf("no links\n");
    return(0);
  }

  return(result == DB_OK ? 0 : 1);
}

static int
cmd_delete(const char *raw)
{
  token_t token;
  db_result_t result;

  // Not echoed back: an invalid argument may carry terminal escape sequences.
  if(!token_parse(raw, &token))
  {
    fprintf(stderr, "shorturl: not a valid %d-character token\n", SHORTURL_TOKEN_LEN);
    return(1);
  }

  if(!db_open())
    return(1);

  result = db_delete(&token);
  db_close();

  if(result == DB_NOT_FOUND)
  {
    fprintf(stderr, "shorturl: no such token\n");
    return(1);
  }

  if(result != DB_OK)
    return(1);

  printf("deleted %s\n", token.s);

  return(0);
}

int
main(int argc, char **argv)
{
  const char *command;

  if(argc < 2)
  {
    usage(stderr);
    return(2);
  }

  command = argv[1];

  if(strcmp(command, "add") == 0 && argc == 3)
    return(cmd_add(argv[2]));

  if(strcmp(command, "list") == 0 && argc == 2)
    return(cmd_list());

  if(strcmp(command, "delete") == 0 && argc == 3)
    return(cmd_delete(argv[2]));

  if(strcmp(command, "version") == 0 && argc == 2)
  {
    printf("%s\n", SHORTURL_VERSION_STRING);
    return(0);
  }

  if(strcmp(command, "help") == 0)
  {
    usage(stdout);
    return(0);
  }

  usage(stderr);

  return(2);
}
