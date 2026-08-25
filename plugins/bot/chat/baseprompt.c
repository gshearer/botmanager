// botmanager — MIT
// Base prompt: the always-included, block-structured half of a system prompt.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

// Every block the assemblers name, and the tokens each one may not lose.
// A base file that is missing a row, or has had a token edited out of
// one, does not load — the bot says so at start and keeps the last
// prompt that worked rather than quietly generating without the clause.
//
// ⭑ A token is listed here only when the runtime substitutes something
// the sentence cannot survive without. Prose is otherwise entirely the
// author's, including whether a token appears more than once.
static const struct
{
  const char *name;
  const char *tokens;   // space-separated, NULL for none
} base_required[] = {
  { "nick",              "$NICK"           },
  { "emote",             "$EMOTE"          },
  { "images",            NULL              },
  { "context-guard",     NULL              },
  { "policy-reminder",   NULL              },
  { "direct-address",    NULL              },
  { "action-at-bot",     "$SENDER $EMOTE"  },
  { "commands",          "$CMDSHAPE"       },
  { "room-written",      NULL              },
  { "room-spoken",       NULL              },
  { "tool-answer",       "$SENDER $ROOM"   },
  { "tool-empty",        "$SENDER"         },
  { "tool-ask-location", "$SENDER"         },
  { "nothing-to-say",    NULL              },
  { "cannot-do",         NULL              },
};

// Strip leading and trailing ASCII whitespace in place, returning s.
// Block bodies are stored trimmed so the assemblers decide their own
// separators and the file stays free to breathe around a header.
static char *
base_trim(char *s)
{
  size_t n;
  char  *p;

  p = s;

  while(*p != '\0' && isspace((unsigned char)*p)) p++;

  if(p != s)
    memmove(s, p, strlen(p) + 1);

  n = strlen(s);

  while(n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';

  return(s);
}

// Is this line a `[[name]]` header? On a hit `*name_out` points at the
// name inside `line` (NUL-terminated in place) and the line is consumed.
static bool
base_is_header(char *line, char **name_out)
{
  char  *p = line;
  size_t n;

  while(*p == ' ' || *p == '\t') p++;

  if(p[0] != '[' || p[1] != '[')
    return(false);

  p += 2;
  n  = strlen(p);

  while(n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
    n--;

  if(n < 3 || p[n - 1] != ']' || p[n - 2] != ']')
    return(false);

  p[n - 2] = '\0';
  *name_out = base_trim(p);

  return((*name_out)[0] != '\0');
}

// Walk `raw` in place, carving it into blocks. Bodies point into raw and
// are NUL-terminated where the next header began, so the whole document
// costs one allocation however many blocks it holds.
static void
base_parse(chatbot_base_t *b, char *raw)
{
  char *p = raw;
  char *body = NULL;

  while(*p != '\0')
  {
    char *eol  = strchr(p, '\n');
    char *next = (eol != NULL) ? eol + 1 : p + strlen(p);
    char *name;
    char  saved;

    // Terminate the line for inspection, then put the byte back: a body
    // spans the lines that follow it and must stay contiguous.
    saved = (eol != NULL) ? *eol : '\0';

    if(eol != NULL)
      *eol = '\0';

    if(p[0] == '#')
    {
      // A comment line belongs to nobody — and a body is prompt text, so
      // close the gap rather than blank it: twenty-five spaces in the
      // middle of a paragraph is a comment the model can still see.
      if(eol != NULL)
      {
        *eol = saved;
        memmove(p, next, strlen(next) + 1);
        continue;          // the next line has moved into p
      }

      *p = '\0';
      break;
    }

    if(base_is_header(p, &name))
    {
      // The previous body ends where this LINE began. base_is_header
      // terminates the name at its "]]", which is several bytes in — so
      // without this the body above would keep the "[[" and the name.
      *p = '\0';

      if(body != NULL)
        base_trim(body);

      if(b->n < CHATBOT_BASE_BLOCKS_MAX)
      {
        b->blk[b->n].name = name;
        b->blk[b->n].body = next;
        body = next;
        b->n++;
      }

      else
      {
        clam(CLAM_WARN, "chatbot",
            "base prompt: more than %d blocks — '[[%s]]' and anything"
            " after it is ignored", CHATBOT_BASE_BLOCKS_MAX, name);
        body = NULL;
      }

      // The header line is consumed; the terminator stays a NUL so the
      // previous body ends here.
      p = next;
      continue;
    }

    if(eol != NULL)
      *eol = saved;

    p = next;
  }

  if(body != NULL)
    base_trim(body);
}

const char *
chatbot_base_body(const chatbot_base_t *b, const char *name)
{
  if(b == NULL || name == NULL)
    return(NULL);

  for(size_t i = 0; i < b->n; i++)
  {
    if(strcmp(b->blk[i].name, name) == 0)
      return(b->blk[i].body);
  }

  return(NULL);
}

// Does `body` still contain every token in the space-separated `tokens`?
static bool
base_has_tokens(const char *body, const char *tokens, char *missing,
    size_t missing_sz)
{
  const char *p = tokens;

  while(*p != '\0')
  {
    char        tok[CHATBOT_BASE_TOKEN_SZ];
    const char *start;
    size_t      len;

    while(*p == ' ') p++;

    if(*p == '\0')
      break;

    start = p;

    while(*p != '\0' && *p != ' ') p++;

    len = (size_t)(p - start);

    if(len >= sizeof(tok))
      len = sizeof(tok) - 1;

    memcpy(tok, start, len);
    tok[len] = '\0';

    if(strstr(body, tok) == NULL)
    {
      strlcpy(missing, tok, missing_sz);
      return(false);
    }
  }

  return(true);
}

chatbot_base_t *
chatbot_base_parse(char *raw)
{
  chatbot_base_t *b;

  if(raw == NULL)
    return(NULL);

  b = mem_alloc("chatbot", "base", sizeof(*b));
  memset(b, 0, sizeof(*b));
  b->raw = raw;

  base_parse(b, raw);

  return(b);
}

bool
chatbot_base_validate(const chatbot_base_t *b, char *err, size_t err_sz)
{
  if(err != NULL && err_sz > 0)
    err[0] = '\0';

  for(size_t i = 0; i < sizeof(base_required) / sizeof(base_required[0]); i++)
  {
    const char *body = chatbot_base_body(b, base_required[i].name);
    char        missing[CHATBOT_BASE_TOKEN_SZ];

    if(body == NULL || body[0] == '\0')
    {
      snprintf(err, err_sz, "no [[%s]] block", base_required[i].name);
      return(false);
    }

    if(base_required[i].tokens != NULL
        && !base_has_tokens(body, base_required[i].tokens, missing,
              sizeof(missing)))
    {
      snprintf(err, err_sz, "[[%s]] has lost %s, which the runtime"
          " substitutes", base_required[i].name, missing);
      return(false);
    }
  }

  return(true);
}

chatbot_base_t *
chatbot_base_load(void)
{
  chatbot_base_t *b;
  char           *raw;
  char            path[CHATBOT_PERSONALITY_PATH_SZ];
  char            dir[PATH_MAX];
  char            err[160];
  FILE           *fp;
  size_t          n;

  if(!chatbot_personality_path(dir, sizeof(dir)))
    return(NULL);

  if(snprintf(path, sizeof(path), "%s/%s.txt", dir,
        CHATBOT_BASE_STEM) >= (int)sizeof(path))
  {
    clam(CLAM_WARN, "chatbot", "base prompt path too long under '%s'", dir);
    return(NULL);
  }

  fp = fopen(path, "rb");

  if(fp == NULL)
  {
    clam(CLAM_WARN, "chatbot", "cannot read base prompt '%s': %s",
        path, strerror(errno));
    return(NULL);
  }

  raw = mem_alloc("chatbot", "basefile", CHATBOT_PERSONALITY_BODY_SZ);
  n   = fread(raw, 1, CHATBOT_PERSONALITY_BODY_SZ - 1, fp);
  raw[n] = '\0';
  fclose(fp);

  b = chatbot_base_parse(raw);

  if(!chatbot_base_validate(b, err, sizeof(err)))
  {
    // Loud and specific: every bot on this daemon is now running on
    // compiled defaults, and the only way anyone learns that is here.
    clam(CLAM_WARN, "chatbot",
        "base prompt '%s': %s — not loaded, every bot falls back to the"
        " compiled prompt until this is fixed", path, err);
    chatbot_base_free(b);
    return(NULL);
  }

  return(b);
}

void
chatbot_base_free(chatbot_base_t *b)
{
  if(b == NULL) return;

  mem_free(b->raw);
  mem_free(b);
}

size_t
chatbot_base_render(const chatbot_base_t *b, const char *block,
    const chatbot_base_tok_t *toks, char *dst, size_t dst_sz)
{
  const char *body;
  const char *p;
  size_t      o = 0;

  if(dst == NULL || dst_sz == 0)
    return(0);

  dst[0] = '\0';
  body   = chatbot_base_body(b, block);

  if(body == NULL)
    return(0);

  for(p = body; *p != '\0'; )
  {
    const char *val = NULL;
    size_t      tok_len = 0;

    // A '$' followed by an uppercase run is a candidate token; anything
    // else — a lone '$', a price, "$5" — is prose and copies through.
    if(*p == '$' && isupper((unsigned char)p[1]) && toks != NULL)
    {
      size_t run = 1;

      while(isupper((unsigned char)p[run]) || p[run] == '_') run++;

      for(size_t i = 0; toks[i].name != NULL; i++)
      {
        size_t nl = strlen(toks[i].name);

        if(nl == run && strncmp(p, toks[i].name, nl) == 0)
        {
          val     = (toks[i].value != NULL) ? toks[i].value : "";
          tok_len = run;
          break;
        }
      }
    }

    if(val != NULL)
    {
      size_t vl = strlen(val);

      if(o + vl >= dst_sz)
        break;

      memcpy(dst + o, val, vl);
      o += vl;
      p += tok_len;
      continue;
    }

    if(o + 1 >= dst_sz)
      break;

    dst[o++] = *p++;
  }

  dst[o] = '\0';

  return(o);
}

// Rotor for chatbot_base_pick. Relaxed is the whole requirement: two
// threads landing on the same line is a repeated fallback, not a bug,
// and the counter exists only so the room does not hear one line
// forever.
static atomic_uint base_rotor;

const char *
chatbot_base_pick(const chatbot_base_t *b, const char *name, char *dst,
    size_t dst_sz)
{
  const char *body;
  const char *p;
  unsigned    lines = 0;
  unsigned    want;

  if(dst == NULL || dst_sz == 0)
    return(NULL);

  dst[0] = '\0';
  body   = chatbot_base_body(b, name);

  if(body == NULL || body[0] == '\0')
    return(NULL);

  for(p = body; *p != '\0'; p++)
  {
    if(*p == '\n')
      lines++;
  }

  lines++;   // the last line carries no terminator

  want = atomic_fetch_add_explicit(&base_rotor, 1, memory_order_relaxed)
         % lines;

  for(p = body; want > 0; p++)
  {
    if(*p == '\0')
      break;

    if(*p == '\n')
      want--;
  }

  {
    const char *eol = strchr(p, '\n');
    size_t      len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

    if(len >= dst_sz)
      len = dst_sz - 1;

    memcpy(dst, p, len);
    dst[len] = '\0';
  }

  return(dst[0] != '\0' ? dst : NULL);
}
