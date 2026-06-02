// warmup.h — WM-WARMUP-2 market warmup lifecycle. Internal;
// WHENMOON_INTERNAL-gated.
//
// On market start (and on a runtime strategy attach) the market enters a
// warmup lifecycle: compute the deepest declared min_history across the
// attached strategy roster, gap-fill the recent 1m window from the
// exchange into the authoritative candle DB, replay it (cascading
// 1m→…→1d), then promote to READY. The trade engine acts on strategy
// advice only in WM_WARM_READY. A periodic authoritative tail-fill keeps
// the DB current thereafter so restart gaps stay small.
//
// Timers are self-rescheduling DEFERRED tasks guarded by
// whenmoon_market.warmup_gen — see market.h. No task_cancel is used.

#ifndef BM_WHENMOON_WARMUP_H
#define BM_WHENMOON_WARMUP_H

#ifdef WHENMOON_INTERNAL

struct whenmoon_state;
struct whenmoon_market;

// Convergence re-check cadence: re-walk the candle gaps until the recent
// window is contiguous to ~now.
#define WM_WARM_RECHECK_INTERVAL_MS   5000u

// Authoritative tail-fill cadence on a READY market.
#define WM_WARM_TAILFILL_INTERVAL_MS  180000u

// A residual forward gap shorter than this is left for the live feed to
// close; warmup promotes to READY rather than chasing the last bars.
#define WM_WARM_TAIL_TOLERANCE_MS     180000LL

// Recent window the tail-fill keeps topped up (covers the cadence +
// slack).
#define WM_WARM_TAILFILL_WINDOW_MS    900000LL

// wm_gap_find_row_gaps out-array cap per pass.
#define WM_WARM_MAX_GAPS              64u

// Safety: give up converging after this many re-checks (~20 min at 5 s)
// so a permanently unfillable backward gap can't spin forever; promote
// to READY with whatever history exists.
#define WM_WARM_MAX_RECHECKS          240u

// Begin (or restart) the warmup lifecycle for a market. Reads the
// attached strategy roster to size the required history, enqueues DB
// gap-fills for the recent window, and schedules the convergence
// re-check (which replays + promotes to READY). Zero strategies =
// feed-only: a shallow ring warm + immediate READY. Idempotent — safe to
// call again when a strategy attaches at runtime (bumps warmup_gen so any
// prior timer retires). Called off mk->lock.
void wm_market_warmup_begin(struct whenmoon_state *st,
    struct whenmoon_market *mk);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_WARMUP_H
