#ifndef BM_CLAM_CMD_H
#define BM_CLAM_CMD_H

// User-facing command interface for the CLAM event bus.
//
// Exposes /clam subscribe, /clam unsubscribe, and /show clam. Each
// subscription attaches to clam through a single shared callback and
// fans out to one or more destinations: the originating channel/DM
// ("here"), a bound bot's method target, or an append-only local file.
//
// Registrations and destinations are in-memory only; restarts drop all
// user subscriptions. All state is cleaned up in clam_cmd_exit(), which
// must run before clam_exit() so that the shared clam subscriber
// unregisters cleanly and any open log files are closed.

#include <stdbool.h>

// Must be called after cmd_init(). The shared clam subscriber is
// lazily installed on the first successful subscription.
void clam_cmd_init(void);

// Tear down every user subscription: unregister the shared clam
// subscriber, close any file destinations, and free all in-memory
// state. Safe to call repeatedly. Call before clam_exit().
void clam_cmd_exit(void);

#ifdef CLAM_CMD_INTERNAL
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <limits.h>  // PATH_MAX
#include "bot.h"     // BOT_NAME_SZ
#include "clam.h"    // CLAM_SUB_NAME_SZ, CLAM_REGEX_SZ
#include "method.h"  // METHOD_NAME_SZ, METHOD_CHANNEL_SZ
#include "userns.h"  // USERNS_USER_SZ

typedef enum
{
  CCD_HERE,
  CCD_METHOD,
  CCD_FILE,
} ccd_kind_t;

#define CLAM_CMD_MAX_DESTS 8

// Destination descriptor. Union-style packing kept as named fields for
// readability; sizes sum to ~1.6 KiB per destination, dominated by the
// file path. Eight destinations per subscription (~13 KiB) is plenty.
typedef struct
{
  ccd_kind_t  kind;

  // HERE:   method_name, target (channel or sender)
  // METHOD: bot_name, method_key, target
  // FILE:   path (other fields unused)
  char        method_name[METHOD_NAME_SZ];
  char        bot_name   [BOT_NAME_SZ];
  char        method_key [METHOD_NAME_SZ];
  char        target     [METHOD_CHANNEL_SZ];
  char        path       [PATH_MAX];
  FILE       *fp;
} ccd_dest_t;

typedef struct clam_user_sub
{
  char                    name [CLAM_SUB_NAME_SZ];
  char                    owner[USERNS_USER_SZ];  // creator, "" if unknown
  uint8_t                 sev;
  char                    regex_str[CLAM_REGEX_SZ];
  regex_t                 regex_compiled;
  bool                    has_regex;

  ccd_dest_t              dests[CLAM_CMD_MAX_DESTS];
  size_t                  n_dests;

  struct clam_user_sub   *next;
} clam_user_sub_t;
#endif // CLAM_CMD_INTERNAL

#endif // BM_CLAM_CMD_H
