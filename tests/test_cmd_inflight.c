// botmanager — MIT
// Cases for the in-flight command accounting (include/cmd.h).
//
// plugin_quiesce() asks cmd_inflight_owned() whether a command handler
// is executing inside the mapping it is about to dlclose. If the answer
// is wrong there is no error path and no log line: the unload proceeds,
// the mapping goes away, and the handler resumes at an address that is
// no longer code. Measured 2026-08-16 — the fault landed inside libc,
// so even a backtrace named the wrong subsystem (OBS-15).
//
// A mapping is only an address range, so no dlopen is needed here: a
// handler brackets its own function address and asks about it. Every
// row drives the shipped path, cmd_invoke().

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"

#include <pthread.h>
#include <string.h>

#define SUITE "cmd_inflight"

// What a handler observed about itself while it was running. Read after
// cmd_invoke() returns, so a failing row still reports rather than
// aborting inside the callback.
typedef struct
{
  uint32_t self;          // count over a range containing the handler
  uint32_t excluded;      // count over a range that does not
  uint32_t empty;         // count over lo >= hi
  char     name[CMD_NAME_SZ];
} probe_t;

static probe_t         probe;
static pthread_mutex_t both_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  both_cond  = PTHREAD_COND_INITIALIZER;
static uint32_t        both_arrived;
static uint32_t        both_read;
static uint32_t        both_count[2];

// The address of a cmd_cb_t, as the barrier computes it.
static uintptr_t
cb_addr(cmd_cb_t cb)
{
  return((uintptr_t)fn_addr(&cb));
}

static void
probe_cmd(const cmd_ctx_t *ctx)
{
  uintptr_t a = cb_addr(probe_cmd);

  (void)ctx;

  probe.self     = cmd_inflight_owned(a, a + 1, probe.name,
      sizeof(probe.name));
  probe.excluded = cmd_inflight_owned(a + 1, a + 2, NULL, 0);
  probe.empty    = cmd_inflight_owned(a, a, NULL, 0);
}

// Both threads park here until the other has arrived, read the count,
// then park again before returning — so both readings are taken with
// two handlers genuinely inside the mapping at once, and neither can
// leave the list while the other is still counting.
static void
pair_cmd(const cmd_ctx_t *ctx)
{
  uintptr_t a = cb_addr(pair_cmd);
  uint32_t  slot;

  (void)ctx;

  pthread_mutex_lock(&both_mutex);
  slot = both_arrived++;
  pthread_cond_broadcast(&both_cond);

  while(both_arrived < 2)
    pthread_cond_wait(&both_cond, &both_mutex);

  pthread_mutex_unlock(&both_mutex);

  both_count[slot] = cmd_inflight_owned(a, a + 1, NULL, 0);

  pthread_mutex_lock(&both_mutex);
  both_read++;
  pthread_cond_broadcast(&both_cond);

  while(both_read < 2)
    pthread_cond_wait(&both_cond, &both_mutex);

  pthread_mutex_unlock(&both_mutex);
}

static const cmd_def_t *pair_def;

static void *
pair_entry(void *arg)
{
  cmd_ctx_t ctx;

  (void)arg;

  memset(&ctx, 0, sizeof(ctx));
  ctx.args = "";
  cmd_invoke(pair_def, &ctx);
  return(NULL);
}

static void
case_handler_sees_itself(void)
{
  const cmd_def_t *def = cmd_find("tinflight");
  cmd_ctx_t        ctx;
  uintptr_t        a = cb_addr(probe_cmd);

  memset(&probe, 0, sizeof(probe));
  memset(&ctx, 0, sizeof(ctx));
  ctx.args = "";

  cmd_invoke(def, &ctx);

  test_check_sz(SUITE, "a running handler counts itself",
      1, probe.self);

  test_check_str(SUITE, "and the count names the command",
      "tinflight", probe.name);

  test_check_sz(SUITE, "a range past the handler counts nothing",
      0, probe.excluded);

  test_check_sz(SUITE, "an empty range counts nothing",
      0, probe.empty);

  test_check_sz(SUITE, "the count clears when the handler returns",
      0, cmd_inflight_owned(a, a + 1, NULL, 0));
}

// The parse runs before the callback and reads the plugin's own
// arg_desc, so the record has to be open across a refusal too — and it
// still has to close.
static void
case_parse_refusal_still_leaves(void)
{
  const cmd_def_t *def = cmd_find("tinflightarg");
  cmd_ctx_t        ctx;
  uintptr_t        a = cb_addr(probe_cmd);

  memset(&probe, 0, sizeof(probe));
  memset(&ctx, 0, sizeof(ctx));
  ctx.args = "";        // required argument missing: parse refuses

  cmd_invoke(def, &ctx);

  test_check_sz(SUITE, "a refused parse never reaches the handler",
      0, probe.self);

  test_check_sz(SUITE, "and leaves nothing behind on the list",
      0, cmd_inflight_owned(a, a + 1, NULL, 0));
}

static void
case_two_at_once(void)
{
  pthread_t a;
  pthread_t b;
  uintptr_t addr = cb_addr(pair_cmd);

  pair_def     = cmd_find("tinflightpair");
  both_arrived = 0;
  both_read    = 0;
  memset(both_count, 0, sizeof(both_count));

  pthread_create(&a, NULL, pair_entry, NULL);
  pthread_create(&b, NULL, pair_entry, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);

  test_check_sz(SUITE, "two concurrent handlers both count (first)",
      2, both_count[0]);

  test_check_sz(SUITE, "two concurrent handlers both count (second)",
      2, both_count[1]);

  test_check_sz(SUITE, "and both clear",
      0, cmd_inflight_owned(addr, addr + 1, NULL, 0));
}

static const cmd_arg_desc_t need_one[] = {
  { "word", CMD_ARG_NONE, CMD_ARG_REQUIRED, 0, NULL },
};

static const cmd_decl_t tinflight_decl = {
  .module      = "test",
  .name        = "tinflight",
  .usage       = "tinflight",
  .description = "probe",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = probe_cmd,
};

static const cmd_decl_t tinflightarg_decl = {
  .module      = "test",
  .name        = "tinflightarg",
  .usage       = "tinflightarg <word>",
  .description = "probe",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = probe_cmd,
  .arg_desc    = need_one,
  .arg_count   = 1,
};

static const cmd_decl_t tinflightpair_decl = {
  .module      = "test",
  .name        = "tinflightpair",
  .usage       = "tinflightpair",
  .description = "probe",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = pair_cmd,
};

int
main(void)
{
  mem_init();
  clam_init();
  cmd_init();

  cmd_register(&tinflight_decl);
  cmd_register(&tinflightarg_decl);
  cmd_register(&tinflightpair_decl);

  case_handler_sees_itself();
  case_parse_refusal_still_leaves();
  case_two_at_once();

  return(test_report(SUITE));
}
