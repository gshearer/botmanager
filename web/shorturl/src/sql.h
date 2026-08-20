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

// Mint: reuse this target's existing token, or insert the caller's candidate.
// Used by botmanager's shorturl plugin; the daemon and the CLI never mint this
// way, and the CLI keeps SQL_INSERT because `add` means "make me a new one".
//
// One row comes back and it is the token to print — the one the target already
// had, or the one just inserted. ZERO rows means the candidate collided with a
// token already in the table, which is the caller's signal to try another; that
// is the same signal SQL_INSERT gives, so the retry loop is unchanged.
//
// md5(target) is what the index is on — target is up to 2048 bytes and makes a
// fat btree — and the `target = $2` beside it is what makes a hash collision
// harmless rather than a wrong redirect. ORDER BY created_at so that a target
// which already has duplicates keeps answering with the same one.
//
// Two concurrent mints of the same target can both find nothing and both
// insert. That races to a duplicate row, which is exactly the state this
// statement tolerates everywhere else, so the index is deliberately NOT unique:
// a unique one would turn a benign duplicate into a failed mint.
//
// $1 = candidate token, $2 = target
#define SQL_MINT \
  "WITH found AS (" \
  "  SELECT token FROM " SHORTURL_TABLE \
  "   WHERE md5(target) = md5($2) AND target = $2" \
  "   ORDER BY created_at LIMIT 1" \
  "), ins AS (" \
  "  INSERT INTO " SHORTURL_TABLE " (token, target)" \
  "  SELECT $1, $2 WHERE NOT EXISTS (SELECT 1 FROM found)" \
  "  ON CONFLICT (token) DO NOTHING" \
  "  RETURNING token" \
  ")" \
  "SELECT token FROM found UNION ALL SELECT token FROM ins"

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
