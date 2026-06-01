// botmanager — MIT
// Whenmoon backtest per-trade chart HTML emitter (WM-BT-8, enriched in
// WM-BT-RPT-4).
//
// One self-contained HTML file per matched buy→sell trade pair per
// snapshot grain for each top-N iteration in a sweep. The price chart
// overlays SMA_20 / SMA_50 / EMA_20 (pre-baked `wm_candle_full_t::ind[]`
// slots the aggregator computes on every bar close) and adds volume,
// RSI(14), and MACD sub-panes when those slots carry finite data.
//
// File layout (WM-BT-RPT-4):
//   <header>           — grain / trade-of / iter + entry→exit summary
//   <nav class=tnav>   — prev/next trade, grain switcher, ↑ index
//   .wrap
//     .cards           — entry / exit / held / qty / P&L $+% / reason
//     <figure role=img aria-label=…>  — stacked LWC panes:
//        #tc-price (candles + overlays + entry/exit guides + markers),
//        #tc-vol, #tc-rsi, #tc-macd (emitted only when data present)
//     <table.visually-hidden>          — OHLC fallback for screen readers
//   <script type=application/json id=trade-data> — all series, read by
//     report.js wmRenderTradeChart() (no executable JS in the body).
//   Lightweight Charts CDN script (~50 KB, browser-cached across a sweep).
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

// WM_BT_CHART_LIB_URL moved to wm_bt_chart.h (WM-BT-RPT-2) so the
// single-config index head can load the same Lightweight Charts build.

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

// Volume histogram series: `[{time,value,color}, ...]`. Up bars
// (close >= open) are tinted --win, down bars --loss, both at low alpha
// so the histogram reads as a backdrop. Non-finite / negative volumes
// are skipped (Lightweight Charts rejects a whole series on one bad
// point, same as the line emitter).
static void
wm_bt_chart_emit_volume(FILE *fp,
    const wm_candle_full_t *bars, uint32_t n_bars)
{
  uint32_t i;
  bool     first = true;

  fputc('[', fp);

  for(i = 0; i < n_bars; i++)
  {
    const wm_candle_full_t *b = &bars[i];

    if(!isfinite(b->volume) || b->volume < 0.0)
      continue;

    if(!first)
      fputc(',', fp);

    first = false;
    fprintf(fp,
        "{\"time\":%" PRId64 ",\"value\":%.6f,\"color\":\"%s\"}",
        b->ts_close_ms / 1000, b->volume,
        (b->close >= b->open) ? "rgba(43,182,115,0.45)"
                              : "rgba(224,83,61,0.45)");
  }

  fputc(']', fp);
}

// Signed histogram series for one indicator slot (the MACD histogram):
// `[{time,value,color}, ...]`, --win when >= 0 else --loss. NaN-skipped
// like wm_bt_chart_emit_line.
static void
wm_bt_chart_emit_hist(FILE *fp,
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
    fprintf(fp,
        "{\"time\":%" PRId64 ",\"value\":%.6f,\"color\":\"%s\"}",
        bars[i].ts_close_ms / 1000, (double)v,
        (v >= 0.0f) ? "rgba(43,182,115,0.6)" : "rgba(224,83,61,0.6)");
  }

  fputc(']', fp);
}

// True if any bar in the slice carries a finite value in indicator
// `slot`. Drives the "skip an all-NaN sub-pane entirely" rule — an
// oscillator with no finite sample over the window gets no empty box.
static bool
wm_bt_slice_has_finite(const wm_candle_full_t *bars, uint32_t n_bars,
    uint32_t slot)
{
  uint32_t i;

  for(i = 0; i < n_bars; i++)
    if(isfinite((double)bars[i].ind[slot]))
      return(true);

  return(false);
}

// True if any bar carries a finite, non-negative volume.
static bool
wm_bt_slice_has_volume(const wm_candle_full_t *bars, uint32_t n_bars)
{
  uint32_t i;

  for(i = 0; i < n_bars; i++)
    if(isfinite(bars[i].volume) && bars[i].volume >= 0.0)
      return(true);

  return(false);
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

// Human-readable span between two epoch-ms instants: "3d 4h", "5h 12m",
// or "47m" (sub-minute => "0m"). Always NUL-terminates.
static void
wm_bt_chart_fmt_dur(int64_t ms, char *out, size_t cap)
{
  long secs;
  long d;
  long h;
  long m;

  if(out == NULL || cap == 0)
    return;

  if(ms < 0)
    ms = 0;

  secs = (long)(ms / 1000);
  d    = secs / 86400;  secs %= 86400;
  h    = secs / 3600;   secs %= 3600;
  m    = secs / 60;

  if(d > 0)
    snprintf(out, cap, "%ldd %ldh", d, h);
  else if(h > 0)
    snprintf(out, cap, "%ldh %ldm", h, m);
  else
    snprintf(out, cap, "%ldm", m);
}

// ----------------------------------------------------------------------- //
// Info cards + nav                                                        //
// ----------------------------------------------------------------------- //

// Structured entry/exit/held/qty/P&L/reason cards (replaces the old
// run-on <h1>). Reuses the shared .cards/.card design-system block.
// P&L % is realized_pnl over the entry notional (qty x entry price);
// the entry notional is always positive for a long-only buy, so the %
// is finite in practice (guarded anyway).
static void
wm_bt_chart_emit_info_cards(FILE *fp,
    const wm_market_fill_t *entry, const wm_market_fill_t *exit_fill,
    wm_gran_t gran)
{
  char entry_px[48];
  char qty[48];

  (void)gran;

  wm_bt_fmt_num(entry->price, 2, entry_px, sizeof(entry_px));
  wm_bt_fmt_num(entry->qty,   6, qty,      sizeof(qty));

  fputs("<div class=\"cards\">\n", fp);

  fprintf(fp,
      "<div class=\"card\"><div class=\"k\">Entry</div>"
      "<div class=\"v mono\">%s</div></div>\n", entry_px);

  if(exit_fill != NULL)
  {
    char exit_px[48];

    wm_bt_fmt_num(exit_fill->price, 2, exit_px, sizeof(exit_px));
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Exit</div>"
        "<div class=\"v mono\">%s</div></div>\n", exit_px);
  }
  else
  {
    fputs("<div class=\"card\"><div class=\"k\">Exit</div>"
          "<div class=\"v mono muted\">(open)</div></div>\n", fp);
  }

  if(exit_fill != NULL)
  {
    char held[48];

    wm_bt_chart_fmt_dur(exit_fill->ts_ms - entry->ts_ms,
        held, sizeof(held));
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Held</div>"
        "<div class=\"v mono\">%s</div></div>\n", held);
  }
  else
  {
    fputs("<div class=\"card\"><div class=\"k\">Held</div>"
          "<div class=\"v mono muted\">&mdash;</div></div>\n", fp);
  }

  fprintf(fp,
      "<div class=\"card\"><div class=\"k\">Qty</div>"
      "<div class=\"v mono\">%s</div></div>\n", qty);

  if(exit_fill != NULL)
  {
    char        pnl_usd[48];
    const char *cls   = (exit_fill->realized_pnl >= 0.0) ? "pos" : "neg";
    const char *psign = (exit_fill->realized_pnl >= 0.0) ? "+"   : "";
    double      notional = entry->qty * entry->price;

    wm_bt_fmt_usd(exit_fill->realized_pnl, pnl_usd, sizeof(pnl_usd));

    if(isfinite(notional) && notional > 0.0)
    {
      char   pnl_pct[48];
      double pct = exit_fill->realized_pnl / notional * 100.0;

      wm_bt_fmt_pct(pct, 2, pnl_pct, sizeof(pnl_pct));
      fprintf(fp,
          "<div class=\"card\"><div class=\"k\">P&amp;L</div>"
          "<div class=\"v mono %s\">%s%s &middot; %s%s</div></div>\n",
          cls, psign, pnl_usd, psign, pnl_pct);
    }
    else
    {
      fprintf(fp,
          "<div class=\"card\"><div class=\"k\">P&amp;L</div>"
          "<div class=\"v mono %s\">%s%s</div></div>\n",
          cls, psign, pnl_usd);
    }
  }
  else
  {
    fputs("<div class=\"card\"><div class=\"k\">P&amp;L</div>"
          "<div class=\"v mono muted\">(open)</div></div>\n", fp);
  }

  if(exit_fill != NULL && exit_fill->reason[0] != '\0')
  {
    char reason_esc[160];

    wm_bt_html_escape(exit_fill->reason, reason_esc, sizeof(reason_esc));
    fprintf(fp,
        "<div class=\"card\"><div class=\"k\">Exit reason</div>"
        "<div class=\"v txt\">%s</div></div>\n", reason_esc);
  }

  fputs("</div>\n", fp);
}

// Trade navigation: prev/next trade (same grain), grain switcher across
// every present grain, and ↑ back to the sweep index. Real <a href>s —
// every present grain charts every trade (the slice walker only drops an
// empty ring), so trade indices 1..n_trades and every grains_present bit
// resolve to a file on disk.
static void
wm_bt_chart_emit_nav(FILE *fp, uint32_t rank, uint32_t tidx,
    uint32_t n_trades, wm_gran_t gran, uint16_t grains_present)
{
  const char *cur = wm_bt_chart_gran_name(gran);
  uint32_t    g;

  (void)rank;

  fputs("<nav class=\"tnav\" aria-label=\"trade navigation\">\n", fp);

  if(tidx > 1)
    fprintf(fp,
        "<a class=\"navlink\" href=\"trade-%u-%s.html\">&#8592; prev</a>\n",
        tidx - 1, cur);
  else
    fputs("<span class=\"navlink off\">&#8592; prev</span>\n", fp);

  if(tidx < n_trades)
    fprintf(fp,
        "<a class=\"navlink\" href=\"trade-%u-%s.html\">next &#8594;</a>\n",
        tidx + 1, cur);
  else
    fputs("<span class=\"navlink off\">next &#8594;</span>\n", fp);

  fputs("<span class=\"sep\">|</span>\n"
        "<span class=\"grp\"><span class=\"lbl\">grain</span>\n", fp);

  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    const char *gn;

    if(((grains_present >> g) & 1u) == 0)
      continue;

    gn = wm_bt_chart_gran_name((wm_gran_t)g);

    if((wm_gran_t)g == gran)
      fprintf(fp,
          "<a class=\"navlink cur\" aria-current=\"page\""
          " href=\"trade-%u-%s.html\">%s</a>\n", tidx, gn, gn);
    else
      fprintf(fp,
          "<a class=\"navlink\" href=\"trade-%u-%s.html\">%s</a>\n",
          tidx, gn, gn);
  }

  fputs("</span>\n", fp);

  fputs("<span class=\"spacer\"></span>\n"
        "<a class=\"navlink\" href=\"../../index.html\">"
        "&#8593; index</a>\n", fp);

  fputs("</nav>\n", fp);
}

// Cap on rows emitted into the visually-hidden OHLC fallback table. The
// table duplicates the candle data row-by-row, so a long 1m trade (a
// multi-week hold = tens of thousands of bars) would otherwise bloat the
// page. The aria-label already carries the trade summary; assistive tech
// gets the first WM_BT_OHLC_TABLE_MAX bars plus an elision caption.
#define WM_BT_OHLC_TABLE_MAX 2000u

// Visually-hidden OHLC data table — the screen-reader fallback for the
// canvas chart (which is otherwise opaque to assistive tech). One row per
// bar in the slice (capped at WM_BT_OHLC_TABLE_MAX); .visually-hidden
// keeps it out of the visual layout.
static void
wm_bt_chart_emit_ohlc_table(FILE *fp,
    const wm_candle_full_t *bars, uint32_t n_bars)
{
  uint32_t shown = (n_bars > WM_BT_OHLC_TABLE_MAX)
      ? WM_BT_OHLC_TABLE_MAX : n_bars;
  uint32_t i;

  fputs("<table class=\"visually-hidden\">"
        "<caption>OHLC bars (accessibility fallback)</caption>\n"
        "<thead><tr><th>Time (UTC)</th><th>Open</th><th>High</th>"
        "<th>Low</th><th>Close</th><th>Volume</th></tr></thead>\n"
        "<tbody>\n", fp);

  for(i = 0; i < shown; i++)
  {
    const wm_candle_full_t *b = &bars[i];
    char                    ts[64];

    wm_bt_chart_fmt_ts(b->ts_close_ms, ts, sizeof(ts));
    fprintf(fp,
        "<tr><td>%s</td><td>%.4f</td><td>%.4f</td><td>%.4f</td>"
        "<td>%.4f</td><td>%.4f</td></tr>\n",
        ts, b->open, b->high, b->low, b->close, b->volume);
  }

  if(shown < n_bars)
    fprintf(fp,
        "<tr><td colspan=\"6\">… %u of %u bars shown; "
        "see the chart for the full series.</td></tr>\n",
        shown, n_bars);

  fputs("</tbody></table>\n", fp);
}

// Emit the `<script type="application/json" id="trade-data">` island
// read by report.js wmRenderTradeChart(). Every series is present (empty
// arrays where a slot is all-NaN); report.js gates each pane on both the
// host div existing and the array being non-empty.
static void
wm_bt_chart_emit_trade_json(FILE *fp,
    const wm_candle_full_t *bars, uint32_t n_bars,
    const wm_market_fill_t *entry_fill,
    const wm_market_fill_t *exit_fill)
{
  fputs("<script type=\"application/json\" id=\"trade-data\">\n", fp);
  fputc('{', fp);

  fputs("\"candles\":", fp);
  wm_bt_chart_emit_candles(fp, bars, n_bars);
  fputs(",\"sma20\":", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_SMA_20);
  fputs(",\"sma50\":", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_SMA_50);
  fputs(",\"ema20\":", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_EMA_20);
  fputs(",\"volume\":", fp);
  wm_bt_chart_emit_volume(fp, bars, n_bars);
  fputs(",\"rsi\":", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_RSI_14);
  fputs(",\"macd\":", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_MACD);
  fputs(",\"macdSignal\":", fp);
  wm_bt_chart_emit_line(fp, bars, n_bars, WM_IND_MACD_SIGNAL);
  fputs(",\"macdHist\":", fp);
  wm_bt_chart_emit_hist(fp, bars, n_bars, WM_IND_MACD_HIST);
  fputs(",\"markers\":", fp);
  wm_bt_chart_emit_markers(fp, entry_fill, exit_fill);

  fprintf(fp, ",\"entryPx\":%.6f", entry_fill->price);

  if(exit_fill != NULL)
    fprintf(fp, ",\"exitPx\":%.6f", exit_fill->price);
  else
    fputs(",\"exitPx\":null", fp);

  fputc('}', fp);
  fputs("\n</script>\n", fp);
}

static void
wm_bt_chart_write_html(FILE *fp,
    uint32_t iter_idx, uint32_t trade_idx, wm_gran_t gran,
    const wm_candle_full_t *bars, uint32_t n_bars,
    const wm_market_fill_t *entry_fill,
    const wm_market_fill_t *exit_fill,
    uint32_t n_trades, uint16_t grains_present)
{
  const char *gname = wm_bt_chart_gran_name(gran);
  char        entry_ts[64];
  char        exit_ts[64];
  char        title[96];
  char        aria[256];
  char        aria_esc[320];
  char        e_px[48];
  char        q[48];
  bool        has_vol  = wm_bt_slice_has_volume(bars, n_bars);
  bool        has_rsi  = wm_bt_slice_has_finite(bars, n_bars, WM_IND_RSI_14);
  bool        has_macd = wm_bt_slice_has_finite(bars, n_bars, WM_IND_MACD);

  wm_bt_chart_fmt_ts(entry_fill->ts_ms, entry_ts, sizeof(entry_ts));

  if(exit_fill != NULL)
    wm_bt_chart_fmt_ts(exit_fill->ts_ms, exit_ts, sizeof(exit_ts));
  else
    snprintf(exit_ts, sizeof(exit_ts), "(open)");

  // Shared head + design system (wm_bt_assets.c). The chart page sits two
  // levels under the sweep dir (charts/iter-K/), so it links the assets
  // via ../../. Title components are integers + a fixed grain token, so no
  // HTML escaping is needed; the body chart is built by report.js from the
  // #trade-data JSON island (no executable JS in this document body).
  snprintf(title, sizeof(title), "%s trade %u/%u", gname, trade_idx,
      n_trades);
  wm_bt_html_doc_open(fp, title, "../../assets/report.css");
  fputs("<script defer src=\"../../assets/report.js\"></script>\n", fp);

  fprintf(fp,
      "<header><h1>%s &middot; trade %u of %u &middot; iter %u</h1>\n"
      "<div class=\"sub\">entry %s @ %.4f",
      gname, trade_idx, n_trades, iter_idx, entry_ts, entry_fill->price);

  if(exit_fill != NULL)
    fprintf(fp, " &rarr; exit %s @ %.4f", exit_ts, exit_fill->price);
  else
    fputs(" &rarr; (open)", fp);

  fputs("</div></header>\n", fp);

  wm_bt_chart_emit_nav(fp, iter_idx, trade_idx, n_trades, gran,
      grains_present);

  fputs("<div class=\"wrap\" id=\"main\">\n", fp);

  wm_bt_chart_emit_info_cards(fp, entry_fill, exit_fill, gran);

  // Accessible chart summary (the canvas itself is opaque to AT).
  wm_bt_fmt_num(entry_fill->price, 2, e_px, sizeof(e_px));
  wm_bt_fmt_num(entry_fill->qty,   6, q,    sizeof(q));

  if(exit_fill != NULL)
  {
    char        x_px[48];
    char        p_pct[32];
    const char *psign = (exit_fill->realized_pnl >= 0.0) ? "+" : "";
    double      notional = entry_fill->qty * entry_fill->price;
    double      pct = (isfinite(notional) && notional > 0.0)
        ? exit_fill->realized_pnl / notional * 100.0 : 0.0;

    wm_bt_fmt_num(exit_fill->price, 2, x_px, sizeof(x_px));
    wm_bt_fmt_pct(pct, 1, p_pct, sizeof(p_pct));
    snprintf(aria, sizeof(aria),
        "%s trade %u of %u: BUY %s at %s then SELL at %s, %s%s",
        gname, trade_idx, n_trades, q, e_px, x_px, psign, p_pct);
  }
  else
  {
    snprintf(aria, sizeof(aria),
        "%s trade %u of %u: BUY %s at %s, position still open",
        gname, trade_idx, n_trades, q, e_px);
  }

  wm_bt_html_escape(aria, aria_esc, sizeof(aria_esc));

  fprintf(fp,
      "<figure class=\"tc-figure\" role=\"img\" aria-label=\"%s\">\n"
      "<div id=\"tc-price\" class=\"tc-pane\"></div>\n", aria_esc);

  if(has_vol)
    fputs("<div id=\"tc-vol\" class=\"tc-pane tc-sub\"></div>\n", fp);

  if(has_rsi)
    fputs("<div id=\"tc-rsi\" class=\"tc-pane tc-sub\"></div>\n", fp);

  if(has_macd)
    fputs("<div id=\"tc-macd\" class=\"tc-pane tc-sub\"></div>\n", fp);

  fputs("<figcaption class=\"tc-cap\">Candles + SMA20/50 + EMA20 with"
        " entry/exit guide lines; volume, RSI(14), and MACD panes share"
        " the time axis. Times in UTC. Per-fill marks only (no intra-bar"
        " mark-to-market).</figcaption>\n"
        "</figure>\n", fp);

  wm_bt_chart_emit_ohlc_table(fp, bars, n_bars);

  fputs("</div>\n", fp);

  wm_bt_chart_emit_trade_json(fp, bars, n_bars, entry_fill, exit_fill);

  fprintf(fp, "<script src=\"%s\"></script>\n", WM_BT_CHART_LIB_URL);
  fputs("</body></html>\n", fp);
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
    uint32_t n_trades, uint16_t grains_present,
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
      entry_fill, exit_fill, n_trades, grains_present);

  return(wm_bt_chart_finalize_file(fp, tmp_path, final_path,
      err, err_cap));
}
