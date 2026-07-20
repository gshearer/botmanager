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
    user = bot_session_find(ctx->bot, ctx->msg->inst, ll.sender);
    snprintf(sayer, sizeof(sayer), "%s",
        (user != NULL && user[0] != '\0') ? user : ll.nickname);

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

bool
uq_commands_register(void)
{
  // Root: recall. Alias `"` mirrors the old quotebot muscle memory.
  if(cmd_register("userquote", "quote",
        "quote [-v] [-i <id>] [sayer]",
        "Recall a saved quote (least-recently-seen first).",
        "With no argument, cycles the quote book for this namespace. "
        "Give a name to recall that person's quotes, -i <id> for a "
        "specific one, or -v for the full provenance card.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        uq_cmd_recall, NULL, NULL, "\"",
        NULL, 0, NULL, &uq_recall_nl) != SUCCESS)
    return(FAIL);

  // quote add — registered users, any level.
  if(cmd_register("userquote", "add",
        "quote add [<sayer> <text>]",
        "Save a quote. With no arguments, quotes the last thing said "
        "in the channel.",
        "`quote add <sayer> <text>` saves an explicit quote under the "
        "search key <sayer>. `quote add` on its own captures the most "
        "recent line in the channel and attributes it to whoever said "
        "it (resolved to their username when known).",
        USERNS_GROUP_USER, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        uq_cmd_add, NULL, "quote", NULL,
        NULL, 0, NULL, NULL) != SUCCESS)
    return(FAIL);

  // quote del — registered users at level >= 100.
  if(cmd_register("userquote", "del",
        "quote del <id>",
        "Delete a quote by id (registered users, level >= 100).",
        NULL,
        USERNS_GROUP_USER, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
        uq_cmd_del, NULL, "quote", NULL,
        uq_del_args, 1, NULL, NULL) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}

// Teardown note: the command system has no parent-aware unregister, and
// unregistering the `add` / `del` leaves by bare name would be ambiguous
// (many plugins register commands of those names). Like the whenmoon
// feature, we therefore leave the tree in place; it is freed wholesale by
// cmd_exit() at daemon shutdown. Hot-unload is not supported — reload via
// restart. Kept as a named hook so the lifecycle reads symmetrically.
void
uq_commands_unregister(void)
{
  clam(CLAM_DEBUG, UQ_CTX,
      "quote command tree left registered (freed at shutdown)");
}
