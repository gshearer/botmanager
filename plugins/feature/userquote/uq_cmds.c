// botmanager — MIT
// userquote command surface: `quote` (recall) with `add` / `del`
// subcommands. Registered globally so it works from any bot on any
// method. Recall is open to everyone; `add` requires a registered user;
// `del` requires a registered user at level >= 100 — all enforced by the
// command system's per-leaf group/level gate.

#define USERQUOTE_INTERNAL
#include "userquote.h"

#include "bot.h"
#include "colors.h"
#include "kv.h"
#include "userns.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ //
// Helpers                                                             //
// ------------------------------------------------------------------ //

// Who is issuing the command: the authenticated username if identified,
// otherwise the protocol nickname (or raw sender as last resort).
static const char *
uq_caller(const cmd_ctx_t *ctx)
{
  if(ctx->username != NULL && ctx->username[0] != '\0')
    return(ctx->username);

  if(ctx->msg != NULL && ctx->msg->nickname[0] != '\0')
    return(ctx->msg->nickname);

  return((ctx->msg != NULL) ? ctx->msg->sender : "");
}

// Byte-truncate a string in place to at most `max` characters. Cosmetic
// bound only — multi-byte sequences are not split-aware, which is fine
// for a length cap that exists to stop pathological input.
static void
uq_truncate(char *s, uint32_t max)
{
  if(max > 0 && strlen(s) > max)
    s[max] = '\0';
}

// ------------------------------------------------------------------ //
// quote — recall                                                      //
// ------------------------------------------------------------------ //

static void
uq_cmd_recall(const cmd_ctx_t *ctx)
{
  userns_t  *ns = userns_session_resolve(ctx);
  uq_quote_t q;
  int64_t    id      = 0;
  bool       verbose = false;
  char       sayer[UQ_SAYER_SZ] = {0};
  char       line[UQ_QUOTE_SZ + 256];
  const char *p;

  if(ns == NULL)   // resolver already replied with "no namespace set"
    return;

  // Tokenise ctx->args: -v (verbose), -i[=| ]<id>, else first bare token
  // is the search key (sayer).
  p = (ctx->args != NULL) ? ctx->args : "";

  while(*p != '\0')
  {
    char tok[UQ_SAYER_SZ];
    size_t n = 0;

    while(*p == ' ' || *p == '\t') p++;

    while(*p != '\0' && *p != ' ' && *p != '\t' && n < sizeof(tok) - 1)
      tok[n++] = *p++;

    tok[n] = '\0';

    while(*p != '\0' && *p != ' ' && *p != '\t') p++;  // drop overflow

    if(tok[0] == '\0')
      continue;

    if(tok[0] == '-')
    {
      switch(tolower((unsigned char)tok[1]))
      {
        case 'v':
          verbose = true;
          break;

        case 'i':
          // -i=<id>, -i<id>, or -i <id>
          if(tok[2] == '=' || (tok[2] >= '0' && tok[2] <= '9'))
            id = (int64_t)strtoll(tok + (tok[2] == '=' ? 3 : 2), NULL, 10);
          else
          {
            while(*p == ' ' || *p == '\t') p++;
            id = (int64_t)strtoll(p, NULL, 10);
            while(*p != '\0' && *p != ' ' && *p != '\t') p++;
          }
          break;

        case 'h':
          cmd_reply(ctx, "usage: quote [-v] [-i <id>] [sayer]");
          return;

        default:
          break;
      }
    }

    else if(sayer[0] == '\0')
      snprintf(sayer, sizeof(sayer), "%s", tok);
  }

  if(!uq_db_get(ns->id, id, sayer, &q))
  {
    cmd_reply(ctx, "No matching quote. :~(");
    return;
  }

  if(verbose)
    snprintf(line, sizeof(line),
        CLR_GRAY "[#%" PRId64 "]" CLR_RESET " " CLR_CYAN "%s" CLR_RESET
        " quoted " CLR_CYAN "%s" CLR_RESET " on %s (last seen %s): %s",
        q.id, q.quoter[0] ? q.quoter : "someone",
        q.sayer[0] ? q.sayer : "someone",
        q.created, q.lastview, q.quote);

  else
    snprintf(line, sizeof(line),
        CLR_GRAY "[#%" PRId64 "]" CLR_RESET " %s", q.id, q.quote);

  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// quote add                                                           //
// ------------------------------------------------------------------ //

static void
uq_cmd_add(const cmd_ctx_t *ctx)
{
  userns_t   *ns = userns_session_resolve(ctx);
  const char *method;
  const char *channel;
  const char *args;
  char        sayer[UQ_SAYER_SZ] = {0};
  char        quote[UQ_QUOTE_SZ] = {0};
  char        line[256];
  int64_t     id;

  if(ns == NULL)
    return;

  method  = (ctx->msg != NULL) ? method_inst_name(ctx->msg->inst) : NULL;
  channel = (ctx->msg != NULL) ? ctx->msg->channel : "";

  args = (ctx->args != NULL) ? ctx->args : "";
  while(*args == ' ' || *args == '\t') args++;

  if(*args == '\0')
  {
    // No arguments: immortalise the last line this bot witnessed here.
    bot_public_line_t ll;
    const char       *user;

    if(channel == NULL || channel[0] == '\0')
    {
      cmd_reply(ctx, "usage: quote add <sayer> <text>  "
          "(the no-arg form only works in a channel)");
      return;
    }

    if(!bot_last_public_line(ctx->bot, ctx->msg->inst, channel, &ll))
    {
      cmd_reply(ctx, "I haven't seen anything quotable here yet. :~(");
      return;
    }

    // Resolve the speaker's nick to a username where possible.
    {
      char ubuf[USERNS_USER_SZ];

      user = bot_identity_resolve(ctx->bot, ctx->msg->inst,
          ll.sender, NULL, ubuf, sizeof(ubuf)) ? ubuf : NULL;
      snprintf(sayer, sizeof(sayer), "%s",
          (user != NULL && user[0] != '\0') ? user : ll.nickname);
    }

    // Bound the witnessed text (up to METHOD_TEXT_SZ) so it always fits
    // alongside the "<nick> " prefix; a length cap is applied below too.
    {
      int room = (int)sizeof(quote) - (int)strlen(ll.nickname) - 8;

      if(room < 0)
        room = 0;

      if(ll.is_action)
        snprintf(quote, sizeof(quote), "* %s %.*s",
            ll.nickname, room, ll.text);
      else
        snprintf(quote, sizeof(quote), "<%s> %.*s",
            ll.nickname, room, ll.text);
    }
  }

  else
  {
    // Explicit: first whitespace-delimited token is the search key, the
    // remainder is the quote body (stored verbatim, as the user typed it).
    const char *sp = args;
    size_t      klen;

    while(*sp != '\0' && *sp != ' ' && *sp != '\t') sp++;
    klen = (size_t)(sp - args);

    if(klen == 0 || klen >= sizeof(sayer))
    {
      cmd_reply(ctx, "usage: quote add <sayer> <text>");
      return;
    }

    memcpy(sayer, args, klen);
    sayer[klen] = '\0';

    while(*sp == ' ' || *sp == '\t') sp++;

    if(*sp == '\0')
    {
      cmd_reply(ctx, "usage: quote add <sayer> <text>");
      return;
    }

    snprintf(quote, sizeof(quote), "%s", sp);
  }

  uq_truncate(sayer, (uint32_t)kv_get_uint(UQ_KV_MAX_SAYER));
  uq_truncate(quote, (uint32_t)kv_get_uint(UQ_KV_MAX_QUOTE));

  id = uq_db_add(ns->id, method != NULL ? method : "",
      channel != NULL ? channel : "", sayer, uq_caller(ctx), quote);

  if(id < 0)
  {
    cmd_reply(ctx, "Failed to save the quote. :~(");
    return;
  }

  snprintf(line, sizeof(line),
      CLR_GREEN "Quote saved" CLR_RESET " as " CLR_GRAY "#%" PRId64
      CLR_RESET " (%s)", id, sayer);
  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// quote del                                                           //
// ------------------------------------------------------------------ //

static void
uq_cmd_del(const cmd_ctx_t *ctx)
{
  userns_t   *ns = userns_session_resolve(ctx);
  const char *ids;
  char        line[128];
  int64_t     id;
  int         affected;

  if(ns == NULL)
    return;

  ids = (ctx->parsed != NULL && ctx->parsed->argc > 0)
      ? ctx->parsed->argv[0] : NULL;

  if(ids == NULL)
  {
    cmd_reply(ctx, "usage: quote del <id>");
    return;
  }

  id       = (int64_t)strtoll(ids, NULL, 10);
  affected = uq_db_del(ns->id, id);

  if(affected < 0)
    cmd_reply(ctx, "Failed to delete the quote. :~(");

  else if(affected == 0)
  {
    snprintf(line, sizeof(line), "No quote #%" PRId64 " here.", id);
    cmd_reply(ctx, line);
  }

  else
  {
    snprintf(line, sizeof(line),
        CLR_GREEN "Deleted" CLR_RESET " quote #%" PRId64 ".", id);
    cmd_reply(ctx, line);
  }
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

// NL hint: let the chat bridge route "what did X say" to a recall.
static const cmd_nl_slot_t uq_recall_slots[] = {
  { .name = "sayer", .type = CMD_NL_ARG_NICK, .flags = CMD_NL_SLOT_OPTIONAL },
};

static const cmd_nl_example_t uq_recall_examples[] = {
  { .utterance = "quote doc", .invocation = "/quote doc" },
  { .utterance = "what did hawken say?", .invocation = "/quote hawken" },
};

static const cmd_nl_t uq_recall_nl = {
  .when          = "User wants to recall a saved quote, optionally by who said it.",
  .syntax        = "/quote [sayer]",
  .slots         = uq_recall_slots,
  .slot_count    = (uint8_t)(sizeof(uq_recall_slots) / sizeof(uq_recall_slots[0])),
  .examples      = uq_recall_examples,
  .example_count = (uint8_t)(sizeof(uq_recall_examples) / sizeof(uq_recall_examples[0])),
};

static const cmd_arg_desc_t uq_del_args[] = {
  { "id", CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 18, NULL },
};

static const cmd_decl_t quote_decl = {
  .module      = "userquote",
  .name        = "quote",
  .usage       = "quote [-v] [-i <id>] [sayer]",
  .description = "Recall a saved quote (least-recently-seen first).",
  .help_long   = "With no argument, cycles the quote book for this namespace. "
                 "Give a name to recall that person's quotes, -i <id> for a "
                 "specific one, or -v for the full provenance card.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = uq_cmd_recall,
  .abbrev      = "\"",
  .nl          = &uq_recall_nl,
};

static const cmd_decl_t quote_add_decl = {
  .module      = "userquote",
  .name        = "add",
  .usage       = "quote add [<sayer> <text>]",
  .description = "Save a quote. With no arguments, quotes the last thing said "
                 "in the channel.",
  .help_long   =
      "`quote add <sayer> <text>` saves an explicit quote under the "
      "search key <sayer>. `quote add` on its own captures the most "
      "recent line in the channel and attributes it to whoever said "
      "it (resolved to their username when known).",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = uq_cmd_add,
  .parent_path = "quote",
};

static const cmd_decl_t quote_del_decl = {
  .module      = "userquote",
  .name        = "del",
  .usage       = "quote del <id>",
  .description = "Delete a quote by id (registered users, level >= 100).",
  .group       = USERNS_GROUP_USER,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = uq_cmd_del,
  .parent_path = "quote",
  .arg_desc    = uq_del_args,
  .arg_count   = 1,
};

bool
uq_commands_register(void)
{
  // Root: recall. Alias `"` mirrors the old quotebot muscle memory.
  if(cmd_register(&quote_decl) != SUCCESS)
    return(FAIL);

  // quote add — registered users, any level.
  if(cmd_register(&quote_add_decl) != SUCCESS)
    return(FAIL);

  // quote del — registered users at level >= 100.
  if(cmd_register(&quote_del_decl) != SUCCESS)
    return(FAIL);

  // `show quotes` hangs off the observability root, not off `quote`.
  if(uq_show_register() != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// One path takes the whole book: `quote` and its `add` / `del` leaves,
// freed depth-first. The bare names that used to make this ambiguous are
// no longer what we address the tree by.
void
uq_commands_unregister(void)
{
  cmd_unregister_path("quote");
  uq_show_unregister();
}
