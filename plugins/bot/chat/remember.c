// botmanager — MIT
// /remember and /forget — keeping a fact the moment somebody says it.
#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "fact_vocab.h"
#include "identity.h"
#include "memory.h"
#include "method.h"
#include "userns.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// A person telling you something about themselves outranks the hourly
// sweep's reading of the same sentence, and never reaches 1.0 — that is
// reserved for an admin seeding a fact by hand.
#define REMEMBER_SOURCE  "user_stated"
#define REMEMBER_CONF    0.95f

// Rendered from fact_vocab.c on demand; sized for the whole table with
// room to grow, since a refusal that lists half the vocabulary is worse
// than no list at all.
#define REMEMBER_VOCAB_LIST_SZ  512

// ---------- helpers ----------

static void
remember_vocab_list(char *out, size_t out_sz)
{
  size_t pos = 0;

  if(out == NULL || out_sz == 0) return;

  out[0] = '\0';

  for(size_t i = 0; i < fact_vocab_count(); i++)
  {
    const fact_vocab_t *v = fact_vocab_at(i);
    int n;

    n = snprintf(out + pos, out_sz - pos, "%s%s", pos > 0 ? ", " : "",
        v->key);

    if(n < 0 || (size_t)n >= out_sz - pos)
      break;

    pos += (size_t)n;
  }
}

// Lowercase, and reject anything that is not a bare key — the value is
// a separate argument, so a key carrying spaces means the speaker (or
// the model bridging for them) mis-split the request.
static bool
remember_key_normalize(const char *in, char *out, size_t out_sz)
{
  size_t i;

  if(in == NULL || out == NULL || out_sz == 0)
    return(FAIL);

  for(i = 0; in[i] != '\0' && i + 1 < out_sz; i++)
  {
    unsigned char c = (unsigned char)in[i];

    if(isspace(c))
      return(FAIL);

    out[i] = (char)tolower(c);
  }

  out[i] = '\0';
  return(out[0] != '\0' ? SUCCESS : FAIL);
}

static void
remember_trim(char *s)
{
  size_t len;
  size_t lead = 0;

  if(s == NULL) return;

  while(s[lead] != '\0' && isspace((unsigned char)s[lead])) lead++;

  if(lead > 0)
    memmove(s, s + lead, strlen(s + lead) + 1);

  len = strlen(s);

  while(len > 0 && isspace((unsigned char)s[len - 1])) len--;

  s[len] = '\0';
}

// The speaker's own dossier, created if this is the first thing we ever
// learn about them — the same resolution nl_observe uses, and
// deliberately not the reply path's: these verbs write about whoever
// typed them and nobody else, which is the whole of their permission
// model (there is no target argument to abuse).
static int64_t
remember_sender_dossier(const cmd_ctx_t *ctx)
{
  userns_t *ns;

  if(ctx == NULL || ctx->msg == NULL || ctx->bot == NULL)
    return(0);

  ns = bot_get_userns(ctx->bot);

  if(ns == NULL)
    return(0);

  return(chat_user_dossier_id(ctx->msg, ns->id, ctx->msg->sender, true));
}

// Shared preamble: canonical key or a refusal that names the vocabulary.
static const fact_vocab_t *
remember_key_or_refuse(const cmd_ctx_t *ctx, const char *raw, char *key,
    size_t key_sz)
{
  const fact_vocab_t *v;
  char list[REMEMBER_VOCAB_LIST_SZ];
  char msg[REMEMBER_VOCAB_LIST_SZ + 128];

  if(remember_key_normalize(raw, key, key_sz) != SUCCESS)
  {
    cmd_reply(ctx, "that isn't a key — give me one word, then the value.");
    return(NULL);
  }

  v = fact_vocab_lookup(key);

  if(v != NULL)
    return(v);

  remember_vocab_list(list, sizeof(list));
  snprintf(msg, sizeof(msg), "I don't keep a '%s'. I can keep: %s", key,
      list);
  cmd_reply(ctx, msg);
  return(NULL);
}

// ---------- /remember ----------

static void
cmd_remember(const cmd_ctx_t *ctx)
{
  const fact_vocab_t *v;
  mem_dossier_fact_t  fact;
  int64_t             did;
  char                key[MEM_FACT_KEY_SZ];
  char                value[MEM_FACT_VALUE_SZ];
  char                ack[MEM_FACT_KEY_SZ + MEM_FACT_VALUE_SZ + 32];
  time_t              now;

  v = remember_key_or_refuse(ctx, ctx->parsed->argv[0], key, sizeof(key));

  if(v == NULL)
    return;

  snprintf(value, sizeof(value), "%s", ctx->parsed->argv[1]);
  remember_trim(value);

  if(value[0] == '\0')
  {
    cmd_reply(ctx, "and the value?");
    return;
  }

  did = remember_sender_dossier(ctx);

  if(did <= 0)
  {
    cmd_reply(ctx, "I can't tell who you are well enough to keep that.");
    return;
  }

  now = time(NULL);

  memset(&fact, 0, sizeof(fact));
  fact.dossier_id = did;
  fact.kind       = v->kind;
  snprintf(fact.fact_key,   sizeof(fact.fact_key),   "%s", v->key);
  snprintf(fact.fact_value, sizeof(fact.fact_value), "%s", value);
  snprintf(fact.source,     sizeof(fact.source),     "%s", REMEMBER_SOURCE);
  // Empty in a DM, which is the partition convention the whole fact
  // store reads as "private" — see CHAT-EXTRACT-DMCHAN-1.
  snprintf(fact.channel,    sizeof(fact.channel),    "%s",
      ctx->msg->channel);
  fact.confidence = REMEMBER_CONF;
  fact.observed_at = now;
  fact.last_seen   = now;

  if(memory_upsert_dossier_fact(&fact, MEM_MERGE_OBSERVE) != SUCCESS)
  {
    cmd_reply(ctx, "something went wrong writing that down.");
    return;
  }

  clam(CLAM_INFO, "chatbot",
      "bot=%s remember %s='%.60s' for %s (dossier %lld)",
      bot_inst_name(ctx->bot), v->key, value, ctx->msg->sender,
      (long long)did);

  snprintf(ack, sizeof(ack), "remembered %s: %s", v->key, value);
  cmd_reply(ctx, ack);
}

// ---------- /forget ----------

static void
cmd_forget(const cmd_ctx_t *ctx)
{
  const fact_vocab_t *v;
  int64_t             did;
  uint32_t            removed;
  char                key[MEM_FACT_KEY_SZ];
  char                ack[MEM_FACT_KEY_SZ + 64];

  v = remember_key_or_refuse(ctx, ctx->parsed->argv[0], key, sizeof(key));

  if(v == NULL)
    return;

  did = remember_sender_dossier(ctx);

  if(did <= 0)
  {
    cmd_reply(ctx, "I can't tell who you are well enough to look.");
    return;
  }

  if(memory_delete_dossier_fact_key(did, v->key, &removed) != SUCCESS)
  {
    cmd_reply(ctx, "something went wrong looking that up.");
    return;
  }

  if(removed == 0)
  {
    snprintf(ack, sizeof(ack), "nothing stored under %s for you.", v->key);
    cmd_reply(ctx, ack);
    return;
  }

  clam(CLAM_INFO, "chatbot",
      "bot=%s forget %s for %s (dossier %lld, %u row(s))",
      bot_inst_name(ctx->bot), v->key, ctx->msg->sender, (long long)did,
      removed);

  snprintf(ack, sizeof(ack), "forgot %s", v->key);
  cmd_reply(ctx, ack);
}

// ---------- registration ----------

static const cmd_arg_desc_t ad_remember[] = {
  { "key",   CMD_ARG_NONE, CMD_ARG_REQUIRED, MEM_FACT_KEY_SZ - 1, NULL },
  { "value", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static const cmd_arg_desc_t ad_forget[] = {
  { "key", CMD_ARG_NONE, CMD_ARG_REQUIRED, MEM_FACT_KEY_SZ - 1, NULL },
};

static const cmd_nl_slot_t remember_slots[] = {
  { .name  = "key",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
  { .name  = "value",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED | CMD_NL_SLOT_REMAINDER },
};

// The FORM lives here, in the examples — never in `.when`. `remember`
// lands alphabetically beside `remind`, and `.when` is the field the
// model routes on: a stray clause here has already been measured
// pulling canonical /remind utterances onto a neighbour (CARE-2b).
static const cmd_nl_example_t remember_examples[] = {
  { .utterance  = "my favorite color is blue",
    .invocation = "/remember favorite_color blue" },
  { .utterance  = "I live in West Chester, Ohio",
    .invocation = "/remember location West Chester, OH" },
  { .utterance  = "my zipcode is 45069",
    .invocation = "/remember postal_code 45069" },
  { .utterance  = "I drive a Ferrari now",
    .invocation = "/remember vehicle Ferrari" },
};

static const cmd_nl_t remember_nl = {
  .when          = "Someone tells you a lasting personal fact about"
                   " themselves to keep in mind, or corrects one (where"
                   " they live, what they drive, favorites, who they"
                   " work for).",
  .syntax        = "/remember <key> <value>",
  .slots         = remember_slots,
  .slot_count    = (uint8_t)(sizeof(remember_slots)
                             / sizeof(remember_slots[0])),
  .examples      = remember_examples,
  .example_count = (uint8_t)(sizeof(remember_examples)
                             / sizeof(remember_examples[0])),
  .dispatch_text = NULL,
};

static const cmd_nl_slot_t forget_slots[] = {
  { .name  = "key",
    .type  = CMD_NL_ARG_FREE,
    .flags = CMD_NL_SLOT_REQUIRED },
};

static const cmd_nl_example_t forget_examples[] = {
  { .utterance  = "forget where I live",
    .invocation = "/forget location" },
  { .utterance  = "I sold my car",
    .invocation = "/forget vehicle" },
};

static const cmd_nl_t forget_nl = {
  .when          = "Someone asks you to forget something you know about"
                   " them, or says a stored fact no longer holds.",
  .syntax        = "/forget <key>",
  .slots         = forget_slots,
  .slot_count    = (uint8_t)(sizeof(forget_slots) / sizeof(forget_slots[0])),
  .examples      = forget_examples,
  .example_count = (uint8_t)(sizeof(forget_examples)
                             / sizeof(forget_examples[0])),
  .dispatch_text = NULL,
};

bool
chatbot_remember_register(void)
{
  if(cmd_register("chat", "remember",
        "remember <key> <value>",
        "Keep a lasting fact about yourself",
        "Writes one fact to what the bot knows about YOU — there is no\n"
        "way to write about anyone else, which is why anybody may use\n"
        "it. Say it plainly and the bot works out the command:\n"
        "'my favorite color is blue' keeps favorite_color.\n"
        "\n"
        "Saying it again with a different value replaces it, so this is\n"
        "also how you correct something the bot got wrong. What the bot\n"
        "picked up from ordinary conversation can never overwrite what\n"
        "you state here.\n"
        "\n"
        "Keys are a fixed list — 'remember' with a key it does not keep\n"
        "answers with the list. Use 'forget <key>' to drop one.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_remember, NULL, NULL, NULL,
        ad_remember,
        (uint8_t)(sizeof(ad_remember) / sizeof(ad_remember[0])),
        NULL, &remember_nl) != SUCCESS)
    return(FAIL);

  if(cmd_register("chat", "forget",
        "forget <key>",
        "Drop a fact the bot keeps about you",
        "Removes what the bot has stored under one key for you, and\n"
        "only for you. 'forget vehicle' after you sell the car.\n"
        "\n"
        "The bot may learn it again from what you say later — this is\n"
        "not a mute, it is an erasure of what is stored now.",
        USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY,
        cmd_forget, NULL, NULL, NULL,
        ad_forget, (uint8_t)(sizeof(ad_forget) / sizeof(ad_forget[0])),
        NULL, &forget_nl) != SUCCESS)
    goto fail_forget;

  return(SUCCESS);

fail_forget:
  cmd_unregister_path("remember");
  return(FAIL);
}
