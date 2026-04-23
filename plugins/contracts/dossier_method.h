#ifndef BM_CONTRACTS_DOSSIER_METHOD_H
#define BM_CONTRACTS_DOSSIER_METHOD_H

#include <stdbool.h>

// Contract: method plugins expose scorer functions that compare identity
// signatures (for sender resolution) and match single tokens against
// signatures (for mention resolution). The chat plugin reads these via
// method_driver_t.dossier_scorer / .dossier_token_scorer and uses them
// to drive its local dossier subsystem. Method plugins that do not
// participate in dossier matching leave both fields NULL.
//
// The scorer types on method_driver_t itself are intentionally
// void*-based so core (include/method.h) stays free of any chat-plugin
// dependency. Implementers cast from their real scorer signature at the
// driver literal; the consumer (chat plugin's dossier module) casts
// back to dossier_method_sig_t at the call site. All cast sites live in
// two places: the method plugin's driver literal and the chat plugin's
// scorer dispatch.

// Signature pair used by both scorer flavours. method_kind names the
// owning method plugin ("irc", "slack", ...). sig_json is a method-
// defined JSON payload. Both fields are borrowed references valid for
// the duration of the scorer call.
typedef struct
{
  const char *method_kind;
  const char *sig_json;
} dossier_method_sig_t;

// Signature-vs-signature scorer. Both sides share method_kind by
// construction. Returns confidence in [0.0, 1.0]; 0 means "no match".
// The caller owns both arguments.
typedef float (*dossier_method_scorer_t)(
    const dossier_method_sig_t *a,
    const dossier_method_sig_t *b);

// Token-vs-signature scorer used for mention resolution. token is a
// single alphanumeric word extracted from chat text (length >= 3,
// stoplist already filtered). Returns confidence in [0.0, 1.0].
typedef float (*dossier_method_token_scorer_t)(
    const char *token,
    const dossier_method_sig_t *sig);

#endif // BM_CONTRACTS_DOSSIER_METHOD_H
