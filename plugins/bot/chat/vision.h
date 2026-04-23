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

// Lifecycle. Mirror the existing state init/destroy pattern.
void chatbot_vision_state_init(chatbot_state_t *st);
void chatbot_vision_state_destroy(chatbot_state_t *st);

#endif // BM_CHATBOT_VISION_H
