#ifndef BM_VALIDATE_H
#define BM_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// maxlen of 0 means no limit.
bool validate_alnum(const char *str, size_t maxlen);

bool validate_digits(const char *str, size_t minlen, size_t maxlen);

// Alphanumeric characters, dots, hyphens, and colons (for IPv6).
bool validate_hostname(const char *str);

// Parses a decimal port in 1-65535. out may be NULL.
bool validate_port(const char *str, uint16_t *out);

// Channel name (without leading '#'). Must not contain spaces, control
// characters (0x00-0x20, 0x07), or commas.
bool validate_irc_channel(const char *str);

#endif // BM_VALIDATE_H
