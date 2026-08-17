#ifndef BM_CHAT_IDENTITY_H
#define BM_CHAT_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "method.h"

// Chat-plugin identity scoring
//
// The chat plugin is the sole arbiter of "are these two protocol-level
// identities the same human?" — it owns the scoring rules. Method
// plugins emit a generic four-field identity tuple per inbound message
// (nickname, username, hostname, verified_id on method_msg_t) and chat
// compares those tuples uniformly across protocols. There is no
// per-method-kind registry: the same scorer runs for IRC, Matrix,
// Slack, Discord, etc., and it interprets the empty/non-empty pattern
// of fields as the protocol's expressed identity strength.
//
// Forward declaration of dossier_sig_t to avoid pulling in dossier.h
// from headers that only need to call the scorer or the dossier
// resolver. The full struct layout lives in dossier.h.
struct dossier_sig;

// The four fields, held as one thing.
//
// Six structs in this plugin carry a person across an async hop — the
// coalescer's slot, a reply request, the weather watcher, a price-watch
// row, the NL observer's task data and the vision fetch's context — and
// each one used to declare the four fields itself and copy them one
// snprintf at a time. Twice that copy was written with fields missing,
// and both times the symptom was silence rather than an error:
// chat_user_dossier_id() refuses an all-empty tuple by design, so the
// person resolved to dossier 0 and the reply lost every fact, mention
// row and semantic recall it should have had, while the per-line logger
// went on resolving the same speaker correctly (dcfc359, the coalescer;
// CHAT-NLOBSERVE-1, the observer, which never wrote a fact in its life).
// A seventh carrier was found still missing them (OBS-8, vision).
//
// One field, one take, one apply: a carrier cannot hold three quarters
// of a person any more.
typedef struct
{
  char nickname   [METHOD_NICKNAME_SZ];
  char username   [METHOD_USERNAME_SZ];
  char hostname   [METHOD_HOSTNAME_SZ];
  char verified_id[METHOD_VERIFIED_ID_SZ];
} chat_identity_t;

// Snapshot the tuple off an inbound message, and lay it back onto a
// synthetic one. `msg` is borrowed by both.
void chat_identity_take(chat_identity_t *id, const method_msg_t *msg);
void chat_identity_apply(const chat_identity_t *id, method_msg_t *msg);

// Pair scorer. Returns confidence in [0.0, 1.0] that the two
// signatures denote the same human:
//
//   - both verified_ids non-empty + match  -> 1.0  (server-attested)
//   - both verified_ids non-empty + differ -> 0.0  (definitive non-match;
//                                                   server guarantees
//                                                   separation)
//   - otherwise: similarity over (nickname, username, hostname).
//     Exact normalized-username match dominates; matching nickname
//     prefixes contribute; matching hostnames are a tiebreaker.
//
// Both arguments are borrowed; the scorer retains no references.
float chat_identity_score(const struct dossier_sig *a,
    const struct dossier_sig *b);

// Token-vs-signature scorer used for mention resolution. token is a
// single alphanumeric word from chat text (length >= 3, stoplist
// pre-filtered). Returns confidence in [0.0, 1.0] that the token
// refers to the dossier identified by `sig`. Token matching prefers
// exact nickname, then nickname prefix, then nickname substring.
float chat_identity_token_score(const char *token,
    const struct dossier_sig *sig);

// Consolidated sender -> dossier resolver. Builds a dossier_sig_t from
// the message's quad-tuple fields and calls dossier_resolve internally.
// Returns 0 on any failure or when no match clears the threshold and
// create_if_missing is false.
//
// Return type is int64_t rather than the chat-plugin-local
// dossier_id_t so this header is includable by method and service
// plugins without pulling in dossier.h. dossier_id_t is a typedef for
// int64_t on the chat-plugin side.
int64_t chat_user_dossier_id(const method_msg_t *msg,
    uint32_t ns_id, const char *display_label, bool create_if_missing);

#endif // BM_CHAT_IDENTITY_H
