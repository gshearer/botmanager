#ifndef BM_ODDSAPI_CMD_H
#define BM_ODDSAPI_CMD_H

// The oddsapi plugin's command surface: `show odds` today, `!odds` and
// `!scores` as they land. Pure presentation — every byte the user sees
// is shaped here, while fetching, caching and pricing stay in the
// service half of the same .so.
//
// Both halves ship as one plugin because oddsapi is a dependency-graph
// leaf (nothing requires service_oddsapi), so the command's upward
// bot_chat dependency is inherited by nobody. See `PLUGIN.md §Layer
// Rules` Rule 1, leaf exception.

#include <stdbool.h>

// Driven by oddsapi.c's lifecycle: the service half owns the plugin
// descriptor, so it raises and lowers the command surface with it.
bool oddsapi_cmd_register(void);
void oddsapi_cmd_unregister(void);

#ifdef ODDSAPI_CMD_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "display.h"
#include "kv.h"
#include "userns.h"
#include "util.h"

#define ODDSAPI_CMD_CTX      ODDSAPI_CTX ".cmd"
#define ODDSAPI_CMD_REPLY_SZ 512

// The `show odds` cache grid. The key column takes whatever the other
// three leave of DISPLAY_COLS.
#define ODDSAPI_SHOW_AGE_W   12
#define ODDSAPI_SHOW_ITEMS_W 8
#define ODDSAPI_SHOW_STATE_W 10
#define ODDSAPI_SHOW_KEY_W   (DISPLAY_COLS - ODDSAPI_SHOW_AGE_W \
                              - ODDSAPI_SHOW_ITEMS_W - ODDSAPI_SHOW_STATE_W)

static void     oddsapi_cell(char *line, size_t cap, const char *text,
    int width, bool right);
static void     oddsapi_age(time_t then, time_t now, char *out, size_t cap);
static uint32_t oddsapi_csv_count(const char *csv);
static void     oddsapi_show_cmd(const cmd_ctx_t *ctx);

#endif // ODDSAPI_CMD_INTERNAL

#endif // BM_ODDSAPI_CMD_H
