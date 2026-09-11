// botmanager — MIT
// /optout and /optin — a person's standing refusal of what the bot does
// about them on its own: the weather watch, birthday wishes, follow-ups
// and price alerts.
//
// The initiative rule (CHATBOT.md) decides whether a chore MAY act on a
// person — *you said so, you asked me*. This decides whether it still
// should, and it is the person's call alone. A refusal is not a fact
// about them and does not live with the facts: facts decay, the
// extractor rewrites them and the persona reads them aloud, and a
// refusal must do none of those. It is a row in dossier_optout, which
// the dossier layer carries through merges and splits.
//
// Enforced in two places, never trusted to one:
//
//   the scan      every chore's WHERE carries DOSSIER_OPTOUT_EXCLUDES,
//                 so nobody who refused is selected, polled or named
//   the delivery  deferred_deliver_row drops a chore's row written
//                 before the refusal, which the scan can no longer undo
#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "db.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#define OPTOUT_CTX       "chatbot"
#define OPTOUT_REPLY_SZ  512
#define OPTOUT_LIST_SZ   192

// One refusable chore. `names` is every word a person might type for it,
// and its FIRST word is the one the bot says back ("optin weather"), so
// the canonical token and the vocabulary cannot drift apart.
typedef struct
{
  const char *chore;    // SOUL_CHORE_*: the key dossier_optout stores
  const char *source;   // the chat_deferred.source it writes; NULL if none
  const char *label;    // what a person hears it called
  const char *how;      // how it comes to be about them at all
  const char *names;    // space-separated; first is canonical
} optout_chore_t;

static const optout_chore_t optout_chores[] = {
  { SOUL_CHORE_WEATHER,    NULL,         "weather alerts",
    "I watch for severe weather wherever you've told me you live",
    "weather wx storm storms" },
  { SOUL_CHORE_OCCASIONS,  "occasion",   "birthday wishes",
    "I wish you a happy birthday once you've told me when it is",
    "birthdays birthday bday bdays occasions" },
  { SOUL_CHORE_FOLLOWUPS,  "followup",   "follow-ups",
    "I ask how things went after you've told me about a plan",
    "followups followup follow-ups follow-up checkins check-ins" },
  { SOUL_CHORE_PRICEWATCH, "pricewatch", "price alerts",
    "I report the price watches you set with 'pricewatch'",
    "prices price pricewatch" },
};

#define OPTOUT_N        (sizeof(optout_chores) / sizeof(optout_chores[0]))
#define OPTOUT_EVERY    ((1u << OPTOUT_N) - 1)

// "All" is its own bit, never the OR of the others: it is stored as
// DOSSIER_OPTOUT_ALL and also covers a chore nobody has written yet.
#define OPTOUT_ALL_BIT  (1u << 31)

// Words that qualify a chore rather than name one — "weather alerts",
// "birthday wishes", "price watch" — so a person may type what they
// would say. Every one of them is shared between two chores, which is
// why none of them may name either.
static const char optout_filler[] =
    "and or & the my me alert alerts wish wishes watch watches stuff";

// ---------- vocabulary ----------

// Is `tok` one of the space-separated `words`? Whole words only.
static bool
optout_word_in(const char *words, const char *tok)
{
  size_t tlen = strlen(tok);

  while(*words != '\0')
  {
    size_t wlen = strcspn(words, " ");

    if(wlen == tlen && strncmp(words, tok, tlen) == 0)
      return(true);

    words += wlen;
    words += strspn(words, " ");
  }

  return(false);
}

static int
optout_index_of(const char *chore)
{
  for(size_t i = 0; i < OPTOUT_N; i++)
    if(strcmp(optout_chores[i].chore, chore) == 0)
      return((int)i);

  return(-1);
}

static uint32_t
optout_bit_of(const char *chore)
{
  int i = optout_index_of(chore);

  return(i >= 0 ? 1u << i : 0);
}

// The chores `args` names, as a mask. Reads a line the most useful way
// it can (CMD.md §A cmd_nl_t makes a model one of your callers):
// commas, case and trailing punctuation are ignored, and qualifiers are
// skipped. FAIL puts the first word that names nothing into `bad`. An
// empty line is SUCCESS with an empty mask — the status question.
//
// "for <nick>" names whom the request is about and lands in `target`
// ("me" and "myself" leave it empty). It exists so a request about
// someone else has somewhere to go: measured, "stop wishing pops a
// happy birthday" collapsed onto the SPEAKER when the grammar offered
// no place for pops, and the bot then told the room it had spared him.
static bool
optout_parse(const char *args, uint32_t *mask, char *target,
    size_t target_sz, char *bad, size_t bad_sz)
{
  char  scratch[CMD_ARG_SZ];
  char *save;
  bool  want_target = false;

  *mask     = 0;
  target[0] = '\0';
  bad[0]    = '\0';

  if(args == NULL)
    return(SUCCESS);

  strlcpy(scratch, args, sizeof(scratch));

  for(char *tok = strtok_r(scratch, " \t,;/+", &save); tok != NULL;
      tok = strtok_r(NULL, " \t,;/+", &save))
  {
    size_t len = strlen(tok);
    bool   hit = false;

    while(len > 0 && strchr(".!?:", tok[len - 1]) != NULL)
      tok[--len] = '\0';

    // The nick keeps its case: it is compared, and it is said back.
    if(want_target)
    {
      want_target = false;

      if(strcasecmp(tok, "me") != 0 && strcasecmp(tok, "myself") != 0)
        strlcpy(target, tok, target_sz);

      continue;
    }

    for(size_t i = 0; i < len; i++)
      tok[i] = (char)tolower((unsigned char)tok[i]);

    if(strcmp(tok, "for") == 0)
    {
      want_target = true;
      continue;
    }

    if(len == 0 || optout_word_in(optout_filler, tok))
      continue;

    if(optout_word_in("all everything *", tok))
    {
      *mask |= OPTOUT_ALL_BIT;
      continue;
    }

    for(size_t i = 0; i < OPTOUT_N && !hit; i++)
      if(optout_word_in(optout_chores[i].names, tok))
      {
        *mask |= 1u << i;
        hit    = true;
      }

    if(!hit)
    {
      strlcpy(bad, tok, bad_sz);
      return(FAIL);
    }
  }

  return(SUCCESS);
}

// What a mask refuses in practice: every chore, when it holds "all".
static uint32_t
optout_effective(uint32_t mask)
{
  return((mask & OPTOUT_ALL_BIT) != 0 ? OPTOUT_EVERY : mask);
}

// "weather alerts, birthday wishes and follow-ups" — or the canonical
// tokens ("weather birthdays followups") when `tokens` is set, which is
// the shape a person can type straight back.
static void
optout_list(uint32_t mask, bool tokens, const char *conj, char *dst,
    size_t cap)
{
  uint32_t left = __builtin_popcount(mask & OPTOUT_EVERY);

  dst[0] = '\0';

  for(size_t i = 0; i < OPTOUT_N; i++)
  {
    const char *sep = "";

    if((mask & (1u << i)) == 0)
      continue;

    if(dst[0] != '\0')
      sep = tokens ? " " : (left == 1 ? conj : ", ");

    strlcat(dst, sep, cap);

    if(tokens)
    {
      size_t n   = strcspn(optout_chores[i].names, " ");
      size_t end = strlen(dst);

      if(end + n < cap)
      {
        memcpy(dst + end, optout_chores[i].names, n);
        dst[end + n] = '\0';
      }
    }

    else
      strlcat(dst, optout_chores[i].label, cap);

    left--;
  }
}

// Does this bot run the chore at all? Asked only when telling someone
// who never opted out how the chore would reach them — "I watch the
// weather where you live" is a lie on a bot whose watch is off.
static bool
optout_chore_live(const char *bot_name, size_t i)
{
  char suffix[64];

  if(kv_get_bot_uint(bot_name, "behavior.chat.enabled") == 0)
    return(false);

  snprintf(suffix, sizeof(suffix), "behavior.soul.%s.enabled",
      optout_chores[i].chore);

  return(kv_get_bot_uint(bot_name, suffix) != 0);
}

// ---------- storage ----------

static bool
optout_load(int64_t did, uint32_t *mask)
{
  db_result_t *res;
  char         sql[128];

  *mask = 0;

  snprintf(sql, sizeof(sql),
      "SELECT chore FROM dossier_optout WHERE dossier_id = %" PRId64, did);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, OPTOUT_CTX, "optout read for dossier %" PRId64 ": %s",
        did, res->error[0] != '\0' ? res->error : "(no driver error)");
    db_result_free(res);
    return(FAIL);
  }

  for(uint32_t r = 0; r < res->rows; r++)
  {
    const char *c = db_result_get(res, r, 0);
    int         i;

    if(c == NULL)
      continue;

    if(strcmp(c, DOSSIER_OPTOUT_ALL) == 0)
    {
      *mask |= OPTOUT_ALL_BIT;
      continue;
    }

    // A key no chore answers to any more refuses nothing we still do.
    i = optout_index_of(c);

    if(i >= 0)
      *mask |= 1u << i;
  }

  db_result_free(res);
  return(SUCCESS);
}

// Replace the person's whole set with `mask`, as one change: a refusal
// half-written between the DELETE and the INSERT would read as none.
static bool
optout_store(int64_t did, uint32_t mask)
{
  db_txn_t    *txn;
  db_result_t *res;
  bool         ok;
  char         err[DB_ERROR_SZ];
  char         sql[512];
  int          n;

  err[0] = '\0';
  txn    = db_txn_begin(err, sizeof(err));

  if(txn == NULL)
  {
    clam(CLAM_WARN, OPTOUT_CTX, "optout write: no transaction: %s",
        err[0] != '\0' ? err : "(no driver error)");
    return(FAIL);
  }

  snprintf(sql, sizeof(sql),
      "DELETE FROM dossier_optout WHERE dossier_id = %" PRId64, did);

  res = db_result_alloc();
  ok  = db_txn_query(txn, sql, res) == SUCCESS && res->ok;
  db_result_free(res);

  if(ok && mask != 0)
  {
    const char *sep = " ";

    // The chore names are compile-time constants, so nothing here needs
    // escaping; the only runtime value is the id. OPTOUT_N rows of a
    // 32-byte name fit the buffer many times over.
    n = snprintf(sql, sizeof(sql),
        "INSERT INTO dossier_optout (dossier_id, chore) VALUES");

    if((mask & OPTOUT_ALL_BIT) != 0)
      n += snprintf(sql + n, sizeof(sql) - (size_t)n,
          " (%" PRId64 ", '" DOSSIER_OPTOUT_ALL "')", did);

    else
      for(size_t i = 0; i < OPTOUT_N; i++)
        if((mask & (1u << i)) != 0)
        {
          n += snprintf(sql + n, sizeof(sql) - (size_t)n,
              "%s(%" PRId64 ", '%s')", sep, did, optout_chores[i].chore);
          sep = ", ";
        }

    res = db_result_alloc();
    ok  = db_txn_query(txn, sql, res) == SUCCESS && res->ok;
    db_result_free(res);
  }

  if(!ok)
  {
    db_txn_rollback(txn);
    clam(CLAM_WARN, OPTOUT_CTX, "optout write for dossier %" PRId64
        " rolled back", did);
    return(FAIL);
  }

  return(db_txn_commit(txn));
}

// ---------- the verbs ----------

static void
optout_refuse(const cmd_ctx_t *ctx, const char *text)
{
  cmd_result_set(ctx, CMD_RESULT_REFUSED);
  cmd_reply(ctx, text);
}

// Bare /optout or /optin: what is off, and what could be.
static void
optout_status(const cmd_ctx_t *ctx)
{
  int64_t  did;
  uint32_t cur = 0;
  uint32_t eff;
  char     off [OPTOUT_LIST_SZ];
  char     on  [OPTOUT_LIST_SZ];
  char     line[OPTOUT_REPLY_SZ];

  // Asking must not mint a dossier; somebody the bot keeps nothing on
  // has simply refused nothing.
  did = chatbot_sender_dossier(ctx, false);

  if(did > 0 && optout_load(did, &cur) != SUCCESS)
  {
    optout_refuse(ctx, "I couldn't look that up just now.");
    return;
  }

  eff = optout_effective(cur);

  optout_list(eff,                 false, " and ", off, sizeof(off));
  optout_list(OPTOUT_EVERY & ~eff, false, " and ", on,  sizeof(on));

  if((cur & OPTOUT_ALL_BIT) != 0)
    snprintf(line, sizeof(line), "you've opted out of everything I do"
        " about you on my own — 'optin all' brings it back.");

  else if(eff == 0)
  {
    optout_list(OPTOUT_EVERY, false, " or ", on, sizeof(on));
    snprintf(line, sizeof(line), "you haven't opted out of anything. I can"
        " leave you out of %s — or all of it.", on);
  }

  else if(on[0] == '\0')
    snprintf(line, sizeof(line), "you've opted out of %s.", off);

  else
    snprintf(line, sizeof(line), "you've opted out of %s. Still on: %s.",
        off, on);

  cmd_reply(ctx, line);
}

// The one handler behind both verbs; `out` says which way it turns.
static void
optout_apply(const cmd_ctx_t *ctx, bool out)
{
  const char *bot_name = bot_inst_name(ctx->bot);
  int64_t     did;
  uint32_t    req;
  uint32_t    req_eff;
  uint32_t    cur;
  uint32_t    eff;
  uint32_t    next;
  uint32_t    changed;
  char        bad [CMD_ARG_SZ];
  char        who [METHOD_NICKNAME_SZ];
  char        list[OPTOUT_LIST_SZ];
  char        toks[OPTOUT_LIST_SZ];
  char        line[OPTOUT_REPLY_SZ];

  if(optout_parse(ctx->args, &req, who, sizeof(who), bad, sizeof(bad))
      != SUCCESS)
  {
    snprintf(line, sizeof(line), "'%s' isn't something I do on my own —"
        " I can leave you out of weather, birthdays, followups or prices,"
        " or all of it.", bad);
    optout_refuse(ctx, line);
    return;
  }

  // About somebody else. Not a refusal of the caller — a true answer they
  // are entitled to hear, so it stays CMD_RESULT_OK and the persona can
  // relay it; nothing is written for anyone.
  if(who[0] != '\0' && strcasecmp(who, ctx->msg->nickname) != 0
      && strcasecmp(who, ctx->msg->sender) != 0)
  {
    optout_list(req & OPTOUT_EVERY, true, NULL, toks, sizeof(toks));

    snprintf(line, sizeof(line), "only %s can opt themselves %s — they can"
        " tell me '%s %s' themselves.", who, out ? "out" : "back in",
        out ? "optout" : "optin",
        (req & OPTOUT_ALL_BIT) != 0 || toks[0] == '\0' ? "all" : toks);
    cmd_reply(ctx, line);
    return;
  }

  if(req == 0)
  {
    optout_status(ctx);
    return;
  }

  did = chatbot_sender_dossier(ctx, true);

  if(did <= 0)
  {
    optout_refuse(ctx, "I can't tell who you are well enough to keep that.");
    return;
  }

  if(optout_load(did, &cur) != SUCCESS)
  {
    optout_refuse(ctx, "something went wrong looking that up.");
    return;
  }

  eff     = optout_effective(cur);
  req_eff = optout_effective(req);

  // `next` is what gets stored and `changed` what the reply is about:
  // newly refused on the way out, actually restored on the way in.
  if(out)
  {
    next    = (req & OPTOUT_ALL_BIT) != 0 ? OPTOUT_ALL_BIT
            : (cur & OPTOUT_ALL_BIT) != 0 ? cur : cur | req;
    changed = req_eff & ~eff;
  }

  else
  {
    // Opting back into one after "all" keeps the rest refused — but as
    // named rows, so a chore added later is no longer covered. That is
    // the person narrowing their own "all", which is theirs to do.
    next    = (req & OPTOUT_ALL_BIT) != 0 ? 0
            : (cur & OPTOUT_ALL_BIT) != 0 ? OPTOUT_EVERY & ~req
            : cur & ~req;
    changed = req_eff & eff;
  }

  if(next != cur && optout_store(did, next) != SUCCESS)
  {
    optout_refuse(ctx, "something went wrong writing that down.");
    return;
  }

  clam(CLAM_INFO, OPTOUT_CTX,
      "bot=%s %s by %s (dossier %" PRId64 "): stored 0x%x -> 0x%x",
      bot_name, out ? "optout" : "optin", ctx->msg->sender, did, cur, next);

  optout_list(changed, false, " and ", list, sizeof(list));
  optout_list(changed, true,  NULL,    toks, sizeof(toks));

  // "Everything" names every chore, not just the ones this call newly
  // covered — listing only those would read as the whole of it.
  if(out && (req & OPTOUT_ALL_BIT) != 0 && changed != 0)
  {
    optout_list(OPTOUT_EVERY, false, " and ", list, sizeof(list));
    snprintf(line, sizeof(line), "done — I'll leave you out of everything"
        " I do about you on my own: %s, and anything I start doing later."
        " 'optin all' undoes it.", list);
  }

  else if(out && changed != 0)
    snprintf(line, sizeof(line), "done — no more %s from me. 'optin %s'"
        " undoes it.%s", list, toks,
        (changed & optout_bit_of(SOUL_CHORE_PRICEWATCH)) != 0
            ? " Your price watches stay set, just silent." : "");

  else if(out)
  {
    optout_list(req_eff, false, " and ", list, sizeof(list));
    snprintf(line, sizeof(line), "you're already opted out of %s.",
        (req & OPTOUT_ALL_BIT) != 0 ? "everything I do on my own" : list);
  }

  else if(changed != 0)
  {
    char still[OPTOUT_LIST_SZ];

    optout_list(optout_effective(next), false, " and ", still, sizeof(still));
    snprintf(line, sizeof(line), "done — %s back on.%s%s%s", list,
        still[0] != '\0' ? " Still off: " : "", still,
        still[0] != '\0' ? "." : "");
  }

  // Not opted out of it in the first place. The one case worth more
  // than a shrug is a single chore, because the person asking to be
  // "signed up" usually wants to know how it would ever reach them.
  else if(__builtin_popcount(req_eff) == 1)
  {
    size_t i = (size_t)__builtin_ctz(req_eff);

    if(optout_chore_live(bot_name, i))
      snprintf(line, sizeof(line), "you weren't opted out of %s — %s.",
          optout_chores[i].label, optout_chores[i].how);

    else
      snprintf(line, sizeof(line), "you weren't opted out of %s — though"
          " I'm not doing those here at the moment.", optout_chores[i].label);
  }

  else
    snprintf(line, sizeof(line), "you weren't opted out of any of that.");

  cmd_reply(ctx, line);
}

static void
cmd_optout(const cmd_ctx_t *ctx)
{
  optout_apply(ctx, true);
}

static void
cmd_optin(const cmd_ctx_t *ctx)
{
  optout_apply(ctx, false);
}

// ---------- what the rest of the plugin asks ----------

bool
chatbot_optout_has(int64_t dossier_id, const char *chore, bool *out)
{
  db_result_t *res;
  int          i;
  bool         ok;
  char         sql[192];

  *out = false;

  if(dossier_id <= 0)
    return(SUCCESS);

  // Only a chore this file knows can be spelled into the statement.
  i = optout_index_of(chore);

  if(i < 0)
    return(FAIL);

  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM dossier_optout WHERE dossier_id = %" PRId64
      " AND chore IN ('%s', '" DOSSIER_OPTOUT_ALL "') LIMIT 1",
      dossier_id, optout_chores[i].chore);

  res = db_result_alloc();
  ok  = db_query(sql, res) == SUCCESS && res->ok;

  if(ok)
    *out = res->rows > 0;

  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

const char *
chatbot_optout_chore_of_source(const char *source)
{
  for(size_t i = 0; i < OPTOUT_N; i++)
    if(optout_chores[i].source != NULL
        && strcmp(optout_chores[i].source, source) == 0)
      return(optout_chores[i].chore);

  return(NULL);
}

// ---------- registration ----------
//
// Both `.when` lines are routing, and every clause in them was measured
// in fresh channels — CHATBOT.md §Opting out has the numbers and the
// one known miss. Re-probe after touching either, and re-run the
// canonical /hush, /forget and /remind utterances with it. ⚠ optout's
// stanza renders at 965 of the builder's 1024 bytes: an example added
// here has to pay for itself or the whole command goes dark.

static const cmd_nl_slot_t optout_slots[] = {
  { .name  = "chores",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_OPTIONAL | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t optout_examples[] = {
  { .utterance  = "stop checking the weather where I live",
    .invocation = "/optout weather" },
  { .utterance  = "please don't do the birthday thing for me",
    .invocation = "/optout birthdays" },
  { .utterance  = "quit asking me how my stuff went",
    .invocation = "/optout followups" },
  { .utterance  = "no more weather or birthday stuff for me",
    .invocation = "/optout weather birthdays" },
  { .utterance  = "leave me out of all your automatic stuff",
    .invocation = "/optout all" },
  { .utterance  = "quit asking dave how his trip went",
    .invocation = "/optout followups for dave" },
  { .utterance  = "what have I opted out of?",
    .invocation = "/optout" },
};

// "not", never "stop": measured, "I'd rather you didn't make a fuss on
// my birthday" got a spoken "Noted." and no dispatch, twice, against a
// line that said "stop doing" — the bot had not started, so nothing
// matched. A refusal in advance is still a refusal.
static const cmd_nl_t optout_nl = {
  .when          = "Someone tells you NOT to do something about them on"
                   " your own — watch the weather where they live, wish"
                   " them happy birthday, ask how their plans went, report"
                   " their price alerts — or asks what they have opted out"
                   " of. About someone else? Add 'for <nick>'. Not a"
                   " request to be quiet right now.",
  .syntax        = "/optout <weather|birthdays|followups|prices|all>"
                   " [for <nick>] — several may be given; bare /optout"
                   " lists theirs",
  .slots         = optout_slots,
  .slot_count    = (uint8_t)(sizeof(optout_slots) / sizeof(optout_slots[0])),
  .examples      = optout_examples,
  .example_count = (uint8_t)(sizeof(optout_examples)
                             / sizeof(optout_examples[0])),
  .dispatch_text = NULL,
};

static const cmd_nl_slot_t optin_slots[] = {
  { .name  = "chores",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t optin_examples[] = {
  { .utterance  = "you can watch the weather for me again",
    .invocation = "/optin weather" },
  { .utterance  = "actually I'd like a birthday wish after all",
    .invocation = "/optin birthdays" },
  { .utterance  = "fine, you can ask how things went again",
    .invocation = "/optin followups" },
  { .utterance  = "turn my price alerts back on",
    .invocation = "/optin prices" },
  { .utterance  = "opt me back in to everything",
    .invocation = "/optin all" },
  { .utterance  = "you can go back to wishing sam happy birthday",
    .invocation = "/optin birthdays for sam" },
};

static const cmd_nl_t optin_nl = {
  .when          = "Someone who opted out of one of those — weather alerts"
                   " where they live, birthday wishes, being asked how their"
                   " plans went, price alerts — wants it back, or says you"
                   " may do it again. About someone else? Add 'for <nick>'.",
  .syntax        = "/optin <weather|birthdays|followups|prices|all>"
                   " [for <nick>] — always name what they want back;"
                   " several may be given",
  .slots         = optin_slots,
  .slot_count    = (uint8_t)(sizeof(optin_slots) / sizeof(optin_slots[0])),
  .examples      = optin_examples,
  .example_count = (uint8_t)(sizeof(optin_examples)
                             / sizeof(optin_examples[0])),
  .dispatch_text = NULL,
};

static const cmd_decl_t optout_decl = {
  .module      = "chat",
  .name        = "optout",
  .usage       = "optout [weather|birthdays|followups|prices|all ...]",
  .description = "Stop the bot doing things about you on its own",
  .help_long   =
      "Stops the bot doing something about YOU on its own initiative,\n"
      "and only for you:\n"
      "\n"
      "  weather     watching for severe weather where you live\n"
      "  birthdays   wishing you a happy birthday\n"
      "  followups   asking how a plan you mentioned went\n"
      "  prices      reporting your price watches\n"
      "  all         every one of these, and any the bot takes up later\n"
      "\n"
      "Name one or several; with none it lists what you have opted out\n"
      "of. It lasts until you 'optin', on every bot that shares this\n"
      "one's memory of you. The bot keeps what it knows — it stops\n"
      "acting on it. Anything you ask for yourself, like a reminder or\n"
      "a weather lookup, is untouched.",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_optout,
  .nl          = &optout_nl,
};

static const cmd_decl_t optin_decl = {
  .module      = "chat",
  .name        = "optin",
  .usage       = "optin <weather|birthdays|followups|prices|all ...>",
  .description = "Undo an optout",
  .help_long   =
      "Lets the bot do again what you opted out of: weather, birthdays,\n"
      "followups, prices, or all. Opting back into one after 'optout\n"
      "all' leaves the rest off. With nothing named it lists what you\n"
      "have opted out of.",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_optin,
  .nl          = &optin_nl,
};

bool
chatbot_optout_register(void)
{
  if(cmd_register(&optout_decl) != SUCCESS)
    return(FAIL);

  if(cmd_register(&optin_decl) != SUCCESS)
  {
    cmd_unregister_path("optout");
    return(FAIL);
  }

  return(SUCCESS);
}
