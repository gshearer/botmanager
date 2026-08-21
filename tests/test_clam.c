// botmanager — MIT
// Cases for clam()'s one-line invariant: every writer that frames a
// message appends the newline itself, so the message may hold none.
#include "test.h"
#include "clam.h"

#include <string.h>

// The context this suite emits under. clam_subscribe() logs a line of
// its own, and the log file subscriber is not registered here, so the
// capture keeps only what a case sent.
#define CASE_CTX  "forge"

static char captured[CLAM_MSG_SZ];

static void
capture_cb(const clam_msg_t *m)
{
  if(strcmp(m->context, CASE_CTX) == 0)
    strlcpy(captured, m->msg, sizeof captured);
}

// A %s argument arrives from wherever a caller got it: PQerrorMessage
// ends every message in a newline and carries DETAIL lines behind it,
// and the llm prompt dump at DEBUG5 carries retrieved web pages.
static const struct
{
  const char *name;
  const char *arg;
  const char *want;
} flatten_cases[] = {
  { "plain text is untouched", "connected to drow", "connected to drow" },
  { "an interior LF becomes a space", "ERROR: syntax\nLINE 1: SELECT",
    "ERROR: syntax LINE 1: SELECT" },
  { "a lone CR becomes a space", "over\rwrite", "over write" },
  { "a trailing break is the writer's own framing", "PQ said no\n",
    "PQ said no" },
  { "a trailing CRLF goes whole", "line\r\n", "line" },
  { "trailing breaks go, interior ones stay as spaces", "a\nb\n\n", "a b" },
  { "breaks alone leave nothing", "\n\r\n", "" },
  // The forgery the row is about: a stamped, severity-tagged line of
  // its own, indistinguishable from one clam_file_cb wrote.
  { "a log entry cannot be forged",
    "page\n2026-08-21 09:00:00  WARN irc     kill sent",
    "page 2026-08-21 09:00:00  WARN irc     kill sent" },
  // Narrow on purpose: these are display and a clam destination on IRC
  // is meant to receive them.
  { "display bytes are not framing", "\x02" "bold" "\x03" "04red",
    "\x02" "bold" "\x03" "04red" },
};

int
main(void)
{
  char big[CLAM_MSG_SZ * 2];

  clam_init();
  clam_subscribe("capture", CLAM_DEBUG5, NULL, capture_cb);

  for(size_t i = 0; i < sizeof(flatten_cases) / sizeof(flatten_cases[0]); i++)
  {
    captured[0] = '\0';
    clam(CLAM_INFO, CASE_CTX, "%s", flatten_cases[i].arg);

    test_check_str("flatten", flatten_cases[i].name,
        flatten_cases[i].want, captured);
  }

  // Overlong, and ending on a non-break so the trailing strip takes
  // nothing: the walk must stop at the NUL vsnprintf wrote, not at the
  // length it wished it had. strcspn finding no break at 999 is both
  // halves of that — the message is whole and it is one line.
  for(size_t i = 0; i < sizeof(big) - 1; i++)
    big[i] = (i % 2 == 0) ? 'a' : '\n';

  big[sizeof(big) - 1] = '\0';

  captured[0] = '\0';
  clam(CLAM_INFO, CASE_CTX, "%s", big);

  test_check_sz("flatten", "an oversize message is flattened to the cap",
      CLAM_MSG_SZ - 1, strcspn(captured, "\r\n"));

  return(test_report("clam"));
}
