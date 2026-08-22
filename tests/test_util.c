// botmanager — MIT
// Cases for util.h's silent-wrongness surfaces: URL redaction, the
// SSRF gate, URL canonicalization, and base64.
#include "test.h"
#include "util.h"
#include "common.h"

#include <string.h>

// A host longer than util_url_is_safe_https()'s 256-byte scratch, built
// from a 40-byte unit so the arithmetic is visible.
#define H40   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define H280  H40 H40 H40 H40 H40 H40 H40

// Every buffer a case writes into. Cases that exercise a small cap pass
// their own, smaller value as `cap` — the buffer itself stays this size
// so an overrun lands in readable memory rather than off the end.
#define OUT_SZ  512

// A byte no case legitimately produces. Pre-loaded into the output
// buffer before each call, it proves a refusal wrote nothing at all
// rather than writing an empty string.
#define UNTOUCHED  "!"

// util_redact_url: anything a log line can reach passes through here,
// so a missed parameter is a credential on disk in plaintext.
static const struct
{
  const char *name;
  const char *url;
  size_t      cap;
  const char *want;
} redact_cases[] = {
  { "no query survives whole", "https://api.example.com/v1/forecast", OUT_SZ,
    "https://api.example.com/v1/forecast" },
  { "null url", NULL, OUT_SZ, "" },
  { "empty query", "https://h/p?", OUT_SZ, "https://h/p?" },
  { "bare key", "https://h/p?key=SEK", OUT_SZ, "https://h/p?key=[CENSORED]" },
  { "credential among innocents",
    "https://h/p?q=weather&api_key=SEK&units=metric", OUT_SZ,
    "https://h/p?q=weather&api_key=[CENSORED]&units=metric" },
  { "hyphenated name", "https://h/p?x-api-key=SEK", OUT_SZ,
    "https://h/p?x-api-key=[CENSORED]" },
  { "two credentials", "https://h/p?access_token=A&client_secret=B", OUT_SZ,
    "https://h/p?access_token=[CENSORED]&client_secret=[CENSORED]" },
  { "name matched case-insensitively", "https://h/p?API_KEY=SEK", OUT_SZ,
    "https://h/p?API_KEY=[CENSORED]" },
  { "short fragment", "https://h/p?sig=abc&t=1", OUT_SZ,
    "https://h/p?sig=[CENSORED]&t=1" },
  // Documented over-match: substring matching catches `x-api-key` and
  // `client_secret` without an exhaustive list, and "monkey" with them.
  { "over-match is the safe direction", "https://h/p?monkey=banana", OUT_SZ,
    "https://h/p?monkey=[CENSORED]" },
  { "parameter with no value", "https://h/p?key", OUT_SZ, "https://h/p?key" },
  // Values are redacted as they are written, so a cut mid-redaction
  // cuts the redaction — never the secret it stands in for.
  { "truncation cannot expose", "https://h/p?key=SUPERSECRET", 20,
    "https://h/p?key=[CE" },
  { "no room for anything", "https://h/p?key=SUPERSECRET", 1, "" },
};

// util_url_is_safe_https: the SSRF gate. A false positive fetches a
// private address on an attacker's behalf.
static const struct
{
  const char *name;
  const char *url;
  bool        want;
} safe_https_cases[] = {
  { "ordinary https", "https://api.example.com/v1/x", true },
  { "scheme matched case-insensitively", "HTTPS://api.example.com", true },
  { "port is not part of the host", "https://example.com:8443/x", true },
  { "plain http", "http://api.example.com/x", false },
  { "other scheme", "ftp://api.example.com/x", false },
  { "null", NULL, false },
  { "empty host", "https://", false },
  { "host longer than the scratch buffer", "https://" H280 ".com/", false },
  { "localhost", "https://localhost/x", false },
  { "localhost subdomain", "https://localhost.localdomain/", false },
  // Dotted-quad literals, private and public alike: the gate refuses the
  // whole form, so these pin the policy and not a range table.
  { "loopback v4", "https://127.0.0.1/", false },
  { "private 10/8", "https://10.0.0.5/", false },
  { "private 192.168/16", "https://192.168.1.1/", false },
  { "private 172.16/12", "https://172.16.0.1/", false },
  { "link-local v4", "https://169.254.1.1/", false },
  { "cloud metadata", "https://169.254.169.254/latest/meta-data/", false },
  { "zero net", "https://0.0.0.0/", false },
  { "public v4 literal is refused with them", "https://93.184.216.34/x", false },
  // Alternate spellings of 127.0.0.1. Each was measured through
  // getaddrinfo(), which is what curl resolves with, and each reached
  // 127.0.0.1; inet_pton(AF_INET) parses none of the four, which is why
  // the gate reads them with inet_aton().
  { "integer-encoded v4", "https://2130706433/", false },
  { "hex-encoded v4", "https://0x7f000001/", false },
  { "octal-encoded v4", "https://0177.0.0.1/", false },
  { "short-form v4", "https://127.1/", false },
  // A bracket introduces an IP literal and may introduce nothing else,
  // so it is refused before the host scan ever runs — which is why the
  // last two cases, neither of them a literal, are refusals too.
  { "loopback v6", "https://[::1]/", false },
  { "expanded loopback v6", "https://[0:0:0:0:0:0:0:1]/", false },
  { "ipv4-mapped loopback v6", "https://[::ffff:127.0.0.1]/", false },
  { "unspecified v6", "https://[::]/", false },
  { "link-local v6", "https://[fe80::1]/", false },
  { "link-local v6 with a zone id", "https://[fe80::1%25eth0]/", false },
  { "unique-local v6 fc00::/8", "https://[fc00::1]/", false },
  { "unique-local v6 fd00::/8", "https://[fd00::1]/", false },
  { "public v6 literal is refused with them", "https://[2606:4700::1111]/",
    false },
  { "bracket wrapping a name", "https://[example.com]/", false },
  { "unclosed bracket", "https://[::1/", false },
  // Names, not literals. The first two were false positives under the
  // old fc/fd prefix tests, which read any name beginning fc or fd plus
  // a hex digit as a unique-local address.
  { "name beginning fc + hex digit", "https://fc2.example.com/i.png", true },
  { "name beginning fd + hex digit", "https://fdn.example.com/i.png", true },
  { "numeric labels that are not an address",
    "https://1.2.3.4.example.com/i.png", true },
  // The documented limit, pinned deliberately: the gate does not
  // resolve, so a name whose A record points at 127.0.0.1 passes.
  { "gate is hostname-only, not resolved",
    "https://192.168.1.1.nip.io/i.png", true },
};

// util_url_canon: the identity a knowledge row is stored under, and
// what knowledge_page_supersede() DELETEs by. Over-collapsing destroys
// a page; under-collapsing keeps the duplicate this exists to stop.
static const struct
{
  const char *name;
  const char *url;
  size_t      cap;
  const char *want;
} canon_cases[] = {
  { "canonical already", "https://en.wikipedia.org/wiki/Waifu", OUT_SZ,
    "https://en.wikipedia.org/wiki/Waifu" },
  // The three spellings one page arrived under, each collapsing onto
  // the case above.
  { "mobile mirror", "https://en.m.wikipedia.org/wiki/Waifu", OUT_SZ,
    "https://en.wikipedia.org/wiki/Waifu" },
  { "encoded apostrophe", "https://en.wikipedia.org/wiki/Sha%27Carri", OUT_SZ,
    "https://en.wikipedia.org/wiki/Sha'Carri" },
  { "bare apostrophe", "https://en.wikipedia.org/wiki/Sha'Carri", OUT_SZ,
    "https://en.wikipedia.org/wiki/Sha'Carri" },
  { "mobile label leads the host", "https://m.facebook.com/x", OUT_SZ,
    "https://facebook.com/x" },
  { "two labels keep their m", "https://m.co/x", OUT_SZ, "https://m.co/x" },
  // Structure stays encoded whatever its case: decoding one of these
  // would rewrite what the URL is made of.
  { "structural triplets kept, hex uppercased", "https://h/a%2fb%3fc", OUT_SZ,
    "https://h/a%2Fb%3Fc" },
  { "NUL is not decoded", "https://h/a%00b", OUT_SZ, "https://h/a%00b" },
  { "truncated triplet is a literal percent", "https://h/100%", OUT_SZ,
    "https://h/100%" },
  { "non-hex triplet is a literal percent", "https://h/%zz", OUT_SZ,
    "https://h/%zz" },
  { "scheme and host lowercase, path does not",
    "HTTPS://EN.Wikipedia.ORG/Wiki/X", OUT_SZ,
    "https://en.wikipedia.org/Wiki/X" },
  { "default https port", "https://h:443/x", OUT_SZ, "https://h/x" },
  { "default http port", "http://h:80/x", OUT_SZ, "http://h/x" },
  { "empty port is the default", "https://h:/x", OUT_SZ, "https://h/x" },
  { "other port is part of the host", "https://h:8443/x", OUT_SZ,
    "https://h:8443/x" },
  { "fragment is resolved by the client", "https://h/x#frag", OUT_SZ,
    "https://h/x" },
  { "empty path", "https://claude.com", OUT_SZ, "https://claude.com/" },
  { "empty path before a query", "https://h?q=1", OUT_SZ, "https://h/?q=1" },
  // The query is identity: these two are different forecasts.
  { "query survives verbatim",
    "https://forecast.weather.gov/MapClick.php?lat=30.265&lon=-97.7466",
    OUT_SZ,
    "https://forecast.weather.gov/MapClick.php?lat=30.265&lon=-97.7466" },
  { "a path's trailing slash is its own", "https://h/x/", OUT_SZ,
    "https://h/x/" },
  // Shapes it declines to touch: each comes back as it arrived.
  { "non-http scheme", "ftp://h/x", OUT_SZ, "ftp://h/x" },
  { "a local path is not a URL", "/tmp/corpus/page.txt", OUT_SZ,
    "/tmp/corpus/page.txt" },
  { "userinfo is case-sensitive", "https://U:P@H/x", OUT_SZ,
    "https://U:P@H/x" },
  { "ip literal", "https://[::1]/x", OUT_SZ, "https://[::1]/x" },
  { "null url", NULL, OUT_SZ, "" },
  { "empty url", "", OUT_SZ, "" },
  // A truncated URL is a different URL, so a buffer that cannot hold
  // the canonical form holds the input instead — cut, but never
  // silently renamed.
  { "no room to canonicalize", "https://en.m.wikipedia.org/x", 6, "https" },
};

// util_b64_encode / util_b64url_encode. `url` selects the encoder;
// a want of 0 bytes with UNTOUCHED output is a refusal.
static const struct
{
  const char *name;
  bool        url;
  const char *in;
  size_t      in_len;
  size_t      cap;
  size_t      want_len;
  const char *want;
} b64_encode_cases[] = {
  // RFC 4648 §10 test vectors, which pin all three padding shapes.
  { "empty",      false, "",       0, OUT_SZ, 0, ""         },
  { "one byte",   false, "f",      1, OUT_SZ, 4, "Zg=="     },
  { "two bytes",  false, "fo",     2, OUT_SZ, 4, "Zm8="     },
  { "three bytes",false, "foo",    3, OUT_SZ, 4, "Zm9v"     },
  { "four bytes", false, "foob",   4, OUT_SZ, 8, "Zm9vYg==" },
  { "five bytes", false, "fooba",  5, OUT_SZ, 8, "Zm9vYmE=" },
  { "six bytes",  false, "foobar", 6, OUT_SZ, 8, "Zm9vYmFy" },
  { "exact capacity", false, "foobar", 6, 9, 8, "Zm9vYmFy" },
  { "one byte short refuses", false, "foobar", 6, 8, 0, UNTOUCHED },
  // 0xfb 0xff is the shortest input reaching both substituted glyphs.
  { "url alphabet", true, "\xfb\xff", 2, OUT_SZ, 3, "-_8" },
  { "url padding stripped", true, "foob", 4, OUT_SZ, 6, "Zm9vYg" },
  { "url needs no padding", true, "foo", 3, OUT_SZ, 4, "Zm9v" },
  { "url refusal propagates", true, "foobar", 6, 8, 0, UNTOUCHED },
};

// util_b64_decode. Returns SUCCESS (which is false — common.h) or FAIL.
static const struct
{
  const char *name;
  const char *in;
  size_t      in_len;
  size_t      cap;
  bool        want_rc;
  size_t      want_len;
  const char *want;
} b64_decode_cases[] = {
  { "roundtrip", "Zm9vYmFy", 8, OUT_SZ, SUCCESS, 6, "foobar" },
  { "empty input", "", 0, OUT_SZ, SUCCESS, 0, "" },
  { "padding ends the stream", "Zm8=", 4, OUT_SZ, SUCCESS, 2, "fo" },
  { "trailing bytes after padding are ignored",
    "Zm8=Zm9v", 8, OUT_SZ, SUCCESS, 2, "fo" },
  { "ascii whitespace skipped", "Zm9v\nYm Fy", 10, OUT_SZ, SUCCESS, 6,
    "foobar" },
  { "invalid character", "Zm9v!Ym", 7, OUT_SZ, FAIL, 0, "" },
  // The decoder is standard-alphabet only; b64url output is not input.
  { "url alphabet rejected", "-_8", 3, OUT_SZ, FAIL, 0, "" },
  { "output too small", "Zm9vYmFy", 8, 3, FAIL, 0, "" },
};

int
main(void)
{
  char out[OUT_SZ];

  for(size_t i = 0; i < sizeof(redact_cases) / sizeof(redact_cases[0]); i++)
  {
    const char *got;

    memset(out, 0, sizeof(out));
    got = util_redact_url(redact_cases[i].url, out, redact_cases[i].cap);
    test_check_str("redact_url", redact_cases[i].name,
        redact_cases[i].want, got);
  }

  for(size_t i = 0;
      i < sizeof(safe_https_cases) / sizeof(safe_https_cases[0]); i++)
    test_check_bool("url_is_safe_https", safe_https_cases[i].name,
        safe_https_cases[i].want,
        util_url_is_safe_https(safe_https_cases[i].url));

  for(size_t i = 0; i < sizeof(canon_cases) / sizeof(canon_cases[0]); i++)
  {
    const char *got;

    memset(out, 0, sizeof(out));
    got = util_url_canon(canon_cases[i].url, out, canon_cases[i].cap);
    test_check_str("url_canon", canon_cases[i].name,
        canon_cases[i].want, got);
  }

  for(size_t i = 0;
      i < sizeof(b64_encode_cases) / sizeof(b64_encode_cases[0]); i++)
  {
    size_t n;

    strlcpy(out, UNTOUCHED, sizeof(out));

    n = b64_encode_cases[i].url
      ? util_b64url_encode(b64_encode_cases[i].in, b64_encode_cases[i].in_len,
          out, b64_encode_cases[i].cap)
      : util_b64_encode(b64_encode_cases[i].in, b64_encode_cases[i].in_len,
          out, b64_encode_cases[i].cap);

    test_check_sz("b64_encode", b64_encode_cases[i].name,
        b64_encode_cases[i].want_len, n);
    test_check_str("b64_encode", b64_encode_cases[i].name,
        b64_encode_cases[i].want, out);
  }

  for(size_t i = 0;
      i < sizeof(b64_decode_cases) / sizeof(b64_decode_cases[0]); i++)
  {
    size_t n = 0;
    bool   rc;

    memset(out, 0, sizeof(out));
    rc = util_b64_decode(b64_decode_cases[i].in, b64_decode_cases[i].in_len,
        out, b64_decode_cases[i].cap, &n);

    test_check_bool("b64_decode", b64_decode_cases[i].name,
        b64_decode_cases[i].want_rc, rc);

    // A refusal makes no promise about *written or the buffer, so
    // want_len / want describe the accepted rows only.
    if(rc != SUCCESS)
      continue;

    test_check_sz("b64_decode", b64_decode_cases[i].name,
        b64_decode_cases[i].want_len, n);

    out[n] = '\0';
    test_check_str("b64_decode", b64_decode_cases[i].name,
        b64_decode_cases[i].want, out);
  }

  return(test_report("util"));
}
