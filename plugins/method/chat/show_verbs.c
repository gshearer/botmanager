// botmanager — MIT
// /show bot <name> <verb> handlers for chat-kind bots (summary, personas, …).

#define CHATBOT_INTERNAL
#include "chatbot.h"
#include "colors.h"
#include "db.h"
#include "inference.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ---- /show bot <name> llm (kind summary) -------------------------------

static void
verb_llm_summary(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  const char *nl;
  const char *nl_state = "disabled";
  const char *model;
  uint64_t mute_until;
  time_t now;
  uint32_t in_flight;
  const char *pname;
  char line[512];
  char key[128];
  const char *name;

  (void)rest;

  name = bot_inst_name(bot);

  cmd_reply(ctx, CLR_BOLD "chat bot" CLR_RESET);

  // Active persona.
  snprintf(key, sizeof(key), "bot.%s.behavior.personality", name);
  pname = kv_get_str(key);
  if(pname != NULL && pname[0] != '\0')
  {
    chatbot_personality_t p = {0};
    if(chatbot_personality_read(pname, &p) == SUCCESS)
    {
      snprintf(line, sizeof(line),
          "  persona:     " CLR_BOLD "%s" CLR_RESET " v%d  %s",
          p.name, p.version, p.description);
      cmd_reply(ctx, line);
      chatbot_personality_free(&p);
    }

    else
    {
      snprintf(line, sizeof(line),
          "  persona:     " CLR_BOLD "%s" CLR_RESET
          " " CLR_YELLOW "(not loaded)" CLR_RESET, pname);
      cmd_reply(ctx, line);
    }
  }

  else
    cmd_reply(ctx, "  persona:     " CLR_GRAY "(none)" CLR_RESET);

  // Bound methods count.
  snprintf(line, sizeof(line), "  methods:     %u",
      bot_method_count(bot));
  cmd_reply(ctx, line);

  // In-flight replies.
  in_flight = chatbot_inflight_get((chatbot_state_t *)bot_get_handle(bot));
  snprintf(line, sizeof(line), "  in_flight:   %u", in_flight);
  cmd_reply(ctx, line);

  // Mute state.
  snprintf(key, sizeof(key), "bot.%s.behavior.mute_until", name);
  mute_until = kv_get_uint(key);
  now = time(NULL);
  if(mute_until > (uint64_t)now)
  {
    long secs = (long)(mute_until - (uint64_t)now);
    snprintf(line, sizeof(line),
        "  mute:        " CLR_YELLOW "muted" CLR_RESET " (%lds remaining)",
        secs);
  }

  else
    snprintf(line, sizeof(line),
        "  mute:        " CLR_GREEN "open" CLR_RESET);
  cmd_reply(ctx, line);

  // Interject probability.
  snprintf(key, sizeof(key), "bot.%s.behavior.speak.interject_prob", name);
  snprintf(line, sizeof(line), "  verbosity:   %lu  (interject_prob)",
      (unsigned long)kv_get_uint(key));
  cmd_reply(ctx, line);

  // Chat model.
  snprintf(key, sizeof(key), "bot.%s.chat_model", name);
  model = kv_get_str(key);
  snprintf(line, sizeof(line), "  chat_model:  %s",
      (model && model[0]) ? model : "(default)");
  cmd_reply(ctx, line);

  // NL bridge.
  snprintf(key, sizeof(key), "bot.%s.behavior.nl_bridge_cmds", name);
  nl = kv_get_str(key);

  if(nl == NULL || nl[0] == '\0')
    ; // nl_state stays "disabled"
  else if(strcmp(nl, "*") == 0)
    nl_state = "enabled (all NL-capable)";
  else
    nl_state = nl;

  snprintf(line, sizeof(line), "  nl_bridge:   %s", nl_state);
  cmd_reply(ctx, line);
}

// ---- /show bot <name> model -------------------------------------------
//
// Single-line report of the LLM bound to this bot: bot.<name>.chat_model
// when set, otherwise llm.default_chat_model. Everyone-gated so the NL
// bridge can route "what LLM are you?" here and the bot can speak a
// factual answer rather than confabulating. The full `llm` summary
// already shows this alongside other state; this verb exists so the
// NL bridge has a narrow, non-admin entry point.

static void
verb_model(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  const char *model;
  const char *name;
  char        key[128];
  char        line[256];

  (void)rest;

  name = bot_inst_name(bot);

  snprintf(key, sizeof(key), "bot.%s.chat_model", name);

  model = kv_get_str(key);

  if(model == NULL || model[0] == '\0')
    model = kv_get_str("llm.default_chat_model");

  if(model == NULL || model[0] == '\0')
  {
    cmd_reply(ctx, "no model configured");
    return;
  }

  snprintf(line, sizeof(line), "%s", model);
  cmd_reply(ctx, line);
}

// ---- /show bot <name> llm personas -------------------------------------

static void
verb_llm_personas(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  const char *active;
  char key[128];
  const char *name;

  (void)rest;

  name = bot_inst_name(bot);
  snprintf(key, sizeof(key), "bot.%s.behavior.personality", name);
  active = kv_get_str(key);

  if(active == NULL || active[0] == '\0')
    cmd_reply(ctx, "  " CLR_GRAY "(no active persona)" CLR_RESET);

  else
  {
    chatbot_personality_t p = {0};
    char line[512];
    if(chatbot_personality_read(active, &p) == SUCCESS)
    {
      snprintf(line, sizeof(line),
          "  " CLR_BOLD "%s" CLR_RESET " v%d  %s  " CLR_GREEN "(active)" CLR_RESET,
          p.name, p.version, p.description);
      cmd_reply(ctx, line);
      chatbot_personality_free(&p);
    }

    else
    {
      snprintf(line, sizeof(line),
          "  " CLR_BOLD "%s" CLR_RESET
          " " CLR_YELLOW "(not loaded)" CLR_RESET " " CLR_GREEN "(active)" CLR_RESET,
          active);
      cmd_reply(ctx, line);
    }
  }

  cmd_reply(ctx,
      CLR_GRAY "(see /show personalities for the full catalog)" CLR_RESET);
}

// ---- helpers shared by remaining verbs --------------------------------

static void
render_fact_kind_short(char *buf, size_t sz, mem_fact_kind_t k)
{
  const char *s;
  switch(k)
  {
    case MEM_FACT_PREFERENCE: s = "preference"; break;
    case MEM_FACT_ATTRIBUTE:  s = "attribute";  break;
    case MEM_FACT_RELATION:   s = "relation";   break;
    case MEM_FACT_EVENT:      s = "event";      break;
    case MEM_FACT_OPINION:    s = "opinion";    break;
    case MEM_FACT_FREEFORM:   s = "freeform";   break;
    default:                  s = "?";          break;
  }
  snprintf(buf, sz, "%s", s);
}

// ---- /show bot <name> llm memories [<query>] --------------------------

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} mem_rag_state_t;

static void
mem_rag_cb(const mem_fact_t *facts, size_t n_facts,
    const mem_msg_t *msgs, size_t n_msgs, void *user)
{
  mem_rag_state_t *st = user;
  char line[1400];

  for(size_t i = 0; i < n_facts; i++)
  {
    char kind[32];
    render_fact_kind_short(kind, sizeof(kind), facts[i].kind);
    snprintf(line, sizeof(line),
        "fact  %s/%s = %s  (conf=%.2f)",
        kind, facts[i].fact_key, facts[i].fact_value,
        (double)facts[i].confidence);
    cmd_reply(st->ctx, line);
    st->count++;
  }

  for(size_t i = 0; i < n_msgs; i++)
  {
    snprintf(line, sizeof(line),
        "msg   [%s] %s",
        msgs[i].channel[0] ? msgs[i].channel : "(dm)",
        msgs[i].text);
    cmd_reply(st->ctx, line);
    st->count++;
  }
}

static void
verb_llm_memories(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  userns_t *ns = bot_get_userns(bot);
  mem_rag_state_t st = { .ctx = ctx, .count = 0 };

  if(ns == NULL) { cmd_reply(ctx, "bot has no userns bound"); return; }

  while(*rest == ' ' || *rest == '\t') rest++;

  if(*rest == '\0')
  {
    // Recent conversation_log for this bot's namespace.
    db_result_t *r = db_result_alloc();
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT EXTRACT(EPOCH FROM ts)::bigint, kind, channel, text"
        " FROM conversation_log"
        " WHERE ns_id = %u AND bot_name = '%s'"
        " ORDER BY ts DESC LIMIT 20",
        ns->id, bot_inst_name(bot));

    if(db_query(sql, r) != SUCCESS || !r->ok)
    {
      cmd_reply(ctx, "conversation_log query failed (see logs)");
      db_result_free(r);
      return;
    }

    if(r->rows == 0)
    {
      cmd_reply(ctx, "(no log entries)");
      db_result_free(r);
      return;
    }

    for(uint32_t i = 0; i < r->rows; i++)
    {
      const char *ts   = db_result_get(r, i, 0);
      const char *kind = db_result_get(r, i, 1);
      const char *chan = db_result_get(r, i, 2);
      const char *text = db_result_get(r, i, 3);

      char line[1400];
      snprintf(line, sizeof(line),
          "[%s] k=%s chan=%s  %s",
          ts ? ts : "?",
          kind ? kind : "?",
          (chan && chan[0]) ? chan : "(dm)",
          text ? text : "");
      cmd_reply(ctx, line);
    }
    db_result_free(r);
    return;
  }

  // Query path: RAG over facts + messages for this namespace.
  if(memory_retrieve((int)ns->id, 0, rest, 10, mem_rag_cb, &st) != SUCCESS)
  {
    cmd_reply(ctx, "retrieve failed");
    return;
  }
  if(st.count == 0)
    cmd_reply(ctx, "(no hits)");
}

// ---- /show bot <name> stats --------------------------------------------

static void
verb_stats(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  uint32_t in_flight;
  char line[256];
  const char *name;
  userns_t *ns;

  (void)rest;

  // Mimick-local per-instance counters: in-flight LLM requests + last
  // reply timestamps per channel are internal; we expose the aggregate
  // as in-flight only. Extend with coalesce/cooldown detail when a use
  // case lands.

  name = bot_inst_name(bot);
  ns = bot_get_userns(bot);

  cmd_reply(ctx, CLR_BOLD "chat bot stats" CLR_RESET);
  snprintf(line, sizeof(line),
      "  name=%s  namespace=%s",
      name,
      (ns && ns->name[0]) ? ns->name : "-");
  cmd_reply(ctx, line);

  // in-flight is tracked in chatbot_state_t (opaque to this TU via the
  // accessor). No direct accessor exposed to core yet -- use the stats
  // block attached to the bot for now.
  in_flight = chatbot_inflight_get((chatbot_state_t *)bot_get_handle(bot));
  snprintf(line, sizeof(line), "  in_flight_replies=%u", in_flight);
  cmd_reply(ctx, line);

  // Dossier / memory counters keyed by this bot's userns.
  if(ns != NULL)
  {
    db_result_t *r = db_result_alloc();
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM dossier WHERE ns_id = %u", ns->id);
    if(db_query(sql, r) == SUCCESS && r->ok && r->rows > 0)
    {
      snprintf(line, sizeof(line), "  dossiers=%s",
          db_result_get(r, 0, 0));
      cmd_reply(ctx, line);
    }
    db_result_free(r);

    r = db_result_alloc();
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM dossier_facts pf"
        " JOIN dossier p ON pf.dossier_id = p.id"
        " WHERE p.ns_id = %u", ns->id);
    if(db_query(sql, r) == SUCCESS && r->ok && r->rows > 0)
    {
      snprintf(line, sizeof(line), "  dossier_facts=%s",
          db_result_get(r, 0, 0));
      cmd_reply(ctx, line);
    }
    db_result_free(r);
  }
}

// ---- /show bot <name> llm knowledge [<query>] -------------------------
// Preview the corpus the active persona is bound to. Without a query,
// shows the bound corpus name and the three most-recently-ingested
// chunks. With a query, runs a real knowledge_retrieve and prints the
// top hits with cosine scores. Mirrors `llm memories` but over the
// corpus-scoped embedding table rather than per-user facts.

// Heap-owned state that survives the verb's return. `knowledge_retrieve`
// submits the query embed to the curl worker pool and fires `kw_rag_cb`
// from that thread after the verb has already returned and `cmd_ctx_t`
// is gone. We snapshot the reply target (method instance + channel/DM
// destination) up front and use `method_send` directly from the cb.
typedef struct
{
  method_inst_t *inst;
  char           target[METHOD_SENDER_SZ];
} kw_rag_state_t;

static void
kw_rag_cb(const knowledge_chunk_t *chunks, size_t n, void *user)
{
  kw_rag_state_t *st = user;
  char line[1400];

  for(size_t i = 0; i < n; i++)
  {
    snprintf(line, sizeof(line),
        "[%.3f] %s%s%s — %.*s",
        (double)chunks[i].score,
        chunks[i].section_heading[0] ? "[" : "",
        chunks[i].section_heading,
        chunks[i].section_heading[0] ? "] " : "",
        (int)(sizeof(line) > 256 ? 256 : sizeof(line) - 1),
        chunks[i].text);
    method_send(st->inst, st->target, line);
  }

  if(n == 0)
    method_send(st->inst, st->target, "(no hits)");

  mem_free(st);
}

static void
verb_llm_knowledge(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  const char *name = bot_inst_name(bot);
  char key[128];
  kw_rag_state_t *st;
  char line[512];
  const char *cl;
  char corpus_list[CHATBOT_CORPUS_LIST_SZ];

  // Read the bot's corpus list from KV (semicolon-separated).
  snprintf(key, sizeof(key), "bot.%s.corpus", name);
  cl = kv_get_str(key);
  snprintf(corpus_list, sizeof(corpus_list), "%s", cl ? cl : "");

  if(corpus_list[0] == '\0')
  {
    cmd_reply(ctx,
        "no corpus bound -- set bot.<name>.corpus"
        " (semicolon-separated corpus list)");
    return;
  }

  snprintf(line, sizeof(line),
      "corpus list: " CLR_BOLD "%s" CLR_RESET, corpus_list);
  cmd_reply(ctx, line);

  while(*rest == ' ' || *rest == '\t') rest++;

  if(*rest == '\0')
  {
    db_result_t *r;
    char sql[1400];
    char *saveptr;
    bool first;

    // Preview: most-recently-ingested chunks across all named corpora.
    // Walk the list ourselves so the SQL stays simple (one IN clause).
    char corpora_sql[1024] = "(";
    size_t pos = 1;
    char list_copy[CHATBOT_CORPUS_LIST_SZ];
    snprintf(list_copy, sizeof(list_copy), "%s", corpus_list);

    saveptr = NULL;
    first = true;
    for(char *tok = strtok_r(list_copy, ";", &saveptr);
        tok != NULL;
        tok = strtok_r(NULL, ";", &saveptr))
    {
      char *e;
      size_t tlen;

      while(*tok == ' ' || *tok == '\t') tok++;
      tlen = strlen(tok);
      while(tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t'))
        tok[--tlen] = '\0';
      if(tok[0] == '\0') continue;

      e = db_escape(tok);
      if(e == NULL) continue;
      pos += (size_t)snprintf(corpora_sql + pos, sizeof(corpora_sql) - pos,
          "%s'%s'", first ? "" : ",", e);
      mem_free(e);
      first = false;
    }
    snprintf(corpora_sql + pos, sizeof(corpora_sql) - pos, ")");

    if(first)
    {
      cmd_reply(ctx, "  (corpus list is empty after parse)");
      return;
    }

    snprintf(sql, sizeof(sql),
        "SELECT corpus, COALESCE(section_heading, ''),"
        " SUBSTRING(text FROM 1 FOR 160)"
        " FROM knowledge_chunks WHERE corpus IN %s"
        " ORDER BY id DESC LIMIT 10", corpora_sql);

    r = db_result_alloc();
    if(db_query(sql, r) == SUCCESS && r->ok && r->rows > 0)
    {
      for(uint32_t i = 0; i < r->rows; i++)
      {
        const char *cn   = db_result_get(r, i, 0);
        const char *sec  = db_result_get(r, i, 1);
        const char *prev = db_result_get(r, i, 2);

        snprintf(line, sizeof(line),
            "  [%s]%s%s%s %s",
            cn ? cn : "?",
            (sec && sec[0]) ? " [" : "",
            sec ? sec : "",
            (sec && sec[0]) ? "]" : "",
            prev ? prev : "");
        cmd_reply(ctx, line);
      }
    }

    else
      cmd_reply(ctx, "  (no chunks — run /knowledge ingest first)");
    db_result_free(r);
    return;
  }

  // Query path: real RAG across every corpus in the list. The callback
  // fires asynchronously on the curl worker thread, so we can't rely on
  // `ctx` (stack-lifetime) or a stack-resident accumulator — snapshot
  // the reply target into heap state that the cb frees when done.
  if(ctx->msg == NULL || ctx->msg->inst == NULL)
  {
    cmd_reply(ctx, "internal error: no method context for async reply");
    return;
  }

  st = mem_alloc("chatbot", "kw_rag_state", sizeof(*st));

  if(st == NULL)
  {
    cmd_reply(ctx, "knowledge_retrieve alloc failed");
    return;
  }

  st->inst = ctx->msg->inst;
  snprintf(st->target, sizeof(st->target), "%s",
      ctx->msg->channel[0] != '\0' ? ctx->msg->channel : ctx->msg->sender);

  if(knowledge_retrieve(corpus_list, rest, 0, kw_rag_cb, st) != SUCCESS)
  {
    cmd_reply(ctx, "knowledge_retrieve failed");
    mem_free(st);
    return;
  }
}

// ---- /show bot <name> llm interests -----------------------------------
// List the bot's registered acquisition topics with per-topic runtime
// stats from acquire_topic_stats. Snapshot comes from the acquire
// engine (no direct registry access); stats come from a single JOIN-
// shaped SELECT keyed on bot_name.

static const char *
verb_interests_mode_label(acquire_mode_t m)
{
  switch(m)
  {
    case ACQUIRE_MODE_ACTIVE:   return("active");
    case ACQUIRE_MODE_REACTIVE: return("reactive");
    case ACQUIRE_MODE_MIXED:    return("mixed");
  }

  return("?");
}

// Stack-resident row of acquire_topic_stats columns collected for the
// bot. `found` distinguishes "row exists in DB" from "no stats row yet"
// so we can render "-" where the topic has never fired.
typedef struct
{
  char      topic_name[ACQUIRE_TOPIC_NAME_SZ];
  bool      found;
  char      last_proactive[40];   // ISO-8601 or "-"
  char      last_reactive [40];
  uint64_t  total_queries;
  uint64_t  total_ingested;
} verb_interests_stats_t;

static void
verb_interests_load_stats(const char *bot_name,
    verb_interests_stats_t *out, size_t n_out)
{
  db_result_t *r;
  char sql[512];
  char *bot_esc;

  if(n_out == 0)
    return;

  bot_esc = db_escape(bot_name);

  if(bot_esc == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "SELECT topic_name,"
      " COALESCE(to_char(last_proactive AT TIME ZONE 'UTC',"
      "   'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '-'),"
      " COALESCE(to_char(last_reactive  AT TIME ZONE 'UTC',"
      "   'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '-'),"
      " total_queries, total_ingested"
      " FROM acquire_topic_stats WHERE bot_name = '%s'",
      bot_esc);

  mem_free(bot_esc);

  r = db_result_alloc();

  if(db_query(sql, r) != SUCCESS || !r->ok)
  {
    db_result_free(r);
    return;
  }

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *tn = db_result_get(r, i, 0);

    if(tn == NULL)
      continue;

    // Linear scan: n_out is tiny (personality-capped handful).
    for(size_t j = 0; j < n_out; j++)
    {
      const char *lp;
      const char *lr;
      const char *tq;
      const char *tin;

      if(strcmp(out[j].topic_name, tn) != 0)
        continue;

      lp = db_result_get(r, i, 1);
      lr = db_result_get(r, i, 2);
      tq = db_result_get(r, i, 3);
      tin = db_result_get(r, i, 4);

      snprintf(out[j].last_proactive, sizeof(out[j].last_proactive),
          "%s", lp != NULL ? lp : "-");
      snprintf(out[j].last_reactive,  sizeof(out[j].last_reactive),
          "%s", lr != NULL ? lr : "-");
      out[j].total_queries  = (tq  != NULL) ? strtoull(tq,  NULL, 10) : 0;
      out[j].total_ingested = (tin != NULL) ? strtoull(tin, NULL, 10) : 0;
      out[j].found          = true;
      break;
    }
  }

  db_result_free(r);
}

static void
verb_llm_interests(const cmd_ctx_t *ctx, bot_inst_t *bot, const char *rest)
{
  enum { VERB_INTERESTS_CAP = 32 };

  char line[512];
  char header[256];
  verb_interests_stats_t stats[VERB_INTERESTS_CAP];
  const char *name;
  acquire_topic_t topics[VERB_INTERESTS_CAP];
  size_t n;

  (void)rest;

  name = bot_inst_name(bot);

  // A generous but bounded snapshot buffer; acquire topics are
  // personality-capped to a handful per bot.

  n = acquire_get_topic_snapshot(name, topics, VERB_INTERESTS_CAP);

  if(n == 0)
  {
    cmd_reply(ctx, "no acquisition topics registered");
    return;
  }

  memset(stats, 0, sizeof(stats));

  for(size_t i = 0; i < n; i++)
  {
    snprintf(stats[i].topic_name, sizeof(stats[i].topic_name),
        "%s", topics[i].name);
    snprintf(stats[i].last_proactive, sizeof(stats[i].last_proactive),
        "%s", "-");
    snprintf(stats[i].last_reactive,  sizeof(stats[i].last_reactive),
        "%s", "-");
  }

  verb_interests_load_stats(name, stats, n);

  snprintf(header, sizeof(header),
      CLR_BOLD "interests" CLR_RESET " (%zu topic%s)",
      n, (n == 1) ? "" : "s");
  cmd_reply(ctx, header);

  for(size_t i = 0; i < n; i++)
  {
    snprintf(line, sizeof(line),
        "  %-12s mode=%-8s weight=%-3u kw=%-2zu"
        "  q=%-4" PRIu64 " ing=%-4" PRIu64
        "  last_proactive=%s  last_reactive=%s",
        topics[i].name,
        verb_interests_mode_label(topics[i].mode),
        topics[i].proactive_weight,
        topics[i].n_keywords,
        stats[i].total_queries,
        stats[i].total_ingested,
        stats[i].last_proactive,
        stats[i].last_reactive);
    cmd_reply(ctx, line);
  }
}

// ---- /show extract {root,stats} ---------------------------------------
// Ported from core/cmd_show.c in R2 when the extract subsystem moved
// into the chat plugin. Plain /show children (not bound to a bot
// instance, no kind_filter).

static void
chatbot_show_extract_stats_cmd(const cmd_ctx_t *ctx)
{
  extract_stats_t s;
  char line[256];

  extract_get_stats(&s);

  cmd_reply(ctx, CLR_BOLD "Fact-extraction subsystem" CLR_RESET);
  snprintf(line, sizeof(line),
      "  sweeps=%llu  rate_limited=%llu  llm_calls=%llu  llm_errors=%llu",
      (unsigned long long)s.sweeps_total,
      (unsigned long long)s.sweeps_skipped_rate_limited,
      (unsigned long long)s.llm_calls,
      (unsigned long long)s.llm_errors);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  facts_written=%llu  facts_rejected=%llu",
      (unsigned long long)s.facts_written,
      (unsigned long long)s.facts_rejected_validation);
  cmd_reply(ctx, line);
}

// Parent handler so /show extract has a usage message when invoked
// with no subcommand. Mirrors the dossier_show.c / /show dossier pattern.
static void
chatbot_show_extract_cmd(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /show extract stats");
}

// ---- cmd_register wrappers --------------------------------------------
//
// The underlying verb_* handlers take (ctx, bot, rest) so they can be
// called from the old per-kind registry or from new cmd_register paths.
// The dispatcher populates ctx->bot and ctx->args before invoking the
// child callback, so the wrappers simply forward.

static void
verb_llm_summary_wrapper(const cmd_ctx_t *ctx)
{
  verb_llm_summary(ctx, ctx->bot, ctx->args);
}

static void
verb_llm_personas_wrapper(const cmd_ctx_t *ctx)
{
  verb_llm_personas(ctx, ctx->bot, ctx->args);
}

static void
verb_llm_memories_wrapper(const cmd_ctx_t *ctx)
{
  verb_llm_memories(ctx, ctx->bot, ctx->args);
}

static void
verb_stats_wrapper(const cmd_ctx_t *ctx)
{
  verb_stats(ctx, ctx->bot, ctx->args);
}

static void
verb_llm_knowledge_wrapper(const cmd_ctx_t *ctx)
{
  verb_llm_knowledge(ctx, ctx->bot, ctx->args);
}

static void
verb_llm_interests_wrapper(const cmd_ctx_t *ctx)
{
  verb_llm_interests(ctx, ctx->bot, ctx->args);
}

static void
verb_model_wrapper(const cmd_ctx_t *ctx)
{
  verb_model(ctx, ctx->bot, ctx->args);
}

// NL hint for /show bot <name> model. Leaf name "model" is what the
// LLM must emit (`/model`); the NL bridge resolves the leaf by name
// and invokes it via cmd_dispatch_resolved, bypassing the admin-gated
// show/bot parent whose generic subcommand walker would otherwise
// never reach here. The chat kind filter confines the verb to chat
// bots so NL doesn't surface it for non-chat drivers.
static const cmd_nl_example_t show_bot_model_examples[] = {
  { .utterance  = "what LLM are you?",
    .invocation = "/model" },
  { .utterance  = "which model is powering you right now?",
    .invocation = "/model" },
};

static const cmd_nl_t show_bot_model_nl = {
  .when          = "Someone asks which LLM, model, or AI engine is"
                   " running you — not what kind of bot you are, but"
                   " the actual model identifier.",
  .syntax        = "/model",
  .slots         = NULL,
  .slot_count    = 0,
  .examples      = show_bot_model_examples,
  .example_count = (uint8_t)(sizeof(show_bot_model_examples)
                             / sizeof(show_bot_model_examples[0])),
  .dispatch_text = "show bot $bot model",
};

// ---- registration ------------------------------------------------------

// llm-kind filter, NUL-terminated. Storage must be static -- cmd_register
// keeps the pointer.
static const char *const chat_kind_filter[] = { "chat", NULL };

// Called from chatbot plugin init (chatbot.c) once per plugin load.
bool
chatbot_show_verbs_register(void)
{
  if(cmd_register("llm", ":default",
        "show bot <name>",
        "LLM bot summary (persona, model, mute, verbosity)",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_llm_summary_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("llm", "personas",
        "show bot <name> personas",
        "List personalities or show the active persona",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_llm_personas_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("llm", "memories",
        "show bot <name> memories [<query>]",
        "Recent conversation log or RAG query over facts/messages",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_llm_memories_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("llm", "stats",
        "show bot <name> stats",
        "Bot-instance counters (in-flight requests, namespace totals)",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_stats_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("llm", "knowledge",
        "show bot <name> knowledge [<query>]",
        "Preview the bound knowledge corpus or run a RAG query against it",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_llm_knowledge_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("llm", "interests",
        "show bot <name> interests",
        "List registered acquisition topics with per-topic runtime stats",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_llm_interests_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  // Everyone-gated by design so the NL bridge can route "what LLM are
  // you?" here without tripping the admin check. Read-only and leaks
  // nothing beyond the model identifier (already semi-public: the bot
  // broadcasts its nature in channel constantly).
  if(cmd_register("chat", "model",
        "show bot <name> model",
        "Report the LLM model currently bound to a chat bot",
        "Reads bot.<name>.chat_model, falling back to\n"
        "llm.default_chat_model when unset. Designed to back NL queries\n"
        "like \"what LLM are you?\" via the NL bridge.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        verb_model_wrapper, NULL, "show/bot", NULL,
        NULL, 0, chat_kind_filter, &show_bot_model_nl) != SUCCESS)
    return(FAIL);

  // /show extract {root,stats} — plain /show children, no kind filter.
  // Registered from the chat plugin since the extract subsystem moved
  // here in R2.
  if(cmd_register("extract", "extract",
        "show extract <subcommand>",
        "Fact extraction subsystem read-only inspection",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        chatbot_show_extract_cmd, NULL, "show", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("extract", "stats",
        "show extract stats",
        "Process-wide fact-extraction counters",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        chatbot_show_extract_stats_cmd, NULL, "show/extract", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
