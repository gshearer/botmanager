// botmanager — MIT
// Cases for the usage line a command's help and its argument refusal
// send (cmd_reply_usage, include/cmd.h).
//
// Every site that rendered "usage: %s" used to size a stack buffer off
// CMD_USAGE_SZ and snprintf into it, so a usage string longer than 136
// bytes lost its tail with no WARN at registration and no marker in the
// output — board #75, hit while shortening /wiki's grammar line to fit.
// The usage pointer a registration hands core has no declared cap, so
// these rows drive one comfortably past the old buffer and read back
// what a collector actually received.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"

#include <string.h>

#define SUITE "cmd_usage_line"

// The longest usage in the tree is ~120 bytes; this is deliberately past
// both that and the CMD_USAGE_SZ + 16 buffer the old sites used.
#define LONG_USAGE \
  "tusage <subject> [property] | tusage <subject> ? | " \
  "tusage <property>=<value> | tusage Q<id> | tusage <person> age | " \
  "tusage --since <when> --until <when> [--limit N] | tusage --explain"

static char         seen[512];
static uint32_t     seen_count;
static cmd_result_t seen_result;

static void
collect(void *data, const char *line, cmd_result_t result)
{
  (void)data;

  seen_count++;
  seen_result = result;
  strlcpy(seen, line != NULL ? line : "", sizeof(seen));
}

static void
noop_cmd(const cmd_ctx_t *ctx)
{
  (void)ctx;
}

static const cmd_arg_desc_t ad_one[] = {
  { "subject", CMD_ARG_NONE, CMD_ARG_REQUIRED, 0, NULL },
};

static const cmd_decl_t tusage_decl = {
  .module      = "test",
  .name        = "tusage",
  .usage       = LONG_USAGE,
  .description = "usage-line probe",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = noop_cmd,
  .arg_desc    = ad_one,
  .arg_count   = 1,
};

static const struct
{
  const char *name;
  const char *usage;
  const char *want;    // the whole line, or NULL for "nothing sent"
} direct_cases[] = {
  { "a usage under the old buffer is unchanged",
    "tusage <subject>", "usage: tusage <subject>" },

  { "a usage past the old buffer keeps its tail",
    LONG_USAGE, "usage: " LONG_USAGE },

  { "an empty usage sends nothing",
    "", NULL },

  { "a NULL usage sends nothing",
    NULL, NULL },
};

int
main(void)
{
  const cmd_def_t *def;
  method_msg_t     msg;
  cmd_ctx_t        ctx;
  uint64_t         sink;

  mem_init();
  clam_init();
  cmd_init();

  cmd_register(&tusage_decl);

  sink = cmd_sink_register(collect, NULL);

  memset(&msg, 0, sizeof(msg));
  strlcpy(msg.inst_name, "test", sizeof(msg.inst_name));
  msg.reply_sink_id = sink;

  memset(&ctx, 0, sizeof(ctx));
  ctx.msg = &msg;

  for(size_t i = 0; i < sizeof(direct_cases) / sizeof(direct_cases[0]); i++)
  {
    seen[0]    = '\0';
    seen_count = 0;

    cmd_reply_usage(&ctx, direct_cases[i].usage);

    if(direct_cases[i].want == NULL)
    {
      test_check_sz(SUITE, direct_cases[i].name, 0, seen_count);
      continue;
    }

    test_check_str(SUITE, direct_cases[i].name, direct_cases[i].want, seen);
  }

  // The shipped path: a required argument left off. The usage line IS
  // the refusal, so it must carry both the whole string and the result
  // that stops a consumer relaying it as an answer.
  def         = cmd_find("tusage");
  seen[0]     = '\0';
  seen_count  = 0;
  seen_result = CMD_RESULT_OK;

  memset(&ctx, 0, sizeof(ctx));
  ctx.msg  = &msg;
  ctx.args = "";

  cmd_invoke(def, &ctx);

  test_check_str(SUITE, "a missing required argument replies the whole usage",
      "usage: " LONG_USAGE, seen);

  test_check_bool(SUITE, "and declares itself a refusal",
      true, seen_result == CMD_RESULT_REFUSED);

  cmd_sink_unregister(sink);

  return(test_report(SUITE));
}
