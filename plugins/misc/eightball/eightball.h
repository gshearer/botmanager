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

// The fixed half of the persona prompt. The rendered line carries the
// ball's answer but not the question, so the question rides along as the
// facts half -- a verdict with nothing to be a verdict about is not a
// line anyone can write.
#define EIGHTBALL_FRAMING  "Deliver that verdict, in your own voice."

static void             eightball_cmd(const cmd_ctx_t *ctx);
static bool             eightball_init(void);
static void             eightball_deinit(void);

#endif // EIGHTBALL_INTERNAL

#endif // BM_EIGHTBALL_H
