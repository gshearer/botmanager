#ifndef BM_ADMIN_H
#define BM_ADMIN_H

// Initialize the administrative command module. Registers system-level
// commands (set, show, status, quit, help). Must be called after
// cmd_init().
void admin_init(void);

// Register KV configuration keys and load values. Must be called
// after kv_init() and kv_load().
void admin_register_config(void);

// Shut down the administrative command module. Unregisters all
// admin commands.
void admin_exit(void);

#ifdef ADMIN_INTERNAL

#include "common.h"
#include "bot.h"
#include "clam.h"
#include "colors.h"
#include "curl.h"
#include "cmd.h"
#include "db.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "plugin.h"
#include "pool.h"
#include "sock.h"
#include "task.h"
#include "userns.h"
#include "validate.h"
#include "version.h"

// Structural max for /show output (array sizing).
#define ADMIN_SHOW_LIMIT 256

// Iteration state for bot listing.
typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} botlist_state_t;

// Iteration state for /show methods.
typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} methodlist_state_t;

// Single KV entry for /show output.
typedef struct
{
  char      key[KV_KEY_SZ];
  char      val[KV_STR_SZ];
  kv_type_t type;
} admin_show_entry_t;

// Collected /show results.
typedef struct
{
  admin_show_entry_t entries[ADMIN_SHOW_LIMIT];
  uint32_t           count;
} admin_show_result_t;

// Iteration state for /userlist.
typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} userlist_state_t;

// Iteration state for /userinfo.
typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} userinfo_state_t;

// Iteration state for /grouplist.
typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} grouplist_state_t;

// Iteration state for /mfa list.
typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} mfa_list_state_t;

// Collected command entry for the help table.
typedef struct
{
  char     name[CMD_NAME_SZ];
  char     abbrev[CMD_NAME_SZ];
  char     module[CMD_MODULE_SZ];
  char     help[CMD_HELP_SZ];
  char     group[USERNS_GROUP_SZ];
  uint16_t level;
} help_cmd_entry_t;

// Maximum commands shown in the help table.
#define HELP_CMD_MAX 128

typedef struct
{
  help_cmd_entry_t entries[HELP_CMD_MAX];
  uint32_t         count;
  method_type_t    caller_type;   // filter: only collect matching commands
} help_cmd_state_t;

// Register user/group/MFA management commands. Called from admin_init().
void admin_register_user_commands(void);

// Register bot management commands and /quit. Called from admin_init().
void admin_register_bot_commands(void);

// Register help commands. Called from admin_init().
void admin_register_help_commands(void);

#endif // ADMIN_INTERNAL

#endif // BM_ADMIN_H
