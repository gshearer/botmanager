#ifndef CHATBOT_WHEN_H
#define CHATBOT_WHEN_H

#include "common.h"

#include <stddef.h>

// The moment a person asks for, read off the head of what they typed:
// "15m", "at 3pm", "tomorrow at 9am", "every monday at 3:00 PM EST",
// "every 2h", "back". It is the grammar /remind and /in share, and the
// only place in the plugin that turns words into a moment.
//
// Parsing is pure: nothing here reads a clock or a database. A clock
// form comes back as a RULE — which days, what minute, in which zone —
// and the instant it names is resolved by Postgres (chat_deferred_next,
// deferred.c), because the zone database lives there and POSIX has no
// thread-safe way to ask for a zone other than the process's own.
//
// A leaf on purpose, like cooldown.h: libc only, so tests/test_when.c
// compiles this one file and nothing else.

// Longest IANA name in tzdata is 32 bytes ("America/Argentina/ComodRivadavia");
// this is the column width chat_deferred.tz is declared with.
#define WHEN_TZ_SZ        64

// Every day of the week, as a when_t.days mask.
#define WHEN_DAYS_ALL     0x7f

typedef enum
{
  WHEN_DELAY = 0,   // once, `secs` from now
  WHEN_BACK,        // once, the next time the person is seen
  WHEN_CLOCK,       // once, at the first `minute` in `tz` that falls on a
                    // `days` day between `lo` and `hi` days from today
  WHEN_EVERY,       // repeating, every `secs`
  WHEN_WEEKLY,      // repeating, at `minute` in `tz` on every `days` day
} when_kind_t;

// Only when_parse() and when_repeat_restore() write one of these —
// the two places a moment comes in from outside, a person and a stored
// row. Past them: `tz` is a zone name made of [A-Za-z0-9/_+-] and
// nothing else, so it is safe to quote into SQL; `minute` is below
// 1440; a clock or weekly `days` is a non-empty mask of bits 0..6, bit
// n being Postgres DOW n (0 = Sunday).
typedef struct
{
  when_kind_t kind;
  uint64_t    secs;
  uint8_t     days;
  uint8_t     lo;
  uint8_t     hi;
  uint16_t    minute;
  bool        tz_named;          // the phrase named the zone itself
  char        tz[WHEN_TZ_SZ];
} when_t;

// Parse the moment at the head of `text`. `local_tz` is the zone a
// clock form means when the phrase names none — the daemon's own, from
// when_local_zone(); NULL or empty refuses every clock form rather than
// guessing one. On success `*rest` points past the phrase and any
// blanks after it (possibly at the NUL). On failure `*why` is a
// sentence fit to show the person, and `*out` is unspecified.
bool when_parse(const char *text, const char *local_tz, when_t *out,
    const char **rest, const char **why);

// A stored repeat back into a when_t, for describing it: a weekly rule
// when `days` is set, else an interval. FAIL when the columns do not
// make one — a hand-edited row, or one that never repeated.
bool when_repeat_restore(uint64_t secs, int64_t days, int64_t minute,
    const char *tz, when_t *out);

// One duration token — "90s", "15m", "1h30m", "2d", "1w", or a bare
// number of seconds. 0 when it is not one: an unknown unit ("1mo") is
// a refusal, never a guess at the nearest known one.
uint64_t when_parse_duration(const char *tok);

// The daemon's own zone as an IANA name — $TZ, else the target of
// /etc/localtime. FAIL when neither names one.
bool when_local_zone(char *dst, size_t cap);

// A repeating moment in words, for a cue or a listing: "every Monday
// at 3:00 PM", "every weekday at 8:30 AM (America/Chicago)", "every
// 2h". The zone is shown only when it differs from `local_tz`. Writes
// an empty string for a moment that does not repeat.
void when_describe_repeat(const when_t *w, const char *local_tz, char *dst,
    size_t cap);

// Minutes past midnight as "3:00 PM".
void when_fmt_minute(uint16_t minute, char *dst, size_t cap);

#ifdef WHEN_INTERNAL

// A duration past this is a typo, not a plan — and the cap is what
// keeps the sum of a compound token from overflowing. Policy (what a
// verb will actually accept) belongs to the verb.
#define WHEN_DURATION_MAX  (10ULL * 365ULL * 86400ULL)

// One word of the phrase. `raw` keeps the person's own case, which an
// IANA zone needs verbatim and a two-letter zone code is gated on;
// `norm` is lower-cased for every other comparison. Both lose trailing
// punctuation ("monday," "p.m.") so a list or a sentence end matches.
// A word too long for either matches nothing.
typedef struct
{
  const char *end;              // one past the word in the source
  bool        long_word;
  char        raw [WHEN_TZ_SZ];
  char        norm[WHEN_TZ_SZ];
} when_word_t;

// The parse in flight: the cursor, and where it is going.
typedef struct
{
  const char *p;
  const char *local_tz;
  when_t     *out;
  const char *why;
} when_cur_t;

// A component the phrase may or may not carry. ERROR means it began
// one and got it wrong, and `why` says how.
typedef enum
{
  WHEN_PART_FOUND = 0,
  WHEN_PART_NONE,
  WHEN_PART_ERROR,
} when_part_t;

static const struct
{
  const char *name;
  uint8_t     dow;
} when_weekdays[] = {
  { "sun", 0 }, { "sunday",    0 }, { "sundays",    0 },
  { "mon", 1 }, { "monday",    1 }, { "mondays",    1 },
  { "tue", 2 }, { "tues",      2 }, { "tuesday",    2 }, { "tuesdays",  2 },
  { "wed", 3 }, { "weds",      3 }, { "wednesday",  3 }, { "wednesdays", 3 },
  { "thu", 4 }, { "thur",      4 }, { "thurs",      4 }, { "thursday",  4 },
  { "thursdays", 4 },
  { "fri", 5 }, { "friday",    5 }, { "fridays",    5 },
  { "sat", 6 }, { "saturday",  6 }, { "saturdays",  6 },
};

static const char *const when_day_names[7] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
  "Saturday",
};

static const struct
{
  const char *name;
  uint32_t    secs;
} when_units[] = {
  { "s", 1 },      { "sec", 1 },     { "secs", 1 },
  { "second", 1 }, { "seconds", 1 },
  { "m", 60 },     { "min", 60 },    { "mins", 60 },
  { "minute", 60 },{ "minutes", 60 },
  { "h", 3600 },   { "hr", 3600 },   { "hrs", 3600 },
  { "hour", 3600 },{ "hours", 3600 },
  { "d", 86400 },  { "day", 86400 }, { "days", 86400 },
  { "w", 604800 }, { "wk", 604800 }, { "wks", 604800 },
  { "week", 604800 }, { "weeks", 604800 },
};

// What people say, mapped to the zone they mean by it. "3pm EST" in
// July means 3pm on the east coast, not 4pm: the abbreviation names a
// PLACE, so it resolves to that place's zone and its daylight rules,
// never to the fixed offset Postgres would read "EST" as. Two-letter
// codes count only in capitals — "at 3pm pt session" is physical
// therapy, and "PT" is not.
static const struct
{
  const char *name;
  const char *zone;
} when_zones[] = {
  { "est", "America/New_York" },    { "edt", "America/New_York" },
  { "et",  "America/New_York" },    { "eastern", "America/New_York" },
  { "cst", "America/Chicago" },     { "cdt", "America/Chicago" },
  { "ct",  "America/Chicago" },     { "central", "America/Chicago" },
  { "mst", "America/Denver" },      { "mdt", "America/Denver" },
  { "mt",  "America/Denver" },      { "mountain", "America/Denver" },
  { "pst", "America/Los_Angeles" }, { "pdt", "America/Los_Angeles" },
  { "pt",  "America/Los_Angeles" }, { "pacific", "America/Los_Angeles" },
  { "akst", "America/Anchorage" },  { "akdt", "America/Anchorage" },
  { "alaska", "America/Anchorage" },
  { "hst", "Pacific/Honolulu" },    { "hawaii", "Pacific/Honolulu" },
  { "utc", "UTC" },                 { "gmt", "UTC" },
};

static bool        when_next_word(const char *p, when_word_t *w);
static bool        when_is(const when_word_t *w, const char *s);
static bool        when_all_digits(const char *s);
static bool        when_zone_valid(const char *s);
static int         when_weekday(const char *norm);
static uint8_t     when_weekday_list(const char *norm);
static uint32_t    when_unit(const char *s);
static int         when_ampm(const char *s);
static bool        when_try_duration(when_cur_t *c);
static when_part_t when_try_time(when_cur_t *c, bool after_at);
static when_part_t when_try_at_time(when_cur_t *c);
static bool        when_try_zone(when_cur_t *c);
static uint8_t     when_try_days(when_cur_t *c);
static bool        when_try_dayword(when_cur_t *c);
static bool        when_parse_every(when_cur_t *c, bool daily);
static bool        when_parse_clock(when_cur_t *c);
static bool        when_finish_zone(when_cur_t *c);
static const char *when_zoneinfo_tail(const char *path);
static void        when_describe_days(uint8_t days, char *dst, size_t cap);
static void        when_fmt_secs(uint64_t secs, char *dst, size_t cap);

#endif // WHEN_INTERNAL

#endif // CHATBOT_WHEN_H
