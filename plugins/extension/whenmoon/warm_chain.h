#ifndef BM_WM_WARM_CHAIN_H
#define BM_WM_WARM_CHAIN_H

#include "task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A self-rescheduling deferred task — a "chain" — and the accounting
// that lets stop() end one. whenmoon's two warmup timers re-arm
// themselves on every hop and retained no handle anywhere, so nothing
// could cancel them: deinit() destroyed the markets rwlock under a body
// that was still holding it (§SC-OBSERVED OBS-43).
//
// A handle alone is not enough, which is the whole reason this is a
// type. The chain replaces its own handle on each hop, so cancelling
// the handle you last saw races the body that is about to publish the
// next one. Arming and the permission to arm live behind one mutex
// here, so "cancel what is queued" and "refuse to queue more" are one
// decision.
//
// ⚠ These are predicates, not lifecycle functions: they return `true`
// for the good outcome. They deliberately do NOT speak in common.h's
// SUCCESS/FAIL, because SUCCESS is `false` there and every call site
// below is written as `if(!wm_warm_chain_arm(...))`.
//
// A chain is BUSY from the moment it is opened, and stays busy until it
// is armed. Between open() and a successful arm() — and between the
// body's enter() and its next arm() or close() — some thread holds the
// pointer, and a drain that freed the node in that window would be the
// very use-after-free this file exists to end. So "busy" is the whole
// question the drain waits on, and arming is the only thing that ends
// it.
//
// This header includes no whenmoon header on purpose: that is what lets
// the suite compile warm_chain.c on its own (tests/test_warm_chain.c),
// and it is why AGENTS.md §MUST names this file as its one exception to
// the WHENMOON_INTERNAL gate.

// Longest a drain waits for a body to leave the plugin's code.
//
// ⛔ This is NOT "long enough to cover the worst body", because there is
// no such number. Measured over 320 logged replays: mean 3,386 ms, but
// 21 exceeded core.plugin.unload_quiesce_ms (5,000) outright and the
// worst ran 144,147 ms — a feed-only market replaying its whole 288,000
// bar 1m ring with 52,544 bars synthesized over the gaps. That cost
// scales with ring size and hole count, so any constant here is a
// constant something will exceed.
//
// What this number actually chooses is the point past which REFUSING
// beats blocking: stop() runs on the command thread, and a
// non-destructive refusal (every parked chain re-armed, the plugin left
// running and intact) is a better answer than holding the daemon's
// command surface for minutes. 40,000 is 8× core's budget — it covers
// every ordinary strategy-sized warm with room to spare and hands the
// pathological full-ring case to the refusal path, which is gated.
// ⛔ Do not "harmonise" it down to match core's 5,000: that is the
// number this whole file exists because it is too small.
#define WM_WARM_DRAIN_MS  40000u

typedef struct wm_warm_chain wm_warm_chain_t;

// Open a chain carrying `ctx`. `release` frees `ctx` and is called
// exactly once, by wm_warm_chain_close, whichever path gets there.
// Returns NULL only when a drain is already in progress — the caller
// then frees `ctx` itself and schedules nothing.
//
// The returned chain is busy: the caller must reach either a successful
// wm_warm_chain_arm or wm_warm_chain_close, and until it does, a
// concurrent drain waits on it rather than freeing it.
wm_warm_chain_t *wm_warm_chain_open(void *ctx, void (*release)(void *));

void *wm_warm_chain_ctx(const wm_warm_chain_t *c);

// Schedule the next hop. `name`, `delay_ms` and `cb` are retained so a
// refused drain can re-arm the chain exactly as it found it. The task
// is submitted with the chain — not the ctx — as its data.
// false means a drain is in progress or the submit failed; the caller
// must then close the chain and schedule nothing.
bool wm_warm_chain_arm(wm_warm_chain_t *c, const char *name,
    uint32_t delay_ms, task_cb_t cb);

// Body entry. false means a drain is in progress: the body must touch
// no plugin state at all and go straight to wm_warm_chain_close.
bool wm_warm_chain_enter(wm_warm_chain_t *c);

// End the chain: release the ctx, unlink, wake any waiting drain, free
// the node. Every exit path of every body ends here, and so does the
// drain's own kill path.
void wm_warm_chain_close(wm_warm_chain_t *c);

// stop()'s barrier. Stops all arming, cancels every queued chain, then
// waits up to `timeout_ms` for every busy chain to leave.
//
// true: nothing of this plugin's is executing or queued. false: a body
// outlasted the budget — and the drain has then RE-ARMED every chain it
// cancelled, so a stop() that returns FAIL leaves the plugin exactly as
// it found it. `offender` receives the task name still running (empty
// when none), for the caller's warning; this file logs nothing itself.
bool wm_warm_chain_drain(uint32_t timeout_ms, char *offender,
    size_t offender_cap);

// init(): a fresh plugin life re-enables arming. Idempotent.
void wm_warm_chain_reset(void);

#endif // BM_WM_WARM_CHAIN_H
