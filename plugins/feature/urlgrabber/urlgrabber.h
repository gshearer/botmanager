#ifndef BM_URLGRABBER_H
#define BM_URLGRABBER_H

// botmanager — MIT
// urlgrabber: watches the channels that bots already sit in for URLs and,
// the moment it spots one, fetches the page and echoes its <title> back to
// the room.
//
// It is a pure feature — no bot driver, no bot of its own. On load it
// *augments* every running bot by adding its own observer to that bot's
// method stream (and it re-attaches to any bot that starts afterwards, via
// the `bot_start` event). A URL is chased only if the specific channel has
// been opted in; everything is silent otherwise.
//
// Configuration rides the standard `!set kv` / `!show kv` framework — no
// bespoke commands. Two knobs live under each bot's own per-channel
// namespace, auto-registered (both, together) as a channel is discovered:
//
//   bot.<bot>.<kind>.chan.<channel>.urlgrabber.enabled   (bool,   default false)
//   bot.<bot>.<kind>.chan.<channel>.urlgrabber.holddown  (uint32, default 5)
//
//   * enabled  — off by default; nothing is fetched until an op opts the
//                channel in. Watching a channel is never implicit.
//   * holddown — minimum gap, in seconds, between fetches in that channel.
//                It blunts a bad actor who floods URLs to make us hammer
//                some third party on their behalf.
//
// e.g.  !set kv  bot.botman.irc.chan.cabal.urlgrabber.enabled  true
//       !set kv  bot.botman.irc.chan.cabal.urlgrabber.holddown 5
//       !show kv bot.botman.irc.chan.cabal
//
// Successor to the old standalone quotebot's urlgrabber, rebuilt around the
// async curl core, the bot/method abstraction, and the KV framework.

#ifdef URLGRABBER_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "method.h"
#include "plugin.h"

#include <stddef.h>
#include <stdint.h>

// CLAM context (registered in CLAM.md).
#define UG_CTX               "urlgrabber"

// The offer contract, both halves of which are documented in CLAM.md.
//
// urlgrabber sees a bot's *inbound* stream, so a URL the bot itself prints
// is invisible to it. A plugin that emits one — searxng's search results
// are the first — offers it for consideration by logging a single line:
//
//   clam(CLAM_INFO, "url_offer", "<bot> <method> <channel> <url>");
//
// clam is the bus rather than a function call because the layer rule
// (PLUGIN.md §Layer Rules) forbids an extension from depending on a
// feature: searxng may not call into urlgrabber, and a log line couples
// neither side to the other. An offer is a *candidate*, not an order — it
// passes exactly the gate a human-pasted URL passes, so a channel that
// never opted in stays silent and a .jpg is still filtered out.
//
// Note the payload must fit CLAM_MSG_SZ, which clam truncates silently;
// an offerer is responsible for not offering a URL that would be cut.
#define UG_OFFER_CTX         "url_offer"

// Plugin-level KV keys — global operational knobs.
#define UG_KV_TIMEOUT        "plugin.urlgrabber.timeout_secs"
#define UG_KV_MAX_TITLE      "plugin.urlgrabber.max_title_len"
#define UG_KV_MAX_BYTES      "plugin.urlgrabber.max_fetch_bytes"
#define UG_KV_USER_AGENT     "plugin.urlgrabber.user_agent"

// The identity worn for the one retry a JavaScript challenge earns. Some
// sites hand a browser a challenge shell instead of a page, and serve the
// real markup only to a named link-preview crawler. Set it to "none"/"off"
// to decline the retry — a challenge shell is then dropped rather than
// announced, which is still better than repeating the site's own name.
#define UG_KV_CRAWLER_AGENT  "plugin.urlgrabber.crawler_agent"

// Per-channel knob suffixes. The keys hang off each bot's own channel
// namespace (bot.<bot>.<kind>.chan.<channel>.urlgrabber.<suffix>) so they
// sit beside the channel's other settings in `!show kv`.
#define UG_SUFFIX_ENABLED    "enabled"
#define UG_SUFFIX_HOLDDOWN   "holddown"

// Default hold-down, in seconds — the registered default for a channel's
// holddown key, and the fallback used before that key exists.
#define UG_DEFAULT_HOLDDOWN  5

// Headers a browser sends that any HTTP client may also honestly send —
// the leading slice of the navigation set. The rest is Fetch Metadata, an
// account of a click, and so belongs only to the browser identity.
#define UG_NAV_GENERIC       2

// How far into a body the challenge-shell markers are looked for. A shell
// is a stub by construction, so this bounds the scan and doubles as the
// false-positive guard against a real document that merely quotes a marker.
#define UG_CHALLENGE_SCAN_SZ 32768

// Storage bounds.
#define UG_URL_SZ            2048
#define UG_TITLE_SZ          1024
#define UG_HOST_SZ           256

// ---- ug_fetch.c ---------------------------------------------------- //

// Scan `text` for the first grabbable URL, copying a cleaned copy into
// `out` (NUL-terminated, capped at `cap`). Returns false when there is no
// URL, or the sole candidate is filtered out (non-web scheme, a binary
// media extension, or a private/loopback host we refuse to touch).
bool ug_find_url(const char *text, char *out, size_t cap);

// Fire an async GET for `url`; on a successful HTML response, announce the
// tidied page title to (method_name, channel). Every argument is copied —
// the call returns immediately and owns nothing of the caller's.
void ug_fetch(const char *method_name, const char *channel, const char *url);

// Extract and tidy the document <title> from an HTML body into `out`:
// decodes the common named/numeric entities and collapses runs of
// whitespace to single spaces. Returns false when no non-empty title is
// present in the (possibly truncated) body.
bool ug_extract_title(const char *html, size_t len, char *out, size_t cap);

#endif // URLGRABBER_INTERNAL

#endif // BM_URLGRABBER_H
