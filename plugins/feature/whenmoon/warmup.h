// warmup.h — WM-WARMUP-2 market warmup lifecycle. Internal;
// WHENMOON_INTERNAL-gated.
//
// On market start (and on a runtime strategy attach) the market enters a
// warmup lifecycle: compute the deepest declared min_history across the
// attached strategy roster, gap-fill the recent 1m window from the
// exchange into the authoritative candle DB, replay it (cascading
// 1m→…→1d), then promote to READY. The trade engine acts on strategy
// advice only in WM_WARM_READY.
//
// Two timers, at DIFFERENT granularities — don't conflate them:
//
//   * Convergence re-check — PER MARKET SESSION (`market_id_str`, which
//     carries the `@<strategy>` instance suffix). A self-rescheduling
//     DEFERRED task guarded by whenmoon_market.warmup_gen (see market.h),
//     so a stopped market or a superseding warmup_begin retires it. It
//     ends for good once the market is promoted to READY. No task_cancel.
//
//   * Authoritative tail-fill — WM-TAILFILL-COALESCE-1: ONE GLOBAL
//     periodic task for the whole plugin, NOT one per session. The work
//     it does (gap-walk + download-enqueue) targets the candle table
//     `wm_candles_<market_id>`, and `market_id` is SHARED by every
//     instance of the same product (all of coinbase-btc-usd@{mako,
//     riptide,juggernaut} are market_id 1). Scheduling it per session
//     therefore ran the identical gap walk N× per table and — since
//     wm_dl_job_enqueue does not dedupe — enqueued N duplicate download
//     jobs whenever the timers landed in the same tick. The sweeper
//     instead reduces the READY markets to their DISTINCT market_ids and
//     tail-fills each table exactly once. Cancelled at plugin deinit.

#ifndef BM_WHENMOON_WARMUP_H
#define BM_WHENMOON_WARMUP_H

#ifdef WHENMOON_INTERNAL

struct whenmoon_state;
struct whenmoon_market;

// Convergence re-check cadence: re-walk the candle gaps until the recent
// window is contiguous to ~now.
#define WM_WARM_RECHECK_INTERVAL_MS   5000u

// Authoritative tail-fill cadence (global sweep; see the header note).
#define WM_WARM_TAILFILL_INTERVAL_MS  180000u

// Distinct candle tables one tail-fill sweep can cover. Sized well above
// the live set (= number of distinct products, 3 today); a sweep that
// would exceed it warns rather than silently dropping a table.
#define WM_WARM_TAILFILL_MAX_TABLES   32u

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

// WM-WARMUP-HERD-1: bound how many full-ring DB replays run at once. A
// bulk restore of N instances would otherwise land N concurrent remote-
// Postgres fetches (~288k rows each) + N replay-loop lock holds, making
// botmanctl/IRC sluggish for ~1-2 min. Cap = one slot per distinct
// product in the trial set. Callers over the cap re-defer.
#define WM_WARMUP_MAX_CONCURRENT      3u

// Re-defer interval for a warmup that hit the concurrency cap.
#define WM_WARMUP_DEFER_MS            250u

// Begin (or restart) the warmup lifecycle for a market. Reads the
// attached strategy roster to size the required history, enqueues DB
// gap-fills for the recent window, and schedules the convergence
// re-check (which replays + promotes to READY). Zero strategies =
// feed-only: a shallow ring warm + immediate READY. Idempotent — safe to
// call again when a strategy attaches at runtime (bumps warmup_gen so any
// prior timer retires). Called off mk->lock.
void wm_market_warmup_begin(struct whenmoon_state *st,
    struct whenmoon_market *mk);

// WM-TAILFILL-COALESCE-1: start/stop the single global tail-fill sweep.
// Init after `st` exists (the task reads the market array through it);
// destroy BEFORE `st` is freed. Both idempotent.
bool wm_warm_tailfill_global_init(struct whenmoon_state *st);
void wm_warm_tailfill_global_destroy(void);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_WARMUP_H
