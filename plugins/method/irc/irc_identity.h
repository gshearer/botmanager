#ifndef BM_IRC_IDENTITY_H
#define BM_IRC_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

// IRC identity helpers
//
// The IRC plugin's job at the identity layer is purely structural:
// take a raw IRC prefix ("nick!ident@host") and project it onto the
// generic four-field identity tuple defined in include/method.h
// (nickname, username, hostname, verified_id). The chat plugin owns
// every interpretive decision (matching thresholds, scoring rules,
// dossier merging) — IRC just hands it normalized fields.
//
// Field projection:
//
//   nickname    <- nick verbatim (current display label, may change)
//   username    <- ident with a single leading '~' stripped, lowercased
//                  (the legacy identd "unverified" marker is ignored;
//                  most networks treat it as cosmetic since the 2000s)
//   hostname    <- host_tail: the stable network-identifying slice of
//                  the host. Hostnames keep their last two dotted labels
//                  ("dyn-159.google-fiber.net" -> "google-fiber.net");
//                  IPv4 literals keep their first two octets
//                  ("192.168.1.100" -> "192.168"); IPv6 literals keep
//                  their first two 16-bit groups, canonical short form
//                  ("2001:db8:85a3::1" -> "2001:db8"). The asymmetry is
//                  deliberate: hostnames carry the ISP/network on the
//                  right, IP literals carry it on the left.
//   verified_id <- when host has a Libera/OFTC user-account cloak
//                  ("user/<X>"), the account portion. Empty otherwise.
//                  Server-attested per SASL-registered account, so
//                  consumers can treat a populated verified_id as
//                  authoritative.
//
// All four output buffers are NUL-terminated on return. On parse failure
// (malformed prefix, JSON-unsafe bytes in any field, or any output
// buffer too small to hold the projected value) every output is reset
// to "" and the function returns FAIL.

bool irc_identity_fill_quad(const char *prefix,
    char *out_nickname,    size_t out_nickname_sz,
    char *out_username,    size_t out_username_sz,
    char *out_hostname,    size_t out_hostname_sz,
    char *out_verified_id, size_t out_verified_id_sz);

#endif // BM_IRC_IDENTITY_H
