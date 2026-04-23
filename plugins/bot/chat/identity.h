#ifndef BM_CHAT_IDENTITY_H
#define BM_CHAT_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "contracts/dossier_method.h"
#include "method.h"

// Chat-plugin-owned identity registry. Method plugins push a
// (signer, scorer, token_scorer) triple per method_kind into the chat
// plugin at their own plugin_start time via
// plugin_dlsym("chat", "chat_identity_register"); the chat plugin
// replaces the legacy in-core method_driver_t hooks with this
// per-method-kind registry. A method plugin that does not participate
// in dossier identity simply omits the registration.
//
// The scorer typedefs use dossier_method_sig_t (see contracts/
// dossier_method.h) so this header is includable by any method plugin
// without pulling in chat-plugin-private types. The chat plugin's
// internal dossier_sig_t is layout-compatible and is reinterpret-cast
// at the in-chat scorer call site.

// Build a method-specific identity signature from an inbound message.
// Fills out_json with a compact JSON object (a few hundred bytes at
// most) and returns SUCCESS; on failure (unparseable metadata, missing
// fields, buffer too small) returns FAIL and leaves out_json empty.
typedef bool (*chat_identity_signer_t)(const method_msg_t *msg,
    char *out_json, size_t out_sz);

// Pair-scorer: compares two signatures of the same method_kind and
// returns confidence in [0.0, 1.0]. Callers own both arguments; scorers
// must not retain references beyond return. Alias for the contract
// typedef so method plugins can register their existing scorer
// functions without an extra function-pointer cast.
typedef dossier_method_scorer_t       chat_identity_scorer_t;

// Token-vs-signature scorer for mention resolution. token is a single
// alphanumeric word (length >= 3, stoplist pre-filtered). Returns
// confidence in [0.0, 1.0].
typedef dossier_method_token_scorer_t chat_identity_token_scorer_t;

// Register identity hooks for a method_kind. Any of signer / scorer /
// token_scorer may be NULL (the corresponding feature is disabled for
// that kind). Subsequent calls for the same method_kind replace the
// earlier triple. Returns FAIL on bad arguments or a full registry.
bool chat_identity_register(const char *method_kind,
    chat_identity_signer_t       signer,
    chat_identity_scorer_t       scorer,
    chat_identity_token_scorer_t token_scorer);

// Remove all identity hooks for a method_kind. No-op if absent.
void chat_identity_unregister(const char *method_kind);

// Build a signature JSON for an inbound message via the registered
// signer for msg->inst's method_kind. Returns FAIL (leaves out_json
// empty) when no signer is registered or the signer itself returns
// FAIL.
bool chat_identity_signature_build(const method_msg_t *msg,
    char *out_json, size_t out_sz);

// Consolidated sender -> dossier resolver exported for sponsor plugins
// that need to attach facts to the inbound speaker's dossier without
// knowing how identity signatures are constructed or how the dossier
// is looked up. Builds the signature via chat_identity_signature_build
// and calls dossier_resolve internally. Returns 0 on any failure or
// when no match clears the threshold and create_if_missing is false.
//
// Return type is int64_t rather than the chat-plugin-local
// dossier_id_t so this header is includable by method and service
// plugins without pulling in dossier.h. dossier_id_t is itself a
// typedef for int64_t on the chat-plugin side.
int64_t chat_user_dossier_id(const method_msg_t *msg,
    uint32_t ns_id, const char *display_label, bool create_if_missing);

// Internal accessors used by the chat plugin's dossier subsystem. Not
// part of the cross-plugin surface; safe to call from chat-local
// translation units.
chat_identity_scorer_t       chat_identity_scorer_lookup(
    const char *method_kind);
chat_identity_token_scorer_t chat_identity_token_scorer_lookup(
    const char *method_kind);

// Lifecycle. chat_identity_init must run before any method plugin's
// start() fires; chat_identity_exit tears the registry down at
// chatbot plugin deinit.
void chat_identity_init(void);
void chat_identity_exit(void);

#endif // BM_CHAT_IDENTITY_H
