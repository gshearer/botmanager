// botmanager — MIT
// Chat bot fact-extractor prompt templates and response parsing.
#define EXTRACT_INTERNAL
#include "extract.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// System prompt: stable across sweeps. Keeps tone + schema contract.
static const char *extract_system_prompt =
    "You extract durable facts about chat participants from a transcript "
    "you did not write. You are not a chatbot. Output ONLY compact JSON "
    "matching this schema:\n"
    "{\"facts\":[{\"dossier_id\":<int>,\"kind\":\"preference|attribute|"
    "relation|event|opinion|freeform\",\"fact_key\":\"<key>\","
    "\"fact_value\":\"<value>\",\"confidence\":<float 0..1>}]}\n"
    "Rules:\n"
    "- dossier_id MUST appear in the participants list.\n"
    "- For facts about one dossier's attitude/behavior toward another, "
    "use kind=\"relation\" and fact_key=\"toward:<other_dossier_id>\".\n"
    "- Prefer few high-confidence facts over many speculative ones.\n"
    "- If nothing is worth recording, output {\"facts\":[]}.\n"
    "- No prose, no markdown fences, no commentary.\n"
    "\n"
    "The JSON object MAY also contain an \"aliases\" array describing "
    "informal nicknames observed in the transcript that refer to a "
    "participant by a shortened or non-canonical name. Schema:\n"
    "\"aliases\":[{\"dossier_id\":<int>,\"alias\":\"<str>\","
    "\"confidence\":<float 0..1>}]\n"
    "Alias rules:\n"
    "- Emit an alias ONLY when the transcript clearly shows a speaker "
    "referring to a participant by a shortened or informal name (e.g. "
    "\"did jaer finish that\" when the participant is \"Jaerchom\").\n"
    "- Do NOT emit the participant's own current display name.\n"
    "- alias MUST be 3-32 alphanumeric characters (no spaces, no "
    "punctuation, no hyphens).\n"
    "- dossier_id MUST appear in the participants list above.\n"
    "- Prefer few high-confidence aliases. If unsure, omit.\n"
    "- If no aliases are clear, output \"aliases\":[].";

const char *
extract_prompt_system(void)
{
  return(extract_system_prompt);
}

// Append formatted text to buf; returns true on overflow (stops writing).
static bool
bufprintf(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
  int n;
  va_list ap;

  if(*pos >= cap)
    return(true);

  va_start(ap, fmt);
  n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
  va_end(ap);

  if(n < 0)
    return(true);

  if((size_t)n >= cap - *pos)
  {
    *pos = cap - 1;
    buf[*pos] = '\0';
    return(true);
  }

  *pos += (size_t)n;
  return(false);
}

size_t
extract_prompt_build(const extract_participant_t *parts, size_t n_parts,
    const mem_msg_t *msgs, size_t n_msgs,
    char *out, size_t out_sz)
{
  size_t pos;

  if(out == NULL || out_sz < 64)
    return(0);

  if(parts == NULL || msgs == NULL || n_parts == 0 || n_msgs == 0)
    return(0);

  pos = 0;
  out[0] = '\0';

  if(bufprintf(out, out_sz, &pos, "Participants:\n"))
    return(pos);

  for(size_t i = 0; i < n_parts; i++)
  {
    const char *role = parts[i].role == EXTRACT_ROLE_SENDER
        ? "sender" : "mentioned";

    if(bufprintf(out, out_sz, &pos,
          "  %" PRId64 " %-16s %s\n",
          parts[i].dossier_id,
          parts[i].display_label[0] != '\0'
              ? parts[i].display_label : "(anon)",
          role))
      return(pos);
  }

  if(bufprintf(out, out_sz, &pos, "\nMessages:\n"))
    return(pos);

  for(size_t i = 0; i < n_msgs; i++)
  {
    const char *text = msgs[i].text;

    // Detect IRC /me convention: "* <nick> <action-text>"
    bool   is_action = (text[0] == '*' && text[1] == ' ');
    const char *body = text;
    char  action_tag[8] = "";

    if(is_action)
    {
      const char *sp;

      snprintf(action_tag, sizeof(action_tag), "/me ");
      // Skip "* <nick> " prefix to leave just the action body.
      sp = strchr(text + 2, ' ');
      body = sp != NULL ? sp + 1 : text;
    }

    if(bufprintf(out, out_sz, &pos,
          "  [%" PRId64 "] %s%s\n",
          msgs[i].dossier_id, action_tag, body))
      return(pos);
  }

  return(pos);
}
