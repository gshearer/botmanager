// botmanager — MIT
// Cases for the weather.gov alert geometry: the one place in that
// plugin where being wrong is SILENT.
//
// weather.gov answers at two resolutions in one response and the
// payload does not say which. A zone product covers every county it
// names; a storm-based product covers only what is inside its polygon,
// while still naming the whole county in `areaDesc`. Getting that
// backwards does not crash and does not log — it reaches a channel as a
// tornado warning claimed for a ZIP nine kilometres clear of one, or as
// silence over a ZIP the polygon actually clips.
//
// The polygons below are REAL: they are the bulletins captured live at
// 20:19 EDT on 2026-09-20 as a tornado crossed Butler into Warren
// county, Ohio, and the distances are the ones the offline replay
// oracle beside that capture produces. They are here rather than read
// from it so this suite needs nothing outside the tree.
//
// Every row drives a distinct code path rather than a distinct input:
// absent geometry, null geometry, a non-areal type, a degenerate ring,
// containment, separation, and the MultiPolygon envelope.
#include "test.h"

#define WEATHERGOV_INTERNAL
#include "weathergov.h"

#include <stdio.h>
#include <string.h>

// The 20:19 Tornado Warning over Warren county — 5 vertices, verbatim.
#define TORNADO \
  "{\"geometry\":{\"type\":\"Polygon\",\"coordinates\":" \
  "[[[-84.33,39.41],[-84.33,39.55],[-84.02,39.52]," \
  "[-84.15,39.33],[-84.33,39.41]]]}}"

// The Special Weather Statement of the same minute: a Moderate product
// that clipped the north-west corner of a ZIP its centroid sat well
// inside. The milder form of the same defect.
#define STATEMENT \
  "{\"geometry\":{\"type\":\"Polygon\",\"coordinates\":" \
  "[[[-84.75,39.49],[-84.58,39.72],[-84.13,39.61],[-84.26,39.44]," \
  "[-84.54,39.32],[-84.75,39.49]]]}}"

// The county-wide Flood Watch. This is how EVERY zone product arrives.
#define ZONE_PRODUCT "{\"geometry\":null}"

#define MULTI \
  "{\"geometry\":{\"type\":\"MultiPolygon\",\"coordinates\":" \
  "[[[[-84.33,39.41],[-84.33,39.55],[-84.02,39.52]," \
  "[-84.15,39.33],[-84.33,39.41]]]]}}"

typedef struct
{
  const char *name;
  const char *feature;
  double      lat;
  double      lon;
  const char *want;     // "<Z|O|N>:<km, 1dp>:<bearing>"
} geo_case_t;

// The two coordinates are the OpenWeather centroids the command
// actually resolves these ZIPs to — not points chosen to pass.
#define MASON_LAT         39.3464
#define MASON_LON        -84.3004
#define WEST_CHESTER_LAT  39.3402
#define WEST_CHESTER_LON -84.3998

static const geo_case_t geo_cases[] = {
  { "zone product covers what it names",
    ZONE_PRODUCT, MASON_LAT, MASON_LON, "Z:0.0:" },

  { "a feature with no geometry key at all",
    "{}", MASON_LAT, MASON_LON, "Z:0.0:" },

  { "a non-areal geometry is not a footprint",
    "{\"geometry\":{\"type\":\"Point\",\"coordinates\":[-84.3,39.4]}}",
    MASON_LAT, MASON_LON, "Z:0.0:" },

  { "two vertices are not a polygon",
    "{\"geometry\":{\"type\":\"Polygon\",\"coordinates\":"
    "[[[-84.33,39.41],[-84.02,39.52]]]}}",
    MASON_LAT, MASON_LON, "Z:0.0:" },

  { "a point inside the warning is overhead",
    TORNADO, 39.40, -84.28, "O:0.0:" },

  // The proof case. The command printed a Flood Watch and nothing else
  // while this warning was aloft over the north of the ZIP.
  { "45040's centroid is outside the tornado polygon",
    TORNADO, MASON_LAT, MASON_LON, "N:4.9:NE" },

  // The operator's phone claimed this one; the bot was right not to.
  { "45069's centroid is nine kilometres clear of it",
    TORNADO, WEST_CHESTER_LAT, WEST_CHESTER_LON, "N:9.8:NE" },

  { "the Moderate statement clipping 45069's corner",
    STATEMENT, WEST_CHESTER_LAT, WEST_CHESTER_LON, "N:3.9:NW" },

  { "a MultiPolygon is answered by its first ring",
    MULTI, MASON_LAT, MASON_LON, "N:4.9:NE" },
};

static void
render(const weathergov_alert_t *a, char *out, size_t cap)
{
  char kind = '?';

  switch(a->proximity)
  {
    case WEATHERGOV_PROX_ZONE:     kind = 'Z'; break;
    case WEATHERGOV_PROX_OVERHEAD: kind = 'O'; break;
    case WEATHERGOV_PROX_NEARBY:   kind = 'N'; break;
  }

  snprintf(out, cap, "%c:%.1f:%s", kind, a->distance_km, a->bearing);
}

int
main(void)
{
  size_t i;

  for(i = 0; i < sizeof(geo_cases) / sizeof(geo_cases[0]); i++)
  {
    const geo_case_t   *c = &geo_cases[i];
    struct json_object *feat;
    weathergov_alert_t  alert;
    char                got[64];

    feat = json_parse_buf(c->feature, strlen(c->feature), "test");

    if(feat == NULL)
    {
      test_check_str("geo", c->name, c->want, "<unparseable fixture>");
      continue;
    }

    memset(&alert, 0, sizeof(alert));
    wxg_alert_locate(feat, c->lat, c->lon, &alert);
    render(&alert, got, sizeof(got));

    test_check_str("geo", c->name, c->want, got);

    json_object_put(feat);
  }

  return(test_report("wxg_geo"));
}
