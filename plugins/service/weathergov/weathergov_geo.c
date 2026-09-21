// botmanager — MIT
// weather.gov alert geometry: is this bulletin over you, or over your county?
#define WEATHERGOV_INTERNAL
#define WXG_GEO_TU
#include "weathergov.h"

#include <math.h>
#include <string.h>

// ----------------------------------------------------------------------
// Geometry — is this alert over you, or merely over your county?
// ----------------------------------------------------------------------
//
// ⚠⚠ A CAP response answers at two resolutions at once and the payload
// does not say which. A zone product (Flood Watch, Beach Hazards
// Statement) carries `geometry: null` and covers every UGC zone it
// names. A storm-based product carries a polygon and covers only what
// is inside it — while still naming the whole county in `areaDesc` and
// `geocode.UGC`. So the UGC list scopes the QUERY and the polygon
// decides the ANSWER, and reasoning about coverage from `areaDesc` is
// how a county-keyed consumer claims a tornado three kilometres away.
//
// Everything here works in degrees, projected to kilometres only where
// a distance is wanted. An alert polygon spans tens of kilometres, so
// the flat approximation is exact enough to print and a projection
// library would be a dependency bought for one line of output.

// Kilometres per degree of latitude. Longitude shrinks by cos(lat) and
// is computed per call, because the caller's latitude is the only one
// that matters.
#define WXG_KM_PER_DEG_LAT  111.19

static const char *const wxg_compass[8] = {
  "N", "NE", "E", "SE", "S", "SW", "W", "NW"
};

// The outer ring of an alert's footprint, or NULL when it has none.
//
// NULL is the ZONE case and is not a failure: it is how every
// county/zone bulletin arrives. A MultiPolygon is answered by its first
// polygon — NWS issues one shape per storm-based product and the
// multi form appears only where a single warning was split across a
// discontinuity, where the first part is the part we are near.
struct json_object *
wxg_ring_first(struct json_object *feat)
{
  struct json_object *geom = json_get_obj(feat, "geometry");
  struct json_object *coords;
  struct json_object *ring;
  char                type[24];

  if(geom == NULL || !json_get_str(geom, "type", type, sizeof(type)))
    return(NULL);

  coords = json_get_array(geom, "coordinates");

  if(coords == NULL || json_object_array_length(coords) == 0)
    return(NULL);

  ring = json_object_array_get_idx(coords, 0);

  if(strcmp(type, "MultiPolygon") == 0)
  {
    if(ring == NULL || json_object_array_length(ring) == 0)
      return(NULL);

    ring = json_object_array_get_idx(ring, 0);
  }

  else if(strcmp(type, "Polygon") != 0)
    return(NULL);

  // Fewer than three vertices is not a polygon; treating it as one
  // would make the ray cast read past the ring it was handed.
  if(ring == NULL || !json_object_is_type(ring, json_type_array)
      || json_object_array_length(ring) < 3)
    return(NULL);

  return(ring);
}

// One [lon, lat] vertex. GeoJSON is longitude-first and NWS is GeoJSON;
// false means the vertex is malformed and the edge is skipped.
static bool
wxg_ring_vertex(struct json_object *ring, size_t i, double *lon, double *lat)
{
  struct json_object *pt = json_object_array_get_idx(ring, i);
  struct json_object *x;
  struct json_object *y;

  if(pt == NULL || !json_object_is_type(pt, json_type_array)
      || json_object_array_length(pt) < 2)
    return(false);

  x = json_object_array_get_idx(pt, 0);
  y = json_object_array_get_idx(pt, 1);

  if(x == NULL || y == NULL)
    return(false);

  *lon = json_object_get_double(x);
  *lat = json_object_get_double(y);

  return(true);
}

// Even-odd ray cast. `wxg_ring_first` has already established at least
// three vertices, which is what makes the `n - 1` seed safe.
bool
wxg_point_in_ring(struct json_object *ring, double lon, double lat)
{
  size_t n      = (size_t)json_object_array_length(ring);
  bool   inside = false;
  size_t i;
  size_t j;

  for(i = 0, j = n - 1; i < n; j = i++)
  {
    double xi;
    double yi;
    double xj;
    double yj;

    if(!wxg_ring_vertex(ring, i, &xi, &yi)
        || !wxg_ring_vertex(ring, j, &xj, &yj))
      continue;

    if((yi > lat) != (yj > lat)
        && lon < (xj - xi) * (lat - yi) / (yj - yi) + xi)
      inside = !inside;
  }

  return(inside);
}

// An east/north offset to one of eight compass points.
static uint8_t
wxg_compass_idx(double east, double north)
{
  double deg = atan2(east, north) * 180.0 / M_PI;

  if(deg < 0.0)
    deg += 360.0;

  return((uint8_t)(((unsigned)((deg + 22.5) / 45.0)) % 8u));
}

// Distance in kilometres from a coordinate to the nearest edge of a
// ring, and the direction that edge lies in. The coordinate is the
// origin, so the closest point on each segment is also the vector to
// it.
void
wxg_ring_nearest(struct json_object *ring, double lon, double lat,
    double *km_out, char *bearing, size_t bearing_sz)
{
  const double km_lon = WXG_KM_PER_DEG_LAT * cos(lat * M_PI / 180.0);
  size_t       n      = (size_t)json_object_array_length(ring);
  double       best   = -1.0;
  double       best_e = 0.0;
  double       best_n = 0.0;
  size_t       i;
  size_t       j;

  *km_out    = 0.0;
  bearing[0] = '\0';

  for(i = 0, j = n - 1; i < n; j = i++)
  {
    double xi;
    double yi;
    double xj;
    double yj;
    double ax;
    double ay;
    double dx;
    double dy;
    double len;
    double t;
    double ex;
    double en;
    double d;

    if(!wxg_ring_vertex(ring, i, &xi, &yi)
        || !wxg_ring_vertex(ring, j, &xj, &yj))
      continue;

    ax  = (xi - lon) * km_lon;
    ay  = (yi - lat) * WXG_KM_PER_DEG_LAT;
    dx  = (xj - lon) * km_lon - ax;
    dy  = (yj - lat) * WXG_KM_PER_DEG_LAT - ay;
    len = dx * dx + dy * dy;
    t   = (len > 0.0) ? -(ax * dx + ay * dy) / len : 0.0;

    if(t < 0.0)
      t = 0.0;

    else if(t > 1.0)
      t = 1.0;

    ex = ax + t * dx;
    en = ay + t * dy;
    d  = hypot(ex, en);

    if(best >= 0.0 && d >= best)
      continue;

    best   = d;
    best_e = ex;
    best_n = en;
  }

  if(best < 0.0)
    return;

  *km_out = best;
  strlcpy(bearing, wxg_compass[wxg_compass_idx(best_e, best_n)], bearing_sz);
}

// Where this alert sits relative to the coordinate the caller asked
// about. Run against a `?point=` response every answer is OVERHEAD or
// ZONE, because the server already applied the same test; run against a
// `?zone=` one, NEARBY is the class that query exists to surface.
void
wxg_alert_locate(struct json_object *feat, double lat, double lon,
    weathergov_alert_t *out)
{
  struct json_object *ring = wxg_ring_first(feat);

  if(ring == NULL)
  {
    out->proximity = WEATHERGOV_PROX_ZONE;
    return;
  }

  if(wxg_point_in_ring(ring, lon, lat))
  {
    out->proximity = WEATHERGOV_PROX_OVERHEAD;
    return;
  }

  out->proximity = WEATHERGOV_PROX_NEARBY;

  wxg_ring_nearest(ring, lon, lat, &out->distance_km, out->bearing,
      sizeof(out->bearing));
}
