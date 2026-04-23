// botmanager — MIT
// Pure decision: reply | interject | silent (no IO, no locks, test-friendly).

#define CHATBOT_INTERNAL
#include "chatbot.h"

// Effective-prob ceiling: even with a fully-relevant on-topic line we
// never raise the per-roll fire probability above this. Combined with
// reply_cooldown_secs and witness_interject_cooldown_secs, this caps
// burst rate without needing a separate token bucket.
#define CHATBOT_INTERJECT_PROB_MAX 75

chatbot_speak_t
chatbot_speak_decide(mem_msg_kind_t kind,
    uint32_t in_flight, uint32_t max_inflight,
    time_t now_secs, time_t last_reply_secs, uint32_t cooldown_secs,
    time_t last_witness_interject, uint32_t witness_cooldown_secs,
    uint32_t interject_prob, uint32_t witness_base_prob,
    uint32_t rand_roll,
    bool harm_signal, uint32_t relevance_boost_pct)
{
  bool in_cooldown;
  uint32_t effective_prob;

  // Max in-flight cap preempts everything — the LLM pipeline is backed
  // up and adding work would make it worse. Harm warnings defer to this
  // because the alternative (flooding a failing pipeline) is itself a
  // reliability harm.
  if(max_inflight > 0 && in_flight >= max_inflight)
    return(CHATBOT_SPEAK_IGNORE);

  // Direct address honours max_inflight but ignores cooldown — a user
  // who addresses the bot is always answered (subject to inflight cap).
  if(kind == MEM_MSG_EXCHANGE_IN)
    return(CHATBOT_SPEAK_REPLY);

  // WITNESS harm-warning fast-path: a line flagged by the caller as
  // destructive (chmod -R 777, rm -rf /, curl | sh, dd of=/dev/sd*, …)
  // preempts BOTH cooldown gates. A safety override that a 30-second
  // cooldown defeats is not a safety override. See
  // finding_harm_bypass_below_cooldown.md — this placement is
  // deliberate.
  if(harm_signal)
    return(CHATBOT_SPEAK_INTERJECT);

  // WITNESS path past this point: gated by reply cooldown,
  // witness-interject cooldown, AND probability. The witness cooldown
  // (VF-3) caps interject distribution so a run of high rolls across
  // probes cannot concentrate replies. 0 disables the gate.
  in_cooldown = (last_reply_secs > 0
      && cooldown_secs > 0
      && (now_secs - last_reply_secs) < (time_t)cooldown_secs);

  if(in_cooldown) return(CHATBOT_SPEAK_IGNORE);

  if(witness_cooldown_secs > 0
      && last_witness_interject > 0
      && (now_secs - last_witness_interject) < (time_t)witness_cooldown_secs)
    return(CHATBOT_SPEAK_IGNORE);

  // Untopical vs. topical split. Topical lines (relevance_boost_pct > 0)
  // use interject_prob multiplied by the boost and capped at
  // CHATBOT_INTERJECT_PROB_MAX; untopical lines use a much lower base
  // floor (witness_base_prob) so small-talk cannot ride a high
  // interject_prob into an unwanted fire. witness_base_prob=0 disables
  // untopical interjects outright.
  if(relevance_boost_pct > 0)
  {
    uint64_t scaled;

    if(interject_prob == 0) return(CHATBOT_SPEAK_IGNORE);
    scaled = (uint64_t)interject_prob
        * (uint64_t)(100u + relevance_boost_pct) / 100u;
    if(scaled > CHATBOT_INTERJECT_PROB_MAX)
      scaled = CHATBOT_INTERJECT_PROB_MAX;
    effective_prob = (uint32_t)scaled;
  }

  else
    effective_prob = witness_base_prob;

  if(effective_prob == 0) return(CHATBOT_SPEAK_IGNORE);
  if(rand_roll >= effective_prob) return(CHATBOT_SPEAK_IGNORE);

  return(CHATBOT_SPEAK_INTERJECT);
}

const char *
chatbot_speak_name(chatbot_speak_t d)
{
  switch(d)
  {
    case CHATBOT_SPEAK_REPLY:     return("REPLY");
    case CHATBOT_SPEAK_INTERJECT: return("INTERJECT");
    case CHATBOT_SPEAK_IGNORE:    return("IGNORE");
  }
  return("UNKNOWN");
}
