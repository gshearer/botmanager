// botmanager — MIT
// shorturl: table-driven cases for the two input boundaries that decide what
// a short link is — token_parse and url_valid. Both are compiled into the
// off-host daemon AND into the shorturl plugin, so a regression here is a
// regression in two processes at once.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "src/token.h"
#include "src/url.h"

typedef struct
{
  const char *name;
  const char *input;
  bool        expected;
} boundary_case_t;

static int failures;

// One case per distinct code path, not per input. Every row below reaches a
// different branch of the function under test.
static const boundary_case_t token_cases[] =
{
  { "exact length, all digits",      "01234567",           true  },
  { "exact length, mixed case",      "kR7mQ2xB",           true  },
  { "both ends of the alphabet",     "0zAZ9aA0",           true  },
  { "NULL pointer",                  NULL,                 false },
  { "empty string",                  "",                   false },
  { "one character short",           "0123456",            false },
  { "one character long",            "012345678",          false },
  { "invalid at first position",     "-1234567",           false },
  { "invalid at middle position",    "0123-567",           false },
  { "invalid at last position",      "0123456-",           false },
  { "space",                         "0123 567",           false },
  { "newline",                       "0123\n567",          false },
  // Guards against a signed char indexing or comparing below zero.
  { "byte with the high bit set",    "0123\xff" "567",     false },
  // Guards against comparing eight bytes without first measuring the string.
  { "NUL before the eighth byte",    "0123\0" "567",       false },
};

static const boundary_case_t url_cases[] =
{
  { "ordinary https URL",            "https://example.org/a/b?c=d#e", true  },
  { "single character",              "x",                  true  },
  { "NULL pointer",                  NULL,                 false },
  { "empty string",                  "",                   false },
  // The load-bearing cases: either one would forge an HTTP response header.
  { "carriage return",               "https://a/\r\nX: y", false },
  { "bare newline",                  "https://a/\nX: y",   false },
  { "tab",                           "https://a/\tb",      false },
  { "DEL",                           "https://a/\x7f" "b", false },
};

static void
check(bool condition, const char *what)
{
  if(condition)
    return;

  fprintf(stderr, "FAIL: %s\n", what);
  failures++;
}

static void
run_token_cases(void)
{
  size_t i;

  for(i = 0; i < sizeof token_cases / sizeof token_cases[0]; i++)
  {
    const boundary_case_t *c = &token_cases[i];
    token_t token;

    memset(&token, 0xAA, sizeof token);

    check(token_parse(c->input, &token) == c->expected, c->name);

    // A token that parsed must be exactly the input, terminated in place.
    if(c->expected)
      check(strcmp(token.s, c->input) == 0, "parsed token round-trips");
  }
}

static void
run_url_cases(void)
{
  size_t i;

  for(i = 0; i < sizeof url_cases / sizeof url_cases[0]; i++)
    check(url_valid(url_cases[i].input) == url_cases[i].expected, url_cases[i].name);
}

// The length boundary needs generated input rather than a literal.
static void
run_url_length_cases(void)
{
  static char url[SHORTURL_TARGET_MAX + 2];

  memset(url, 'u', sizeof url - 1);

  url[SHORTURL_TARGET_MAX] = '\0';
  check(url_valid(url), "URL of exactly SHORTURL_TARGET_MAX bytes");

  url[SHORTURL_TARGET_MAX] = 'u';
  url[SHORTURL_TARGET_MAX + 1] = '\0';
  check(!url_valid(url), "URL one byte over SHORTURL_TARGET_MAX");
}

// Generation and parsing are each other's inverse; anything token_generate
// produces must survive the boundary it will later be presented at.
static void
run_generate_cases(void)
{
  token_t generated;
  token_t reparsed;
  token_t previous;
  bool all_valid = true;
  bool any_differ = false;
  int i;

  memset(&previous, 0, sizeof previous);

  for(i = 0; i < 4096; i++)
  {
    if(!token_generate(&generated) || !token_parse(generated.s, &reparsed))
    {
      all_valid = false;
      break;
    }

    if(i > 0 && strcmp(generated.s, previous.s) != 0)
      any_differ = true;

    previous = generated;
  }

  check(all_valid, "every generated token parses");
  check(any_differ, "generated tokens are not constant");
}

int
main(void)
{
  run_token_cases();
  run_url_cases();
  run_url_length_cases();
  run_generate_cases();

  if(failures > 0)
  {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return(1);
  }

  puts("all checks passed");

  return(0);
}
