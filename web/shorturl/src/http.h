#ifndef SHORTURL_HTTP_H
#define SHORTURL_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <fcgiapp.h>

// Reads and validates SHORTURL_NOT_FOUND_URL once. False means the variable is
// set but malformed, which should stop the daemon at startup rather than
// surprise a visitor later. Call before serving.
bool http_init(void);

// The three replies this service can make. Each writes a complete response,
// headers and body, and none fails in a way the caller could act on.

// target must satisfy url_valid(); a control character would forge headers.
void http_redirect(FCGX_Request *, const char *);

// Redirects to SHORTURL_NOT_FOUND_URL when set, otherwise serves a static body.
void http_not_found(FCGX_Request *);

void http_server_error(FCGX_Request *);

#ifdef HTTP_INTERNAL

static void http_send(FCGX_Request *, const char *, const char *, size_t);

#endif

#endif
