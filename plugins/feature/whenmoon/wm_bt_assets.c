// botmanager — MIT
// Whenmoon backtest report shared assets: CSS/JS, <head> opener, C format
// helpers (WM-BT-RPT-1).

#define WHENMOON_INTERNAL
#include "wm_bt_assets.h"

#include "wm_bt_report.h"   // wm_bt_write_atomic, WM_BT_REPORT_DIR_MODE

#include "common.h"         // SUCCESS / FAIL

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// ----------------------------------------------------------------------- //
// Shared stylesheet                                                       //
// ----------------------------------------------------------------------- //
//
// The design-system palette + component rules were lifted verbatim out of
// the old inlined wm_bt_idx_write_head <style> block; the trailing block
// adds the Web-Interface-Guidelines baseline (color-scheme, focus-visible,
// tabular-nums, balanced headings, reduced-motion, print) and the
// per-trade chart-page rules (formerly the chart TU's own inline style).

static const char WM_BT_REPORT_CSS[] =
    ":root{--bg:#0e0f13;--surface:#171922;--surface2:#1e2230;"
    "--border:#2a2f3e;--text:#e6e8ee;--muted:#8b93a7;--accent:#5b8cff;"
    "--win:#2bb673;--loss:#e0533d;--gold:#f5c451}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;background:var(--bg);color:var(--text);"
    "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,"
    "Helvetica,Arial,sans-serif;line-height:1.5}\n"
    ".mono{font-family:ui-monospace,'SF Mono',Menlo,Consolas,monospace}\n"
    "header{position:sticky;top:0;z-index:10;background:"
    "rgba(14,15,19,.92);backdrop-filter:blur(8px);"
    "border-bottom:1px solid var(--border);padding:18px 28px}\n"
    "header h1{margin:0 0 2px;font-size:19px;font-weight:650}\n"
    "header .sub{color:var(--muted);font-size:13px}\n"
    ".wrap{max-width:1180px;margin:0 auto;padding:24px 28px 64px}\n"
    ".cards{display:grid;grid-template-columns:repeat(auto-fit,"
    "minmax(150px,1fr));gap:14px;margin:22px 0}\n"
    ".card{background:var(--surface);border:1px solid var(--border);"
    "border-radius:12px;padding:14px 16px}\n"
    ".card .k{color:var(--muted);font-size:12px;text-transform:uppercase;"
    "letter-spacing:.04em}\n"
    ".card .v{font-size:23px;font-weight:650;margin-top:4px}\n"
    ".card .v.mono{font-size:20px}\n"
    "section{margin:30px 0}\n"
    "h2{font-size:15px;font-weight:600;color:var(--muted);"
    "text-transform:uppercase;letter-spacing:.05em;"
    "border-bottom:1px solid var(--border);padding-bottom:8px}\n"
    "table{border-collapse:collapse;width:100%;font-size:13px}\n"
    "th,td{padding:7px 10px;text-align:left;border-bottom:1px solid "
    "var(--border);white-space:nowrap}\n"
    "th{color:var(--muted);font-weight:600;font-size:11px;"
    "text-transform:uppercase;letter-spacing:.03em}\n"
    ".num{text-align:right;font-family:ui-monospace,'SF Mono',Menlo,"
    "Consolas,monospace}\n"
    "td.reason{color:var(--muted);font-size:12px;white-space:normal}\n"
    ".meta dt{color:var(--muted);font-size:12px;text-transform:uppercase;"
    "letter-spacing:.03em}\n"
    ".meta{display:grid;grid-template-columns:repeat(auto-fit,"
    "minmax(220px,1fr));gap:10px 28px;margin:8px 0}\n"
    ".meta div{border-bottom:1px solid var(--border);padding:6px 0}\n"
    ".meta .vv{font-size:14px}\n"
    "details{background:var(--surface);border:1px solid var(--border);"
    "border-radius:12px;margin:14px 0;overflow:hidden}\n"
    "details[open]{box-shadow:0 0 0 1px var(--accent) inset}\n"
    "summary{cursor:pointer;padding:14px 18px;list-style:none;"
    "display:flex;align-items:center;gap:14px;flex-wrap:wrap}\n"
    "summary::-webkit-details-marker{display:none}\n"
    "summary .rank{background:var(--surface2);border:1px solid "
    "var(--border);border-radius:8px;padding:2px 10px;font-weight:700;"
    "font-size:13px}\n"
    "summary .params{font-size:13px}\n"
    "summary .spacer{flex:1}\n"
    ".badge{font-family:ui-monospace,monospace;font-size:13px;"
    "padding:2px 9px;border-radius:8px;border:1px solid var(--border)}\n"
    ".badge.win{color:var(--win);border-color:rgba(43,182,115,.4)}\n"
    ".badge.loss{color:var(--loss);border-color:rgba(224,83,61,.4)}\n"
    ".strip{display:flex;flex-wrap:wrap;gap:8px 22px;padding:4px 18px "
    "14px;border-bottom:1px solid var(--border)}\n"
    ".strip .it{font-size:13px}\n"
    ".strip .it span{color:var(--muted);margin-right:6px}\n"
    ".tbl-scroll{max-height:560px;overflow:auto;padding:0 6px 6px}\n"
    "tr.win td.pnl{color:var(--win)}\n"
    "tr.loss td.pnl{color:var(--loss)}\n"
    "tr.win:hover,tr.loss:hover,tr.open:hover{background:var(--surface2)}\n"
    ".pos{color:var(--win)}.neg{color:var(--loss)}\n"
    ".muted{color:var(--muted)}\n"
    "a{color:var(--accent);text-decoration:none}a:hover{text-decoration:"
    "underline}\n"
    ".chart-link{font-family:ui-monospace,monospace;font-size:12px;"
    "margin-right:8px}\n"
    ".note{color:var(--muted);font-size:12px;padding:10px 18px}\n"
    "footer{color:var(--muted);font-size:12px;margin-top:40px;"
    "border-top:1px solid var(--border);padding-top:16px}\n"
    // --- Web Interface Guidelines baseline (WM-BT-RPT-1) ---
    "html{color-scheme:dark}\n"
    ":focus-visible{outline:2px solid var(--accent);outline-offset:2px}\n"
    ".num{font-variant-numeric:tabular-nums}\n"
    "h1,h2{text-wrap:balance}\n"
    "@media (prefers-reduced-motion:reduce){*{animation:none!important;"
    "transition:none!important}}\n"
    "@media print{header{position:static}details{break-inside:avoid}"
    ".note{color:#444}}\n"
    // --- per-trade chart page (was the chart TU's inline <style>) ---
    "#chart{height:86vh}\n"
    "body>h1{margin:0;padding:14px 20px;font-size:14px;font-weight:600}\n";

// ----------------------------------------------------------------------- //
// Shared client bootstrap                                                 //
// ----------------------------------------------------------------------- //
//
// No-op until later chunks append render functions; provides the
// reduced-motion flag + Intl formatters they will reuse. Single-quoted
// throughout so the C string literal needs no escaping.

static const char WM_BT_REPORT_JS[] =
    "// whenmoon backtest report - shared client bootstrap (WM-BT-RPT-1).\n"
    "'use strict';\n"
    "const wmReduceMotion = (typeof window !== 'undefined'"
    " && window.matchMedia)\n"
    "  ? window.matchMedia('(prefers-reduced-motion: reduce)').matches"
    " : false;\n"
    "const wmFmtUsd = new Intl.NumberFormat('en-US',\n"
    "  {style:'currency',currency:'USD',maximumFractionDigits:0});\n"
    "const wmFmtNum = new Intl.NumberFormat('en-US');\n"
    "const wmFmtPct = new Intl.NumberFormat('en-US',\n"
    "  {style:'percent',minimumFractionDigits:1,maximumFractionDigits:1});\n"
    "function wmFmtDate(unixSec){\n"
    "  return new Date(unixSec*1000).toISOString().slice(0,16)"
    ".replace('T',' ');\n"
    "}\n";

// ----------------------------------------------------------------------- //
// Document head                                                           //
// ----------------------------------------------------------------------- //

void
wm_bt_html_doc_open(FILE *fp, const char *title_esc, const char *css_href)
{
  if(fp == NULL)
    return;

  if(title_esc == NULL)
    title_esc = "";

  if(css_href == NULL)
    css_href = "";

  fputs(
      "<!DOCTYPE html>\n"
      "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
      "<meta name=\"viewport\" content=\"width=device-width,"
      "initial-scale=1\">\n"
      "<meta name=\"theme-color\" content=\"#0e0f13\">\n", fp);

  fprintf(fp, "<title>%s</title>\n", title_esc);

  fputs("<link rel=\"preconnect\" href=\"https://unpkg.com\">\n", fp);

  fprintf(fp, "<link rel=\"stylesheet\" href=\"%s\">\n", css_href);

  fputs("</head><body>\n", fp);
}

// ----------------------------------------------------------------------- //
// Numeric formatting                                                      //
// ----------------------------------------------------------------------- //

// Group the all-digit string `digits` (no sign, no decimal point) into
// `out`, inserting ',' every three positions from the right:
// "1234567" -> "1,234,567". Always NUL-terminates; stops at the buffer
// edge.
static void
wm_bt_group_digits(const char *digits, char *out, size_t cap)
{
  size_t len;
  size_t first;
  size_t i;
  size_t o = 0;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  if(digits == NULL || digits[0] == '\0')
    return;

  len   = strlen(digits);
  first = (len % 3 == 0) ? 3 : (len % 3);

  for(i = 0; i < len; i++)
  {
    bool comma = (i >= first && (i - first) % 3 == 0);

    if(o + (comma ? 2u : 1u) + 1u > cap)
      break;

    if(comma)
      out[o++] = ',';

    out[o++] = digits[i];
  }

  out[o] = '\0';
}

void
wm_bt_fmt_usd(double v, char *out, size_t cap)
{
  char        digits[40];
  char        grouped[64];
  const char *sign = "";
  long long   iv;

  if(out == NULL || cap == 0)
    return;

  if(!isfinite(v))
  {
    snprintf(out, cap, "n/a");
    return;
  }

  if(v < 0.0)
  {
    sign = "-";
    v    = -v;
  }

  iv = (long long)(v + 0.5);

  snprintf(digits, sizeof(digits), "%lld", iv);
  wm_bt_group_digits(digits, grouped, sizeof(grouped));

  snprintf(out, cap, "%s$%s", sign, grouped);
}

void
wm_bt_fmt_num(double v, int frac, char *out, size_t cap)
{
  char        raw[48];
  char        grouped[64];
  const char *sign = "";
  char       *dot;

  if(out == NULL || cap == 0)
    return;

  if(!isfinite(v))
  {
    snprintf(out, cap, "n/a");
    return;
  }

  if(frac < 0)
    frac = 0;

  if(frac > 9)
    frac = 9;

  if(v < 0.0)
  {
    sign = "-";
    v    = -v;
  }

  snprintf(raw, sizeof(raw), "%.*f", frac, v);

  dot = strchr(raw, '.');

  if(dot != NULL)
    *dot = '\0';

  wm_bt_group_digits(raw, grouped, sizeof(grouped));

  if(dot != NULL)
    snprintf(out, cap, "%s%s.%s", sign, grouped, dot + 1);
  else
    snprintf(out, cap, "%s%s", sign, grouped);
}

void
wm_bt_fmt_pct(double v, int frac, char *out, size_t cap)
{
  if(out == NULL || cap == 0)
    return;

  if(!isfinite(v))
  {
    snprintf(out, cap, "n/a");
    return;
  }

  if(frac < 0)
    frac = 0;

  if(frac > 9)
    frac = 9;

  snprintf(out, cap, "%.*f%%", frac, v);
}

// ----------------------------------------------------------------------- //
// Asset emit                                                              //
// ----------------------------------------------------------------------- //

bool
wm_bt_assets_emit(const char *sweep_dir, char *err, size_t err_cap)
{
  char assets_dir[1024];
  char file_path[1152];
  int  n;

  if(sweep_dir == NULL || sweep_dir[0] == '\0')
  {
    if(err != NULL)
      snprintf(err, err_cap, "assets_emit: bad args");

    return(FAIL);
  }

  n = snprintf(assets_dir, sizeof(assets_dir), "%s/assets", sweep_dir);

  if(n < 0 || (size_t)n >= sizeof(assets_dir))
  {
    if(err != NULL)
      snprintf(err, err_cap, "assets dir path overflow");

    return(FAIL);
  }

  if(mkdir(assets_dir, WM_BT_REPORT_DIR_MODE) != 0 && errno != EEXIST)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "mkdir('%s') failed: %s", assets_dir, strerror(errno));

    return(FAIL);
  }

  n = snprintf(file_path, sizeof(file_path), "%s/report.css", assets_dir);

  if(n < 0 || (size_t)n >= sizeof(file_path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "report.css path overflow");

    return(FAIL);
  }

  if(wm_bt_write_atomic(file_path, WM_BT_REPORT_CSS, err, err_cap) != SUCCESS)
    return(FAIL);

  n = snprintf(file_path, sizeof(file_path), "%s/report.js", assets_dir);

  if(n < 0 || (size_t)n >= sizeof(file_path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "report.js path overflow");

    return(FAIL);
  }

  if(wm_bt_write_atomic(file_path, WM_BT_REPORT_JS, err, err_cap) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
