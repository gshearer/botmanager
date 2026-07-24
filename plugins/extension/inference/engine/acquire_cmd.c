// botmanager — MIT
// Acquisition engine: user commands + A8 corpus lifecycle sweep.

#include "acquire_priv.h"
#include "knowledge_priv.h"

#include "cmd.h"
#include "db.h"
#include "method.h"
#include "userns.h"
#include "util.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// /acquire digest-test  (helper; removed or kept as prompt-tuning
// surface once A6/A7 land)

typedef struct
{
  cmd_ctx_t    ctx;
  method_msg_t msg;
  char         topic[ACQUIRE_TOPIC_NAME_SZ];
} acquire_digest_test_ctx_t;

static void
acquire_digest_test_done(const acquire_digest_response_t *resp)
{
  acquire_digest_test_ctx_t *tc =
      (acquire_digest_test_ctx_t *)resp->user_data;
  char line[640];

  if(!resp->ok)
  {
    snprintf(line, sizeof(line), "digest-test: FAIL (%s)",
        resp->error != NULL ? resp->error : "unknown");
    cmd_reply(&tc->ctx, line);
    mem_free(tc);
    return;
  }

  snprintf(line, sizeof(line),
      "digest-test topic='%s' relevance=%u", tc->topic, resp->relevance);
  cmd_reply(&tc->ctx, line);

  if(resp->summary != NULL && resp->summary[0] != '\0')
  {
    snprintf(line, sizeof(line), "  summary: %.*s",
        (int)(sizeof(line) - 14), resp->summary);
    cmd_reply(&tc->ctx, line);
  }

  mem_free(tc);
}

static void
acquire_cmd_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /acquire <subcommand>");
  cmd_reply(ctx, "  trigger <bot> <topic>         — fire a proactive query now");
  cmd_reply(ctx, "  fire <bot> <topic> <subject>  — enqueue a reactive job now");
  cmd_reply(ctx, "  source trigger <bot> <url>    — arm a personality feed for next tick");
  cmd_reply(ctx, "  digest-test <topic> <body>    — dry-run the digester");
}

static void
acquire_cmd_source_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /acquire source <subcommand>");
  cmd_reply(ctx, "  trigger <bot> <url>  — arm a declared feed for the next tick");
}

static void
acquire_cmd_digest_test(const cmd_ctx_t *ctx)
{
  const char *args = ctx->args;

  const char *topic_start;
  size_t topic_len;
  const char *body;
  acquire_digest_test_ctx_t *tc;
  const char *topic_end;
  if(args == NULL || args[0] == '\0')
  {
    cmd_reply(ctx, "usage: /acquire digest-test <topic> <body>");
    return;
  }

  // Split raw args on the first run of whitespace: topic then body.
  while(*args == ' ' || *args == '\t') args++;

  topic_start = args;
  topic_end = args;

  while(*topic_end != '\0' && *topic_end != ' ' && *topic_end != '\t')
    topic_end++;

  if(topic_end == topic_start || *topic_end == '\0')
  {
    cmd_reply(ctx, "usage: /acquire digest-test <topic> <body>");
    return;
  }

  topic_len = (size_t)(topic_end - topic_start);

  if(topic_len >= ACQUIRE_TOPIC_NAME_SZ)
  {
    cmd_reply(ctx, "digest-test: topic name too long");
    return;
  }

  body = topic_end;

  while(*body == ' ' || *body == '\t') body++;

  if(*body == '\0')
  {
    cmd_reply(ctx, "usage: /acquire digest-test <topic> <body>");
    return;
  }

  tc = mem_alloc(ACQUIRE_CTX,
      "digest_test_ctx", sizeof(*tc));

  memcpy(&tc->msg, ctx->msg, sizeof(tc->msg));
  tc->ctx.bot      = ctx->bot;
  tc->ctx.msg      = &tc->msg;
  tc->ctx.args     = NULL;
  tc->ctx.username = NULL;
  tc->ctx.parsed   = NULL;
  tc->ctx.data     = NULL;

  memcpy(tc->topic, topic_start, topic_len);
  tc->topic[topic_len] = '\0';

  if(acquire_digest_submit(tc->topic, NULL, body, strlen(body),
      acquire_digest_test_done, tc) != SUCCESS)
  {
    cmd_reply(ctx, "digest-test: failed to submit (check"
        " llm.default_chat_model)");
    mem_free(tc);
  }
}

// /show acquire  — engine-wide stats + config snapshot

static const char *
acq_yn(bool v)
{
  return(v ? "yes" : "no");
}

static size_t
acq_count_registered_bots(void)
{
  size_t n = 0;

  pthread_rwlock_rdlock(&acquire_entries_lock);

  for(acquire_bot_entry_t *e = acquire_entries; e != NULL; e = e->next)
    if(e->active)
      n++;

  pthread_rwlock_unlock(&acquire_entries_lock);
  return(n);
}

static void
acq_cmd_show_acquire(const cmd_ctx_t *ctx)
{
  acquire_stats_t s;
  acquire_cfg_t cfg;
  size_t n_bots;
  char line[256];
  acquire_get_stats(&s);

  pthread_mutex_lock(&acquire_cfg_mutex);
  cfg = acquire_cfg;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  n_bots = acq_count_registered_bots();


  snprintf(line, sizeof(line),
      "acquire: enabled=%s registered_bots=%zu"
      "  tick_cadence_secs=%u  max_reactive_per_tick=%u"
      "  sweep_interval_secs=%u",
      acq_yn(cfg.enabled), n_bots,
      cfg.tick_cadence_secs, cfg.max_reactive_per_tick,
      cfg.sweep_interval_secs);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  ticks=%llu  queries=%llu  ingested=%llu",
      (unsigned long long)s.total_ticks,
      (unsigned long long)s.total_queries,
      (unsigned long long)s.total_ingested);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  summaries_ok=%llu  summaries_fail=%llu",
      (unsigned long long)s.total_summaries_ok,
      (unsigned long long)s.total_summaries_fail);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  relevance_threshold=%u  max_sources_per_query=%u"
      "  digest_body_truncate_chars=%u",
      cfg.relevance_threshold, cfg.max_sources_per_query,
      cfg.digest_body_truncate_chars);
  cmd_reply(ctx, line);

  snprintf(line, sizeof(line),
      "  max_reactive_per_hour=%u  dedup_lru_size=%u"
      "  dedup_ttl_secs=%u",
      cfg.max_reactive_per_hour, cfg.reactive_dedup_lru_size,
      cfg.reactive_dedup_ttl_secs);
  cmd_reply(ctx, line);

  // A6R — reactive-path counters. Matches the two-space indent and
  // underscore_case used by the lines above; kept under 120 chars so
  // botmanctl doesn't wrap.
  snprintf(line, sizeof(line),
      "  reactive: enq=%llu  kick=%llu  drain=%llu"
      "  dedup_drop=%llu  rate_drop=%llu",
      (unsigned long long)s.total_reactive_enqueued,
      (unsigned long long)s.total_reactive_kicks,
      (unsigned long long)s.total_reactive_drained,
      (unsigned long long)s.total_reactive_dedup_drops,
      (unsigned long long)s.total_reactive_rate_drops);
  cmd_reply(ctx, line);

  // Personality-declared feed counters. Distinct from the SXNG-backed
  // reactive/proactive counters above: these track the dispatcher
  // owned by acquire_feed.c.
  snprintf(line, sizeof(line),
      "  feeds: fetches=%llu  304=%llu  new_items=%llu  errors=%llu"
      "  sources_enabled=%s",
      (unsigned long long)s.total_feed_fetches,
      (unsigned long long)s.total_feed_304,
      (unsigned long long)s.total_feed_new_items,
      (unsigned long long)s.total_feed_errors,
      acq_yn(cfg.sources_enabled));
  cmd_reply(ctx, line);

  // Per-bot snapshot: ring depth, kick-pending, live rate slots.
  // Single rdlock held across the walk; each bot produces one line.
  pthread_rwlock_rdlock(&acquire_entries_lock);

  for(acquire_bot_entry_t *e = acquire_entries; e != NULL; e = e->next)
  {
    uint32_t ring_depth;
    if(!e->active)
      continue;

    pthread_mutex_lock(&e->ring_mutex);
    ring_depth = e->ring_count;
    pthread_mutex_unlock(&e->ring_mutex);

    snprintf(line, sizeof(line),
        "  bot=%s  ring_depth=%u  kick_pending=%s  rate_slots=%zu",
        e->name, ring_depth, acq_yn(e->kick_pending), e->n_rate);
    cmd_reply(ctx, line);
  }

  pthread_rwlock_unlock(&acquire_entries_lock);
}

// /acquire trigger <bot> <topic>  — manual proactive fire

static size_t
acq_find_topic_index_locked(const acquire_bot_entry_t *e,
    const char *topic_name)
{
  for(size_t i = 0; i < e->n_topics; i++)
    if(strcmp(e->topics[i].name, topic_name) == 0)
      return(i);

  return(SIZE_MAX);
}

static void
acquire_cmd_trigger(const cmd_ctx_t *ctx)
{
  const char *args = ctx->args;

  const char *bot_start;
  size_t bot_len;
  const char *topic_start;
  const char *topic_end;
  size_t topic_len;
  char bot_name[ACQUIRE_BOT_NAME_SZ];
  acquire_bot_entry_t *e;
  size_t topic_idx;
  char query[ACQUIRE_TOPIC_QUERY_SZ];
  const char *bot_end;
  char topic_name[ACQUIRE_TOPIC_NAME_SZ];
  acq_proactive_result_t r;
  char reply[1024];
  if(args == NULL)
  {
    cmd_reply(ctx, "usage: /acquire trigger <bot> <topic>");
    return;
  }

  while(*args == ' ' || *args == '\t') args++;

  // Parse bot name.
  bot_start = args;
  bot_end = args;

  while(*bot_end != '\0' && *bot_end != ' ' && *bot_end != '\t')
    bot_end++;

  bot_len = (size_t)(bot_end - bot_start);

  if(bot_len == 0 || *bot_end == '\0')
  {
    cmd_reply(ctx, "usage: /acquire trigger <bot> <topic>");
    return;
  }

  if(bot_len >= ACQUIRE_BOT_NAME_SZ)
  {
    cmd_reply(ctx, "trigger: bot name too long");
    return;
  }

  topic_start = bot_end;

  while(*topic_start == ' ' || *topic_start == '\t') topic_start++;

  topic_end = topic_start;

  while(*topic_end != '\0' && *topic_end != ' ' && *topic_end != '\t')
    topic_end++;

  topic_len = (size_t)(topic_end - topic_start);

  if(topic_len == 0)
  {
    cmd_reply(ctx, "usage: /acquire trigger <bot> <topic>");
    return;
  }

  if(topic_len >= ACQUIRE_TOPIC_NAME_SZ)
  {
    cmd_reply(ctx, "trigger: topic name too long");
    return;
  }


  memcpy(bot_name, bot_start, bot_len);
  bot_name[bot_len] = '\0';
  memcpy(topic_name, topic_start, topic_len);
  topic_name[topic_len] = '\0';

  // Refuse cleanly if the subsystem isn't up — otherwise the fire
  // path would hand the ctx to a NULL sxng_search and drop it on the
  // floor with a WARN instead of a reply the operator can see.
  if(!acquire_ready)
  {
    cmd_reply(ctx, "trigger: acquisition engine is not ready");
    return;
  }

  pthread_rwlock_rdlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);

  if(e == NULL || !e->active)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    cmd_reply(ctx, "trigger: bot not registered with the acquisition engine");
    return;
  }

  topic_idx = acq_find_topic_index_locked(e, topic_name);

  if(topic_idx == SIZE_MAX)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    cmd_reply(ctx, "trigger: topic not registered for this bot");
    return;
  }

  // Fire. acq_proactive_fire_locked releases the rwlock for us on
  // every return path — success or decline.

  r = acq_proactive_fire_locked(e, topic_idx,
      query, sizeof(query));

  // Reply buffer sized to fit the worst-case OK path: a fully
  // populated ACQUIRE_TOPIC_QUERY_SZ (512 B) query plus the surrounding
  // literal text + bounded bot/topic names. 1024 clears all three
  // paths without tripping -Wformat-truncation.

  switch(r)
  {
    case ACQ_PROACTIVE_OK:
      snprintf(reply, sizeof(reply),
          "triggered proactive bot=%s topic=%s query='%s'"
          " (results land in logs + corpus)",
          bot_name, topic_name, query);
      break;

    case ACQ_PROACTIVE_RATE_LIMITED:
      snprintf(reply, sizeof(reply),
          "trigger declined: rate-limited bot=%s topic=%s"
          " (see acquire.max_reactive_per_hour)",
          bot_name, topic_name);
      break;

    case ACQ_PROACTIVE_NO_QUERY:
      snprintf(reply, sizeof(reply),
          "trigger declined: topic=%s has no proactive query text"
          " (template-only topic; proactive needs topic.query)",
          topic_name);
      break;

    case ACQ_PROACTIVE_NO_TOPIC:
    default:
      snprintf(reply, sizeof(reply),
          "trigger declined: unknown failure bot=%s topic=%s",
          bot_name, topic_name);
      break;
  }

  cmd_reply(ctx, reply);
}

// /acquire fire <bot> <topic> <subject>  — deterministic reactive fire

static void
acquire_cmd_fire(const cmd_ctx_t *ctx)
{
  const char *args = ctx->args;

  const char *bot_start;
  size_t bot_len;
  const char *topic_start;
  const char *topic_end;
  size_t topic_len;
  const char *subject_start;
  size_t subject_len;
  char bot_name  [ACQUIRE_BOT_NAME_SZ];
  acq_enq_result_t r;
  const char *bot_end;
  char topic_name[ACQUIRE_TOPIC_NAME_SZ];
  char reply[512];
  char subject   [ACQUIRE_SUBJECT_SZ];
  if(args == NULL)
  {
    cmd_reply(ctx, "usage: /acquire fire <bot> <topic> <subject>");
    return;
  }

  while(*args == ' ' || *args == '\t') args++;

  // Parse bot name.
  bot_start = args;
  bot_end = args;

  while(*bot_end != '\0' && *bot_end != ' ' && *bot_end != '\t')
    bot_end++;

  bot_len = (size_t)(bot_end - bot_start);

  if(bot_len == 0 || *bot_end == '\0')
  {
    cmd_reply(ctx, "usage: /acquire fire <bot> <topic> <subject>");
    return;
  }

  if(bot_len >= ACQUIRE_BOT_NAME_SZ)
  {
    cmd_reply(ctx, "fire: bot name too long");
    return;
  }

  // Parse topic name.
  topic_start = bot_end;

  while(*topic_start == ' ' || *topic_start == '\t') topic_start++;

  topic_end = topic_start;

  while(*topic_end != '\0' && *topic_end != ' ' && *topic_end != '\t')
    topic_end++;

  topic_len = (size_t)(topic_end - topic_start);

  if(topic_len == 0 || *topic_end == '\0')
  {
    cmd_reply(ctx, "usage: /acquire fire <bot> <topic> <subject>");
    return;
  }

  if(topic_len >= ACQUIRE_TOPIC_NAME_SZ)
  {
    cmd_reply(ctx, "fire: topic name too long");
    return;
  }

  // Subject is the whole remainder (trimmed). Multi-word names like
  // "Dua Lipa" are the common case, so we do not tokenize further.
  subject_start = topic_end;

  while(*subject_start == ' ' || *subject_start == '\t') subject_start++;

  subject_len = strlen(subject_start);

  // Trim trailing whitespace.
  while(subject_len > 0 &&
      (subject_start[subject_len - 1] == ' ' ||
       subject_start[subject_len - 1] == '\t' ||
       subject_start[subject_len - 1] == '\r' ||
       subject_start[subject_len - 1] == '\n'))
    subject_len--;

  if(subject_len == 0)
  {
    cmd_reply(ctx, "usage: /acquire fire <bot> <topic> <subject>");
    return;
  }

  if(subject_len >= ACQUIRE_SUBJECT_SZ)
  {
    cmd_reply(ctx, "fire: subject too long");
    return;
  }


  memcpy(bot_name,   bot_start,     bot_len);    bot_name[bot_len]     = '\0';
  memcpy(topic_name, topic_start,   topic_len);  topic_name[topic_len] = '\0';
  memcpy(subject,    subject_start, subject_len); subject[subject_len] = '\0';

  if(!acquire_ready)
  {
    cmd_reply(ctx, "fire: acquisition engine is not ready");
    return;
  }

  r = acquire_enqueue_reactive(bot_name, topic_name,
      subject);

  // Sized for the worst-case ACCEPTED path: bot_name (64), topic_name
  // (64), subject (128) plus surrounding literal text. 512 bytes clears
  // every branch without tripping -Wformat-truncation.

  switch(r)
  {
    case ACQ_ENQ_ACCEPTED:
      snprintf(reply, sizeof(reply),
          "accepted bot=%s topic=%s subject='%s' (drain kicked)",
          bot_name, topic_name, subject);
      break;

    case ACQ_ENQ_DEDUP:
      snprintf(reply, sizeof(reply),
          "dedup bot=%s topic=%s subject='%s'"
          " (recent duplicate — see acquire.reactive_dedup_ttl_secs)",
          bot_name, topic_name, subject);
      break;

    case ACQ_ENQ_TOPIC_UNKNOWN:
      snprintf(reply, sizeof(reply),
          "topic_not_registered: topic '%s' is not on bot '%s'",
          topic_name, bot_name);
      break;

    case ACQ_ENQ_BOT_UNKNOWN:
      snprintf(reply, sizeof(reply),
          "bot_not_registered: bot '%s' is not registered with the"
          " acquisition engine",
          bot_name);
      break;

    case ACQ_ENQ_NOT_READY:
    default:
      snprintf(reply, sizeof(reply),
          "subsystem_off: acquire subsystem not ready"
          " (check acquire.enabled)");
      break;
  }

  cmd_reply(ctx, reply);
}

// /acquire source trigger <bot> <url> — arm a personality-declared
// feed for the next tick. Mirrors the parsing shape of /acquire trigger
// but searches by URL (case-insensitive) instead of topic name; on a
// hit, zeroes the feed's `last_fetched` so the next acq_feeds_tick
// will fire it.
static void
acquire_cmd_source_trigger(const cmd_ctx_t *ctx)
{
  const char *args = ctx->args;

  const char *bot_start;
  const char *bot_end;
  size_t      bot_len;
  const char *url_start;
  size_t      url_len;
  char        bot_name[ACQUIRE_BOT_NAME_SZ];
  char        url     [ACQUIRE_FEED_URL_SZ];
  char        reply[320 + ACQUIRE_FEED_URL_SZ];

  acquire_bot_entry_t *e;
  size_t               match_idx;

  if(args == NULL)
  {
    cmd_reply(ctx, "usage: /acquire source trigger <bot> <url>");
    return;
  }

  while(*args == ' ' || *args == '\t') args++;

  bot_start = args;
  bot_end   = args;

  while(*bot_end != '\0' && *bot_end != ' ' && *bot_end != '\t')
    bot_end++;

  bot_len = (size_t)(bot_end - bot_start);

  if(bot_len == 0 || *bot_end == '\0')
  {
    cmd_reply(ctx, "usage: /acquire source trigger <bot> <url>");
    return;
  }

  if(bot_len >= ACQUIRE_BOT_NAME_SZ)
  {
    cmd_reply(ctx, "source trigger: bot name too long");
    return;
  }

  url_start = bot_end;

  while(*url_start == ' ' || *url_start == '\t') url_start++;

  url_len = strlen(url_start);

  while(url_len > 0 &&
      (url_start[url_len - 1] == ' ' ||
       url_start[url_len - 1] == '\t' ||
       url_start[url_len - 1] == '\r' ||
       url_start[url_len - 1] == '\n'))
    url_len--;

  if(url_len == 0)
  {
    cmd_reply(ctx, "usage: /acquire source trigger <bot> <url>");
    return;
  }

  if(url_len >= ACQUIRE_FEED_URL_SZ)
  {
    cmd_reply(ctx, "source trigger: url too long");
    return;
  }

  memcpy(bot_name, bot_start, bot_len);
  bot_name[bot_len] = '\0';
  memcpy(url, url_start, url_len);
  url[url_len] = '\0';

  if(!acquire_ready)
  {
    cmd_reply(ctx, "source trigger: acquisition engine is not ready");
    return;
  }

  // Phase 1 — rdlock scan for a matching URL.
  match_idx = SIZE_MAX;

  pthread_rwlock_rdlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);

  if(e == NULL || !e->active)
  {
    pthread_rwlock_unlock(&acquire_entries_lock);
    cmd_reply(ctx, "source trigger: bot not registered with the"
        " acquisition engine");
    return;
  }

  for(size_t i = 0; i < e->n_feeds_total; i++)
  {
    if(strcasecmp(e->feeds[i].url, url) == 0)
    {
      match_idx = i;
      break;
    }
  }

  pthread_rwlock_unlock(&acquire_entries_lock);

  if(match_idx == SIZE_MAX)
  {
    snprintf(reply, sizeof(reply),
        "source trigger: no feed with url='%s' on bot=%s", url, bot_name);
    cmd_reply(ctx, reply);
    return;
  }

  // Phase 2 — wrlock to arm the slot.
  pthread_rwlock_wrlock(&acquire_entries_lock);

  e = acquire_entry_find_locked(bot_name);

  if(e != NULL && e->feed_state != NULL
      && match_idx < e->n_feeds_total)
    e->feed_state[match_idx].last_fetched = 0;

  pthread_rwlock_unlock(&acquire_entries_lock);

  snprintf(reply, sizeof(reply),
      "armed bot=%s url='%s' — next tick will fire", bot_name, url);
  cmd_reply(ctx, reply);
}

// Commands — /show acquire, /acquire trigger, /acquire fire,
// /acquire source trigger, /acquire digest-test. A2 shipped only the
// /acquire root stub.

void
acquire_register_commands(void)
{
  cmd_register(ACQUIRE_CTX, ACQUIRE_CTX,
      "acquire",
      "Acquisition engine commands",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      acquire_cmd_root, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  // /show acquire — subsystem state. Shares the "acquire" name with
  // the /acquire root above; tree position (parent "show" vs parent
  // NULL) keeps them distinct at dispatch time. Mirrors the
  // /show knowledge pattern.
  cmd_register(ACQUIRE_CTX, "acquire",
      "show acquire",
      "Show acquisition engine state + lifetime counters",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      acq_cmd_show_acquire, NULL, "show", "acq", NULL, 0, NULL, NULL);

  cmd_register(ACQUIRE_CTX, "trigger",
      "acquire trigger <bot> <topic>",
      "Fire a proactive query for a registered (bot, topic) pair",
      "Bypasses the per-bot tick cadence and weight-random topic"
      " picker: the named topic runs its proactive query right now,"
      " through the same SXNG → fetch → digest → ingest chain the"
      " scheduled path uses.\n"
      "\n"
      "The hourly rate limiter (acquire.max_reactive_per_hour) still"
      " applies — spamming trigger will be declined once the cap is"
      " reached. Topics with no concrete `query` (template-only)"
      " cannot be proactively fired and will be declined with"
      " no_query.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      acquire_cmd_trigger, NULL, ACQUIRE_CTX, NULL,
      NULL, 0, NULL, NULL);

  cmd_register(ACQUIRE_CTX, "fire",
      "acquire fire <bot> <topic> <subject>",
      "Enqueue a reactive job end-to-end for (bot, topic, subject)",
      "Bypasses the chat-line keyword scanner: synthesises the exact"
      " (bot, topic, subject) tuple that a matching chat mention would"
      " produce, and kicks the per-bot drain so the query fires within"
      " ~1 s. Honours the dedup LRU and rate limiter.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      acquire_cmd_fire, NULL, ACQUIRE_CTX, NULL,
      NULL, 0, NULL, NULL);

  cmd_register(ACQUIRE_CTX, "digest-test",
      "acquire digest-test <topic> <body>",
      "Summarize + relevance-score an inline body via the configured"
      " chat model",
      "Submits the given body to the acquisition digester as if it"
      " had been fetched by the reactive/active pipeline. Prints the"
      " relevance score and summary paragraph once the LLM responds.\n"
      "\n"
      "Useful for tuning the digester's prompt or sampling the"
      " distribution of scores on known-relevant / known-irrelevant"
      " text fixtures before letting the engine run unattended.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      acquire_cmd_digest_test, NULL, ACQUIRE_CTX, NULL,
      NULL, 0, NULL, NULL);

  // /acquire source <subcommand> — grouping node for feed-related
  // admin commands. The root stub emits a short usage line.
  cmd_register(ACQUIRE_CTX, "source",
      "acquire source <subcommand>",
      "Personality-declared feed admin commands",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      acquire_cmd_source_root, NULL, ACQUIRE_CTX, NULL,
      NULL, 0, NULL, NULL);

  // /acquire source trigger <bot> <url> — arm a declared feed for the
  // next tick. Parent path "acquire source" (space-separated) puts
  // this under the source grouping node above.
  cmd_register(ACQUIRE_CTX, "trigger",
      "acquire source trigger <bot> <url>",
      "Arm a personality-declared feed to fire on the next tick",
      "Locates the feed whose URL matches (case-insensitive) on the"
      " named bot and zeroes its last-fetched timestamp. The next"
      " acq_feeds_tick will therefore select it, honouring the shared"
      " per-(bot, topic) hourly rate limiter and the normal parse /"
      " digest / ingest chain.\n"
      "\n"
      "Does not wait for the fetch to complete — returns immediately"
      " once the slot is armed. Watch the clam log for the resulting"
      " `feed fetch`, `feed RSS`, and `ingested` lines.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_PRIVATE, METHOD_T_ANY,
      acquire_cmd_source_trigger, NULL, "acquire/source", NULL,
      NULL, 0, NULL, NULL);
}

// A8 — corpus lifecycle sweep
//
// Single engine-global periodic task. On every tick it snapshots the
// registered-bot list under the read lock into a stack-allocated
// scratch array, then runs the DB work lock-free. For each unique
// destination corpus it:
//
//   1. Computes the tightest per-bot policy (minimum size cap;
//      minimum positive TTL) across every bot that names this corpus.
//   2. If ttl_days > 0, deletes rows older than NOW() - INTERVAL.
//   3. If corpus still exceeds size cap, batch-deletes oldest rows
//      (100 at a time, capped at a defensive loop bound).
//   4. Logs a single summary line per non-trivial sweep.
//
// Intentionally skips corpora that aren't bound to any registered
// bot's llm.acquired_corpus — we must never touch archwiki &c.

// Scratch snapshot of one (bot_name, dest_corpus) pair harvested from
// the registry. `dedup_owner` is true on the first occurrence of each
// dest_corpus in the scratch array; later occurrences point back to
// the owner via `owner_idx`. This keeps the per-corpus tightest-cap
// reduction a single linear pass.
typedef struct
{
  char      bot_name   [ACQUIRE_BOT_NAME_SZ];
  char      dest_corpus[ACQUIRE_CORPUS_NAME_SZ];
  bool      dedup_owner;
  size_t    owner_idx;
  uint32_t  max_mb;
  uint32_t  ttl_days;
} acq_sweep_entry_t;

// Reason enum for the post-sweep log line. Values are OR'd together:
// the final log uses "ttl", "size", "both", or "none" accordingly.
#define ACQ_SWEEP_REASON_NONE  0u
#define ACQ_SWEEP_REASON_TTL   1u
#define ACQ_SWEEP_REASON_SIZE  2u

static const char *
acq_sweep_reason_label(uint32_t mask)
{
  switch(mask)
  {
    case ACQ_SWEEP_REASON_TTL:                           return("ttl");
    case ACQ_SWEEP_REASON_SIZE:                          return("size");
    case ACQ_SWEEP_REASON_TTL | ACQ_SWEEP_REASON_SIZE:   return("both");
    default:                                             return("none");
  }
}

static void
acq_sweep_read_bot_policy(const char *bot_name,
    uint32_t *out_max_mb, uint32_t *out_ttl_days)
{
  // Make the bot_name bound visible to gcc's format-truncation
  // analysis via an explicit strnlen — otherwise the const-char*
  // parameter has no provable upper bound and the snprintf below
  // emits a false-positive -Wformat-truncation.
  char     name[ACQUIRE_BOT_NAME_SZ];
  size_t   name_len = strnlen(bot_name, sizeof(name) - 1);

  char     key[KV_KEY_SZ];
  uint32_t max_mb;
  uint32_t ttl_days;
  memcpy(name, bot_name, name_len);
  name[name_len] = '\0';

  max_mb = 0;
  ttl_days = 0;

  snprintf(key, sizeof(key),
      "bot.%s.acquired_corpus_max_mb", name);
  max_mb = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key),
      "bot.%s.acquired_corpus_ttl_days", name);
  ttl_days = (uint32_t)kv_get_uint(key);

  if(max_mb == 0)
    max_mb = ACQUIRE_DEF_CORPUS_MAX_MB;

  *out_max_mb   = max_mb;
  *out_ttl_days = ttl_days;
}

static bool
acq_sweep_corpus_bytes(const char *corpus_escaped, uint64_t *out_bytes)
{
  char sql[256];

  db_result_t *res;
  bool ok;
  snprintf(sql, sizeof(sql),
      "SELECT COALESCE(SUM(octet_length(text)), 0)"
      " FROM knowledge_chunks WHERE corpus = '%s'",
      corpus_escaped);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  *out_bytes = 0;

  if(ok && res->rows == 1)
  {
    const char *v = db_result_get(res, 0, 0);

    if(v != NULL)
      *out_bytes = strtoull(v, NULL, 10);
  }

  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

// Count the total rows in a corpus. Used once, to bound the batch
// loop to (rows/100 + 2) iterations — a defensive belt-and-braces
// upper bound against a pathological state that could otherwise spin.
static bool
acq_sweep_corpus_rowcount(const char *corpus_escaped, uint64_t *out_rows)
{
  char sql[256];

  db_result_t *res;
  bool ok;
  snprintf(sql, sizeof(sql),
      "SELECT COUNT(*) FROM knowledge_chunks WHERE corpus = '%s'",
      corpus_escaped);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  *out_rows = 0;

  if(ok && res->rows == 1)
  {
    const char *v = db_result_get(res, 0, 0);

    if(v != NULL)
      *out_rows = strtoull(v, NULL, 10);
  }

  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

// TTL-prune: delete every chunk older than now() - <days> days in a
// single server-side statement. FK cascade removes embeddings.
// ttl_days:       TTL; must be > 0 (caller gates)
static bool
acq_sweep_ttl_delete(const char *corpus_escaped, uint32_t ttl_days,
    uint32_t *out_deleted)
{
  char sql[320];

  db_result_t *res;
  bool ok;
  snprintf(sql, sizeof(sql),
      "DELETE FROM knowledge_chunks"
      " WHERE corpus = '%s'"
      " AND created < NOW() - INTERVAL '%u days'",
      corpus_escaped, ttl_days);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  *out_deleted = ok ? res->rows_affected : 0;
  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

static bool
acq_sweep_size_delete_batch(const char *corpus_escaped,
    uint32_t *out_deleted)
{
  char sql[384];

  db_result_t *res;
  bool ok;
  snprintf(sql, sizeof(sql),
      "DELETE FROM knowledge_chunks WHERE id IN ("
      "SELECT id FROM knowledge_chunks"
      " WHERE corpus = '%s'"
      " ORDER BY created ASC"
      " LIMIT %u)",
      corpus_escaped, (uint32_t)ACQUIRE_SWEEP_DELETE_BATCH);

  res = db_result_alloc();
  ok = (db_query(sql, res) == SUCCESS) && res->ok;

  *out_deleted = ok ? res->rows_affected : 0;
  db_result_free(res);
  return(ok ? SUCCESS : FAIL);
}

// Run the full lifecycle pass for one (corpus, policy) tuple: TTL
// delete → size-cap batch loop → log. Best-effort: any DB error logs
// a WARN and short-circuits the remaining steps for this corpus, but
// the outer sweep continues to the next corpus.
static void
acq_sweep_one_corpus(const acq_sweep_entry_t *e)
{
  char *esc = db_escape(e->dest_corpus);

  uint64_t bytes_before;
  uint32_t mb_before;
  uint64_t max_bytes;
  uint32_t mb_after;
  uint32_t reason_mask;
  uint64_t bytes_now;
  uint32_t total_deleted;
  if(esc == NULL)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "sweep: escape failed for corpus='%s' — skipping",
        e->dest_corpus);
    return;
  }

  bytes_before = 0;

  if(acq_sweep_corpus_bytes(esc, &bytes_before) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "sweep: size-probe failed for corpus='%s' — skipping",
        e->dest_corpus);
    mem_free(esc);
    return;
  }

  // Cheap bail-out: empty corpus + no TTL = nothing to do.
  if(bytes_before == 0 && e->ttl_days == 0)
  {
    mem_free(esc);
    return;
  }

  mb_before = (uint32_t)(bytes_before / (1024ULL * 1024ULL));
  reason_mask = ACQ_SWEEP_REASON_NONE;
  total_deleted = 0;

  // Phase 1 — TTL delete, if configured.
  if(e->ttl_days > 0)
  {
    uint32_t n = 0;

    if(acq_sweep_ttl_delete(esc, e->ttl_days, &n) != SUCCESS)
    {
      clam(CLAM_WARN, ACQUIRE_CTX,
          "sweep: ttl delete failed for corpus='%s' ttl_days=%u",
          e->dest_corpus, e->ttl_days);
      mem_free(esc);
      return;
    }

    if(n > 0)
    {
      total_deleted += n;
      reason_mask   |= ACQ_SWEEP_REASON_TTL;
    }
  }

  // Phase 2 — size-cap batch loop. Re-sum after each batch so we
  // never delete more than necessary. Upper iteration bound is the
  // worst case (every remaining row goes) plus a small slack; it is
  // a defensive guard against any pathological DB state that keeps
  // rows_affected at zero while bytes stay high.
  max_bytes = (uint64_t)e->max_mb * 1024ULL * 1024ULL;
  bytes_now = 0;

  if(acq_sweep_corpus_bytes(esc, &bytes_now) != SUCCESS)
  {
    clam(CLAM_WARN, ACQUIRE_CTX,
        "sweep: size-probe (post-ttl) failed for corpus='%s'",
        e->dest_corpus);
    mem_free(esc);
    return;
  }

  if(bytes_now > max_bytes)
  {
    uint64_t rows_total = 0;

    uint64_t max_iters;
    if(acq_sweep_corpus_rowcount(esc, &rows_total) != SUCCESS)
    {
      clam(CLAM_WARN, ACQUIRE_CTX,
          "sweep: row-count failed for corpus='%s'", e->dest_corpus);
      mem_free(esc);
      return;
    }

    max_iters = (rows_total / ACQUIRE_SWEEP_DELETE_BATCH) + 2;

    for(uint64_t i = 0; i < max_iters && bytes_now > max_bytes; i++)
    {
      uint32_t n = 0;

      if(acq_sweep_size_delete_batch(esc, &n) != SUCCESS)
      {
        clam(CLAM_WARN, ACQUIRE_CTX,
            "sweep: size delete batch failed corpus='%s' iter=%" PRIu64,
            e->dest_corpus, i);
        break;
      }

      if(n == 0)
        break;  // nothing left matching — defensive; loop bound also catches

      total_deleted += n;
      reason_mask   |= ACQ_SWEEP_REASON_SIZE;

      if(acq_sweep_corpus_bytes(esc, &bytes_now) != SUCCESS)
      {
        clam(CLAM_WARN, ACQUIRE_CTX,
            "sweep: size-probe (post-batch) failed corpus='%s'",
            e->dest_corpus);
        break;
      }
    }
  }

  mem_free(esc);

  mb_after = (uint32_t)(bytes_now / (1024ULL * 1024ULL));

  if(total_deleted == 0)
  {
    clam(CLAM_DEBUG, ACQUIRE_CTX,
        "sweep corpus=%s size_mb=%u no-op (within cap=%u, ttl_days=%u)",
        e->dest_corpus, mb_before, e->max_mb, e->ttl_days);
    return;
  }

  clam(CLAM_INFO, ACQUIRE_CTX,
      "sweep corpus=%s size_mb_before=%u size_mb_after=%u"
      " chunks_deleted=%u reason=%s",
      e->dest_corpus, mb_before, mb_after, total_deleted,
      acq_sweep_reason_label(reason_mask));
}

// Sweep callback (engine-global, runs on the periodic task thread).
// Mirrors acquire_bot_tick's gating pattern: if the subsystem is not
// ready or the engine is disabled the tick is a cheap no-op.
void
acquire_sweep_tick(task_t *t)
{
  bool enabled;
  enum { SCRATCH_CAP = 64 };
  acq_sweep_entry_t scratch[SCRATCH_CAP];
  size_t            n_scratch;
  if(!acquire_ready)
  {
    t->state = TASK_ENDED;
    return;
  }

  pthread_mutex_lock(&acquire_cfg_mutex);
  enabled = acquire_cfg.enabled;
  pthread_mutex_unlock(&acquire_cfg_mutex);

  if(!enabled)
  {
    t->state = TASK_ENDED;
    return;
  }

  // Snapshot the registry under the read lock. Capacity is bounded
  // by the registry length; real-world deployments register a
  // handful of bots. A fixed upper bound avoids heap churn on the
  // hot path and keeps the critical section tight.

  n_scratch = 0;

  pthread_rwlock_rdlock(&acquire_entries_lock);

  for(acquire_bot_entry_t *e = acquire_entries;
      e != NULL && n_scratch < SCRATCH_CAP;
      e = e->next)
  {
    acq_sweep_entry_t *s;
    if(!e->active)
      continue;

    if(e->dest_corpus[0] == '\0')
      continue;

    s = &scratch[n_scratch++];

    snprintf(s->bot_name,    sizeof(s->bot_name),    "%s", e->name);
    snprintf(s->dest_corpus, sizeof(s->dest_corpus), "%s", e->dest_corpus);
    s->dedup_owner = true;
    s->owner_idx   = n_scratch - 1;
    s->max_mb      = 0;  // populated below, outside the lock
    s->ttl_days    = 0;
  }

  pthread_rwlock_unlock(&acquire_entries_lock);

  if(n_scratch == 0)
  {
    t->state = TASK_ENDED;
    return;
  }

  // Pull per-bot policies lock-free from KV.
  for(size_t i = 0; i < n_scratch; i++)
    acq_sweep_read_bot_policy(scratch[i].bot_name,
        &scratch[i].max_mb, &scratch[i].ttl_days);

  // Dedup pass: mark followers and reduce to tightest policy per
  // unique dest_corpus. O(n^2) but n is tiny (<= SCRATCH_CAP).
  for(size_t i = 1; i < n_scratch; i++)
  {
    for(size_t j = 0; j < i; j++)
    {
      acq_sweep_entry_t *owner;
      if(!scratch[j].dedup_owner)
        continue;

      if(strcmp(scratch[i].dest_corpus, scratch[j].dest_corpus) != 0)
        continue;

      scratch[i].dedup_owner = false;
      scratch[i].owner_idx   = j;

      // Tightest policy wins:
      //   max_mb:   minimum across owners (smaller is stricter)
      //   ttl_days: minimum positive (0 = no-TTL; any positive wins,
      //             and if two owners both set it, the smaller wins)
      owner = &scratch[j];

      if(scratch[i].max_mb < owner->max_mb)
        owner->max_mb = scratch[i].max_mb;

      if(scratch[i].ttl_days > 0
          && (owner->ttl_days == 0 || scratch[i].ttl_days < owner->ttl_days))
        owner->ttl_days = scratch[i].ttl_days;
      break;
    }
  }

  // Run the pipeline once per unique corpus.
  for(size_t i = 0; i < n_scratch; i++)
  {
    if(!scratch[i].dedup_owner)
      continue;

    if(!acquire_ready)
      break;  // shutdown raced us — drop the rest of the sweep

    acq_sweep_one_corpus(&scratch[i]);
  }

  t->state = TASK_ENDED;
}
