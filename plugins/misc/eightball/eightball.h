#ifndef BM_EIGHTBALL_H
#define BM_EIGHTBALL_H

#ifdef EIGHTBALL_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "plugin.h"

#define EIGHTBALL_CTX   "eightball"

// The one knob a misc toy is allowed. `plugin.<kind>.in_voice` is the
// convention for offering a line to the bot's persona instead of the
// local table (include/bot.h §bot_persona_reply); coinflip was its
// first consumer, dice its second.
#define EIGHTBALL_KV_IN_VOICE   "plugin.eightball.in_voice"

static void             eightball_cmd(const cmd_ctx_t *ctx);
static bool             eightball_init(void);
static void             eightball_deinit(void);

#endif // EIGHTBALL_INTERNAL

#endif // BM_EIGHTBALL_H
