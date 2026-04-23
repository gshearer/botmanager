#ifndef BM_MATH_PLUGIN_H
#define BM_MATH_PLUGIN_H

// No public API — this plugin is loaded via dlopen and interacts
// with the core exclusively through cmd_register / cmd_reply.
// All declarations are internal.

#ifdef MATH_INTERNAL

#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "plugin.h"

#include "math_expr.h"

#define MATH_CTX        "misc_math"
#define MATH_EXPR_SZ    256
#define MATH_REPLY_SZ   512

static bool             math_validate_expr(const char *s);

static const cmd_arg_desc_t math_ad[] = {
  { "expression", CMD_ARG_CUSTOM,
    CMD_ARG_REQUIRED | CMD_ARG_REST, MATH_EXPR_SZ,
    math_validate_expr },
};

static void             math_cmd(const cmd_ctx_t *ctx);
static bool             math_init(void);
static void             math_deinit(void);

#endif // MATH_INTERNAL

#endif // BM_MATH_PLUGIN_H
