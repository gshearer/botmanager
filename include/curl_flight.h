#ifndef BM_CURL_FLIGHT_H
#define BM_CURL_FLIGHT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "curl.h"

// A plugin's set of requests that are in the air — submitted, or about
// to be, and not yet finished with. It exists for one moment, stop(),
// where a plugin about to be unmapped has to get its own completion
// callbacks over with before deinit() tears down what they touch.
//
// Core cannot do this for a plugin. The quiescence barrier that runs
// before deinit() waits only for a callback that is already *executing*;
// a transfer still on the wire holds no lock, so it is not waited for,
// and it delivers afterwards — into whatever deinit() left behind. A
// mutex destroyed under that callback is undefined behaviour that
// reports as nothing at all: locking a destroyed mutex returns EINVAL,
// no caller in this tree checks it, and the write it was guarding simply
// proceeds unguarded (OBS-34, measured live on
// tmdb).
//
// The shape is llm_stop()'s, factored out of it: latch, cancel, wait for
// your own callbacks — not for the transfers, because a cancelled
// request still delivers.
//
// The unit is a **slot**, not a transfer, because a request that chains
// (point -> forecast -> observation) is one piece of work wearing a new
// curl id per leg. A slot opens when the plugin commits to the work,
// carries whichever leg is airborne, and closes when the last callback
// is done with it:
//
//   init()      curl_flight_init(&f)
//   entry       curl_flight_open(&f, &r->slot)      — refuse on FAIL
//   each leg    curl_flight_relay(&f, req, &r->slot)
//   terminal    curl_flight_close(&f, r->slot)      — LAST statement
//   stop()      curl_flight_drain(&f, ms)
//   deinit()    curl_flight_destroy(&f)
//
// ⚠ The close is what the drain waits for, so it marks the end of the
// work, not the end of the transfer. Close early and the drain returns
// while the callback is still reading the state deinit() is about to
// free — which is the bug, not a smaller version of it. Most plugins
// here already have the right line: the one release/free funnel every
// terminal path goes through.

// A slot carries either a curl request id (a leg is on the wire) or a
// token of the flight's own minting (nothing is). The high bit tells
// them apart, and curl's ids are a monotonic counter that will not reach
// it. Zero is neither: it is what a slot handle reads before it is
// opened and after it is closed, so closing one twice — or closing one
// that was never opened — finds nothing and does nothing.
#define CURL_FLIGHT_IDLE (1ULL << 63)

typedef struct
{
  pthread_mutex_t mu;
  pthread_cond_t  idle;      // broadcast when the set empties
  uint64_t       *slots;     // grow-only; never shrinks below high water
  uint32_t        n;
  uint32_t        cap;
  uint64_t        seq;       // mints the idle-slot tokens
  bool            grounded;  // latched by drain: nothing else takes off
} curl_flight_t;

// From init(). A flight is usable from any thread from here on.
void curl_flight_init(curl_flight_t *f);

// Open a slot for one piece of work. Nothing is on the wire yet, but
// the drain will wait for it from here on. FAIL means the flight is
// grounded (the plugin is stopping) and the caller must refuse the work
// rather than start it; *slot is left at 0, so the ordinary failure path
// may close it blind.
bool curl_flight_open(curl_flight_t *f, uint64_t *slot);

// Submit `req` as this slot's next leg. Ownership of `req` transfers
// either way, exactly as curl_request_submit's does; on FAIL the request
// was released and its callback will never fire, so the caller still
// owns its own context and must close the slot.
//
// The slot takes the new id BEFORE the submit — the completion can run
// on a curl worker before curl_request_submit has returned — and goes
// back to an idle token if the submit is refused.
bool curl_flight_relay(curl_flight_t *f, curl_request_t *req, uint64_t *slot);

// Give the slot back. A no-op for a handle that was never opened, or
// that has already been closed. The LAST statement of the work's
// terminal path — see the warning above.
void curl_flight_close(curl_flight_t *f, uint64_t slot);

// Ground the flight, cancel every leg still airborne, and wait up to
// `ms` for the slots to close. Returns the number still open — zero
// means every callback this plugin can be entered through has finished,
// and deinit() may free what they read. A non-zero return is a stop()
// that must refuse: FAIL there leaves the plugin running and intact,
// where proceeding would tear its state down under a live callback.
//
// The cancel is re-issued on every pass on purpose: a request in the
// moment between leaving curl's submit queue and entering its in-flight
// list is reachable by neither of curl's walks.
//
// A complete drain grounds the flight for good — nothing else takes off,
// and a plugin that stops and starts again re-arms with
// curl_flight_init. An incomplete one leaves it open, because a refused
// unload has to leave a working plugin behind.
uint32_t curl_flight_drain(curl_flight_t *f, uint32_t ms);

// From deinit(), after the drain. Frees the slot array and the lock. A
// flight with anything still open is a drain that timed out — destroy it
// anyway, since refusing here would only trade one leak for a zombie.
void curl_flight_destroy(curl_flight_t *f);

#endif
