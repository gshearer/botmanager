// botmanager — MIT
// /show contracts: scans bot.chat.contractpath, parallels personality_show.c.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "cmd.h"
#include "colors.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
  const cmd_ctx_t *ctx;
  size_t           count;
} cs_state_t;

static void
cs_emit_row(const char *stem, void *data)
{
  cs_state_t      *st = (cs_state_t *)data;
  persona_header_t hdr;
  char             line[512];

  chatbot_contract_read_header(stem, &hdr);

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
cmd_show_contracts(const cmd_ctx_t *ctx)
{
  char       dir[PATH_MAX];
  char       hdr[256];
  cs_state_t st = { .ctx = ctx, .count = 0 };

  if(!chatbot_contract_path(dir, sizeof(dir)))
  {
    cmd_reply(ctx, "bot.chat.contractpath not set");
    return;
  }

  snprintf(hdr, sizeof(hdr),
      CLR_CYAN "contracts in" CLR_RESET " %s", dir);
  cmd_reply(ctx, hdr);

  cmd_reply(ctx,
      "  " CLR_BOLD
      "NAME                     DESCRIPTION"
      CLR_RESET);

  chatbot_contract_scan(cs_emit_row, &st);

  if(st.count == 0)
    cmd_reply(ctx, "  (no contracts found)");

  else
  {
    char footer[64];

    snprintf(footer, sizeof(footer), "%zu contract%s",
        st.count, st.count == 1 ? "" : "s");
    cmd_reply(ctx, footer);
  }
}

bool
chatbot_contract_show_register(void)
{
  return(cmd_register("chat", "contracts",
      "show contracts",
      "List output contract files in bot.chat.contractpath",
      "Lists every *.txt file in the configured contractpath,\n"
      "parsing each file's frontmatter for name and description.\n"
      "Contracts with parse errors are shown in red with the error\n"
      "message. No file bodies are read.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_contracts, NULL, "show", NULL,
      NULL, 0,
      NULL, NULL));
}
