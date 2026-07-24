// botmanager — MIT
// Chat bot memory store: admin commands (/memory fact …, /memory forget …,
// /show user {facts,log,rag}, /show memory).

#define MEMORY_INTERNAL
#include "memory.h"

#include "bot.h"
#include "botmanctl.h"
#include "cmd.h"
#include "db.h"
#include "method.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Commands

static userns_t *
memory_resolve_ns(const cmd_ctx_t *ctx)
{
  if(ctx->bot != NULL && ctx->msg != NULL && ctx->msg->inst != NULL)
  {
    const char *cd_name = bot_session_get_userns_cd(ctx->bot,
        ctx->msg->inst, ctx->msg->sender);
    userns_t *ns;

    if(cd_name != NULL && cd_name[0] != '\0')
    {
      userns_t *ns = userns_find(cd_name);
      if(ns != NULL)
        return(ns);
    }

    ns = bot_get_userns(ctx->bot);
    if(ns != NULL)
      return(ns);
  }

  else
  {
    const char *session_ns = botmanctl_get_user_ns();

    if(session_ns[0] != '\0')
    {
      userns_t *ns = userns_find(session_ns);
      if(ns != NULL)
        return(ns);
    }
  }

  cmd_reply(ctx, "no namespace set \xe2\x80\x94 use /user cd <namespace>");
  return(NULL);
}

// Look up a user id by (ns, username). Returns 0 on not found.
static int
memory_lookup_user_id(const userns_t *ns, const char *username, uint32_t *ns_id_out)
{
  int id;
  db_result_t *res;
  char sql[512];
  char *e_name;

  if(ns == NULL || username == NULL || username[0] == '\0')
    return(0);

  e_name = db_escape(username);

  if(e_name == NULL)
    return(0);

  snprintf(sql, sizeof(sql),
      "SELECT id, ns_id FROM userns_user"
      " WHERE username = '%s' AND ns_id = %u",
      e_name, (unsigned)ns->id);

  mem_free(e_name);

  id = 0;
  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *v = db_result_get(res, 0, 0);

    if(v != NULL)
      id = (int)strtol(v, NULL, 10);

    if(ns_id_out != NULL)
    {
      v = db_result_get(res, 0, 1);

      if(v != NULL)
        *ns_id_out = (uint32_t)strtoul(v, NULL, 10);
    }
  }

  db_result_free(res);
  return(id);
}

static const char *
memory_kind_name(mem_fact_kind_t k)
{
  switch(k)
  {
    case MEM_FACT_PREFERENCE: return("preference");
    case MEM_FACT_ATTRIBUTE:  return("attribute");
    case MEM_FACT_RELATION:   return("relation");
    case MEM_FACT_EVENT:      return("event");
    case MEM_FACT_OPINION:    return("opinion");
    case MEM_FACT_FREEFORM:   return("freeform");
  }

  return("?");
}

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

void
memory_show_user_facts(const cmd_ctx_t *ctx, userns_t *ns,
    const char *username)
{
  mem_fact_t facts[32];
  size_t n;
  int user_id;

  if(ns == NULL || username == NULL || username[0] == '\0')
  {
    cmd_reply(ctx, "usage: show user <name> facts");
    return;
  }

  user_id = memory_lookup_user_id(ns, username, NULL);

  if(user_id == 0)
  {
    cmd_reply(ctx, "error: unknown user");
    return;
  }

  n = memory_get_facts(user_id, MEM_FACT_KIND_ANY, facts, 32);

  if(n == 0)
  {
    cmd_reply(ctx, "(no facts)");
    return;
  }

  for(size_t i = 0; i < n; i++)
  {
    char line[1400];
    snprintf(line, sizeof(line),
        "[%" PRId64 "] %s/%s = %s  (conf=%.2f src=%s chan=%s)",
        facts[i].id,
        memory_kind_name(facts[i].kind),
        facts[i].fact_key,
        facts[i].fact_value,
        (double)facts[i].confidence,
        facts[i].source,
        facts[i].channel[0] ? facts[i].channel : "(dm)");
    cmd_reply(ctx, line);
  }
}

// /memory fact del <id>
static void
cmd_memory_fact_del(const cmd_ctx_t *ctx)
{
  int64_t id;

  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: memory fact del <id>");
    return;
  }

  id = (int64_t)strtoll(ctx->parsed->argv[0], NULL, 10);

  if(memory_forget_fact(id) != SUCCESS)
  {
    cmd_reply(ctx, "error: delete failed");
    return;
  }

  cmd_reply(ctx, "ok");
}

// /memory fact set <user> <kind> <key> <value> [conf]
static void
cmd_memory_fact_set(const cmd_ctx_t *ctx)
{
  mem_fact_t f;
  float conf;
  mem_fact_kind_t k;
  int user_id;
  userns_t *ns;

  if(ctx->parsed == NULL || ctx->parsed->argc < 4)
  {
    cmd_reply(ctx, "usage: memory fact set <user> <kind> <key> <value> [conf]");
    return;
  }

  ns = memory_resolve_ns(ctx);
  if(ns == NULL) return;

  user_id = memory_lookup_user_id(ns, ctx->parsed->argv[0], NULL);

  if(user_id == 0)
  {
    cmd_reply(ctx, "error: unknown user");
    return;
  }

  if(memory_kind_from_name(ctx->parsed->argv[1], &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: kind must be preference|attribute|relation|event|opinion|freeform");
    return;
  }

  conf = 1.0f;

  if(ctx->parsed->argc >= 5)
  {
    conf = (float)strtod(ctx->parsed->argv[4], NULL);

    if(conf <= 0.0f || conf > 1.0f)
      conf = 1.0f;
  }

  memset(&f, 0, sizeof(f));
  f.user_id    = user_id;
  f.kind       = k;
  snprintf(f.fact_key,   sizeof(f.fact_key),   "%s", ctx->parsed->argv[2]);
  snprintf(f.fact_value, sizeof(f.fact_value), "%s", ctx->parsed->argv[3]);
  snprintf(f.source,     sizeof(f.source),     "admin_seed");
  f.confidence = conf;

  if(memory_upsert_fact(&f, MEM_MERGE_REPLACE) != SUCCESS)
  {
    cmd_reply(ctx, "error: upsert failed");
    return;
  }

  cmd_reply(ctx, "ok");
}

void
memory_show_user_log(const cmd_ctx_t *ctx, userns_t *ns,
    const char *username, uint32_t limit)
{
  db_result_t *res;
  char sql[512];
  int user_id;

  if(ns == NULL || username == NULL || username[0] == '\0')
  {
    cmd_reply(ctx, "usage: show user <name> log [limit]");
    return;
  }

  user_id = memory_lookup_user_id(ns, username, NULL);

  if(user_id == 0)
  {
    cmd_reply(ctx, "error: unknown user");
    return;
  }

  if(limit == 0 || limit > 200)
    limit = 20;

  snprintf(sql, sizeof(sql),
      "SELECT kind, channel, method, text,"
      " EXTRACT(EPOCH FROM ts)::bigint"
      " FROM conversation_log WHERE user_id = %d"
      " ORDER BY ts DESC LIMIT %u", user_id, limit);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    cmd_reply(ctx, res->error[0] ? res->error : "query failed");
    db_result_free(res);
    return;
  }

  if(res->rows == 0)
  {
    cmd_reply(ctx, "(no log entries)");
    db_result_free(res);
    return;
  }

  for(uint32_t i = 0; i < res->rows; i++)
  {
    const char *kind_s = db_result_get(res, i, 0);
    const char *chan_s = db_result_get(res, i, 1);
    const char *meth_s = db_result_get(res, i, 2);
    const char *text_s = db_result_get(res, i, 3);
    const char *ts_s   = db_result_get(res, i, 4);

    int kind = kind_s != NULL ? (int)strtol(kind_s, NULL, 10) : 0;
    const char *kname = kind == 0 ? "WIT" : kind == 1 ? "IN " : "OUT";

    char line[1400];
    snprintf(line, sizeof(line),
        "[%s] %s %s %s: %s",
        ts_s ? ts_s : "?",
        kname,
        meth_s ? meth_s : "",
        chan_s && chan_s[0] ? chan_s : "(dm)",
        text_s ? text_s : "");
    cmd_reply(ctx, line);
  }

  db_result_free(res);
}

// /memory forget user <user>
static void
cmd_memory_forget_user(const cmd_ctx_t *ctx)
{
  int user_id;
  userns_t *ns;

  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: memory forget user <user>");
    return;
  }

  ns = memory_resolve_ns(ctx);
  if(ns == NULL) return;

  user_id = memory_lookup_user_id(ns, ctx->parsed->argv[0], NULL);

  if(user_id == 0)
  {
    cmd_reply(ctx, "error: unknown user");
    return;
  }

  if(memory_forget_user(user_id) != SUCCESS)
  {
    cmd_reply(ctx, "error: forget failed");
    return;
  }

  cmd_reply(ctx, "ok");
}

// /memory rag <user> <query>  — synchronous debug.
typedef struct
{
  const cmd_ctx_t *ctx;
  size_t           n_facts;
  size_t           n_msgs;
} memory_rag_sink_t;

static void
memory_rag_cb(const mem_fact_t *facts, size_t n_facts,
    const mem_msg_t *msgs, size_t n_msgs, void *user)
{
  memory_rag_sink_t *s;

  (void)facts;
  (void)msgs;
  s = user;
  s->n_facts = n_facts;
  s->n_msgs  = n_msgs;
}

void
memory_show_user_rag(const cmd_ctx_t *ctx, userns_t *ns,
    const char *username, const char *query)
{
  char line[256];
  memory_rag_sink_t sink = { .ctx = ctx, .n_facts = 0, .n_msgs = 0 };
  mem_cfg_t cfg;
  uint32_t ns_id;
  int user_id;

  if(ns == NULL || username == NULL || username[0] == '\0'
      || query == NULL || query[0] == '\0')
  {
    cmd_reply(ctx, "usage: show user <name> rag <query>");
    return;
  }

  ns_id = 0;
  user_id = memory_lookup_user_id(ns, username, &ns_id);

  if(user_id == 0)
  {
    cmd_reply(ctx, "error: unknown user");
    return;
  }

  memory_cfg_snapshot(&cfg);

  if(memory_retrieve((int)ns_id, user_id, query,
      cfg.rag_top_k, memory_rag_cb, &sink) != SUCCESS)
  {
    cmd_reply(ctx, "error: retrieve failed");
    return;
  }

  snprintf(line, sizeof(line),
      "rag: %zu fact(s), %zu msg(s)",
      sink.n_facts, sink.n_msgs);
  cmd_reply(ctx, line);
}

// /show memory
static void
cmd_show_memory(const cmd_ctx_t *ctx)
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

// Argument descriptors + registration
//
// User-scoped read commands (facts, log, rag) live under /show user as
// verb dispatch (see core/userns_cmd.c). User-scoped mutators (fact set,
// fact del, forget) live under /user. Only the subsystem state view
// (/show memory) is registered here.

static const cmd_arg_desc_t ad_user_fact_del[] = {
  { "id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 20, NULL },
};

static const cmd_arg_desc_t ad_user_fact_set[] = {
  { "user",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ,      NULL },
  { "kind",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, 32,                  NULL },
  { "key",   CMD_ARG_NONE,  CMD_ARG_REQUIRED, MEM_FACT_KEY_SZ,     NULL },
  { "value", CMD_ARG_NONE,  CMD_ARG_REQUIRED, MEM_FACT_VALUE_SZ,   NULL },
  { "conf",  CMD_ARG_NONE,  CMD_ARG_OPTIONAL, 16,                  NULL },
};

static const cmd_arg_desc_t ad_user_forget[] = {
  { "user", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ, NULL },
};

void
memory_register_cmds_internal(void)
{
  // /user fact (container for set + del).
  cmd_register("memory", "fact",
      "user fact",
      "Fact manipulation (set, del)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      NULL, NULL, "user", NULL, NULL, 0, NULL, NULL);

  cmd_register("memory", "set",
      "user fact set <user> <kind> <key> <value> [conf]",
      "Admin-seed a fact (source=admin_seed, never decayed)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_memory_fact_set, NULL, "user/fact", "s", ad_user_fact_set,
      (uint8_t)(sizeof(ad_user_fact_set) / sizeof(ad_user_fact_set[0])), NULL, NULL);

  cmd_register("memory", "del",
      "user fact del <id>",
      "Delete a fact by id",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_memory_fact_del, NULL, "user/fact", "d", ad_user_fact_del,
      (uint8_t)(sizeof(ad_user_fact_del) / sizeof(ad_user_fact_del[0])), NULL, NULL);

  // /user forget <user> — destructive wipe of facts + log.
  cmd_register("memory", "forget",
      "user forget <user>",
      "Delete all facts and log rows for a user",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_memory_forget_user, NULL, "user", NULL,
      ad_user_forget,
      (uint8_t)(sizeof(ad_user_forget) / sizeof(ad_user_forget[0])), NULL, NULL);

  // /show memory — subsystem state (decay, totals).
  cmd_register("memory", "memory",
      "show memory",
      "Show memory subsystem state",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_memory, NULL, "show", "mem", NULL, 0, NULL, NULL);
}
