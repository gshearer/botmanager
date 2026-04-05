#ifndef BM_BCONF_H
#define BM_BCONF_H

#include <stdbool.h>

// returns: SUCCESS or FAIL (file not found or unreadable)
// path: file path, or NULL for default ($HOME/.config/botmanager/botman.conf)
bool bconf_init(const char *path);

// returns: value string or NULL if not found
// key: configuration key (case-insensitive lookup)
const char *bconf_get(const char *key);

// returns: integer value, or def if not found or invalid
// key: configuration key
// def: default value
int bconf_get_int(const char *key, int def);

// Free stored data.
void bconf_exit(void);

#ifdef BCONF_INTERNAL

#include "common.h"
#include "clam.h"
#include "mem.h"

#include <errno.h>
#include <limits.h>

#define BCONF_KEY_SZ   64
#define BCONF_VAL_SZ   256
#define BCONF_MAX      64
#define BCONF_LINE_SZ  512

typedef struct
{
  char key[BCONF_KEY_SZ];
  char val[BCONF_VAL_SZ];
} bconf_entry_t;

static bconf_entry_t *entries = NULL;
static uint32_t       entry_count = 0;
static bool           bconf_ready = false;

#endif // BCONF_INTERNAL

#endif // BM_BCONF_H
