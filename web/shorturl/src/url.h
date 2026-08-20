#ifndef SHORTURL_URL_H
#define SHORTURL_URL_H

#include <stdbool.h>

#include "config.h"

// The destination-URL boundary.
//
// A target reaches this program from three untrusted directions: the
// operator's argv, the process environment, and a database row that some
// other client may have written. All three pass through here.
//
// Accepts a non-empty string of at most SHORTURL_TARGET_MAX bytes carrying no
// control characters. The control-character rule is the load-bearing one: a CR
// or LF would forge additional HTTP response headers the moment the value
// reached a Location line.
bool url_valid(const char *);

#endif
