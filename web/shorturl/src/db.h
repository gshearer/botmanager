#ifndef SHORTURL_DB_H
#define SHORTURL_DB_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "token.h"
#include "url.h"

// Outcomes that oblige the caller to do different things: redirect, 404, retry
// with a fresh token, or 500. Five ways a query can fail that the caller
// handles identically remain one value, DB_ERROR.
//
// DB_OK is 0 so that a two-state test of the result comes out incomplete
// rather than inverted — it separates success from failure and loses only
// which obligation applies. The obligation it cannot discharge is
// DB_CONFLICT, which db_insert alone returns and which alone is retryable.
typedef enum
{
  DB_OK = 0,
  DB_NOT_FOUND,
  DB_CONFLICT,
  DB_ERROR
} db_result_t;

// One row of db_list output. Every field points into storage owned by the
// call and is invalid once the callback returns.
typedef struct
{
  const char *token;
  const char *target;
  const char *hits;
  const char *created_at;
  const char *last_hit_at;
} db_row_t;

typedef void (*db_row_fn)(const db_row_t *, void *);

// Reads the connection settings from the environment and connects. Diagnoses
// a missing variable on stderr. Call once, before serving.
bool db_open(void);

void db_close(void);

// Resolves a token to its destination and counts the hit, atomically.
// target receives a NUL-terminated URL on DB_OK and is untouched otherwise.
//
// The database is another process, so its output is untrusted here: a row that
// is too long for target, or that contains a control character, yields
// DB_ERROR rather than a truncated or header-splitting redirect.
db_result_t db_resolve(const token_t *, char *, size_t);

// DB_CONFLICT means the token is already taken — generate another and retry.
db_result_t db_insert(const token_t *, const char *);

db_result_t db_delete(const token_t *);

// Invokes the callback once per row, most-visited first.
db_result_t db_list(db_row_fn, void *);

#ifdef DB_INTERNAL

#include <libpq-fe.h>

static bool        db_connect(void);
static bool        db_ready(void);
static bool        db_prepare_resolve(void);
static const char *require_env(const char *);
static db_result_t db_result_of(PGresult *, const char *);
static db_result_t copy_target(const char *, char *, size_t);

#endif

#endif
