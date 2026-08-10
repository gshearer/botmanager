// botmanager — MIT
// weather.gov active alerts: CAP parse, supersession dedup, impact line.
#define WEATHERGOV_INTERNAL
#define WXG_ALERTS_TU
#include "weathergov.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ----------------------------------------------------------------------
// CAP vocabularies
// ----------------------------------------------------------------------
//
// Each of these is a closed set upstream, so an unrecognised value is a
// protocol surprise and lands on the UNKNOWN arm rather than reaching a
// human as raw text.

static weathergov_severity_t
wxg_severity_of(const char *s)
{
  if(strcmp(s, "Extreme")  == 0) return(WEATHERGOV_SEV_EXTREME);
  if(strcmp(s, "Severe")   == 0) return(WEATHERGOV_SEV_SEVERE);
  if(strcmp(s, "Moderate") == 0) return(WEATHERGOV_SEV_MODERATE);
  if(strcmp(s, "Minor")    == 0) return(WEATHERGOV_SEV_MINOR);

  return(WEATHERGOV_SEV_UNKNOWN);
}

static weathergov_urgency_t
wxg_urgency_of(const char *s)
{
  if(strcmp(s, "Immediate") == 0) return(WEATHERGOV_URG_IMMEDIATE);
  if(strcmp(s, "Expected")  == 0) return(WEATHERGOV_URG_EXPECTED);
  if(strcmp(s, "Future")    == 0) return(WEATHERGOV_URG_FUTURE);
  if(strcmp(s, "Past")      == 0) return(WEATHERGOV_URG_PAST);

  return(WEATHERGOV_URG_UNKNOWN);
}

static weathergov_certainty_t
wxg_certainty_of(const char *s)
{
  if(strcmp(s, "Observed") == 0) return(WEATHERGOV_CRT_OBSERVED);
  if(strcmp(s, "Likely")   == 0) return(WEATHERGOV_CRT_LIKELY);
  if(strcmp(s, "Possible") == 0) return(WEATHERGOV_CRT_POSSIBLE);
  if(strcmp(s, "Unlikely") == 0) return(WEATHERGOV_CRT_UNLIKELY);

  return(WEATHERGOV_CRT_UNKNOWN);
}

// The protective action, as a phrase a channel can read. All nine CAP
// values are mapped so an unexpected one cannot print raw; the three
// that say nothing actionable map to the empty string and are dropped
// by the caller.
//
// `Execute` is not exotic — it carried 4 of 11 alerts in the sample
// this was written against, every one of them a Special Weather
// Statement.
static const char *
wxg_response_phrase(const char *response)
{
  if(strcmp(response, "Shelter")  == 0) return("take shelter");
  if(strcmp(response, "Evacuate") == 0) return("evacuate");
  if(strcmp(response, "Avoid")    == 0) return("avoid the area");
  if(strcmp(response, "Prepare")  == 0) return("prepare");
  if(strcmp(response, "Execute")  == 0) return("act now");
  if(strcmp(response, "Monitor")  == 0) return("monitor");

  // Assess, AllClear, None, and anything unrecognised.
  return("");
}

// ----------------------------------------------------------------------
// The impact segment
// ----------------------------------------------------------------------

// ⚠ Every value in the CAP `parameters` map is an ARRAY of strings,
// including the ones that are logically scalar. Read element 0.
static bool
wxg_param_first(struct json_object *params, const char *key,
    char *out, size_t sz)
{
  struct json_object *arr;
  struct json_object *e;
  const char *s;

  out[0] = '\0';

  if(params == NULL)
    return(false);

  arr = json_get_array(params, key);

  if(arr == NULL || json_object_array_length(arr) == 0)
    return(false);

  e = json_object_array_get_idx(arr, 0);

  if(e == NULL)
    return(false);

  s = json_object_get_string(e);

  if(s == NULL || s[0] == '\0')
    return(false);

  snprintf(out, sz, "%s", s);

  return(true);
}

// Append an item only if it fits whole. A half-written "hail 0.7" is
// worse than a missing one, and the renderer drops trailing items to
// meet its own width budget anyway.
static void
wxg_impact_append(char *out, size_t sz, const char *item)
{
  size_t len;
  size_t ilen;

  if(item == NULL || item[0] == '\0')
    return;

  len  = strnlen(out, sz);
  ilen = strlen(item);

  if(len + ilen + (len > 0 ? 2 : 0) >= sz)
    return;

  // Copied rather than snprintf'd: the fit is already proven above, and
  // the bounded form leaves the compiler unable to see that.
  if(len > 0)
  {
    out[len++] = ',';
    out[len++] = ' ';
  }

  memcpy(out + len, item, ilen + 1);
}

// "70 MPH" -> "70mph gusts". A zero measurement is upstream saying "not
// this hazard" — an issuer fills the whole parameter block whether or
// not every field applies — and `hail 0.00"` on a channel is noise.
static void
wxg_gust_phrase(const char *raw, char *out, size_t sz)
{
  char   tidy[32];
  size_t i;
  size_t j = 0;

  out[0] = '\0';

  if(strtod(raw, NULL) <= 0.0)
    return;

  for(i = 0; raw[i] != '\0' && j + 1 < sizeof(tidy); i++)
  {
    if(raw[i] == ' ')
      continue;

    tidy[j++] = (char)tolower((unsigned char)raw[i]);
  }

  tidy[j] = '\0';

  if(j > 0)
    snprintf(out, sz, "%s gusts", tidy);
}

// "Up to .75" -> hail 0.75"  — the leading zero is ours; upstream omits
// it and a bare .75 reads as a typo.
static void
wxg_hail_phrase(const char *raw, char *out, size_t sz)
{
  const char *v = raw;

  out[0] = '\0';

  while(*v == ' ')
    v++;

  if(strncasecmp(v, "up to ", 6) == 0)
    v += 6;

  while(*v == ' ')
    v++;

  if(*v != '\0' && strtod(v, NULL) > 0.0)
    snprintf(out, sz, "hail %s%s\"", *v == '.' ? "0" : "", v);
}

// "CONSIDERABLE" -> "damage considerable"
static void
wxg_threat_phrase(const char *noun, const char *raw, char *out, size_t sz)
{
  char   low[48];
  size_t i;

  out[0] = '\0';

  for(i = 0; raw[i] != '\0' && i + 1 < sizeof(low); i++)
    low[i] = (char)tolower((unsigned char)raw[i]);

  low[i] = '\0';

  if(i > 0)
    snprintf(out, sz, "%s %s", noun, low);
}

// What the alert will do to you, in the order a human wants it: the
// measured hazard first, the advice only when there is no measurement
// to give. A Special Weather Statement carries no threat parameters at
// all, so for those the advice is the whole segment.
static void
wxg_impact_build(struct json_object *params, const char *response,
    char *out, size_t sz)
{
  char raw [64];
  char item[96];

  out[0] = '\0';

  if(wxg_param_first(params, "maxWindGust", raw, sizeof(raw)))
  {
    wxg_gust_phrase(raw, item, sizeof(item));
    wxg_impact_append(out, sz, item);
  }

  if(wxg_param_first(params, "maxHailSize", raw, sizeof(raw)))
  {
    wxg_hail_phrase(raw, item, sizeof(item));
    wxg_impact_append(out, sz, item);
  }

  if(wxg_param_first(params, "thunderstormDamageThreat", raw, sizeof(raw)))
  {
    wxg_threat_phrase("damage", raw, item, sizeof(item));
    wxg_impact_append(out, sz, item);
  }

  if(wxg_param_first(params, "flashFloodDamageThreat", raw, sizeof(raw)))
  {
    wxg_threat_phrase("flooding", raw, item, sizeof(item));
    wxg_impact_append(out, sz, item);
  }

  if(out[0] == '\0')
    wxg_impact_append(out, sz, wxg_response_phrase(response));
}

// ----------------------------------------------------------------------
// Area
// ----------------------------------------------------------------------

// The last comma within the first `len` bytes, or NULL. Hand-rolled
// because memrchr is a _GNU_SOURCE symbol and this file needs nothing
// else from that dialect.
static const char *
wxg_last_comma(const char *s, size_t len)
{
  size_t i = len;

  while(i > 0)
  {
    i--;

    if(s[i] == ',')
      return(s + i);
  }

  return(NULL);
}

// `areaDesc` arrives in two shapes: county products name the state on
// every entry ("Fairfield, OH; Hocking, OH") and zone products name no
// state at all ("Perry; Morgan; Athens"). Count the areas, and report
// the state only when every entry agreed on one — a mixed or absent
// state is the empty case, not a parse failure.
static void
wxg_area_summarize(const char *area_desc, char *state_out, size_t state_sz,
    uint8_t *count_out)
{
  const char *p = area_desc;
  char        state[WEATHERGOV_STATE_SZ];
  bool        agreed = true;
  uint8_t     n      = 0;

  state_out[0] = '\0';
  state[0]     = '\0';
  *count_out   = 0;

  if(p == NULL || *p == '\0')
    return;

  while(*p != '\0')
  {
    const char *end   = strstr(p, "; ");
    size_t      len   = (end != NULL) ? (size_t)(end - p) : strlen(p);
    const char *comma = wxg_last_comma(p, len);

    n++;

    if(comma != NULL)
    {
      const char *st = comma + 1;
      size_t      stlen;

      while(*st == ' ')
        st++;

      stlen = len - (size_t)(st - p);

      if(stlen > 0 && stlen < state_sz)
      {
        char cand[WEATHERGOV_STATE_SZ];

        memcpy(cand, st, stlen);
        cand[stlen] = '\0';

        if(state[0] == '\0')
          snprintf(state, sizeof(state), "%s", cand);

        else if(strcmp(state, cand) != 0)
          agreed = false;
      }

      else
        agreed = false;
    }

    else
      agreed = false;

    p = (end != NULL) ? end + 2 : p + len;
  }

  *count_out = n;

  if(agreed && state[0] != '\0')
    snprintf(state_out, state_sz, "%s", state);
}

// ----------------------------------------------------------------------
// One alert
// ----------------------------------------------------------------------

// When an alert lifts. `ends` is null on a Special Weather Statement —
// four of eleven in the sample — and `expires` is the fallback CAP
// always carries.
static time_t
wxg_alert_until(const weathergov_alert_t *a)
{
  return(a->ends != 0 ? a->ends : a->expires);
}

// Worst first, then soonest to lift. The three that fit a reply are
// therefore the three that matter.
static bool
wxg_alert_precedes(const weathergov_alert_t *a, const weathergov_alert_t *b)
{
  if(a->severity != b->severity)
    return(a->severity > b->severity);

  return(wxg_alert_until(a) < wxg_alert_until(b));
}

static void
wxg_alert_parse_one(struct json_object *props, weathergov_alert_t *out)
{
  char ts[48];

  memset(out, 0, sizeof(*out));

  json_get_str(props, "id",          out->id,          sizeof(out->id));
  json_get_str(props, "event",       out->event,       sizeof(out->event));
  json_get_str(props, "areaDesc",    out->area,        sizeof(out->area));
  json_get_str(props, "headline",    out->headline,    sizeof(out->headline));
  json_get_str(props, "description", out->desc,        sizeof(out->desc));
  json_get_str(props, "instruction", out->instruction,
      sizeof(out->instruction));
  json_get_str(props, "senderName",  out->sender,      sizeof(out->sender));
  json_get_str(props, "response",    out->response,    sizeof(out->response));

  if(json_get_str(props, "severity", ts, sizeof(ts)))
    out->severity = wxg_severity_of(ts);

  if(json_get_str(props, "urgency", ts, sizeof(ts)))
    out->urgency = wxg_urgency_of(ts);

  if(json_get_str(props, "certainty", ts, sizeof(ts)))
    out->certainty = wxg_certainty_of(ts);

  // The offset is read off `expires` and nothing else: it is the one
  // timestamp CAP populates on every product, so the alert can never
  // end up rendered against a 0-by-accident UTC.
  if(json_get_str(props, "expires", ts, sizeof(ts)))
    out->expires = wxg_parse_iso8601(ts, &out->tz_offset);

  if(json_get_str(props, "ends", ts, sizeof(ts)))
    out->ends = wxg_parse_iso8601(ts, NULL);

  if(json_get_str(props, "onset", ts, sizeof(ts)))
    out->onset = wxg_parse_iso8601(ts, NULL);

  if(json_get_str(props, "sent", ts, sizeof(ts)))
    out->sent = wxg_parse_iso8601(ts, NULL);

  wxg_area_summarize(out->area, out->state, sizeof(out->state),
      &out->area_count);

  wxg_impact_build(json_get_obj(props, "parameters"), out->response,
      out->impact, sizeof(out->impact));
}

// ----------------------------------------------------------------------
// Dedup
// ----------------------------------------------------------------------
//
// Two passes, and the first is the one that matters. CAP states
// supersession outright: an Update names the bulletin it replaces in
// references[], so a storm's 10-minute radar refreshes collapse to the
// single current bulletin without guessing. The second pass is the
// weaker label test, kept because two genuinely distinct products can
// still land on one event+area pair.

static void
wxg_refs_collect(struct json_object *features, wxg_alert_refs_t *refs)
{
  int n = (int)json_object_array_length(features);
  int i;
  int j;

  refs->count = 0;

  for(i = 0; i < n && refs->count < WXG_ALERT_REF_MAX; i++)
  {
    struct json_object *feat = json_object_array_get_idx(features, i);
    struct json_object *props;
    struct json_object *arr;
    int                 rn;

    if(feat == NULL)
      continue;

    props = json_get_obj(feat, "properties");

    if(props == NULL)
      continue;

    arr = json_get_array(props, "references");

    if(arr == NULL)
      continue;

    rn = (int)json_object_array_length(arr);

    for(j = 0; j < rn && refs->count < WXG_ALERT_REF_MAX; j++)
    {
      struct json_object *e = json_object_array_get_idx(arr, j);

      if(e == NULL)
        continue;

      if(json_get_str(e, "identifier", refs->id[refs->count],
          WEATHERGOV_ID_SZ))
        refs->count++;
    }
  }
}

static bool
wxg_refs_contains(const wxg_alert_refs_t *refs, const char *id)
{
  uint8_t i;

  if(id[0] == '\0')
    return(false);

  for(i = 0; i < refs->count; i++)
  {
    if(strcmp(refs->id[i], id) == 0)
      return(true);
  }

  return(false);
}

// Returns false when this event+area pair has already been kept. The
// fingerprint is two 32-bit hashes packed into one word — the test is
// exact-match only, so the strings themselves buy nothing.
static bool
wxg_labels_add(wxg_alert_labels_t *seen, const weathergov_alert_t *a)
{
  uint64_t key = ((uint64_t)util_fnv1a(a->event) << 32)
      | (uint64_t)util_fnv1a(a->area);
  uint8_t  i;

  for(i = 0; i < seen->count; i++)
  {
    if(seen->label[i] == key)
      return(false);
  }

  if(seen->count < WXG_ALERT_LABEL_MAX)
    seen->label[seen->count++] = key;

  return(true);
}

// Insertion sort into a fixed set: the worst entry falls off the end
// once the set is full, which is exactly the right one to lose.
static void
wxg_alert_set_insert(weathergov_alert_set_t *set, const weathergov_alert_t *a)
{
  uint8_t pos = 0;
  uint8_t i;

  while(pos < set->count && !wxg_alert_precedes(a, &set->alerts[pos]))
    pos++;

  if(pos >= WEATHERGOV_ALERT_MAX)
    return;

  if(set->count < WEATHERGOV_ALERT_MAX)
    set->count++;

  for(i = (uint8_t)(set->count - 1); i > pos; i--)
    set->alerts[i] = set->alerts[i - 1];

  set->alerts[pos] = *a;
}

// ----------------------------------------------------------------------
// Transfer
// ----------------------------------------------------------------------

static void
wxg_alerts_deliver(wxg_request_t *r)
{
  wxg_caller_t caller;

  wxg_req_take_caller(r, &caller);

  if(caller.cb.alerts != NULL)
    caller.cb.alerts(&r->acc.alerts, caller.user);

  wxg_req_release(r);
}

static void
wxg_alerts_done(const curl_response_t *resp)
{
  wxg_request_t             *r   = (wxg_request_t *)resp->user_data;
  weathergov_alert_result_t *res = &r->acc.alerts;
  struct json_object        *root;
  struct json_object        *features;
  wxg_alert_refs_t           refs;
  wxg_alert_labels_t         seen;
  weathergov_alert_t         alert;
  int                        n;
  int                        i;

  res->covered = true;

  if(!wxg_http_status_ok(resp, res->err, sizeof(res->err), &res->covered))
  {
    // An uncovered coordinate answers 400 here where /points answers
    // 404. Neither is an error a user hears; both mean "ask the other
    // provider".
    if(res->err[0] == '\0')
      clam(CLAM_DEBUG2, WXG_CTX, "alerts %s: outside NWS coverage (HTTP %ld)",
          r->coord, resp->status);

    wxg_alerts_deliver(r);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, WXG_CTX);

  if(root == NULL)
  {
    snprintf(res->err, sizeof(res->err),
        "weather.gov returned an unreadable response.");
    wxg_alerts_deliver(r);
    return;
  }

  features = json_get_array(root, "features");

  if(features == NULL)
  {
    // A covered point with nothing active answers `features: []`, so a
    // missing array is a shape surprise rather than quiet weather. It
    // still costs the user nothing: an empty set reads as "no alerts".
    json_object_put(root);
    clam(CLAM_DEBUG, WXG_CTX, "alerts %s: response carried no features[]",
        r->coord);
    wxg_alerts_deliver(r);
    return;
  }

  wxg_refs_collect(features, &refs);

  seen.count = 0;
  n          = (int)json_object_array_length(features);

  for(i = 0; i < n && i < WXG_ALERT_FEATURE_MAX; i++)
  {
    struct json_object *feat = json_object_array_get_idx(features, i);
    struct json_object *props;

    if(feat == NULL)
      continue;

    props = json_get_obj(feat, "properties");

    if(props == NULL)
      continue;

    wxg_alert_parse_one(props, &alert);

    if(alert.event[0] == '\0')
      continue;

    // Superseded by a later bulletin in this same response, or a
    // repeat of one already kept.
    if(wxg_refs_contains(&refs, alert.id) || !wxg_labels_add(&seen, &alert))
      continue;

    if(res->alerts.total < UINT8_MAX)
      res->alerts.total++;

    wxg_alert_set_insert(&res->alerts, &alert);
  }

  json_object_put(root);

  clam(CLAM_DEBUG2, WXG_CTX, "alerts %s: %d feature(s) -> %u distinct, "
      "%u carried", r->coord, n, res->alerts.total, res->alerts.count);

  wxg_alerts_deliver(r);
}

// ----------------------------------------------------------------------
// Public mechanism API
// ----------------------------------------------------------------------

bool
weathergov_alerts_async(double lat, double lon,
    weathergov_alerts_cb_t cb, void *user)
{
  char url[WXG_URL_SZ];
  wxg_request_t *r;

  if(cb == NULL || !weathergov_enabled())
    return(FAIL);

  r = wxg_req_alloc();
  r->type      = WXG_REQ_ALERTS;
  r->lat       = lat;
  r->lon       = lon;
  r->cb.alerts = cb;
  r->user      = user;

  // Rounded to four places like every other weather.gov URL tail: the
  // full-precision form answers 301 to exactly this string.
  snprintf(r->coord, sizeof(r->coord), "%.4f,%.4f", lat, lon);
  wxg_ua(r->ua, sizeof(r->ua));

  // File it before anything can be submitted, never after: a completion
  // can run on a curl worker before the submitting call has returned.
  wxg_req_track(r);

  snprintf(url, sizeof(url), "%s?point=%s", WXG_ALERTS_URL, r->coord);

  if(wxg_http_get(url, r->ua, wxg_alerts_done, r) != SUCCESS)
  {
    wxg_req_release(r);
    return(FAIL);
  }

  return(SUCCESS);
}
