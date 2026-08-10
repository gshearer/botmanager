// botmanager — MIT
// Provider structs in, neutral view out. The only file that knows both.
#define WEATHER_INTERNAL
#define WEATHER_ADAPT_TU
#include "weather.h"

#include <stdio.h>
#include <string.h>

// One area entry, stripped of its state suffix: "Fairfield, OH" is the
// county Fairfield, and printing the state on every one of three
// entries says "OH" three times.
static void
weather_area_bare(const char *part, size_t len, char *out, size_t sz)
{
  size_t i;

  for(i = 0; i + 1 < len; i++)
  {
    if(part[i] == ',' && part[i + 1] == ' ')
    {
      len = i;
      break;
    }
  }

  if(len >= sz)
    len = sz - 1;

  memcpy(out, part, len);
  out[len] = '\0';
}

// The area segment, print-ready.
//
// Up to three areas are named outright, with the state stated once at
// the end when every one of them shares it. Beyond three the names stop
// being information and become a wall, so they collapse to a count.
static void
weather_area_compose(const weathergov_alert_t *a, char *out, size_t sz)
{
  const char *p   = a->area;
  size_t      len = 0;

  out[0] = '\0';

  if(a->area_count == 0 || p[0] == '\0')
    return;

  if(a->area_count > 3)
  {
    snprintf(out, sz, "%u counties%s%s", (unsigned)a->area_count,
        a->state[0] != '\0' ? " " : "", a->state);
    return;
  }

  while(*p != '\0' && len + 1 < sz)
  {
    const char *end  = strstr(p, "; ");
    size_t      plen = (end != NULL) ? (size_t)(end - p) : strlen(p);
    char        bare[64];

    weather_area_bare(p, plen, bare, sizeof(bare));

    len += (size_t)snprintf(out + len, sz - len, "%s%s",
        len > 0 ? ", " : "", bare);

    if(end == NULL || len + 1 >= sz)
      break;

    p = end + 2;
  }

  if(a->state[0] != '\0' && len + 1 < sz)
    snprintf(out + len, sz - len, " %s", a->state);
}

void
weather_view_from_wxg_alerts(const weathergov_alert_set_t *src,
    weather_view_alert_set_t *out)
{
  uint8_t i;

  memset(out, 0, sizeof(*out));

  out->total = src->total;

  for(i = 0; i < src->count
      && i < (uint8_t)(sizeof(out->alerts) / sizeof(out->alerts[0])); i++)
  {
    const weathergov_alert_t *a = &src->alerts[i];
    weather_view_alert_t     *v = &out->alerts[i];

    snprintf(v->event,  sizeof(v->event),  "%s", a->event);
    snprintf(v->impact, sizeof(v->impact), "%s", a->impact);
    snprintf(v->state,  sizeof(v->state),  "%s", a->state);

    weather_area_compose(a, v->area, sizeof(v->area));

    v->severity   = (uint8_t)a->severity;
    v->area_count = a->area_count;
    v->tz_offset  = a->tz_offset;

    // `ends` is null on every Special Weather Statement; `expires` is
    // the fallback CAP always carries.
    v->until = (a->ends != 0) ? a->ends : a->expires;

    out->count++;
  }
}

// OpenWeather carries the product name and nothing else — no area, no
// end time, no impact — so most of the view stays empty and the line
// grammar omits each missing segment along with its separator.
//
// Severity is pinned at Severe rather than Unknown on purpose: One Call
// only ever surfaces real warnings, and Unknown would demote every
// non-US alert to a grey ⚑ when today it is a red ⚠.
void
weather_view_from_ow_alerts(const openweather_alert_set_t *src,
    weather_view_alert_set_t *out)
{
  uint8_t i;

  memset(out, 0, sizeof(*out));

  for(i = 0; i < src->count
      && i < (uint8_t)(sizeof(out->alerts) / sizeof(out->alerts[0])); i++)
  {
    if(src->alerts[i].event[0] == '\0')
      continue;

    snprintf(out->alerts[out->count].event,
        sizeof(out->alerts[out->count].event), "%s", src->alerts[i].event);

    out->alerts[out->count].severity = 3;   // Severe
    out->count++;
  }

  out->total = out->count;
}
