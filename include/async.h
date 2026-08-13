#ifndef BM_ASYNC_H
#define BM_ASYNC_H

// What an *_async() submit call returns, and the whole of the contract
// its consumer needs. The question a bare bool could not answer was
// whether a refusal had already fired the completion callback, because
// that — not the refusal — decides who still owns the closure. Guessing
// it wrong frees a closure the callback already freed, and the tracked
// allocator aborts: no FATAL line, no core, the log simply stops.
//
// ASYNC_AIRBORNE is 0, hence SUCCESS, so a call site that only asks
// "did this get off the ground?" still reads `!= SUCCESS` and still
// means it. A call site that owns a closure asks the named question:
//
//   if(foo_async(..., cb, ctx) == ASYNC_FAILED_UNDELIVERED)
//     mem_free(ctx);
//
// Each value claims who owns the closure, not when anything runs —
// ASYNC_AIRBORNE covers a warm cache that answered from inside the
// submit call as well as a request still on the wire, because the
// callback owns your closure either way. That a callback CAN run
// before the submit returns is a locking concern, not an ownership
// one, and stays where it belongs: PLUGIN.md §Async failure
// semantics, which also tables which service follows which
// convention. Which failures a given entry point can return is that
// entry point's own contract, stated in its header.
typedef enum
{
  ASYNC_AIRBORNE = 0,        // accepted; the callback owns your closure
  ASYNC_FAILED_DELIVERED,    // refused, and the callback ALREADY fired
  ASYNC_FAILED_UNDELIVERED   // refused, nothing fired — free your closure
} async_rc_t;

#endif
