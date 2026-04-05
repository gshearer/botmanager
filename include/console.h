#ifndef BM_CONSOLE_H
#define BM_CONSOLE_H

#include <stdbool.h>
#include <time.h>

#include "method.h"

// Register the console as a method instance and start the input
// reader. Must be called after method_init() and cmd_init().
// start_time: program start time (for uptime display)
void console_register_method(time_t start_time);

// Get the console method instance.
// returns: console method instance, or NULL if not yet registered
method_inst_t *console_get_inst(void);

// Write user-facing output directly to the console, bypassing CLAM.
// Used by the console method driver for command responses and other
// user-facing text that should not be formatted as log messages.
// text: output text (one line, no trailing newline needed)
void console_print(const char *text);

// Register console KV settings. Must be called after kv_init()/kv_load().
void console_register_config(void);

// Shut down the console method.
void console_exit(void);

// Lock/unlock console output for thread-safe readline interleaving.
// CLAM's stdinout subscriber calls these to avoid corrupting the
// readline input line. No-ops when the console is not active.
void console_output_lock(void);
void console_output_unlock(void);

#ifdef CONSOLE_INTERNAL

#include "common.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "plugin.h"
#include "pool.h"
#include "task.h"
#include "userns.h"

#include <readline/readline.h>
#include <readline/history.h>

static bool console_active = false;

static time_t con_start_time = 0;

// Console method instance (registered with the method subsystem).
static method_inst_t *con_method_inst = NULL;

// --- Readline state ---

// Mutex for thread-safe output interleaving with readline.
static pthread_mutex_t con_output_mutex = PTHREAD_MUTEX_INITIALIZER;

// Dynamic prompt string built from attach/associate state.
#define CON_PROMPT_SZ  128
static char con_prompt[CON_PROMPT_SZ];

// True while readline() is blocking for input.
static bool con_readline_active = false;

// --- History state ---

// Resolved history file path (~ expanded to $HOME).
#define CON_HISTORY_PATH_SZ  256
static char con_history_path[CON_HISTORY_PATH_SZ];

// Cached KV values for history configuration.
static uint32_t con_history_size = 1000;

// Cached KV values for prompt customization.
#define CON_PROMPT_FMT_SZ  64
static char    con_prompt_fmt[CON_PROMPT_FMT_SZ] = "{bot}:{user}> ";
static uint8_t con_prompt_color = 1;

// --- Attach/associate state ---

// Currently attached bot instance (NULL when not attached).
static bot_inst_t *con_attached_bot = NULL;

// Currently associated user identity (defaults to @owner).
static char con_associated_user[USERNS_USER_SZ] = USERNS_OWNER_USER;

// Forward declarations.
static void  con_input_cb(task_t *t);
static void  con_dispatch(char *line);
static void  con_build_prompt(void);
static int   con_check_shutdown(void);
static void  con_kv_changed(const char *key, void *data);
static void  con_load_config(void);
static void  con_resolve_history_path(void);
static void  con_cmd_console(const cmd_ctx_t *ctx);
static void  con_cmd_attach(const cmd_ctx_t *ctx);
static void  con_cmd_unattach(const cmd_ctx_t *ctx);
static void  con_cmd_associate(const cmd_ctx_t *ctx);
static void  con_cmd_unassociate(const cmd_ctx_t *ctx);
static void  con_cmd_history(const cmd_ctx_t *ctx);
static void  con_cmd_history_list(const cmd_ctx_t *ctx);
static void  con_cmd_history_clear(const cmd_ctx_t *ctx);
static void  con_cmd_history_search(const cmd_ctx_t *ctx);
static void  con_cmd_clear(const cmd_ctx_t *ctx);
static int   con_ctrl_c_handler(int count, int key);

// --- Readline tab completion ---

// Maximum number of completion matches collected.
#define CON_COMP_MAX  128

// State for the completion match generator.
typedef struct
{
  char  **matches;       // collected match strings
  int     count;         // number of matches
  int     index;         // current generator index
} con_comp_state_t;

static con_comp_state_t con_comp;

static char **con_completion(const char *text, int start, int end);
static char  *con_comp_generator(const char *text, int state);
static void   con_comp_add(const char *str);
static void   con_comp_reset(void);
static void   con_complete_command(const char *text);
static void   con_complete_subcommand(const char *text,
                  const cmd_def_t *parent);
static void   con_complete_args(const char *text, const char *cmd_name,
                  const cmd_def_t *cmd, int arg_pos);

#endif // CONSOLE_INTERNAL

#endif // BM_CONSOLE_H
