// botmanager — MIT
// Whenmoon backtest report shared assets: CSS/JS, <head> opener, C format
// helpers (WM-BT-RPT-1).

#define WHENMOON_INTERNAL
#include "wm_bt_assets.h"

#include "wm_bt_report.h"   // wm_bt_write_atomic, WM_BT_REPORT_DIR_MODE

#include "alloc.h"          // mem_alloc / mem_free
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

// Split into parts so no single string literal exceeds the C99 4095-byte
// "minimum maximum" (-Woverlength-strings under -Wpedantic). The parts are
// joined into one buffer at write time (wm_bt_assets_write_joined). Keep
// each part comfortably under 4095 as RPT-4..6 grow the stylesheet.
static const char *const WM_BT_REPORT_CSS_PARTS[] = {
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
    ".wrap{max-width:1480px;margin:0 auto;padding:24px 28px 64px}\n"
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
    "border-top:1px solid var(--border);padding-top:16px}\n",
    // --- Web Interface Guidelines baseline (WM-BT-RPT-1) ---
    "html{color-scheme:dark}\n"
    ":focus-visible{outline:2px solid var(--accent);outline-offset:2px}\n"
    ".skip{position:absolute;left:8px;top:-48px;z-index:20;"
    "background:var(--accent);color:#fff;padding:8px 14px;border-radius:8px;"
    "transition:top .15s}\n"
    ".skip:focus{top:8px}\n"
    ".num{font-variant-numeric:tabular-nums}\n"
    "h1,h2{text-wrap:balance}\n"
    "@media (prefers-reduced-motion:reduce){*{animation:none!important;"
    "transition:none!important}}\n"
    "@media print{header{position:static}.tnav{display:none}"
    "section,details,.card,.verdict{break-inside:avoid}"
    ".cards{grid-template-columns:repeat(2,1fr)}"
    ".chartbox,.ddbox,.tc-pane{min-height:180px}"
    ".tbl-scroll{max-height:none;overflow:visible}"
    ".note{color:#444}}\n"
    // --- equity & drawdown panes (WM-BT-RPT-2) ---
    ".chartbox{height:320px;margin:6px 0}\n"
    ".ddbox{height:150px;margin:6px 0 2px}\n"
    // --- trade analytics: distribution, chips, sortable table (RPT-3) ---
    "h3.sub-h{font-size:12px;font-weight:600;color:var(--muted);"
    "text-transform:uppercase;letter-spacing:.04em;margin:14px 0 4px}\n"
    ".hist{width:100%;height:auto;display:block;margin:4px 0 8px;"
    "background:var(--surface);border:1px solid var(--border);"
    "border-radius:10px}\n"
    ".hist rect.win{fill:var(--win)}\n"
    ".hist rect.loss{fill:var(--loss)}\n"
    ".hist .axis{stroke:var(--border)}\n"
    ".hist .lbl{fill:var(--muted);font-size:11px;"
    "font-family:ui-monospace,monospace}\n"
    ".chips{display:flex;flex-wrap:wrap;gap:8px;padding:4px 0 12px}\n"
    ".chip{background:var(--surface);border:1px solid var(--border);"
    "color:var(--text);border-radius:999px;padding:5px 14px;font-size:12px;"
    "cursor:pointer;font:inherit}\n"
    ".chip[aria-pressed='true']{background:var(--accent);"
    "border-color:var(--accent);color:#fff}\n"
    "th[aria-sort] button.sort{all:unset;cursor:pointer;display:inline-flex;"
    "align-items:center;gap:4px;color:inherit;font:inherit}\n"
    "th[aria-sort] .arrow::after{content:'\\2195';opacity:.35}\n"
    "th[aria-sort='ascending'] .arrow::after{content:'\\2191';opacity:1}\n"
    "th[aria-sort='descending'] .arrow::after{content:'\\2193';opacity:1}\n"
    "tr.hidden{display:none}\n"
    ".tbl-scroll{content-visibility:auto;contain-intrinsic-size:auto 560px}\n"
    // --- per-trade chart page (WM-BT-RPT-4: nav, info cards, panes) ---
    ".tnav{display:flex;flex-wrap:wrap;align-items:center;gap:8px 14px;"
    "padding:10px 28px;border-bottom:1px solid var(--border);font-size:13px}\n"
    ".tnav .grp{display:flex;flex-wrap:wrap;align-items:center;gap:8px}\n"
    ".tnav .lbl{color:var(--muted);text-transform:uppercase;font-size:11px;"
    "letter-spacing:.04em}\n"
    ".tnav .sep{color:var(--border)}\n"
    ".tnav .spacer{flex:1}\n"
    ".navlink{padding:3px 10px;border:1px solid var(--border);"
    "border-radius:8px;color:var(--accent)}\n"
    ".navlink:hover{background:var(--surface2);text-decoration:none}\n"
    ".navlink.off{color:var(--muted);opacity:.45;cursor:default}\n"
    ".navlink.cur{background:var(--surface2);border-color:var(--accent);"
    "color:var(--text)}\n"
    ".card .v.txt{font-size:14px;font-weight:550;text-transform:none}\n"
    ".tc-figure{margin:18px 0 0}\n"
    ".tc-pane{margin:6px 0;border:1px solid var(--border);border-radius:10px;"
    "background:var(--surface);overflow:hidden}\n"
    "#tc-price{height:54vh;min-height:320px}\n"
    ".tc-sub{height:15vh;min-height:120px}\n"
    ".tc-cap{color:var(--muted);font-size:12px;margin-top:6px}\n"
    ".visually-hidden{position:absolute;width:1px;height:1px;margin:-1px;"
    "padding:0;overflow:hidden;clip:rect(0 0 0 0);clip-path:inset(50%);"
    "border:0;white-space:nowrap}\n",
    // --- sweep dashboard: score heatmap (WM-BT-RPT-5). The per-axis
    // marginal bars reuse the .hist classes above; the heatmap needs its
    // own surface + muted-cell + label styling. Cell colors are inline
    // var(--win/--loss/--accent) fills with a scaled fill-opacity. ---
    ".heat{width:100%;height:auto;display:block;margin:4px 0 8px;"
    "background:var(--surface);border:1px solid var(--border);"
    "border-radius:10px;padding:8px}\n"
    ".heat rect.miss{fill:var(--surface2)}\n"
    ".heat .lbl{fill:var(--muted);font-size:11px;"
    "font-family:ui-monospace,monospace}\n",
    // --- verdict banner + tooltip affordances (WM-BT-RPT-6). The banner
    // is a flex pill + plain-English takeaway above the headline cards;
    // win/loss tint the border + pill. th[title]/.k[title] get a help
    // cursor so the hover-tooltip metric definitions are discoverable. ---
    ".verdict{display:flex;flex-wrap:wrap;align-items:center;gap:14px;"
    "margin:22px 0;padding:16px 20px;border:1px solid var(--border);"
    "border-radius:14px;background:var(--surface)}\n"
    ".verdict .pill{font-weight:700;font-size:12px;letter-spacing:.05em;"
    "text-transform:uppercase;padding:5px 13px;border-radius:999px;"
    "border:1px solid var(--border);white-space:nowrap}\n"
    ".verdict.win{border-color:rgba(43,182,115,.45)}\n"
    ".verdict.win .pill{color:var(--win);border-color:rgba(43,182,115,.5);"
    "background:rgba(43,182,115,.08)}\n"
    ".verdict.loss{border-color:rgba(224,83,61,.45)}\n"
    ".verdict.loss .pill{color:var(--loss);border-color:rgba(224,83,61,.5);"
    "background:rgba(224,83,61,.08)}\n"
    ".verdict .takeaway{flex:1;min-width:280px;font-size:15px}\n"
    ".verdict .takeaway b{font-weight:650}\n"
    ".verdict .takeaway .sub2{display:block;color:var(--muted);"
    "font-size:13px;margin-top:3px}\n"
    "th[title],.card .k[title],.strip .it span[title]{cursor:help}\n",
    NULL,
};

// ----------------------------------------------------------------------- //
// Shared client bootstrap                                                 //
// ----------------------------------------------------------------------- //
//
// No-op until later chunks append render functions; provides the
// reduced-motion flag + Intl formatters they will reuse. Single-quoted
// throughout so the C string literal needs no escaping.

// Split into parts (see WM_BT_REPORT_CSS_PARTS) to stay under the C99
// 4095-byte literal limit; joined at write time.
static const char *const WM_BT_REPORT_JS_PARTS[] = {
    "// whenmoon backtest report - shared client bootstrap (WM-BT-RPT-1).\n"
    "'use strict';\n"
    // Hoisted once (WM-BT-RPT-6 js-hoist-regex): strips everything but a
    // numeric literal from a sort cell. Global flag, but .replace ignores
    // lastIndex so the shared instance is reuse-safe.
    "const wmNumRe = /[^0-9eE.+-]/g;\n"
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
    "}\n"
    // --- WM-BT-RPT-2: equity curve + underwater drawdown ---
    // Reads the #eq-data JSON island ([{t:sec,e:equity,dd:pct},...]) and
    // draws an LWC area series (equity) above a baseline series pinned at 0
    // (drawdown, plotted negative so it hangs underwater). No-ops on pages
    // without #eq (the per-trade charts), and degrades to a readable .note
    // when the JSON is missing/bad or the CDN library failed to load.
    "function wmRenderEquity(){\n"
    "  const host = document.getElementById('eq');\n"
    "  const src  = document.getElementById('eq-data');\n"
    "  if(!host || !src) return;\n"
    "  let pts = null;\n"
    "  try { pts = JSON.parse(src.textContent || '[]'); }"
    " catch(e) { pts = null; }\n"
    "  if(!Array.isArray(pts) || pts.length === 0){\n"
    "    host.innerHTML = '<p class=note>No equity data to plot.</p>';"
    " return;\n"
    "  }\n"
    "  if(typeof LightweightCharts === 'undefined'){\n"
    "    host.innerHTML = '<p class=note>Charts need network"
    " (Lightweight Charts CDN).</p>';\n"
    "    return;\n"
    "  }\n"
    "  const opts = {\n"
    "    autoSize:true,\n"
    "    layout:{background:{color:'#0e0f13'},textColor:'#8b93a7',"
    "fontSize:11},\n"
    "    grid:{vertLines:{color:'#1e2230'},horzLines:{color:'#1e2230'}},\n"
    "    rightPriceScale:{borderColor:'#2a2f3e'},\n"
    "    timeScale:{borderColor:'#2a2f3e',timeVisible:true,"
    "secondsVisible:false},\n"
    "    crosshair:{mode:0},\n"
    // reduced motion: kill the inertial fling animation, keep panning.
    "    kineticScroll:{mouse:false,touch:!wmReduceMotion}\n"
    "  };\n"
    "  const eqChart = LightweightCharts.createChart(host, opts);\n"
    "  const eqSeries = eqChart.addAreaSeries({\n"
    "    lineColor:'#5b8cff',\n"
    "    topColor:'rgba(91,140,255,0.30)',\n"
    "    bottomColor:'rgba(91,140,255,0.02)',\n"
    "    lineWidth:2,\n"
    "    priceFormat:{type:'price',precision:2,minMove:0.01}\n"
    "  });\n"
    "  eqSeries.setData(pts.map(p => ({time:p.t, value:p.e})));\n"
    "  let ddChart = null;\n"
    "  const ddHost = document.getElementById('dd');\n"
    "  if(ddHost){\n"
    "    ddChart = LightweightCharts.createChart(ddHost, opts);\n"
    "    const ddSeries = ddChart.addBaselineSeries({\n"
    "      baseValue:{type:'price',price:0},\n"
    "      topLineColor:'rgba(224,83,61,0)',\n"
    "      topFillColor1:'rgba(224,83,61,0)',\n"
    "      topFillColor2:'rgba(224,83,61,0)',\n"
    "      bottomLineColor:'#e0533d',\n"
    "      bottomFillColor1:'rgba(224,83,61,0.05)',\n"
    "      bottomFillColor2:'rgba(224,83,61,0.38)',\n"
    "      lineWidth:1,\n"
    "      priceFormat:{type:'percent'}\n"
    "    });\n"
    "    ddSeries.setData(pts.map(p => ({time:p.t,"
    " value:-Math.abs(p.dd)})));\n"
    "  }\n"
    "  eqChart.timeScale().fitContent();\n"
    "  if(ddChart) ddChart.timeScale().fitContent();\n"
    "}\n",
    // --- WM-BT-RPT-3: sortable + filterable trade table ---
    // Both are progressive enhancement: the server-rendered row order +
    // the full row set work with JS off. Sort reads each cell's data-v
    // (held = ms) when present, else parses the visible text; numeric
    // NaN cells (open trades, em-dashes) always sink to the bottom.
    "function wmSortTable(btn){\n"
    "  const th = btn.closest('th'), table = btn.closest('table');\n"
    "  if(!th || !table || !table.tBodies[0]) return;\n"
    "  const col = parseInt(btn.dataset.col, 10);\n"
    "  const type = btn.dataset.type || 'text';\n"
    "  const dir = th.getAttribute('aria-sort') === 'ascending'\n"
    "    ? 'descending' : 'ascending';\n"
    "  table.querySelectorAll('thead th[aria-sort]').forEach(h => {\n"
    "    if(h !== th) h.setAttribute('aria-sort', 'none');\n"
    "  });\n"
    "  th.setAttribute('aria-sort', dir);\n"
    "  const sign = dir === 'ascending' ? 1 : -1;\n"
    "  const val = (row) => {\n"
    "    const cell = row.cells[col];\n"
    "    if(!cell) return type === 'num' ? NaN : '';\n"
    "    if(type === 'num'){\n"
    "      const dv = cell.dataset.v;\n"
    "      const raw = (dv !== undefined && dv !== '') ? dv : cell.textContent;\n"
    "      return parseFloat(String(raw).replace(wmNumRe, ''));\n"
    "    }\n"
    "    return cell.textContent.trim();\n"
    "  };\n"
    "  const tbody = table.tBodies[0];\n"
    "  const rows = Array.prototype.slice.call(tbody.rows);\n"
    "  rows.sort((a, b) => {\n"
    "    const va = val(a), vb = val(b);\n"
    "    if(type === 'num'){\n"
    "      const an = isNaN(va), bn = isNaN(vb);\n"
    "      if(an && bn) return 0;\n"
    "      if(an) return 1;\n"
    "      if(bn) return -1;\n"
    "      return (va - vb) * sign;\n"
    "    }\n"
    "    return va.localeCompare(vb) * sign;\n"
    "  });\n"
    "  rows.forEach(r => tbody.appendChild(r));\n"
    "}\n"
    "function wmFilterTrades(grp, kind){\n"
    "  const table = grp.parentElement\n"
    "    ? grp.parentElement.querySelector('table.trades') : null;\n"
    "  if(!table || !table.tBodies[0]) return;\n"
    "  Array.prototype.forEach.call(table.tBodies[0].rows, (row) => {\n"
    "    const show = (kind === 'all') || (row.dataset.cls === kind);\n"
    "    row.classList.toggle('hidden', !show);\n"
    "  });\n"
    "  grp.querySelectorAll('.chip').forEach(c => {\n"
    "    c.setAttribute('aria-pressed',\n"
    "      c.dataset.kind === kind ? 'true' : 'false');\n"
    "  });\n"
    "}\n"
    "function wmInitTrades(){\n"
    "  document.querySelectorAll('table.trades thead button.sort')\n"
    "    .forEach(btn => btn.addEventListener('click',"
    " () => wmSortTable(btn)));\n"
    "  document.querySelectorAll('.chips').forEach(grp => {\n"
    "    grp.querySelectorAll('.chip').forEach(chip =>\n"
    "      chip.addEventListener('click',\n"
    "        () => wmFilterTrades(grp, chip.dataset.kind)));\n"
    "  });\n"
    "}\n"
    "function wmInit(){ wmRenderEquity(); wmInitTrades();"
    " wmRenderTradeChart(); }\n"
    "if(document.readyState === 'complete') wmInit();\n"
    "else document.addEventListener('DOMContentLoaded', wmInit);\n",
    // --- WM-BT-RPT-4: per-trade multi-pane chart (price + volume + RSI +
    // MACD), stacked LWC charts with a synced time axis. Reads the
    // #trade-data JSON island; no-ops on pages without #tc-price.
    "function wmTcOpts(){\n"
    "  return {\n"
    "    autoSize:true,\n"
    "    layout:{background:{color:'#0e0f13'},textColor:'#8b93a7',"
    "fontSize:11},\n"
    "    grid:{vertLines:{color:'#1e2230'},horzLines:{color:'#1e2230'}},\n"
    "    rightPriceScale:{borderColor:'#2a2f3e'},\n"
    "    timeScale:{borderColor:'#2a2f3e',timeVisible:true,"
    "secondsVisible:false},\n"
    "    crosshair:{mode:0},\n"
    "    kineticScroll:{mouse:false,touch:!wmReduceMotion}\n"
    "  };\n"
    "}\n"
    // Two-way time-axis sync across the stacked panes; a re-entrancy lock
    // stops the visible-range callbacks from echoing each other. Synced by
    // *time* range (not logical/index): the oscillator panes drop NaN
    // warmup bars, so they hold fewer points than price/volume and an
    // index sync would shift them by the warmup length.
    "function wmSyncTime(charts){\n"
    "  let lock = false;\n"
    "  charts.forEach(src => {\n"
    "    src.timeScale().subscribeVisibleTimeRangeChange(tr => {\n"
    "      if(!tr || lock) return;\n"
    "      lock = true;\n"
    "      charts.forEach(d => {\n"
    "        if(d !== src){ try { d.timeScale().setVisibleRange(tr); }"
    " catch(e) {} }\n"
    "      });\n"
    "      lock = false;\n"
    "    });\n"
    "  });\n"
    "}\n",
    "function wmRenderTradeChart(){\n"
    "  const host = document.getElementById('tc-price');\n"
    "  const src  = document.getElementById('trade-data');\n"
    "  if(!host || !src) return;\n"
    "  let d = null;\n"
    "  try { d = JSON.parse(src.textContent || '{}'); }"
    " catch(e) { d = null; }\n"
    "  if(!d || !Array.isArray(d.candles) || d.candles.length === 0){\n"
    "    host.innerHTML = '<p class=note>No candle data to plot.</p>';"
    " return;\n"
    "  }\n"
    "  if(typeof LightweightCharts === 'undefined'){\n"
    "    host.innerHTML = '<p class=note>Charts need network"
    " (Lightweight Charts CDN).</p>';\n"
    "    return;\n"
    "  }\n"
    "  const charts = [];\n"
    "  const price = LightweightCharts.createChart(host, wmTcOpts());\n"
    "  charts.push(price);\n"
    "  const candles = price.addCandlestickSeries({upColor:'#2bb673',"
    "downColor:'#e0533d',borderVisible:false,wickUpColor:'#2bb673',"
    "wickDownColor:'#e0533d'});\n"
    "  candles.setData(d.candles);\n"
    "  const addLine = (arr, color, title) => {\n"
    "    if(!Array.isArray(arr) || !arr.length) return;\n"
    "    price.addLineSeries({color:color,lineWidth:1,title:title,"
    "priceLineVisible:false,lastValueVisible:false}).setData(arr);\n"
    "  };\n"
    "  addLine(d.sma20, '#5b8cff', 'SMA 20');\n"
    "  addLine(d.sma50, '#f5c451', 'SMA 50');\n"
    "  addLine(d.ema20, '#2bb673', 'EMA 20');\n"
    "  if(typeof d.entryPx === 'number')"
    " candles.createPriceLine({price:d.entryPx,color:'#f5c451',"
    "lineWidth:1,lineStyle:2,title:'entry'});\n"
    "  if(typeof d.exitPx === 'number')"
    " candles.createPriceLine({price:d.exitPx,color:'#5b8cff',"
    "lineWidth:1,lineStyle:2,title:'exit'});\n"
    "  if(Array.isArray(d.markers) && d.markers.length)"
    " candles.setMarkers(d.markers);\n"
    "  const volHost = document.getElementById('tc-vol');\n"
    "  if(volHost && Array.isArray(d.volume) && d.volume.length){\n"
    "    const vc = LightweightCharts.createChart(volHost, wmTcOpts());\n"
    "    charts.push(vc);\n"
    "    vc.addHistogramSeries({priceFormat:{type:'volume'},"
    "priceLineVisible:false}).setData(d.volume);\n"
    "  }\n"
    "  const rsiHost = document.getElementById('tc-rsi');\n"
    "  if(rsiHost && Array.isArray(d.rsi) && d.rsi.length){\n"
    "    const rc = LightweightCharts.createChart(rsiHost, wmTcOpts());\n"
    "    charts.push(rc);\n"
    "    const r = rc.addLineSeries({color:'#b48cff',lineWidth:1,"
    "title:'RSI 14'});\n"
    "    r.setData(d.rsi);\n"
    "    r.createPriceLine({price:70,color:'#e0533d',lineWidth:1,"
    "lineStyle:2,title:'70'});\n"
    "    r.createPriceLine({price:30,color:'#2bb673',lineWidth:1,"
    "lineStyle:2,title:'30'});\n"
    "  }\n"
    "  const macdHost = document.getElementById('tc-macd');\n"
    "  if(macdHost && Array.isArray(d.macd) && d.macd.length){\n"
    "    const mc = LightweightCharts.createChart(macdHost, wmTcOpts());\n"
    "    charts.push(mc);\n"
    "    if(Array.isArray(d.macdHist) && d.macdHist.length)\n"
    "      mc.addHistogramSeries({priceLineVisible:false,"
    "lastValueVisible:false}).setData(d.macdHist);\n"
    "    mc.addLineSeries({color:'#5b8cff',lineWidth:1,title:'MACD'})"
    ".setData(d.macd);\n"
    "    if(Array.isArray(d.macdSignal) && d.macdSignal.length)\n"
    "      mc.addLineSeries({color:'#f5c451',lineWidth:1,title:'signal'})"
    ".setData(d.macdSignal);\n"
    "  }\n"
    "  charts.forEach(c => c.timeScale().fitContent());\n"
    "  wmSyncTime(charts);\n"
    "}\n",
    NULL,
};

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

  // Skip-to-content link (Web Interface Guidelines; WM-BT-RPT-6). Every
  // page's main landmark carries id="main"; the link is off-screen until
  // focused (.skip CSS).
  fputs("</head><body>\n"
        "<a class=\"skip\" href=\"#main\">Skip to content</a>\n", fp);
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

void
wm_bt_html_escape(const char *in, char *out, size_t cap)
{
  size_t o = 0;

  if(out == NULL || cap == 0)
    return;

  if(in == NULL)
    in = "";

  for(; *in != '\0' && o + 1 < cap; in++)
  {
    const char *rep = NULL;
    size_t      rl;

    switch(*in)
    {
      case '&':  rep = "&amp;";  break;
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '"':  rep = "&quot;"; break;
      case '\'': rep = "&#39;";  break;
      default:
        out[o++] = *in;
        continue;
    }

    rl = strlen(rep);

    if(o + rl >= cap)
      break;

    memcpy(out + o, rep, rl);
    o += rl;
  }

  out[o] = '\0';
}

// ----------------------------------------------------------------------- //
// Asset emit                                                              //
// ----------------------------------------------------------------------- //

// Join a NULL-terminated array of string parts into one heap buffer and
// write it atomically (tmp + fsync + rename via wm_bt_write_atomic). The
// parts are split only to dodge the C99 4095-byte literal cap; on disk
// they are one contiguous file. Returns SUCCESS / FAIL (err populated).
static bool
wm_bt_assets_write_joined(const char *path, const char *const *parts,
    char *err, size_t err_cap)
{
  size_t total = 0;
  size_t off   = 0;
  size_t i;
  char  *buf;
  bool   rc;

  for(i = 0; parts[i] != NULL; i++)
    total += strlen(parts[i]);

  buf = mem_alloc("whenmoon.bt.report", "asset_join", total + 1);

  if(buf == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "asset join alloc failed");

    return(FAIL);
  }

  for(i = 0; parts[i] != NULL; i++)
  {
    size_t len = strlen(parts[i]);

    memcpy(buf + off, parts[i], len);
    off += len;
  }

  buf[off] = '\0';

  rc = wm_bt_write_atomic(path, buf, err, err_cap);

  mem_free(buf);

  return(rc);
}

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

  if(wm_bt_assets_write_joined(file_path, WM_BT_REPORT_CSS_PARTS,
         err, err_cap) != SUCCESS)
    return(FAIL);

  n = snprintf(file_path, sizeof(file_path), "%s/report.js", assets_dir);

  if(n < 0 || (size_t)n >= sizeof(file_path))
  {
    if(err != NULL)
      snprintf(err, err_cap, "report.js path overflow");

    return(FAIL);
  }

  if(wm_bt_assets_write_joined(file_path, WM_BT_REPORT_JS_PARTS,
         err, err_cap) != SUCCESS)
    return(FAIL);

  return(SUCCESS);
}
