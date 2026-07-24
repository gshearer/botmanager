#ifndef BM_EIGHTBALL_H
#define BM_EIGHTBALL_H

#ifdef EIGHTBALL_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"

#define EIGHTBALL_CTX   "eightball"

static void             eightball_cmd(const cmd_ctx_t *ctx);
static bool             eightball_init(void);
static void             eightball_deinit(void);

#endif // EIGHTBALL_INTERNAL

#endif // BM_EIGHTBALL_H
