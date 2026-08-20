#ifndef SHORTURL_TOKEN_H
#define SHORTURL_TOKEN_H

#include <stdbool.h>

#include "config.h"

// A validated short-link token: exactly SHORTURL_TOKEN_LEN base62 characters,
// NUL-terminated.
//
// This is the domain type for the query-string trust boundary. The only two
// functions that may produce one are declared below; everything downstream
// takes a const token_t * and re-validates nothing. It is a struct rather than
// a typedef over char[] so that a raw pointer cannot be laundered into one
// without a compile error.
typedef struct
{
  char s[SHORTURL_TOKEN_LEN + 1];
} token_t;

// The boundary. raw is untrusted and may be NULL, unterminated within the
// inspected window, or arbitrary bytes. out is written only on success.
bool token_parse(const char *, token_t *);

// Draws from getrandom(2) with rejection sampling, so the distribution over
// the alphabet is uniform. False means the kernel refused to supply entropy.
bool token_generate(token_t *);

#ifdef TOKEN_INTERNAL

static bool base62_is_member(unsigned char);

#endif

#endif
