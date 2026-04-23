#ifndef BM_IRC_DOSSIER_H
#define BM_IRC_DOSSIER_H

#include <stdbool.h>
#include <stddef.h>

#include "contracts/dossier_method.h"

// IRC identity signature and scorer
//
// Three functions split into their own translation unit so the unit
// tests can link them directly without pulling in the whole IRC method
// plugin (which links as a .so). irc.c registers the triple with the
// chat plugin's identity registry at plugin_start via
// plugin_dlsym("chat", "chat_identity_register"). The IRC plugin does
// not reach into the chat plugin's dossier subsystem directly; the
// scorer signatures use dossier_method_sig_t from the contract header,
// which is layout-compatible with the chat-plugin-local dossier_sig_t.
//
// Signature JSON shape:
//
//   {"nick":"<nick>","ident":"<ident>",
//    "host_tail":"<tail>","cloak":"<account>"}
//
// host_tail is the stable network-identifying slice of the host:
//   - hostname:     last two dotted labels
//                   ("dyn-159.google-fiber.net" -> "google-fiber.net")
//   - IPv4 literal: first two octets
//                   ("192.168.1.100"            -> "192.168")
//   - IPv6 literal: first two 16-bit groups, canonical short form
//                   ("2001:db8:85a3::1"         -> "2001:db8")
// The asymmetry is deliberate: hostnames encode the ISP on the right
// (TLD side), IP literals encode the network on the left. Stored for
// debugging; the scorer does not use it.
//
// cloak is the account portion of a Libera/OFTC "user/<name>" cloak
// when present, empty otherwise. Server-enforced per SASL-registered
// account, so when both sides carry a cloak the scorer treats it as
// authoritative.
//
// Scorer rules:
//
//   - both sides cloaked, same account                 -> 1.0
//   - both sides cloaked, different accounts           -> 0.0
//                                                        (server
//                                                        guarantees
//                                                        separation)
//   - otherwise: normalize ident (strip leading '~',
//     lowercase). If normalized idents match AND nicks
//     share a case-insensitive alphanumeric prefix
//     of >= 3 chars                                    -> 0.9
//   - normalized idents match, shorter nick prefix     -> 0.4
//   - anything else                                    -> 0.0
//
// Limitation: two hosts that share a nick but have different
// (normalized) idents are kept as separate dossiers. The operator can
// collapse them with /dossier merge when duplicates are spotted.

// Build a signature JSON payload from an IRC prefix string of the form
// "nick!ident@host". NUL-terminates out on success; on failure leaves
// out[0] = '\0'.
//
// returns: SUCCESS or FAIL (malformed prefix, buffer too small, or any
//          field contains a character that would require JSON escaping)
// prefix: raw "nick!ident@host" string
// out:    destination buffer
// out_sz: capacity of out
bool irc_dossier_build_signature(const char *prefix,
    char *out, size_t out_sz);

float irc_identity_score(const dossier_method_sig_t *a,
    const dossier_method_sig_t *b);

float irc_identity_token_score(const char *token,
    const dossier_method_sig_t *sig);

#endif // BM_IRC_DOSSIER_H
