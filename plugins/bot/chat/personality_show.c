// botmanager — MIT
// /show personalities: scans bot.chat.personalitypath, renders a table row each.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "cmd.h"
#include "colors.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

// Per-scan accumulator — each visited file becomes one cmd_reply row.
typedef struct
{
  const cmd_ctx_t *ctx;
  size_t           count;
} ps_state_t;

static void
ps_emit_row(const char *stem, void *data)
{
  ps_state_t      *st = (ps_state_t *)data;
  persona_header_t hdr;
  char             line[512];

  chatbot_personality_read_header(stem, &hdr);

  if(hdr.ok)
    snprintf(line, sizeof(line),
        "  " CLR_BOLD "%-24s" CLR_RESET " %s",
        hdr.name[0] ? hdr.name : stem,
        hdr.description[0] ? hdr.description : "");

  else
    snprintf(line, sizeof(line),
        "  " CLR_RED "%-24s" CLR_RESET " "
        CLR_RED "parse error:" CLR_RESET " %s",
        stem, hdr.err);

  cmd_reply(st->ctx, line);
  st->count++;
}

static void
cmd_show_personalities(const cmd_ctx_t *ctx)
{
  char       dir[PATH_MAX];
  char       hdr[256];
  ps_state_t st = { .ctx = ctx, .count = 0 };

  if(!chatbot_personality_path(dir, sizeof(dir)))
  {
    cmd_reply(ctx, "bot.chat.personalitypath not set");
    return;
  }

  snprintf(hdr, sizeof(hdr),
      CLR_CYAN "personalities in" CLR_RESET " %s", dir);
  cmd_reply(ctx, hdr);

  cmd_reply(ctx,
      "  " CLR_BOLD
      "NAME                     DESCRIPTION"
      CLR_RESET);

  chatbot_personality_scan(ps_emit_row, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (no personalities found)");

  else
  {
    char footer[64];

    snprintf(footer, sizeof(footer), "%zu personalit%s",
        st.count, st.count == 1 ? "y" : "ies");
    cmd_reply(ctx, footer);
  }
}

bool
chatbot_personality_show_register(void)
{
  return(cmd_register("chat", "personalities",
      "show personalities",
      "List personality files in bot.chat.personalitypath",
      "Lists every *.txt file in the configured personalitypath,\n"
      "parsing each file's frontmatter for name, version, and\n"
      "description. Personalities with parse errors are shown in\n"
      "red with the error message. No file bodies are read.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_personalities, NULL, "show", NULL,
      NULL, 0,
      NULL, NULL));
}
