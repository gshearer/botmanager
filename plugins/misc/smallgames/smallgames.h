#ifndef BM_SMALLGAMES_H
#define BM_SMALLGAMES_H

#ifdef SMALLGAMES_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"

#define SMALLGAMES_CTX  "misc_smallgames"

static void             eightball_cmd(const cmd_ctx_t *ctx);
static bool             smallgames_init(void);
static void             smallgames_deinit(void);

#endif // SMALLGAMES_INTERNAL

#endif // BM_SMALLGAMES_H
