// botmanager — MIT
// Gemini Spot REST mechanism + freelist + response classifier.
//
// gem_submit_public  — GET <rest_url><path>; no auth, no headers.
// gem_submit_private — POST <rest_url><path>; mints + signs nonce
//                      inside the payload JSON; attaches X-GEMINI-*
//                      headers, sends with an empty HTTP body.
//
// The freelist pattern mirrors kraken_rest.c; Gemini's wire shape
// diverges from Kraken's:
//
//   * Auth is HMAC-SHA384 (vs. SHA-512), signature hex (vs. base64),
//     payload base64 in a header (vs. form-urlencoded in the body).
//   * The HTTP body is intentionally empty.
//   * Errors arrive in a typed envelope:
//        { "result":"error", "reason":"...", "message":"..." }
//     with the HTTP status set to a 4xx/5xx code (vs. Kraken's HTTP
//     200 + in-body `error[]` array).
#define GEM_INTERNAL
#include "gemini.h"

#include "curl_flight.h"
#include "gemini_sign.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------
// Module state
// ------------------------------------------------------------------

static gem_request_t  *gem_req_free = NULL;
static pthread_mutex_t gem_req_mu;

// Every gemini transfer — typed wrapper or exchange-vtable dispatch —
// is sent from one of the two submitters below, so one flight covers
// the plugin. gem_req_mu itself is the reason it has to: gem_rest_deinit
// destroys it, and every completion path takes it on the way out
// through gem_req_release (root TODO.md §SC-OBSERVED OBS-39).
static curl_flight_t   gem_flight;

// ------------------------------------------------------------------
// Freelist
// ------------------------------------------------------------------

gem_request_t *
gem_req_alloc(void)
{
  gem_request_t *r = NULL;

  pthread_mutex_lock(&gem_req_mu);

  if(gem_req_free != NULL)
  {
    r = gem_req_free;
    gem_req_free = r->next;
  }

  pthread_mutex_unlock(&gem_req_mu);

  if(r == NULL)
    r = mem_alloc(GEM_CTX, "request", sizeof(*r));

  memset(r, 0, sizeof(*r));

  return(r);
}

void
gem_req_release(gem_request_t *r)
{
  uint64_t slot;

  if(r == NULL)
    return;

  slot = r->slot;

  if(r->payload != NULL)
  {
    mem_free(r->payload);
    r->payload     = NULL;
    r->payload_len = 0;
  }

  pthread_mutex_lock(&gem_req_mu);
  r->next      = gem_req_free;
  gem_req_free = r;
  pthread_mutex_unlock(&gem_req_mu);

  // Last, always: every typed path in this plugin ends here, so this is
  // where gem_rest_drain() learns the work is over — and gem_req_mu,
  // just above it, is one of the locks gem_deinit() destroys.
  curl_flight_close(&gem_flight, slot);
}

// ------------------------------------------------------------------
// URL helpers
// ------------------------------------------------------------------

bool
gem_rest_base_url(char *out, size_t cap)
{
  const char *base;
  size_t      blen;

  if(out == NULL || cap == 0)
    return(FAIL);

  base = kv_get_str("plugin.gemini.rest_url");

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
// Successful Gemini responses are plain JSON without the "result":
// "error" envelope. Errors carry the typed envelope and an HTTP 4xx
// (most often 400 / 422) or 5xx status.
// ------------------------------------------------------------------

static gem_resp_kind_t
gem_classify_body(const char *body, size_t body_len,
    char *err_out, size_t err_cap)
{
  struct json_object *root;
  char                result[16];
  char                reason[64];
  char                message[GEM_ERR_SZ];
  gem_resp_kind_t     kind = GEM_RESP_OK;

  if(err_out != NULL && err_cap > 0)
    err_out[0] = '\0';

  if(body == NULL || body_len == 0)
    return(GEM_RESP_OK);

  root = json_parse_buf(body, body_len, GEM_CTX);

  if(root == NULL)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: malformed JSON from Gemini");
    return(GEM_RESP_HARD_ERROR);
  }

  result[0]  = '\0';
  reason[0]  = '\0';
  message[0] = '\0';

  // Gemini's error envelope is keyed on "result":"error". Successful
  // responses have either an array root (e.g. /v1/symbols) or an
  // object root without the result key at all.
  json_get_str(root, "result",  result,  sizeof(result));
  json_get_str(root, "reason",  reason,  sizeof(reason));
  json_get_str(root, "message", message, sizeof(message));

  if(strcmp(result, "error") != 0)
  {
    json_object_put(root);
    return(GEM_RESP_OK);
  }

  // RateLimit reasons known: "RateLimit", "MaintenanceMode" treated
  // as transient. Everything else is terminal.
  if(strcmp(reason, "RateLimit") == 0
      || strcmp(reason, "MaintenanceMode") == 0)
    kind = GEM_RESP_RATE_LIMIT;
  else
    kind = GEM_RESP_HARD_ERROR;

  if(err_out != NULL)
  {
    if(message[0] != '\0')
      snprintf(err_out, err_cap, "Error: Gemini: %s: %s",
          reason[0] ? reason : "Error", message);
    else
      snprintf(err_out, err_cap, "Error: Gemini: %s",
          reason[0] ? reason : "unspecified error");
  }

  json_object_put(root);

  return(kind);
}

gem_resp_kind_t
gem_classify_curl(const curl_response_t *resp, char *err_out, size_t err_cap)
{
  if(err_out != NULL && err_cap > 0)
    err_out[0] = '\0';

  if(resp == NULL)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Gemini: null response");
    return(GEM_RESP_TRANSPORT);
  }

  if(resp->curl_code != 0)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Gemini transport: %s",
          resp->error != NULL ? resp->error : "transport error");
    return(GEM_RESP_TRANSPORT);
  }

  if(resp->status == 429)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Gemini HTTP 429 (rate limit), retryable");
    return(GEM_RESP_RATE_LIMIT);
  }

  if(resp->status >= 500 && resp->status < 600)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Gemini HTTP %ld (server), retryable", resp->status);
    return(GEM_RESP_TRANSPORT);
  }

  // 4xx (other than 429) usually carries the typed error envelope;
  // try to surface the structured message rather than the bare code.
  if(resp->status >= 400 && resp->status < 500)
  {
    gem_resp_kind_t kind = gem_classify_body(resp->body, resp->body_len,
        err_out, err_cap);

    // If the body did not contain the envelope (rare), fall back to
    // a generic HTTP error.
    if(kind == GEM_RESP_OK)
    {
      if(err_out != NULL)
        snprintf(err_out, err_cap,
            "Error: Gemini HTTP %ld", resp->status);
      kind = GEM_RESP_HARD_ERROR;
    }

    return(kind);
  }

  if(resp->status != 200 && resp->status != 0)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Gemini HTTP %ld", resp->status);
    return(GEM_RESP_HARD_ERROR);
  }

  return(gem_classify_body(resp->body, resp->body_len, err_out, err_cap));
}

gem_resp_kind_t
gem_classify_exchange(int http_status, const char *body, size_t body_len,
    const char *err_hint, char *err_out, size_t err_cap)
{
  if(err_out != NULL && err_cap > 0)
    err_out[0] = '\0';

  if(err_hint != NULL)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Gemini: %s", err_hint);
    return(GEM_RESP_TRANSPORT);
  }

  if(http_status == 429)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Gemini HTTP 429 (rate limit), retryable");
    return(GEM_RESP_RATE_LIMIT);
  }

  if(http_status >= 500 && http_status < 600)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap,
          "Error: Gemini HTTP %d (server), retryable", http_status);
    return(GEM_RESP_TRANSPORT);
  }

  if(http_status >= 400 && http_status < 500)
  {
    gem_resp_kind_t kind = gem_classify_body(body, body_len, err_out,
        err_cap);

    if(kind == GEM_RESP_OK)
    {
      if(err_out != NULL)
        snprintf(err_out, err_cap, "Error: Gemini HTTP %d", http_status);
      kind = GEM_RESP_HARD_ERROR;
    }

    return(kind);
  }

  if(http_status != 200 && http_status != 0)
  {
    if(err_out != NULL)
      snprintf(err_out, err_cap, "Error: Gemini HTTP %d", http_status);
    return(GEM_RESP_HARD_ERROR);
  }

  return(gem_classify_body(body, body_len, err_out, err_cap));
}

// ------------------------------------------------------------------
// Submitters
// ------------------------------------------------------------------

bool
gem_submit_public(void *user_data, uint64_t *slot, uint8_t prio,
    const char *path, curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[GEM_URL_SZ];
  char            url [GEM_URL_SZ];
  uint32_t        timeout_secs;
  int             n;

  if(path == NULL || slot == NULL || path[0] == '\0' || done_cb == NULL)
    return(FAIL);

  if(gem_rest_base_url(base, sizeof(base)) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX, "submit: REST base URL not configured");
    return(FAIL);
  }

  n = snprintf(url, sizeof(url), "%s%s", base, path);

  if(n < 0 || (size_t)n >= sizeof(url))
  {
    clam(CLAM_WARN, GEM_CTX, "submit: URL overflow path='%s'", path);
    return(FAIL);
  }

  // Nothing is on the wire until the relay below, so the slot opens
  // here: every refusal above it has nothing to give back, and a
  // grounded flight turns the work away before curl ever sees it.
  if(curl_flight_open(&gem_flight, slot) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "submit: gemini is stopping; refusing path='%s'", path);
    return(FAIL);
  }

  cr = curl_request_create(CURL_METHOD_GET, url, done_cb, user_data);

  if(cr == NULL)
  {
    clam(CLAM_WARN, GEM_CTX,
        "submit: curl_request_create failed url='%s'", url);
    curl_flight_close(&gem_flight, *slot);
    return(FAIL);
  }

  curl_request_add_header(cr, "Accept: application/json");
  (void)curl_request_set_prio(cr, (curl_prio_t)prio);

  timeout_secs = (uint32_t)kv_get_uint("plugin.gemini.request_timeout");
  if(timeout_secs > 0)
    (void)curl_request_set_timeout(cr, timeout_secs);

  if(curl_flight_relay(&gem_flight, cr, slot) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "submit: curl_request_submit failed url='%s'", url);
    curl_flight_close(&gem_flight, *slot);
    return(FAIL);
  }

  return(SUCCESS);
}

bool
gem_submit_private(void *user_data, uint64_t *slot, uint8_t prio,
    const char *path, const char *payload_json, size_t payload_len,
    curl_done_cb_t done_cb)
{
  curl_request_t *cr;
  char            base[GEM_URL_SZ];
  char            url [GEM_URL_SZ];
  // Base64 expansion is 4/3; sign output is hex-of-SHA384 (97 chars).
  // Headers run "Header: <value>" so add a 32-byte prefix margin.
  // 288 covers Gemini's 64-char master keys with prefix + ample slack.
  char            payload_b64[GEM_BODY_SZ * 2];
  char            sig_hex[128];
  char            api_key_hdr[288];
  char            payload_hdr[GEM_BODY_SZ * 2 + 32];
  char            sig_hdr[256];
  const char     *api_key;
  uint32_t        timeout_secs;
  int             n;

  if(path == NULL || path[0] == '\0' || done_cb == NULL
      || payload_json == NULL || payload_len == 0)
    return(FAIL);

  if(!gem_apikey_configured())
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: credentials not configured");
    return(FAIL);
  }

  if(payload_len >= GEM_BODY_SZ)
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: payload too large (%zu >= %d)",
        payload_len, GEM_BODY_SZ);
    return(FAIL);
  }

  if(gem_rest_base_url(base, sizeof(base)) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: REST base URL not configured");
    return(FAIL);
  }

  n = snprintf(url, sizeof(url), "%s%s", base, path);

  if(n < 0 || (size_t)n >= sizeof(url))
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: URL overflow path='%s'", path);
    return(FAIL);
  }

  if(gem_sign_request(payload_json, payload_len,
        payload_b64, sizeof(payload_b64),
        sig_hex,     sizeof(sig_hex)) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: signing failed path='%s'", path);
    return(FAIL);
  }

  kv_admin_context_set(true);
  api_key = kv_get_str("plugin.gemini.creds.apikey");
  kv_admin_context_set(false);

  if(api_key == NULL || api_key[0] == '\0')
  {
    clam(CLAM_WARN, GEM_CTX, "private submit: api_key empty");
    return(FAIL);
  }

  n = snprintf(api_key_hdr, sizeof(api_key_hdr),
      "X-GEMINI-APIKEY: %s", api_key);

  if(n < 0 || (size_t)n >= sizeof(api_key_hdr))
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: api_key header overflow");
    return(FAIL);
  }

  n = snprintf(payload_hdr, sizeof(payload_hdr),
      "X-GEMINI-PAYLOAD: %s", payload_b64);

  if(n < 0 || (size_t)n >= sizeof(payload_hdr))
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: payload header overflow");
    return(FAIL);
  }

  n = snprintf(sig_hdr, sizeof(sig_hdr),
      "X-GEMINI-SIGNATURE: %s", sig_hex);

  if(n < 0 || (size_t)n >= sizeof(sig_hdr))
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: signature header overflow");
    return(FAIL);
  }

  // As in gem_submit_public: the slot opens where the transfer begins,
  // not where the caller committed, so every refusal above gives back
  // nothing and a stopping plugin refuses here.
  if(curl_flight_open(&gem_flight, slot) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: gemini is stopping; refusing path='%s'", path);
    return(FAIL);
  }

  cr = curl_request_create(CURL_METHOD_POST, url, done_cb, user_data);

  if(cr == NULL)
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: curl_request_create failed url='%s'", url);
    curl_flight_close(&gem_flight, *slot);
    return(FAIL);
  }

  (void)curl_request_set_prio(cr, (curl_prio_t)prio);

  timeout_secs = (uint32_t)kv_get_uint("plugin.gemini.request_timeout");
  if(timeout_secs > 0)
    (void)curl_request_set_timeout(cr, timeout_secs);

  curl_request_add_header(cr, api_key_hdr);
  curl_request_add_header(cr, payload_hdr);
  curl_request_add_header(cr, sig_hdr);
  curl_request_add_header(cr, "Accept: application/json");
  curl_request_add_header(cr, "Cache-Control: no-cache");
  curl_request_add_header(cr, "Content-Length: 0");

  // Empty body: every parameter rides in the base64-encoded payload
  // header. We still hand the curl layer an explicit text/plain
  // content-type so it matches Gemini's documented contract.
  curl_request_set_body(cr, "text/plain", "", 0);

  if(curl_flight_relay(&gem_flight, cr, slot) != SUCCESS)
  {
    clam(CLAM_WARN, GEM_CTX,
        "private submit: curl_request_submit failed url='%s'", url);
    curl_flight_close(&gem_flight, *slot);
    return(FAIL);
  }

  return(SUCCESS);
}

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------

void
gem_rest_init(void)
{
  pthread_mutex_init(&gem_req_mu, NULL);
  curl_flight_init(&gem_flight);
}

uint32_t
gem_rest_drain(uint32_t ms)
{
  return(curl_flight_drain(&gem_flight, ms));
}

void
gem_rest_slot_close(uint64_t slot)
{
  curl_flight_close(&gem_flight, slot);
}

void
gem_rest_deinit(void)
{
  pthread_mutex_lock(&gem_req_mu);

  while(gem_req_free != NULL)
  {
    gem_request_t *r = gem_req_free;

    gem_req_free = r->next;

    if(r->payload != NULL)
      mem_free(r->payload);

    mem_free(r);
  }

  pthread_mutex_unlock(&gem_req_mu);
  pthread_mutex_destroy(&gem_req_mu);

  curl_flight_destroy(&gem_flight);
}
