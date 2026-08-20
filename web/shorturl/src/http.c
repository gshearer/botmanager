// botmanager — MIT
// shorturl: HTTP response emission — redirect, not-found and server-error.

#define HTTP_INTERNAL

#include "http.h"
#include "config.h"
#include "url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char NOT_FOUND_BODY[] =
  "<!doctype html>\n"
  "<meta charset=\"utf-8\">\n"
  "<title>404 Not Found</title>\n"
  "<h1>404 Not Found</h1>\n"
  "<p>That short link does not exist.</p>\n";

static const char SERVER_ERROR_BODY[] =
  "<!doctype html>\n"
  "<meta charset=\"utf-8\">\n"
  "<title>500 Internal Server Error</title>\n"
  "<h1>500 Internal Server Error</h1>\n"
  "<p>The link database is unavailable. Please try again shortly.</p>\n";

// Resolved once by http_init(). NULL means serve the static body instead.
// Caching it also keeps a linear scan of environ off the 404 path, which is
// where scanner traffic lands and is therefore busier than the redirect path.
static const char *not_found_url;

static void
http_send(FCGX_Request *request, const char *status, const char *body, size_t length)
{
  // libfcgi ships its own printf, and it does not implement the z length
  // modifier: FCGX_FPrintF("%zu") emits nothing and abandons the rest of the
  // format string, which silently costs the blank line separating headers from
  // body. The number is therefore formatted by the C library and everything
  // goes out as plain strings.
  //
  // 24 bytes holds the decimal form of any 64-bit size_t, so no truncation is
  // representable here and none is checked for.
  char length_text[24];

  snprintf(length_text, sizeof length_text, "%zu", length);

  FCGX_PutS("Status: ", request->out);
  FCGX_PutS(status, request->out);
  FCGX_PutS("\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: ", request->out);
  FCGX_PutS(length_text, request->out);
  FCGX_PutS("\r\n"
            "Cache-Control: no-store\r\n"
            "\r\n", request->out);

  FCGX_PutStr(body, (int)length, request->out);
}

bool
http_init(void)
{
  const char *configured = getenv(SHORTURL_ENV_NOT_FOUND_URL);

  if(!configured || !*configured)
    return(true);

  if(!url_valid(configured))
  {
    fprintf(stderr, "shorturl: %s is not a usable URL\n", SHORTURL_ENV_NOT_FOUND_URL);
    return(false);
  }

  not_found_url = configured;

  return(true);
}

void
http_redirect(FCGX_Request *request, const char *target)
{
  // Emitted in pieces rather than formatted into a buffer: there is no fixed
  // size to overrun, and therefore no truncation to guard against.
  //
  // 302, not 301. A permanent redirect is cached by the browser, so every
  // repeat visit would bypass this process entirely and go uncounted — which
  // would quietly defeat the hit counter. no-store asks intermediate caches to
  // stand aside for the same reason.
  FCGX_PutS("Status: 302 Found\r\n"
            "Location: ", request->out);

  FCGX_PutS(target, request->out);

  FCGX_PutS("\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Length: 0\r\n"
            "\r\n", request->out);
}

void
http_not_found(FCGX_Request *request)
{
  if(not_found_url)
  {
    http_redirect(request, not_found_url);
    return;
  }

  http_send(request, "404 Not Found", NOT_FOUND_BODY, sizeof NOT_FOUND_BODY - 1);
}

void
http_server_error(FCGX_Request *request)
{
  http_send(request, "500 Internal Server Error",
            SERVER_ERROR_BODY, sizeof SERVER_ERROR_BODY - 1);
}
