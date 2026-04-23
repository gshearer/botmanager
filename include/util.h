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

// Scan text for the first image URL (case-insensitive match on
// https?://[^\s<>"']+\.(jpe?g|png|gif|webp)(\?[^\s<>"']*)?). Copies
// the URL into out (NUL-terminated). Returns true on hit.
bool util_find_image_url(const char *text, char *out, size_t out_cap);

// Reject URLs that would SSRF onto private / link-local / loopback /
// cloud-metadata IPs, or that use any non-https scheme. Hostname-only
// (does not resolve DNS); operators needing rebind resistance should
// firewall bot egress to deny RFC 1918. Returns true when the URL is
// safe to fetch.
bool util_url_is_safe_https(const char *url);

#endif // BM_UTIL_H
