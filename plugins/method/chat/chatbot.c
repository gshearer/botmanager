// botmanager — MIT
// Mimick bot driver + plugin descriptor.

#define CHATBOT_INTERNAL
#include "chatbot.h"
#include "vision.h"

#include "clam.h"
#include "inference.h"
#include "plugin.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static void chatbot_personality_kv_cb(const char *key, void *data);

// --- NL-KV responders (NB2) --------------------------------------
// Declarative responders attached via the 6th field on chatbot_inst_schema
// entries. Each gets the bot a "/kv <suffix>" surface that the NL bridge
// walks into kv_get_val_str + "$value" templating. Starter set stays
// tiny and strictly additive: no existing KV loses a change callback,
// no command registration is added.

static const nl_example_t chat_model_examples[] = {
  { .utterance = "what LLM are you?",           .invocation = "/kv chat_model" },
  { .utterance = "which model powers you?",     .invocation = "/kv chat_model" },
  { .utterance = "what model are you running?", .invocation = "/kv chat_model" },
};

static const kv_nl_t chat_model_nl = {
  .when              =
    "User asks which LLM, model, or AI engine is running you.",
  .examples          = chat_model_examples,
  .example_count     =
    (uint8_t)(sizeof(chat_model_examples) / sizeof(chat_model_examples[0])),
  .response_template = "I'm running on $value.",
};

static const nl_example_t personality_examples[] = {
  { .utterance = "what personality are you?",      .invocation = "/kv behavior.personality" },
  { .utterance = "which persona are you running?", .invocation = "/kv behavior.personality" },
  { .utterance = "what's your current persona?",   .invocation = "/kv behavior.personality" },
};

static const kv_nl_t behavior_personality_nl = {
  .when              =
    "User asks which personality, persona, or character the bot is using.",
  .examples          = personality_examples,
  .example_count     =
    (uint8_t)(sizeof(personality_examples) / sizeof(personality_examples[0])),
  .response_template = "My current personality is $value.",
};

static const nl_example_t mute_until_examples[] = {
  { .utterance = "are you muted?",                .invocation = "/kv behavior.mute_until" },
  { .utterance = "how long are you silenced for?", .invocation = "/kv behavior.mute_until" },
};

static const kv_nl_t behavior_mute_until_nl = {
  .when              =
    "User asks whether the bot is muted / hushed and for how long.",
  .examples          = mute_until_examples,
  .example_count     =
    (uint8_t)(sizeof(mute_until_examples) / sizeof(mute_until_examples[0])),
  // Epoch-seconds serialization is blunt, but it's honest — the mute
  // deadline is a uint. Human-friendly time-delta rendering is the
  // deferred format_hook work noted in the plan.
  .response_template = "My mute-until epoch is $value (0 = not muted).",
};

// Plugin KV schema. Per-instance active personality lives in
// "bot.<name>.behavior.personality" (registered via kv_inst_schema so
// each bound bot gets its own slot). All behavior-shaping knobs --
// including the persona binding itself -- are grouped under
// "bot.<name>.behavior.*"; model / knowledge bindings remain flat.
static const plugin_kv_entry_t chatbot_kv_schema[] = {
  { "plugin.chat.default_personality", KV_STR, "",
    "Default personality name for new chat bot instances", NULL, NULL },
  { "plugin.chat.default_contract", KV_STR, "default",
    "Default output contract stem for chat bot instances"
    " (overridden per-bot by bot.<name>.behavior.contract). Resolves"
    " against bot.chat.contractpath.", NULL, NULL },
};

static const plugin_kv_entry_t chatbot_inst_schema[] = {
  // --- model / knowledge bindings (bot.<name>.*) -----------------------
  { "chat_model", KV_STR, "",
    "LLM chat model name (empty = use llm.default_chat_model)",
    NULL, &chat_model_nl },
  { "speak_temperature", KV_UINT32, "70",
    "Sampling temperature, stored as int * 100 (e.g. 70 = 0.70)", NULL },
  { "max_reply_tokens", KV_UINT32, "256",
    "Upper bound on reply length in tokens", NULL },
  { "corpus", KV_STR, "",
    "Knowledge corpora to retrieve from, semicolon-separated (e.g."
    " 'archwiki;linuxfoundation'). Each name must exist in"
    " knowledge_corpora; unknown names in the list are silently ignored."
    " Empty = no knowledge retrieval for this bot.", NULL },
  { "acquired_corpus", KV_STR, "",
    "Knowledge corpus the acquisition engine writes digested content"
    " into. Upserted at bot start if missing. Empty = no acquisition"
    " even when the personality declares interests.", NULL },
  { "acquired_corpus_max_mb", KV_UINT32, "200",
    "Soft size cap (megabytes) for this bot's acquired corpus. The"
    " engine's periodic sweep oldest-first prunes chunks (batches of"
    " 100) until the corpus is under the cap. When two bots share a"
    " corpus, the smaller cap wins.", NULL },
  { "acquired_corpus_ttl_days", KV_UINT32, "0",
    "Age cutoff (days) for this bot's acquired-corpus chunks. 0"
    " disables TTL pruning; any positive value causes the sweep to"
    " delete rows older than NOW() - INTERVAL before the size-cap"
    " pass runs. When two bots share a corpus, the smaller positive"
    " TTL wins.", NULL },

  // --- behavior: runtime conduct (bot.<name>.behavior.*) ---------------
  { "behavior.personality", KV_STR, "",
    "Active personality name for this bot instance. Resolves against"
    " bot.chat.personalitypath/<name>.txt. Empty falls back to"
    " plugin.chat.default_personality.",
    chatbot_personality_kv_cb, &behavior_personality_nl },
  { "behavior.contract", KV_STR, "",
    "Output contract filename stem (no .txt) for this bot instance."
    " Resolved against bot.chat.contractpath. Empty falls back to"
    " plugin.chat.default_contract.", NULL },
  { "behavior.witness_log", KV_BOOL, "true",
    "Log WITNESS messages (bot not addressed) to conversation_log", NULL },
  { "behavior.max_inflight", KV_UINT32, "2",
    "Concurrent LLM reply cap for this bot", NULL },
  { "behavior.mute_until", KV_UINT32, "0",
    "Epoch seconds until which this bot is muted (0 = not muted)."
    " Set via /hush, cleared by /unmute or when the deadline passes.",
    NULL, &behavior_mute_until_nl },
  { "behavior.coalesce_ms", KV_UINT32, "1500",
    "Paste coalescing window in milliseconds. Consecutive lines from the"
    " same sender are buffered and treated as a single message once the"
    " sender goes idle for this long. 0 disables coalescing (per-line"
    " speak decisions).", NULL },
  { "behavior.anonymous_dossiers", KV_BOOL, "false",
    "When true, the bot creates dossiers for unregistered senders so"
    " their conversations and facts accumulate. When false, only senders"
    " whose MFA pattern matches a registered user get a dossier; anyone"
    " else is logged with NULL dossier_id and no per-sender memory.", NULL },
  { "behavior.nl_bridge_cmds", KV_STR, "",
    "NL bridge allowlist. Empty = disabled. '*' = every NL-capable"
    " command permitted by caller perms. Otherwise comma list"
    " (e.g. 'weather,hush,dice').", NULL },

  // --- behavior.speak.* — speak-policy knobs ---------------------------
  { "behavior.speak.interject_prob", KV_UINT32, "0",
    "Probability (0–100) of interjecting on a topical WITNESS message"
    " (relevance match on persona interests). Multiplied by the"
    " relevance boost and capped at 75 absolute.", NULL },
  { "behavior.speak.witness_base_prob", KV_UINT32, "3",
    "Probability (0–100) of interjecting on an UNTOPICAL WITNESS"
    " message — a line with zero relevance match against the persona's"
    " interest keywords. Decoupled from interject_prob so topical fire"
    " can stay high (e.g. 15) while small-talk interjects stay rare."
    " 0 disables untopical interjects entirely. Harm-pattern matches"
    " still bypass this gate.", NULL },
  { "behavior.speak.reply_cooldown_secs", KV_UINT32, "30",
    "Seconds between spoken replies on the same target", NULL },
  { "behavior.speak.witness_interject_cooldown_secs", KV_UINT32, "600",
    "VF-3: seconds between WITNESS-driven interjects on the same target"
    " (channel for channel traffic, sender for DMs). Caps interject"
    " density regardless of the probability roll. Direct-address"
    " replies do not consume this budget. 0 disables the gate.", NULL },
  { "behavior.speak.engagement_window_secs", KV_UINT32, "90",
    "Seconds after an EXCHANGE_IN exchange that follow-up messages from"
    " the same user in the same channel are promoted to EXCHANGE_IN"
    " without re-addressing the bot. 0 disables sticky behaviour.", NULL },
  { "behavior.speak.handoff_window_secs", KV_UINT32, "45",
    "CV-12: seconds within which a line from user X is demoted from"
    " sticky-EXCHANGE_IN to WITNESS if the channel's previous non-bot"
    " speaker was a different user Y — treats the line as a reply to Y"
    " rather than a continuation of the bot-exchange. 0 disables the"
    " handoff gate.", NULL },
  { "behavior.speak.engagement_require_reply", KV_BOOL, "true",
    "When true (default), engagement is stamped only after the bot"
    " actually replies. When false, any inbound EXCHANGE_IN stamps the"
    " slot regardless of whether the speak policy gated a reply.", NULL },

  // --- behavior.mention.* — mention-expansion knobs --------------------
  { "behavior.mention.top_k", KV_UINT32, "6",
    "Facts per mentioned dossier to inject into EXCHANGE_IN replies."
    " Capped at CHATBOT_MENTION_FACTS_CAP (10).", NULL },
  { "behavior.mention.max_dossiers", KV_UINT32, "4",
    "Cap on how many dossiers mentioned in the incoming message get"
    " expanded into the prompt. Extras (ranked by the mention scorer)"
    " are dropped.", NULL },
  { "behavior.mention.max_chars", KV_UINT32, "2048",
    "Byte budget for the ABOUT PEOPLE MENTIONED prompt block. Trimmed"
    " from the tail once exceeded.", NULL },

  // --- behavior.fact_extract.* — LLM fact extraction knobs -------------
  { "behavior.fact_extract.enabled", KV_BOOL, "false",
    "Enable LLM-driven fact extraction from recent conversation_log"
    " rows. Pipeline wiring lands in follow-up chunks (F-2/F-3).", NULL },
  { "behavior.fact_extract.interval_secs", KV_UINT32, "300",
    "Seconds between extraction sweeps when fact extraction is enabled.", NULL },
  { "behavior.fact_extract.max_per_hour", KV_UINT32, "20",
    "Maximum extraction sweeps per rolling hour (rate-limit hint).", NULL },
  { "behavior.fact_extract.min_conf", KV_UINT32, "50",
    "Minimum confidence (0-100, interpreted as 0.00-1.00) below which"
    " LLM-produced facts are rejected.", NULL },
  { "behavior.fact_extract.batch_cap", KV_UINT32, "20",
    "Maximum conversation_log rows pulled per extraction sweep.", NULL },
  { "behavior.fact_extract.hwm", KV_UINT64, "0",
    "High-water mark: largest conversation_log.id already processed"
    " by the extractor. Advanced on each successful sweep.", NULL },
  { "behavior.fact_extract.aliases_enabled", KV_BOOL, "false",
    "Enable LLM-proposed dossier aliases during the fact-extraction"
    " sweep. Accepted aliases are stored as synthetic IRC signature"
    " rows (with empty ident/cloak) so the mention-resolution"
    " token scorer picks them up. Off by default until proven.", NULL },
  { "behavior.fact_extract.aliases_min_conf", KV_UINT32, "70",
    "Minimum confidence (0-100, interpreted as 0.00-1.00) below"
    " which LLM-proposed aliases are rejected. Stricter than"
    " fact min_conf because a bad alias cross-contaminates future"
    " mention resolution.", NULL },
  { "behavior.fact_extract.aliases_per_sweep_max", KV_UINT32, "3",
    "Maximum aliases accepted per extraction sweep.", NULL },

  // --- behavior.volunteer.* — volunteer-speech knobs (V1+V2) -----------
  // Master switch defaults OFF; enabling introduces unsolicited channel
  // speech and should be a conscious operator choice. Every other knob
  // has a conservative default so flipping the switch alone produces
  // well-behaved volunteering.
  { "behavior.volunteer.enabled", KV_BOOL, "false",
    "Master switch: when true, the bot may spontaneously post a"
    " one-liner about freshly-acquired content without being"
    " addressed. All volunteer gates below apply only when this"
    " is true.", NULL },
  { "behavior.volunteer.prob", KV_UINT32, "20",
    "Probability (0-100) that a qualifying ingest enters the full"
    " gate cascade. Rolled once per ingest before any channel"
    " iteration.", NULL },
  { "behavior.volunteer.relevance_floor", KV_UINT32, "80",
    "Minimum acquire-relevance to consider for volunteering."
    " Strictly above acquire.relevance_threshold so mediocre hits"
    " never drive unsolicited speech.", NULL },
  { "behavior.volunteer.max_per_hour", KV_UINT32, "3",
    "Cap on successful volunteer posts per rolling hour, summed"
    " across all channels the bot is joined to.", NULL },
  { "behavior.volunteer.channel_cooldown_secs", KV_UINT32, "1800",
    "Minimum seconds between successful volunteer posts in the"
    " same channel.", NULL },
  { "behavior.volunteer.max_quiet_secs", KV_UINT32, "3600",
    "Skip channels whose last witnessed activity is older than"
    " this many seconds (don't volunteer into a dead channel).", NULL },
  { "behavior.volunteer.min_quiet_secs", KV_UINT32, "120",
    "Skip channels where anyone spoke within this many seconds"
    " (don't barge into a live conversation).", NULL },
  { "behavior.volunteer.min_since_own_secs", KV_UINT32, "900",
    "Skip channels where the bot's own last reply is newer than"
    " this. Prevents the bot from talking to itself too often.", NULL },

  // V2 — content-level dedup knobs. Subject cooldown is cheap
  // (case-insensitive exact-match on acquire's resolved subject);
  // dedup threshold drives the semantic gate (cosine against the
  // last CHATBOT_VOLUNTEER_EMBED_RING posted chunks).
  { "behavior.volunteer.subject_cooldown_secs", KV_UINT32, "21600",
    "Seconds before volunteering about the same subject again."
    " Case-insensitive exact-match on the acquire-resolved subject"
    " string. 0 disables the gate.", NULL },
  { "behavior.volunteer.dedup_threshold", KV_UINT32, "85",
    "Cosine-similarity threshold (0-100) above which a candidate"
    " chunk is treated as a duplicate of a recently-volunteered"
    " chunk. 0 disables the semantic gate; 100 requires exact"
    " vector identity.", NULL },

  // --- behavior.image_vision.* (IV3) -----------------------------------
  { "behavior.image_vision.enabled", KV_BOOL, "false",
    "Master switch for image-vision. When true, the bot detects image"
    " URLs in messages, fetches them, and describes them via a"
    " vision-capable LLM while adhering to the personality.", NULL },
  { "behavior.image_vision.model", KV_STR, "",
    "Vision-capable LLM model name for this bot. Empty falls back to"
    " bot.<name>.chat_model (safe when chat_model is itself multimodal).",
    NULL },
  { "behavior.image_vision.allow_dm", KV_BOOL, "false",
    "When true, DMs with image URLs also trigger vision replies. Default"
    " false: the feature is public-channel-only.", NULL },
  { "behavior.image_vision.cooldown_secs", KV_UINT32, "60",
    "Minimum seconds between vision replies in the same channel.", NULL },
  { "behavior.image_vision.url_cooldown_secs", KV_UINT32, "600",
    "Minimum seconds before the same image URL re-triggers in the same"
    " channel.", NULL },
  { "behavior.image_vision.max_bytes", KV_UINT32, "8000000",
    "Reject image bodies larger than this. Bounded above by"
    " core.curl.max_response_sz; operator must bump that too for caps"
    " past 10 MiB.", NULL },
  { "behavior.image_vision.max_inflight", KV_UINT32, "1",
    "Concurrent vision fetches per bot.", NULL },
  { "behavior.image_vision.skip_knowledge", KV_BOOL, "true",
    "Skip knowledge-corpus RAG on the vision path. Memory (facts +"
    " conversation) is still retrieved for persona coherence.", NULL },
};

// KV change callback for bot.<botname>.behavior.personality. The
// cmd_set_kv path writes the new value unconditionally; this hook
// propagates the change into the running chatbot state so reply.c
// picks it up on the next message without requiring a bot restart.
static void
chatbot_personality_kv_cb(const char *key, void *data)
{
  const char *val;
  chatbot_state_t *st;
  bot_inst_t *bot;
  size_t name_len;
  char   botname[BOT_NAME_SZ];
  const char *bot_prefix = "bot.";
  const char *suffix = ".behavior.personality";
  size_t prefix_len;
  size_t suffix_len;
  size_t key_len;

  (void)data;

  prefix_len = strlen(bot_prefix);
  suffix_len = strlen(suffix);
  key_len = strlen(key);

  if(key_len <= prefix_len + suffix_len) return;
  if(strncmp(key, bot_prefix, prefix_len) != 0) return;
  if(strcmp(key + key_len - suffix_len, suffix) != 0) return;

  name_len = key_len - prefix_len - suffix_len;
  if(name_len >= sizeof(botname)) return;
  memcpy(botname, key + prefix_len, name_len);
  botname[name_len] = '\0';

  bot = bot_find(botname);
  if(bot == NULL) return;
  if(strcmp(bot_driver_name(bot), "chat") != 0) return;

  st = bot_get_handle(bot);
  if(st == NULL) return;

  val = kv_get_str(key);
  if(val == NULL) val = "";

  pthread_rwlock_wrlock(&st->lock);
  if(strcmp(st->active_name, val) != 0)
  {
    snprintf(st->active_name, sizeof(st->active_name), "%s", val);
    st->registered_persona[0] = '\0';
  }
  pthread_rwlock_unlock(&st->lock);
}

// ---------- harm-pattern fast-path ----------

// Forward decl so the harm helper can use the case-insensitive
// substring search that lives further down in this file (defined for
// the reactive-topic scan). Declaring here keeps both consumers happy
// without reordering the whole file.
static const char *chatbot_ci_strstr(const char *haystack, const char *needle);

// Returns true when `text` contains a destructive shell pattern that
// warrants a harm warning regardless of the WITNESS interject roll.
//
// The patterns are deliberately tight: each requires either an
// unambiguous literal substring (`chmod -R 777`, classic forkbomb) or
// a co-occurring secondary token (curl + `| sh`, dd + `of=/dev/sd`).
// `rm -rf /` and `rm -rf ~` only match when the slash/tilde is
// followed by end-of-string or a shell separator (space, `;`, `&`,
// newline) — that excludes benign forms like `rm -rf /tmp`.
//
// A false positive means the bot speaks up where it didn't strictly
// need to; the cost is one extra in-character reply, not a wrong
// action. A false negative means we silently miss a harm warning,
// which is the failure mode this gate exists to prevent — so when in
// doubt we err toward speaking.
static bool
chatbot_text_has_harm_pattern(const char *text)
{
  bool has_curl;
  bool has_wget;
  static const char rmslash[] = "rm -rf /";
  static const char rmtilde[] = "rm -rf ~";
  const char *p;

  if(text == NULL || text[0] == '\0')
    return(false);

  // World-writable chmod with the recursive flag, or bare chmod 777
  // followed by a path. The trailing space on "chmod 777 " rules out
  // accidental matches inside larger numbers (e.g. "7770").
  if(chatbot_ci_strstr(text, "chmod -R 777")  != NULL) return(true);
  if(chatbot_ci_strstr(text, "chmod -R 0777") != NULL) return(true);
  if(chatbot_ci_strstr(text, "chmod 777 ")    != NULL) return(true);
  if(chatbot_ci_strstr(text, "chmod 0777 ")   != NULL) return(true);

  // rm -rf rooted at / or ~. Boundary check on the byte after the
  // root char: end-of-string or a shell-statement separator.

  if((p = chatbot_ci_strstr(text, rmslash)) != NULL)
  {
    char c = p[sizeof(rmslash) - 1];
    if(c == '\0' || c == ' ' || c == '\t'
        || c == ';' || c == '&' || c == '\n' || c == '\r')
      return(true);
  }

  if((p = chatbot_ci_strstr(text, rmtilde)) != NULL)
  {
    char c = p[sizeof(rmtilde) - 1];
    if(c == '\0' || c == ' ' || c == '\t'
        || c == ';' || c == '&' || c == '\n' || c == '\r')
      return(true);
  }

  // Pipe-to-shell from a network fetcher.
  has_curl = (chatbot_ci_strstr(text, "curl ") != NULL);
  has_wget = (chatbot_ci_strstr(text, "wget ") != NULL);
  if(has_curl || has_wget)
  {
    if(chatbot_ci_strstr(text, "| sh")   != NULL) return(true);
    if(chatbot_ci_strstr(text, "| bash") != NULL) return(true);
    if(chatbot_ci_strstr(text, "|sh ")   != NULL) return(true);
    if(chatbot_ci_strstr(text, "|bash ") != NULL) return(true);
  }

  // dd writing to a raw block device.
  if(chatbot_ci_strstr(text, "dd if=") != NULL)
  {
    if(chatbot_ci_strstr(text, "of=/dev/sd")    != NULL) return(true);
    if(chatbot_ci_strstr(text, "of=/dev/nvme")  != NULL) return(true);
    if(chatbot_ci_strstr(text, "of=/dev/hd")    != NULL) return(true);
    if(chatbot_ci_strstr(text, "of=/dev/disk")  != NULL) return(true);
  }

  // mkfs against a /dev path (filesystem create on a raw device).
  if(chatbot_ci_strstr(text, "mkfs.") != NULL
      && chatbot_ci_strstr(text, "/dev/") != NULL)
    return(true);

  // Classic forkbomb. Verbatim because the structure is the signature.
  if(chatbot_ci_strstr(text, ":(){ :|:& };:") != NULL) return(true);
  if(chatbot_ci_strstr(text, ":(){:|:&};:")   != NULL) return(true);

  return(false);
}

// Count the number of distinct persona-interest topics that have at
// least one keyword matching `text`. Used as the relevance signal for
// the WITNESS interject probability boost: more matches mean more
// reason to think the bot has something useful to add. Pure-read
// against the topic cache; safe on the hot path.
static size_t
chatbot_relevance_score(chatbot_state_t *st, const char *text)
{
  size_t n_topics;
  size_t hits;

  if(st == NULL || text == NULL || text[0] == '\0')
    return(0);

  hits = 0;

  pthread_rwlock_rdlock(&st->lock);

  n_topics = st->n_topics;

  for(size_t ti = 0; ti < n_topics; ti++)
  {
    const acquire_topic_t *t = &st->topics[ti];

    if(t->n_keywords == 0)
      continue;

    for(size_t ki = 0; ki < t->n_keywords; ki++)
    {
      const char *kw = t->keywords[ki];

      if(kw[0] != '\0' && chatbot_ci_strstr(text, kw) != NULL)
      {
        hits++;
        break;
      }
    }
  }

  pthread_rwlock_unlock(&st->lock);

  return(hits);
}

// ---------- message-kind classification ----------

// Case-insensitive prefix check; returns bytes consumed (incl. an
// optional single trailing punctuation char in {':',',',' ',';','-'})
// or 0 if no match.
static size_t
text_starts_with_nick(const char *text, const char *nick)
{
  char c;
  size_t n;

  if(text == NULL || nick == NULL || nick[0] == '\0')
    return(0);

  n = strlen(nick);
  if(strncasecmp(text, nick, n) != 0) return(0);

  c = text[n];
  if(c == '\0') return(n);
  if(c == ':' || c == ',' || c == ';' || c == '-' || isspace((unsigned char)c))
    return(n + 1);
  return(0);
}

// Word-boundary nick scan: returns true if `nick` appears anywhere in
// `text` flanked by non-identifier characters (or string boundaries).
// Used so that mid-sentence mentions ("hey dale, what's up") and CTCP
// ACTION lines ("looks at dale") are recognized as addressing the bot,
// not just leading "nick:" / "@nick" prefixes.
static bool
text_contains_nick(const char *text, const char *nick)
{
  size_t n;

  if(text == NULL || nick == NULL || nick[0] == '\0') return(false);

  n = strlen(nick);
  for(const char *p = text; *p != '\0'; p++)
  {
    char prev;
    char next;
    bool prev_boundary;
    bool next_boundary;

    if(strncasecmp(p, nick, n) != 0) continue;

    prev = (p == text) ? '\0' : p[-1];
    next = p[n];

    prev_boundary = (prev == '\0') ||
        (!isalnum((unsigned char)prev) && prev != '_');
    next_boundary = (next == '\0') ||
        (!isalnum((unsigned char)next) && next != '_');

    if(prev_boundary && next_boundary) return(true);
  }
  return(false);
}

mem_msg_kind_t
chatbot_classify_message(const method_msg_t *msg, const char *bot_nick)
{
  const char *t;

  if(msg == NULL) return(MEM_MSG_WITNESS);

  // DM (no channel) is always EXCHANGE_IN.
  if(msg->channel[0] == '\0')
    return(MEM_MSG_EXCHANGE_IN);

  // "@nick ..." or "nick: ..." style mention at the start of the line.
  t = msg->text;
  while(*t == ' ' || *t == '\t') t++;

  if(*t == '@' && text_starts_with_nick(t + 1, bot_nick) > 0)
    return(MEM_MSG_EXCHANGE_IN);

  if(text_starts_with_nick(t, bot_nick) > 0)
    return(MEM_MSG_EXCHANGE_IN);

  // Mid-sentence mention or CTCP ACTION ("/me looks at <nick>"): scan
  // the full body for a word-bounded occurrence of the bot's nick.
  if(text_contains_nick(msg->text, bot_nick))
    return(MEM_MSG_EXCHANGE_IN);

  return(MEM_MSG_WITNESS);
}

// ---------- sticky engagement ring ----------

// Classification-reason tag: surfaced to the per-message debug log so an
// operator attached via botmanctl -A can see why a line was promoted (or
// not) — direct nick address vs. the sticky-engagement window vs. a
// handoff that cleared the sender's slot.
typedef enum
{
  CHATBOT_CLASSIFY_DIRECT,
  CHATBOT_CLASSIFY_STICKY,
  CHATBOT_CLASSIFY_HANDOFF,
  CHATBOT_CLASSIFY_WITNESS
} chatbot_classify_reason_t;

// Short label for the reason tag. Bounded static strings; callers
// embed the result in a clam format without further copying.
static const char *
chatbot_classify_reason_tag(chatbot_classify_reason_t r)
{
  switch(r)
  {
    case CHATBOT_CLASSIFY_DIRECT:   return("direct");
    case CHATBOT_CLASSIFY_STICKY:   return("sticky");
    case CHATBOT_CLASSIFY_HANDOFF:  return("handoff");
    case CHATBOT_CLASSIFY_WITNESS:  return("witness");
  }
  return("?");
}

// Locate a slot matching (channel, user), or the LRU (smallest
// last_seen, including cleared slots at 0) for overwrite. The caller
// holds e->mutex. Returns a pointer into e->slots — never NULL, since
// the ring is fixed-size and always evictable.
//
// Matching is exact on channel (channel names are the protocol's
// authoritative identifier) and case-insensitive on user (IRC nicks
// fold case per RFC 2812; `text_starts_with_nick` above uses the same
// rule so the two paths stay consistent).
static chatbot_engagement_slot_t *
chatbot_engagement_find_or_lru(chatbot_engagement_t *e,
    const char *channel, const char *user)
{
  chatbot_engagement_slot_t *lru = &e->slots[0];

  for(size_t i = 0; i < CHATBOT_ENGAGEMENT_SLOTS; i++)
  {
    chatbot_engagement_slot_t *s = &e->slots[i];

    if(s->channel[0] != '\0'
        && strcmp(s->channel, channel) == 0
        && strcasecmp(s->user, user) == 0)
      return(s);

    if(s->last_seen < lru->last_seen)
      lru = s;
  }

  return(lru);
}

// Stamp the (channel, user) slot with `now`. Either refreshes the
// existing slot or evicts the LRU and writes a fresh one. Ignores
// empty-channel or empty-user inputs — DMs and header-only events
// should not engage the ring.
static void
chatbot_engagement_stamp(chatbot_engagement_t *e,
    const char *channel, const char *user, time_t now)
{
  chatbot_engagement_slot_t *s;

  if(e == NULL || channel == NULL || channel[0] == '\0'
      || user == NULL || user[0] == '\0')
    return;

  pthread_mutex_lock(&e->mutex);

  s = chatbot_engagement_find_or_lru(e, channel, user);

  // If we fell through to the LRU path, the slot may hold a stale key;
  // overwrite both fields. Using snprintf guarantees NUL termination
  // regardless of input length.
  snprintf(s->channel, sizeof(s->channel), "%s", channel);
  snprintf(s->user,    sizeof(s->user),    "%s", user);
  s->last_seen = now;

  pthread_mutex_unlock(&e->mutex);
}

// Non-destructive query: returns true when a live stamp exists for
// (channel, user) within `window_secs` of `now`. `window_secs == 0`
// means the sticky feature is disabled — caller should filter before
// calling, but we honour the convention defensively.
static bool
chatbot_engagement_peek(chatbot_engagement_t *e,
    const char *channel, const char *user,
    time_t now, uint32_t window_secs)
{
  bool hit;

  if(e == NULL || window_secs == 0
      || channel == NULL || channel[0] == '\0'
      || user == NULL || user[0] == '\0')
    return(false);

  hit = false;

  pthread_mutex_lock(&e->mutex);

  for(size_t i = 0; i < CHATBOT_ENGAGEMENT_SLOTS; i++)
  {
    chatbot_engagement_slot_t *s = &e->slots[i];

    if(s->channel[0] == '\0' || s->last_seen == 0)
      continue;

    if(strcmp(s->channel, channel) != 0)
      continue;

    if(strcasecmp(s->user, user) != 0)
      continue;

    if(now >= s->last_seen
        && (uint64_t)(now - s->last_seen) < (uint64_t)window_secs)
      hit = true;

    break;
  }

  pthread_mutex_unlock(&e->mutex);

  return(hit);
}

// Clear the (channel, user) slot by zeroing its `last_seen`. The slot
// stays allocated (so its position in the LRU scan is preserved) but
// will not promote until the next stamp refreshes it. No-op when no
// matching slot exists.
static void
chatbot_engagement_clear(chatbot_engagement_t *e,
    const char *channel, const char *user)
{
  if(e == NULL || channel == NULL || channel[0] == '\0'
      || user == NULL || user[0] == '\0')
    return;

  pthread_mutex_lock(&e->mutex);

  for(size_t i = 0; i < CHATBOT_ENGAGEMENT_SLOTS; i++)
  {
    chatbot_engagement_slot_t *s = &e->slots[i];

    if(s->channel[0] == '\0') continue;

    if(strcmp(s->channel, channel) == 0
        && strcasecmp(s->user, user) == 0)
    {
      s->last_seen = 0;
      break;
    }
  }

  pthread_mutex_unlock(&e->mutex);
}

// Rewrite `s->user` to `new_user` on every slot whose user matches
// `old_user`. Called on METHOD_MSG_NICK_CHANGE so the sticky engagement
// window survives an IRC rename — the dossier layer already collapses
// both identities into one record, but the engagement ring is keyed on
// the protocol-level nick and would otherwise orphan the slot until the
// user re-addresses the bot. No channel argument: an IRC nick is
// globally unique per network, so every channel's slot for this user
// should move. Case-insensitive match to stay consistent with peek.
static void
chatbot_engagement_rename(chatbot_engagement_t *e,
    const char *old_user, const char *new_user)
{
  if(e == NULL
      || old_user == NULL || old_user[0] == '\0'
      || new_user == NULL || new_user[0] == '\0')
    return;

  pthread_mutex_lock(&e->mutex);

  for(size_t i = 0; i < CHATBOT_ENGAGEMENT_SLOTS; i++)
  {
    chatbot_engagement_slot_t *s = &e->slots[i];

    if(s->channel[0] == '\0') continue;

    if(strcasecmp(s->user, old_user) == 0)
      snprintf(s->user, sizeof(s->user), "%s", new_user);
  }

  pthread_mutex_unlock(&e->mutex);
}

// ---------- CV-12 handoff ring ----------
//
// Records the channel's most recent *non-bot* speaker so the sticky
// branch of the classifier can distinguish "user X continuing a chat
// with the bot" from "user X replying to another participant Y who
// just spoke". Stamped after every inbound non-self line; consulted
// from the sticky branch immediately before promotion.

// Stamp the channel's last-non-bot-speaker slot. Callers guard on
// `msg->sender != self` before calling so the bot's own replies never
// enter the ring. LRU on insert: overflow evicts the slot with the
// oldest last_seen.
static void
chatbot_handoff_stamp(chatbot_handoff_t *h,
    const char *channel, const char *user, time_t now)
{
  int idx;
  int    free_idx;
  int    lru_idx;
  time_t lru_ts;

  if(h == NULL || channel == NULL || channel[0] == '\0'
      || user == NULL || user[0] == '\0')
    return;

  pthread_mutex_lock(&h->mutex);

  free_idx = -1;
  lru_idx = 0;
  lru_ts = h->slots[0].last_seen;

  for(int i = 0; i < CHATBOT_HANDOFF_SLOTS; i++)
  {
    chatbot_handoff_slot_t *s = &h->slots[i];

    if(s->channel[0] == '\0')
    {
      if(free_idx < 0) free_idx = i;
      continue;
    }

    if(strcmp(s->channel, channel) == 0)
    {
      snprintf(s->user, sizeof(s->user), "%s", user);
      s->last_seen = now;
      pthread_mutex_unlock(&h->mutex);
      return;
    }

    if(s->last_seen < lru_ts)
    {
      lru_ts  = s->last_seen;
      lru_idx = i;
    }
  }

  idx = (free_idx >= 0) ? free_idx : lru_idx;
  snprintf(h->slots[idx].channel, sizeof(h->slots[idx].channel), "%s",
      channel);
  snprintf(h->slots[idx].user,    sizeof(h->slots[idx].user),    "%s",
      user);
  h->slots[idx].last_seen = now;

  pthread_mutex_unlock(&h->mutex);
}

// Returns true and fills *out_user when the channel's last non-bot
// speaker was *someone other than* `current_user`, stamped within
// `window_secs` of `now`. Returns false on empty slot, same-user, or
// out-of-window. Case-insensitive on user to stay consistent with the
// engagement ring's matching rule.
static bool
chatbot_handoff_peek_other(chatbot_handoff_t *h,
    const char *channel, const char *current_user,
    time_t now, uint32_t window_secs,
    char *out_user, size_t out_user_sz)
{
  bool found;

  if(h == NULL || channel == NULL || channel[0] == '\0'
      || current_user == NULL || current_user[0] == '\0'
      || window_secs == 0)
    return(false);

  found = false;

  pthread_mutex_lock(&h->mutex);

  for(int i = 0; i < CHATBOT_HANDOFF_SLOTS; i++)
  {
    chatbot_handoff_slot_t *s = &h->slots[i];

    if(s->channel[0] == '\0' || s->last_seen == 0) continue;
    if(strcmp(s->channel, channel) != 0)            continue;

    if(strcasecmp(s->user, current_user) == 0)           break; // same speaker
    if(now < s->last_seen)                               break; // clock skew
    if((uint64_t)(now - s->last_seen)
        > (uint64_t)window_secs)                         break; // out of window

    if(out_user != NULL && out_user_sz > 0)
      snprintf(out_user, out_user_sz, "%s", s->user);
    found = true;
    break;
  }

  pthread_mutex_unlock(&h->mutex);
  return(found);
}

// IRC nick character class: letters, digits, underscore, hyphen, and
// the IRC-RFC "special" chars. Good enough for the handoff detector,
// which only needs to recognise a nick-looking token — false positives
// here mean we disengage a sticky slot slightly too eagerly, never
// that the bot speaks out of turn.
static inline bool
chatbot_nick_char(unsigned char c)
{
  return(isalnum(c) || c == '_' || c == '-' || c == '[' || c == ']');
}

// Does the first non-whitespace token of `text` look like a nick
// prefix addressed to someone else? Two accepted shapes:
//
//   "@nick ..."           — @-prefixed, terminated by whitespace/end
//   "nick: ..." / "nick," — punctuation-terminated
//
// The upstream classifier has already concluded WITNESS, so any nick
// we match here is by definition *not* the bot — no comparison to
// bot_nick is needed. Returns false for empty/whitespace-only text.
static bool
chatbot_text_starts_with_some_other_nick(const char *text)
{
  char term;
  const char *start;
  bool at_prefix;
  const char *p;

  if(text == NULL) return(false);

  p = text;

  while(*p == ' ' || *p == '\t') p++;
  if(*p == '\0') return(false);

  at_prefix = false;

  if(*p == '@')
  {
    at_prefix = true;
    p++;
  }

  start = p;

  while(*p != '\0' && chatbot_nick_char((unsigned char)*p))
    p++;

  if(p == start) return(false);

  term = *p;

  if(at_prefix)
    // @nick must be followed by whitespace (or end-of-string) to count.
    return(term == '\0' || term == ' ' || term == '\t');

  // Bareword must be followed by address-punctuation.
  return(term == ':' || term == ',');
}

// Classify with engagement awareness. Runs the stateless classifier
// first; only when it returns WITNESS does the sticky path run.
//
// Returns the effective kind and a reason tag for the debug log.
// Side-effects: clears the sender's engagement slot on handoff.
//
// CV-12: `handoff_window_secs` gates sticky promotion on a per-channel
// "last non-bot speaker" ring. When the ring shows a different user
// spoke within the window, the current line is treated as a reply to
// that speaker and demoted to WITNESS. 0 disables the gate.
static mem_msg_kind_t
chatbot_classify_with_engagement(chatbot_state_t *st,
    const method_msg_t *msg, const char *bot_nick,
    uint32_t window_secs, uint32_t handoff_window_secs,
    chatbot_classify_reason_t *out_reason)
{
  mem_msg_kind_t k = chatbot_classify_message(msg, bot_nick);
  time_t now;

  if(k == MEM_MSG_EXCHANGE_IN)
  {
    if(out_reason) *out_reason = CHATBOT_CLASSIFY_DIRECT;
    return(k);
  }

  // Feature disabled or no channel context: plain witness.
  if(window_secs == 0 || msg->channel[0] == '\0')
  {
    if(out_reason) *out_reason = CHATBOT_CLASSIFY_WITNESS;
    return(MEM_MSG_WITNESS);
  }

  if(chatbot_text_starts_with_some_other_nick(msg->text))
  {
    chatbot_engagement_clear(&st->engagement, msg->channel, msg->sender);
    if(out_reason) *out_reason = CHATBOT_CLASSIFY_HANDOFF;
    return(MEM_MSG_WITNESS);
  }

  now = time(NULL);

  if(chatbot_engagement_peek(&st->engagement, msg->channel, msg->sender,
      now, window_secs))
  {
    // CV-12 handoff gate: if the channel's prior non-bot speaker was a
    // *different* user within the handoff window, this line is almost
    // certainly a reply to that other speaker — not a continuation of
    // the bot-exchange. Demote to WITNESS (same reason tag as the
    // nick-address handoff branch above so traces stay uniform). See
    // finding_sticky_engagement_answers_others.md.
    char other[METHOD_SENDER_SZ];
    if(chatbot_handoff_peek_other(&st->handoff, msg->channel,
        msg->sender, now, handoff_window_secs,
        other, sizeof(other)))
    {
      clam(CLAM_DEBUG, "chatbot",
          "bot=%s sticky suppressed: prior=%s window=%us "
          "sender=%s text=%.120s",
          bot_nick != NULL ? bot_nick : "?",
          other, handoff_window_secs,
          msg->sender, msg->text);
      if(out_reason) *out_reason = CHATBOT_CLASSIFY_WITNESS;
      return(MEM_MSG_WITNESS);
    }

    if(out_reason) *out_reason = CHATBOT_CLASSIFY_STICKY;
    return(MEM_MSG_EXCHANGE_IN);
  }

  if(out_reason) *out_reason = CHATBOT_CLASSIFY_WITNESS;
  return(MEM_MSG_WITNESS);
}

// ---------- driver callbacks ----------

static void *
chatbot_create(bot_inst_t *inst)
{
  chatbot_state_t *st = mem_alloc("chatbot", "state", sizeof(*st));
  const char *name;
  char key[KV_KEY_SZ];

  if(st == NULL) return(NULL);

  memset(st, 0, sizeof(*st));
  st->inst = inst;
  pthread_rwlock_init(&st->lock, NULL);
  pthread_mutex_init(&st->flight_mutex, NULL);
  pthread_mutex_init(&st->coalesce_mutex, NULL);
  pthread_mutex_init(&st->engagement.mutex, NULL);
  pthread_mutex_init(&st->handoff.mutex,    NULL);
  pthread_mutex_init(&st->volunteer.mutex,  NULL);
  pthread_mutex_init(&st->witness_cd.mutex, NULL);

  chatbot_vision_state_init(st);

  // Seed active personality from per-instance KV (may be empty).
  snprintf(key, sizeof(key), "bot.%s.behavior.personality",
      bot_inst_name(inst));

  name = kv_get_str(key);
  if(name == NULL || name[0] == '\0')
    name = kv_get_str("plugin.chat.default_personality");

  if(name != NULL)
    snprintf(st->active_name, sizeof(st->active_name), "%s", name);

  return(st);
}

static void
chatbot_destroy(void *handle)
{
  chatbot_state_t *st = handle;
  if(st == NULL) return;

  pthread_rwlock_destroy(&st->lock);
  pthread_mutex_destroy(&st->flight_mutex);
  pthread_mutex_destroy(&st->coalesce_mutex);
  pthread_mutex_destroy(&st->engagement.mutex);
  pthread_mutex_destroy(&st->handoff.mutex);
  pthread_mutex_destroy(&st->volunteer.mutex);
  pthread_mutex_destroy(&st->witness_cd.mutex);
  chatbot_vision_state_destroy(st);
  mem_free(st);
}

static void chatbot_register_interests(chatbot_state_t *st);

// Idempotent registration gate. Called from chatbot_start (eager),
// chatbot_on_message (lazy retry), and commands.c's /bot <name>
// refresh_prompts path (explicit operator-driven re-sync). Compares
// the currently active personality name against the one reflected in
// the cached topic list; if they match, returns without work.
// Otherwise runs the registration pass and updates the snapshot on
// success. Failure is silent on the hot path — the WARN inside
// chatbot_register_interests is the single source of truth for the
// operator.
void
chatbot_ensure_interests(chatbot_state_t *st)
{
  char active[CHATBOT_PERSONALITY_NAME_SZ];
  char registered[CHATBOT_PERSONALITY_NAME_SZ];

  if(st == NULL) return;

  pthread_rwlock_rdlock(&st->lock);
  snprintf(active,     sizeof(active),     "%s", st->active_name);
  snprintf(registered, sizeof(registered), "%s", st->registered_persona);
  pthread_rwlock_unlock(&st->lock);

  if(active[0] == '\0')
    return;

  if(strcmp(active, registered) == 0)
    return;

  chatbot_register_interests(st);

  // Re-check under the wrlock: if active_name changed again while the
  // worker ran, leave registered_persona empty so the next call re-runs.
  pthread_rwlock_wrlock(&st->lock);
  if(strcmp(st->active_name, active) == 0 && st->n_topics > 0)
    snprintf(st->registered_persona,
        sizeof(st->registered_persona), "%s", active);
  else
    st->registered_persona[0] = '\0';
  pthread_rwlock_unlock(&st->lock);
}

// Register the bot's interests (if any) with the acquisition engine.
// Reads the active personality's interests_json, parses into topics,
// and hands them to acquire_register_topics. No-op when the bot lacks
// an acquired_corpus KV or an active personality. All per-bot side
// effects are best-effort: a missing corpus, parse failure, etc. logs
// a WARN and the bot otherwise starts normally.
static void
chatbot_register_interests(chatbot_state_t *st)
{
  const char *botname = bot_inst_name(st->inst);
  char key[128];
  acquire_topic_t topics[CHATBOT_TOPIC_CACHE_MAX];
  size_t n;
  chatbot_personality_t p;
  char persona[CHATBOT_PERSONALITY_NAME_SZ];
  const char *corpus;

  snprintf(key, sizeof(key), "bot.%s.acquired_corpus", botname);
  corpus = kv_get_str(key);

  if(corpus == NULL || corpus[0] == '\0')
    return;

  // Grab the active personality name under the rwlock so we don't race
  // personality switches (KV writes to bot.<name>.behavior.personality). A bot
  // without any active personality
  // has nothing to register — it may get one later via the KV change
  // callback, which re-runs registration at that point.

  pthread_rwlock_rdlock(&st->lock);
  snprintf(persona, sizeof(persona), "%s", st->active_name);
  pthread_rwlock_unlock(&st->lock);

  if(persona[0] == '\0')
  {
    clam(CLAM_DEBUG2, "chatbot",
        "bot='%s' has acquired_corpus='%s' but no active personality;"
        " skipping interest registration",
        botname, corpus);
    return;
  }

  if(chatbot_personality_read(persona, &p) != SUCCESS)
  {
    clam(CLAM_WARN, "chatbot",
        "bot='%s' personality='%s' not readable; skipping interests",
        botname, persona);
    return;
  }

  // Upsert the destination corpus so the engine can write into it
  // without requiring the operator to pre-create it.
  if(knowledge_corpus_upsert(corpus, NULL) != SUCCESS)
    clam(CLAM_WARN, "chatbot",
        "bot='%s' could not upsert acquired_corpus='%s'; "
        "topic registration will proceed but ingest may fail later",
        botname, corpus);

  n = 0;

  if(chatbot_interests_parse(p.interests_json, topics,
      sizeof(topics) / sizeof(topics[0]), &n) == SUCCESS && n > 0)
  {
    if(acquire_register_topics(botname, topics, n, corpus) == SUCCESS)
      clam(CLAM_INFO, "chatbot",
          "bot='%s' registered %zu acquisition topic(s) from "
          "personality='%s' into corpus='%s'",
          botname, n, persona, corpus);
    else
      clam(CLAM_WARN, "chatbot",
          "bot='%s' acquire_register_topics failed", botname);

    // Cache the parsed topics on the bot state so the reactive scanner
    // on the on_message hot path can match keywords without re-parsing
    // the personality JSON on every inbound line.
    pthread_rwlock_wrlock(&st->lock);
    if(n > CHATBOT_TOPIC_CACHE_MAX) n = CHATBOT_TOPIC_CACHE_MAX;
    memcpy(st->topics, topics, n * sizeof(topics[0]));
    st->n_topics = n;
    pthread_rwlock_unlock(&st->lock);
  }

  else if(n == 0 && p.interests_json != NULL && p.interests_json[0] != '\0')
  {
    clam(CLAM_WARN, "chatbot",
        "bot='%s' personality='%s' declared interests but zero parsed;"
        " check interests_json content",
        botname, persona);

    pthread_rwlock_wrlock(&st->lock);
    st->n_topics = 0;
    pthread_rwlock_unlock(&st->lock);
  }

  else
  {
    pthread_rwlock_wrlock(&st->lock);
    st->n_topics = 0;
    pthread_rwlock_unlock(&st->lock);
  }

  chatbot_personality_free(&p);
}

static bool
chatbot_start(void *handle)
{
  chatbot_state_t *st = handle;
  uint32_t interval;
  userns_t *ns;
  uint32_t  ns_id;
  const char *botname;
  char key[128];

  if(st == NULL) return(SUCCESS);

  chatbot_ensure_interests(st);

  botname = bot_inst_name(st->inst);

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.enabled", botname);
  if(kv_get_uint(key) == 0)
    return(SUCCESS);

  snprintf(key, sizeof(key), "bot.%s.behavior.fact_extract.interval_secs", botname);
  interval = (uint32_t)kv_get_uint(key);

  ns = bot_get_userns(st->inst);
  ns_id = ns != NULL ? ns->id : 0;
  if(ns_id == 0)
    return(SUCCESS);

  extract_schedule(botname, ns_id, interval);
  return(SUCCESS);
}

static void
chatbot_stop(void *handle)
{
  chatbot_state_t *st = handle;
  if(st == NULL) return;

  acquire_unregister_bot(bot_inst_name(st->inst));
  extract_unschedule(bot_inst_name(st->inst));
}

// Evaluate speak policy for a (possibly coalesced) message and, if it
// fires, submit it to the reply pipeline. Shared by the per-line path
// (coalesce_ms == 0) and the coalescer flush task.
static void
chatbot_consider_speaking(chatbot_state_t *st, const method_msg_t *msg,
    mem_msg_kind_t kind, chatbot_classify_reason_t reason)
{
  const char *botname = bot_inst_name(st->inst);
  char key[128];
  chatbot_speak_t decision;
  uint32_t ewin;
  const char *target;
  time_t now;
  time_t last;
  time_t witness_last;
  uint32_t inflight;
  bool engaged_peek;
  uint32_t roll;
  bool harm_signal;
  uint32_t relevance_boost_pct;
  uint32_t witness_cd;
  uint32_t cooldown;
  uint32_t witness_base_prob;
  uint32_t interject_prob;
  uint32_t max_inflight;
  uint64_t mute_until;
  time_t now_mute;

  // Mute gate: if mute_until is in the future, suppress all replies.
  // Clears itself lazily once the deadline passes so idle bots don't
  // need a scheduled task to re-enable.
  snprintf(key, sizeof(key), "bot.%s.behavior.mute_until", botname);
  mute_until = kv_get_uint(key);
  now_mute = time(NULL);
  if(mute_until > 0)
  {
    if((time_t)mute_until > now_mute)
      return;
    kv_set_uint(key, 0);
  }

  // IV3 vision intercept. Runs after the mute gate so /hush still wins.
  // Bypasses speak-policy on purpose: when image_vision.enabled and an
  // image URL is present, vision is the bot's response to the message.
  if(chatbot_vision_maybe_submit(st, msg)) return;

  snprintf(key, sizeof(key), "bot.%s.behavior.max_inflight", botname);
  max_inflight = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.interject_prob", botname);
  interject_prob = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.witness_base_prob", botname);
  witness_base_prob = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.reply_cooldown_secs", botname);
  cooldown = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.witness_interject_cooldown_secs", botname);
  witness_cd = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.engagement_window_secs", botname);
  ewin = (uint32_t)kv_get_uint(key);

  target = msg->channel[0] != '\0' ? msg->channel : msg->sender;
  now = time(NULL);
  last = chatbot_inflight_last_reply(st, target);
  witness_last = chatbot_last_witness_interject(st, target);
  inflight = chatbot_inflight_get(st);
  engaged_peek = (msg->channel[0] != '\0')
      && chatbot_engagement_peek(&st->engagement, msg->channel, msg->sender,
          now, ewin);

  roll = (uint32_t)(rand() % 100);

  // Harm-warning fast-path + relevance boost feed only the WITNESS
  // path. EXCHANGE_IN bypasses both gates inside speak_policy, but we
  // still compute them for consistent log output.
  harm_signal = false;
  relevance_boost_pct = 0;
  if(kind == MEM_MSG_WITNESS)
  {
    size_t hits;

    harm_signal = chatbot_text_has_harm_pattern(msg->text);
    hits = chatbot_relevance_score(st, msg->text);
    if(hits >= 2)      relevance_boost_pct = 500;
    else if(hits == 1) relevance_boost_pct = 200;
  }

  decision = chatbot_speak_decide(kind,
      inflight, max_inflight, now, last, cooldown,
      witness_last, witness_cd,
      interject_prob, witness_base_prob, roll,
      harm_signal, relevance_boost_pct);

  // VF-1 interject decision trace: emitted unconditionally so an
  // IGNORE roll is as visible as a fire. One line per
  // consider-speaking call; greppable via `clam=interject trace=decide`.
  // VF-3: witness_last / witness_cd surface the new cooldown so
  // operators can see when a roll that would otherwise have fired was
  // gated by the distribution shaper. harm/relevance fields surface
  // the convo-uplift gates so a silence can be attributed to either
  // the LLM SKIPping (kind=EXCHANGE_IN ... decision=REPLY) or to
  // genuine WITNESS gating (kind=WITNESS harm=0 relevance=0
  // decision=IGNORE).
  clam(CLAM_DEBUG, "interject",
      "trace=decide bot=%s kind=%s reason=%s prob=%u base=%u roll=%u "
      "cooldown=%u in_flight=%u/%u last_reply=%ld engaged=%d target=%s "
      "witness_last=%ld witness_cd=%u harm=%d relevance=%u decision=%s",
      botname, kind == MEM_MSG_EXCHANGE_IN ? "EXCHANGE_IN" : "WITNESS",
      chatbot_classify_reason_tag(reason),
      interject_prob, witness_base_prob, roll,
      cooldown, inflight, max_inflight,
      (long)last, engaged_peek ? 1 : 0,
      target[0] ? target : "",
      (long)witness_last, witness_cd,
      harm_signal ? 1 : 0, relevance_boost_pct,
      chatbot_speak_name(decision));

  // VF-3: emit a dedicated skip line when the new witness-cooldown
  // gate was the reason for IGNORE. Mirrors the volunteer cascade's
  // skip-reason style so `grep 'reason=witness_cooldown'` (from the
  // verify steps in TODO §VF-3) surfaces the gate in the log.
  if(decision == CHATBOT_SPEAK_IGNORE
      && kind == MEM_MSG_WITNESS
      && witness_cd > 0
      && witness_last > 0
      && (now - witness_last) < (time_t)witness_cd)
    clam(CLAM_DEBUG, "interject",
        "skip reason=witness_cooldown bot=%s target=%s since=%lds cooldown=%u",
        botname, target[0] ? target : "",
        (long)(now - witness_last), witness_cd);

  if(decision == CHATBOT_SPEAK_IGNORE)
    return;

  // Sticky engagement: the bot actually decided to reply to this
  // EXCHANGE_IN, so mark the (channel, sender) pair as engaged. This
  // is the stamp site honoured by both engagement_require_reply modes
  // — in the default (true) mode it's the only stamp; in require_reply
  // = false mode the inbound path stamps additionally. DMs have an
  // empty channel and do not engage the ring: they're already
  // promoted unconditionally by the stateless classifier.
  if(kind == MEM_MSG_EXCHANGE_IN && msg->channel[0] != '\0')
    chatbot_engagement_stamp(&st->engagement, msg->channel, msg->sender, now);

  // VF-1: proves the legacy reply/interject path is the one that spoke.
  clam(CLAM_DEBUG, "interject",
      "trace=path bot=%s path=reply kind=%s reason=%s target=%s",
      botname, kind == MEM_MSG_EXCHANGE_IN ? "EXCHANGE_IN" : "WITNESS",
      chatbot_classify_reason_tag(reason),
      target[0] ? target : "");

  // CV-7: is_direct_address narrows the CV-4 fallback gate. Coalesce
  // flushes reach this site via chatbot_consider_speaking with reason
  // synthesised at chatbot.c:1345-1346, so DIRECT propagates correctly
  // without an extra field on chatbot_coalesce_slot_t.
  chatbot_reply_submit(st, msg, decision == CHATBOT_SPEAK_REPLY,
      reason == CHATBOT_CLASSIFY_DIRECT);

  // VF-3: stamp the per-target witness-interject ring ONLY after a
  // successful interject submit. Direct-address replies (REPLY) do
  // not consume this budget — a user who addresses the bot is always
  // answered subject to the reply cooldown, not this one.
  if(decision == CHATBOT_SPEAK_INTERJECT && target[0] != '\0')
    chatbot_stamp_witness_interject(st, target, now);
}

// Resolve the dossier for an inbound message. See chatbot.h for the
// contract. Returns 0 if the driver does not produce signatures, the
// namespace is missing, or the sender has no match and the
// anonymous_dossiers toggle is off.
dossier_id_t
chatbot_resolve_dossier(chatbot_state_t *st, const method_msg_t *msg)
{
  bool allow_anon;
  bool create;
  const char *label;
  dossier_sig_t sig;
  dossier_id_t pid;
  char key[128];
  const char *matched_user;
  uint32_t    matched_uid;
  const char *method_kind;
  char sig_json[512];
  userns_t *ns;

  if(st == NULL || msg == NULL || msg->inst == NULL)
    return(0);

  ns = bot_get_userns(st->inst);
  if(ns == NULL)
    return(0);

  if(chat_identity_signature_build(msg, sig_json, sizeof(sig_json))
      != SUCCESS)
    return(0);

  method_kind = method_inst_kind(msg->inst);
  if(method_kind == NULL || method_kind[0] == '\0')
    return(0);

  // MFA match gates automatic dossier creation for registered users.
  // metadata is "nick!ident@host" on IRC; empty / absent elsewhere.
  matched_user = NULL;
  matched_uid = 0;
  if(msg->metadata[0] != '\0')
  {
    matched_user = userns_mfa_match(ns, msg->metadata);
    if(matched_user != NULL)
      matched_uid = userns_user_id(ns, matched_user);
  }

  // Anonymous-dossier toggle: when off, only MFA-matched senders get
  // a dossier; others resolve to 0 and their messages log without one.
  snprintf(key, sizeof(key), "bot.%s.behavior.anonymous_dossiers",
      bot_inst_name(st->inst));
  allow_anon = (kv_get_uint(key) != 0);

  create = (matched_user != NULL) || allow_anon;

  label = matched_user != NULL ? matched_user : msg->sender;

  sig.method_kind = method_kind;
  sig.sig_json = sig_json;

  pid = dossier_resolve(ns->id, &sig, label, create);
  if(pid == 0)
    return(0);

  if(matched_uid > 0)
  {
    // Link the dossier to the registered user. Idempotent at the SQL
    // level; dossier_set_user issues an UPDATE regardless.
    dossier_set_user(pid, (int)matched_uid);
  }

  return(pid);
}

// Handle a METHOD_MSG_NICK_CHANGE event: if the old identity resolves
// to an existing dossier (no create), record the new identity as a
// sighting on the same dossier at confidence 1.0 (ground truth).
// Returns SUCCESS even when no dossier exists for the old nick -- that
// just means there was no prior dossier to collapse into.
bool
chatbot_handle_nick_change(chatbot_state_t *st, const method_msg_t *msg)
{
  dossier_sig_t new_sig;
  char new_sig_json[512];
  dossier_sig_t old_sig;
  dossier_id_t pid;
  char old_sig_json[512];
  method_msg_t scratch;
  const char *method_kind;
  userns_t *ns;

  if(st == NULL || msg == NULL || msg->inst == NULL) return(SUCCESS);

  // Rename engagement slots before the dossier work. The ring is a
  // bot-local feature — independent of userns, dossier, and method_kind
  // — and we want the rename even when the old nick has no prior
  // dossier. msg->sender is the old nick, msg->text the new nick
  // (see plugins/method/irc/irc.c:1601-1602).
  chatbot_engagement_rename(&st->engagement, msg->sender, msg->text);

  ns = bot_get_userns(st->inst);
  if(ns == NULL) return(SUCCESS);

  method_kind = method_inst_kind(msg->inst);
  if(method_kind == NULL || method_kind[0] == '\0') return(SUCCESS);

  // Build the old signature: reuse chat_identity_signature_build by
  // constructing a scratch msg whose metadata is the old identity.
  scratch = *msg;
  snprintf(scratch.metadata, sizeof(scratch.metadata), "%s", msg->prev_metadata);

  if(chat_identity_signature_build(&scratch,
        old_sig_json, sizeof(old_sig_json)) != SUCCESS)
    return(SUCCESS);

  old_sig.method_kind = method_kind;
  old_sig.sig_json = old_sig_json;

  pid = dossier_resolve(ns->id, &old_sig, "", false);
  if(pid == 0)
    return(SUCCESS);   // no prior dossier to collapse into -- fine

  if(chat_identity_signature_build(msg,
        new_sig_json, sizeof(new_sig_json)) != SUCCESS)
    return(SUCCESS);

  new_sig.method_kind = method_kind;
  new_sig.sig_json = new_sig_json;

  dossier_record_sighting(pid, &new_sig);
  clam(CLAM_DEBUG, "chatbot",
      "NICK %s -> %s collapsed into dossier %lld",
      msg->sender, msg->text, (long long)pid);
  return(SUCCESS);
}

// Log one inbound line to conversation_log. Done inline per-line (even
// when coalescing is on) so RAG retrieval always sees individual
// utterances in the shape the corpus uses.
static void
chatbot_log_line(chatbot_state_t *st, const method_msg_t *msg,
    mem_msg_kind_t kind)
{
  userns_t *ns = bot_get_userns(st->inst);
  uint32_t ns_id = (ns != NULL) ? ns->id : 0;

  dossier_id_t pid = chatbot_resolve_dossier(st, msg);

  mem_msg_t out = {0};
  out.ns_id        = (int)ns_id;
  out.user_id_or_0 = 0;
  out.dossier_id   = pid;
  snprintf(out.bot_name, sizeof(out.bot_name),
      "%s", bot_inst_name(st->inst));
  snprintf(out.method, sizeof(out.method),
      "%s", msg->inst ? method_inst_kind(msg->inst) : "");
  snprintf(out.channel, sizeof(out.channel), "%s", msg->channel);
  out.kind = kind;
  if(msg->is_action)
    snprintf(out.text, sizeof(out.text), "* %s %s", msg->sender, msg->text);
  else
    snprintf(out.text, sizeof(out.text), "%s", msg->text);

  if(ns != NULL && msg->inst != NULL && out.text[0] != '\0')
  {
    const char *method_kind = method_inst_kind(msg->inst);
    if(method_kind != NULL && method_kind[0] != '\0')
    {
      dossier_id_t refs[MEM_MSG_REFS_MAX];
      size_t n = dossier_find_mentions(ns->id, method_kind,
          24 * 3600, out.text,
          refs, MEM_MSG_REFS_MAX);
      for(size_t i = 0; i < n; i++)
      {
        // Skip the sender's own dossier: self-mentions are noise.
        if(refs[i] == pid) continue;
        out.referenced_dossiers[out.n_referenced++] = (int64_t)refs[i];
      }
    }
  }

  memory_log_message(&out);
}

// ---------- paste coalescing ----------

typedef struct
{
  chatbot_state_t *st;
  uint32_t        slot_idx;
  uint32_t        seq;
} chatbot_coalesce_flush_t;

// Find an existing coalesce slot for (method, sender), or an empty one.
// Must be called with st->coalesce_mutex held. Returns slot index or -1
// if all slots are full and no match exists.
static int
chatbot_coalesce_find_slot(chatbot_state_t *st,
    const method_inst_t *method, const char *sender, bool *out_new)
{
  int empty = -1;
  for(uint32_t i = 0; i < CHATBOT_COALESCE_SLOTS; i++)
  {
    chatbot_coalesce_slot_t *s = &st->coalesce[i];
    if(s->in_use && s->method == method
        && strncasecmp(s->sender, sender, METHOD_SENDER_SZ) == 0)
    {
      if(out_new) *out_new = false;
      return((int)i);
    }
    if(!s->in_use && empty < 0) empty = (int)i;
  }
  if(out_new) *out_new = true;
  return(empty);
}

// Deferred-task callback: if the slot's seq still matches, extract and
// flush it through the normal speak-policy pipeline. If a newer line
// superseded us, do nothing (the newer task will flush later).
static void
chatbot_coalesce_fire(task_t *t)
{
  chatbot_coalesce_flush_t *fl = t->data;
  chatbot_state_t *st;
  chatbot_coalesce_slot_t *slot;
  method_msg_t synth = {0};
  mem_msg_kind_t kind;
  bool slot_any_direct;
  bool fire;

  t->state = TASK_ENDED;
  if(fl == NULL) return;

  st = fl->st;
  slot = &st->coalesce[fl->slot_idx];

  // Take a copy of the slot under the lock, then release. We must not
  // hold coalesce_mutex across the reply-submit path (which runs through
  // memory/llm and could reenter chatbot code on the same thread).
  kind = MEM_MSG_WITNESS;
  slot_any_direct = false;
  fire = false;

  pthread_mutex_lock(&st->coalesce_mutex);
  if(slot->in_use && slot->seq == fl->seq)
  {
    size_t n;

    synth.inst = slot->method;
    synth.is_action = slot->is_action;
    snprintf(synth.sender,   sizeof(synth.sender),   "%s", slot->sender);
    snprintf(synth.metadata, sizeof(synth.metadata), "%s", slot->metadata);
    snprintf(synth.channel,  sizeof(synth.channel),  "%s", slot->channel);
    n = slot->text_len;
    if(n >= sizeof(synth.text)) n = sizeof(synth.text) - 1;
    memcpy(synth.text, slot->text, n);
    synth.text[n] = '\0';
    synth.timestamp = slot->first_ts;
    kind = slot->was_addressed ? MEM_MSG_EXCHANGE_IN : MEM_MSG_WITNESS;
    slot_any_direct = slot->any_direct;

    if(slot->lines > 1)
      clam(CLAM_DEBUG, "chatbot",
          "coalesced %u lines from '%s' on '%s'%s",
          slot->lines, slot->sender,
          slot->channel[0] ? slot->channel : "(dm)",
          slot->truncated ? " (truncated)" : "");

    // Clear the slot for reuse.
    memset(slot, 0, sizeof(*slot));
    fire = true;
  }
  pthread_mutex_unlock(&st->coalesce_mutex);

  if(fire)
  {
    // Reason is preserved across the flush via slot->any_direct; see
    // chatbot_coalesce_slot_t in chatbot.h. A slot that EXCHANGE_IN'd
    // with no DIRECT line inside was sticky-promoted, not addressed.
    chatbot_classify_reason_t r;
    if(slot_any_direct)
      r = CHATBOT_CLASSIFY_DIRECT;
    else if(kind == MEM_MSG_EXCHANGE_IN)
      r = CHATBOT_CLASSIFY_STICKY;
    else
      r = CHATBOT_CLASSIFY_WITNESS;
    chatbot_consider_speaking(st, &synth, kind, r);
  }

  mem_free(fl);
}

// Append this line into the sender's coalesce slot and schedule a
// deferred flush. Returns true if the line was buffered (caller should
// not run speak-policy immediately); false if coalescing is disabled or
// no slot was available.
//
// `reason` is the classify reason computed for this line; CV-7 threads
// it through so the fire callback can distinguish a DIRECT-only slot
// from a STICKY-only slot (both mark was_addressed=true).
static bool
chatbot_coalesce_enqueue(chatbot_state_t *st, const method_msg_t *msg,
    mem_msg_kind_t kind, chatbot_classify_reason_t reason,
    uint32_t coalesce_ms)
{
  chatbot_coalesce_flush_t *fl;
  size_t llen;
  size_t room;
  char line[METHOD_TEXT_SZ + METHOD_SENDER_SZ + 8];
  chatbot_coalesce_slot_t *s;
  int idx;
  bool is_new;
  uint32_t seq_snapshot;

  if(coalesce_ms == 0) return(false);

  is_new = false;
  seq_snapshot = 0;

  pthread_mutex_lock(&st->coalesce_mutex);

  idx = chatbot_coalesce_find_slot(st, msg->inst, msg->sender, &is_new);
  if(idx < 0)
  {
    // All slots in use; fall back to per-line handling for this sender.
    pthread_mutex_unlock(&st->coalesce_mutex);
    return(false);
  }

  s = &st->coalesce[idx];

  if(is_new)
  {
    memset(s, 0, sizeof(*s));
    s->in_use   = true;
    s->method   = msg->inst;
    s->first_ts = (msg->timestamp != 0) ? msg->timestamp : time(NULL);
    s->is_action = msg->is_action;
    snprintf(s->sender,   sizeof(s->sender),   "%s", msg->sender);
    snprintf(s->channel,  sizeof(s->channel),  "%s", msg->channel);
  }

  // Refresh metadata each append: the sender may have reconnected with
  // a new ident/host between lines, and dossier resolution downstream
  // relies on this being current.
  snprintf(s->metadata, sizeof(s->metadata), "%s", msg->metadata);

  // Append " " separator for very short pastes, otherwise newline — the
  // model reads newline-joined blocks as pasted content and space-joined
  // fragments as a single continued thought.
  if(s->text_len > 0 && s->text_len + 1 < sizeof(s->text))
    s->text[s->text_len++] = '\n';

  // Action lines keep their "* nick text" shape inside the block so the
  // model sees them as emotes even when coalesced with plain lines.
  if(msg->is_action)
    snprintf(line, sizeof(line), "* %s %s", msg->sender, msg->text);
  else
    snprintf(line, sizeof(line), "%s", msg->text);

  llen = strlen(line);
  room = (s->text_len < sizeof(s->text))
      ? (sizeof(s->text) - 1 - s->text_len) : 0;
  if(llen > room) { llen = room; s->truncated = true; }
  memcpy(s->text + s->text_len, line, llen);
  s->text_len += llen;
  s->text[s->text_len] = '\0';

  s->lines++;
  if(s->lines > CHATBOT_COALESCE_MAX_LINES) s->truncated = true;
  if(kind == MEM_MSG_EXCHANGE_IN) s->was_addressed = true;
  if(reason == CHATBOT_CLASSIFY_DIRECT) s->any_direct = true;

  s->seq++;
  seq_snapshot = s->seq;

  pthread_mutex_unlock(&st->coalesce_mutex);

  fl = mem_alloc("chatbot", "coalesce_flush", sizeof(*fl));
  if(fl == NULL) return(true);  // line still buffered; just no new task
  fl->st = st;
  fl->slot_idx = (uint32_t)idx;
  fl->seq = seq_snapshot;

  task_add_deferred("chatbot_coalesce", TASK_ANY, 100,
      coalesce_ms, chatbot_coalesce_fire, fl);

  return(true);
}

// ---------- reactive acquisition scan ----------

// Case-insensitive substring match: returns a pointer to the first byte
// of `needle` inside `haystack`, or NULL. Plain ASCII case-fold — good
// enough for keyword matches like "music" or "album".
static const char *
chatbot_ci_strstr(const char *haystack, const char *needle)
{
  size_t nlen;

  if(haystack == NULL || needle == NULL || needle[0] == '\0')
    return(NULL);

  nlen = strlen(needle);

  for(const char *p = haystack; *p != '\0'; p++)
    if(strncasecmp(p, needle, nlen) == 0)
      return(p);

  return(NULL);
}

// Subject-token predicate: letters, digits, and a couple of inside-name
// punctuation characters ('-' and '\''). Leading/trailing punctuation is
// not part of a token.
static inline bool
chatbot_subj_token_char(unsigned char c)
{
  return(isalnum(c) || c == '-' || c == '\'');
}

// Is `text[p..p+n)` a capitalized token (first alpha byte upper, rest
// mixed)? Rejects ALL-CAPS tokens ("USA") to avoid treating acronyms as
// names — the heuristic wants "Dua Lipa", not "USA FREEDOM".
static bool
chatbot_subj_token_capitalized(const char *text, size_t p, size_t n)
{
  bool any_lower;
  unsigned char first;

  if(n == 0)
    return(false);

  first = (unsigned char)text[p];

  if(!isupper(first))
    return(false);

  any_lower = false;

  for(size_t i = 1; i < n; i++)
  {
    unsigned char c = (unsigned char)text[p + i];

    if(islower(c))
      any_lower = true;
  }

  // Single-char capitalized token (e.g. "I") is fine; multi-char
  // all-caps is not.
  return(n == 1 || any_lower);
}

// Extract a subject phrase near the keyword hit in msg_text. Scans
// ±30 bytes around the match for the longest run of capitalized tokens;
// on no match, falls back to the keyword itself. Writes a NUL-terminated
// subject into `out` (at most out_cap-1 bytes).
static void
chatbot_extract_subject(const char *msg_text, const char *match,
    const char *keyword, char *out, size_t out_cap)
{
  size_t best_start;
  size_t best_len;
  size_t i;
  size_t text_len;
  size_t match_off;
  size_t lo;
  size_t hi;

  if(out_cap == 0) return;
  out[0] = '\0';

  text_len = strlen(msg_text);
  match_off = (size_t)(match - msg_text);

  // Window = [lo, hi) within msg_text.
  lo = (match_off >= 30) ? (match_off - 30) : 0;
  hi = match_off + 30;
  if(hi > text_len) hi = text_len;

  // Walk the window, finding runs of capitalized tokens. Track the
  // longest (in byte count).
  best_start = 0;
  best_len = 0;

  i = lo;

  while(i < hi)
  {
    size_t run_start;
    size_t run_end;

    // Skip non-token bytes.
    while(i < hi && !chatbot_subj_token_char((unsigned char)msg_text[i]))
      i++;

    run_start = i;
    run_end = i;

    while(i < hi)
    {
      size_t tok_start = i;
      size_t tok_len;

      while(i < hi && chatbot_subj_token_char((unsigned char)msg_text[i]))
        i++;

      tok_len = i - tok_start;
      if(tok_len == 0)
        break;

      if(!chatbot_subj_token_capitalized(msg_text, tok_start, tok_len))
      {
        // Non-capitalized token terminates the run; rewind to right
        // before it so the outer loop re-enters non-token skip.
        i = tok_start;
        break;
      }

      run_end = i;

      // After a capitalized token, allow exactly one space/punctuation
      // separator before the next token. Anything else breaks the run.
      if(i < hi)
      {
        unsigned char sep = (unsigned char)msg_text[i];

        if(sep == ' ' || sep == '\t')
          i++;
        else
          break;
      }
    }

    if(run_end > run_start)
    {
      size_t run_len = run_end - run_start;

      if(run_len > best_len)
      {
        best_len   = run_len;
        best_start = run_start;
      }
    }

    // If we didn't advance (empty run after skip), break out.
    if(i == run_start)
      i++;
  }

  if(best_len > 0)
  {
    size_t n;

    // Trim trailing possessive "'s" — "Dua Lipa's" → "Dua Lipa".
    if(best_len >= 2
        && msg_text[best_start + best_len - 2] == '\''
        && (msg_text[best_start + best_len - 1] == 's'
            || msg_text[best_start + best_len - 1] == 'S'))
      best_len -= 2;

    n = best_len < out_cap - 1 ? best_len : out_cap - 1;
    memcpy(out, msg_text + best_start, n);
    out[n] = '\0';
  }

  else
    snprintf(out, out_cap, "%s", keyword);
}

void
chatbot_scan_reactive_topics(chatbot_state_t *st, const method_msg_t *msg)
{
  size_t n_topics;
  const char *botname;

  if(st == NULL || msg == NULL || msg->text[0] == '\0')
    return;

  botname = bot_inst_name(st->inst);

  // Read-lock the topic cache. Keywords are pure read-only data; the
  // window is a handful of short strncasecmp loops — safe to hold the
  // reader lock across the enqueue calls (acquire_enqueue_reactive
  // takes its own locks and does not call back into chatbot).
  pthread_rwlock_rdlock(&st->lock);

  n_topics = st->n_topics;

  for(size_t ti = 0; ti < n_topics; ti++)
  {
    const acquire_topic_t *t = &st->topics[ti];

    // ACTIVE topics never fire reactively; skip them cheaply.
    if(t->mode == ACQUIRE_MODE_ACTIVE)
      continue;

    if(t->n_keywords == 0)
      continue;

    for(size_t ki = 0; ki < t->n_keywords; ki++)
    {
      const char *kw = t->keywords[ki];
      char subject[ACQUIRE_SUBJECT_SZ];
      const char *hit;

      if(kw[0] == '\0')
        continue;

      hit = chatbot_ci_strstr(msg->text, kw);

      if(hit == NULL)
        continue;

      chatbot_extract_subject(msg->text, hit, kw, subject, sizeof(subject));

      clam(CLAM_DEBUG, "chatbot",
          "reactive enqueue bot=%s topic=%s keyword='%s' subject='%s'",
          botname, t->name, kw, subject);

      (void)acquire_enqueue_reactive(botname, t->name, subject);

      // One enqueue per topic per message — if the same text hits two
      // keywords of the same topic, we've already captured the intent.
      break;
    }
  }

  pthread_rwlock_unlock(&st->lock);
}

// Incoming message: classify + log to memory + run speak policy (either
// immediately, or after the paste-coalescing window closes).
static void
chatbot_on_message(void *handle, const method_msg_t *msg)
{
  chatbot_state_t *st = handle;
  uint32_t coalesce_ms;
  chatbot_classify_reason_t reason;
  mem_msg_kind_t kind;
  uint32_t handoff_window;
  bool engagement_require_reply;
  uint32_t engagement_window;
  const char *botname;
  char key[128];
  char self[METHOD_SENDER_SZ] = {0};

  if(st == NULL || msg == NULL) return;

  // Identity events are side-band: no chat log, no speak policy, just
  // dossier bookkeeping.
  if(msg->kind == METHOD_MSG_NICK_CHANGE)
  {
    chatbot_handle_nick_change(st, msg);
    return;
  }

  // Resolve our nick on this method for address detection.
  if(msg->inst != NULL)
    method_get_self(msg->inst, self, sizeof(self));

  botname = bot_inst_name(st->inst);

  // Sticky engagement knobs (per message, same cadence as the rest of
  // the per-line KV reads below). Two small integer reads — no point
  // in a cached-config struct for this.
  snprintf(key, sizeof(key), "bot.%s.behavior.speak.engagement_window_secs", botname);
  engagement_window = (uint32_t)kv_get_uint(key);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.engagement_require_reply", botname);
  engagement_require_reply = (kv_get_uint(key) != 0);

  snprintf(key, sizeof(key), "bot.%s.behavior.speak.handoff_window_secs", botname);
  handoff_window = (uint32_t)kv_get_uint(key);
  if(handoff_window == 0)
    handoff_window = CHATBOT_HANDOFF_WINDOW_DEFAULT_SECS;

  reason = CHATBOT_CLASSIFY_WITNESS;
  kind = chatbot_classify_with_engagement(st, msg,
      self[0] != '\0' ? self : NULL,
      engagement_window, handoff_window, &reason);

  // When the operator opts into require_reply=false, stamp on every
  // inbound EXCHANGE_IN — but only on DIRECT, never on a STICKY
  // promotion. CV-12 Part A: stamping on STICKY would indefinitely
  // extend the sticky window during incidental chatter, making every
  // subsequent line by the same user keep riding the EXCHANGE_IN path.
  // See finding_sticky_engagement_answers_others.md.
  if(!engagement_require_reply
      && kind == MEM_MSG_EXCHANGE_IN
      && reason == CHATBOT_CLASSIFY_DIRECT
      && msg->channel[0] != '\0')
    chatbot_engagement_stamp(&st->engagement, msg->channel, msg->sender,
        time(NULL));

  // Drop WITNESS lines if witness logging is disabled for this instance.
  if(kind == MEM_MSG_WITNESS)
  {
    snprintf(key, sizeof(key), "bot.%s.behavior.witness_log", botname);
    if(kv_get_uint(key) == 0)
      return;
  }

  chatbot_log_line(st, msg, kind);

  // CV-12: after classification, record this line's sender as the
  // channel's most recent non-bot speaker so the *next* inbound line
  // can consult the handoff ring. Stamping AFTER the classify call is
  // deliberate — otherwise handoff_peek_other would self-exclude the
  // current sender and the ring would never return a different user.
  // Bot's own outbound lines don't flow through this path, but a
  // belt-and-braces self-compare is cheap insurance.
  if(msg->channel[0] != '\0' && msg->sender[0] != '\0'
      && !(self[0] != '\0' && strcasecmp(msg->sender, self) == 0))
    chatbot_handoff_stamp(&st->handoff, msg->channel, msg->sender,
        time(NULL));

  // Ensure the reactive topic cache reflects the currently active
  // personality before the scan runs. Steady-state this is a
  // two-rdlock-peek no-op; on the first message after a lazy persona
  // load it registers now so the scan below matches.
  chatbot_ensure_interests(st);

  // Scan for reactive topic keywords and fire enqueues before the
  // speak/coalesce path — the acquisition engine drains the per-bot
  // ring on its own periodic tick, independent of whether we reply.
  chatbot_scan_reactive_topics(st, msg);

  // Uniform observe-trace, greppable via botmanctl -A <bot>. One line
  // per inbound message regardless of kind so attach-mode users see
  // exactly what the bot is witnessing. The reason tag distinguishes
  // direct nick-address from the sticky-engagement window and from a
  // handoff that just cleared the sender's slot.
  clam(CLAM_DEBUG, "chatbot",
      "bot=%s kind=%s(%s) from=%s on=%s: %.200s",
      botname,
      kind == MEM_MSG_EXCHANGE_IN ? "EXCHANGE_IN" : "WITNESS",
      chatbot_classify_reason_tag(reason),
      msg->sender,
      msg->channel[0] ? msg->channel : "(dm)",
      msg->text);

  snprintf(key, sizeof(key), "bot.%s.behavior.coalesce_ms", botname);
  coalesce_ms = (uint32_t)kv_get_uint(key);

  if(chatbot_coalesce_enqueue(st, msg, kind, reason, coalesce_ms))
    return;

  chatbot_consider_speaking(st, msg, kind, reason);
}

// ---------- driver + plugin descriptor ----------

const bot_driver_t chatbot_driver = {
  .name       = "chat",
  .create     = chatbot_create,
  .destroy    = chatbot_destroy,
  .start      = chatbot_start,
  .stop       = chatbot_stop,
  .on_message = chatbot_on_message,
};

static bool
chatbot_plugin_start(void)
{
  memory_ensure_schema();
  dossier_register_config();
  // Method plugins push their identity hooks (signer, scorer,
  // token_scorer) into the chat_identity registry via plugin_dlsym
  // at their own plugin_start time. Nothing to do from here.
  return(SUCCESS);
}

static bool
chatbot_plugin_init(void)
{
  // Kind-wide KV: directory of personality files. Registered directly
  // (not via kv_schema / kv_inst_schema) because the key sits under
  // the "bot.<kind>." prefix rather than "plugin.<kind>." or per-bot.
  if(kv_register("bot.chat.personalitypath", KV_STR, "./personalities",
        NULL, NULL,
        "Filesystem directory scanned for *.txt personality files."
        " Read on demand by the chat reply pipeline.") != SUCCESS)
    return(FAIL);

  if(kv_register("bot.chat.contractpath", KV_STR,
        "./personalities/contracts", NULL, NULL,
        "Filesystem directory holding *.txt output contract files."
        " Selected per-bot via bot.<name>.behavior.contract (or the plugin"
        " default plugin.chat.default_contract).") != SUCCESS)
    return(FAIL);

  // CV-6 — Recent-own-replies anti-repeat slice. Knobs snapshotted at
  // submit time in chatbot_reply_submit; the splice itself renders in
  // assemble_prompt step 4d.
  if(kv_register("chatbot.recent_replies_in_prompt", KV_UINT32, "3",
        NULL, NULL,
        "Number of most-recent own replies (MEM_MSG_EXCHANGE_OUT) to"
        " splice into the system prompt as an anti-repeat reminder."
        " Clamped at CHATBOT_RECENT_REPLIES_MAX. 0 disables the block.")
      != SUCCESS)
    return(FAIL);

  if(kv_register("chatbot.recent_replies_max_age_secs", KV_UINT32, "3600",
        NULL, NULL,
        "Maximum age in seconds for rows considered by the"
        " recent-own-replies anti-repeat block. 0 disables the age"
        " filter. Default 1 hour — old replies are irrelevant.") != SUCCESS)
    return(FAIL);

  // CV-13 — Deterministic post-generation near-repeat guard paired
  // with the CV-6 slice above. Models routinely ignore the soft
  // "do not repeat" instruction on near-identical inputs and emit
  // verbatim copies; this gate drops such lines before method_send.
  if(kv_register("chatbot.anti_repeat_jaccard_pct", KV_UINT32, "85",
        NULL, NULL,
        "CV-13 post-LLM guard. Outgoing reply lines whose trigram"
        " Jaccard similarity (as an integer percent) against any"
        " entry in the CV-6 recent-own-replies slice meets or"
        " exceeds this value are suppressed before method_send."
        " 0 disables the guard; values of 100 or higher are treated"
        " as disabled so a misconfigured knob can't silence all"
        " output. Default 85 catches word-for-word and trivially"
        " reworded repeats while leaving genuine paraphrases through.")
      != SUCCESS)
    return(FAIL);

  memory_init();
  memory_register_config();
  memory_register_commands();

  // Fact-extraction subsystem (moved from libcore into the chat plugin
  // in R2). Must follow memory_init because extract depends on the
  // memory types; extract_register_config is a no-op today but the call
  // site mirrors dossier_register_config so any future subsystem-level
  // knob lands here without reshuffling.
  extract_init();
  extract_register_config();

  // Identity registry (ND4): method plugins publish their per-kind
  // signer + scorer + token-scorer triples into this registry at
  // plugin_start time via plugin_dlsym. Must be up before any method
  // plugin starts.
  chat_identity_init();

  // Dossier subsystem (moved from libcore into the chat plugin in R4).
  // Order at init time: bring up in-memory state and register commands
  // (cmd subsystem is already up). DDL must wait until userns_init has
  // run -- the dossier table FKs into userns -- so dossier_register_config
  // is called from chatbot_plugin_start, which runs after userns_init.
  dossier_init();
  dossier_show_register_commands();
  dossier_register_commands();

  // Reply-pipeline regex compile runs before command register so a
  // compile failure (shouldn't happen — pattern is literal) doesn't
  // leave half-installed commands behind.
  if(chatbot_reply_init() != SUCCESS)
  {
    dossier_exit();
    chat_identity_exit();
    extract_exit();
    memory_exit();
    return(FAIL);
  }

  // V1 — wire the acquire post-ingest hook. Failure is fatal because
  // the plugin is a fresh install; a later consumer can tolerate a
  // missing callback, but plugin_init shouldn't silently half-install.
  if(chatbot_volunteer_init() != SUCCESS)
  {
    chatbot_reply_deinit();
    dossier_exit();
    chat_identity_exit();
    extract_exit();
    memory_exit();
    return(FAIL);
  }

  if(chatbot_cmds_register() != SUCCESS)
  {
    chatbot_volunteer_deinit();
    chatbot_reply_deinit();
    dossier_exit();
    chat_identity_exit();
    extract_exit();
    memory_exit();
    return(FAIL);
  }

  if(chatbot_personality_show_register() != SUCCESS)
  {
    chatbot_cmds_unregister();
    chatbot_volunteer_deinit();
    chatbot_reply_deinit();
    dossier_exit();
    chat_identity_exit();
    extract_exit();
    memory_exit();
    return(FAIL);
  }

  if(chatbot_contract_show_register() != SUCCESS)
  {
    chatbot_cmds_unregister();
    chatbot_volunteer_deinit();
    chatbot_reply_deinit();
    dossier_exit();
    chat_identity_exit();
    extract_exit();
    memory_exit();
    return(FAIL);
  }

  if(chatbot_user_show_verbs_register() != SUCCESS)
  {
    chatbot_cmds_unregister();
    chatbot_volunteer_deinit();
    chatbot_reply_deinit();
    dossier_exit();
    chat_identity_exit();
    extract_exit();
    memory_exit();
    return(FAIL);
  }

  return(SUCCESS);
}

static void
chatbot_plugin_deinit(void)
{
  chatbot_cmds_unregister();
  chatbot_volunteer_deinit();
  chatbot_reply_deinit();
  // Reverse order of init. dossier_exit only frees the stat mutex; no
  // DB writes. Extract teardown must run before memory_exit because
  // extract may hold sweep-state pointing at memory types; extract owns
  // no DB state the memory teardown needs.
  dossier_exit();
  chat_identity_exit();
  extract_exit();
  memory_exit();
}

const plugin_desc_t bm_plugin_desc = {
  .api_version          = PLUGIN_API_VERSION,
  .name                 = "chat",
  .version              = "0.1",
  .type                 = PLUGIN_METHOD,
  .kind                 = "chat",
  .provides             = { { .name = "method_chat" } },
  .provides_count       = 1,
  .requires             = {
    { .name = "inference"   },
    { .name = "core_kv"     },
    { .name = "core_db"     },
    { .name = "core_userns" },
    { .name = "core_cmd"    },
    { .name = "core_task"   },
  },
  .requires_count       = 6,
  .kv_schema            = chatbot_kv_schema,
  .kv_schema_count      = sizeof(chatbot_kv_schema) / sizeof(chatbot_kv_schema[0]),
  .kv_inst_schema       = chatbot_inst_schema,
  .kv_inst_schema_count = sizeof(chatbot_inst_schema) / sizeof(chatbot_inst_schema[0]),
  .init                 = chatbot_plugin_init,
  .start                = chatbot_plugin_start,
  .stop                 = NULL,
  .deinit               = chatbot_plugin_deinit,
  .ext                  = &chatbot_driver,
};
