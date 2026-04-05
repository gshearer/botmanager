// command.h — Command bot plugin (kind: command)
//
// Provides command-based interaction with humans via methods. Detects
// command prefixes, dispatches to registered handlers via the task system,
// and responds on the originating method.
//
// The core's cmd_dispatch() does all the heavy lifting. This plugin wires
// it to the bot driver's on_message() callback and registers the
// authentication commands (identify, deauth).

#ifndef BM_COMMAND_H
#define BM_COMMAND_H

// No public API — this plugin interacts with the core exclusively
// through bot/cmd/method interfaces. All declarations are internal.

#ifdef COMMAND_INTERNAL

#include "bot.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"

// Per-instance state.
typedef struct
{
  bot_inst_t *inst;     // back-pointer to bot instance
} cmdbot_state_t;

// KV schema for the command bot plugin.
static const plugin_kv_entry_t cmdbot_kv_schema[] = {
  { "plugin.command.prefix", KV_STR, "!" },
};

// Bot driver vtable.
static const bot_driver_t cmdbot_driver;

// Argument descriptors for authentication commands.
static const cmd_arg_desc_t cmdbot_ad_identify[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_USER_SZ, NULL },
  { "password", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,            NULL },
};

static const cmd_arg_desc_t cmdbot_ad_register[] = {
  { "password", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

#endif // COMMAND_INTERNAL

#endif // BM_COMMAND_H
