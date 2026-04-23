// botmanager — MIT
// Slash-line parser for LLM-produced replies; pure (allowlist/permit elsewhere).

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include <ctype.h>
#include <string.h>

bool
chatbot_nl_extract_cmd(const char *text, char *cmd_out, size_t cmd_sz,
    char *args_out, size_t args_sz)
{
  size_t arg_len;
  size_t cmd_len;
  const char *cmd_start;
  const char *p;

  if(cmd_out != NULL && cmd_sz > 0) cmd_out[0] = '\0';
  if(args_out != NULL && args_sz > 0) args_out[0] = '\0';

  if(text == NULL || cmd_out == NULL || args_out == NULL
      || cmd_sz == 0 || args_sz == 0)
    return(false);

  p = text;

  while(*p == ' ' || *p == '\t') p++;

  if(*p != '/') return(false);
  p++;

  if(!isalpha((unsigned char)*p)) return(false);

  cmd_start = p;

  while(isalnum((unsigned char)*p) || *p == '_' || *p == '-') p++;

  cmd_len = (size_t)(p - cmd_start);

  if(cmd_len == 0 || cmd_len >= cmd_sz) return(false);

  memcpy(cmd_out, cmd_start, cmd_len);
  cmd_out[cmd_len] = '\0';

  while(*p == ' ' || *p == '\t') p++;

  // Strip trailing newlines/whitespace from args.
  arg_len = strlen(p);

  while(arg_len > 0
      && (p[arg_len - 1] == '\n' || p[arg_len - 1] == '\r'
          || p[arg_len - 1] == ' '  || p[arg_len - 1] == '\t'))
    arg_len--;

  if(arg_len >= args_sz) arg_len = args_sz - 1;
  memcpy(args_out, p, arg_len);
  args_out[arg_len] = '\0';

  return(true);
}
