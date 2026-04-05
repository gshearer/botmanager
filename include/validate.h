#ifndef BM_VALIDATE_H
#define BM_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Validate that str contains only alphanumeric characters and underscores.
// Returns false if str is NULL, empty, or exceeds maxlen.
// str: NUL-terminated string to validate
// maxlen: maximum allowed length (0 = no limit)
bool validate_alnum(const char *str, size_t maxlen);

// Validate that str contains only ASCII digits within the given length
// bounds.  Returns false if str is NULL, empty, shorter than minlen,
// or longer than maxlen.
// str: NUL-terminated string to validate
// minlen: minimum required length
// maxlen: maximum allowed length
bool validate_digits(const char *str, size_t minlen, size_t maxlen);

// Validate that str is a valid hostname: alphanumeric characters, dots,
// hyphens, and colons (for IPv6).  Returns false if str is NULL or empty.
// str: NUL-terminated string to validate
bool validate_hostname(const char *str);

// Parse and validate a port number string.  Returns true if the string
// is a valid decimal port number in the range 1-65535.
// str: NUL-terminated string to validate
// out: if non-NULL, receives the parsed port number on success
bool validate_port(const char *str, uint16_t *out);

// Validate an IRC channel name: must not contain spaces, control
// characters (0x00-0x20, 0x07), or commas.  Returns false if str is
// NULL or empty.
// str: NUL-terminated channel name (without leading '#')
bool validate_irc_channel(const char *str);

#endif // BM_VALIDATE_H
