#ifndef BM_UTIL_H
#define BM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Seeds the PRNG. Must be called early in startup, before any module
// that uses rand().
void util_init(void);

// Uniformly distributed random integer in [0, upper). upper must be > 0.
int util_rand(int upper);

// Format a byte count into a compact human-readable string (B/K/M/G).
void util_fmt_bytes(size_t bytes, char *buf, size_t sz);

// Format seconds into a compact human-readable duration.
// Output examples: "12s", "3m42s", "2h15m", "1d6h". Negative input is
// clamped to 0.
void util_fmt_duration(time_t secs, char *buf, size_t sz);

// Case-sensitive 32-bit FNV-1a hash.
uint32_t util_fnv1a(const char *s);

// Case-insensitive 32-bit FNV-1a hash.
uint32_t util_fnv1a_ci(const char *s);

uint32_t util_djb2(const char *s);

// Bounded substring search: find the first occurrence of `needle` in
// [hay, hay+haylen). Returns pointer into hay or NULL.
const char *util_memstr(const char *hay, size_t haylen, const char *needle);

// Parse a signed decimal integer at p, bounded by end. Returns a pointer
// just past the parsed digits (same as strtol's endptr), or NULL if no
// digits were consumed or parsing ran past `end`.
const char *util_read_int(const char *p, const char *end, long *out);

// After locating a key in a key:value stream, advance past whitespace and
// the ':' to the start of the value. Returns pointer or NULL on malformed.
const char *util_skip_to_value(const char *p, const char *end);

// Milliseconds elapsed since `start` on CLOCK_MONOTONIC. Clamped to 0.
uint64_t util_ms_since(const struct timespec *start);

// Base64-encode in into out. RFC 4648 standard alphabet, padded.
// Returns bytes written excluding the trailing NUL, or 0 on buffer
// overflow. out_cap must be >= ((in_len + 2) / 3) * 4 + 1.
size_t util_b64_encode(const void *in, size_t in_len,
    char *out, size_t out_cap);

// Base64url-encode in into out. RFC 4648 §5 URL-safe alphabet
// (`+`→`-`, `/`→`_`) with the trailing `=` padding stripped — the
// shape JWTs require. Returns bytes written excluding the trailing
// NUL, or 0 on buffer overflow. out_cap must be >=
// ((in_len + 2) / 3) * 4 + 1.
size_t util_b64url_encode(const void *in, size_t in_len,
    char *out, size_t out_cap);

// Base64-decode in[0,in_len) into out. RFC 4648 standard alphabet;
// ASCII whitespace is skipped and a '=' pad byte ends the stream.
// Writes at most out_cap bytes and stores the decoded length in
// *written (may be NULL). Returns SUCCESS, or FAIL on an invalid
// character or when out_cap is too small to hold the payload.
bool util_b64_decode(const char *in, size_t in_len, void *out,
    size_t out_cap, size_t *written);

// Scan text for the first image URL (case-insensitive match on
// https?://[^\s<>"']+\.(jpe?g|png|gif|webp)(\?[^\s<>"']*)?). Copies
// the URL into out (NUL-terminated). Returns true on hit.
bool util_find_image_url(const char *text, char *out, size_t out_cap);

// Copy `url` into `out`, replacing the value of every credential-bearing
// query parameter with "[CENSORED]". Anything a log line can reach must
// pass through here first: several services authenticate with a plain
// `?key=`/`?api_key=` query parameter, so logging a request URL verbatim
// writes the secret to disk in plaintext.
//
// A parameter is credential-bearing when its name CONTAINS any of: key,
// token, secret, password, passwd, pass, auth, signature, sig, crumb,
// credential, session, cookie. Substring matching is deliberate — it
// catches `x-api-key`, `client_secret` and `access_token` without an
// exhaustive list — and so is the over-matching that comes with it: a
// parameter innocently named "monkey" is redacted too. In a log line that
// is the harmless direction to be wrong in.
//
// The scheme, host, path and every non-credential parameter survive intact,
// so the result is still worth reading. Output is always NUL-terminated and
// truncates rather than overflowing; because values are redacted as they are
// written, a truncated result can never expose a secret it meant to hide.
// Returns `out`, so it can be used inline as a clam() argument.
const char *util_redact_url(const char *url, char *out, size_t out_cap);

// Reject URLs that would SSRF onto an internal address, or that use any
// non-https scheme. Returns true when the URL is safe to fetch.
//
// The host must be a DNS name: every IP literal is refused, in every
// encoding and whatever it addresses, rather than the unsafe ranges
// being enumerated. Enumerating ranges means enumerating spellings too,
// and one address has many — `127.1`, `0177.0.0.1` and `2130706433` all
// reach 127.0.0.1 through the resolver. Refusing the whole form costs
// only the ability to fetch from a bare public address, which the
// chat-sourced URLs this gate exists for never are.
//
// Hostname-only: it does not resolve DNS, so a name whose A record
// points at 127.0.0.1 passes, and so does a rebind between this check
// and the connect. That is the standing limit of a URL-shaped gate —
// operators needing resistance to either should firewall bot egress to
// deny RFC 1918.
bool util_url_is_safe_https(const char *url);

// Rewrite `url` into `out` as the form that names the page rather than
// the link, so that two URLs which fetch one page come back as one
// string. Returns `out` and always NUL-terminates, so it reads inline
// like util_redact_url().
//
// Normalized: scheme and host lowercased, a default port dropped, a
// label spelled "m" dropped while two labels survive it (the mobile
// mirror — en.m.wikipedia.org), the fragment dropped, an empty path
// written "/", and percent-triplets uppercased, decoding those that
// spell an octet with no structural meaning (`%27` becomes `'`).
//
// Deliberately not normalized: the query, a non-empty path's trailing
// slash, dot segments, and a leading "www.". Each of those can name a
// different resource, and this string decides which knowledge rows
// knowledge_page_supersede() DELETEs — over-collapsing here destroys a
// page rather than deduplicating one. The query is identity: three rows
// in the acquired corpus differ only by a `?lat=`/`?lon=` pair.
//
// Anything it cannot canonicalize comes back as a bounded copy of the
// input — a NULL, a non-http(s) scheme, an authority carrying userinfo
// or an IP literal, or a result that will not fit. out_cap must hold
// strlen(url) + 2 for canonicalization to run at all, because a
// truncated URL is a different URL.
const char *util_url_canon(const char *url, char *out, size_t out_cap);

// Bump an eventfd's counter so the loop polling it runs a turn now rather
// than when its poll expires. Best-effort by design — a lost wake costs one
// poll timeout and nothing else — but every way it can fail is a defect: a
// stale fd, or a counter saturated because nobody is draining. Both are
// logged under the caller's own clam context, which `ctx` names. Both of
// these therefore clam() on the failure path — call them with no lock held.
void util_evfd_wake(int fd, const char *ctx);

// Consume the counter an epoll wake reported, so the level-triggered event
// stops firing. An empty counter is a wake another reader already drained,
// not a failure, and is silent.
void util_evfd_drain(int fd, const char *ctx);

#endif // BM_UTIL_H
