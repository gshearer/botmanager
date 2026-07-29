#ifndef BM_ROULETTE_H
#define BM_ROULETTE_H

#ifdef ROULETTE_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"

#define ROULETTE_CTX      "roulette"
#define ROULETTE_CHAMBERS 6            // a classic single-action revolver

static method_eject_t   roulette_eject(const cmd_ctx_t *ctx, const char *nick);
static void             roulette_cmd(const cmd_ctx_t *ctx);
static bool             roulette_init(void);
static void             roulette_deinit(void);

#endif // ROULETTE_INTERNAL

#endif // BM_ROULETTE_H
