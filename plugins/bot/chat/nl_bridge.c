// botmanager — MIT
// Slash-line parser for LLM-produced replies; pure (allowlist/permit elsewhere).

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include <ctype.h>
#include <string.h>

// Line-scoped by design (CHAT-BRIDGE-SWALLOW-1): the first line that IS
// a slash command wins, wherever it sits in the reply, and args end at
// that line's end. The old whole-text parse had both failure arms — a
// persona framing line before the command swallowed it entirely (the
// wire suppressor catches slash lines anywhere, but extraction insisted
// on first-non-blank), and prose after the command was swept into args
// with its embedded newlines, poisoning an otherwise correct dispatch.
bool
chatbot_nl_extract_cmd(const char *text, char *cmd_out, size_t cmd_sz,
    char *args_out, size_t args_sz)
{
  const char *p;

  if(cmd_out != NULL && cmd_sz > 0) cmd_out[0] = '\0';
  if(args_out != NULL && args_sz > 0) args_out[0] = '\0';

  if(text == NULL || cmd_out == NULL || args_out == NULL
      || cmd_sz == 0 || args_sz == 0)
    return(false);

  for(p = text; *p != '\0'; )
  {
    const char *eol = strchr(p, '\n');
    const char *end = (eol != NULL) ? eol : p + strlen(p);
    const char *q   = p;

    while(q < end && (*q == ' ' || *q == '\t')) q++;

    if(q < end && *q == '/' && isalpha((unsigned char)q[1]))
    {
      const char *cmd_start;
      size_t      cmd_len;
      size_t      arg_len;

      q++;
      cmd_start = q;

      while(q < end
          && (isalnum((unsigned char)*q) || *q == '_' || *q == '-'))
        q++;

      cmd_len = (size_t)(q - cmd_start);

      if(cmd_len == 0 || cmd_len >= cmd_sz)
        return(false);

      memcpy(cmd_out, cmd_start, cmd_len);
      cmd_out[cmd_len] = '\0';

      while(q < end && (*q == ' ' || *q == '\t')) q++;

      // Args: the rest of THIS line only, trailing whitespace/CR
      // stripped. Later lines are the model's own prose and belong to
      // the wire, not to the command.
      arg_len = (size_t)(end - q);

      while(arg_len > 0
          && (q[arg_len - 1] == '\r' || q[arg_len - 1] == ' '
              || q[arg_len - 1] == '\t'))
        arg_len--;

      if(arg_len >= args_sz) arg_len = args_sz - 1;
      memcpy(args_out, q, arg_len);
      args_out[arg_len] = '\0';

      return(true);
    }

    if(eol == NULL)
      break;

    p = eol + 1;
  }

  return(false);
}
