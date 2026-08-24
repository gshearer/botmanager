// botmanager — MIT
// Math misc plugin: arithmetic expression evaluator exposed as a user command.
#define MATH_INTERNAL
#include "math_plugin.h"

#include <math.h>
#include <stdio.h>

#include "colors.h"

// Input validation

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

// Command handler

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

// Plugin lifecycle

// NL hint for /math. The LLM emits the expression verbatim; the chat
// plugin forwards it to /math without further translation.
static const cmd_nl_slot_t math_slots[] = {
  { .name  = "expression",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t math_examples[] = {
  { .utterance  = "what's 137 times 42?",
    .invocation = "/math 137 * 42" },
  { .utterance  = "split 750 gold six ways",
    .invocation = "/math 750 / 6" },
};

static const cmd_nl_t math_nl = {
  .when          = "User asks for arithmetic — sums, splits, conversions.",
  .syntax        = "/math <expression>",
  .slots         = math_slots,
  .slot_count    = (uint8_t)(sizeof(math_slots) / sizeof(math_slots[0])),
  .examples      = math_examples,
  .example_count = (uint8_t)(sizeof(math_examples) / sizeof(math_examples[0])),
};

static const cmd_decl_t math_decl = {
  .module      = MATH_CTX,
  .name        = "math",
  .usage       = "math <expression>",
  .description = "Evaluate a mathematical expression",
  .group       = "everyone",
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = math_cmd,
  .abbrev      = "ma",
  .arg_desc    = math_ad,
  .arg_count   = 1,
  .nl          = &math_nl,
};

static bool
math_init(void)
{
  if(cmd_register(&math_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

static void
math_deinit(void)
{
  cmd_unregister_path("math");
}

// Plugin descriptor

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "math",
  .version         = "1.0",
  .type            = PLUGIN_MISC,
  .kind            = "math",
  .provides        = { { .name = "misc_math" } },
  .provides_count  = 1,
  .requires        = { { .name = "bot_chat" } },
  .requires_count  = 1,
  .kv_schema       = NULL,
  .kv_schema_count = 0,
  .init            = math_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = math_deinit,
  .ext             = NULL,
};
