// botmanager — MIT
// The moment a person asks for, read off the head of what they typed.

#define WHEN_INTERNAL
#include "when.h"

#include <ctype.h>
#include <unistd.h>

// ---------- words ----------

// The word at `p`, blanks before it skipped. false at the end of text.
static bool
when_next_word(const char *p, when_word_t *w)
{
  const char *start;
  size_t      n;

  while(*p == ' ' || *p == '\t')
    p++;

  if(*p == '\0')
    return(false);

  start = p;

  while(*p != '\0' && *p != ' ' && *p != '\t')
    p++;

  w->end = p;
  n      = (size_t)(p - start);

  while(n > 0 && strchr(",;.!?", start[n - 1]) != NULL)
    n--;

  w->long_word = (n >= sizeof(w->raw));

  if(w->long_word)
    n = 0;

  memcpy(w->raw, start, n);
  w->raw[n] = '\0';

  for(size_t i = 0; i <= n; i++)
    w->norm[i] = (char)tolower((unsigned char)w->raw[i]);

  return(true);
}

static bool
when_is(const when_word_t *w, const char *s)
{
  return(strcmp(w->norm, s) == 0);
}

static bool
when_all_digits(const char *s)
{
  if(*s == '\0')
    return(false);

  for(; *s != '\0'; s++)
    if(*s < '0' || *s > '9')
      return(false);

  return(true);
}

// What a zone name may be made of, and the whole reason when_t.tz can
// be quoted into SQL: letters, digits and "/_+-", no leading slash and
// no "..". Explicit ranges rather than isalnum(), which is the
// locale's to answer.
static bool
when_zone_valid(const char *s)
{
  if(s[0] == '\0' || s[0] == '/' || strstr(s, "..") != NULL)
    return(false);

  for(; *s != '\0'; s++)
  {
    char ch = *s;

    if(!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9') || strchr("/_+-", ch) != NULL))
      return(false);
  }

  return(true);
}

static int
when_weekday(const char *norm)
{
  for(size_t i = 0; i < sizeof(when_weekdays) / sizeof(when_weekdays[0]); i++)
    if(strcmp(norm, when_weekdays[i].name) == 0)
      return(when_weekdays[i].dow);

  return(-1);
}

// "mon" or "mon,wed,fri" as a mask; 0 when any piece is not a day.
static uint8_t
when_weekday_list(const char *norm)
{
  const char *p    = norm;
  uint8_t     mask = 0;

  for(;;)
  {
    const char *comma = strchr(p, ',');
    size_t      n     = comma != NULL ? (size_t)(comma - p) : strlen(p);
    char        piece[16];
    int         dow;

    if(n == 0 || n >= sizeof(piece))
      return(0);

    memcpy(piece, p, n);
    piece[n] = '\0';

    if((dow = when_weekday(piece)) < 0)
      return(0);

    mask |= (uint8_t)(1u << dow);

    if(comma == NULL)
      return(mask);

    p = comma + 1;
  }
}

static uint32_t
when_unit(const char *s)
{
  for(size_t i = 0; i < sizeof(when_units) / sizeof(when_units[0]); i++)
    if(strcmp(s, when_units[i].name) == 0)
      return(when_units[i].secs);

  return(0);
}

// 0 for AM, 12 for PM, -1 for neither.
static int
when_ampm(const char *s)
{
  if(strcmp(s, "am") == 0 || strcmp(s, "a.m") == 0)
    return(0);

  if(strcmp(s, "pm") == 0 || strcmp(s, "p.m") == 0)
    return(12);

  return(-1);
}

// ---------- durations ----------

uint64_t
when_parse_duration(const char *tok)
{
  const char *p     = tok;
  uint64_t    total = 0;

  if(tok == NULL || *tok == '\0')
    return(0);

  // A bare number is seconds — /bot hush has always read it that way.
  if(when_all_digits(tok))
  {
    for(; *p != '\0'; p++)
      if((total = total * 10 + (uint64_t)(*p - '0')) > WHEN_DURATION_MAX)
        return(0);

    return(total);
  }

  // "1h30m": number-unit pairs, every unit a known one. Until this
  // replaced chatbot_parse_duration_secs only the first letter of the
  // first unit was read, so "1h30m" was an hour and "1mo" a minute.
  while(*p != '\0')
  {
    uint64_t n = 0;
    uint32_t mult;
    char     unit[16];
    size_t   u = 0;

    if(*p < '0' || *p > '9')
      return(0);

    for(; *p >= '0' && *p <= '9'; p++)
      if((n = n * 10 + (uint64_t)(*p - '0')) > WHEN_DURATION_MAX)
        return(0);

    for(; *p != '\0' && (*p < '0' || *p > '9'); p++)
    {
      if(u + 1 >= sizeof(unit))
        return(0);

      unit[u++] = (char)tolower((unsigned char)*p);
    }

    unit[u] = '\0';

    if((mult = when_unit(unit)) == 0)
      return(0);

    // n and mult are each bounded, so the product cannot wrap.
    if((total += n * mult) > WHEN_DURATION_MAX)
      return(0);
  }

  return(total);
}

// A delay — "15m", "1h30m", or a number and its unit as two words
// ("15 minutes"). Consumes it into out->secs, or consumes nothing.
static bool
when_try_duration(when_cur_t *c)
{
  when_word_t w;
  when_word_t nx;
  const char *end;
  uint64_t    secs;
  bool        more;

  if(!when_next_word(c->p, &w) || w.long_word)
    return(false);

  more = when_next_word(w.end, &nx) && !nx.long_word;
  end  = w.end;
  secs = when_parse_duration(w.norm);

  if(when_all_digits(w.norm) && more)
  {
    // "3 pm" is a clock, and the clock grammar will say so.
    if(when_ampm(nx.norm) >= 0)
      return(false);

    if(when_unit(nx.norm) != 0)
    {
      secs *= when_unit(nx.norm);
      end   = nx.end;
    }
  }

  if(secs == 0 || secs > WHEN_DURATION_MAX)
    return(false);

  c->out->secs = secs;
  c->p         = end;
  return(true);
}

// ---------- clock parts ----------

// A time of day: "3pm", "3:30 PM", "15:00", "noon". A bare hour counts
// only after "at", and then only past 12 — "at 3" is a coin toss the
// person has to call, never one we call for them. HH:MM with no AM/PM
// is 24-hour.
static when_part_t
when_try_time(when_cur_t *c, bool after_at)
{
  when_word_t w;
  when_word_t nx;
  const char *s;
  const char *end;
  unsigned    h      = 0;
  unsigned    m      = 0;
  size_t      nd     = 0;
  bool        colon  = false;
  int         ampm   = -1;

  if(!when_next_word(c->p, &w) || w.long_word)
    return(WHEN_PART_NONE);

  if(when_is(&w, "noon") || when_is(&w, "midnight"))
  {
    c->out->minute = when_is(&w, "noon") ? 720 : 0;
    c->p           = w.end;
    return(WHEN_PART_FOUND);
  }

  for(s = w.norm; *s >= '0' && *s <= '9' && nd < 3; s++, nd++)
    h = h * 10 + (unsigned)(*s - '0');

  if(nd == 0 || nd > 2)
    return(WHEN_PART_NONE);

  if(*s == ':')
  {
    if(!(s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9'))
      return(WHEN_PART_NONE);

    m     = (unsigned)(s[1] - '0') * 10 + (unsigned)(s[2] - '0');
    s    += 3;
    colon = true;
  }

  end = w.end;

  if(*s != '\0')
  {
    if((ampm = when_ampm(s)) < 0)
      return(WHEN_PART_NONE);
  }

  else if(when_next_word(w.end, &nx) && (ampm = when_ampm(nx.norm)) >= 0)
    end = nx.end;

  if(ampm < 0 && !colon && !after_at)
    return(WHEN_PART_NONE);

  if(m > 59)
  {
    c->why = "minutes run 00 to 59";
    return(WHEN_PART_ERROR);
  }

  if(ampm >= 0)
  {
    if(h < 1 || h > 12)
    {
      c->why = "with AM or PM the hour runs 1 to 12";
      return(WHEN_PART_ERROR);
    }

    h = h % 12 + (unsigned)ampm;
  }

  else if(!colon && h <= 12)
  {
    c->why = "is that AM or PM? (say 3pm, or 15:00)";
    return(WHEN_PART_ERROR);
  }

  else if(h > 23)
  {
    c->why = "the hour runs 0 to 23";
    return(WHEN_PART_ERROR);
  }

  c->out->minute = (uint16_t)(h * 60 + m);
  c->p           = end;
  return(WHEN_PART_FOUND);
}

// The time, with or without its "at". An "at" that is not followed by
// a time is an error rather than the start of the message, because
// every caller asks only while the phrase still owes one.
static when_part_t
when_try_at_time(when_cur_t *c)
{
  when_word_t w;
  const char *save = c->p;
  when_part_t t;

  if(!when_next_word(c->p, &w) || !when_is(&w, "at"))
    return(when_try_time(c, false));

  c->p = w.end;
  t    = when_try_time(c, true);

  if(t == WHEN_PART_NONE)
  {
    c->p   = save;
    c->why = "at what time?";
    return(WHEN_PART_ERROR);
  }

  return(t);
}

static bool
when_try_zone(when_cur_t *c)
{
  when_word_t w;
  when_word_t nx;
  const char *zone = NULL;

  if(!when_next_word(c->p, &w) || w.long_word)
    return(false);

  for(size_t i = 0; i < sizeof(when_zones) / sizeof(when_zones[0]); i++)
    if(strcmp(w.norm, when_zones[i].name) == 0)
    {
      zone = when_zones[i].zone;
      break;
    }

  if(zone != NULL && strlen(w.raw) == 2)
    for(const char *s = w.raw; *s != '\0'; s++)
      if(*s < 'A' || *s > 'Z')
        zone = NULL;

  // An IANA name is Area/Location and every Area is a word, which is
  // what keeps "at 3pm 50/50 split" a message rather than a zone.
  if(zone == NULL && strchr(w.raw, '/') != NULL && when_zone_valid(w.raw)
      && ((w.raw[0] >= 'A' && w.raw[0] <= 'Z')
          || (w.raw[0] >= 'a' && w.raw[0] <= 'z')))
    zone = w.raw;

  if(zone == NULL)
    return(false);

  // Both sources fit: the table's literals are short and `raw` is
  // already bounded by WHEN_TZ_SZ.
  strlcpy(c->out->tz, zone, sizeof(c->out->tz));
  c->out->tz_named = true;
  c->p             = w.end;

  if(when_next_word(c->p, &nx) && when_is(&nx, "time"))
    c->p = nx.end;

  return(true);
}

// The days a repeat lands on: "day", "weekday", "weekend", "monday",
// "mon,wed,fri", "monday and thursday". 0 when the word is none of it.
static uint8_t
when_try_days(when_cur_t *c)
{
  when_word_t w;
  uint8_t     mask;

  if(!when_next_word(c->p, &w) || w.long_word)
    return(0);

  if(when_is(&w, "day") || when_is(&w, "days"))
    mask = WHEN_DAYS_ALL;

  else if(when_is(&w, "weekday") || when_is(&w, "weekdays"))
    mask = 0x3e;

  else if(when_is(&w, "weekend") || when_is(&w, "weekends"))
    mask = 0x41;

  else if((mask = when_weekday_list(w.norm)) == 0)
    return(0);

  c->p = w.end;

  for(;;)
  {
    when_word_t nx;
    when_word_t day;
    uint8_t     more;

    if(!when_next_word(c->p, &nx) || nx.long_word)
      break;

    if(when_is(&nx, "and") || when_is(&nx, "&"))
    {
      if(!when_next_word(nx.end, &day)
          || (more = when_weekday_list(day.norm)) == 0)
        break;

      c->p = day.end;
    }

    else if((more = when_weekday_list(nx.norm)) != 0)
      c->p = nx.end;

    else
      break;

    mask |= more;
  }

  return(mask);
}

// The day a one-shot lands on: "today", "tomorrow", or the next
// "friday" — which is today, if the time has not passed yet.
static bool
when_try_dayword(when_cur_t *c)
{
  when_word_t w;
  int         dow;

  if(!when_next_word(c->p, &w) || w.long_word)
    return(false);

  if(when_is(&w, "today") || when_is(&w, "tomorrow"))
  {
    c->out->days = WHEN_DAYS_ALL;
    c->out->lo   = when_is(&w, "tomorrow") ? 1 : 0;
    c->out->hi   = c->out->lo;
  }

  else if((dow = when_weekday(w.norm)) >= 0)
  {
    c->out->days = (uint8_t)(1u << dow);
    c->out->lo   = 0;
    c->out->hi   = 7;
  }

  else
    return(false);

  c->p = w.end;
  return(true);
}

// A clock form that names no zone means the daemon's.
static bool
when_finish_zone(when_cur_t *c)
{
  if(c->out->tz_named)
    return(SUCCESS);

  if(c->local_tz == NULL || !when_zone_valid(c->local_tz)
      || strlcpy(c->out->tz, c->local_tz, sizeof(c->out->tz))
          >= sizeof(c->out->tz))
  {
    c->why = "I can't tell what timezone I'm in — name one"
        " (e.g. 3pm America/New_York)";
    return(FAIL);
  }

  return(SUCCESS);
}

// ---------- the two clock grammars ----------

// "every monday at 3pm EST", "every weekday at 8:30", "every 2h",
// "daily at 9am". The time is required: a repeat with no hour in it is
// a question to put back to the person, not a default to pick.
static bool
when_parse_every(when_cur_t *c, bool daily)
{
  when_word_t w;
  bool        have_time = false;
  bool        have_zone = false;

  if(daily)
    c->out->days = WHEN_DAYS_ALL;

  else if((c->out->days = when_try_days(c)) == 0)
  {
    if(when_try_duration(c))
    {
      c->out->kind = WHEN_EVERY;
      return(SUCCESS);
    }

    c->why = "every what? — try every 2h, every day at 9am, or every"
        " monday at 3pm";
    return(FAIL);
  }

  c->out->kind = WHEN_WEEKLY;

  while(when_next_word(c->p, &w))
  {
    if(!have_time)
    {
      when_part_t t = when_try_at_time(c);

      if(t == WHEN_PART_ERROR)
        return(FAIL);

      if(t == WHEN_PART_FOUND)
      {
        have_time = true;
        continue;
      }
    }

    if(have_time && !have_zone && when_try_zone(c))
    {
      have_zone = true;
      continue;
    }

    break;
  }

  if(!have_time)
  {
    c->why = "at what time? (e.g. every monday at 3pm)";
    return(FAIL);
  }

  return(when_finish_zone(c));
}

// A one-shot on the clock: "at 3pm", "tomorrow at 9am", "friday
// 15:00", "at 3pm on monday EST". Each part at most once, in any
// order, and the first word that is none of them starts the message.
// With no day named it is the next such time — today, or tomorrow.
static bool
when_parse_clock(when_cur_t *c)
{
  when_word_t w;
  bool        have_day  = false;
  bool        have_time = false;
  bool        have_zone = false;

  c->out->kind = WHEN_CLOCK;
  c->out->days = WHEN_DAYS_ALL;
  c->out->lo   = 0;
  c->out->hi   = 1;

  while(when_next_word(c->p, &w))
  {
    if(!have_day && when_is(&w, "on"))
    {
      const char *save = c->p;

      c->p = w.end;

      if(when_try_dayword(c))
      {
        have_day = true;
        continue;
      }

      c->p = save;

      // "at 3pm on the call" — a complete moment, then the message.
      if(have_time)
        break;

      c->why = "on which day?";
      return(FAIL);
    }

    if(!have_day && when_try_dayword(c))
    {
      have_day = true;
      continue;
    }

    if(!have_time)
    {
      when_part_t t = when_try_at_time(c);

      if(t == WHEN_PART_ERROR)
        return(FAIL);

      if(t == WHEN_PART_FOUND)
      {
        have_time = true;
        continue;
      }
    }

    if(have_time && !have_zone && when_try_zone(c))
    {
      have_zone = true;
      continue;
    }

    break;
  }

  if(!have_time)
  {
    c->why = have_day ? "at what time? (e.g. tomorrow at 9am)"
        : "I can't tell when you mean — try 20m, at 3pm, tomorrow at"
          " 9am, every monday at 3pm, or back";
    return(FAIL);
  }

  return(when_finish_zone(c));
}

// ---------- public ----------

bool
when_parse(const char *text, const char *local_tz, when_t *out,
    const char **rest, const char **why)
{
  when_cur_t  c  = { .p = text != NULL ? text : "", .local_tz = local_tz,
                     .out = out, .why = NULL };
  when_word_t w;
  bool        ok = SUCCESS;

  memset(out, 0, sizeof(*out));

  if(!when_next_word(c.p, &w))
  {
    *why = "tell me when — e.g. 20m, at 3pm, every monday at 3pm, or back";
    return(FAIL);
  }

  if(when_is(&w, "back"))
  {
    out->kind = WHEN_BACK;
    c.p       = w.end;
  }

  else if(when_is(&w, "every") || when_is(&w, "daily"))
  {
    c.p = w.end;
    ok  = when_parse_every(&c, when_is(&w, "daily"));
  }

  else if(when_is(&w, "in"))
  {
    c.p = w.end;

    if(!when_try_duration(&c))
    {
      c.why = "in how long? (e.g. in 20m)";
      ok    = FAIL;
    }
  }

  // A delay first: no duration reads as a clock, and "3 pm" is refused
  // here so the clock grammar gets it.
  else if(!when_try_duration(&c))
    ok = when_parse_clock(&c);

  if(ok != SUCCESS)
  {
    *why = c.why;
    return(FAIL);
  }

  while(*c.p == ' ' || *c.p == '\t')
    c.p++;

  *rest = c.p;
  return(SUCCESS);
}

bool
when_repeat_restore(uint64_t secs, int64_t days, int64_t minute,
    const char *tz, when_t *out)
{
  memset(out, 0, sizeof(*out));

  if(days != 0)
  {
    if(days < 0 || days > WHEN_DAYS_ALL || minute < 0 || minute >= 1440
        || tz == NULL || !when_zone_valid(tz)
        || strlcpy(out->tz, tz, sizeof(out->tz)) >= sizeof(out->tz))
      return(FAIL);

    out->kind     = WHEN_WEEKLY;
    out->days     = (uint8_t)days;
    out->minute   = (uint16_t)minute;
    out->tz_named = true;
    return(SUCCESS);
  }

  if(secs == 0 || secs > WHEN_DURATION_MAX)
    return(FAIL);

  out->kind = WHEN_EVERY;
  out->secs = secs;
  return(SUCCESS);
}

static const char *
when_zoneinfo_tail(const char *path)
{
  const char *z = strstr(path, "zoneinfo/");

  return(z != NULL ? z + strlen("zoneinfo/") : NULL);
}

bool
when_local_zone(char *dst, size_t cap)
{
  const char *env  = getenv("TZ");
  const char *name = NULL;
  char        link[256];
  ssize_t     n;

  if(env != NULL && env[0] != '\0')
  {
    if(env[0] == ':')
      env++;

    name = env[0] == '/' ? when_zoneinfo_tail(env) : env;
  }

  else if((n = readlink("/etc/localtime", link, sizeof(link) - 1)) > 0)
  {
    link[n] = '\0';
    name    = when_zoneinfo_tail(link);
  }

  // A POSIX rule string in $TZ ("EST5EDT,M3.2.0,M11.1.0") or a copied
  // rather than linked /etc/localtime names no zone we can pass on —
  // and a clock form refused beats one resolved in the wrong place.
  if(name == NULL || !when_zone_valid(name)
      || strlcpy(dst, name, cap) >= cap)
    return(FAIL);

  return(SUCCESS);
}

void
when_fmt_minute(uint16_t minute, char *dst, size_t cap)
{
  unsigned h   = (unsigned)(minute / 60) % 24;
  unsigned h12 = h % 12 == 0 ? 12 : h % 12;

  snprintf(dst, cap, "%u:%02u %s", h12, (unsigned)(minute % 60),
      h < 12 ? "AM" : "PM");
}

// "day", "weekday", "Monday", "Monday and Thursday", "Monday,
// Wednesday and Friday" — Monday first, because that is how a week
// reads aloud even where a calendar starts on Sunday.
static void
when_describe_days(uint8_t days, char *dst, size_t cap)
{
  static const uint8_t order[7] = { 1, 2, 3, 4, 5, 6, 0 };
  size_t               total    = 0;
  size_t               seen     = 0;

  dst[0] = '\0';

  if(days == WHEN_DAYS_ALL)
  {
    strlcpy(dst, "day", cap);
    return;
  }

  if(days == 0x3e)
  {
    strlcpy(dst, "weekday", cap);
    return;
  }

  for(size_t i = 0; i < 7; i++)
    if(days & (1u << order[i]))
      total++;

  for(size_t i = 0; i < 7; i++)
  {
    if(!(days & (1u << order[i])))
      continue;

    if(seen > 0)
      strlcat(dst, seen + 1 == total ? " and " : ", ", cap);

    strlcat(dst, when_day_names[order[i]], cap);
    seen++;
  }
}

// "90s", "15m", "2h", "1h30m", "1d12h" — the largest two parts, which
// is all a repeat interval ever needs to say.
static void
when_fmt_secs(uint64_t secs, char *dst, size_t cap)
{
  static const struct
  {
    uint64_t secs;
    char     unit;
  } parts[] = { { 86400, 'd' }, { 3600, 'h' }, { 60, 'm' }, { 1, 's' } };
  size_t pos  = 0;
  int    used = 0;

  dst[0] = '\0';

  for(size_t i = 0; i < 4 && used < 2 && pos < cap; i++)
  {
    uint64_t n = secs / parts[i].secs;
    int      k;

    if(n == 0)
    {
      // Once the leading part is written, a zero between two parts
      // ends the phrase: "1d" rather than "1d0h".
      if(used > 0)
        break;

      continue;
    }

    k = snprintf(dst + pos, cap - pos, "%llu%c", (unsigned long long)n,
        parts[i].unit);

    if(k < 0)
      break;

    pos  += (size_t)k;
    secs -= n * parts[i].secs;
    used++;
  }
}

void
when_describe_repeat(const when_t *w, const char *local_tz, char *dst,
    size_t cap)
{
  char days[96];
  char clock[16];

  dst[0] = '\0';

  if(w->kind == WHEN_EVERY)
  {
    char span[32];

    when_fmt_secs(w->secs, span, sizeof(span));
    snprintf(dst, cap, "every %s", span);
    return;
  }

  if(w->kind != WHEN_WEEKLY)
    return;

  when_describe_days(w->days, days, sizeof(days));
  when_fmt_minute(w->minute, clock, sizeof(clock));

  if(local_tz != NULL && strcmp(w->tz, local_tz) == 0)
    snprintf(dst, cap, "every %s at %s", days, clock);

  else
    snprintf(dst, cap, "every %s at %s (%s)", days, clock, w->tz);
}
