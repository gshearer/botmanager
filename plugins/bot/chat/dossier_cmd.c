// botmanager — MIT
// Chat-plugin admin commands for dossiers: /bot <name> dossiersweep + /dossier.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "memory.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// /bot <name> dossiersweep — synchronous fact-extraction sweep

// Synchronous fact-extraction sweep. Registered as a kind-filtered
// child of "bot" so invocation is:
//   /bot <name> dossiersweep            (or abbreviated "dsweep")
// The kind_filter restricts dispatch to chat-kind bots; other kinds
// get "unknown verb" from the unified dispatcher.
static void
llm_verb_dossiersweep(const cmd_ctx_t *ctx)
{
  bot_inst_t *bot = ctx->bot;

  userns_t *ns = bot_get_userns(bot);
  long ms;
  char buf[128];
  size_t n;
  struct timespec t0, t1;

  if(ns == NULL) { cmd_reply(ctx, "bot has no userns bound"); return; }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  n = extract_run_once(bot_inst_name(bot), ns->id);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  ms = (t1.tv_sec - t0.tv_sec) * 1000
          + (t1.tv_nsec - t0.tv_nsec) / 1000000;

  snprintf(buf, sizeof(buf),
      "extracted %zu fact%s (elapsed %ldms)",
      n, n == 1 ? "" : "s", ms);
  cmd_reply(ctx, buf);
}

// Kind filter: restrict this verb to chat-kind bots. Storage static --
// cmd_register keeps the pointer.
static const char *const chat_kind_filter[] = { "chat", NULL };

bool
chatbot_dossiersweep_cmd_register(void)
{
  if(cmd_register("llm", "dossiersweep",
        "bot <name> dossiersweep",
        "Fire one LLM fact-extraction sweep for this bot now",
        NULL,
        USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        llm_verb_dossiersweep, NULL, "bot", "dsweep",
        NULL, 0, chat_kind_filter, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// /dossier admin mutators (merge, split, fact set, fact del)

static const cmd_arg_desc_t ad_dossier_merge[] =
{
  { "bot",      CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,        NULL },
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ - 1, NULL },
  { "ids",      CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,   NULL },
};

static const cmd_arg_desc_t ad_dossier_split[] =
{
  { "signature_id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 20, NULL },
};

static const cmd_arg_desc_t ad_dossier_fact_set[] =
{
  { "dossier_id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 20,                NULL },
  { "kind",       CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, 32,                NULL },
  { "key",        CMD_ARG_NONE,   CMD_ARG_REQUIRED, MEM_FACT_KEY_SZ,   NULL },
  { "value",      CMD_ARG_NONE,   CMD_ARG_REQUIRED, MEM_FACT_VALUE_SZ, NULL },
  { "conf",       CMD_ARG_NONE,   CMD_ARG_OPTIONAL, 16,                NULL },
};

static const cmd_arg_desc_t ad_dossier_fact_del[] =
{
  { "fact_id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 20, NULL },
};

// /dossier (root) — subcommand listing

static void
cmd_dossier_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "dossier: merge | split | fact");
}

// /dossier merge <bot> <username> <id> [<id>...]

static size_t
parse_ids(const char *s, dossier_id_t *out, size_t max)
{
  size_t n = 0;

  while(*s != '\0' && n < max)
  {
    char *endp;
    long long v;

    while(*s == ' ' || *s == '\t') s++;
    if(*s == '\0') break;

    endp = NULL;
    v = strtoll(s, &endp, 10);

    if(endp == s || v <= 0) return(0);

    out[n++] = (dossier_id_t)v;
    s = endp;
  }

  return(n);
}

static void
cmd_dossier_merge(const cmd_ctx_t *ctx)
{
  const char *arg_bot  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];

  bot_inst_t *bot = bot_find(arg_bot);
  char buf[128];
  dossier_id_t survivor;
  dossier_id_t ids[32];
  size_t n;
  uint32_t user_id;
  userns_t *ns;

  if(bot == NULL)
  {
    char buf[BOT_NAME_SZ + 32];

    snprintf(buf, sizeof(buf), "bot not found: %s", arg_bot);
    cmd_reply(ctx, buf);
    return;
  }

  ns = bot_get_userns(bot);

  if(ns == NULL) { cmd_reply(ctx, "bot has no userns bound"); return; }

  user_id = userns_user_id(ns, username);

  if(user_id == 0) { cmd_reply(ctx, "user not found"); return; }

  n = parse_ids(ctx->parsed->argv[2], ids,
      sizeof(ids) / sizeof(ids[0]));

  if(n < 1)
  {
    cmd_reply(ctx,
        "usage: dossier merge <bot> <username> <id> [<id>...]");
    return;
  }

  survivor = ids[0];

  if(n > 1)
  {
    if(dossier_merge(survivor, &ids[1], n - 1) != SUCCESS)
    {
      cmd_reply(ctx, "merge failed (see logs)");
      return;
    }
  }

  if(dossier_set_user(survivor, (int)user_id) != SUCCESS)
  {
    cmd_reply(ctx,
        "merged, but failed to attach user_id (see logs)");
    return;
  }

  snprintf(buf, sizeof(buf),
      "merged %zu dossier%s into %" PRId64 " and attached to %s (user_id=%u)",
      n - 1, (n - 1) == 1 ? "" : "s", (int64_t)survivor, username, user_id);
  cmd_reply(ctx, buf);
}

// /dossier split <signature_id>

static void
cmd_dossier_split(const cmd_ctx_t *ctx)
{
  int64_t sig_id = strtoll(ctx->parsed->argv[0], NULL, 10);
  dossier_id_t new_id = 0;
  char buf[128];

  if(dossier_split_signature(sig_id, &new_id) != SUCCESS)
  {
    cmd_reply(ctx,
        "split failed: signature not found, or source would be left empty");
    return;
  }

  snprintf(buf, sizeof(buf),
      "signature %" PRId64 " detached into new dossier %" PRId64
      " (facts not migrated)",
      sig_id, (int64_t)new_id);
  cmd_reply(ctx, buf);
}

// /dossier fact — container + set + del
//
// Admin-seed facts directly onto a dossier, bypassing the LLM extractor.
// Mirrors /user fact set but keyed on dossier_id, with source stamped
// "admin_seed" and merge policy REPLACE so an explicit admin write
// overrides whatever the extractor produced.

static void
cmd_dossier_fact_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "dossier fact: set | del");
}

static void
cmd_dossier_fact_set(const cmd_ctx_t *ctx)
{
  dossier_id_t did =
      (dossier_id_t)strtoll(ctx->parsed->argv[0], NULL, 10);
  char ack[128];
  mem_dossier_fact_t f;
  float conf;
  mem_fact_kind_t k;
  dossier_info_t info;

  if(did <= 0)
  {
    cmd_reply(ctx, "error: dossier_id must be a positive integer");
    return;
  }

  if(dossier_get(did, &info) != SUCCESS)
  {
    cmd_reply(ctx, "error: dossier not found");
    return;
  }

  if(memory_kind_from_name(ctx->parsed->argv[1], &k) != SUCCESS)
  {
    cmd_reply(ctx,
        "error: kind must be"
        " preference|attribute|relation|event|opinion|freeform");
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
  f.dossier_id = did;
  f.kind       = k;
  snprintf(f.fact_key,   sizeof(f.fact_key),   "%s", ctx->parsed->argv[2]);
  snprintf(f.fact_value, sizeof(f.fact_value), "%s", ctx->parsed->argv[3]);
  snprintf(f.source,     sizeof(f.source),     "admin_seed");
  f.confidence = conf;

  if(memory_upsert_dossier_fact(&f, MEM_MERGE_REPLACE) != SUCCESS)
  {
    cmd_reply(ctx, "error: upsert failed (see logs)");
    return;
  }

  snprintf(ack, sizeof(ack),
      "ok: fact set on dossier %" PRId64 " (conf=%.2f)",
      (int64_t)did, conf);
  cmd_reply(ctx, ack);
}

static void
cmd_dossier_fact_del(const cmd_ctx_t *ctx)
{
  int64_t fid = strtoll(ctx->parsed->argv[0], NULL, 10);

  if(fid <= 0)
  {
    cmd_reply(ctx, "error: fact_id must be a positive integer");
    return;
  }

  if(memory_forget_dossier_fact(fid) != SUCCESS)
  {
    cmd_reply(ctx,
        "error: delete failed (fact not found or db error)");
    return;
  }

  cmd_reply(ctx, "ok: fact deleted");
}

// Registration

void
dossier_register_commands(void)
{
  // /dossier (container)
  cmd_register("dossier", "dossier",
      "dossier <subcommand> ...",
      "Dossier admin mutators",
      "Manages dossiers in a namespace.\n"
      "Subcommands: merge, split, fact",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_dossier_root, NULL, NULL, NULL,
      NULL, 0, NULL, NULL);

  cmd_register("dossier", "merge",
      "dossier merge <bot> <username> <id> [<id>...]",
      "Merge dossiers into the first id and attach to user",
      "First id is the survivor. Remaining ids are absorbed via\n"
      "dossier_merge(), then the survivor is attached to the named\n"
      "user via dossier_set_user(). All ids must be in the named\n"
      "bot's userns.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_dossier_merge, NULL, "dossier", "m",
      ad_dossier_merge,
      (uint8_t)(sizeof(ad_dossier_merge) / sizeof(ad_dossier_merge[0])),
      NULL, NULL);

  cmd_register("dossier", "split",
      "dossier split <signature_id>",
      "Detach a signature into a new dossier (facts not migrated)",
      "Best-effort split: the named signature row is reassigned to\n"
      "a brand-new dossier in the same namespace, inheriting the\n"
      "source dossier's display_label. Facts stay on the source --\n"
      "dossier_facts has no source-signature provenance. The source\n"
      "must retain at least one remaining signature.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_dossier_split, NULL, "dossier", NULL,
      ad_dossier_split,
      (uint8_t)(sizeof(ad_dossier_split) / sizeof(ad_dossier_split[0])),
      NULL, NULL);

  // /dossier fact (container)
  cmd_register("dossier", "fact",
      "dossier fact",
      "Dossier-fact mutators (set, del)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_dossier_fact_root, NULL, "dossier", NULL,
      NULL, 0, NULL, NULL);

  cmd_register("dossier", "set",
      "dossier fact set <dossier_id> <kind> <key> <value> [conf]",
      "Admin-seed a fact on a dossier (source=admin_seed)",
      "Writes a single fact onto a dossier, bypassing the LLM\n"
      "extractor. The row is stamped source='admin_seed' and upserted\n"
      "with REPLACE merge policy, so a subsequent admin set overrides\n"
      "whatever the extractor (or a prior admin seed) wrote. The fact\n"
      "becomes visible in the llm prompt's 'ABOUT PEOPLE MENTIONED'\n"
      "block the next time the bot resolves this dossier.\n"
      "\n"
      "Arguments:\n"
      "  dossier_id  Numeric dossier id (see /show dossier, or\n"
      "              /show dossiers candidates <bot> <user>).\n"
      "              Must exist in the database.\n"
      "  kind        One of preference | attribute | relation | event |\n"
      "              opinion | freeform. Determines how the llm reply\n"
      "              pipeline frames the fact in the prompt.\n"
      "  key         Short stable slug (e.g. 'pronouns', 'hometown',\n"
      "              'favorite_editor'). Combined with (dossier_id, kind)\n"
      "              for dedupe: a second set with the same triple\n"
      "              overwrites the existing row.\n"
      "  value       Free-form value. Quote if it contains spaces.\n"
      "  conf        Optional confidence, 0.0-1.0 (default 1.0). Lower\n"
      "              values let a higher-confidence LLM extraction\n"
      "              override later.\n"
      "\n"
      "Examples:\n"
      "  /dossier fact set 42 attribute pronouns she/her\n"
      "  /dossier fact set 42 preference favorite_editor neovim 0.9\n"
      "  /dossier fact set 17 relation employer \"ACME Corp\"",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_dossier_fact_set, NULL, "dossier/fact", "s",
      ad_dossier_fact_set,
      (uint8_t)(sizeof(ad_dossier_fact_set) / sizeof(ad_dossier_fact_set[0])),
      NULL, NULL);

  cmd_register("dossier", "del",
      "dossier fact del <fact_id>",
      "Delete a dossier-fact by id",
      "Removes a single row from dossier_facts. The id is the row's\n"
      "own primary key (NOT the dossier_id). Find it via\n"
      "/show dossier <dossier_id>, which renders each fact with its\n"
      "fact-id prefix.\n"
      "\n"
      "Arguments:\n"
      "  fact_id     Primary-key id of the dossier_facts row.\n"
      "\n"
      "Examples:\n"
      "  /dossier fact del 312\n"
      "  /dossier fact del 9",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_dossier_fact_del, NULL, "dossier/fact", "d",
      ad_dossier_fact_del,
      (uint8_t)(sizeof(ad_dossier_fact_del) / sizeof(ad_dossier_fact_del[0])),
      NULL, NULL);

  // /show dossiers + /show dossiers candidates (implemented in
  // dossier_show.c — delegated so candidates rendering stays next to
  // the rest of the dossier read paths).
  dossier_show_register_candidates();
}
