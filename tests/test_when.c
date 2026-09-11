// botmanager — MIT
// Cases for when.h, whose every failure is silent: a phrase read wrong
// stores a reminder for the wrong moment, and nothing says so until it
// fires then — or never.
#include "test.h"
#include "when.h"

#include <stdio.h>

#define LOCAL "America/New_York"

// Every field the grammar writes, in one line, so a row states the
// whole outcome and a diff shows which part moved.
static void
render(const when_t *w, const char *rest, char *dst, size_t cap)
{
  static const char *const kinds[] = { "delay", "back", "clock", "every",
                                       "weekly" };

  snprintf(dst, cap, "%s secs=%llu days=%02x lo=%u hi=%u min=%u tz=%s%s"
      " rest='%s'", kinds[w->kind], (unsigned long long)w->secs, w->days,
      w->lo, w->hi, w->minute, w->tz, w->tz_named ? "!" : "", rest);
}

static const struct
{
  const char *name;
  const char *text;
  const char *local;
  const char *want;     // the render, or "FAIL"
} parse_cases[] = {
  { "compact delay",          "15m water",  LOCAL,
    "delay secs=900 days=00 lo=0 hi=0 min=0 tz= rest='water'" },
  { "compound delay is whole", "1h30m call mom", LOCAL,
    "delay secs=5400 days=00 lo=0 hi=0 min=0 tz= rest='call mom'" },
  { "unknown unit refused",    "1mo pay rent", LOCAL, "FAIL" },
  { "number and unit as words", "15 minutes to stretch", LOCAL,
    "delay secs=900 days=00 lo=0 hi=0 min=0 tz= rest='to stretch'" },
  { "in prefix",               "in 2 hours laundry", LOCAL,
    "delay secs=7200 days=00 lo=0 hi=0 min=0 tz= rest='laundry'" },
  { "bare number is seconds",  "5 things to buy", LOCAL,
    "delay secs=5 days=00 lo=0 hi=0 min=0 tz= rest='things to buy'" },
  { "back",                    "back look at the PR", LOCAL,
    "back secs=0 days=00 lo=0 hi=0 min=0 tz= rest='look at the PR'" },
  { "at a time, next one",     "at 3pm call", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York rest='call'" },
  { "am/pm as its own word",   "3 p.m. call", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York rest='call'" },
  { "tomorrow first",          "tomorrow at 9am dentist", LOCAL,
    "clock secs=0 days=7f lo=1 hi=1 min=540 tz=America/New_York rest='dentist'" },
  { "tomorrow last",           "9am tomorrow dentist", LOCAL,
    "clock secs=0 days=7f lo=1 hi=1 min=540 tz=America/New_York rest='dentist'" },
  { "a weekday, 24-hour",      "friday 15:00 standup", LOCAL,
    "clock secs=0 days=20 lo=0 hi=7 min=900 tz=America/New_York rest='standup'" },
  { "on after the time",       "at 3pm on monday EST prep", LOCAL,
    "clock secs=0 days=02 lo=0 hi=7 min=900 tz=America/New_York! rest='prep'" },
  { "on that is not a day",    "at 3pm on the call", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York rest='on the call'" },
  { "the example that started it",
    "every monday at 3:00 PM EST water the plants", LOCAL,
    "weekly secs=0 days=02 lo=0 hi=0 min=900 tz=America/New_York!"
    " rest='water the plants'" },
  { "weekdays, 24-hour",       "every weekday at 8:30 weather 45069", LOCAL,
    "weekly secs=0 days=3e lo=0 hi=0 min=510 tz=America/New_York"
    " rest='weather 45069'" },
  { "a comma list",            "every mon,wed,fri at 9am gym", LOCAL,
    "weekly secs=0 days=2a lo=0 hi=0 min=540 tz=America/New_York rest='gym'" },
  { "and between days",        "every monday and thursday at 7pm PT trash",
    LOCAL, "weekly secs=0 days=12 lo=0 hi=0 min=1140"
    " tz=America/Los_Angeles! rest='trash'" },
  { "an interval",             "every 2h stretch", LOCAL,
    "every secs=7200 days=00 lo=0 hi=0 min=0 tz= rest='stretch'" },
  { "daily at noon",           "daily at noon lunch", LOCAL,
    "weekly secs=0 days=7f lo=0 hi=0 min=720 tz=America/New_York rest='lunch'" },
  { "zone word and 'time'",    "at 3pm eastern time meeting", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York! rest='meeting'" },
  { "an IANA zone",            "at 3pm America/Chicago call", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/Chicago! rest='call'" },
  { "a slash is not a zone",   "at 3pm 50/50 split", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York"
    " rest='50/50 split'" },
  { "lower-case pt is a word", "at 3pm pt session", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York"
    " rest='pt session'" },
  { "at a bare hour past 12",  "at 15 call", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=America/New_York rest='call'" },
  { "12am is midnight",        "at 12am x", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=0 tz=America/New_York rest='x'" },
  { "12pm is noon",            "at 12pm x", LOCAL,
    "clock secs=0 days=7f lo=0 hi=1 min=720 tz=America/New_York rest='x'" },
  { "at 3 is a coin toss",     "at 3 call", LOCAL, "FAIL" },
  { "13pm",                    "at 13pm call", LOCAL, "FAIL" },
  { "at with no time",         "at the store", LOCAL, "FAIL" },
  { "a repeat owes a time",    "every monday water", LOCAL, "FAIL" },
  { "a day owes a time",       "tomorrow water", LOCAL, "FAIL" },
  { "every what",              "every so often", LOCAL, "FAIL" },
  { "no moment at all",        "water the plants", LOCAL, "FAIL" },
  { "nothing",                 "", LOCAL, "FAIL" },
  { "no local zone refuses",   "at 3pm call", NULL, "FAIL" },
  { "no local zone, named ok", "at 3pm UTC call", NULL,
    "clock secs=0 days=7f lo=0 hi=1 min=900 tz=UTC! rest='call'" },
  { "a delay needs no zone",   "20m call", NULL,
    "delay secs=1200 days=00 lo=0 hi=0 min=0 tz= rest='call'" },
};

static const struct
{
  const char *name;
  const char *tok;
  size_t      want;
} duration_cases[] = {
  { "seconds",           "90s",          90     },
  { "minutes",           "15m",          900    },
  { "compound",          "1h30m",        5400   },
  { "days",              "2d",           172800 },
  { "weeks",             "1w",           604800 },
  { "unit word",         "3hours",       10800  },
  { "bare seconds",      "45",           45     },
  { "unknown unit",      "1mo",          0      },
  { "trailing junk",     "5mx",          0      },
  { "no number",         "m",            0      },
  { "a sign",            "-5m",          0      },
  { "absurdly long",     "99999999999s", 0      },
};

static const struct
{
  const char *name;
  when_t      w;
  const char *want;
} describe_cases[] = {
  { "one day",   { .kind = WHEN_WEEKLY, .days = 0x02, .minute = 900,
                   .tz = LOCAL }, "every Monday at 3:00 PM" },
  { "two days",  { .kind = WHEN_WEEKLY, .days = 0x12, .minute = 1140,
                   .tz = LOCAL }, "every Monday and Thursday at 7:00 PM" },
  { "three",     { .kind = WHEN_WEEKLY, .days = 0x2a, .minute = 540,
                   .tz = LOCAL }, "every Monday, Wednesday and Friday at 9:00 AM" },
  { "weekday",   { .kind = WHEN_WEEKLY, .days = 0x3e, .minute = 510,
                   .tz = LOCAL }, "every weekday at 8:30 AM" },
  { "weekend",   { .kind = WHEN_WEEKLY, .days = 0x41, .minute = 0,
                   .tz = LOCAL }, "every Saturday and Sunday at 12:00 AM" },
  { "every day", { .kind = WHEN_WEEKLY, .days = 0x7f, .minute = 720,
                   .tz = LOCAL }, "every day at 12:00 PM" },
  { "elsewhere", { .kind = WHEN_WEEKLY, .days = 0x02, .minute = 900,
                   .tz = "America/Los_Angeles" },
                 "every Monday at 3:00 PM (America/Los_Angeles)" },
  { "interval",  { .kind = WHEN_EVERY, .secs = 5400 }, "every 1h30m" },
  { "one-shot",  { .kind = WHEN_CLOCK, .days = 0x7f, .minute = 900,
                   .tz = LOCAL }, "" },
};

int
main(void)
{
  char got[256];

  for(size_t i = 0; i < sizeof(parse_cases) / sizeof(parse_cases[0]); i++)
  {
    when_t      w;
    const char *rest = NULL;
    const char *why  = NULL;

    if(when_parse(parse_cases[i].text, parse_cases[i].local, &w, &rest,
        &why) == SUCCESS)
      render(&w, rest, got, sizeof(got));

    else
      snprintf(got, sizeof(got), "FAIL");

    test_check_str("parse", parse_cases[i].name, parse_cases[i].want, got);

    // A refusal is spoken to a person, so it must always have words.
    if(strcmp(got, "FAIL") == 0)
      test_check_bool("parse-why", parse_cases[i].name, true,
          why != NULL && why[0] != '\0');
  }

  for(size_t i = 0; i < sizeof(duration_cases) / sizeof(duration_cases[0]);
      i++)
    test_check_sz("duration", duration_cases[i].name, duration_cases[i].want,
        (size_t)when_parse_duration(duration_cases[i].tok));

  for(size_t i = 0; i < sizeof(describe_cases) / sizeof(describe_cases[0]);
      i++)
  {
    when_describe_repeat(&describe_cases[i].w, LOCAL, got, sizeof(got));
    test_check_str("describe", describe_cases[i].name,
        describe_cases[i].want, got);
  }

  return(test_report("when"));
}
