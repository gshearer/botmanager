// dispatch.h — command-dispatch half of the chat bot plugin
//
// The `text` method fuses two halves into one bot driver: this one,
// which always interprets inbound lines as potential commands, and the
// conversational (NL/LLM) half in chatbot.c, which speaks only when the
// per-bot `behavior.chat.enabled` toggle is on.
//
// Layering (PTREE-6): this TU is the plugin's downward-facing half. It
// speaks to the core bot/cmd/method/userns interfaces and NEVER calls
// into the NL half. The NL half reaches commands only through the core
// cmd registry (cmd_permits + cmd_dispatch_resolved), never into here.
// Keep that direction — it is the in-module image of PLUGIN.md Rule 2.

#ifndef BM_TEXT_DISPATCH_H
#define BM_TEXT_DISPATCH_H

#include "bot.h"
#include "cmd.h"
#include "method.h"

#include <stdbool.h>

// Register / unregister the identity + authentication command surface
// (identify, deauth, register, id). Driven by the text plugin's
// init / deinit in chatbot.c; registration is process-wide, not per-bot.
bool text_dispatch_register(void);
void text_dispatch_unregister(void);

// Per-line identity bookkeeping: user discovery from method metadata,
// MFA refresh of live sessions, and autoidentify. Runs for every inbound
// line whether it turns out to be a command or conversation — it is the
// authentication substrate both halves rest on.
void text_identity_observe(bot_inst_t *inst, const method_msg_t *msg);

// Command-shaped lines belong to the command half, full stop. Returns
// true when `msg` opened with the bot's command prefix — dispatched,
// denied, or an unknown verb — meaning the caller must NOT hand the line
// to the conversational half. Returns false for ordinary talk.
bool text_dispatch_message(bot_inst_t *inst, const method_msg_t *msg);

// Emit the command-half rows of the `/show bot <name>` summary. Composed
// with the conversational rows by the unified :default verb in
// show_verbs.c, which owns the heading and the row ordering.
void text_dispatch_summary(const cmd_ctx_t *ctx, bot_inst_t *bot);

#ifdef TEXT_DISPATCH_INTERNAL

#include "clam.h"
#include "colors.h"
#include "common.h"
#include "kv.h"
#include "plugin.h"
#include "userns.h"

// Argument descriptors for the authentication commands.
static const cmd_arg_desc_t text_ad_identify[] = {
  { "username", CMD_ARG_ALNUM, CMD_ARG_REQUIRED,              USERNS_USER_SZ, NULL },
  { "password", CMD_ARG_NONE,  CMD_ARG_REQUIRED | CMD_ARG_REST, 0,            NULL },
};

static const cmd_arg_desc_t text_ad_register[] = {
  { "password", CMD_ARG_NONE, CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

static const cmd_arg_desc_t text_ad_id[] = {
  { "nickname", CMD_ARG_ALNUM, CMD_ARG_OPTIONAL, METHOD_SENDER_SZ, NULL },
};

#endif // TEXT_DISPATCH_INTERNAL

#endif // BM_TEXT_DISPATCH_H
