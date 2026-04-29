// botmanager — MIT
// Chat-plugin generic identity scorer. Replaces the per-method-kind
// scorer registry — every protocol's quad-tuple now scores through
// the same code path. The verified_id field short-circuits when the
// protocol attests an identity; otherwise we fall back to similarity
// over (nickname, username, hostname).

#include "dossier.h"
#include "identity.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "common.h"

// Case-insensitive prefix-match length in alphanumeric chars only.
// Stops at the first non-alnum byte or the first mismatching byte.
static size_t
ci_alnum_prefix(const char *a, const char *b)
{
  size_t i = 0;

  while(a[i] != '\0' && b[i] != '\0'
      && tolower((unsigned char)a[i]) == tolower((unsigned char)b[i])
      && isalnum((unsigned char)a[i]))
    i++;

  return(i);
}

// Case-insensitive substring search in ASCII; returns match length when
// `needle` is found in `hay`, 0 otherwise.
static size_t
ci_substr(const char *hay, const char *needle)
{
  size_t nlen = strlen(needle);

  if(nlen == 0) return(0);

  for(const char *h = hay; *h != '\0'; h++)
  {
    size_t i = 0;
    while(i < nlen && h[i] != '\0'
        && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
      i++;
    if(i == nlen) return(nlen);
  }

  return(0);
}

static bool
str_nonempty(const char *s)
{
  return(s != NULL && s[0] != '\0');
}

float
chat_identity_score(const dossier_sig_t *a, const dossier_sig_t *b)
{
  bool both_verified;
  bool same_username;

  if(a == NULL || b == NULL)
    return(0.0f);

  // Verified-id short-circuit. When both sides carry a server-attested
  // unforgeable handle (IRC SASL account, Slack user_id, Matrix MXID,
  // ...), that's authoritative — equal means same human, unequal means
  // definitively different humans (the server enforces the separation).
  both_verified = str_nonempty(a->verified_id) && str_nonempty(b->verified_id);

  if(both_verified)
  {
    if(strcasecmp(a->verified_id, b->verified_id) == 0)
      return(1.0f);

    return(0.0f);
  }

  // Similarity fallback. The username field is the strongest non-
  // verified signal — it's the client-claimed login (IRC's normalized
  // ident, etc.) and is harder to spoof than a display nickname.
  same_username =
      str_nonempty(a->username) && str_nonempty(b->username)
      && strcasecmp(a->username, b->username) == 0;

  if(same_username)
  {
    // Username match plus a >=3-char nickname prefix is high confidence.
    if(str_nonempty(a->nickname) && str_nonempty(b->nickname)
        && ci_alnum_prefix(a->nickname, b->nickname) >= 3)
      return(0.9f);

    // Username match alone — moderate confidence. Guards against false
    // merges when several humans share an unconfigured default username
    // (e.g., the OS account on a misconfigured client).
    return(0.4f);
  }

  return(0.0f);
}

float
chat_identity_token_score(const char *token, const dossier_sig_t *sig)
{
  size_t nlen;
  size_t tlen;
  size_t pref;

  if(token == NULL || sig == NULL || token[0] == '\0')
    return(0.0f);

  if(!str_nonempty(sig->nickname))
    return(0.0f);

  tlen = strlen(token);
  nlen = strlen(sig->nickname);

  // Exact nickname match (case-insensitive).
  if(tlen == nlen && strcasecmp(token, sig->nickname) == 0)
    return(0.95f);

  // Long-enough alphanumeric prefix match.
  pref = ci_alnum_prefix(token, sig->nickname);
  if(pref >= 3 && tlen >= 3 && nlen >= 3)
    return(0.8f);

  // Substring match (>= 4 chars to avoid noise).
  if(ci_substr(sig->nickname, token) >= 4)
    return(0.5f);

  return(0.0f);
}

int64_t
chat_user_dossier_id(const method_msg_t *msg, uint32_t ns_id,
    const char *display_label, bool create_if_missing)
{
  dossier_sig_t  sig;
  const char    *method_kind;

  if(msg == NULL || msg->inst == NULL)
    return(0);

  method_kind = method_inst_kind(msg->inst);
  if(method_kind == NULL || method_kind[0] == '\0')
    return(0);

  // No identity at all (parser failed, protocol didn't fill anything):
  // refuse to resolve. Otherwise we'd treat every empty-tuple speaker
  // as the same dossier.
  if(msg->nickname[0]    == '\0' && msg->username[0]    == '\0'
      && msg->hostname[0] == '\0' && msg->verified_id[0] == '\0')
    return(0);

  sig.method_kind = method_kind;
  sig.nickname    = msg->nickname;
  sig.username    = msg->username;
  sig.hostname    = msg->hostname;
  sig.verified_id = msg->verified_id;

  return(dossier_resolve(ns_id, &sig,
      display_label != NULL ? display_label : "", create_if_missing));
}
