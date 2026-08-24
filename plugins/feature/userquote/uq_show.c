// botmanager — MIT
// `show quotes`: a telemetry card for the quote book — size, reach, the
// span it covers, and who owns most of it. Read-only and userns-scoped,
// so it answers for exactly the book the caller's `!quote` recalls from.

#define USERQUOTE_INTERNAL
#include "userquote.h"

#include "colors.h"
#include "display.h"
#include "userns.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Card geometry, derived from the house width: one column of indent,
// three stat cells, one space between them.
#define UQ_W_INDENT  1
#define UQ_W_CELL   ((DISPLAY_COLS - UQ_W_INDENT - 2) / 3)
#define UQ_W_LABEL   11
#define UQ_W_VALUE  (UQ_W_CELL - UQ_W_LABEL)

// Leaderboard: name, bar, count. The bar takes whatever the other two
// leave, which is what keeps the card on DISPLAY_COLS.
#define UQ_W_NAME    16
#define UQ_W_COUNT   7
#define UQ_W_BAR    (DISPLAY_COLS - UQ_W_INDENT - UQ_W_NAME - UQ_W_COUNT - 2)

// A block is three bytes and the bar is padded to its full width.
#define UQ_BAR_SZ   (UQ_W_BAR * 3 + UQ_W_BAR + 1)

// ------------------------------------------------------------------ //
// Small renderers                                                     //
// ------------------------------------------------------------------ //

static void
uq_rule(char *out, size_t cap, int cols)
{
  size_t n;
  int    i;

  n = strlcpy(out, CLR_GRAY, cap);

  for(i = 0; i < cols && n + 4 < cap; i++)
  {
    memcpy(out + n, "─", 3);
    n += 3;
  }

  strlcpy(out + n, CLR_RESET, cap - n);
}

// Thousands separators. A book that has run since 1999 reads in the
// thousands, and four undifferentiated digits is the one number on the
// card nobody can take in at a glance.
static void
uq_commify(char *out, size_t cap, int64_t v)
{
  char raw[24];
  int  len;
  int  i;
  int  n = 0;

  len = snprintf(raw, sizeof(raw), "%" PRId64, v);

  for(i = 0; i < len && (size_t)n + 2 < cap; i++)
  {
    if(i > 0 && ((len - i) % 3) == 0)
      out[n++] = ',';

    out[n++] = raw[i];
  }

  out[n] = '\0';
}

// Coarse duration: the interesting part of a 27-year book is the years.
static void
uq_span_text(char *out, size_t cap, int64_t days)
{
  int64_t years = days / 365;

  if(years > 0)
    snprintf(out, cap, "%" PRId64 "y %" PRId64 "mo",
        years, (days % 365) / 30);

  else if(days >= 30)
    snprintf(out, cap, "%" PRId64 "mo", days / 30);

  else
    snprintf(out, cap, "%" PRId64 "d", days);
}

// Scaled to the leader, rounded UP: a sayer with one quote in a book of
// two thousand still gets a mark, because an empty bar beside a nonzero
// count reads as a rendering fault rather than as scale.
static void
uq_bar(char *out, size_t cap, int64_t count, int64_t max, int cols)
{
  size_t n      = 0;
  int    filled = 0;
  int    i;

  if(max > 0 && count > 0)
    filled = (int)((count * cols + max - 1) / max);

  if(filled > cols)
    filled = cols;

  for(i = 0; i < filled && n + 4 < cap; i++)
  {
    memcpy(out + n, "█", 3);
    n += 3;
  }

  out[n] = '\0';
  display_align_left(out, cap, cols);
}

// One row of three label/value pairs. The last value is not padded —
// trailing blanks buy nothing on a chat line.
static void
uq_stat_row(const cmd_ctx_t *ctx,
    const char *l1, const char *v1,
    const char *l2, const char *v2,
    const char *l3, const char *v3)
{
  char line[512];

  snprintf(line, sizeof(line),
      "%*s"
      CLR_GRAY "%-*s" CLR_RESET CLR_BOLD "%-*s" CLR_RESET " "
      CLR_GRAY "%-*s" CLR_RESET CLR_BOLD "%-*s" CLR_RESET " "
      CLR_GRAY "%-*s" CLR_RESET CLR_BOLD "%s" CLR_RESET,
      UQ_W_INDENT, "",
      UQ_W_LABEL, l1, UQ_W_VALUE, v1,
      UQ_W_LABEL, l2, UQ_W_VALUE, v2,
      UQ_W_LABEL, l3, v3);

  cmd_reply(ctx, line);
}

// ------------------------------------------------------------------ //
// show quotes                                                         //
// ------------------------------------------------------------------ //

static void
uq_show_leaderboard(const cmd_ctx_t *ctx, const uq_stats_t *st)
{
  char     line[512];
  char     bar[UQ_BAR_SZ];
  char     num[24];
  char     name[UQ_W_NAME * 4 + 8];
  uint32_t i;

  if(st->n_top == 0)
    return;

  snprintf(line, sizeof(line), "%*s" CLR_GRAY "most quoted" CLR_RESET,
      UQ_W_INDENT, "");
  cmd_reply(ctx, line);

  // top[0] is the leader — the result arrived ordered by count.
  for(i = 0; i < st->n_top; i++)
  {
    display_fit(st->top[i].name, UQ_W_NAME - 2, name, sizeof(name), "…");
    display_align_left(name, sizeof(name), UQ_W_NAME);
    uq_bar(bar, sizeof(bar), st->top[i].count, st->top[0].count, UQ_W_BAR);
    uq_commify(num, sizeof(num), st->top[i].count);

    snprintf(line, sizeof(line),
        "%*s" CLR_CYAN "%s" CLR_RESET " " CLR_BLUE "%s" CLR_RESET
        " " CLR_BOLD "%s" CLR_RESET,
        UQ_W_INDENT, "", name, bar, num);
    cmd_reply(ctx, line);
  }
}

static void
uq_cmd_show(const cmd_ctx_t *ctx)
{
  userns_t  *ns = userns_session_resolve(ctx);
  uq_stats_t st;
  char       line[512];
  char       rule[DISPLAY_COLS * 3 + 16];
  char       v_total[24];
  char       v_sayers[24];
  char       v_quoters[24];
  char       v_span[48];
  char       v_avg[32];
  char       v_max[32];
  char       v_chans[24];
  char       v_recent[32];
  char       v_unseen[48];
  char       v_busy[48];
  char       scratch[24];

  if(ns == NULL)   // the resolver already replied
    return;

  if(uq_db_stats(ns->id, &st) != SUCCESS)
  {
    cmd_reply(ctx, "Couldn't read the quote book. :~(");
    return;
  }

  if(st.total == 0)
  {
    snprintf(line, sizeof(line),
        "The quote book for " CLR_CYAN "%s" CLR_RESET
        " is empty — `quote add` starts it.", ns->name);
    cmd_reply(ctx, line);
    return;
  }

  uq_commify(v_total,   sizeof(v_total),   st.total);
  uq_commify(v_sayers,  sizeof(v_sayers),  st.sayers);
  uq_commify(v_quoters, sizeof(v_quoters), st.quoters);
  uq_commify(v_chans,   sizeof(v_chans),   st.channels);
  uq_commify(v_recent,  sizeof(v_recent),  st.recent);

  uq_span_text(scratch, sizeof(scratch), st.span_days);
  uq_commify(v_span, sizeof(v_span), st.span_days);
  snprintf(line, sizeof(line), "%s (%s days)", scratch, v_span);
  strlcpy(v_span, line, sizeof(v_span));

  uq_commify(scratch, sizeof(scratch), st.avg_len);
  snprintf(v_avg, sizeof(v_avg), "%s chars", scratch);
  uq_commify(scratch, sizeof(scratch), st.max_len);
  snprintf(v_max, sizeof(v_max), "%s chars", scratch);

  // A share, not a second raw count: "never recalled" only means
  // anything against the size of the book it sits in.
  uq_commify(scratch, sizeof(scratch), st.unseen);
  snprintf(v_unseen, sizeof(v_unseen), "%s (%" PRId64 "%%)",
      scratch, (st.unseen * 100) / st.total);

  if(st.busiest[0] != '\0')
  {
    uq_commify(scratch, sizeof(scratch), st.busiest_n);
    snprintf(v_busy, sizeof(v_busy), "%s (%s)", st.busiest, scratch);
  }

  else
    strlcpy(v_busy, "—", sizeof(v_busy));

  snprintf(line, sizeof(line),
      "📖 " CLR_BOLD "QUOTE BOOK" CLR_RESET CLR_GRAY " — " CLR_RESET
      CLR_CYAN "%s" CLR_RESET, ns->name);
  cmd_reply(ctx, line);

  uq_rule(rule, sizeof(rule), DISPLAY_COLS);
  cmd_reply(ctx, rule);

  uq_stat_row(ctx, "quotes", v_total, "sayers", v_sayers,
      "quoters", v_quoters);
  uq_stat_row(ctx, "oldest",
      (st.oldest[0] != '\0') ? st.oldest : "—", "newest",
      (st.newest[0] != '\0') ? st.newest : "—", "span", v_span);
  uq_stat_row(ctx, "channels", v_chans, "avg len", v_avg,
      "longest", v_max);
  uq_stat_row(ctx, "last 30d", v_recent, "unrecalled", v_unseen,
      "busiest", v_busy);

  cmd_reply(ctx, rule);
  uq_show_leaderboard(ctx, &st);
}

// ------------------------------------------------------------------ //
// Registration                                                        //
// ------------------------------------------------------------------ //

static const cmd_nl_example_t uq_show_examples[] = {
  { .utterance = "how many quotes do we have?", .invocation = "/show quotes" },
  { .utterance = "quote book stats",            .invocation = "/show quotes" },
};

static const cmd_nl_t uq_show_nl = {
  .when          = "User asks about the size, age or composition of the quote book.",
  .syntax        = "/show quotes",
  .slots         = NULL,
  .slot_count    = 0,
  .examples      = uq_show_examples,
  .example_count = (uint8_t)(sizeof(uq_show_examples) / sizeof(uq_show_examples[0])),
};

static const cmd_decl_t show_quotes_decl = {
  .module      = "userquote",
  .name        = "quotes",
  .usage       = "show quotes",
  .description = "Telemetry for this namespace's quote book.",
  .help_long   = "Counts, the span the book covers, how much of it has never "
                 "been recalled, and who is quoted most. Scoped to the same "
                 "namespace `quote` recalls from.",
  .group       = USERNS_GROUP_EVERYONE,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = uq_cmd_show,
  .parent_path = "show",
  .abbrev      = "\"",
  .nl          = &uq_show_nl,
};

// Abbrev `"` mirrors the root recall's, so `show "` is the whole card.
bool
uq_show_register(void)
{
  return(cmd_register(&show_quotes_decl));
}

void
uq_show_unregister(void)
{
  cmd_unregister_path("show/quotes");
}
