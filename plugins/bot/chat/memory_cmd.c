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

  snprintf(buf, sizeof(buf),
      "  embed_min_chars=%u embed_batch_size=%u",
      cfg.embed_min_chars, cfg.embed_batch_size);
  cmd_reply(ctx, buf);

  if(cfg.recall_top_k == 0)
    snprintf(buf, sizeof(buf), "  recall=(off - recall_top_k 0)");

  else
    snprintf(buf, sizeof(buf), "  recall_top_k=%u recall_min_cosine=%.2f",
        cfg.recall_top_k, (double)cfg.recall_min_cosine_x100 / 100.0);

  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf), "  recall_instruct=%s",
      cfg.recall_instruct[0] != '\0' ? cfg.recall_instruct : "(off)");
  cmd_reply(ctx, buf);

  if(cfg.embed_burst_max == 0)
    snprintf(buf, sizeof(buf), "  embed_burst=(off)");

  else
    snprintf(buf, sizeof(buf), "  embed_burst=%u msgs/%us",
        cfg.embed_burst_max, cfg.embed_burst_secs);

  cmd_reply(ctx, buf);

  // Report what is enforced, not what is stored: a pattern that failed
  // to compile leaves the gate off, and a state view that echoed the
  // knob would be lying about it.
  if(memory_kv_str_disabled(cfg.embed_exclude_regex))
    snprintf(buf, sizeof(buf), "  embed_exclude_regex=(off)");

  else if(!memory_exclude_regex_active())
    snprintf(buf, sizeof(buf),
        "  embed_exclude_regex=(off - '%s' does not compile)",
        cfg.embed_exclude_regex);

  else
    snprintf(buf, sizeof(buf), "  embed_exclude_regex=%s",
        cfg.embed_exclude_regex);

  cmd_reply(ctx, buf);

  memory_backfill_status(buf, sizeof(buf));
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

static const cmd_decl_t show_memstore_decl = {
  .module      = "memory",
  .name        = "memstore",
  .usage       = "show memstore",
  .description = "Show bot memory store state (facts, log, decay)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_memstore,
  .parent_path = "show",
  .abbrev      = "ms",
};

void
memory_register_cmds_internal(void)
{
  // /show memstore — bot memory subsystem state (decay, totals).
  cmd_register(&show_memstore_decl);

  // /bot <name> embedbackfill — the mutating half, so it hangs off /bot
  // rather than /show (feedback_show_vs_bot_verbs).
  memory_backfill_cmd_register();
}
