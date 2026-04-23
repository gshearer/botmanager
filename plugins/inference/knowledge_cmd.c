// botmanager — MIT
// Knowledge subsystem admin + /show commands.

#include "knowledge_priv.h"

#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "userns.h"
#include "method.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Commands

static void
cmd_knowledge_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /knowledge <subcommand>");
  cmd_reply(ctx, "  ingest <corpus> <path>   — ingest a file or directory");
  cmd_reply(ctx, "  corpus upsert <name>     — create (or ensure) a named corpus");
  cmd_reply(ctx, "  corpus del <name>        — delete a corpus (cascades)");
  cmd_reply(ctx, "  stats [<corpus>]         — subsystem or per-corpus stats");
  cmd_reply(ctx, "reads: /show knowledge corpora | /show knowledge corpus <name>");
}

// ---- /knowledge ingest <corpus> <path> ----

// Optional third positional: base-url. When supplied, each ingested
// file's source_url column is set to "<base-url>/<filename-stem>"
// instead of the raw on-disk path. This is how a throwaway ingest dir
// (e.g. mktemp scratch from fetch_archwiki.sh) maps onto citable public
// URLs without re-plumbing the ingest pipeline per-corpus. See
// kw_derive_source_url (knowledge_file.c) for the mapping rules.
static const cmd_arg_desc_t ad_kw_ingest[] = {
  { "corpus",   CMD_ARG_ALNUM, CMD_ARG_REQUIRED, KNOWLEDGE_CORPUS_NAME_SZ - 1, NULL },
  { "path",     CMD_ARG_NONE,  CMD_ARG_REQUIRED, 1024, NULL },
  { "base-url", CMD_ARG_NONE,  CMD_ARG_OPTIONAL, KNOWLEDGE_SOURCE_URL_SZ - 1, NULL },
};

static void
cmd_knowledge_ingest(const cmd_ctx_t *ctx)
{
  const char *corpus   = ctx->parsed->argv[0];
  const char *path     = ctx->parsed->argv[1];
  const char *base_url = (ctx->parsed->argc > 2) ? ctx->parsed->argv[2] : NULL;

  char line[256];
  size_t files, chunks, skipped;
  uint64_t embed_ok, embed_fail;
  if(knowledge_corpus_upsert(corpus, NULL) != SUCCESS)
  {
    cmd_reply(ctx, "error: corpus upsert failed");
    return;
  }

  if(base_url != NULL && base_url[0] != '\0')
    snprintf(line, sizeof(line),
        "ingesting '%s' into corpus '%s' (base-url=%s) …",
        path, corpus, base_url);
  else
    snprintf(line, sizeof(line),
        "ingesting '%s' into corpus '%s' …", path, corpus);
  cmd_reply(ctx, line);

  files = 0;
  chunks = 0;
  skipped = 0;
  embed_ok = 0;
  embed_fail = 0;
  if(knowledge_ingest_path(corpus, path, base_url,
        &files, &chunks, &skipped,
        &embed_ok, &embed_fail) != SUCCESS)
  {
    cmd_reply(ctx, "error: ingest failed (stat/open)");
    return;
  }

  snprintf(line, sizeof(line),
      "ingested %zu chunk(s) from %zu file(s) (%zu skipped).",
      chunks, files, skipped);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "embeds submitted: %llu; failed: %llu. "
      "(Completion is asynchronous; /show knowledge tracks progress.)",
      (unsigned long long)embed_ok,
      (unsigned long long)embed_fail);
  cmd_reply(ctx, line);
}

// ---- /knowledge corpus <container> + /knowledge corpus {upsert,del} ----

static void
cmd_knowledge_corpus_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /knowledge corpus <subcommand>");
  cmd_reply(ctx, "  upsert <name>   — create a named corpus (idempotent)");
  cmd_reply(ctx, "  del    <name>   — delete a corpus (cascades)");
}

static const cmd_arg_desc_t ad_kw_corpus_upsert[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, KNOWLEDGE_CORPUS_NAME_SZ - 1, NULL },
};

static void
cmd_knowledge_corpus_upsert(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  if(knowledge_corpus_upsert(name, NULL) != SUCCESS)
  {
    cmd_reply(ctx, "error: corpus upsert failed");
    return;
  }
  cmd_reply(ctx, "ok.");
}

static const cmd_arg_desc_t ad_kw_corpus_del[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, KNOWLEDGE_CORPUS_NAME_SZ - 1, NULL },
};

static void
cmd_knowledge_corpus_del(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];
  if(knowledge_corpus_delete(name) != SUCCESS)
  {
    cmd_reply(ctx, "error: corpus not found or delete failed");
    return;
  }
  cmd_reply(ctx, "deleted.");
}

// ---- /knowledge stats [<corpus>] ----

static const cmd_arg_desc_t ad_kw_stats[] = {
  { "corpus", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, KNOWLEDGE_CORPUS_NAME_SZ - 1, NULL },
};

typedef struct
{
  const cmd_ctx_t *ctx;
  const char      *filter;    // NULL = all corpora
  uint32_t         matched;
} kw_stats_iter_t;

static void
kw_stats_cb(const char *name, const char *description,
    int64_t chunk_count, time_t last_ingested, void *user)
{
  kw_stats_iter_t *s = user;

  char tbuf[32];
  char line[512];
  if(s->filter != NULL && strcmp(s->filter, name) != 0)
    return;

  if(last_ingested > 0)
  {
    struct tm tm;
    gmtime_r(&last_ingested, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
  }

  else
    snprintf(tbuf, sizeof(tbuf), "never");

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-24s" CLR_RESET " chunks=%-6" PRId64 " last=%s  %s",
      name, chunk_count, tbuf, description);
  cmd_reply(s->ctx, line);
  s->matched++;
}

static void
cmd_knowledge_stats(const cmd_ctx_t *ctx)
{
  const char *filter = (ctx->parsed != NULL && ctx->parsed->argc > 0
      && ctx->parsed->argv[0][0] != '\0') ? ctx->parsed->argv[0] : NULL;

  kw_stats_iter_t s = { .ctx = ctx, .filter = filter, .matched = 0 };
  knowledge_stats_t g;
  char line[256];
  knowledge_corpus_iterate(kw_stats_cb, &s);

  if(s.matched == 0)
  {
    if(filter != NULL)
      cmd_reply(ctx, "no such corpus");
    else
      cmd_reply(ctx, "  (no corpora)");
    return;
  }

  knowledge_get_stats(&g);

  snprintf(line, sizeof(line),
      "total: corpora=%llu chunks=%llu embeddings=%llu images=%llu",
      (unsigned long long)g.total_corpora,
      (unsigned long long)g.total_chunks,
      (unsigned long long)g.total_embeds,
      (unsigned long long)g.total_images);
  cmd_reply(ctx, line);
}

// ---- /show knowledge root + corpora + corpus <name> ----

static void
cmd_show_knowledge(const cmd_ctx_t *ctx)
{
  knowledge_stats_t g;
  knowledge_cfg_t cfg;
  char effective_model[KNOWLEDGE_EMBED_MODEL_SZ];
  char line[256];
  knowledge_get_stats(&g);

  knowledge_cfg_snapshot(&cfg);

  knowledge_effective_embed_model(effective_model, sizeof(effective_model));

  snprintf(line, sizeof(line),
      "knowledge: corpora=%llu chunks=%llu embeddings=%llu images=%llu",
      (unsigned long long)g.total_corpora,
      (unsigned long long)g.total_chunks,
      (unsigned long long)g.total_embeds,
      (unsigned long long)g.total_images);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  enabled=%s embed_model='%s'%s top_k=%u max_ctx_chars=%u chunk_max=%u",
      cfg.enabled ? "yes" : "no",
      effective_model,
      cfg.embed_model[0] != '\0' ? "" : " (inherited)",
      cfg.rag_top_k, cfg.rag_max_context_chars, cfg.chunk_max_chars);
  cmd_reply(ctx, line);
}

typedef struct
{
  const cmd_ctx_t *ctx;
  uint32_t         count;
} kw_list_iter_t;

static void
kw_list_cb(const char *name, const char *description,
    int64_t chunk_count, time_t last_ingested, void *user)
{
  kw_list_iter_t *s = user;
  char tbuf[32];

  char line[512];
  if(last_ingested > 0)
  {
    struct tm tm;
    gmtime_r(&last_ingested, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d", &tm);
  }

  else
    snprintf(tbuf, sizeof(tbuf), "—");

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%-24s" CLR_RESET " chunks=%-6" PRId64 " %s  %s",
      name, chunk_count, tbuf, description);
  cmd_reply(s->ctx, line);
  s->count++;
}

static void
cmd_show_knowledge_corpora(const cmd_ctx_t *ctx)
{
  kw_list_iter_t s = { .ctx = ctx, .count = 0 };
  knowledge_corpus_iterate(kw_list_cb, &s);
  if(s.count == 0)
    cmd_reply(ctx, "  (no corpora)");
  else
  {
    char foot[64];
    snprintf(foot, sizeof(foot), "%u corpus%s",
        s.count, s.count == 1 ? "" : "es");
    cmd_reply(ctx, foot);
  }
}

static const cmd_arg_desc_t ad_kw_corpus_show[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, KNOWLEDGE_CORPUS_NAME_SZ - 1, NULL },
};

static void
cmd_show_knowledge_corpus(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];

  char *e_name = db_escape(name);
  char sql[512];
  db_result_t *res;
  char header[128];
  bool ok;
  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: escape failed");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT id, COALESCE(section_heading, ''),"
      " COALESCE(source_url, ''), SUBSTRING(text FROM 1 FOR 160)"
      " FROM knowledge_chunks WHERE corpus = '%s'"
      " ORDER BY id DESC LIMIT 20",
      e_name);
  mem_free(e_name);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  if(!ok)
  {
    cmd_reply(ctx, "error: query failed");
    db_result_free(res);
    return;
  }

  if(res->rows == 0)
  {
    cmd_reply(ctx, "  (no chunks — unknown or empty corpus)");
    db_result_free(res);
    return;
  }

  snprintf(header, sizeof(header),
      CLR_BOLD "corpus: %s" CLR_RESET " (showing most recent %u of N)",
      name, res->rows);
  cmd_reply(ctx, header);

  for(uint32_t i = 0; i < res->rows; i++)
  {
    const char *id   = db_result_get(res, i, 0);
    const char *sec  = db_result_get(res, i, 1);
    const char *url  = db_result_get(res, i, 2);
    const char *prev = db_result_get(res, i, 3);

    char line[512];
    snprintf(line, sizeof(line),
        "  #%-6s [%s] %s  — %s",
        id  ? id  : "?",
        sec ? sec : "",
        url ? url : "",
        prev ? prev : "");
    cmd_reply(ctx, line);
  }

  db_result_free(res);
}

// Registration

void
knowledge_register_commands(void)
{
  // /knowledge root (container).
  cmd_register("knowledge", "knowledge",
      "knowledge",
      "Knowledge corpus administration",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_knowledge_root, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  // /knowledge ingest <corpus> <path> [base-url]
  cmd_register("knowledge", "ingest",
      "knowledge ingest <corpus> <path> [base-url]",
      "Ingest a file or directory into a corpus",
      "Slurps the file (or every .md/.markdown/.txt in a directory),"
      " splits into chunks, and writes them to the named corpus."
      " Creates the corpus on first use. Embeddings are submitted"
      " asynchronously; /show knowledge reports completion. When"
      " base-url is given, each chunk's source_url becomes"
      " '<base-url>/<filename-stem>' — so a throwaway ingest dir"
      " (e.g. mktemp scratch) can still produce citable public URLs"
      " (e.g. https://wiki.archlinux.org/title/ZFS).",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_knowledge_ingest, NULL, "knowledge", "i",
      ad_kw_ingest, (uint8_t)(sizeof(ad_kw_ingest) / sizeof(ad_kw_ingest[0])), NULL, NULL);

  // /knowledge corpus (container) + /knowledge corpus {upsert,del}
  cmd_register("knowledge", "corpus",
      "knowledge corpus",
      "Corpus management (upsert, del)",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_knowledge_corpus_root, NULL, "knowledge", NULL, NULL, 0, NULL, NULL);

  cmd_register("knowledge", "upsert",
      "knowledge corpus upsert <name>",
      "Create (or ensure) a named corpus; thin wrapper over"
      " knowledge_corpus_upsert()",
      "Useful as a bootstrap step — freshstart.sh uses this to create"
      " an acquired-content corpus before binding it via"
      " bot.<name>.llm.acquired_corpus. Idempotent: re-running is a"
      " no-op when the corpus already exists.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_knowledge_corpus_upsert, NULL, "knowledge/corpus", "u",
      ad_kw_corpus_upsert,
      (uint8_t)(sizeof(ad_kw_corpus_upsert) / sizeof(ad_kw_corpus_upsert[0])), NULL, NULL);

  cmd_register("knowledge", "del",
      "knowledge corpus del <name>",
      "Delete a corpus and all its chunks + embeddings",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      cmd_knowledge_corpus_del, NULL, "knowledge/corpus", "d",
      ad_kw_corpus_del,
      (uint8_t)(sizeof(ad_kw_corpus_del) / sizeof(ad_kw_corpus_del[0])), NULL, NULL);

  // /knowledge stats [<corpus>]
  cmd_register("knowledge", "stats",
      "knowledge stats [<corpus>]",
      "Show per-corpus or subsystem-wide statistics",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_knowledge_stats, NULL, "knowledge", "s",
      ad_kw_stats, (uint8_t)(sizeof(ad_kw_stats) / sizeof(ad_kw_stats[0])), NULL, NULL);

  // /show knowledge — subsystem state. Shares the "knowledge" name
  // with the /knowledge root above; tree position (parent "show" vs
  // parent NULL) keeps them distinct at dispatch time.
  cmd_register("knowledge", "knowledge",
      "show knowledge",
      "Show knowledge subsystem state",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_knowledge, NULL, "show", "kw", NULL, 0, NULL, NULL);

  // /show knowledge corpora — list all corpora.
  cmd_register("knowledge", "corpora",
      "show knowledge corpora",
      "List all ingested corpora",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_knowledge_corpora, NULL, "show/knowledge", NULL, NULL, 0, NULL, NULL);

  // /show knowledge corpus <name> — preview recent chunks. Same name
  // as /knowledge corpus above; parent path disambiguates.
  cmd_register("knowledge", "corpus",
      "show knowledge corpus <name>",
      "Preview the 20 most recently ingested chunks of a corpus",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_knowledge_corpus, NULL, "show/knowledge", "c",
      ad_kw_corpus_show,
      (uint8_t)(sizeof(ad_kw_corpus_show) / sizeof(ad_kw_corpus_show[0])), NULL, NULL);
}
