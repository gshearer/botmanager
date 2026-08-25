// botmanager — MIT
// Cases for the base prompt's block parser, whose failures are the quiet
// kind: a body that swallows the block below it, or a $TOKEN half
// substituted, reaches the model as prose and nothing anywhere says so.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "test.h"

#include <stdio.h>
#include <string.h>

// baseprompt.c reaches for this only from chatbot_base_load(), which no
// case here calls — the split between load and parse is what lets this
// suite run without a KV registry or a filesystem.
bool
chatbot_personality_path(char *out_path, size_t sz)
{
  (void)out_path;
  (void)sz;
  return(false);
}

// chatbot_base_parse takes ownership of a mem_alloc'd buffer, so each
// case hands it its own copy.
static chatbot_base_t *
parse(const char *doc)
{
  return(chatbot_base_parse(mem_strdup("test", "doc", doc)));
}

// One document exercised from every angle a body can be cut wrong: the
// first block, a middle one, the last (which ends at EOF rather than at
// a header), a header wearing whitespace, a comment inside a body, and a
// line that only looks like a header. A comment inside a body closes
// its own gap — a body is prompt text, so a blanked line would be a
// comment the model can still read.
static const char doc_shapes[] =
    "# a leading comment, owned by nobody\n"
    "\n"
    "[[first]]\n"
    "one\n"
    "\n"
    "  [[ spaced ]]  \n"
    "two\n"
    "# a comment inside a body\n"
    "still two\n"
    "\n"
    "[[bracketed]]\n"
    "[not a header]\n"
    "[[also not a header\n"
    "\n"
    "[[last]]\n"
    "three";

static const struct
{
  const char *name;
  const char *block;
  const char *want;
} shape_cases[] = {
  { "first block ends at the next header",  "first",     "one"              },
  { "header tolerates surrounding space",   "spaced",    "two\nstill two" },
  { "a bracketed line is not a header",     "bracketed", "[not a header]\n"
                                                         "[[also not a header" },
  { "last block ends at EOF",               "last",      "three"            },
  { "an absent block is absent",            "missing",   NULL               },
};

// Rendering. The document is deliberately minimal — what is under test
// is the substitution, not the parse.
static const char doc_tokens[] =
    "[[t]]\n"
    "$NICK told $NICK's friend it cost $5 and $UNKNOWN, $NICK_OK.";

static const struct
{
  const char *name;
  const char *value;
  size_t      dst_sz;
  const char *want;
} render_cases[] = {
  { "every occurrence, prose intact", "bo", 128,
    "bo told bo's friend it cost $5 and $UNKNOWN, $NICK_OK." },
  { "an empty value erases the token", "",  128,
    " told 's friend it cost $5 and $UNKNOWN, $NICK_OK." },
  { "a short buffer truncates, never overruns", "bo", 12,
    "bo told bo'" },
};

// Picking. A one-line block is the common case and must be stable; a
// many-line block must eventually offer every line, which is the whole
// reason the fallback stopped being a #define.
static const char doc_pick_one[]  = "[[p]]\ncouldn't tell you.";
static const char doc_pick_many[] = "[[p]]\nalpha\nbravo\ncharlie";

// Validation. Each row is the whole document, so a case cannot depend on
// the row above it.
static const struct
{
  const char *name;
  const char *doc;
  bool        want_ok;
  const char *want_err;
} validate_cases[] = {
  { "an empty document fails on the first required block", "",
    false, "no [[nick]] block" },
  { "a declared but empty block does not count",
    "[[nick]]\n",
    false, "no [[nick]] block" },
  { "a block that lost its token is named with the token",
    "[[nick]]\nyou are somebody\n",
    false, "[[nick]] has lost $NICK, which the runtime substitutes" },
};

int
main(void)
{
  mem_init();

  // --- bodies -------------------------------------------------------
  {
    chatbot_base_t *b = parse(doc_shapes);

    for(size_t i = 0; i < sizeof(shape_cases) / sizeof(shape_cases[0]); i++)
    {
      const char *got = chatbot_base_body(b, shape_cases[i].block);

      test_check_str("shapes", shape_cases[i].name,
          shape_cases[i].want == NULL ? "(absent)" : shape_cases[i].want,
          got == NULL ? "(absent)" : got);
    }

    chatbot_base_free(b);
  }

  // --- rendering ----------------------------------------------------
  {
    chatbot_base_t *b = parse(doc_tokens);

    for(size_t i = 0; i < sizeof(render_cases) / sizeof(render_cases[0]); i++)
    {
      const chatbot_base_tok_t toks[] = {
        { "$NICK", render_cases[i].value },
        { NULL,    NULL                  },
      };
      char out[128];

      chatbot_base_render(b, "t", toks, out, render_cases[i].dst_sz);
      test_check_str("render", render_cases[i].name,
          render_cases[i].want, out);
    }

    // A block nobody declared renders nothing and says so with a zero,
    // which is how every call site decides to keep its compiled default.
    {
      char out[32];

      test_check_sz("render", "an absent block writes nothing",
          0, chatbot_base_render(b, "nope", NULL, out, sizeof(out)));
      test_check_str("render", "an absent block leaves dst empty",
          "", out);
    }

    chatbot_base_free(b);
  }

  // --- picking ------------------------------------------------------
  {
    chatbot_base_t *b = parse(doc_pick_one);
    char            out[64];

    for(int i = 0; i < 3; i++)
    {
      chatbot_base_pick(b, "p", out, sizeof(out));
      test_check_str("pick", "a single line is always that line",
          "couldn't tell you.", out);
    }

    chatbot_base_free(b);
  }

  {
    chatbot_base_t *b    = parse(doc_pick_many);
    bool            seen[3] = {false, false, false};
    char            out[64];

    // Three lines, six draws: the rotor is modular, so every line has
    // to come up whatever offset the counter starts at.
    for(int i = 0; i < 6; i++)
    {
      chatbot_base_pick(b, "p", out, sizeof(out));

      if(strcmp(out, "alpha")   == 0) seen[0] = true;
      if(strcmp(out, "bravo")   == 0) seen[1] = true;
      if(strcmp(out, "charlie") == 0) seen[2] = true;
    }

    test_check_bool("pick", "every line is reachable",
        true, seen[0] && seen[1] && seen[2]);

    chatbot_base_free(b);
  }

  // --- validation ---------------------------------------------------
  for(size_t i = 0; i < sizeof(validate_cases) / sizeof(validate_cases[0]); i++)
  {
    chatbot_base_t *b = parse(validate_cases[i].doc);
    char            err[160];

    test_check_bool("validate", validate_cases[i].name,
        validate_cases[i].want_ok,
        chatbot_base_validate(b, err, sizeof(err)));
    test_check_str("validate", validate_cases[i].name,
        validate_cases[i].want_err, err);

    chatbot_base_free(b);
  }

  // The shipped document is the one every bot on the daemon actually
  // runs on, so it is a case: a base.txt that cannot validate takes
  // every prompt back to its compiled default and no other suite here
  // would notice.
  {
    FILE *fp = fopen(BASE_TXT_PATH, "rb");

    test_check_bool("shipped", BASE_TXT_PATH " is readable",
        true, fp != NULL);

    if(fp != NULL)
    {
      char           *raw = mem_alloc("test", "base.txt",
          CHATBOT_PERSONALITY_BODY_SZ);
      size_t          n   = fread(raw, 1, CHATBOT_PERSONALITY_BODY_SZ - 1, fp);
      chatbot_base_t *b;
      char            err[160];

      raw[n] = '\0';
      fclose(fp);

      b = chatbot_base_parse(raw);

      test_check_bool("shipped", "base.txt validates",
          true, chatbot_base_validate(b, err, sizeof(err)));
      test_check_str("shipped", "base.txt validates with no complaint",
          "", err);

      chatbot_base_free(b);
    }
  }

  return(test_report("baseprompt"));
}
