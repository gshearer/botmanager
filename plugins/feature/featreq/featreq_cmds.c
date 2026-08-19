// botmanager — MIT
// featreq write surface: `feature` files a request, `bug` files the
// commonest kind of one, `feature status` moves a row along. Filing
// needs a registered user (group `user`) because a request is
// attributed; moving one needs the owner, because the board is theirs
// to work through.

#define FEATREQ_INTERNAL
#include "featreq.h"

#include "colors.h"
#include "display.h"
#include "kv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ //
// The trust boundary                                                  //
// ------------------------------------------------------------------ //

// A description arrives as whatever the method handed us, and leaves
// here as text safe to store and to echo back into a channel: no C0
// controls (a stray \x01 would otherwise come back out of the DB as a
// colour marker of the user's choosing), no tabs, no trailing space.
//
// Dropping bytes below 0x20 cannot damage a UTF-8 sequence — every
// continuation byte is >= 0x80 — so multi-byte text passes through
// whole. Returns the length in display columns, which is what the cap
// is expressed in.
static size_t
fr_desc_clean(const char *in, char *out, size_t cap)
{
  size_t n = 0;

  for(; *in != '\0' && n + 1 < cap; in++)
  {
    const unsigned char c = (unsigned char)*in;

    if(c == '\t')
      out[n++] = ' ';

    else if(c >= 0x20 && c != 0x7f)
      out[n++] = (char)c;
  }

  while(n > 0 && out[n - 1] == ' ')
    n--;

  out[n] = '\0';
  return(display_vis_len(out));
}

// ------------------------------------------------------------------ //
// feature                                                             //
// ------------------------------------------------------------------ //

// `feature`'s usage line, rendered from the type vocabulary at
// registration (see below) so that renaming a type cannot leave a stale
// copy of the old word in a help string. `bug` names no vocabulary and
// so is a literal.
#define FR_BUG_USAGE  "bug <description>"

static char fr_feature_usage[CMD_USAGE_SZ];

// Who filed it, as we know them. `feature` is gated on the `user`
// group, so an authenticated username is the normal case.
static const char *
fr_caller(const cmd_ctx_t *ctx)
{
  if(ctx->username != NULL && ctx->username[0] != '\0')
    return(ctx->username);

  if(ctx->msg != NULL && ctx->msg->nickname[0] != '\0')
    return(ctx->msg->nickname);

  return((ctx->msg != NULL) ? ctx->msg->sender : "");
}

// Consume the leading `--flag value` run, folding what it finds into
// `type`. Returns the first byte of the description, or NULL after
// replying about a flag it could not use.
//
// Only a LEADING flag is a flag. Once ordinary text has started, a
// `--type` in the middle of a sentence is the user describing their
// problem, and it stays in the description verbatim.
static const char *
fr_flags_take(const cmd_ctx_t *ctx, const char *p, fr_type_t *type)
{
  char line[FR_LINE_SZ];
  char tokens[128];

  for(;;)
  {
    char        flag[32];
    char        val [32];
    const char *q;

    p = fr_skip_ws(p);

    if(p[0] != '-' || p[1] != '-')
      return(p);

    q = fr_token(p, flag, sizeof(flag));
    q = fr_skip_ws(q);
    q = fr_token(q, val, sizeof(val));

    if(strcasecmp(flag, "--type") != 0)
    {
      snprintf(line, sizeof(line),
          "I don't know the option " CLR_YELLOW "%s" CLR_RESET
          ". The only one here is --type.", flag);
      cmd_reply(ctx, line);
      return(NULL);
    }

    if(!fr_type_parse(val, type))
    {
      fr_type_tokens(tokens, sizeof(tokens));
      snprintf(line, sizeof(line),
          "--type takes one of: " CLR_CYAN "%s" CLR_RESET ".", tokens);
      cmd_reply(ctx, line);
      return(NULL);
    }

    p = q;
  }
}

// Both filing verbs land here; what separates them is the type a line
// carries when it names none. A leading --type still wins on either,
// so `bug` is exactly `feature` with a different starting assumption
// rather than a second, subtly different parser.
static void
fr_file(const cmd_ctx_t *ctx, fr_type_t type)
{
  fr_new_t    req    = { 0 };
  const char *p      = (ctx->args != NULL) ? ctx->args : "";
  const char *method = NULL;
  char        desc[FR_DESC_SZ];
  char        line[FR_LINE_SZ];
  size_t      cols;
  size_t      max;
  int64_t     id;

  p = fr_flags_take(ctx, p, &type);

  if(p == NULL)   // fr_flags_take already said what was wrong
    return;

  if(*p == '\0')
  {
    snprintf(line, sizeof(line), "usage: %s",
        (type == FR_TYPE_BUG) ? FR_BUG_USAGE : fr_feature_usage);
    cmd_reply(ctx, line);
    return;
  }

  cols = fr_desc_clean(p, desc, sizeof(desc));
  max  = (size_t)kv_get_uint(FR_KV_MAX_DESC);

  if(cols == 0)
  {
    cmd_reply(ctx, "There's nothing in there I can write down.");
    return;
  }

  if(cols > max)
  {
    snprintf(line, sizeof(line),
        "That's " CLR_YELLOW "%zu" CLR_RESET " characters and the limit "
        "is " CLR_CYAN "%zu" CLR_RESET ". Trim it and I'll take it.",
        cols, max);
    cmd_reply(ctx, line);
    return;
  }

  if(ctx->msg != NULL && ctx->msg->inst != NULL)
    method = method_inst_kind(ctx->msg->inst);

  req.type     = type;
  req.desc     = desc;
  req.username = fr_caller(ctx);
  req.nickname = (ctx->msg != NULL) ? ctx->msg->nickname : "";
  req.method   = (method != NULL)   ? method : "";

  // Not bot_inst_name(ctx->bot): with no bot instance that answers with
  // the string "(null)", which is a placeholder for a display and not a
  // name to write down. A request filed over the control socket came
  // through no bot, and the empty column is what says so.
  req.botname  = (ctx->bot != NULL) ? bot_inst_name(ctx->bot) : "";

  id = fr_db_add(&req);

  if(id < 0)
  {
    cmd_reply(ctx, "I couldn't reach the board. :~(");
    return;
  }

  snprintf(line, sizeof(line),
      CLR_GREEN "Filed" CLR_RESET " as " CLR_BOLD "#%" PRId64 CLR_RESET
      " (%s%s" CLR_RESET ") — " CLR_GRAY "show feature %" PRId64
      CLR_RESET " follows it.",
      id, fr_type_color(type), fr_type_word(type), id);
  cmd_reply(ctx, line);
}

static void
fr_cmd_feature(const cmd_ctx_t *ctx)
{
  fr_file(ctx, FR_TYPE_FEAT);
}

static void
fr_cmd_bug(const cmd_ctx_t *ctx)
{
  fr_file(ctx, FR_TYPE_BUG);
}

// ------------------------------------------------------------------ //
// feature status                                                      //
// ------------------------------------------------------------------ //

static void
fr_cmd_status(const cmd_ctx_t *ctx)
{
  fr_status_t status;
  fr_upd_t    rc;
  char        line[FR_LINE_SZ];
  char        tokens[128];
  int64_t     id;

  // Both arguments are REQUIRED in the spec above, and the framework
  // answers a missing one with the usage line before ever calling us,
  // so `parsed` is here and holds two.
  //
  // argv[0] passed CMD_ARG_DIGITS, so it is a bare run of digits.
  id = (int64_t)strtoll(ctx->parsed->argv[0], NULL, 10);

  if(!fr_status_parse(ctx->parsed->argv[1], &status))
  {
    fr_status_tokens(tokens, sizeof(tokens));
    snprintf(line, sizeof(line),
        "A status is one of: " CLR_CYAN "%s" CLR_RESET ".", tokens);
    cmd_reply(ctx, line);
    return;
  }

  rc = fr_db_set_status(id, status);

  if(rc == FR_UPD_NO_ROW)
  {
    snprintf(line, sizeof(line),
        "Nothing on the board carries id " CLR_YELLOW "%" PRId64 CLR_RESET
        ".", id);
    cmd_reply(ctx, line);
    return;
  }

  if(rc != FR_UPD_OK)
  {
    cmd_reply(ctx, "I couldn't reach the board. :~(");
    return;
  }

  snprintf(line, sizeof(line),
      CLR_BOLD "#%" PRId64 CLR_RESET " is now %s%s" CLR_RESET ".",
      id, fr_status_color(status), fr_status_word(status));
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

// The description is scanned by hand rather than declared as a REST
// argument: the cap is in display columns and the framework's token
// buffer would have truncated the line to CMD_ARG_SZ before we could
// measure it — reporting "205 characters" for something the user typed
// at 300.
static const cmd_arg_desc_t fr_status_args[] = {
  { "id",     CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 18, NULL },
  { "status", CMD_ARG_NONE,   CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

// `feature status`'s usage line, built at registration from the same
// vocabulary the parser uses, so the values a caller is offered and the
// values that will be accepted cannot drift apart.
//
// File-scope rather than a local because cmd_register stores the
// pointer rather than copying: it has to outlive this call. That is the
// same lifetime a string literal here already has — both live in this
// plugin's mapping, and core reclaims the registration when the mapping
// goes (`/plugin audit featreq` lists the usage pointer for exactly
// this reason). fr_feature_usage, declared at the top of this file
// because the filing path answers an empty line with it, is the same.
static char fr_status_usage[CMD_USAGE_SZ];

bool
fr_commands_register(void)
{
  char values[FR_SYNTAX_SZ];

  fr_type_syntax(values, sizeof(values));
  snprintf(fr_feature_usage, sizeof(fr_feature_usage),
      "feature [--type %s] <description>", values);

  fr_status_syntax(values, sizeof(values));
  snprintf(fr_status_usage, sizeof(fr_status_usage),
      "feature status <id> %s", values);

  if(cmd_register("featreq", "feature",
        fr_feature_usage,
        "Ask for a feature, or report a bug.",
        "Writes your request to the board every bot shares — it is not "
        "scoped to this channel or this namespace. --type defaults to "
        "`feat` (a feature you would like); use `bug` for something "
        "broken and `change` for something that should work "
        "differently. `bug <description>` is the same thing said "
        "shorter. The description is capped at "
        "plugin.featreq.max_desc_cols characters. Read the board back "
        "with `show feature`, and one request in full with `show "
        "feature <id>`. Note that a description beginning with the word "
        "`status` is read as the owner's `feature status` subcommand — "
        "start it with anything else.",
        USERNS_GROUP_USER, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        fr_cmd_feature, NULL, NULL, "feat",
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // A root command of its own rather than an alias: an alias renames a
  // verb, and what makes this one worth having is the argument it
  // supplies. Reporting something broken is most of what the board
  // receives, and `feature --type bug` is a lot of typing to say it.
  if(cmd_register("featreq", "bug",
        FR_BUG_USAGE,
        "Report something broken.",
        "Files a bug on the same board `feature` writes to, and is "
        "exactly `feature --type bug <description>`. Everything true of "
        "that verb is true here: the board is global rather than scoped "
        "to this channel, the description is capped at "
        "plugin.featreq.max_desc_cols characters, and `show feature` "
        "reads it back. A leading --type still overrides, for the rare "
        "line that turns out not to be a bug after all.",
        USERNS_GROUP_USER, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        fr_cmd_bug, NULL, NULL, NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  if(cmd_register("featreq", "status",
        fr_status_usage,
        "Move a request along the board (owner).",
        "Sets the request's status and stamps the moment it changed. "
        "Accepts `new`, `in-prog`, `completed` and `canceled` (also "
        "`done` and `cancelled`). Nothing else on the board changes — a "
        "request is never edited or deleted, only moved.",
        USERNS_GROUP_OWNER, 65535, CMD_SCOPE_ANY, METHOD_T_ANY,
        fr_cmd_status, NULL, "feature", NULL,
        fr_status_args,
        (uint8_t)(sizeof(fr_status_args) / sizeof(fr_status_args[0])),
        NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

void
fr_commands_unregister(void)
{
  cmd_unregister_path("feature");
  cmd_unregister_path("bug");
}
