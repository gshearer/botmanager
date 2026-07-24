#ifndef BM_WHENMOON_WM_BT_ASSETS_H
#define BM_WHENMOON_WM_BT_ASSETS_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Shared backtest-report front-end assets (WM-BT-RPT-1).
//
// Every emitted HTML page — the single-config trade report `index.html`,
// the per-trade charts under `charts/iter-K/`, and (from WM-BT-RPT-5) the
// sweep dashboard — links one shared `assets/report.css` + `assets/
// report.js` per sweep dir instead of inlining a <style> block. This
// module owns that stylesheet + script, the shared <head> opener, and the
// numeric/temporal C formatting helpers the renderers print through.
//
// Internal to the whenmoon plugin. WHENMOON_INTERNAL gated.

// Create `<sweep_dir>/assets/` (mode WM_BT_REPORT_DIR_MODE — the umask
// decides) and atomically write `report.css` + `report.js` into it. Call
// once per sweep dir, right after wm_bt_sweep_dir_create succeeds. Returns
// SUCCESS on both files written; FAIL with `err` populated on mkdir / write
// failure (callers warn + continue — an unstyled page is still readable).
bool wm_bt_assets_emit(const char *sweep_dir, char *err, size_t err_cap);

// Emit the shared document head into `fp`: `<!DOCTYPE>` + `<html
// lang="en">` + charset / viewport / theme-color meta, a preconnect to the
// Lightweight-Charts CDN host, and a `<link rel="stylesheet">` to
// `css_href`. Opens `<body>` and returns; the caller emits all body
// content. `title_esc` must already be HTML-escaped. `css_href` is the
// relative path to report.css from the page being written (e.g.
// "assets/report.css" for the sweep-root index, "../../assets/report.css"
// for a per-trade chart). NULL `fp` is a no-op; NULL `title_esc` /
// `css_href` are treated as empty strings.
void wm_bt_html_doc_open(FILE *fp, const char *title_esc,
    const char *css_href);

// Thousands-separated signed currency, rounded to whole dollars:
// 10000 -> "$10,000"; -1234.5 -> "-$1,235". Non-finite -> "n/a". Always
// NUL-terminates.
void wm_bt_fmt_usd(double v, char *out, size_t cap);

// Thousands-separated number with `frac` (clamped 0..9) decimal places:
// (1234567.5, 2) -> "1,234,567.50". Negatives keep a leading '-'.
// Non-finite -> "n/a". Always NUL-terminates.
void wm_bt_fmt_num(double v, int frac, char *out, size_t cap);

// Percent with `frac` (clamped 0..9) decimals and a trailing '%'. The
// value is already in percent units (8.2 -> "8.2%", not 0.082). Negatives
// keep a leading '-'; positives are unsigned. Non-finite -> "n/a".
void wm_bt_fmt_pct(double v, int frac, char *out, size_t cap);

// Escape the five HTML-significant characters (& < > " ') from `in` into
// `out`. Always NUL-terminates; silently stops at the buffer edge. NULL
// `in` is treated as empty. Shared by every report/chart emitter that
// prints free text (exit reasons, strategy names, paths) into markup.
void wm_bt_html_escape(const char *in, char *out, size_t cap);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WM_BT_ASSETS_H
