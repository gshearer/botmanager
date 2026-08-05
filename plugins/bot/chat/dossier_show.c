// botmanager — MIT
// /show dossiers and /show dossier <id>: namespace-scoped read commands.

#include "common.h"
#include "bot.h"
#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "dossier.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Helpers

static void
fmt_relative_time(time_t ts, char *out, size_t sz)
{
  time_t now;
  long delta;

  if(ts <= 0)
  {
    snprintf(out, sz, CLR_GRAY "never" CLR_RESET);
    return;
  }

  now = time(NULL);
  delta = (long)(now - ts);

  if(delta < 0) delta = 0;

  if(delta < 60)
    snprintf(out, sz, "%lds ago", delta);
  else if(delta < 3600)
    snprintf(out, sz, "%ldm ago", delta / 60);
  else if(delta < 86400)
    snprintf(out, sz, "%ldh ago", delta / 3600);
  else
    snprintf(out, sz, "%ldd ago", delta / 86400);
}

// /show dossiers — colorized table of all dossiers in working namespace

static void
cmd_show_dossiers(const cmd_ctx_t *ctx)
{
  userns_t *ns = userns_session_resolve(ctx);
  char footer[64];
  char hdr[USERNS_NAME_SZ + 64];
  db_result_t *r;
  char sql[256];

  if(ns == NULL) return;

  r = db_result_alloc();
  snprintf(sql, sizeof(sql),
      "SELECT id, display_label, user_id, message_count,"
      " EXTRACT(EPOCH FROM last_seen)::bigint"
      " FROM dossier WHERE ns_id = %u"
      " ORDER BY last_seen DESC NULLS LAST, id DESC LIMIT 200",
      ns->id);

  if(db_query(sql, r) != SUCCESS || !r->ok)
  {
    cmd_reply(ctx, "dossier query failed (see logs)");
    db_result_free(r);
    return;
  }

  snprintf(hdr, sizeof(hdr),
      "dossiers in " CLR_BOLD "%s" CLR_RESET ":", ns->name);
  cmd_reply(ctx, hdr);

  if(r->rows == 0)
  {
    cmd_reply(ctx, "  " CLR_GRAY "(none)" CLR_RESET);
    db_result_free(r);
    return;
  }

  cmd_reply(ctx,
      "  " CLR_BOLD
      "ID        MSGS   LAST SEEN     USER  LABEL"
      CLR_RESET);

  for(uint32_t i = 0; i < r->rows; i++)
  {
    const char *id_s    = db_result_get(r, i, 0);
    const char *label_s = db_result_get(r, i, 1);
    const char *uid_s   = db_result_get(r, i, 2);
    const char *cnt_s   = db_result_get(r, i, 3);
    const char *ts_s    = db_result_get(r, i, 4);

    time_t ts = ts_s ? (time_t)strtoll(ts_s, NULL, 10) : 0;
    char age[32];
    const char *uid;
    const char *label;
    char line[512];
    char idcol[64];

    fmt_relative_time(ts, age, sizeof(age));

    snprintf(idcol, sizeof(idcol),
        CLR_BOLD "%-8s" CLR_RESET, id_s ? id_s : "?");

    uid = (uid_s && uid_s[0] && strcmp(uid_s, "0") != 0)
        ? uid_s
        : CLR_GRAY "-" CLR_RESET;

    label = (label_s && label_s[0] != '\0')
        ? label_s
        : CLR_GRAY "\xe2\x80\x94" CLR_RESET;

    snprintf(line, sizeof(line),
        "  %s  %-5s  %-12s  %-4s  %s",
        idcol,
        cnt_s ? cnt_s : "0",
        age,
        uid,
        label);
    cmd_reply(ctx, line);
  }

  snprintf(footer, sizeof(footer),
      CLR_GRAY "%u dossier%s" CLR_RESET,
      r->rows, r->rows == 1 ? "" : "s");
  cmd_reply(ctx, footer);

  db_result_free(r);
}

// /show dossier <id> — single dossier detail (signatures + facts).
// Requires the resolved row's ns_id to match the active namespace.

static void
render_dossier_detail(const cmd_ctx_t *ctx, dossier_id_t did)
{
  char first[32], last[32]; struct tm tm;

  dossier_info_t info;
  db_result_t *res;
  char sql[512];
  char tline[256];
  char hdr[256];
  userns_t *ns;

  if(dossier_get(did, &info) != SUCCESS)
  {
    cmd_reply(ctx, "dossier not found");
    return;
  }

  // Namespace guard: the dossier must belong to the cd'd namespace.
  // Caller already resolved ns; we re-check inside since dossier_get
  // doesn't filter on ns_id.
  ns = userns_session_resolve(ctx);
  if(ns == NULL) return;

  if(info.ns_id != ns->id)
  {
    cmd_reply(ctx, "dossier not found in current namespace");
    return;
  }

  gmtime_r(&info.first_seen, &tm);
  strftime(first, sizeof(first), "%Y-%m-%d %H:%M", &tm);
  gmtime_r(&info.last_seen, &tm);
  strftime(last, sizeof(last), "%Y-%m-%d %H:%M", &tm);

  snprintf(hdr, sizeof(hdr),
      "dossier " CLR_BOLD "%" PRId64 CLR_RESET
      " in %s  user_id=%d  msgs=%u",
      (int64_t)info.id, ns->name, info.user_id_or_0,
      info.message_count);
  cmd_reply(ctx, hdr);

  snprintf(tline, sizeof(tline),
      "  " CLR_CYAN "label:" CLR_RESET "  %s",
      info.display_label[0] ? info.display_label
                            : CLR_GRAY "\xe2\x80\x94" CLR_RESET);
  cmd_reply(ctx, tline);

  snprintf(tline, sizeof(tline),
      "  " CLR_CYAN "first:" CLR_RESET "  %s    "
      CLR_CYAN "last:" CLR_RESET " %s",
      first, last);
  cmd_reply(ctx, tline);

  // Signatures.
  snprintf(sql, sizeof(sql),
      "SELECT method_kind, nickname, username, hostname, verified_id,"
      " seen_count,"
      " EXTRACT(EPOCH FROM last_seen)::bigint"
      " FROM dossier_signature WHERE dossier_id = %" PRId64
      " ORDER BY last_seen DESC",
      (int64_t)did);

  res = db_result_alloc();
  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    cmd_reply(ctx, "  " CLR_CYAN "signatures:" CLR_RESET);
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *mk    = db_result_get(res, i, 0);
      const char *nick  = db_result_get(res, i, 1);
      const char *uname = db_result_get(res, i, 2);
      const char *hname = db_result_get(res, i, 3);
      const char *vid   = db_result_get(res, i, 4);
      const char *sc    = db_result_get(res, i, 5);
      char line[512];
      snprintf(line, sizeof(line),
          "    [%s] seen=%s  nick=%s user=%s host=%s verified=%s",
          mk    ? mk    : "?",
          sc    ? sc    : "?",
          nick  ? nick  : "",
          uname ? uname : "",
          hname ? hname : "",
          vid   ? vid   : "");
      cmd_reply(ctx, line);
    }
  }
  db_result_free(res);

  // Facts.
  snprintf(sql, sizeof(sql),
      "SELECT kind, fact_key, fact_value, confidence,"
      " EXTRACT(EPOCH FROM last_seen)::bigint"
      " FROM dossier_facts WHERE dossier_id = %" PRId64
      " ORDER BY last_seen DESC LIMIT 64",
      (int64_t)did);

  res = db_result_alloc();
  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    cmd_reply(ctx, "  " CLR_CYAN "facts:" CLR_RESET);
    for(uint32_t i = 0; i < res->rows; i++)
    {
      const char *k    = db_result_get(res, i, 0);
      const char *fk   = db_result_get(res, i, 1);
      const char *fv   = db_result_get(res, i, 2);
      const char *conf = db_result_get(res, i, 3);
      char line[1024];
      snprintf(line, sizeof(line),
          "    [k=%s conf=%s] %s = %s",
          k ? k : "?", conf ? conf : "?",
          fk ? fk : "?", fv ? fv : "");
      cmd_reply(ctx, line);
    }
  }
  db_result_free(res);
}

static const cmd_arg_desc_t ad_show_dossier[] = {
  { "id", CMD_ARG_DIGITS, CMD_ARG_OPTIONAL, 20, NULL },
};

static void
cmd_show_dossier(const cmd_ctx_t *ctx)
{
  dossier_id_t did;

  // No id: list all dossiers in the working namespace.
  if(ctx->parsed == NULL || ctx->parsed->argc < 1
      || ctx->parsed->argv[0][0] == '\0')
  {
    cmd_show_dossiers(ctx);
    return;
  }

  did = (dossier_id_t)strtoll(ctx->parsed->argv[0], NULL, 10);
  if(did <= 0)
  {
    cmd_reply(ctx, "invalid dossier id");
    return;
  }

  render_dossier_detail(ctx, did);
}

// /show dossier stats — process-wide subsystem counters

static void
cmd_show_dossier_stats(const cmd_ctx_t *ctx)
{
  dossier_stats_t s;
  char line[256];

  dossier_get_stats(&s);

  cmd_reply(ctx, CLR_BOLD "Dossier subsystem" CLR_RESET);
  snprintf(line, sizeof(line),
      "  resolves=%llu  creates=%llu  sightings=%llu"
      "  merges=%llu  scorer_calls=%llu",
      (unsigned long long)s.resolves,
      (unsigned long long)s.creates,
      (unsigned long long)s.sightings,
      (unsigned long long)s.merges,
      (unsigned long long)s.scorer_calls);
  cmd_reply(ctx, line);
}

// /show dossiers candidates <bot> <user> — MFA-match dossier candidates
//
// Reconstructs each dossier's MFA string from its signatures and checks
// against the user's registered MFA patterns via userns_user_mfa_match.
// Only IRC-method dossiers participate today; other methods are skipped
// (their scorers haven't learned candidate semantics yet). Logic ported
// from the former llm-plugin verb.

typedef struct
{
  const userns_t *ns;
  const char     *username;
} mfa_filter_t;

// Build an MFA string "nick!user@host" from an IRC signature and test
// it against the target user's patterns. Non-IRC method_kinds are
// skipped — MFA patterns are written assuming IRC's hostmask shape.
// Alias rows (empty username/hostname) are skipped: they aren't real
// observations and shouldn't be matched against an MFA pattern.
static bool
mfa_sig_filter(const dossier_sig_t *sig, void *user)
{
  mfa_filter_t *f = user;
  char mfa[320];

  if(sig == NULL || strcmp(sig->method_kind, "irc") != 0)
    return(false);

  if(sig->nickname == NULL || sig->nickname[0] == '\0'
      || sig->username == NULL || sig->username[0] == '\0'
      || sig->hostname == NULL || sig->hostname[0] == '\0')
    return(false);

  snprintf(mfa, sizeof(mfa), "%s!%s@%s",
      sig->nickname, sig->username, sig->hostname);

  return(userns_user_mfa_match(f->ns, f->username, mfa));
}

// Render one dossier_info row for the candidates listing.
static void
render_candidate_row(const cmd_ctx_t *ctx, const dossier_info_t *info)
{
  char last[32]; struct tm tm;
  char line[256];

  gmtime_r(&info->last_seen, &tm);
  strftime(last, sizeof(last), "%Y-%m-%d %H:%M", &tm);

  snprintf(line, sizeof(line),
      "  " CLR_BOLD "%" PRId64 CLR_RESET
      "  msgs=%u  last=%s  user=%d  label=%s",
      (int64_t)info->id, info->message_count, last,
      info->user_id_or_0, info->display_label);
  cmd_reply(ctx, line);
}

static const cmd_arg_desc_t ad_show_dossiers_candidates[] =
{
  { "bot",  CMD_ARG_ALNUM, CMD_ARG_REQUIRED, BOT_NAME_SZ,    NULL },
  { "user", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, USERNS_USER_SZ, NULL },
};

static void
cmd_show_dossiers_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "show dossiers: candidates");
}

static void
cmd_show_dossiers_candidates(const cmd_ctx_t *ctx)
{
  const char *arg_bot  = ctx->parsed->argv[0];
  const char *username = ctx->parsed->argv[1];

  bot_inst_t *bot = bot_find(arg_bot);
  char hdr[96];
  dossier_id_t ids[64];
  mfa_filter_t f;
  size_t n;
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

  if(!userns_user_exists(ns, username))
  {
    cmd_reply(ctx, "user not found");
    return;
  }

  if(!userns_user_has_mfa(ns, username))
  {
    cmd_reply(ctx,
        "user has no MFA patterns -- cannot match candidates");
    return;
  }

  f.ns = ns;
  f.username = username;

  n = dossier_find_candidates(ns->id, mfa_sig_filter, &f,
      ids, sizeof(ids) / sizeof(ids[0]));

  if(n == 0) { cmd_reply(ctx, "no candidate dossiers"); return; }

  snprintf(hdr, sizeof(hdr),
      CLR_BOLD "%zu" CLR_RESET " candidate dossier%s for %s:",
      n, n == 1 ? "" : "s", username);
  cmd_reply(ctx, hdr);

  for(size_t i = 0; i < n; i++)
  {
    dossier_info_t info;

    if(dossier_get(ids[i], &info) == SUCCESS)
      render_candidate_row(ctx, &info);
  }
}

// Registration — /show dossiers container

void
dossier_show_register_candidates(void)
{
  cmd_register("dossier", "dossiers",
      "show dossiers [<subcommand>]",
      "Dossier listings and candidates",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_dossiers_root, NULL, "show", NULL,
      NULL, 0, NULL, NULL);

  cmd_register("dossier", "candidates",
      "show dossiers candidates <bot> <user>",
      "MFA-match dossier candidates for a user",
      "Reconstructs each dossier's MFA string from its signatures\n"
      "and checks against the user's registered MFA patterns. Only\n"
      "IRC-method dossiers participate today; other methods are\n"
      "skipped. The <bot> argument drives userns lookup.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_dossiers_candidates, NULL, "show/dossiers", NULL,
      ad_show_dossiers_candidates,
      (uint8_t)(sizeof(ad_show_dossiers_candidates)
               / sizeof(ad_show_dossiers_candidates[0])),
      NULL, NULL);
}

// Registration

void
dossier_show_register_commands(void)
{
  cmd_register("dossier", "dossier",
      "show dossier [<id>]",
      "List dossiers in the working namespace, or detail one by id",
      "Without <id>, renders a colorized table of every dossier in\n"
      "the cd'd userns. With <id>, displays that dossier's identity,\n"
      "signatures, and facts. The dossier must belong to the cd'd\n"
      "namespace. Use the 'stats' subcommand for process-wide counters.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_dossier, NULL, "show", "dos", ad_show_dossier,
      (uint8_t)(sizeof(ad_show_dossier) / sizeof(ad_show_dossier[0])), NULL, NULL);

  cmd_register("dossier", "stats",
      "show dossier stats",
      "Process-wide dossier subsystem counters",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_dossier_stats, NULL, "show/dossier", NULL, NULL, 0, NULL, NULL);
}
