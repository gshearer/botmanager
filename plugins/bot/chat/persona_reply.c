// botmanager — MIT
// Persona reply: a command surface borrowing the bot's voice for one line.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"

#include <stdio.h>
#include <string.h>

#define PERSONA_REPLY_CTX "persona_reply"

// What the mind is told about the JOB, on top of whatever the persona
// file says about the character. The caller's `instruction` says what
// the line is for; this says what shape it has to come back in, and it
// is fixed here so every consumer of the slot sounds like the same bot
// doing the same trick.
//
// The last clause is the one that earns its place: a persona file is
// written for conversation, where a preamble and a follow-up question
// are good manners. Here they are noise wrapped around a toy's answer.
#define PERSONA_REPLY_STYLE                                            \
  "\n\nYou are being asked for a single line of flavour text, in"       \
  " character, for a command someone just ran. Reply with that line"    \
  " and nothing else: no preamble, no quotation marks, no markdown,"    \
  " no follow-up question, and no explanation of what you are doing."   \
  " Keep it to one sentence unless a second genuinely lands better."    \
  " The command's own output is given to you as FACTS to report, not"   \
  " as a draft to reword — take what happened from it and write your"   \
  " own line about it, borrowing none of its phrasing."

// A line, not an essay. The model is answering with one sentence; this
// is the ceiling that keeps a runaway from paying for a paragraph
// nobody reads, and a truncation here is still delivered — a clipped
// joke beats a dropped one.
#define PERSONA_REPLY_MAX_TOKENS  120

// A flourish is not worth the reply pipeline's patience. Well under
// llm.timeout_secs (300): the human is sitting in a channel waiting for
// a coin to land, and the caller's own line is already good.
#define PERSONA_REPLY_TIMEOUT_SECS  20

// Ceiling on what we hand back to the channel. method_send takes
// METHOD_TEXT_SZ; a model that ignores the style clause and writes an
// essay gets cut rather than dropped.
#define PERSONA_REPLY_LINE_SZ  512

typedef struct
{
  // Teardown accounting. Linked at allocation, unlinked on the one free
  // path — core's quiescence barrier can see neither side of an LLM
  // request (hold.h), so this is what makes a /plugin reload chat safe
  // while a flourish is on the wire.
  chatbot_hold_t   hold;

  // Where the answer goes once cmd_ctx_t has died. By NAME, re-resolved
  // at delivery — see chatbot.h §verb_reply_to_t.
  verb_reply_to_t  to;

  // The caller's own rendering. This is the whole reason the slot can
  // promise exactly one reply: every path that cannot produce a better
  // line sends this one instead.
  char             plain[PERSONA_REPLY_LINE_SZ];
} persona_req_t;

// The one delivery path. A persona body teaches "/me ", so the model
// writes emotes here exactly as it does in conversation — and this slot
// hands its line to method_send directly rather than through the reply
// pipeline, which is where the framing used to be applied. Untranslated
// it reached the channel as the literal four characters.
static void
persona_req_send(persona_req_t *r, const char *line)
{
  method_inst_t *inst = method_find(r->to.inst_name);
  size_t         emote_off;

  // The instance the command was run on may have been reloaded away
  // while the model was thinking; there is then nowhere to answer, and
  // no second address to guess at (OBS-29).
  if(inst != NULL)
  {
    emote_off = chatbot_emote_prefix_len(line);

    if(emote_off != 0)
      method_send_emote(inst, verb_reply_to_addr(&r->to),
          line + emote_off);

    else
      method_send(inst, verb_reply_to_addr(&r->to), line);
  }

  chatbot_hold_unlink(&r->hold);
  mem_free(r);
}

// Trim to one line. A model that opens with a newline-separated
// preamble gets its first non-empty line taken rather than the lot:
// what reaches the channel must look like something the bot said.
static void
persona_first_line(const char *src, char *out, size_t out_sz)
{
  size_t n;

  while(*src == '\n' || *src == '\r' || *src == ' ' || *src == '\t')
    src++;

  strlcpy(out, src, out_sz);
  n = strcspn(out, "\r\n");
  out[n] = '\0';

  while(n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
    out[--n] = '\0';
}

static void
persona_done(const llm_chat_response_t *resp)
{
  persona_req_t *r = resp->user_data;
  char           line[PERSONA_REPLY_LINE_SZ];

  // A teardown claimed this record while the request was airborne. The
  // bot it was speaking for is gone, so there is nobody to answer and
  // nothing of the owner's left to touch (hold.h).
  if(chatbot_hold_disowned(&r->hold))
  {
    chatbot_hold_unlink(&r->hold);
    mem_free(r);
    return;
  }

  if(!resp->ok || resp->content == NULL)
  {
    clam(CLAM_INFO, PERSONA_REPLY_CTX,
        "model declined (http=%ld%s%s) — sending the caller's line",
        resp->http_status, resp->error != NULL ? ": " : "",
        resp->error != NULL ? resp->error : "");
    persona_req_send(r, r->plain);
    return;
  }

  persona_first_line(resp->content, line, sizeof(line));

  // An empty answer is not a line. Everything else the model produced
  // is its own business — this is a toy's flourish, not a contract.
  persona_req_send(r, line[0] != '\0' ? line : r->plain);
}

// The bot_driver_t.persona_reply slot. Declining is the common case and
// costs the caller nothing: it replies for itself with the line it
// already had. Speaking is the promise — from here, exactly one reply
// goes out on every path, including a model that never answers well.
bool
chatbot_persona_reply(void *handle, const cmd_ctx_t *ctx,
    const char *instruction, const char *plain)
{
  chatbot_state_t       *st = handle;
  chatbot_personality_t  p  = {0};
  const char            *botname;
  const char            *model;
  persona_req_t         *r;
  llm_message_t          messages[2];
  llm_chat_params_t      params;
  char                   pname[CHATBOT_PERSONALITY_NAME_SZ];
  char                   user[PERSONA_REPLY_LINE_SZ * 2];
  char                  *system;
  size_t                 system_sz;
  bool                   launched;

  if(st == NULL || ctx == NULL || ctx->msg == NULL)
    return(false);

  // Somebody is already capturing this command's output to voice it --
  // the NL bridge, or the deferred spine (interpret.c). That is the
  // PROSE register and it owns its own persona pass, so answering here
  // would both bypass the collector, which only cmd_reply feeds, and
  // arrive beside the "(no output)" cue the empty capture settles into.
  // Declining hands the line back to cmd_reply, which is what the
  // capture is waiting for. This knob is for the BANGED register.
  if(ctx->msg->reply_sink_id != 0)
    return(false);

  botname = bot_inst_name(st->inst);

  // The conversational half is this bot's whole permission to speak in
  // voice. A command bot with it off (botman, pacman) answers as the
  // tool, which is what the operator configured it to be.
  if(kv_get_bot_uint(botname, "behavior.chat.enabled") == 0)
    return(false);

  // Active persona first, then the bot's configured one, then the
  // plugin default — the resolution chatbot_reply_submit uses, and for
  // the same reason: the active name is empty until the bot has spoken.
  pthread_rwlock_rdlock(&st->lock);
  strlcpy(pname, st->active_name, sizeof pname);
  pthread_rwlock_unlock(&st->lock);

  if(pname[0] == '\0')
  {
    const char *kv_name = kv_get_bot_str(botname, "behavior.personality");

    if(kv_name == NULL || kv_name[0] == '\0')
      kv_name = kv_get_str("plugin.chat.default_personality");

    if(kv_name != NULL)
      strlcpy(pname, kv_name, sizeof pname);
  }

  if(pname[0] == '\0')
    return(false);

  model = kv_get_bot_str(botname, "chat_model");

  if(model == NULL || model[0] == '\0')
    model = kv_get_str("llm.default_chat_model");

  if(model == NULL || model[0] == '\0')
  {
    clam(CLAM_WARN, PERSONA_REPLY_CTX,
        "bot=%s has no chat model bound", botname);
    return(false);
  }

  // Fresh read per call, as the reply pipeline does: a persona edit
  // takes effect on the next flourish without a bot restart.
  if(chatbot_personality_read(pname, &p) != SUCCESS)
  {
    clam(CLAM_WARN, PERSONA_REPLY_CTX,
        "bot=%s personality '%s' failed to load", botname, pname);
    return(false);
  }

  // A persona body has no fixed ceiling — it is whatever the operator
  // wrote in the file — so the prompt is sized to the one in hand.
  // llm_chat_submit copies every string it is given, so this lives only
  // until the submit returns.
  system_sz = (p.body != NULL ? strlen(p.body) : 0)
      + sizeof(PERSONA_REPLY_STYLE);
  system    = mem_alloc("chatbot", "persona_prompt", system_sz);

  snprintf(system, system_sz, "%s%s",
      p.body != NULL ? p.body : "", PERSONA_REPLY_STYLE);
  chatbot_personality_free(&p);

  // The caller's rendering goes in labelled as the tool's output, not
  // as a line to improve. Handed over unlabelled a model treats it as a
  // draft and hands back a paraphrase — measured on the first live
  // gate, one flip in three came back wearing the toy's own joke.
  snprintf(user, sizeof(user),
      "%s\n\nThe command's own output, for the facts in it:\n%s",
      instruction != NULL ? instruction : "Say this in your own words.",
      plain);

  r = mem_alloc("chatbot", "persona_reply", sizeof(*r));
  memset(r, 0, sizeof(*r));

  // Before the first path that can fail: from here every exit runs
  // through the unlink.
  chatbot_hold_link(&r->hold, st, r);
  verb_reply_to_snapshot(&r->to, ctx);
  strlcpy(r->plain, plain, sizeof r->plain);

  memset(messages, 0, sizeof(messages));
  messages[0].role    = LLM_ROLE_SYSTEM;
  messages[0].content = system;
  messages[1].role    = LLM_ROLE_USER;
  messages[1].content = user;

  memset(&params, 0, sizeof(params));
  params.max_tokens   = PERSONA_REPLY_MAX_TOKENS;
  params.timeout_secs = PERSONA_REPLY_TIMEOUT_SECS;

  launched = (llm_chat_submit(model, &params, messages, 2, persona_done,
        NULL, r) == SUCCESS);
  mem_free(system);

  if(!launched)
  {
    // Nothing is airborne, so nothing will call back. Speaking here
    // rather than returning false keeps the promise the true return
    // would have made — but we have not made it yet, so hand the
    // obligation back instead and let the caller use its own reply
    // path, which is cheaper and already correct.
    clam(CLAM_WARN, PERSONA_REPLY_CTX,
        "bot=%s llm_chat_submit failed (model='%s')", botname, model);
    chatbot_hold_unlink(&r->hold);
    mem_free(r);
    return(false);
  }

  return(true);
}
