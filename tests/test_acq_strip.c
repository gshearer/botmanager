// botmanager — MIT
// Cases for the acquisition engine's HTML-to-text stripper.
//
// This suite exists because the stripper spent months silently
// destroying every page it was given and nothing said so. It elided
// tags but not the CONTENT of raw-text elements, so a modern news page
// stripped to 97-99% inline JavaScript — and since the digester reads
// only the first `acquire.digest_body_truncate_chars` (6000) bytes, the
// article never reached it at all. The corpus filled with digests that
// described the page rather than the story, which reads as a weak model
// rather than a broken parser. The comment above the function asserted
// that "the digester tolerates the noise fine"; it did not.
//
// The rows below are the paths the raw-text scan can take, not a corpus
// of pages. Two of them are the ones that would lose data if the scan
// were written the obvious way: an element whose name merely STARTS
// with a raw-text name, and an UNTERMINATED one — where skipping to a
// close tag that never comes would swallow the rest of the document.
//
// `acq_strip_html` is pure, which is why acquire_strip.c is its own file
// and compiles straight into this binary: no allocator, no lock, no
// config, nothing to stand up before a row can run.

#include "test.h"

#include "acquire_priv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SUITE "acq_strip"

typedef struct
{
  const char *name;
  const char *in;
  const char *want;
} strip_case_t;

static const strip_case_t strip_cases[] = {
  // The defect this suite was written for.
  { "script content goes with its tag",
    "<p>Real text</p><script>var x = 1; document.cookie;</script><p>More</p>",
    "Real text More" },
  { "style content goes with its tag",
    "<style>.a{color:red}</style><p>Body</p>",
    "Body" },
  { "noscript content goes with its tag",
    "<noscript>Enable JavaScript</noscript><p>Body</p>",
    "Body" },
  { "the open tag may carry attributes",
    "<script type=\"text/javascript\" async>junk()</script>Body",
    "Body" },
  { "tag names fold case",
    "<SCRIPT>junk()</SCRIPT>Body",
    "Body" },
  { "a self-closing form still needs its end tag",
    "<script/>Body",
    "Body" },

  // Data loss if the scan is written without these two.
  { "a longer name that merely starts the same is not raw text",
    "<scriptable>kept</scriptable>",
    "kept" },
  { "an unterminated raw-text element costs its tag, not the document",
    "<script>Body survives",
    "Body survives" },
  { "an end tag with no start is an ordinary tag",
    "</script>Body",
    "Body" },

  // A tag is a word boundary unless it is inline. Gluing `</p><p>`
  // produces a token that is in no index and no vocabulary.
  { "a block tag breaks the words around it",
    "<p>Real text</p><p>More</p>",
    "Real text More" },
  { "an inline tag does not break a word",
    "un<b>believable</b>",
    "unbelievable" },
  { "an inline end tag does not break either",
    "<i>a</i><i>b</i>",
    "ab" },
  { "an unknown tag breaks — the list is the exception",
    "a<madeup>b",
    "a b" },
  { "one break, not two, across an empty element",
    "a<br><br>b",
    "a b" },
  { "a leading break emits nothing to trim",
    "<div>a",
    "a" },

  // Behaviour that predates the change and must survive it.
  { "ordinary tags elide, content stays",
    "<div class=\"x\"><b>Bold</b> plain</div>",
    "Bold plain" },
  { "hex numeric entities decode",
    "Dua Lipa&#x27;s chips",
    "Dua Lipa's chips" },
  { "hex is case-insensitive",
    "a&#X41;b",
    "aAb" },
  { "entities decode",
    "a &amp; b &lt;c&gt; &quot;d&quot; &#39;e&#39;",
    "a & b <c> \"d\" 'e'" },
  { "a short unknown entity drops whole",
    "x &foo; y",
    "x y" },
  { "an entity longer than the 8-char scan window stays literal",
    "x &notanentity; y",
    "x &notanentity; y" },
  { "whitespace runs collapse and the ends are trimmed",
    "  \n\t a   b \r\n ",
    "a b" },
  { "nbsp is whitespace, not a character",
    "a&nbsp;&nbsp;b",
    "a b" },
  { "empty input is empty output",
    "",
    "" },
  { "a tag that never closes elides to end of input",
    "text<div",
    "text" },
};

// The bound is the contract `acq_strip_html` states: the output is
// NUL-terminated and never exceeds out_cap - 1 bytes. A raw-text scan
// that walks the input independently of the write cursor is exactly the
// shape that can violate it, so it is asserted separately.
static void
strip_bound_cases(void)
{
  static const char *const inputs[] = {
    "<script>0123456789</script>abcdefghijklmnop",
    "abcdefghijklmnopqrstuvwxyz",
    "<style>xxxxxxxxxxxxxxxxxxxx</style>",
  };

  for(size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++)
  {
    for(size_t cap = 1; cap <= 12; cap++)
    {
      char   out[16];
      size_t n;
      char   name[96];

      memset(out, 'Z', sizeof(out));
      n = acq_strip_html(inputs[i], strlen(inputs[i]), out, cap);

      snprintf(name, sizeof(name), "input %zu cap %zu: within bound", i, cap);
      test_check_bool("bound", name, true, n <= cap - 1);

      snprintf(name, sizeof(name), "input %zu cap %zu: NUL at n", i, cap);
      test_check_bool("bound", name, true, out[n] == '\0');

      snprintf(name, sizeof(name), "input %zu cap %zu: nothing past cap",
          i, cap);
      test_check_bool("bound", name, true, out[cap] == 'Z');
    }
  }

  // out_cap 0 writes nothing at all, so there is no NUL to check.
  {
    char out[4] = { 'Z', 'Z', 'Z', 'Z' };

    test_check_sz("bound", "cap 0 returns 0", 0,
        acq_strip_html("abc", 3, out, 0));
    test_check_bool("bound", "cap 0 writes nothing", true, out[0] == 'Z');
  }
}

// A deterministic sweep of markup-shaped noise. Not a fuzz target — the
// tree has no fuzzing configuration and standing one up costs more than
// this change — but the raw-text scan and the entity window both walk
// the input independently of the write cursor, which is the shape that
// reads off the end. The alphabet is biased toward the bytes those two
// scanners branch on so the rows exercise them rather than plain text;
// the real detector here is ASan, and the assertions are the contract.
//
// ⛔ The missing target is named in the ship note, deliberately: a
// libFuzzer build over `acq_strip_html` is the right tool if this
// function ever grows a state machine.
static void
strip_sweep_cases(void)
{
  static const char alphabet[] =
      "<>/&;#xX \t\n\rabcimnoprstuy0189=\"'!-";

  uint64_t     seed  = 0x5DEECE66DULL;
  const size_t rows  = 4000;
  size_t       worst = 0;

  for(size_t r = 0; r < rows; r++)
  {
    char   in[192];
    char   out[128];
    size_t in_len;
    size_t cap;
    size_t n;

    // xorshift64 — deterministic, self-contained, and nothing here
    // needs a real generator.
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    in_len = (size_t)(seed % sizeof(in));
    cap    = (size_t)(seed % (sizeof(out) - 1)) + 1;   // out[cap] must be readable

    for(size_t i = 0; i < in_len; i++)
    {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      in[i] = alphabet[seed % (sizeof(alphabet) - 1)];
    }

    memset(out, 'Z', sizeof(out));
    n = acq_strip_html(in, in_len, out, cap);

    if(n > worst)
      worst = n;

    if(n > cap - 1 || out[n] != '\0' || out[cap] != 'Z')
    {
      char name[64];

      snprintf(name, sizeof(name), "row %zu (len %zu cap %zu)",
          r, in_len, cap);
      test_check_bool("sweep", name, true, false);
      return;
    }
  }

  // One assertion for the whole sweep: a table of N inputs is one row.
  test_check_bool("sweep", "4000 markup-shaped inputs stay in bounds",
      true, true);
  test_check_bool("sweep", "the sweep actually produced output",
      true, worst > 0);
}

int
main(void)
{
  for(size_t i = 0; i < sizeof(strip_cases) / sizeof(strip_cases[0]); i++)
  {
    const strip_case_t *c = &strip_cases[i];
    char                out[512];

    acq_strip_html(c->in, strlen(c->in), out, sizeof(out));
    test_check_str("strip", c->name, c->want, out);
  }

  strip_bound_cases();
  strip_sweep_cases();

  return(test_report(SUITE));
}
