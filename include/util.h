#ifndef BM_UTIL_H
#define BM_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Initialize shared utilities. Seeds the PRNG.
// Must be called early in startup, before any module that uses rand().
void util_init(void);

// Return a uniformly distributed random integer in [0, upper).
// upper: exclusive upper bound (must be > 0)
int util_rand(int upper);

// Format a byte count into a compact human-readable string (B/K/M/G).
// bytes: byte count
// buf: output buffer
// sz: size of output buffer
void util_fmt_bytes(size_t bytes, char *buf, size_t sz);

// Format seconds into a compact human-readable duration.
// Output examples: "12s", "3m42s", "2h15m", "1d6h".
// secs: duration in seconds (clamped to 0 if negative)
// buf: output buffer
// sz: size of output buffer
void util_fmt_duration(time_t secs, char *buf, size_t sz);

// Compute a 32-bit FNV-1a hash of a string (case-sensitive).
// returns: hash value
// s: NUL-terminated input string
uint32_t util_fnv1a(const char *s);

// Compute a 32-bit FNV-1a hash of a string (case-insensitive).
// returns: hash value
// s: NUL-terminated input string
uint32_t util_fnv1a_ci(const char *s);

#endif // BM_UTIL_H
