#ifndef BM_ROULETTE_H
#define BM_ROULETTE_H

#ifdef ROULETTE_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "method.h"
#include "plugin.h"

#define ROULETTE_CTX      "roulette"
#define ROULETTE_CHAMBERS 6            // a classic single-action revolver

// The fixed half of the persona prompt. Who pulled and how it went
// ride along as facts: the click line names nobody, and the bang line
// names the puller only by way of its flavour string.
#define ROULETTE_FRAMING  "Call it, in your own voice."

static method_eject_t   roulette_eject_probe(const cmd_ctx_t *ctx);
static void             roulette_eject(const cmd_ctx_t *ctx, method_eject_t force);
static void             roulette_cmd(const cmd_ctx_t *ctx);
static bool             roulette_init(void);
static void             roulette_deinit(void);

#endif // ROULETTE_INTERNAL

#endif // BM_ROULETTE_H
