#ifndef BM_COINFLIP_H
#define BM_COINFLIP_H

#ifdef COINFLIP_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "plugin.h"

#define COINFLIP_CTX      "coinflip"

// The one knob. `plugin.<kind>.in_voice` is the convention a command
// surface declares to offer its output to the bot's persona instead of
// its own table (include/bot.h §bot_persona_reply); this is its first
// consumer, and any toy freshened later declares the same suffix.
#define COINFLIP_KV_IN_VOICE  "plugin.coinflip.in_voice"

// What the mind is told the line is FOR. The fixed half of the prompt —
// register, length, no preamble — belongs to the persona_reply slot;
// this half is the only part a consumer owns, and it says what
// happened, never how to say it.
#define COINFLIP_INSTRUCTION                                           \
  "Someone just flipped a coin and you are calling the result."        \
  " Announce which side came up, in your own voice."

static void  coinflip_cmd(const cmd_ctx_t *ctx);
static bool  coinflip_init(void);
static void  coinflip_deinit(void);

#endif // COINFLIP_INTERNAL

#endif // BM_COINFLIP_H
