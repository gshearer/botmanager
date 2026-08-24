// botmanager — MIT
// Cases for the maxlen contract on an argument descriptor (include/cmd.h),
// at both of its ends: what cmd_register will accept, and what the
// tokenizer does with a token that exceeds what was accepted.
//
// The tokenizer fills one CMD_ARG_SZ row per argument and stops at the
// descriptor's maxlen, so registration is the only place that can
// establish maxlen < CMD_ARG_SZ — the parser takes it on trust, exactly
// as it takes arg_count. Without the gate there is no error path and no
// log line: an over-declared argument writes across the next argument's
// row, and the handler reads an argv the token after it overwrote.
// Nine descriptors in seven plugins declared one until 2026-08-21.
//
// The parser end is the same silence from the other direction. Stopping
// at maxlen and dropping the overflow hands the callback a truncated URL
// or path that still validates — and on the quoted branch the tail was
// not dropped but re-read as the *next* argument (board #55). Every row
// below drives the shipped path, cmd_invoke().

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"

#include <string.h>

#define SUITE "cmd_argspec"

static void
noop_cmd(const cmd_ctx_t *ctx)
{
  (void)ctx;
}

// One argument each, differing only in how much it claims to hold.
static const cmd_arg_desc_t ad_default[] = {
  { "word", CMD_ARG_NONE, CMD_ARG_REQUIRED, 0, NULL },
};

static const cmd_arg_desc_t ad_at_cap[] = {
  { "word", CMD_ARG_NONE, CMD_ARG_REQUIRED, CMD_ARG_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_over_cap[] = {
  { "word", CMD_ARG_NONE, CMD_ARG_REQUIRED, CMD_ARG_SZ, NULL },
};

static const struct
{
  const char           *name;
  const char           *left;    // ...and what registering it left behind
  const char           *cmd;
  const cmd_arg_desc_t *desc;
  bool                  want;    // SUCCESS / FAIL, not true / false
} maxlen_cases[] = {
  { "maxlen 0 means the default cap",
    "and the command is registered",
    "targspecdef",  ad_default,  SUCCESS },

  { "maxlen at the row's capacity registers",
    "and that command is registered too",
    "targspeccap",  ad_at_cap,   SUCCESS },

  // One past the last byte a row can hold, which is where the write
  // lands in the next argument's row.
  { "one byte past the row is refused",
    "and leaves no command behind",
    "targspecover", ad_over_cap, FAIL },
};

// The parser end: what a handler was handed, or that it never ran.

#define PROBE_MAXLEN 8

static bool ran;
static char seen[2][CMD_ARG_SZ];

static void
record_cmd(const cmd_ctx_t *ctx)
{
  ran = true;

  for(uint8_t i = 0; i < 2; i++)
    strlcpy(seen[i], ctx->parsed != NULL && i < ctx->parsed->argc
        ? ctx->parsed->argv[i] : "", sizeof(seen[i]));
}

// Two rows so the quoted branch has somewhere to leak a tail into, and
// a rest-of-line row, which carries its own copy of the same cut.
static const cmd_arg_desc_t ad_pair[] = {
  { "first",  CMD_ARG_NONE, CMD_ARG_REQUIRED, PROBE_MAXLEN, NULL },
  { "second", CMD_ARG_NONE, CMD_ARG_OPTIONAL, PROBE_MAXLEN, NULL },
};

static const cmd_arg_desc_t ad_rest[] = {
  { "line", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, PROBE_MAXLEN, NULL },
};

static const struct
{
  const char *name;
  const char *cmd;
  const char *args;
  const char *first;    // what argv[0] must be, or NULL for "refused"
  const char *second;   // ...and argv[1], which the tail must not reach
} parse_cases[] = {
  { "a token filling maxlen exactly reaches the handler",
    "targspecpair", "12345678 tail", "12345678", "tail" },

  { "a token one byte over maxlen is refused, not cut",
    "targspecpair", "123456789", NULL, NULL },

  { "a quoted token over maxlen is refused, not spilled onto the next arg",
    "targspecpair", "\"123456789\"", NULL, NULL },

  { "a rest-of-line over maxlen is refused, not cut",
    "targspecrest", "123456789", NULL, NULL },

  { "a quoted rest-of-line over maxlen is refused too",
    "targspecrest", "\"123456789\"", NULL, NULL },
};

static void
run_parse_cases(void)
{
  for(size_t i = 0; i < sizeof(parse_cases) / sizeof(parse_cases[0]); i++)
  {
    const cmd_def_t *def = cmd_find(parse_cases[i].cmd);
    cmd_ctx_t        ctx;

    ran = false;
    memset(seen, 0, sizeof(seen));
    memset(&ctx, 0, sizeof(ctx));
    ctx.args = parse_cases[i].args;

    cmd_invoke(def, &ctx);

    if(parse_cases[i].first == NULL)
    {
      test_check_bool(SUITE, parse_cases[i].name, false, ran);
      continue;
    }

    test_check_str(SUITE, parse_cases[i].name,
        parse_cases[i].first, seen[0]);

    test_check_str(SUITE, "and the argument after it is its own token",
        parse_cases[i].second, seen[1]);
  }
}

static const cmd_decl_t targspecpair_decl = {
  .module      = "test",
  .name        = "targspecpair",
  .usage       = "targspecpair <first> [second]",
  .description = "argspec probe",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = record_cmd,
  .arg_desc    = ad_pair,
  .arg_count   = 2,
};

static const cmd_decl_t targspecrest_decl = {
  .module      = "test",
  .name        = "targspecrest",
  .usage       = "targspecrest <line>",
  .description = "argspec probe",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = record_cmd,
  .arg_desc    = ad_rest,
  .arg_count   = 1,
};

int
main(void)
{
  mem_init();
  clam_init();
  cmd_init();

  cmd_register(&targspecpair_decl);
  cmd_register(&targspecrest_decl);

  for(size_t i = 0; i < sizeof(maxlen_cases) / sizeof(maxlen_cases[0]); i++)
  {
    const cmd_decl_t decl = {
      .module      = "test",
      .name        = maxlen_cases[i].cmd,
      .usage       = maxlen_cases[i].cmd,
      .description = "argspec probe",
      .group       = USERNS_GROUP_EVERYONE,
      .scope       = CMD_SCOPE_ANY,
      .methods     = METHOD_T_ANY,
      .cb          = noop_cmd,
      .arg_desc    = maxlen_cases[i].desc,
      .arg_count   = 1,
    };
    bool got = cmd_register(&decl);

    test_check_bool(SUITE, maxlen_cases[i].name,
        maxlen_cases[i].want, got);

    // The parser reaches a descriptor only through a registered def, so
    // a refusal has to leave nothing findable behind.
    test_check_bool(SUITE, maxlen_cases[i].left,
        maxlen_cases[i].want == SUCCESS,
        cmd_find(maxlen_cases[i].cmd) != NULL);
  }

  run_parse_cases();

  return(test_report(SUITE));
}
