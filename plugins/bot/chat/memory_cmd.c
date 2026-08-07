// botmanager — MIT
// Chat bot memory store: the subsystem state view (/show memstore).

#define MEMORY_INTERNAL
#include "memory.h"

#include "cmd.h"
#include "method.h"
#include "userns.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Shared with dossier_cmd.c, which parses the same kind vocabulary for
// /dossier fact set.
bool
memory_kind_from_name(const char *s, mem_fact_kind_t *out)
{
  if(s == NULL)
    return(FAIL);

  if(strcmp(s, "preference") == 0) { *out = MEM_FACT_PREFERENCE; return(SUCCESS); }
  if(strcmp(s, "attribute")  == 0) { *out = MEM_FACT_ATTRIBUTE;  return(SUCCESS); }
  if(strcmp(s, "relation")   == 0) { *out = MEM_FACT_RELATION;   return(SUCCESS); }
  if(strcmp(s, "event")      == 0) { *out = MEM_FACT_EVENT;      return(SUCCESS); }
  if(strcmp(s, "opinion")    == 0) { *out = MEM_FACT_OPINION;    return(SUCCESS); }
  if(strcmp(s, "freeform")   == 0) { *out = MEM_FACT_FREEFORM;   return(SUCCESS); }

  return(FAIL);
}

// /show memstore
static void
cmd_show_memstore(const cmd_ctx_t *ctx)
{
  memory_stats_t s;
  char buf[512];
  mem_cfg_t cfg;

  memory_get_stats(&s);

  memory_cfg_snapshot(&cfg);

  snprintf(buf, sizeof(buf),
      "memory: facts=%lu logs=%lu sweeps=%lu forgets=%lu",
      (unsigned long)s.total_facts, (unsigned long)s.total_logs,
      (unsigned long)s.decay_sweeps, (unsigned long)s.forgets);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  enabled=%s witness_embeds=%s embed_own_replies=%s",
      cfg.enabled ? "yes" : "no",
      cfg.witness_embeds ? "yes" : "no",
      cfg.embed_own_replies ? "yes" : "no");
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  log_retention_days=%u half_life_days=%u min_conf=%.2f",
      cfg.log_retention_days, cfg.fact_decay_half_life_days,
      (double)cfg.min_fact_confidence);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  rag_top_k=%u rag_max_chars=%u sweep_interval=%us",
      cfg.rag_top_k, cfg.rag_max_context_chars,
      cfg.decay_sweep_interval_secs);
  cmd_reply(ctx, buf);

  if(memory_last_sweep != 0)
  {
    time_t now = time(NULL);
    long elapsed = (long)(now - memory_last_sweep);
    long remain  = (long)cfg.decay_sweep_interval_secs - elapsed;

    if(remain < 0)
      remain = 0;

    snprintf(buf, sizeof(buf),
        "  next decay sweep in ~%lds", remain);
    cmd_reply(ctx, buf);
  }

  else
    cmd_reply(ctx, "  decay sweep not yet run");
}

// Registration
//
// Facts are dossier-keyed: the read and write surfaces both live under
// /dossier and /show dossier. This file registers only the subsystem
// state view.
//
// That view is deliberately NOT /show memory: core's tracked allocator
// owns that name (core/alloc.c mem_register_commands) and registers
// first, so the bot-memory view was rejected as a duplicate for as long
// as it claimed it. The two "memory" concepts are distinct — AGENTS.md
// §Two Different "Memory" Concepts.

void
memory_register_cmds_internal(void)
{
  // /show memstore — bot memory subsystem state (decay, totals).
  cmd_register("memory", "memstore",
      "show memstore",
      "Show bot memory store state (facts, log, decay)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_memstore, NULL, "show", "ms", NULL, 0, NULL, NULL);
}
