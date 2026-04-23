#ifndef BM_BOTMANCTL_H
#define BM_BOTMANCTL_H

#include <stdbool.h>

// Must be called after method_init() and cmd_init().
void botmanctl_register_method(void);

// Must be called during the config registration phase.
void botmanctl_register_config(void);

// Closes all connections, unlinks the socket file, and unregisters the
// method instance.
void botmanctl_exit(void);

// Returns empty string if no namespace cd is set or no dispatch is active.
const char *botmanctl_get_user_ns(void);

void botmanctl_set_user_ns(const char *name);

#ifdef BOTMANCTL_INTERNAL

#include "method.h"
#include "userns.h"
#include "common.h"
#include "clam.h"
#include "cmd.h"
#include "kv.h"
#include "alloc.h"
#include "pool.h"
#include "task.h"

#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BCTL_INPUT_SZ      512
#define BCTL_SOCK_PATH_SZ  256
#define BCTL_MAX_CLIENTS   16

typedef enum {
  BCTL_MODE_COMMAND,    // interactive command mode
  BCTL_MODE_SUBSCRIBE,  // CLAM subscription mode
} bctl_mode_t;

typedef struct bctl_client
{
  int                  fd;
  bctl_mode_t          mode;
  char                 user_ns_cd[64];   // per-session working namespace
  char                 as_user[USERNS_USER_SZ]; // asserted identity for dispatch (default @owner)
  bool                 closing;          // marked for removal

  // Subscribe mode state (only used when mode == BCTL_MODE_SUBSCRIBE):
  uint8_t              clam_sev;         // subscribed severity level
  regex_t              clam_regex;       // compiled regex filter
  bool                 has_clam_regex;   // true if clam_regex is compiled

  struct bctl_client  *next;
} bctl_client_t;

typedef struct
{
  int                listen_fd;
  char               sock_path[BCTL_SOCK_PATH_SZ];
  method_inst_t     *inst;
  bctl_client_t     *clients;
  uint32_t           client_count;
  pthread_mutex_t    client_mutex;       // protects client list
} bctl_server_t;

static bctl_server_t *bctl_state  = NULL;
static bool           bctl_active = false;

// Set before cmd_dispatch_owner, cleared after.
static bctl_client_t *bctl_reply_target = NULL;

// Shared CLAM subscriber for all botmanctl subscribe clients.
static bool bctl_clam_subscribed = false;

static void  bctl_task_cb(task_t *t);
static void  bctl_dispatch(bctl_server_t *srv, bctl_client_t *c, char *line);
static void  bctl_handle_subscribe(bctl_server_t *srv, bctl_client_t *c,
                 char *args);
static void  bctl_clam_cb(const clam_msg_t *m);
static void *bctl_drv_create(const char *inst_name);
static void  bctl_drv_destroy(void *handle);
static bool  bctl_drv_connect(void *handle);
static void  bctl_drv_disconnect(void *handle);
static bool  bctl_drv_send(void *handle, const char *target, const char *text);
static bool  bctl_drv_get_context(void *handle, const char *sender,
                 char *ctx, size_t ctx_sz);

#endif // BOTMANCTL_INTERNAL

#endif // BM_BOTMANCTL_H
