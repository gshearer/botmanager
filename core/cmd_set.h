#ifndef BM_CMD_SET_H
#define BM_CMD_SET_H

// cmd_set_register() is declared with the rest of the command registry
// in include/cmd.h. What lives here is the part core/cmd_set.c keeps to
// itself: the argument parse, exposed so tests/test_set_bot.c can drive
// it without a daemon, a bot subsystem or a KV store.

#ifdef CMD_SET_INTERNAL

#include "bot.h"
#include "kv.h"
#include "plugin.h"

#include <stdbool.h>

// Which of /set's three actions a command line asked for. ASSIGN is 0,
// so a two-state test separates "a plain write" from "a flag was given"
// and loses only which flag — never reporting a write as a flag.
typedef enum
{
  SET_VERB_ASSIGN = 0,   // <key> <value>
  SET_VERB_CLEAR,        // --clear <key>
  SET_VERB_DELETE,       // --delete <key>
} set_verb_t;

// How `set bot`'s argument blob parsed. Each failure obliges the caller
// to say something different, which is why they are named rather than
// folded into one value.
typedef enum
{
  SET_BOT_OK = 0,
  SET_BOT_USAGE,          // the shape is wrong; print the usage line
  SET_BOT_NO_METHOD,      // .kind names no method this bot has bound
  SET_BOT_KEY_TOO_LONG,   // the composed key overruns KV_KEY_SZ
} set_bot_rc_t;

// bot_has_method_kind()'s signature, taken as a parameter so the parse
// stays a pure function of its arguments.
typedef bool (*set_bot_has_kind_fn)(const bot_inst_t *, const char *);

typedef struct
{
  set_verb_t  verb;
  char        key[KV_KEY_SZ];        // composed bot.<bot>[.<kind>].<suffix>
  char        kind[PLUGIN_NAME_SZ];  // the offending token on NO_METHOD, else ""
  const char *value;                 // borrows `rest`; NULL unless ASSIGN
} set_bot_parse_t;

// Decide `set bot`'s form and compose its key. `rest` is the CMD_ARG_REST
// blob following <bot>; out->value points into it and dies with it.
set_bot_rc_t set_bot_parse(const char *botname, const char *rest,
    set_bot_has_kind_fn has_kind, const bot_inst_t *bot,
    set_bot_parse_t *out);

#endif // CMD_SET_INTERNAL
#endif // BM_CMD_SET_H
