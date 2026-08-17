#ifndef CHATBOT_HOLD_H
#define CHATBOT_HOLD_H

#include <stdbool.h>
#include <stdint.h>

// Teardown accounting for any record that outlives the turn that made
// it. A reply streams from a curl worker long after the line that asked
// for it, and a bot's state is freed the moment that bot is destroyed —
// which at `quit` is bot_exit(), three shutdown steps before the LLM
// engine cancels what is on the wire. Every such record carries a hold
// and is on hold.c's list from allocation to free; a teardown walks the
// list, disowns its own entries, cancels them at their engine and waits
// for the callbacks to arrive.
//
// This header deliberately includes no chat header. core's quiescence
// barrier range-tests tasks and curl requests and can see neither side
// of an LLM request, so the drain is chat's own — and the memory
// subsystem needs it too, which chatbot.h cannot give it (chatbot.h
// includes memory.h, so memory*.c cannot include chatbot.h).

// Seconds chatbot_hold_shutdown() will wait for disowned work to come
// back. Sized against core's Class-B grace: a straggler must cost the
// shutdown less than the unload it is holding up.
#define CHATBOT_HOLD_DRAIN_SECS   5

typedef struct chatbot_hold
{
  struct chatbot_hold *next;

  // Who this record belongs to: a bot's chatbot_state_t for per-turn
  // work, NULL for work the PLUGIN owns rather than any one bot — the
  // embed backfill, the live embed, a recall. The drain matches on this
  // pointer and never dereferences it, which is what lets a translation
  // unit below chatbot.h carry one.
  const void          *owner;

  // What to ask an engine to cut short. `llm_user` is the user_data an
  // LLM request was submitted with, `curl_id` a plain HTTP request's
  // stable identity; either may be absent, since a record between two
  // async legs has nothing on the wire. A cancelled request still
  // delivers, so these only shorten the wait — the drain is what makes
  // it safe.
  const void          *llm_user;
  uint64_t             curl_id;

  bool                 disowned;
} chatbot_hold_t;

// Put `h` on the live list under `owner`. `llm_user` is the user_data
// the record's LLM requests will carry (NULL when it submits none).
void chatbot_hold_link(chatbot_hold_t *h, const void *owner,
    const void *llm_user);

// Name the HTTP request this record is currently waiting on, so a
// teardown can cancel it. 0 clears.
void chatbot_hold_set_curl(chatbot_hold_t *h, uint64_t curl_id);

// True once a teardown has disowned this record. Every callback that
// resumes one asks first: a true answer means free it and touch nothing
// that belonged to the owner.
bool chatbot_hold_disowned(const chatbot_hold_t *h);

// Take `h` off the live list and wake any drain waiting on it. Called
// from the record's own free path, never before it.
void chatbot_hold_unlink(chatbot_hold_t *h);

// Disown every record owned by `owner`, cancel what can be cancelled at
// its engine, and wait for the callbacks to arrive. Called from BOTH
// chatbot_stop() and chatbot_destroy() for a bot: bot_destroy() reaches
// destroy() with no stop() when the bot was never RUNNING, and a
// delivery still inside on_message can submit a fresh reply after
// stop() has already drained. `owner` NULL drains the plugin's own
// work at memory_stop(). Returns once nothing airborne can still
// dereference the owner — or after CHATBOT_HOLD_DRAIN_SECS, which logs
// a WARN and leaves the stragglers disowned rather than blocking the
// shutdown for ever.
void chatbot_hold_shutdown(const void *owner);

#endif
