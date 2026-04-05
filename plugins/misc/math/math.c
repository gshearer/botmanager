#define MATH_INTERNAL
#include "math_plugin.h"

#include <math.h>
#include <stdio.h>

#include "colors.h"

// -----------------------------------------------------------------------
// Input validation
// -----------------------------------------------------------------------

// Validate expression string.  Accepts digits, letters (for function
// names like sin, cos, pi), arithmetic operators, parentheses, dots,
// commas, spaces, and underscores.  Rejects everything else.
// returns: true if valid, false otherwise
// s: NUL-terminated expression string
static bool
math_validate_expr(const char *s)
{
  if(s == NULL || s[0] == '\0')
    return false;

  for(int i = 0; s[i] != '\0'; i++)
  {
    char c = s[i];

    if((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c == '+' || c == '-'
        || c == '*' || c == '/' || c == '^' || c == '%'
        || c == '(' || c == ')' || c == '.' || c == ','
        || c == ' ' || c == '\t' || c == '_')
      continue;

    return false;
  }

  return true;
}

// -----------------------------------------------------------------------
// Command handler
// -----------------------------------------------------------------------

static void
math_cmd(const cmd_ctx_t *ctx)
{
  const char *expr = ctx->parsed->argv[0];
  int err = 0;
  double result = math_eval(expr, &err);
  char line[MATH_REPLY_SZ];

  if(err != 0)
  {
    snprintf(line, sizeof(line),
        CLR_RED "Error" CLR_RESET " at position %d", err);
    cmd_reply(ctx, line);
    return;
  }

  if(isnan(result))
  {
    cmd_reply(ctx, CLR_RED "Error" CLR_RESET ": result is undefined");
    return;
  }

  if(isinf(result))
  {
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET " = " CLR_CYAN "Infinity" CLR_RESET,
        expr);
    cmd_reply(ctx, line);
    return;
  }

  // Display as integer if no fractional part and within safe range.
  if(result == (double)(long long)result
      && result >= -1e15 && result <= 1e15)
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET " = " CLR_GREEN "%lld" CLR_RESET,
        expr, (long long)result);
  else
    snprintf(line, sizeof(line),
        CLR_BOLD "%s" CLR_RESET " = " CLR_GREEN "%.10g" CLR_RESET,
        expr, result);

  cmd_reply(ctx, line);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

static bool
math_init(void)
{
  if(cmd_register(MATH_CTX, "math",
      "math <expression>",
      "Evaluate a mathematical expression",
      "Evaluates the given mathematical expression and returns\n"
      "the result. Supports standard arithmetic operators\n"
      "(+, -, *, /, ^, %), parentheses, and built-in functions:\n"
      "\n"
      "  abs, acos, asin, atan, atan2, ceil, cos, cosh, e, exp,\n"
      "  fac, floor, ln, log, log10, ncr, npr, pi, pow, sin,\n"
      "  sinh, sqrt, tan, tanh\n"
      "\n"
      "Examples:\n"
      "  !math 2 + 3 * 4\n"
      "  !math sqrt(144)\n"
      "  !math pi * 2^8\n"
      "  !math ncr(10, 3)",
      "everyone", 0, CMD_SCOPE_ANY, METHOD_T_ANY, math_cmd, NULL, NULL, "ma",
      math_ad, 1) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static void
math_deinit(void)
{
  cmd_unregister("math");
}

// -----------------------------------------------------------------------
// Plugin descriptor
// -----------------------------------------------------------------------

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "misc_math",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "misc_math",
  .provides        = { { .name = "misc_misc_math" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_command" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = math_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = math_deinit,
  .ext             = NULL,
};
