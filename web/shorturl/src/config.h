#ifndef SHORTURL_CONFIG_H
#define SHORTURL_CONFIG_H

// ---- Environment: database connection (all required) ----

#define SHORTURL_ENV_DB_HOST "SHORTURL_DB_HOST"
#define SHORTURL_ENV_DB_PORT "SHORTURL_DB_PORT"
#define SHORTURL_ENV_DB_NAME "SHORTURL_DB_NAME"
#define SHORTURL_ENV_DB_USER "SHORTURL_DB_USER"
#define SHORTURL_ENV_DB_PASS "SHORTURL_DB_PASS"

// ---- Environment: optional ----

// Public prefix the admin CLI prepends when printing a short link, e.g.
// "https://lame.org/p0ada". The path portion is nginx-side decoration; the
// daemon never reads it. Unset means the CLI prints the bare token.
#define SHORTURL_ENV_BASE_URL "SHORTURL_BASE_URL"

// Where an unknown token is sent. Unset means a small static 404 body.
#define SHORTURL_ENV_NOT_FOUND_URL "SHORTURL_NOT_FOUND_URL"

// ---- The table ----

// A #define and deliberately not a KV knob, unlike every other table-owning
// plugin in this tree. The daemon runs on another host and reads its settings
// from the environment; it can never see botmanager's KV. A name only one side
// honoured would break resolution silently, so both compile against this.
#define SHORTURL_TABLE "urls"

// ---- Limits ----

// 62^8 ~= 2.18e14. Changing this requires a matching CHECK in sql/schema.sql.
#define SHORTURL_TOKEN_LEN 8

// Upper bound on a destination URL, enforced on the way in by the CLI and on
// the way out by the daemon. Must match the CHECK in sql/schema.sql.
#define SHORTURL_TARGET_MAX 2048

// libpq connect_timeout, in seconds, as a string.
#define SHORTURL_CONNECT_TIMEOUT "5"

#endif
