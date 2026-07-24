// botmanager — MIT
// Post-acquire volunteer speech: gate cascade → seed msg → chatbot_reply_submit.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "alloc.h"
#include "bot.h"
#include "clam.h"
#include "inference.h"
#include "kv.h"
#include "method.h"
#include "task.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Job struct — copied out of the acquire callback so we survive the hop
// onto the main task queue. Freed by the task that consumes it.

typedef struct
{
  char     bot_name  [ACQUIRE_BOT_NAME_SZ];
  char     topic_name[ACQUIRE_TOPIC_NAME_SZ];
  char     subject   [ACQUIRE_SUBJECT_SZ];
  int64_t  chunk_id;
  uint32_t relevance;
  acquire_ingest_mode_t mode;

  // V2 — populated by the semantic-dedup gate in volunteer_cascade()
  // when the gate runs (threshold > 0 and the embedding row is ready).
  // Threaded through so volunteer_submit() can stamp the embed ring
  // without a second knowledge_get_chunk_embedding() round-trip.
  // seed_dim == 0 means "no vec available" (gate disabled, embed not
  // yet written, or dim > CHATBOT_VOLUNTEER_EMBED_DIM_MAX).
  uint32_t seed_dim;
  float    seed_vec[CHATBOT_VOLUNTEER_EMBED_DIM_MAX];
} volunteer_job_t;

// Channel discovery — ask the bot's method driver for the channels it
// is currently joined to, and pick the first one that passes the
// remaining gates. Live membership, not KV configuration: a bot that
// has manually parted an autojoin channel won't volunteer into it, and
// a bot that has manually joined a non-autojoin channel can volunteer
// there. Drivers without multi-channel membership (e.g. botmanctl)
// leave list_joined_channels unimplemented and yield zero channels.

typedef struct
{
  char   channels[CHATBOT_VOLUNTEER_CHAN_SLOTS][METHOD_CHANNEL_SZ];
  size_t n_channels;
} volunteer_chan_collect_t;

static void
volunteer_collect_joined_cb(const char *channel, void *data)
{
  volunteer_chan_collect_t *ctx = data;

  if(ctx->n_channels >= CHATBOT_VOLUNTEER_CHAN_SLOTS)
    return;

  if(channel == NULL || channel[0] == '\0')
    return;

  snprintf(ctx->channels[ctx->n_channels], METHOD_CHANNEL_SZ, "%s",
      channel);
  ctx->n_channels++;
}

// Per-channel volunteer ring helpers. All run under st->volunteer.mutex.

static chatbot_volunteer_slot_t *
volunteer_slot_find_locked(chatbot_volunteer_state_t *v, const char *chan)
{
  for(size_t i = 0; i < CHATBOT_VOLUNTEER_CHAN_SLOTS; i++)
    if(strcmp(v->slots[i].channel, chan) == 0)
      return(&v->slots[i]);
  return(NULL);
}

static chatbot_volunteer_slot_t *
volunteer_slot_get_or_evict_locked(chatbot_volunteer_state_t *v,
    const char *chan)
{
  chatbot_volunteer_slot_t *hit = volunteer_slot_find_locked(v, chan);
  size_t oldest;
  time_t oldest_t;

  if(hit != NULL) return(hit);

  // Find an empty slot first.
  for(size_t i = 0; i < CHATBOT_VOLUNTEER_CHAN_SLOTS; i++)
  {
    if(v->slots[i].channel[0] == '\0')
    {
      snprintf(v->slots[i].channel, sizeof(v->slots[i].channel),
          "%s", chan);
      v->slots[i].last_volunteered = 0;
      v->slots[i].last_attempted   = 0;
      return(&v->slots[i]);
    }
  }

  // Evict the oldest (min of last_attempted / last_volunteered).
  oldest = 0;
  oldest_t = v->slots[0].last_attempted > v->slots[0].last_volunteered
      ? v->slots[0].last_attempted : v->slots[0].last_volunteered;

  for(size_t i = 1; i < CHATBOT_VOLUNTEER_CHAN_SLOTS; i++)
  {
    time_t t = v->slots[i].last_attempted > v->slots[i].last_volunteered
        ? v->slots[i].last_attempted : v->slots[i].last_volunteered;
    if(t < oldest_t) { oldest = i; oldest_t = t; }
  }

  snprintf(v->slots[oldest].channel, sizeof(v->slots[oldest].channel),
      "%s", chan);
  v->slots[oldest].last_volunteered = 0;
  v->slots[oldest].last_attempted   = 0;
  return(&v->slots[oldest]);
}

// V2 content-dedup ring helpers. All run with st->volunteer.mutex held
// by the caller; functions suffixed _locked don't take the lock.

static bool
volunteer_chunk_ring_hit_locked(chatbot_volunteer_state_t *v, int64_t chunk_id)
{
  if(chunk_id <= 0) return(false);

  for(size_t i = 0; i < CHATBOT_VOLUNTEER_CHUNK_RING; i++)
    if(v->chunk_ring[i].chunk_id == chunk_id)
      return(true);
  return(false);
}

static void
volunteer_chunk_ring_stamp_locked(chatbot_volunteer_state_t *v,
    int64_t chunk_id, time_t now)
{
  size_t oldest;
  time_t oldest_t;

  if(chunk_id <= 0) return;

  // Refresh existing slot first so repeat writes don't fragment the ring.
  for(size_t i = 0; i < CHATBOT_VOLUNTEER_CHUNK_RING; i++)
  {
    if(v->chunk_ring[i].chunk_id == chunk_id)
    {
      v->chunk_ring[i].volunteered_at = now;
      return;
    }
  }

  // Empty slot.
  for(size_t i = 0; i < CHATBOT_VOLUNTEER_CHUNK_RING; i++)
  {
    if(v->chunk_ring[i].chunk_id == 0)
    {
      v->chunk_ring[i].chunk_id       = chunk_id;
      v->chunk_ring[i].volunteered_at = now;
      return;
    }
  }

  // Evict oldest.
  oldest = 0;
  oldest_t = v->chunk_ring[0].volunteered_at;
  for(size_t i = 1; i < CHATBOT_VOLUNTEER_CHUNK_RING; i++)
  {
    if(v->chunk_ring[i].volunteered_at < oldest_t)
    {
      oldest = i;
      oldest_t = v->chunk_ring[i].volunteered_at;
    }
  }
  v->chunk_ring[oldest].chunk_id       = chunk_id;
  v->chunk_ring[oldest].volunteered_at = now;
}

static time_t
volunteer_subject_ring_last_locked(chatbot_volunteer_state_t *v,
    const char *subject)
{
  if(subject == NULL || subject[0] == '\0') return(0);

  for(size_t i = 0; i < CHATBOT_VOLUNTEER_SUBJECT_RING; i++)
    if(v->subj_ring[i].subject[0] != '\0'
        && strcasecmp(v->subj_ring[i].subject, subject) == 0)
      return(v->subj_ring[i].volunteered_at);
  return(0);
}

static void
volunteer_subject_ring_stamp_locked(chatbot_volunteer_state_t *v,
    const char *subject, time_t now)
{
  size_t oldest;
  time_t oldest_t;

  if(subject == NULL || subject[0] == '\0') return;

  for(size_t i = 0; i < CHATBOT_VOLUNTEER_SUBJECT_RING; i++)
  {
    if(v->subj_ring[i].subject[0] != '\0'
        && strcasecmp(v->subj_ring[i].subject, subject) == 0)
    {
      v->subj_ring[i].volunteered_at = now;
      return;
    }
  }

  for(size_t i = 0; i < CHATBOT_VOLUNTEER_SUBJECT_RING; i++)
  {
    if(v->subj_ring[i].subject[0] == '\0')
    {
      snprintf(v->subj_ring[i].subject, sizeof(v->subj_ring[i].subject),
          "%s", subject);
      v->subj_ring[i].volunteered_at = now;
      return;
    }
  }

  oldest = 0;
  oldest_t = v->subj_ring[0].volunteered_at;
  for(size_t i = 1; i < CHATBOT_VOLUNTEER_SUBJECT_RING; i++)
  {
    if(v->subj_ring[i].volunteered_at < oldest_t)
    {
      oldest = i;
      oldest_t = v->subj_ring[i].volunteered_at;
    }
  }
  snprintf(v->subj_ring[oldest].subject, sizeof(v->subj_ring[oldest].subject),
      "%s", subject);
  v->subj_ring[oldest].volunteered_at = now;
}

// Scans the embed ring for the highest cosine against `vec`. Skips slots
// with a different `dim` (stale vectors from a model swap) and the slot
// that belongs to the candidate's own chunk_id (shouldn't happen once the
// chunk ring has stamped it, but defensive). Returns the max cosine as an
// integer 0-100 (matches the KV knob's scale; avoids a float threshold in
// the caller) and writes the matching chunk_id into *out_chunk when > 0.
static uint32_t
volunteer_embed_ring_max_cos_locked(chatbot_volunteer_state_t *v,
    const float *vec, uint32_t dim, int64_t self_chunk, int64_t *out_chunk)
{
  float best = 0.0f;
  int64_t best_chunk = 0;

  for(size_t i = 0; i < CHATBOT_VOLUNTEER_EMBED_RING; i++)
  {
    float c;

    if(v->embed_ring[i].dim != dim) continue;
    if(v->embed_ring[i].chunk_id == 0) continue;
    if(v->embed_ring[i].chunk_id == self_chunk) continue;

    c = knowledge_cosine(v->embed_ring[i].vec, vec, dim);
    if(c > best)
    {
      best = c;
      best_chunk = v->embed_ring[i].chunk_id;
    }
  }

  if(out_chunk != NULL) *out_chunk = best_chunk;

  if(best <= 0.0f) return(0);
  if(best >= 1.0f) return(100);
  return((uint32_t)(best * 100.0f + 0.5f));
}

static void
volunteer_embed_ring_stamp_locked(chatbot_volunteer_state_t *v,
    int64_t chunk_id, uint32_t dim, const float *vec, time_t now)
{
  size_t oldest;
  time_t oldest_t;

  if(chunk_id <= 0 || dim == 0 || dim > CHATBOT_VOLUNTEER_EMBED_DIM_MAX
      || vec == NULL)
    return;

  // Refresh existing slot if the same chunk_id is already stamped.
  for(size_t i = 0; i < CHATBOT_VOLUNTEER_EMBED_RING; i++)
  {
    if(v->embed_ring[i].chunk_id == chunk_id)
    {
      v->embed_ring[i].dim            = dim;
      v->embed_ring[i].volunteered_at = now;
      memcpy(v->embed_ring[i].vec, vec, sizeof(float) * dim);
      return;
    }
  }

  for(size_t i = 0; i < CHATBOT_VOLUNTEER_EMBED_RING; i++)
  {
    if(v->embed_ring[i].chunk_id == 0)
    {
      v->embed_ring[i].chunk_id       = chunk_id;
      v->embed_ring[i].dim            = dim;
      v->embed_ring[i].volunteered_at = now;
      memcpy(v->embed_ring[i].vec, vec, sizeof(float) * dim);
      return;
    }
  }

  oldest = 0;
  oldest_t = v->embed_ring[0].volunteered_at;
  for(size_t i = 1; i < CHATBOT_VOLUNTEER_EMBED_RING; i++)
  {
    if(v->embed_ring[i].volunteered_at < oldest_t)
    {
      oldest = i;
      oldest_t = v->embed_ring[i].volunteered_at;
    }
  }
  v->embed_ring[oldest].chunk_id       = chunk_id;
  v->embed_ring[oldest].dim            = dim;
  v->embed_ring[oldest].volunteered_at = now;
  memcpy(v->embed_ring[oldest].vec, vec, sizeof(float) * dim);
}

// Channel-activity lookups via the engagement ring. The engagement ring
// tracks per-(channel, user) last_seen; the most-recent entry for a
// channel is a good approximation of "last witnessed activity".

static time_t
volunteer_channel_last_activity(chatbot_state_t *st, const char *chan)
{
  time_t newest = 0;

  pthread_mutex_lock(&st->engagement.mutex);
  for(size_t i = 0; i < CHATBOT_ENGAGEMENT_SLOTS; i++)
    if(strcmp(st->engagement.slots[i].channel, chan) == 0
        && st->engagement.slots[i].last_seen > newest)
      newest = st->engagement.slots[i].last_seen;
  pthread_mutex_unlock(&st->engagement.mutex);

  return(newest);
}

// Gate cascade. Each gate that drops logs a CLAM_DEBUG line naming the
// reason so operators can tune. Returns true if the post should proceed.
// On success, `chosen_chan` is populated.

static bool
volunteer_cascade(chatbot_state_t *st, volunteer_job_t *job,
    char *chosen_chan, size_t chosen_chan_sz)
{
  const char *bot_name = job->bot_name;
  uint32_t    relevance = job->relevance;
  char        kbuf[160];
  time_t      now = time(NULL);
  uint32_t min_since_own;
  uint32_t min_quiet;
  uint32_t max_quiet;
  uint32_t chan_cooldown;
  volunteer_chan_collect_t cc;
  method_inst_t *method;
  uint32_t thresh;
  uint32_t hcap;
  uint32_t roll;
  uint32_t prob;
  uint32_t subj_cd;
  bool dup_chunk;
  uint32_t floor;

  // (a) Master switch.
  snprintf(kbuf, sizeof(kbuf), "bot.%s.behavior.volunteer.enabled", bot_name);
  if(kv_get_uint(kbuf) == 0)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=disabled", bot_name);
    return(false);
  }

  // (b) Relevance floor.
  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.relevance_floor", bot_name);
  floor = (uint32_t)kv_get_uint(kbuf);
  if(floor > 0 && relevance < floor)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=relevance (%u < %u)",
        bot_name, relevance, floor);
    return(false);
  }

  // (b1) Chunk-id ring — cheapest content gate. Fires when acquire
  // re-invokes the ingest callback with a chunk_id we've already
  // spoken about. Typically doesn't hit (acquire's URL dedup LRU
  // catches the common case); kept for defensive coverage against
  // future ingest paths that skip that LRU.
  pthread_mutex_lock(&st->volunteer.mutex);
  dup_chunk = volunteer_chunk_ring_hit_locked(&st->volunteer, job->chunk_id);
  pthread_mutex_unlock(&st->volunteer.mutex);
  if(dup_chunk)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=same_chunk chunk=%" PRId64,
        bot_name, job->chunk_id);
    return(false);
  }

  // (b2) Subject ring — case-insensitive exact match on the acquire-
  // resolved subject string. Catches re-ingests that produce a fresh
  // chunk_id for the same underlying story.
  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.subject_cooldown_secs", bot_name);
  subj_cd = (uint32_t)kv_get_uint(kbuf);
  if(subj_cd > 0 && job->subject[0] != '\0')
  {
    time_t last_subj;

    pthread_mutex_lock(&st->volunteer.mutex);
    last_subj = volunteer_subject_ring_last_locked(
        &st->volunteer, job->subject);
    pthread_mutex_unlock(&st->volunteer.mutex);
    if(last_subj > 0 && now - last_subj < (time_t)subj_cd)
    {
      clam(CLAM_DEBUG, "chatbot",
          "volunteer skip bot=%s subject='%s' reason=subject_cooldown"
          " last_vol=%ld cd=%us",
          bot_name, job->subject, (long)last_subj, subj_cd);
      return(false);
    }
  }

  // (c) Probability roll.
  snprintf(kbuf, sizeof(kbuf), "bot.%s.behavior.volunteer.prob", bot_name);
  prob = (uint32_t)kv_get_uint(kbuf);
  if(prob == 0)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=prob0", bot_name);
    return(false);
  }
  roll = (uint32_t)(rand() % 100);
  if(roll >= prob)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=probability (%u >= %u)",
        bot_name, roll, prob);
    return(false);
  }

  // (d) Hourly cap (under the volunteer ring mutex).
  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.max_per_hour", bot_name);
  hcap = (uint32_t)kv_get_uint(kbuf);
  if(hcap == 0) hcap = 3;

  pthread_mutex_lock(&st->volunteer.mutex);

  if(now - st->volunteer.per_hour_window_start >= 3600)
  {
    st->volunteer.per_hour_window_start = now;
    st->volunteer.per_hour_counter      = 0;
  }

  if(st->volunteer.per_hour_counter >= hcap)
  {
    pthread_mutex_unlock(&st->volunteer.mutex);
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=hourly_cap (%u >= %u)",
        bot_name, st->volunteer.per_hour_counter, hcap);
    return(false);
  }

  pthread_mutex_unlock(&st->volunteer.mutex);

  // (d1) Semantic dedup — cosine between this chunk's embedding and
  // the last CHATBOT_VOLUNTEER_EMBED_RING posted chunks. Gated behind
  // the hourly cap because it requires a DB read plus up to
  // CHATBOT_VOLUNTEER_EMBED_RING cosine evaluations.
  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.dedup_threshold", bot_name);
  thresh = (uint32_t)kv_get_uint(kbuf);
  if(thresh > 0)
  {
    uint32_t dim = knowledge_get_chunk_embedding(job->chunk_id,
        job->seed_vec, CHATBOT_VOLUNTEER_EMBED_DIM_MAX);
    if(dim > 0)
    {
      int64_t  dup_chunk_id = 0;
      uint32_t max_cos;

      pthread_mutex_lock(&st->volunteer.mutex);
      max_cos = volunteer_embed_ring_max_cos_locked(
          &st->volunteer, job->seed_vec, dim, job->chunk_id,
          &dup_chunk_id);
      pthread_mutex_unlock(&st->volunteer.mutex);
      if(max_cos >= thresh)
      {
        clam(CLAM_DEBUG, "chatbot",
            "volunteer skip bot=%s chunk=%" PRId64
            " reason=semantic_dup cos=%u/100 dup_chunk=%" PRId64,
            bot_name, job->chunk_id, max_cos, dup_chunk_id);
        return(false);
      }
      // Thread the vec through to volunteer_submit for stamping.
      job->seed_dim = dim;
    }
    // dim == 0: embedding not ready yet (or > DIM_MAX). Cheaper gates
    // above already fired; let the volunteer through and skip the
    // embed-ring stamp at submit time.
  }

  // (e) Channel iteration. Ask the bot's method driver for its live
  //     set of joined channels, then pick the first that passes
  //     cooldown + quiet windows. Must match the method we'll later
  //     submit to (volunteer_submit also uses bot_first_method).
  method = bot_first_method(st->inst);
  if(method == NULL)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=no_method", bot_name);
    return(false);
  }

  memset(&cc, 0, sizeof(cc));
  method_list_joined_channels(method, volunteer_collect_joined_cb, &cc);

  if(cc.n_channels == 0)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s reason=no_joined_channels", bot_name);
    return(false);
  }

  // KV defaults are pinned at schema registration; 0 means "gate
  // disabled" (not "use the compiled default"). Read raw.
  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.channel_cooldown_secs", bot_name);
  chan_cooldown = (uint32_t)kv_get_uint(kbuf);

  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.max_quiet_secs", bot_name);
  max_quiet = (uint32_t)kv_get_uint(kbuf);

  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.min_quiet_secs", bot_name);
  min_quiet = (uint32_t)kv_get_uint(kbuf);

  snprintf(kbuf, sizeof(kbuf),
      "bot.%s.behavior.volunteer.min_since_own_secs", bot_name);
  min_since_own = (uint32_t)kv_get_uint(kbuf);

  for(size_t i = 0; i < cc.n_channels; i++)
  {
    const char *chan = cc.channels[i];
    time_t own_last;
    time_t last_activity;
    time_t last_volunteered;
    chatbot_volunteer_slot_t *slot;

    // (f) Per-channel cooldown.
    pthread_mutex_lock(&st->volunteer.mutex);
    last_volunteered = 0;
    slot = volunteer_slot_find_locked(&st->volunteer, chan);
    if(slot != NULL) last_volunteered = slot->last_volunteered;
    pthread_mutex_unlock(&st->volunteer.mutex);

    if(last_volunteered > 0
        && now - last_volunteered < (time_t)chan_cooldown)
    {
      clam(CLAM_DEBUG, "chatbot",
          "volunteer skip bot=%s chan=%s reason=cooldown"
          " (last_vol=%ld cd=%us)",
          bot_name, chan, (long)last_volunteered, chan_cooldown);
      continue;
    }

    // (g/h) Active/dead channel windows.
    last_activity = volunteer_channel_last_activity(st, chan);
    if(last_activity > 0)
    {
      time_t age = now - last_activity;

      if(age < (time_t)min_quiet)
      {
        clam(CLAM_DEBUG, "chatbot",
            "volunteer skip bot=%s chan=%s reason=active_channel (age=%lds)",
            bot_name, chan, (long)age);
        continue;
      }

      if(max_quiet > 0 && age > (time_t)max_quiet)
      {
        clam(CLAM_DEBUG, "chatbot",
            "volunteer skip bot=%s chan=%s reason=dead_channel (age=%lds)",
            bot_name, chan, (long)age);
        continue;
      }
    }
    // Unknown (no engagement entry yet) — fall through. Don't block the
    // first post into a fresh channel.

    // (i) Bot-own cooldown via chatbot_inflight_last_reply.
    own_last = chatbot_inflight_last_reply(st, chan);
    if(own_last > 0 && now - own_last < (time_t)min_since_own)
    {
      clam(CLAM_DEBUG, "chatbot",
          "volunteer skip bot=%s chan=%s reason=own_cooldown",
          bot_name, chan);
      continue;
    }

    // Channel passed all gates.
    snprintf(chosen_chan, chosen_chan_sz, "%s", chan);
    return(true);
  }

  clam(CLAM_DEBUG, "chatbot",
      "volunteer skip bot=%s reason=no_channel_passed_gates",
      bot_name);
  return(false);
}

// Compose + submit. Builds a synthetic method_msg_t that routes through
// the existing reply pipeline so RAG, KNOWLEDGE, and IMAGES all splice
// naturally. The seed text is framed as a user-side "you just noticed"
// note; the persona retains SKIP as the final veto.

static void
volunteer_submit(chatbot_state_t *st, const volunteer_job_t *job,
    const char *chan)
{
  bot_inst_t *inst = st->inst;
  chatbot_volunteer_slot_t *slot;
  time_t now;
  method_msg_t msg;
  method_inst_t *method;

  if(inst == NULL) return;

  method = bot_first_method(inst);
  if(method == NULL)
  {
    clam(CLAM_DEBUG, "chatbot",
        "volunteer skip bot=%s chan=%s reason=no_method",
        job->bot_name, chan);
    return;
  }

  memset(&msg, 0, sizeof(msg));
  msg.inst      = method;
  msg.timestamp = time(NULL);

  // `sender` is synthetic: not the bot's nick (so dossier resolution
  // doesn't echo the bot into its own logs), not a real user either.
  // The reply pipeline will log EXCHANGE_OUT against this sender; any
  // downstream tools can filter by sender='acquire-seed' to find
  // volunteer-driven rows.
  snprintf(msg.sender,   sizeof(msg.sender),  "acquire-seed");
  snprintf(msg.channel,  sizeof(msg.channel), "%s", chan);

  // The seed is the *user turn* of a single-message exchange. Persona
  // body + anti-injection reminder + RAG all run above it; the model
  // produces either a short in-character mention or a SKIP line.
  snprintf(msg.text, sizeof(msg.text),
      "[internal cue: you've just read something new about '%s' "
      "(topic=%s, relevance=%u) via your regular feeds. If it's worth "
      "mentioning in %s right now, post a single short in-character "
      "line referencing it. If it's not channel-appropriate or not "
      "worth bringing up, reply with SKIP on its own line. Do not "
      "explain yourself.]",
      job->subject, job->topic_name, job->relevance, chan);

  clam(CLAM_INFO, "chatbot",
      "volunteer posted bot=%s chan=%s subject='%s' chunk=%" PRId64
      " relevance=%u mode=%s",
      job->bot_name, chan, job->subject, job->chunk_id, job->relevance,
      job->mode == ACQUIRE_INGEST_PROACTIVE ? "proactive" : "reactive");

  // Stamp attempt + bump hourly counter BEFORE submit so two
  // back-to-back ingests don't both pass the hourly cap race. The
  // per-channel slot's last_volunteered is stamped here too — we
  // accept that SKIP still counts against the cap for v1 (SKIP frequency
  // is a tuning signal; if it's constant, lower the probability).
  //
  // V2: stamp all three content rings under the same critical section
  // so a concurrent cascade observes a consistent view. Stamps apply
  // on attempt (matches V1 semantics): a persona SKIP on a given
  // subject shouldn't invite an instant retry.
  now = time(NULL);
  pthread_mutex_lock(&st->volunteer.mutex);
  slot = volunteer_slot_get_or_evict_locked(&st->volunteer, chan);
  slot->last_volunteered = now;
  slot->last_attempted   = slot->last_volunteered;
  st->volunteer.per_hour_counter++;

  volunteer_chunk_ring_stamp_locked(&st->volunteer, job->chunk_id, now);
  if(job->subject[0] != '\0')
    volunteer_subject_ring_stamp_locked(&st->volunteer, job->subject, now);
  if(job->seed_dim > 0)
    volunteer_embed_ring_stamp_locked(&st->volunteer, job->chunk_id,
        job->seed_dim, job->seed_vec, now);
  pthread_mutex_unlock(&st->volunteer.mutex);

  // VF-1: proves the V1 volunteer path is the one that spoke. A
  // path=volunteer line appearing without a preceding trace=path
  // path=reply for the same target is the smoking gun for case B.
  clam(CLAM_DEBUG, "interject",
      "trace=path bot=%s path=volunteer chan=%s subject='%s'",
      job->bot_name, chan, job->subject);

  // Hand off to the existing reply pipeline. was_addressed=false so the
  // mention-expansion / dossier-fan-out paths stay quiet — the seed
  // doesn't name anyone, and we don't want synthetic dossier hits.
  // is_direct_address=false: volunteer speech is spontaneous, never a
  // reply to a direct address, so the CV-4 SKIP fallback must not fire.
  chatbot_reply_submit(st, &msg, false, false);
}

// Task callback — consumes a volunteer_job_t that was queued from the
// acquire callback. Runs the cascade and, if a channel passes, submits.

static void
volunteer_task(task_t *t)
{
  volunteer_job_t *job = t->data;
  char chan[METHOD_CHANNEL_SZ];
  chatbot_state_t *st;
  bot_inst_t *inst;

  if(job == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  inst = bot_find(job->bot_name);
  if(inst == NULL)
  {
    mem_free(job);
    t->state = TASK_ENDED;
    return;
  }

  st = bot_get_handle(inst);
  if(st == NULL)
  {
    mem_free(job);
    t->state = TASK_ENDED;
    return;
  }

  if(volunteer_cascade(st, job, chan, sizeof(chan)))
    volunteer_submit(st, job, chan);

  mem_free(job);
  t->state = TASK_ENDED;
}

// Acquire callback. Runs on the curl worker thread; must not do anything
// expensive here. Copy the args into a job struct and schedule the task.

static void
volunteer_ingest_cb(const char *bot_name, const char *topic_name,
    const char *subject, const char *corpus, int64_t chunk_id,
    uint32_t relevance, acquire_ingest_mode_t mode, void *user)
{
  task_t *t;
  volunteer_job_t *job;

  (void)corpus;
  (void)user;

  if(bot_name == NULL || topic_name == NULL || subject == NULL)
    return;

  job = mem_alloc("chatbot", "volunteer_job", sizeof(*job));
  if(job == NULL) return;

  memset(job, 0, sizeof(*job));
  snprintf(job->bot_name,   sizeof(job->bot_name),   "%s", bot_name);
  snprintf(job->topic_name, sizeof(job->topic_name), "%s", topic_name);
  snprintf(job->subject,    sizeof(job->subject),    "%s", subject);
  job->chunk_id  = chunk_id;
  job->relevance = relevance;
  job->mode      = mode;

  t = task_add("chatbot.volunteer", TASK_ANY, 200,
      volunteer_task, job);

  if(t == NULL)
  {
    mem_free(job);
    clam(CLAM_WARN, "chatbot",
        "volunteer task spawn failed bot=%s subject='%s'",
        bot_name, subject);
  }
}

// Public lifecycle. Pair init/deinit with chatbot_plugin_init/deinit.

bool
chatbot_volunteer_init(void)
{
  acquire_register_ingest_cb(volunteer_ingest_cb, NULL);
  return(SUCCESS);
}

void
chatbot_volunteer_deinit(void)
{
  acquire_register_ingest_cb(NULL, NULL);
}
