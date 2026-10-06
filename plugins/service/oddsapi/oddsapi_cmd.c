// botmanager — MIT
// The oddsapi plugin's command surface. `show odds` reads only local
// state — the quota as the API last described it, and the cache — so it
// answers synchronously and costs nothing.
#define ODDSAPI_INTERNAL
#define ODDSAPI_CMD_INTERNAL
#include "oddsapi.h"
#include "oddsapi_cmd.h"

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

// Append one padded cell. Color markers count zero columns, so a cell
// may carry them.
static void
oddsapi_cell(char *line, size_t cap, const char *text, int width, bool right)
{
  char cell[ODDSAPI_CMD_REPLY_SZ];

  strlcpy(cell, text, sizeof(cell));

  if(right)
    display_align_right(cell, sizeof(cell), width);

  else
    display_align_left(cell, sizeof(cell), width);

  display_cat(line, cap, cell);
}

static void
oddsapi_age(time_t then, time_t now, char *out, size_t cap)
{
  if(then == 0)
    strlcpy(out, "—", cap);

  else
    util_fmt_duration(now - then, out, cap);
}

// Fields in a comma-separated list, empty ones not counted.
static uint32_t
oddsapi_csv_count(const char *csv)
{
  uint32_t n    = 0;
  bool     word = false;

  for(; *csv != '\0'; csv++)
  {
    if(*csv == ',')
      word = false;

    else if(*csv != ' ' && !word)
    {
      word = true;
      n++;
    }
  }

  return(n);
}

// ----------------------------------------------------------------------
// show odds
// ----------------------------------------------------------------------

static void
oddsapi_show_cmd(const cmd_ctx_t *ctx)
{
  oddsapi_quota_t     q;
  oddsapi_cache_row_t rows[ODDSAPI_CACHE_SZ];
  uint32_t            n     = oddsapi_cache_rows(rows, ODDSAPI_CACHE_SZ);
  const char         *books = kv_get_str("plugin.oddsapi.bookmakers");
  time_t              now   = time(NULL);
  char                line[ODDSAPI_CMD_REPLY_SZ];
  char                rule[DISPLAY_RULE_SZ(DISPLAY_COLS)];
  char                age[UTIL_DURATION_SZ];
  char                backoff[UTIL_DURATION_SZ];
  char                books_txt[24];

  oddsapi_quota_get(&q);

  if(q.remaining < 0)
    snprintf(line, sizeof(line), CLR_BOLD "🎲 ODDS API" CLR_RESET
        "   credits " CLR_GRAY "— (no call yet)" CLR_RESET);

  else
  {
    oddsapi_age(q.seen, now, age, sizeof(age));
    snprintf(line, sizeof(line), CLR_BOLD "🎲 ODDS API" CLR_RESET
        "   credits " CLR_BOLD "%d" CLR_RESET " left · %d used · "
        "last call %d · seen %s ago", q.remaining, q.used, q.last, age);
  }

  cmd_reply(ctx, line);

  if(q.backoff_left > 0)
    util_fmt_duration((time_t)q.backoff_left, backoff, sizeof(backoff));

  else
    strlcpy(backoff, "none", sizeof(backoff));

  if(books != NULL && books[0] != '\0')
    snprintf(books_txt, sizeof(books_txt), "%u", oddsapi_csv_count(books));

  else
    strlcpy(books_txt, "us region", sizeof(books_txt));

  snprintf(line, sizeof(line), "reserve %u · backoff %s · books %s",
      (uint32_t)kv_get_uint("plugin.oddsapi.quota_reserve"), backoff,
      books_txt);
  cmd_reply(ctx, line);

  if(n == 0)
  {
    cmd_reply(ctx, CLR_GRAY "cache empty" CLR_RESET);
    return;
  }

  line[0] = '\0';
  oddsapi_cell(line, sizeof(line), CLR_GRAY "cache", ODDSAPI_SHOW_KEY_W,
      false);
  oddsapi_cell(line, sizeof(line), "age", ODDSAPI_SHOW_AGE_W, true);
  oddsapi_cell(line, sizeof(line), "items", ODDSAPI_SHOW_ITEMS_W, true);
  display_cat(line, sizeof(line), "  state" CLR_RESET);
  cmd_reply(ctx, line);

  rule[0] = '\0';
  display_rule(rule, sizeof(rule), DISPLAY_COLS);
  cmd_reply(ctx, rule);

  for(uint32_t i = 0; i < n; i++)
  {
    char key[ODDSAPI_CACHE_KEY_SZ + 4];
    char items[16];

    // Budget one column for the mark and one for the gutter.
    display_fit(rows[i].key, ODDSAPI_SHOW_KEY_W - 2, key, sizeof(key), "…");
    oddsapi_age(rows[i].fetched, now, age, sizeof(age));
    snprintf(items, sizeof(items), "%u", rows[i].items);

    line[0] = '\0';
    oddsapi_cell(line, sizeof(line), key, ODDSAPI_SHOW_KEY_W, false);
    oddsapi_cell(line, sizeof(line), age, ODDSAPI_SHOW_AGE_W, true);
    oddsapi_cell(line, sizeof(line), rows[i].fetched != 0 ? items : "—",
        ODDSAPI_SHOW_ITEMS_W, true);

    if(rows[i].inflight)
      display_cat(line, sizeof(line), "  " CLR_YELLOW "fetching" CLR_RESET);

    cmd_reply(ctx, line);
  }
}

// ----------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------

static const cmd_decl_t show_odds_decl = {
  .module      = ODDSAPI_CTX,
  .name        = "odds",
  .usage       = "show odds",
  .description = "The Odds API's credit quota and what the odds cache holds.",
  .help_long   = "Credits come from the headers of the last reply of any "
                 "kind, free calls included, so they are unknown until the "
                 "plugin has made one. Each cache row is one league's lines, "
                 "game list, scores or futures, with its age. Reads only "
                 "local state; costs nothing.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = oddsapi_show_cmd,
  .parent_path = "show",
};

bool
oddsapi_cmd_register(void)
{
  if(cmd_register(&show_odds_decl) != SUCCESS)
    return(FAIL);

  clam(CLAM_INFO, ODDSAPI_CMD_CTX, "show odds registered");
  return(SUCCESS);
}

void
oddsapi_cmd_unregister(void)
{
  cmd_unregister_path("show/odds");
  clam(CLAM_INFO, ODDSAPI_CMD_CTX, "show odds unregistered");
}
