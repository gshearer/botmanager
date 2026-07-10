// botmanager — MIT
// wm_exch_query — shared helpers for the operator-facing synchronous
// exchange observability commands (`/show whenmoon balances`, `orders`).
//
// These commands must return fresh authenticated data over the control
// socket, but core/botmanctl.c clears its single global reply target the
// instant synchronous dispatch returns — so a reply emitted from a later
// async curl callback is silently dropped (it only surfaces over IRC).
// The commands therefore block the dispatch thread until the exchange
// fetch completes (bounded by core.curl.timeout) and format inline.

#ifndef WM_EXCH_QUERY_H
#define WM_EXCH_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Default wait bound for a synchronous exchange query. Comfortably above
// core.curl.timeout (30 s) + connect_timeout (10 s) so the curl layer's
// own timeout fires first on a dead network; this is just a backstop.
#define WM_EXCH_QUERY_WAIT_MS  45000

// --- Synchronous bridge over an async exchange fetch -----------------
//
// The result buffer is owned by the refcounted handle (never the
// caller's stack), so a wait that times out before a late callback
// cannot use-after-free it (cf. the gemini prime-symbols stack-reclaim
// crash, finding_gemini_prime_use_after_free).
//
// Pattern:
//   wm_sync_fetch_t *w = wm_sync_fetch_begin(sizeof(res_t));
//   exchange_..._async(..., typed_cb, w);     // typed_cb -> _complete
//   res_t out;
//   if(wm_sync_fetch_wait(w, &out, sizeof(out), WM_EXCH_QUERY_WAIT_MS))
//     { ...out... } else { ...timeout... }
typedef struct wm_sync_fetch wm_sync_fetch_t;

// Allocate a handle owning a zeroed result buffer of `result_sz` bytes.
// refs = 2 (the waiter + the callback). Returns NULL on OOM.
wm_sync_fetch_t *wm_sync_fetch_begin(size_t result_sz);

// Called from the typed completion callback. Copies `src` (clamped to
// the handle buffer) into the handle, marks done, wakes the waiter, and
// drops the callback's ref. Pass a non-NULL `src` (a NULL result must be
// translated to a typed error struct by the caller's callback).
void wm_sync_fetch_complete(wm_sync_fetch_t *w,
    const void *src, size_t src_sz);

// Block up to `timeout_ms`. On completion copies the result into `out`
// (clamped to `out_sz`) and returns true; on timeout leaves `out`
// untouched and returns false. Always drops the caller's ref (freeing
// the handle once the callback has dropped its ref too).
bool wm_sync_fetch_wait(wm_sync_fetch_t *w,
    void *out, size_t out_sz, uint32_t timeout_ms);

// --- Display helpers -------------------------------------------------

// Format a monetary / crypto amount as fixed-point with trailing zeros
// trimmed, so tiny quantities read as "0.00000003" rather than the "%g"
// form "3e-08". Up to 10 fractional digits. Returns `buf`.
const char *wm_fmt_amount(double v, char *buf, size_t cap);

// Format an elapsed duration (the "vintage" of a cached snapshot) as a
// compact human age: "3s", "45s", "2m 14s", "1h 03m", "2d 4h". Negative
// inputs clamp to 0. Returns `buf`.
const char *wm_fmt_age(int64_t age_ms, char *buf, size_t cap);

// --- Colorized table helpers -----------------------------------------
//
// Shared by the operator telemetry tables (`/show whenmoon markets`,
// `/show whenmoon market` session list). Percentages carry a green/red
// tint by sign so a wall of markets reads at a glance; the padding
// helpers are color-aware so a cell's inline `\x01…` control bytes don't
// throw the column alignment off.

// Format a signed percentage with a sign-driven tint: green when > 0,
// red when < 0, plain when exactly 0 ("+1.4%", "-0.8%", "0.0%"). `prec`
// fractional digits. Returns `buf`.
const char *wm_fmt_pct(double pct, int prec, char *buf, size_t cap);

// Visible width of `s`, skipping the two-byte `\x01<code>` color escapes
// (colors.h) so alignment math counts glyphs, not control bytes.
size_t wm_vis_len(const char *s);

// Pad `buf` to a visible `width`, in place. `rjust` right-justifies
// (leading spaces — for numeric columns); otherwise left-justifies
// (trailing spaces — for labels). No-op when already at/over width or
// when the padded result would not fit `cap`. Color escapes are ignored
// for the width computation.
void wm_col_pad(char *buf, size_t cap, int width, bool rjust);

#endif
