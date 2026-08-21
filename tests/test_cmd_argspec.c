// botmanager — MIT
// Cases for the argument-descriptor gate in cmd_register (include/cmd.h).
//
// The tokenizer fills one CMD_ARG_SZ row per argument and stops at the
// descriptor's maxlen, so registration is the only place that can
// establish maxlen < CMD_ARG_SZ — the parser takes it on trust, exactly
// as it takes arg_count. Without the gate there is no error path and no
// log line: an over-declared argument writes across the next argument's
// row, and the handler reads an argv the token after it overwrote.
// Nine descriptors in seven plugins declared one until 2026-08-21.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"

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

int
main(void)
{
  mem_init();
  clam_init();
  cmd_init();

  for(size_t i = 0; i < sizeof(maxlen_cases) / sizeof(maxlen_cases[0]); i++)
  {
    bool got = cmd_register("test", maxlen_cases[i].cmd,
        maxlen_cases[i].cmd, "argspec probe", NULL,
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        noop_cmd, NULL, NULL, NULL, maxlen_cases[i].desc, 1, NULL, NULL);

    test_check_bool(SUITE, maxlen_cases[i].name,
        maxlen_cases[i].want, got);

    // The parser reaches a descriptor only through a registered def, so
    // a refusal has to leave nothing findable behind.
    test_check_bool(SUITE, maxlen_cases[i].left,
        maxlen_cases[i].want == SUCCESS,
        cmd_find(maxlen_cases[i].cmd) != NULL);
  }

  return(test_report(SUITE));
}
