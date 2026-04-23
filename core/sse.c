// botmanager — MIT
// Server-Sent Events stream reader for streaming LLM responses.
#include "sse.h"

#include "alloc.h"

#include <stdbool.h>
#include <string.h>

#define SSE_INIT_CAP 4096

struct sse_parser
{
  // Current line accumulator (grows until \n seen).
  char   *line;
  size_t  line_len;
  size_t  line_cap;

  // Current frame data accumulator (multiple data: fields joined by \n).
  char   *frame;
  size_t  frame_len;
  size_t  frame_cap;
  bool    has_data;   // at least one data: line seen in current frame
};

// cap: pointer to capacity
static void
sse_ensure_cap(char **buf, size_t *cap, size_t needed)
{
  size_t required = needed + 1;
  size_t newcap;

  if(required <= *cap)
    return;

  newcap = (*cap == 0) ? SSE_INIT_CAP : *cap;

  while(newcap < required)
    newcap *= 2;

  if(*buf == NULL)
    *buf = mem_alloc("sse", "buf", newcap);
  else
    *buf = mem_realloc(*buf, newcap);

  *cap = newcap;
}

sse_parser_t *
sse_parser_new(void)
{
  sse_parser_t *p = mem_alloc("sse", "parser", sizeof(*p));

  p->line     = NULL;
  p->line_len = 0;
  p->line_cap = 0;

  p->frame     = NULL;
  p->frame_len = 0;
  p->frame_cap = 0;
  p->has_data  = false;

  return(p);
}

void
sse_parser_free(sse_parser_t *p)
{
  if(p == NULL)
    return;

  if(p->line != NULL)
    mem_free(p->line);

  if(p->frame != NULL)
    mem_free(p->frame);

  mem_free(p);
}

// p: parser (must not be NULL)
void
sse_parser_reset(sse_parser_t *p)
{
  p->line_len  = 0;
  p->frame_len = 0;
  p->has_data  = false;
}

static void
sse_frame_append(sse_parser_t *p, const char *data, size_t len)
{
  size_t add = len + (p->has_data ? 1 : 0);

  sse_ensure_cap(&p->frame, &p->frame_cap, p->frame_len + add);

  if(p->has_data)
    p->frame[p->frame_len++] = '\n';

  if(len > 0)
    memcpy(p->frame + p->frame_len, data, len);

  p->frame_len += len;
  p->frame[p->frame_len] = '\0';
  p->has_data = true;
}

// Process one complete line (no trailing CR/LF). Handle SSE fields:
// - empty line: emit frame if data accumulated
// - leading ':': comment, skip
// - "data:"/"data": accumulate payload into frame buffer
// - other fields (event, id, retry, unknown): ignore
// Returns 1 if a frame event was emitted, 0 otherwise.
static size_t
sse_process_line(sse_parser_t *p, sse_event_cb_t cb, void *user)
{
  const char *line;
  size_t      llen;
  size_t      colon = 0;
  size_t      fname_len;
  const char *val;
  size_t      vlen;

  // Blank line: dispatch frame if any data was accumulated.
  if(p->line_len == 0)
  {
    if(!p->has_data)
      return(0);

    // Ensure NUL terminator is present.
    sse_ensure_cap(&p->frame, &p->frame_cap, p->frame_len);
    p->frame[p->frame_len] = '\0';

    if(cb != NULL)
      cb(p->frame, p->frame_len, user);

    p->frame_len = 0;
    p->has_data  = false;

    return(1);
  }

  line = p->line;
  llen = p->line_len;

  // Comment line.
  if(line[0] == ':')
    return(0);

  // Locate field/value separator.
  while(colon < llen && line[colon] != ':')
    colon++;

  // No colon: treat whole line as field name with empty value — per spec
  // we would dispatch an empty value, but we only care about data fields.
  // So lines without a colon are ignored.
  if(colon == llen)
    return(0);

  fname_len = colon;
  val       = line + colon + 1;
  vlen      = llen - colon - 1;

  // Strip a single optional leading space on the value.
  if(vlen > 0 && val[0] == ' ')
  {
    val++;
    vlen--;
  }

  // Only "data" is honored. Everything else (event, id, retry, unknown)
  // is deliberately skipped — OpenAI's chat stream only uses data.
  if(fname_len == 4 && memcmp(line, "data", 4) == 0)
    sse_frame_append(p, val, vlen);

  return(0);
}

static void
sse_line_push(sse_parser_t *p, char c)
{
  sse_ensure_cap(&p->line, &p->line_cap, p->line_len + 1);
  p->line[p->line_len++] = c;
  p->line[p->line_len]   = '\0';
}

// p: parser (must not be NULL)
// cb: event callback (may be NULL to drain without dispatch)
size_t
sse_parser_feed(sse_parser_t *p, const char *buf, size_t len,
    sse_event_cb_t cb, void *user)
{
  size_t emitted = 0;

  for(size_t i = 0; i < len; i++)
  {
    char c = buf[i];

    if(c == '\n')
    {
      // Strip trailing CR if present (normalize CRLF -> LF).
      if(p->line_len > 0 && p->line[p->line_len - 1] == '\r')
      {
        p->line_len--;
        p->line[p->line_len] = '\0';
      }

      emitted += sse_process_line(p, cb, user);
      p->line_len = 0;
    }

    else
      sse_line_push(p, c);
  }

  return(emitted);
}
