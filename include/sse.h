#ifndef BM_SSE_H
#define BM_SSE_H

#include <stddef.h>

typedef struct sse_parser sse_parser_t;

// Called once per complete SSE frame. data is the concatenated
// data-field payload (NUL-terminated, owned by the parser until the
// callback returns). len excludes trailing NUL.
typedef void (*sse_event_cb_t)(const char *data, size_t len, void *user);

sse_parser_t *sse_parser_new(void);
void          sse_parser_free(sse_parser_t *p);
void          sse_parser_reset(sse_parser_t *p);

// Splits on blank line (\n\n or \r\n\r\n), accumulates multiple
// "data:" lines with "\n" separators per SSE spec. Lines starting
// with ':' are comments (skipped). The "[DONE]" sentinel is still
// delivered as an event — caller decides what to do. Returns number
// of events emitted during this feed.
size_t sse_parser_feed(sse_parser_t *p, const char *buf, size_t len,
    sse_event_cb_t cb, void *user);

#endif // BM_SSE_H
