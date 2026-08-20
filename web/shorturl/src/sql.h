#ifndef SHORTURL_SQL_H
#define SHORTURL_SQL_H

#include "config.h"

// Every statement below is parameterised. No value is ever interpolated into
// SQL text anywhere in this project.

// ---- Hot path ----

// Prepared once per session and executed on every request, so it carries a
// name. Re-prepared after a connection reset, since prepared statements do not
// survive the session that created them.
#define SQL_RESOLVE_NAME "shorturl_resolve"

// Lookup and increment are one statement: one round trip, and no window for a
// read-modify-write race. Zero rows returned means the token is unknown.
// $1 = token
#define SQL_RESOLVE \
  "UPDATE " SHORTURL_TABLE \
  "   SET hits = hits + 1, last_hit_at = now()" \
  " WHERE token = $1" \
  " RETURNING target"

// ---- Administrative path ----

// ON CONFLICT DO NOTHING turns a token collision into zero returned rows,
// which is cheaper to detect than parsing SQLSTATE 23505 off the error.
// $1 = token, $2 = target
#define SQL_INSERT \
  "INSERT INTO " SHORTURL_TABLE " (token, target)" \
  " VALUES ($1, $2)" \
  " ON CONFLICT (token) DO NOTHING" \
  " RETURNING token"

// $1 = token
#define SQL_DELETE \
  "DELETE FROM " SHORTURL_TABLE " WHERE token = $1 RETURNING token"

#define SQL_LIST \
  "SELECT token, target, hits," \
  "       to_char(created_at,  'YYYY-MM-DD')," \
  "       coalesce(to_char(last_hit_at, 'YYYY-MM-DD'), '-')" \
  "  FROM " SHORTURL_TABLE \
  " ORDER BY hits DESC, created_at DESC"

#endif
