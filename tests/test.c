// botmanager — MIT
// Table-driven test harness: counts checks, prints one line per failure.
#define TEST_INTERNAL
#include "test.h"

#include <stdio.h>
#include <string.h>

static unsigned test_checks;
static unsigned test_failures;

// Render s into out with control and high bytes as `\xNN`, so a
// difference between two marker strings is visible in the failure
// line. A NULL renders as (nil), and an over-long string stops at the
// last escape that fits — a failure line is a hint, not evidence.
static const char *
test_escape(const char *s, char *out, size_t out_cap)
{
  size_t o = 0;

  if(s == NULL)
  {
    snprintf(out, out_cap, "(nil)");
    return(out);
  }

  for(size_t i = 0; s[i] != '\0'; i++)
  {
    unsigned char c = (unsigned char)s[i];

    if(c >= 0x20 && c < 0x7f)
    {
      if(o + 1 >= out_cap) break;

      out[o++] = (char)c;
    }

    else
    {
      if(o + 4 >= out_cap) break;

      o += (size_t)snprintf(out + o, out_cap - o, "\\x%02x", c);
    }
  }

  out[o] = '\0';
  return(out);
}

void
test_check_str(const char *table, const char *name,
    const char *want, const char *got)
{
  char ewant[512];
  char egot[512];

  test_checks++;

  if(want != NULL && got != NULL && strcmp(want, got) == 0)
    return;

  test_failures++;
  fprintf(stderr, "FAIL %s/%s: want \"%s\" got \"%s\"\n", table, name,
      test_escape(want, ewant, sizeof(ewant)),
      test_escape(got, egot, sizeof(egot)));
}

void
test_check_bool(const char *table, const char *name, bool want, bool got)
{
  test_checks++;

  if(want == got)
    return;

  test_failures++;
  fprintf(stderr, "FAIL %s/%s: want %s got %s\n", table, name,
      want ? "true" : "false", got ? "true" : "false");
}

void
test_check_sz(const char *table, const char *name, size_t want, size_t got)
{
  test_checks++;

  if(want == got)
    return;

  test_failures++;
  fprintf(stderr, "FAIL %s/%s: want %zu got %zu\n", table, name, want, got);
}

int
test_skip(const char *suite, const char *why)
{
  printf("%s: SKIP — %s\n", suite, why);
  return(77);
}

int
test_report(const char *suite)
{
  printf("%s: %u checks, %u failed\n", suite, test_checks, test_failures);
  return(test_failures == 0 ? 0 : 1);
}
