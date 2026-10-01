// botmanager — MIT
// Cases for acquisition's page-skip rules: host, verdict, shipped regex.
//
// All three fail silently. A host cut from the wrong part of a URL skips
// a publisher nobody named, or learns a skip against the wrong host; a
// verdict that counts a 404 condemns a healthy site one bad link at a
// time; a default pattern that matches `notreuters.com` drops results
// that were never paywalled. None of it errors — the corpus just gets
// thinner. The learning itself (distinct URLs, reset, expiry) is one SQL
// statement in acquire_skip.c and is verified against the database, not
// here.

#include "test.h"

#include "acquire_priv.h"

#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SUITE "acq_skip"

typedef struct
{
  const char *name;
  const char *url;
  bool        ok;
  const char *want;
} host_case_t;

static const host_case_t host_cases[] = {
  { "a plain https URL names its host",
    "https://www.reuters.com/world/x", true, "www.reuters.com" },
  { "http is accepted too",
    "http://example.com/", true, "example.com" },
  { "the scheme and host fold case",
    "HTTPS://WWW.Example.COM/Path", true, "www.example.com" },
  { "a host with no path ends at the string",
    "https://example.com", true, "example.com" },
  { "a port is not part of the host",
    "https://example.com:8443/a", true, "example.com" },
  { "userinfo is not part of the host",
    "https://user:pw@example.com/a", true, "example.com" },
  { "an @ in the path cannot supply the host",
    "https://good.com/x@evil.com", true, "good.com" },
  { "an @ in the query cannot supply the host",
    "https://good.com?u=a@evil.com", true, "good.com" },
  { "an @ in the fragment cannot supply the host",
    "https://good.com#a@evil.com", true, "good.com" },
  { "a bracketed IPv6 literal keeps its brackets, loses its port",
    "https://[2001:db8::1]:8080/", true, "[2001:db8::1]" },
  { "one trailing dot is the same host",
    "https://example.com./a", true, "example.com" },
  { "an unterminated IPv6 literal names nothing",
    "https://[2001:db8::1/", false, "" },
  { "any other scheme names nothing",
    "ftp://example.com/", false, "" },
  { "a relative URL names nothing",
    "/world/x", false, "" },
  { "an empty authority names nothing",
    "https:///path", false, "" },
  { "userinfo with no host names nothing",
    "https://user@/path", false, "" },
  { "an underscore is a host byte",
    "https://foo_bar.example.com/", true, "foo_bar.example.com" },
  { "a control byte in the host refuses the URL",
    "https://exa\x03mple.com/", false, "" },
  { "a space in the host refuses the URL",
    "https://exa mple.com/", false, "" },
  { "a bracket outside an IPv6 literal refuses the URL",
    "https://exa]mple.com/", false, "" },
};

typedef struct
{
  const char          *name;
  long                 curl_code;
  long                 status;
  size_t               strip_len;
  uint32_t             min_usable;
  acq_fetch_verdict_t  want;
} verdict_case_t;

static const verdict_case_t verdict_cases[] = {
  { "a transport error says nothing about the host",
    7, 0, 0, 200, ACQ_FETCH_TRANSIENT },
  { "401 is the host refusing us",
    0, 401, 0, 200, ACQ_FETCH_UNUSABLE },
  { "403 is the host refusing us",
    0, 403, 0, 200, ACQ_FETCH_UNUSABLE },
  { "451 is the host refusing us",
    0, 451, 0, 200, ACQ_FETCH_UNUSABLE },
  { "404 is one missing page, not the host",
    0, 404, 0, 200, ACQ_FETCH_TRANSIENT },
  { "429 is rate limiting, and it clears",
    0, 429, 0, 200, ACQ_FETCH_TRANSIENT },
  { "a 200 that strips to nothing is a refusal",
    0, 200, 0, 200, ACQ_FETCH_UNUSABLE },
  { "a 200 under the floor is a refusal (the msn.com shape)",
    0, 200, 3, 200, ACQ_FETCH_UNUSABLE },
  { "a 200 at the floor is a page",
    0, 200, 200, 200, ACQ_FETCH_USABLE },
  { "a floor of 0 still refuses an empty page",
    0, 200, 0, 0, ACQ_FETCH_UNUSABLE },
  { "a floor of 0 accepts one byte",
    0, 200, 1, 0, ACQ_FETCH_USABLE },
};

typedef struct
{
  const char *name;
  const char *url;
  bool        skip;
} rule_case_t;

static const rule_case_t rule_cases[] = {
  { "the bare host is skipped",
    "https://reuters.com/", true },
  { "a subdomain is skipped",
    "https://www.wsj.com/articles/x", true },
  { "a deep subdomain is skipped",
    "https://markets.ft.bloomberg.com/news", true },
  { "case does not matter",
    "HTTPS://WWW.ECONOMIST.COM/a", true },
  { "a host that ends the URL is skipped",
    "http://forbes.com", true },
  { "a port does not hide the host",
    "https://www.reuters.com:443/x", true },
  { "a query straight after the host does not hide it",
    "https://reuters.com?x=1", true },
  { "userinfo does not hide the host",
    "https://user@reuters.com/x", true },
  { "a trailing dot does not hide the host",
    "https://reuters.com./x", true },
  { "a longer name ending the same is a different host",
    "https://notreuters.com/", false },
  { "the name as a subdomain of another host is not that host",
    "https://reuters.com.evil.example/", false },
  { "a regional domain is not the .com",
    "https://www.forbes.com.au/", false },
  { "the name in the query of another host is not that host",
    "https://example.com/?ref=https://reuters.com/", false },
  { "a query with no path cannot lend another host its name",
    "https://example.com?ref=a.reuters.com/x", false },
  { "a fragment with no path cannot lend another host its name",
    "https://example.com#a.wsj.com/", false },
  { "an @ in the query is not userinfo",
    "https://example.com?u=a@reuters.com/", false },
  { "the name in the path of another host is not that host",
    "https://example.com/reuters.com/x", false },
};

static void
host_rows(void)
{
  for(size_t i = 0; i < sizeof(host_cases) / sizeof(host_cases[0]); i++)
  {
    const host_case_t *c = &host_cases[i];
    char               out[ACQUIRE_HOST_SZ];
    bool               ok;

    memset(out, 'Z', sizeof(out));
    ok = acq_url_host(c->url, out, sizeof(out));
    test_check_bool("host", c->name, c->ok, ok);
    test_check_str("host", c->name, c->want, out);
  }
}

// The two bounds: a host that exactly fills the buffer, and one byte
// more. Over the bound is a refusal, never a truncated host — a cut
// name is a different host, and the skip would land on it.
static void
host_bound_rows(void)
{
  char out[8];

  test_check_bool("host bound", "a host of cap - 1 bytes fits", true,
      acq_url_host("https://abcdefg/", out, sizeof(out)));
  test_check_str("host bound", "a host of cap - 1 bytes is whole",
      "abcdefg", out);
  test_check_bool("host bound", "a host of cap bytes is refused", false,
      acq_url_host("https://abcdefgh/", out, sizeof(out)));
  test_check_str("host bound", "a refused host leaves out empty", "", out);
}

static void
verdict_rows(void)
{
  for(size_t i = 0; i < sizeof(verdict_cases) / sizeof(verdict_cases[0]); i++)
  {
    const verdict_case_t *c = &verdict_cases[i];

    test_check_sz("verdict", c->name, (size_t)c->want,
        (size_t)acq_fetch_verdict(c->curl_code, c->status, c->strip_len,
            c->min_usable));
  }
}

// The rule is compiled with the flags acq_skip_set_regex uses — the
// same macro, so the two cannot drift.
static void
rule_rows(void)
{
  regex_t rx;

  if(regcomp(&rx, ACQUIRE_DEF_PAGE_URL_SKIP_REGEX,
      ACQUIRE_SKIP_REGEX_FLAGS) != 0)
  {
    test_check_bool("rule", "the shipped pattern compiles", true, false);
    return;
  }

  for(size_t i = 0; i < sizeof(rule_cases) / sizeof(rule_cases[0]); i++)
  {
    const rule_case_t *c = &rule_cases[i];

    test_check_bool("rule", c->name, c->skip,
        regexec(&rx, c->url, 0, NULL, 0) == 0);
  }

  regfree(&rx);
}

int
main(void)
{
  host_rows();
  host_bound_rows();
  verdict_rows();
  rule_rows();

  return(test_report(SUITE));
}
