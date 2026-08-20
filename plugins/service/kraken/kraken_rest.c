// botmanager — MIT
// Kraken Spot REST mechanism + freelist + response classifier + form-
// urlencoded body builder.
//
// kr_submit_public  — GET /0/public/<path>; no auth, no headers.
// kr_submit_private — POST /0/private/<path>; mints nonce, signs
//                     uripath || SHA256(nonce_str || postdata) via the
//                     KR-3 signer, attaches API-Key + API-Sign +
//                     application/x-www-form-urlencoded.
//
// The freelist + form builder mirror the coinbase_rest.c pattern;
// Kraken's wire shape diverges (form-urlencoded body vs JSON, in-body
// error envelope vs HTTP status) so the classifier is plugin-specific.
#define KR_INTERNAL
#include "kraken.h"

#include "curl_flight.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------
// Module state
// ------------------------------------------------------------------

static kr_request_t   *kr_req_free = NULL;
static pthread_mutex_t kr_req_mu;

// Every kraken transfer — typed wrapper, exchange-vtable dispatch, WS
// token mint — is sent from one of the two submitters below, so one
// flight covers the plugin. kr_req_mu itself is the reason it has to:
// kr_rest_deinit destroys it, and every completion path takes it on the
// way out through kr_req_release (OBS-39).
static curl_flight_t   kr_flight;

// ------------------------------------------------------------------
// Freelist
// ------------------------------------------------------------------

kr_request_t *
kr_req_alloc(void)
{
  kr_request_t *r = NULL;

  pthread_mutex_lock(&kr_req_mu);

  if(kr_req_free != NULL)
  {
    r = kr_req_free;
    kr_req_free = r->next;
  }

  pthread_mutex_unlock(&kr_req_mu);

  if(r == NULL)
    r = mem_alloc(KR_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

void
kr_req_release(kr_request_t *r)
{
  uint64_t slot;

  if(r == NULL)
    return;

  slot = r->slot;

  if(r->body != NULL)
  {
    mem_free(r->body);
    r->body     = NULL;
    r->body_len = 0;
  }

  pthread_mutex_lock(&kr_req_mu);
  r->next     = kr_req_free;
  kr_req_free = r;
  pthread_mutex_unlock(&kr_req_mu);

  // Last, always: every typed path in this plugin ends here, so this is
  // where kr_rest_drain() learns the work is over — and kr_req_mu, just
  // above it, is one of the locks kr_deinit() destroys.
  curl_flight_close(&kr_flight, slot);
}

// ------------------------------------------------------------------
// URL helpers
// ------------------------------------------------------------------

bool
kr_rest_base_url(char *out, size_t cap)
{
  const char *base;
  size_t      blen;

  if(out == NULL || cap == 0)
    return(FAIL);

  base = kv_get_str("plugin.kraken.rest_url");

  if(base == NULL || base[0] == '\0')
    return(FAIL);

  blen = strlen(base);

  if(blen >= cap)
    return(FAIL);

  memcpy(out, base, blen);
  out[blen] = '\0';

  return(SUCCESS);
}

// ------------------------------------------------------------------
// Response classifier
//
// Kraken's REST response shape:
//   { "error":  ["EAPI:Rate limit exceeded", ...],
//     "result": { ... } }
//
// HTTP status is almost always 200; the application-level error lives
// in the "error" array. The classifier walks the array and maps the
// first entry into a kind. Rate-limit hits are retry-eligible at the
// abstraction layer; hard errors are terminal.
// ------------------------------------------------------------------

static kr_resp_kind_t
kr_classify_body(const char *body, size_t body_len,
    char *err_out, size_t err_cap)
{
  struct json_object *root;
  struct json_object *arr;
  struct json_object *first;
  const char         *msg;
  kr_resp_kind_t      kind = KR_RESP_HARD_ERROR;

  if(err_out != NULL && err_cap > 0)
    err_out[0] = '\0';

  if(body == NULL || body_len == 0)
    return(KR_RESP_OK);

  root = json_parse_buf(body, body_len, KR_CTX);

  if(root == NULL)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: malformed JSON from Kraken");
    return(KR_RESP_HARD_ERROR);
  }

  arr = json_get_array(root, "error");

  // No error[] (or empty array) → success.
  if(arr == NULL || json_object_array_length(arr) == 0)
  {
    json_object_put(root);
    return(KR_RESP_OK);
  }

  first = json_object_array_get_idx(arr, 0);
  msg   = first ? json_object_get_string(first) : NULL;

  if(msg == NULL)
    msg = "Kraken error (unspecified)";

  if(strstr(msg, "EAPI:Rate limit") != NULL
      || strstr(msg, "EOrder:Rate limit") != NULL
      || strstr(msg, "EGeneral:Temporary lockout") != NULL)
    kind = KR_RESP_RATE_LIMIT;
  else
    kind = KR_RESP_HARD_ERROR;

  if(err_out != NULL)
    snprintf(err_out, err_cap, "Error: Kraken: %s", msg);

  json_object_put(root);

  return(kind);
}

kr_resp_kind_t
kr_classify_curl(const curl_response_t *resp, char *err_out, size_t err_cap)
{
  if(err_out != NULL && err_cap > 0)
    err_out[0] = '\0';

  if(resp == NULL)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Kraken: null response");
    return(KR_RESP_TRANSPORT);
  }

  if(resp->curl_code != 0)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Kraken transport: %s",
          resp->error != NULL ? resp->error : "transport error");
    return(KR_RESP_TRANSPORT);
  }

  // HTTP-level rate-limit / server-error signals. Kraken usually
  // surfaces these in-body, but treat the HTTP form as authoritative
  // when it appears.
  if(resp->status == 429)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Kraken HTTP 429 (rate limit), retryable");
    return(KR_RESP_RATE_LIMIT);
  }

  if(resp->status >= 500 && resp->status < 600)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Kraken HTTP %ld (server), retryable", resp->status);
    return(KR_RESP_TRANSPORT);
  }

  if(resp->status != 200)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Kraken HTTP %ld", resp->status);
    return(KR_RESP_HARD_ERROR);
  }

  return(kr_classify_body(resp->body, resp->body_len, err_out, err_cap));
}

kr_resp_kind_t
kr_classify_exchange(int http_status, const char *body, size_t body_len,
    const char *err_hint, char *err_out, size_t err_cap)
{
  if(err_out != NULL && err_cap > 0)
    err_out[0] = '\0';

  if(err_hint != NULL)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Kraken: %s", err_hint);
    return(KR_RESP_TRANSPORT);
  }

  if(http_status == 429)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Kraken HTTP 429 (rate limit), retryable");
    return(KR_RESP_RATE_LIMIT);
  }

  if(http_status >= 500 && http_status < 600)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Kraken HTTP %d (server), retryable", http_status);
    return(KR_RESP_TRANSPORT);
  }

  if(http_status != 200 && http_status != 0)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Kraken HTTP %d", http_status);
    return(KR_RESP_HARD_ERROR);
  }

  return(kr_classify_body(body, body_len, err_out, err_cap));
}

// ------------------------------------------------------------------
// Submitters
// ------------------------------------------------------------------

bool
kr_submit_public(void *user_data, uint64_t *slot, uint8_t prio,
    const char *path, curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[KR_URL_SZ];
  char            url [KR_URL_SZ];
  uint32_t        timeout_secs;
  int             n;

  if(path == NULL || slot == NULL || path[0] == '\0' || done_cb == NULL)
    return(FAIL);

  if(kr_rest_base_url(base, sizeof(base)) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX, "submit: REST base URL not configured");
    return(FAIL);
  }

  n = snprintf(url, sizeof(url), "%s/0/public/%s", base, path);

  if(n < 0 || (size_t)n >= sizeof(url))
  {
    clam(CLAM_WARN, KR_CTX, "submit: URL overflow path='%s'", path);
    return(FAIL);
  }

  // Nothing is on the wire until the relay below, so the slot opens
  // here: every refusal above it has nothing to give back, and a
  // grounded flight turns the work away before curl ever sees it.
  if(curl_flight_open(&kr_flight, slot) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "submit: kraken is stopping; refusing path='%s'", path);
    return(FAIL);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, done_cb, user_data);

  if(cr == NULL)
  {
    clam(CLAM_WARN, KR_CTX,
        "submit: curl_request_create failed url='%s'", url);
    curl_flight_close(&kr_flight, *slot);
    return(FAIL);
  }

  curl_request_add_header(cr, "Accept: application/json");
  (void)curl_request_set_prio(cr, (curl_prio_t)prio);

  timeout_secs = (uint32_t)kv_get_uint("plugin.kraken.request_timeout");
  if(timeout_secs > 0)
    (void)curl_request_set_timeout(cr, timeout_secs);

  if(curl_flight_relay(&kr_flight, cr, slot) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "submit: curl_request_submit failed url='%s'", url);
    curl_flight_close(&kr_flight, *slot);
    return(FAIL);
  }

  return(SUCCESS);
}

// Helper: concatenate caller's body with a leading `nonce=<n>` prefix
// (Kraken accepts the nonce anywhere in the form body, but the docs
// recommend it first). On SUCCESS `*out` is heap-owned and `*out_len`
// holds the byte count (excl. NUL).
static bool
kr_build_private_body(const char *nonce_str, size_t nonce_len,
    const char *body, size_t body_len, char **out, size_t *out_len)
{
  char  *p;
  size_t total;

  // "nonce=" + nonce_len + (body_len > 0 ? "&" + body_len : 0) + NUL
  total = 6 + nonce_len + (body_len > 0 ? 1 + body_len : 0);

  if(total >= KR_BODY_SZ)
    return(FAIL);

  p = mem_alloc(KR_CTX, "private_body", total + 1);

  memcpy(p, "nonce=", 6);
  memcpy(p + 6, nonce_str, nonce_len);

  if(body_len > 0 && body != NULL)
  {
    p[6 + nonce_len] = '&';
    memcpy(p + 6 + nonce_len + 1, body, body_len);
  }

  p[total] = '\0';

  *out     = p;
  *out_len = total;

  return(SUCCESS);
}

bool
kr_submit_private(void *user_data, uint64_t *slot, uint8_t prio,
    const char *path, const char *body, size_t body_len,
    curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[KR_URL_SZ];
  char            url [KR_URL_SZ];
  char            uripath[KR_URL_SZ];
  char            nonce[32];
  char            sig[128];
  char            api_key_hdr[256];
  char            sig_hdr[256];
  const char     *api_key;
  char           *full_body = NULL;
  size_t          full_body_len = 0;
  uint32_t        timeout_secs;
  int             n;

  if(path == NULL || slot == NULL || path[0] == '\0' || done_cb == NULL)
    return(FAIL);

  if(!kr_apikey_configured())
  {
    clam(CLAM_WARN, KR_CTX, "private submit: credentials not configured");
    return(FAIL);
  }

  if(kr_rest_base_url(base, sizeof(base)) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: REST base URL not configured");
    return(FAIL);
  }

  n = snprintf(url, sizeof(url), "%s/0/private/%s", base, path);

  if(n < 0 || (size_t)n >= sizeof(url))
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: URL overflow path='%s'", path);
    return(FAIL);
  }

  n = snprintf(uripath, sizeof(uripath), "/0/private/%s", path);

  if(n < 0 || (size_t)n >= sizeof(uripath))
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: uripath overflow path='%s'", path);
    return(FAIL);
  }

  if(kr_next_nonce(nonce, sizeof(nonce)) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX, "private submit: nonce mint failed");
    return(FAIL);
  }

  if(kr_build_private_body(nonce, strlen(nonce), body, body_len,
        &full_body, &full_body_len) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: body assembly failed (len=%zu)", body_len);
    return(FAIL);
  }

  if(kr_sign_request(uripath, strlen(uripath), nonce, strlen(nonce),
        full_body, full_body_len, sig, sizeof(sig)) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: signing failed path='%s'", path);
    mem_free(full_body);
    return(FAIL);
  }

  kv_admin_context_set(true);
  api_key = kv_get_str("plugin.kraken.creds.apikey");
  kv_admin_context_set(false);

  if(api_key == NULL || api_key[0] == '\0')
  {
    clam(CLAM_WARN, KR_CTX, "private submit: api_key empty");
    mem_free(full_body);
    return(FAIL);
  }

  n = snprintf(api_key_hdr, sizeof(api_key_hdr),
      "API-Key: %s", api_key);

  if(n < 0 || (size_t)n >= sizeof(api_key_hdr))
  {
    clam(CLAM_WARN, KR_CTX, "private submit: api_key header overflow");
    mem_free(full_body);
    return(FAIL);
  }

  n = snprintf(sig_hdr, sizeof(sig_hdr), "API-Sign: %s", sig);

  if(n < 0 || (size_t)n >= sizeof(sig_hdr))
  {
    clam(CLAM_WARN, KR_CTX, "private submit: api_sign header overflow");
    mem_free(full_body);
    return(FAIL);
  }

  // As in kr_submit_public: the slot opens where the transfer begins,
  // not where the caller committed, so every refusal above gives back
  // nothing and a stopping plugin refuses here.
  if(curl_flight_open(&kr_flight, slot) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: kraken is stopping; refusing path='%s'", path);
    mem_free(full_body);
    return(FAIL);
  }

  cr = curl_request_create(CURL_METHOD_POST, url, done_cb, user_data);

  if(cr == NULL)
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: curl_request_create failed url='%s'", url);
    mem_free(full_body);
    curl_flight_close(&kr_flight, *slot);
    return(FAIL);
  }

  (void)curl_request_set_prio(cr, (curl_prio_t)prio);

  timeout_secs = (uint32_t)kv_get_uint("plugin.kraken.request_timeout");
  if(timeout_secs > 0)
    (void)curl_request_set_timeout(cr, timeout_secs);

  curl_request_add_header(cr, api_key_hdr);
  curl_request_add_header(cr, sig_hdr);
  curl_request_add_header(cr, "Accept: application/json");

  curl_request_set_body(cr, "application/x-www-form-urlencoded",
      full_body, full_body_len);

  // The curl subsystem copied the body bytes into its own buffer.
  mem_free(full_body);

  if(curl_flight_relay(&kr_flight, cr, slot) != SUCCESS)
  {
    clam(CLAM_WARN, KR_CTX,
        "private submit: curl_request_submit failed url='%s'", url);
    curl_flight_close(&kr_flight, *slot);
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------
// Form-urlencoded body builder
//
// RFC 3986 unreserved set: A-Z a-z 0-9 - . _ ~
// Everything else is %xx-encoded. Kraken accepts both encoded and
// unencoded slashes etc. for most form values; encoding defensively
// keeps the wire bytes deterministic against the signer.
// ------------------------------------------------------------------

void
kr_form_init(kr_form_t *f, char *buf, size_t cap)
{
  f->buf = buf;
  f->cap = cap;
  f->len = 0;

  if(cap > 0)
    buf[0] = '\0';
}

static bool
kr_is_unreserved(unsigned char c)
{
  if(c >= 'A' && c <= 'Z') return(true);
  if(c >= 'a' && c <= 'z') return(true);
  if(c >= '0' && c <= '9') return(true);
  return(c == '-' || c == '.' || c == '_' || c == '~');
}

// Append `s` to the form buffer, URL-encoding every byte outside the
// unreserved set. Returns FAIL on overflow.
static bool
kr_form_append_encoded(kr_form_t *f, const char *s)
{
  size_t       i;
  size_t       slen = strlen(s);
  static const char hex[] = "0123456789ABCDEF";

  for(i = 0; i < slen; i++)
  {
    unsigned char c = (unsigned char)s[i];

    if(kr_is_unreserved(c))
    {
      if(f->len + 1 + 1 > f->cap)
        return(FAIL);

      f->buf[f->len++] = (char)c;
    }
    else
    {
      if(f->len + 3 + 1 > f->cap)
        return(FAIL);

      f->buf[f->len++] = '%';
      f->buf[f->len++] = hex[(c >> 4) & 0x0F];
      f->buf[f->len++] = hex[c & 0x0F];
    }
  }

  f->buf[f->len] = '\0';

  return(SUCCESS);
}

static bool
kr_form_append_raw(kr_form_t *f, const char *s)
{
  size_t slen = strlen(s);

  if(f->len + slen + 1 > f->cap)
    return(FAIL);

  memcpy(f->buf + f->len, s, slen);
  f->len += slen;
  f->buf[f->len] = '\0';

  return(SUCCESS);
}

bool
kr_form_add(kr_form_t *f, const char *key, const char *value)
{
  if(f == NULL || key == NULL || value == NULL)
    return(FAIL);

  if(f->len > 0)
  {
    if(kr_form_append_raw(f, "&") != SUCCESS)
      return(FAIL);
  }

  if(kr_form_append_raw(f, key) != SUCCESS)
    return(FAIL);

  if(kr_form_append_raw(f, "=") != SUCCESS)
    return(FAIL);

  return(kr_form_append_encoded(f, value));
}

bool
kr_form_add_int(kr_form_t *f, const char *key, int64_t v)
{
  char tmp[32];

  snprintf(tmp, sizeof(tmp), "%lld", (long long)v);

  return(kr_form_add(f, key, tmp));
}

bool
kr_form_add_uint(kr_form_t *f, const char *key, uint64_t v)
{
  char tmp[32];

  snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);

  return(kr_form_add(f, key, tmp));
}

bool
kr_form_add_double(kr_form_t *f, const char *key, double v)
{
  char tmp[64];

  snprintf(tmp, sizeof(tmp), "%.10g", v);

  return(kr_form_add(f, key, tmp));
}

bool
kr_form_add_bool(kr_form_t *f, const char *key, bool v)
{
  return(kr_form_add(f, key, v ? "true" : "false"));
}

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------

void
kr_rest_init(void)
{
  pthread_mutex_init(&kr_req_mu, NULL);
  curl_flight_init(&kr_flight);
}

uint32_t
kr_rest_drain(uint32_t ms)
{
  return(curl_flight_drain(&kr_flight, ms));
}

void
kr_rest_slot_close(uint64_t slot)
{
  curl_flight_close(&kr_flight, slot);
}

void
kr_rest_deinit(void)
{
  pthread_mutex_lock(&kr_req_mu);

  while(kr_req_free != NULL)
  {
    kr_request_t *r = kr_req_free;

    kr_req_free = r->next;

    if(r->body != NULL)
      mem_free(r->body);

    mem_free(r);
  }

  pthread_mutex_unlock(&kr_req_mu);
  pthread_mutex_destroy(&kr_req_mu);

  curl_flight_destroy(&kr_flight);
}
