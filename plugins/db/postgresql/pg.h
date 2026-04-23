#ifndef BM_PG_H
#define BM_PG_H

#include "db.h"

// The PostgreSQL driver. Pass to db_init() to use PostgreSQL.
extern const db_driver_t pg_driver;

#ifdef PG_INTERNAL

#include "common.h"
#include "alloc.h"
#include "plugin.h"

#include <libpq-fe.h>

// Last error from a failed connect (no handle available to query).
static char pg_last_error[DB_ERROR_SZ] = "";

#endif // PG_INTERNAL

#endif
