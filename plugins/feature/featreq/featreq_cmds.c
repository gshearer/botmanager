// botmanager — MIT
// featreq write surface: `feature` files a request, `bug` files the
// commonest kind of one, `feature status` moves a row along and
// `feature note` writes the answer onto it. Filing needs a registered
// user (group `user`) because a request is attributed; the other two
// need the owner, because the board is theirs to work through.

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

// A description, or the note answering one, arrives as whatever the
// method handed us and leaves here as text safe to store and to echo
// back into a channel: no C0 controls (a stray \x01 would otherwise
// come back out of the DB as a colour marker of the user's choosing),
// no tabs, and no space at either end — the note surface hands us the
// gap that followed the id, and a description that starts one column
// in on the card is the tell.
//
// Dropping bytes below 0x20 cannot damage a UTF-8 sequence — every
// continuation byte is >= 0x80 — so multi-byte text passes through
// whole. Returns the length in display columns, which is what both
// caps are expressed in.
static size_t
fr_text_clean(const char *in, char *out, size_t cap)
{
  size_t n = 0;

  while(*in == ' ' || *in == '\t')
    in++;

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

// Both caps are measured in display columns and refused in the same
// sentence, so the knob a reader would go and change is named by the
// command's help rather than repeated here in two dialects.
static void
fr_cap_reply(const cmd_ctx_t *ctx, size_t cols, size_t max)
{
  char line[FR_LINE_SZ];

  snprintf(line, sizeof(line),
      "That's " CLR_YELLOW "%zu" CLR_RESET " characters and the limit "
      "is " CLR_CYAN "%zu" CLR_RESET ". Trim it and I'll take it.",
      cols, max);
  cmd_reply(ctx, line);
}

// Both owner verbs address a request by id, and both have to say the
// same thing about one that is not there.
static void
fr_no_such(const cmd_ctx_t *ctx, int64_t id)
{
  char line[FR_LINE_SZ];

  snprintf(line, sizeof(line),
      "Nothing on the board carries id " CLR_YELLOW "%" PRId64 CLR_RESET
      ".", id);
  cmd_reply(ctx, line);
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

  cols = fr_text_clean(p, desc, sizeof(desc));
  max  = (size_t)kv_get_uint(FR_KV_MAX_DESC);

  if(cols == 0)
  {
    cmd_reply(ctx, "There's nothing in there I can write down.");
    return;
  }

  if(cols > max)
  {
    fr_cap_reply(ctx, cols, max);
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
    fr_no_such(ctx, id);
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
// feature note                                                        //
// ------------------------------------------------------------------ //

// No vocabulary in it, so a literal like `bug`'s. --clear is spelled
// the way `set kv` spells it, for the same job.
#define FR_NOTE_USAGE  "feature note <id> <text|--clear>"

// The note is scanned out of ctx->args by hand for the reason the
// description is (see the arg table below): declared as a REST argument
// the framework would cut it to CMD_ARG_SZ long before the column cap
// could measure it. Subcommand resolution has already eaten the word
// `note`, so the id is the first token on this line.
static void
fr_cmd_note(const cmd_ctx_t *ctx)
{
  const char *p = (ctx->args != NULL) ? ctx->args : "";
  char        tok[24];
  char        note[FR_NOTE_SZ];
  char        line[FR_LINE_SZ];
  size_t      cols;
  size_t      max;
  int64_t     id;
  fr_upd_t    rc;

  p = fr_token(fr_skip_ws(p), tok, sizeof(tok));

  if(!fr_all_digits(tok))
  {
    snprintf(line, sizeof(line), "usage: %s", FR_NOTE_USAGE);
    cmd_reply(ctx, line);
    return;
  }

  id   = (int64_t)strtoll(tok, NULL, 10);
  cols = fr_text_clean(p, note, sizeof(note));
  max  = (size_t)kv_get_uint(FR_KV_MAX_NOTE);

  // --clear has to be the WHOLE of the note or it is prose: a note may
  // perfectly well be about a flag, and quietly dropping the words
  // after one would be a worse answer than writing them down.
  if(strcasecmp(note, "--clear") == 0)
  {
    note[0] = '\0';
    cols    = 0;
  }

  else if(cols == 0)
  {
    snprintf(line, sizeof(line), "usage: %s", FR_NOTE_USAGE);
    cmd_reply(ctx, line);
    return;
  }

  else if(cols > max)
  {
    fr_cap_reply(ctx, cols, max);
    return;
  }

  rc = fr_db_set_note(id, note);

  if(rc == FR_UPD_NO_ROW)
  {
    fr_no_such(ctx, id);
    return;
  }

  if(rc != FR_UPD_OK)
  {
    cmd_reply(ctx, "I couldn't reach the board. :~(");
    return;
  }

  if(note[0] == '\0')
    snprintf(line, sizeof(line),
        CLR_BOLD "#%" PRId64 CLR_RESET "'s note is gone.", id);

  else
    snprintf(line, sizeof(line),
        CLR_GREEN "Noted" CLR_RESET " on " CLR_BOLD "#%" PRId64 CLR_RESET
        " — " CLR_GRAY "show feature %" PRId64 CLR_RESET
        " reads it back.", id, id);

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

static const cmd_decl_t feature_decl = {
  .module      = "featreq",
  .name        = "feature",
  .usage       = fr_feature_usage,
  .description = "Ask for a feature, or report a bug.",
  .help_long   =
      "Writes your request to the board every bot shares — it is not "
      "scoped to this channel or this namespace. --type defaults to "
      "`feat` (a feature you would like); use `bug` for something "
      "broken and `change` for something that should work "
      "differently. `bug <description>` is the same thing said "
      "shorter. The description is capped at "
      "plugin.featreq.max_desc_cols characters. Read the board back "
      "with `show feature`, and one request in full with `show "
      "feature <id>`. Note that a description beginning with `status` "
      "or `note` is read as one of the owner's subcommands — start it "
      "with anything else.",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = fr_cmd_feature,
  .abbrev      = "feat",
};

static const cmd_decl_t bug_decl = {
  .module      = "featreq",
  .name        = "bug",
  .usage       = FR_BUG_USAGE,
  .description = "Report something broken.",
  .help_long   =
      "Files a bug on the same board `feature` writes to, and is "
      "exactly `feature --type bug <description>`. Everything true of "
      "that verb is true here: the board is global rather than scoped "
      "to this channel, the description is capped at "
      "plugin.featreq.max_desc_cols characters, and `show feature` "
      "reads it back. A leading --type still overrides, for the rare "
      "line that turns out not to be a bug after all.",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = fr_cmd_bug,
};

static const cmd_decl_t feature_status_decl = {
  .module      = "featreq",
  .name        = "status",
  .usage       = fr_status_usage,
  .description = "Move a request along the board (owner).",
  .help_long   =
      "Sets the request's status and stamps the moment it changed. "
      "Accepts `new`, `in-prog`, `on-hold`, `completed` and "
      "`canceled` (also "
      "`done` and `cancelled`). The request itself is never edited or "
      "deleted, only moved; `feature note` is where an answer to one "
      "goes.",
  .group       = USERNS_GROUP_OWNER,
  .level       = 65535,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = fr_cmd_status,
  .parent_path = "feature",
  .arg_desc    = fr_status_args,
  .arg_count   = (uint8_t)(sizeof(fr_status_args) / sizeof(fr_status_args[0])),
};

static const cmd_decl_t feature_note_decl = {
  .module      = "featreq",
  .name        = "note",
  .usage       = FR_NOTE_USAGE,
  .description = "Write the answer onto a request (owner).",
  .help_long   =
      "Attaches a note to the request and stamps when it was written. "
      "This is where an investigation's answer lives once the row is "
      "closed — `show feature <id>` prints it under the description, "
      "so it outlasts the channel it was worked out in. The board "
      "itself does not show notes; the card does. A second note "
      "replaces the first and `--clear` removes it, which is the only "
      "way anything on a request is ever unwritten. The note is "
      "capped at plugin.featreq.max_note_cols characters.",
  .group       = USERNS_GROUP_OWNER,
  .level       = 65535,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = fr_cmd_note,
  .parent_path = "feature",
};

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

  if(cmd_register(&feature_decl) != SUCCESS)
    return(FAIL);

  // A root command of its own rather than an alias: an alias renames a
  // verb, and what makes this one worth having is the argument it
  // supplies. Reporting something broken is most of what the board
  // receives, and `feature --type bug` is a lot of typing to say it.
  if(cmd_register(&bug_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&feature_status_decl) != SUCCESS)
    return(FAIL);

  // No arg spec, so ctx->args reaches the handler whole — see
  // fr_cmd_note. That costs the framework's usage reply on a missing
  // argument, which the handler answers with FR_NOTE_USAGE itself.
  if(cmd_register(&feature_note_decl) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

void
fr_commands_unregister(void)
{
  cmd_unregister_path("feature");
  cmd_unregister_path("bug");
}
