// botmanager — MIT
// Whenmoon WM-BT-8 Lightweight Charts HTML emitter.
//
// One self-contained HTML file per matched buy→sell trade pair per
// subscribed grain for each top-N iteration in a sweep. Indicator
// overlays are hard-coded to SMA_20 / SMA_50 / EMA_20 — pre-baked
// slots in `wm_candle_full_t::ind[]` that the aggregator computes on
// every bar close. Per-strategy overlay declarations are a later
// refinement.
//
// File layout:
//   <h1> trade header (entry/exit timestamps + prices + pnl)
//   <div id=chart>     — full-height candlestick + overlay container
//   Lightweight Charts CDN script (~50 KB, browser-cached after first
//   load across all charts in a sweep).
//   Inlined JSON literals for candles, overlay series, and markers.
//
// Atomic write: render through fopen on a `<final>.tmp` path, then
// fflush + fsync + close + rename. Partial files are never visible
// to the user.

#define WHENMOON_INTERNAL
#include "wm_bt_chart.h"

#include "wm_bt_assets.h"
#include "common.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Lightweight Charts v4 CDN. Pinned major-version to avoid v5 API
// drift breaking sweep artifacts; browser caches across files.
#define WM_BT_CHART_LIB_URL \
    "https://unpkg.com/lightweight-charts@4" \
    "/dist/lightweight-charts.standalone.production.js"

// ----------------------------------------------------------------------- //
// Grain name                                                              //
// ----------------------------------------------------------------------- //

const char *
wm_bt_chart_gran_name(wm_gran_t g)
{
  switch(g)
  {
    case WM_GRAN_1M:  return("1m");
    case WM_GRAN_5M:  return("5m");
    case WM_GRAN_15M: return("15m");
    case WM_GRAN_1H:  return("1h");
    case WM_GRAN_4H:  return("4h");
    case WM_GRAN_1D:  return("1d");
    case WM_GRAN_MAX: break;
  }

  return("?");
}

// ----------------------------------------------------------------------- //
// Atomic finalize (fflush + fsync + rename)                               //
// ----------------------------------------------------------------------- //
//
// Mirrors `wm_bt_finalize_file` in wm_bt_report.c — duplicated rather
// than promoted because the pattern is small and the cross-file cost
// of a new public surface for one extra caller isn't worth it.

static bool
wm_bt_chart_finalize_file(FILE *fp, const char *tmp, const char *final_path,
    char *err, size_t err_cap)
{
  int fd;

  if(fp == NULL)
    return(FAIL);

  if(fflush(fp) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fflush('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  fd = fileno(fp);

  if(fd >= 0 && fsync(fd) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "fsync('%s') failed: %s", tmp, strerror(errno));
    fclose(fp);
    unlink(tmp);
    return(FAIL);
  }

  fclose(fp);

  if(rename(tmp, final_path) != 0)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "rename('%s' → '%s') failed: %s",
          tmp, final_path, strerror(errno));
    unlink(tmp);
    return(FAIL);
  }

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// JSON series emitters                                                    //
// ----------------------------------------------------------------------- //

// Candle array: `[{time:<unix-s>, open, high, low, close}, ...]`.
// Lightweight Charts requires strictly-ascending unique `time` values;
// the source ring is already chronological, so we just walk it.
static void
wm_bt_chart_emit_candles(FILE *fp,
    const wm_candle_full_t *bars, uint32_t n_bars)
{
  uint32_t i;

  fputc('[', fp);

  for(i = 0; i < n_bars; i++)
  {
    const wm_candle_full_t *b = &bars[i];
    int64_t                 t_sec = b->ts_close_ms / 1000;

    if(i > 0)
      fputc(',', fp);

    fprintf(fp,
        "{\"time\":%" PRId64 ",\"open\":%.6f,\"high\":%.6f,"
        "\"low\":%.6f,\"close\":%.6f}",
        t_sec, b->open, b->high, b->low, b->close);
  }

  fputc(']', fp);
}

// Overlay line series: `[{time, value}, ...]`. Skips non-finite slots
// (NaN/Inf) so the renderer doesn't break — Lightweight Charts treats
// any NaN as a hard error and refuses to draw the whole series.
static void
wm_bt_chart_emit_line(FILE *fp,
    const wm_candle_full_t *bars, uint32_t n_bars, uint32_t slot)
{
  uint32_t i;
  bool     first = true;

  fputc('[', fp);

  for(i = 0; i < n_bars; i++)
  {
    float v = bars[i].ind[slot];

    if(!isfinite((double)v))
      continue;

    if(!first)
      fputc(',', fp);

    first = false;
    fprintf(fp, "{\"time\":%" PRId64 ",\"value\":%.6f}",
        bars[i].ts_close_ms / 1000, (double)v);
  }

  fputc(']', fp);
}

// Markers: entry (buy) = aboveBar yellow arrowUp; exit (sell) =
// belowBar red arrowDown. The optional exit_fill is omitted when the
// trade never closed.
static void
wm_bt_chart_emit_markers(FILE *fp,
    const wm_market_fill_t *entry_fill,
    const wm_market_fill_t *exit_fill)
{
  fputc('[', fp);

  fprintf(fp,
      "{\"time\":%" PRId64 ",\"position\":\"aboveBar\","
      "\"color\":\"#f5d442\",\"shape\":\"arrowUp\","
      "\"text\":\"BUY %.6f @ %.4f\"}",
      entry_fill->ts_ms / 1000, entry_fill->qty, entry_fill->price);

  if(exit_fill != NULL)
  {
    fprintf(fp,
        ",{\"time\":%" PRId64 ",\"position\":\"belowBar\","
        "\"color\":\"#e8503a\",\"shape\":\"arrowDown\","
        "\"text\":\"SELL %.6f @ %.4f\"}",
        exit_fill->ts_ms / 1000, exit_fill->qty, exit_fill->price);
  }

  fputc(']', fp);
}

// ----------------------------------------------------------------------- //
// HTML template                                                           //
// ----------------------------------------------------------------------- //

// Format `ts_ms` as "YYYY-MM-DD HH:MM:SS UTC". Falls back to numeric
// epoch on gmtime_r failure (which shouldn't happen for any realistic
// ts but keep the path explicit).
static void
wm_bt_chart_fmt_ts(int64_t ts_ms, char *out, size_t cap)
{
  time_t    t = (time_t)(ts_ms / 1000);
  struct tm tm;

  if(gmtime_r(&t, &tm) == NULL)
  {
    snprintf(out, cap, "%" PRId64, ts_ms);
    return;
  }

  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d UTC",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void
wm_bt_chart_write_html(FILE *fp,
    uint32_t iter_idx, uint32_t trade_idx, wm_gran_t gran,
    const wm_candle_full_t *bars, uint32_t n_bars,
    const wm_market_fill_t *entry_fill,
    const wm_market_fill_t *exit_fill)
{
  const char *gname = wm_bt_chart_gran_name(gran);
  char        entry_ts[64];
  char        exit_ts[64];
  char        title[96];

  wm_bt_chart_fmt_ts(entry_fill->ts_ms, entry_ts, sizeof(entry_ts));

  if(exit_fill != NULL)
    wm_bt_chart_fmt_ts(exit_fill->ts_ms, exit_ts, sizeof(exit_ts));
  else
    snprintf(exit_ts, sizeof(exit_ts), "(open)");

  // Shared head + design system (wm_bt_assets.c). The chart page sits two
  // levels under the sweep dir (charts/iter-K/), so it links the assets
  // via ../../; the page-specific layout (#chart height, the bare-<h1>
  // trade header) lives in report.css. Title components are integers + a
  // fixed grain token, so no HTML escaping is needed.
  snprintf(title, sizeof(title), "iter %u trade %u %s",
      iter_idx, trade_idx, gname);
  wm_bt_html_doc_open(fp, title, "../../assets/report.css");
  fputs("<script defer src=\"../../assets/report.js\"></script>\n", fp);

  fprintf(fp,
      "<h1>iter %u &middot; trade %u &middot; %s &middot;"
      " entry %s @ %.4f &middot; exit %s",
      iter_idx, trade_idx, gname, entry_ts, entry_fill->price, exit_ts);

  if(exit_fill != NULL)
  {
    fprintf(fp, " @ %.4f &middot; pnl %+.4f",
        exit_fill->price, exit_fill->realized_pnl);
  }

  fputs("</h1>\n<div id=\"chart\"></div>\n", fp);

  fprintf(fp, "<script src=\"%s\"></script>\n", WM_BT_CHART_LIB_URL);

  // Chart instance + dark layout.
  fputs(
      "<script>\n"
      "const chart = LightweightCharts.createChart("
      "document.getElementById('chart'),"
      "{layout:{background:{color:'#0a0a0a'},textColor:'#ddd'},"
      "grid:{vertLines:{color:'#1a1a1a'},"
      "horzLines:{color:'#1a1a1a'}}});\n", fp);

  // Candlestick series.
  fputs("const candles = chart.addCandlestickSeries();\n"
        "candles.setData(", fp);
  wm_bt_chart_emit_candles(fp, bars, n_bars);
  fputs(");\n", fp);

  // SMA 20 overlay.
  fputs("const sma20 = chart.addLineSeries("
        "{color:'#5af',lineWidth:1,title:'SMA 20'});\n"
        "sma20.setData(", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_SMA_20);
  fputs(");\n", fp);

  // SMA 50 overlay.
  fputs("const sma50 = chart.addLineSeries("
        "{color:'#fa5',lineWidth:1,title:'SMA 50'});\n"
        "sma50.setData(", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_SMA_50);
  fputs(");\n", fp);

  // EMA 20 overlay.
  fputs("const ema20 = chart.addLineSeries("
        "{color:'#5fa',lineWidth:1,title:'EMA 20'});\n"
        "ema20.setData(", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_EMA_20);
  fputs(");\n", fp);

  // Entry / exit markers anchored on the candle series.
  fputs("candles.setMarkers(", fp);
  wm_bt_chart_emit_markers(fp, entry_fill, exit_fill);
  fputs(");\n", fp);

  // Fit + epilogue.
  fputs(
      "chart.timeScale().fitContent();\n"
      "</script>\n"
      "</body></html>\n", fp);
}

// ----------------------------------------------------------------------- //
// Public emit                                                             //
// ----------------------------------------------------------------------- //

bool
wm_bt_chart_emit(const char *dir,
    uint32_t iter_idx, uint32_t trade_idx, wm_gran_t gran,
    const wm_candle_full_t *bars, uint32_t n_bars,
    const wm_market_fill_t *entry_fill,
    const wm_market_fill_t *exit_fill,
    char *err, size_t err_cap)
{
  char        final_path[1024];
  char        tmp_path[1024];
  const char *gname;
  FILE       *fp;
  int         n;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(dir == NULL || dir[0] == '\0' || bars == NULL ||
     n_bars == 0 || entry_fill == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "chart_emit: bad args");
    return(FAIL);
  }

  gname = wm_bt_chart_gran_name(gran);

  n = snprintf(final_path, sizeof(final_path),
      "%s/trade-%u-%s.html", dir, trade_idx, gname);

  if(n < 0 || (size_t)n >= sizeof(final_path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "chart path overflow");
    return(FAIL);
  }

  n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

  if(n < 0 || (size_t)n >= sizeof(tmp_path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "chart tmp path overflow");
    return(FAIL);
  }

  fp = fopen(tmp_path, "w");

  if(fp == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "fopen('%s') failed: %s",
          tmp_path, strerror(errno));
    return(FAIL);
  }

  wm_bt_chart_write_html(fp, iter_idx, trade_idx, gran, bars, n_bars,
      entry_fill, exit_fill);

  return(wm_bt_chart_finalize_file(fp, tmp_path, final_path,
      err, err_cap));
}
