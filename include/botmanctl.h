#ifndef BM_BOTMANCTL_H
#define BM_BOTMANCTL_H

#include <stdbool.h>

#include "method.h"

// Register the botmanctl method instance and start the listener.
// Must be called after method_init() and cmd_init().
void botmanctl_register_method(void);

// Register KV configuration for the botmanctl method.
// Must be called during the config registration phase.
void botmanctl_register_config(void);

// Shut down the botmanctl method. Closes all connections, unlinks
// the socket file, and unregisters the method instance.
void botmanctl_exit(void);

#ifdef BOTMANCTL_INTERNAL

#include "common.h"
#include "clam.h"
#include "cmd.h"
#include "kv.h"
#include "mem.h"
#include "pool.h"
#include "task.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BCTL_INPUT_SZ   512
#define BCTL_SOCK_PATH_SZ  256

// Module state.
typedef struct
{
  int            listen_fd;              // listener socket fd
  int            client_fd;              // connected client fd (-1 if none)
  char           sock_path[BCTL_SOCK_PATH_SZ];
  method_inst_t *inst;                   // our method instance
} botmanctl_state_t;

static botmanctl_state_t *bctl_state = NULL;
static bool               bctl_active = false;

// Forward declarations.
static void  bctl_task_cb(task_t *t);
static void  bctl_dispatch(botmanctl_state_t *st, char *line);
static void *bctl_drv_create(const char *inst_name);
static void  bctl_drv_destroy(void *handle);
static bool  bctl_drv_connect(void *handle);
static void  bctl_drv_disconnect(void *handle);
static bool  bctl_drv_send(void *handle, const char *target, const char *text);
static bool  bctl_drv_get_context(void *handle, const char *sender,
                 char *ctx, size_t ctx_sz);

#endif // BOTMANCTL_INTERNAL

#endif // BM_BOTMANCTL_H
