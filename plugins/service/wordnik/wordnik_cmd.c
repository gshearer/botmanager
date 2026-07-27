// botmanager — MIT
// wordnik command surface: the plugin's two public commands. !wotd prints
// today's word of the day — or the word published on a given date — as a
// headline plus one line per sense. !dict answers a lookup: numbered
// senses, synonyms and antonyms, and a citation showing the word in use.
// Both take -v, which adds the dictionary each sense came from and spends
// lines on every citation rather than the first.
//
// WORDNIK_INTERNAL suppresses wordnik_api.h's dlsym shims: this TU is
// linked into the same .so as wordnik.c, so it calls the service entry
// points directly rather than resolving the plugin against itself.
#define WORDNIK_INTERNAL
#define WORDNIK_CMD_INTERNAL
#include "wordnik_cmd.h"

#include <stdio.h>
#include <string.h>

// Per-line body cap for %.*s slots. Leaves room for the widest leading
// label we emit plus the terminating NUL.
#define WORDNIK_CMD_LINE_BODY  ((int)(WORDNIK_CMD_REPLY_SZ - 48))

// Split of that budget for a citation line, which carries the passage and
// the work it came from side by side. Sums (plus the colour markers and
// the separator) to under the line.
#define WORDNIK_CMD_EX_TEXT    440
#define WORDNIK_CMD_EX_TITLE   120

// A definition line carries two optional labels ahead of the text — the
// part of speech and, under -v, the source dictionary — so it keeps a
// wider margin than a plain body line.
#define WORDNIK_CMD_SENSE_BODY ((int)(WORDNIK_CMD_REPLY_SZ - 112))

// ----------------------------------------------------------------------
// Formatting helpers
// ----------------------------------------------------------------------

// The headline: the word itself, with the publication date beside it so a
// dated lookup is self-evidently the day the reader asked for.
static void
wordnik_emit_headline(cmd_ctx_t *ctx, const wordnik_wotd_t *w)
{
  char line[WORDNIK_CMD_REPLY_SZ];

  if(w->date[0] != '\0')
    snprintf(line, sizeof(line),
        "Word of the day " CLR_GRAY "(%s)" CLR_RESET " " CLR_BOLD "%.*s"
        CLR_RESET, w->date, (int)sizeof(w->word), w->word);

  else
    snprintf(line, sizeof(line), "Word of the day " CLR_BOLD "%.*s"
        CLR_RESET, (int)sizeof(w->word), w->word);

  cmd_reply(ctx, line);
}

// The parenthesised part of speech, ready to prefix a definition — or
// nothing at all, since a sense Wordnik left unlabelled reads better bare
// than under a placeholder.
static const char *
wordnik_pos_of(const wordnik_def_t *d, char *buf, size_t cap)
{
  if(d->part_of_speech[0] == '\0')
    return("");

  snprintf(buf, cap, CLR_CYAN "(%s)" CLR_RESET " ", d->part_of_speech);
  return(buf);
}

// One sense: part of speech, the dictionary it came from under -v, then
// the definition text.
static void
wordnik_emit_def(cmd_ctx_t *ctx, const wordnik_def_t *d, bool verbose)
{
  char line[WORDNIK_CMD_REPLY_SZ];
  char pos[WORDNIK_POS_SZ + 12];             // room for the colour markers
  char attribution[WORDNIK_SOURCE_SZ + 12];

  attribution[0] = '\0';

  if(verbose && d->source[0] != '\0')
    snprintf(attribution, sizeof(attribution), CLR_GRAY "[%s] " CLR_RESET,
        d->source);

  snprintf(line, sizeof(line), "  %s%s%.*s",
      wordnik_pos_of(d, pos, sizeof(pos)), attribution,
      WORDNIK_CMD_SENSE_BODY, d->text);
  cmd_reply(ctx, line);
}

// A usage citation, attributed to the work it was quoted from.
static void
wordnik_emit_example(cmd_ctx_t *ctx, const wordnik_example_t *e)
{
  char line[WORDNIK_CMD_REPLY_SZ];

  snprintf(line, sizeof(line), "  " CLR_GRAY "\"%.*s\"" CLR_RESET "%s%.*s",
      WORDNIK_CMD_EX_TEXT, e->text,
      e->title[0] != '\0' ? " — " : "",
      WORDNIK_CMD_EX_TITLE, e->title);
  cmd_reply(ctx, line);
}

// ----------------------------------------------------------------------
// Async completion
// ----------------------------------------------------------------------

static void
wordnik_cmd_done(const wordnik_response_t *resp)
{
  wordnik_cmd_req_t *r   = (wordnik_cmd_req_t *)resp->user_data;
  cmd_ctx_t          ctx = r->ctx;
  const wordnik_wotd_t *w = resp->wotd;
  char               line[WORDNIK_CMD_REPLY_SZ];

  ctx.msg = &r->msg;

  if(resp->status != WORDNIK_OK || w == NULL)
  {
    if(r->date[0] != '\0')
      snprintf(line, sizeof(line), "wotd: %s (%s)",
          wordnik_status_str(resp->status), r->date);

    else
      snprintf(line, sizeof(line), "wotd: %s",
          wordnik_status_str(resp->status));

    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  wordnik_emit_headline(&ctx, w);

  for(int32_t i = 0; i < w->n_defs; i++)
    wordnik_emit_def(&ctx, &w->defs[i], r->verbose);

  if(w->note[0] != '\0')
  {
    snprintf(line, sizeof(line), "  " CLR_GRAY "note: %.*s" CLR_RESET,
        WORDNIK_CMD_LINE_BODY, w->note);
    cmd_reply(&ctx, line);
  }

  if(r->verbose)
    for(int32_t i = 0; i < w->n_examples; i++)
      wordnik_emit_example(&ctx, &w->examples[i]);

  mem_free(r);
}

// ----------------------------------------------------------------------
// Argument parsing
// ----------------------------------------------------------------------

// Every token is either -v or the date to look up; a second date, or
// anything else, is a usage error rather than something to guess at.
// Validation of the date's shape stays in the service half — the command
// only needs to know which token was meant to be one.
static void
wordnik_parse_args(const char *args, wordnik_cmd_args_t *a)
{
  char  scratch[METHOD_TEXT_SZ];
  char *save;

  memset(a, 0, sizeof(*a));

  if(args == NULL)
    return;

  snprintf(scratch, sizeof(scratch), "%s", args);

  for(char *tok = strtok_r(scratch, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    if(!a->verbose && strcmp(tok, "-v") == 0)
    {
      a->verbose = true;
      continue;
    }

    if(a->date[0] == '\0' && strnlen(tok, WORDNIK_DATE_SZ) == 10)
    {
      snprintf(a->date, sizeof(a->date), "%s", tok);
      continue;
    }

    a->bad_arg = true;
    return;
  }
}

// ----------------------------------------------------------------------
// Command body
// ----------------------------------------------------------------------

#define WORDNIK_CMD_USAGE "wotd [-v] [YYYY-MM-DD]"

static void
wordnik_cmd(const cmd_ctx_t *ctx)
{
  wordnik_cmd_args_t a;
  wordnik_cmd_req_t *r;

  wordnik_parse_args(ctx->args, &a);

  if(a.bad_arg)
  {
    cmd_reply(ctx, "Usage: " WORDNIK_CMD_USAGE);
    return;
  }

  if(!wordnik_configured())
  {
    cmd_reply(ctx, "wotd: no API key configured "
        "(operator: set plugin.wordnik.creds.apikey)");
    return;
  }

  r = mem_alloc(WORDNIK_CMD_CTX, "req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx     = *ctx;
  r->verbose = a.verbose;
  snprintf(r->date, sizeof(r->date), "%s", a.date);

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  if(wordnik_fetch_wotd(a.date, wordnik_cmd_done, r) != SUCCESS)
  {
    cmd_reply(ctx, a.date[0] != '\0'
        ? "wotd: failed to submit lookup (date must be YYYY-MM-DD)"
        : "wotd: failed to submit lookup");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// !dict — the dictionary half
// ----------------------------------------------------------------------

// Render a related-words bucket as one "label: a, b, c" line, stopping
// before the line budget rather than letting the tail be cut mid-word.
static void
wordnik_emit_related(cmd_ctx_t *ctx, const char *label,
    const char (*words)[WORDNIK_WORD_SZ], int32_t n)
{
  char   line[WORDNIK_CMD_REPLY_SZ];
  size_t len;

  if(n <= 0)
    return;

  len = (size_t)snprintf(line, sizeof(line), "  " CLR_GRAY "%s:" CLR_RESET,
      label);

  for(int32_t i = 0; i < n; i++)
  {
    size_t room = sizeof(line) - len;
    int    wrote;

    if(room < strnlen(words[i], WORDNIK_WORD_SZ) + 4)
      break;

    wrote = snprintf(line + len, room, "%s %s", i > 0 ? "," : "", words[i]);

    if(wrote < 0)
      break;

    len += (size_t)wrote;
  }

  cmd_reply(ctx, line);
}

// One numbered sense. Same anatomy as the word-of-the-day sense line, but
// numbered, since a dictionary entry is a list the reader may refer back
// to by index.
static void
wordnik_emit_sense(cmd_ctx_t *ctx, const wordnik_def_t *d, int32_t n,
    bool verbose)
{
  char line[WORDNIK_CMD_REPLY_SZ];
  char pos[WORDNIK_POS_SZ + 12];             // room for the colour markers
  char attribution[WORDNIK_SOURCE_SZ + 12];

  attribution[0] = '\0';

  if(verbose && d->source[0] != '\0')
    snprintf(attribution, sizeof(attribution), CLR_GRAY "[%s] " CLR_RESET,
        d->source);

  snprintf(line, sizeof(line), "  %d. %s%s%.*s", n,
      wordnik_pos_of(d, pos, sizeof(pos)), attribution,
      WORDNIK_CMD_SENSE_BODY, d->text);
  cmd_reply(ctx, line);
}

static void
wordnik_dict_done(const wordnik_word_response_t *resp)
{
  wordnik_dict_req_t   *r   = (wordnik_dict_req_t *)resp->user_data;
  cmd_ctx_t             ctx = r->ctx;
  const wordnik_word_t *w   = resp->word;
  char                  line[WORDNIK_CMD_REPLY_SZ];
  int32_t               n_examples;

  ctx.msg = &r->msg;

  if(resp->status == WORDNIK_NOT_FOUND || (resp->status == WORDNIK_OK
      && w != NULL && w->n_defs == 0))
  {
    snprintf(line, sizeof(line), "dict: no entry for \"%s\"", r->word);
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  if(resp->status != WORDNIK_OK || w == NULL)
  {
    snprintf(line, sizeof(line), "dict: %s",
        wordnik_status_str(resp->status));
    cmd_reply(&ctx, line);
    mem_free(r);
    return;
  }

  snprintf(line, sizeof(line), CLR_BOLD "%.*s" CLR_RESET,
      (int)sizeof(w->word), w->word);
  cmd_reply(&ctx, line);

  for(int32_t i = 0; i < w->n_defs; i++)
    wordnik_emit_sense(&ctx, &w->defs[i], i + 1, r->verbose);

  wordnik_emit_related(&ctx, "synonyms", w->synonyms, w->n_synonyms);
  wordnik_emit_related(&ctx, "antonyms", w->antonyms, w->n_antonyms);

  // One citation is enough to show the word in use; -v spends the lines.
  n_examples = r->verbose || w->n_examples < 1 ? w->n_examples : 1;

  for(int32_t i = 0; i < n_examples; i++)
    wordnik_emit_example(&ctx, &w->examples[i]);

  mem_free(r);
}

#define WORDNIK_DICT_USAGE "dict [-v] <word>"

// Everything that is not -v is the headword, joined back together with
// single spaces so a multi-word entry ("ad hoc") survives tokenization.
static void
wordnik_dict_parse_args(const char *args, bool *verbose, char *word,
    size_t cap)
{
  char   scratch[METHOD_TEXT_SZ];
  char  *save;
  size_t len = 0;

  *verbose = false;
  word[0]  = '\0';

  if(args == NULL)
    return;

  snprintf(scratch, sizeof(scratch), "%s", args);

  for(char *tok = strtok_r(scratch, " \t", &save); tok != NULL;
      tok = strtok_r(NULL, " \t", &save))
  {
    int wrote;

    if(!*verbose && strcmp(tok, "-v") == 0)
    {
      *verbose = true;
      continue;
    }

    if(len + 1 >= cap)
      break;

    wrote = snprintf(word + len, cap - len, "%s%s", len > 0 ? " " : "", tok);

    if(wrote < 0)
      break;

    len += (size_t)wrote < cap - len ? (size_t)wrote : cap - len - 1;
  }
}

static void
wordnik_dict_cmd(const cmd_ctx_t *ctx)
{
  wordnik_dict_req_t *r;
  char                word[WORDNIK_WORD_SZ];
  bool                verbose;

  wordnik_dict_parse_args(ctx->args, &verbose, word, sizeof(word));

  if(word[0] == '\0')
  {
    cmd_reply(ctx, "Usage: " WORDNIK_DICT_USAGE);
    return;
  }

  if(!wordnik_configured())
  {
    cmd_reply(ctx, "dict: no API key configured "
        "(operator: set plugin.wordnik.creds.apikey)");
    return;
  }

  r = mem_alloc(WORDNIK_CMD_CTX, "dict_req", sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->ctx     = *ctx;
  r->verbose = verbose;
  snprintf(r->word, sizeof(r->word), "%s", word);

  if(ctx->msg != NULL)
    r->msg = *ctx->msg;

  r->ctx.msg      = &r->msg;
  r->ctx.args     = NULL;
  r->ctx.username = NULL;
  r->ctx.parsed   = NULL;
  r->ctx.data     = NULL;

  if(wordnik_fetch_word(word, WORDNIK_PART_ALL, wordnik_dict_done, r)
      != SUCCESS)
  {
    cmd_reply(ctx, "dict: failed to submit lookup");
    mem_free(r);
  }
}

// ----------------------------------------------------------------------
// Natural-language hints
// ----------------------------------------------------------------------

static const cmd_nl_example_t wordnik_nl_examples[] = {
  { .utterance  = "what's the word of the day",
    .invocation = "/wotd" },
  { .utterance  = "teach me a new word, with examples",
    .invocation = "/wotd -v" },
  { .utterance  = "what was the word of the day on July 4th 2026",
    .invocation = "/wotd 2026-07-04" },
};

static const cmd_nl_t wordnik_nl = {
  .when          = "User asks for the word of the day, a new vocabulary "
                   "word, or what word was featured on a given date.",
  .syntax        = "/wotd | /wotd -v | /wotd <YYYY-MM-DD>",
  .slots         = NULL,
  .slot_count    = 0,
  .examples      = wordnik_nl_examples,
  .example_count = (uint8_t)(sizeof(wordnik_nl_examples)
                             / sizeof(wordnik_nl_examples[0])),
};

static const cmd_nl_slot_t wordnik_dict_nl_slots[] = {
  { .name = "word", .type = CMD_NL_ARG_TOPIC,
    .flags = CMD_NL_SLOT_REMAINDER },
};

static const cmd_nl_example_t wordnik_dict_nl_examples[] = {
  { .utterance  = "what does perspicacious mean",
    .invocation = "/dict perspicacious" },
  { .utterance  = "define brusque with synonyms and a sentence",
    .invocation = "/dict -v brusque" },
  { .utterance  = "look up the word gainsay in the dictionary",
    .invocation = "/dict gainsay" },
};

static const cmd_nl_t wordnik_dict_nl = {
  .when          = "User asks what a word means, for a definition, for "
                   "synonyms or antonyms, or how a word is used in a "
                   "sentence.",
  .syntax        = "/dict <word> | /dict -v <word>",
  .slots         = wordnik_dict_nl_slots,
  .slot_count    = (uint8_t)(sizeof(wordnik_dict_nl_slots)
                             / sizeof(wordnik_dict_nl_slots[0])),
  .examples      = wordnik_dict_nl_examples,
  .example_count = (uint8_t)(sizeof(wordnik_dict_nl_examples)
                             / sizeof(wordnik_dict_nl_examples[0])),
};

// ----------------------------------------------------------------------
// Registration — driven by the service half's plugin lifecycle
// ----------------------------------------------------------------------

static const char wordnik_help[] =
    "Wordnik's word of the day: the word, its senses, and the\n"
    "editors' note on where it comes from.\n"
    "\n"
    "  !wotd                today's word\n"
    "  !wotd <YYYY-MM-DD>   the word published on that date\n"
    "\n"
    "Flags:\n"
    "  -v           also show the dictionary each sense came from\n"
    "               and the usage citations Wordnik ships with it\n"
    "\n"
    "How many senses and citations are carried is set by\n"
    "plugin.wordnik.max_definitions and plugin.wordnik.max_examples.\n"
    "\n"
    "Examples:\n"
    "  !wotd\n"
    "  !wotd -v\n"
    "  !wotd 2026-01-01";

static const char wordnik_dict_help[] =
    "Look a word up in the dictionary: what it means, what it is\n"
    "like and unlike, and how it reads in a sentence.\n"
    "\n"
    "  !dict <word>         senses, synonyms, antonyms, one citation\n"
    "\n"
    "Flags:\n"
    "  -v           tag each sense with the dictionary it came from\n"
    "               and print every citation, not just the first\n"
    "\n"
    "A headword may be a phrase — !dict ad hoc reads as one entry.\n"
    "How much is carried is set by plugin.wordnik.max_definitions,\n"
    "max_related, and max_examples.\n"
    "\n"
    "See also !wotd, the word of the day.\n"
    "\n"
    "Examples:\n"
    "  !dict perspicacious\n"
    "  !dict -v brusque\n"
    "  !dict ad hoc";

// The registry records WORDNIK_CTX as the providing module: the wordnik
// plugin itself owns these commands, and `/show commands` should say so.
// The two are raised together — !dict without !wotd (or the reverse) is a
// half-loaded plugin, so the first failure unwinds the other.
bool
wordnik_cmd_register(void)
{
  if(cmd_register(WORDNIK_CTX, "wotd", WORDNIK_CMD_USAGE,
      "Word of the day via Wordnik (wordnik.com)",
      wordnik_help,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      wordnik_cmd, NULL, NULL, NULL,
      NULL, 0, NULL, &wordnik_nl) != SUCCESS)
    return(FAIL);

  if(cmd_register(WORDNIK_CTX, "dict", WORDNIK_DICT_USAGE,
      "Dictionary lookup via Wordnik (wordnik.com)",
      wordnik_dict_help,
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
      wordnik_dict_cmd, NULL, NULL, NULL,
      NULL, 0, NULL, &wordnik_dict_nl) != SUCCESS)
  {
    cmd_unregister("wotd");
    return(FAIL);
  }

  clam(CLAM_INFO, WORDNIK_CMD_CTX, "!wotd and !dict registered");
  return(SUCCESS);
}

void
wordnik_cmd_unregister(void)
{
  cmd_unregister("dict");
  cmd_unregister("wotd");
  clam(CLAM_INFO, WORDNIK_CMD_CTX, "!wotd and !dict unregistered");
}
