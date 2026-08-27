#ifndef BM_CHATBOT_VISION_H
#define BM_CHATBOT_VISION_H

#ifndef CHATBOT_INTERNAL
#error "vision.h is internal to the chat bot plugin (define CHATBOT_INTERNAL before inclusion)"
#endif

#include "chatbot.h"
#include "method.h"

// Called from chatbot_consider_speaking, after the mute gate and
// before chatbot_speak_decide. Returns true if the vision path took
// ownership of this message (caller must NOT run speak-policy).
bool chatbot_vision_maybe_submit(chatbot_state_t *st,
    const method_msg_t *msg);

// Declare the per-channel image_vision knobs for every channel this bot
// is already configured for, so they are settable the instant the bot
// starts. The gate above registers a channel it has not seen, which is
// what covers one joined later; this is the head start, not the only
// trigger. Safe to call repeatedly.
void chatbot_vision_register_channels(chatbot_state_t *st);

// Lifecycle. Mirror the existing state init/destroy pattern.
void chatbot_vision_state_init(chatbot_state_t *st);
void chatbot_vision_state_destroy(chatbot_state_t *st);

#endif // BM_CHATBOT_VISION_H
