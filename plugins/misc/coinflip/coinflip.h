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

// What the mind is told the line is FOR. Wholly fixed — a flip has no
// detail the rendered line does not already carry — so it is the whole
// declaration and cmd_reply_voiced is passed no facts.
#define COINFLIP_FRAMING                                               \
  "Someone just flipped a coin and you are calling the result."        \
  " Announce which side came up, in your own voice."

static void  coinflip_cmd(const cmd_ctx_t *ctx);
static bool  coinflip_init(void);
static void  coinflip_deinit(void);

#endif // COINFLIP_INTERNAL

#endif // BM_COINFLIP_H
