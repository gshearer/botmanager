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

// The one knob a misc toy is allowed. `plugin.<kind>.in_voice` is the
// convention for offering a line to the bot's persona instead of the
// local table (include/bot.h §bot_persona_reply); coinflip was its
// first consumer, dice its second. ⚠ Read roulette_cmd before assuming
// this toy offers every line it renders — one of them cannot wait.
#define ROULETTE_KV_IN_VOICE  "plugin.roulette.in_voice"

static method_eject_t   roulette_eject_probe(const cmd_ctx_t *ctx);
static void             roulette_eject(const cmd_ctx_t *ctx, method_eject_t force);
static void             roulette_cmd(const cmd_ctx_t *ctx);
static bool             roulette_init(void);
static void             roulette_deinit(void);

#endif // ROULETTE_INTERNAL

#endif // BM_ROULETTE_H
