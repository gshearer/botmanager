// botmanager — MIT
// libcurl multi-handle wrapper and HTTP request helpers.
#define CURL_INTERNAL
#include "curl.h"

// Request freelist helpers

static curl_request_t *
curl_req_alloc(void)
{
  curl_request_t *req = NULL;

  pthread_mutex_lock(&curl_req_mutex);

  if(curl_req_free != NULL)
  {
    req = curl_req_free;
    curl_req_free = req->next;
  }

  pthread_mutex_unlock(&curl_req_mutex);

  if(req == NULL)
    req = mem_alloc("curl", "request", sizeof(*req));

  memset(req, 0, sizeof(*req));

  return(req);
}

// Release a request: free owned memory and return to the freelist.
// req: request to release (must not be NULL)
static void
curl_request_release(curl_request_t *req)
{
  curl_hdr_t *hdr;

  // Free heap-allocated fields.
  if(req->req_body != NULL)
  {
    mem_free(req->req_body);
    req->req_body = NULL;
  }

  if(req->resp_body != NULL)
  {
    mem_free(req->resp_body);
    req->resp_body = NULL;
  }

  // Free custom header list.
  hdr = req->headers;
  while(hdr != NULL)
  {
    curl_hdr_t *next = hdr->next;

    mem_free(hdr->value);
    mem_free(hdr);
    hdr = next;
  }

  // Free curl_slist (built at submit time).
  if(req->curl_headers != NULL)
  {
    curl_slist_free_all(req->curl_headers);
    req->curl_headers = NULL;
  }

  req->headers      = NULL;
  req->easy         = NULL;
  req->resp_body_len = 0;
  req->resp_body_cap = 0;

  // Return to freelist.
  pthread_mutex_lock(&curl_req_mutex);
  req->next = curl_req_free;
  curl_req_free = req;
  pthread_mutex_unlock(&curl_req_mutex);
}

// Header callback for libcurl. Captures only ETag and Last-Modified.
// libcurl delivers each header line including the trailing CRLF; the
// match is case-insensitive, the value is copied into the fixed request
// buffer (bounded, NUL-terminated, CRLF-trimmed).
static size_t
curl_header_cb(char *buf, size_t size, size_t nitems, void *userdata)
{
  curl_request_t *req;
  size_t total;

  req   = (curl_request_t *)userdata;
  total = size * nitems;

  // Ignore empty lines, status lines, and anything short of "X: v".
  if(total < 4)
    return(total);

  // Match header name prefix case-insensitively; locate the colon and
  // skip linear whitespace after it.
  {
    size_t name_len;
    char  *dst;
    size_t dst_cap;
    size_t i;
    size_t vstart;
    size_t vend;

    if(strncasecmp(buf, "ETag:", 5) == 0)
    {
      name_len = 5;
      dst      = req->resp_etag;
      dst_cap  = sizeof(req->resp_etag);
    }

    else if(strncasecmp(buf, "Last-Modified:", 14) == 0)
    {
      name_len = 14;
      dst      = req->resp_last_modified;
      dst_cap  = sizeof(req->resp_last_modified);
    }

    else
      return(total);

    // Skip leading whitespace after the colon.
    vstart = name_len;

    while(vstart < total && (buf[vstart] == ' ' || buf[vstart] == '\t'))
      vstart++;

    // Trim trailing CRLF + any trailing whitespace.
    vend = total;

    while(vend > vstart
        && (buf[vend - 1] == '\r' || buf[vend - 1] == '\n'
            || buf[vend - 1] == ' ' || buf[vend - 1] == '\t'))
      vend--;

    if(vend <= vstart)
      return(total);

    i = vend - vstart;

    if(i >= dst_cap)
      i = dst_cap - 1;

    memcpy(dst, buf + vstart, i);
    dst[i] = '\0';
  }

  return(total);
}

// Write callback for libcurl (accumulates response body)

// libcurl write callback: accumulates response body into req->resp_body.
// Enforces max_response_sz; returns 0 to abort if exceeded.
static size_t
curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  curl_request_t *req;
  size_t bytes;
  size_t needed;

  req = (curl_request_t *)userdata;
  bytes = size * nmemb;

  if(bytes == 0)
    return(0);

  // Deliver to streaming chunk callback before accumulation so the
  // callback sees the new bytes independent of the response buffer.
  if(req->chunk_cb != NULL)
  {
    curl_response_t partial;

    memset(&partial, 0, sizeof(partial));
    partial.request   = req;
    partial.status    = 0;   // not final yet
    partial.curl_code = 0;
    partial.error     = NULL;
    partial.user_data = req->cb_data;

    if(req->accumulate && req->resp_body != NULL)
    {
      partial.body     = req->resp_body;
      partial.body_len = req->resp_body_len;
    }

    if(req->easy != NULL)
    {
      char *ct = NULL;

      curl_easy_getinfo(req->easy, CURLINFO_CONTENT_TYPE, &ct);
      partial.content_type = ct;
    }

    req->chunk_cb(&partial, ptr, bytes, req->chunk_user);
  }

  // When accumulation is disabled, skip buffering and the size cap
  // entirely — chunk_cb is the sole data sink.
  if(!req->accumulate)
    return(bytes);

  // Enforce max response size.
  if(req->resp_body_len + bytes > curl_cfg.max_response_sz)
  {
    clam(CLAM_WARN, "curl", "%s: response exceeds max_response_sz (%u)",
        req->url, curl_cfg.max_response_sz);
    return(0);   // returning 0 aborts the transfer
  }

  // Grow buffer if needed (always keep room for NUL terminator).
  needed = req->resp_body_len + bytes + 1;
  if(needed > req->resp_body_cap)
  {
    size_t newcap;

    newcap = req->resp_body_cap;
    if(newcap == 0)
      newcap = CURL_RESP_INIT_CAP;

    while(newcap < needed)
      newcap *= 2;

    if(newcap > curl_cfg.max_response_sz + 1)
      newcap = curl_cfg.max_response_sz + 1;

    req->resp_body = mem_realloc(req->resp_body, newcap);
    req->resp_body_cap = newcap;
  }

  memcpy(req->resp_body + req->resp_body_len, ptr, bytes);
  req->resp_body_len += bytes;
  req->resp_body[req->resp_body_len] = '\0';

  return(bytes);
}

// Public request API

// Create a new HTTP request in CURL_REQ_CREATED state.
curl_request_t *
curl_request_create(curl_method_t method, const char *url,
    curl_done_cb_t cb, void *user_data)
{
  curl_request_t *req;

  if(url == NULL || cb == NULL)
    return(NULL);

  req = curl_req_alloc();
  req->method     = method;
  req->state      = CURL_REQ_CREATED;
  req->cb         = cb;
  req->cb_data    = user_data;
  req->chunk_cb   = NULL;
  req->chunk_user = NULL;
  req->accumulate = true;
  req->follow_redirects = true;

  snprintf(req->url, sizeof(req->url), "%s", url);

  clam(CLAM_DEBUG5, "curl", "create: %s", url);

  return(req);
}

// Attach a request body and optional Content-Type. Must be called
// before curl_request_submit(). Body data is copied internally.
// req: request handle (must be in CREATED state)
bool
curl_request_set_body(curl_request_t *req, const char *content_type,
    const char *body, size_t body_len)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  if(body != NULL && body_len > 0)
  {
    req->req_body = mem_alloc("curl", "req_body", body_len);
    memcpy(req->req_body, body, body_len);
    req->req_body_len = body_len;
  }

  if(content_type != NULL)
    snprintf(req->content_type, sizeof(req->content_type), "%s", content_type);

  return(SUCCESS);
}

// Prepend a custom header to the request's header list.
// May be called multiple times before submit.
// req: request handle (must be in CREATED state)
bool
curl_request_add_header(curl_request_t *req, const char *header)
{
  curl_hdr_t *hdr;

  if(req == NULL || header == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  hdr = mem_alloc("curl", "header", sizeof(*hdr));
  hdr->value = mem_strdup("curl", "hdr_val", header);
  hdr->next  = req->headers;
  req->headers = hdr;

  return(SUCCESS);
}

// Set a per-request timeout, overriding the global KV default.
// req: request handle (must be in CREATED state)
bool
curl_request_set_timeout(curl_request_t *req, uint32_t timeout_secs)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  req->timeout_secs = timeout_secs;

  return(SUCCESS);
}

// Set a per-request User-Agent, overriding the global KV default.
// req: request handle (must be in CREATED state)
bool
curl_request_set_user_agent(curl_request_t *req, const char *ua)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  if(ua != NULL)
    snprintf(req->user_agent, sizeof(req->user_agent), "%s", ua);
  else
    req->user_agent[0] = '\0';

  return(SUCCESS);
}

// Attach a streaming chunk callback. The done callback still fires at
// transfer end with final status; when accumulate is false, the done
// callback's body will be NULL and body_len 0.
// req: request handle (must be in CREATED state)
bool
curl_request_set_chunk_cb(curl_request_t *req,
    curl_chunk_cb_t cb, void *user_data)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  req->chunk_cb   = cb;
  req->chunk_user = user_data;

  return(SUCCESS);
}

// Toggle response body accumulation. Default true. Set false to skip
// growing resp_body; the chunk callback becomes the sole data sink and
// the max_response_sz cap no longer applies.
// req: request handle (must be in CREATED state)
bool
curl_request_set_accumulate(curl_request_t *req, bool accumulate)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  req->accumulate = accumulate;

  return(SUCCESS);
}

// Toggle following of HTTP Location: redirects. Default true.
// req: request handle (must be in CREATED state)
bool
curl_request_set_follow_redirects(curl_request_t *req, bool follow)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  req->follow_redirects = follow;

  return(SUCCESS);
}

// Submit a request for async execution. Thread-safe. Ownership of
// req transfers to the curl subsystem; caller must not use it after.
// returns: SUCCESS or FAIL (not ready, queue full, or invalid state)
// req: request handle (must be in CREATED state)
bool
curl_request_submit(curl_request_t *req)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  if(!curl_ready)
  {
    clam(CLAM_WARN, "curl", "submit rejected: subsystem not ready");
    curl_request_release(req);
    return(FAIL);
  }

  pthread_mutex_lock(&curl_submit_mutex);

  if(curl_submit_count >= curl_cfg.max_queued)
  {
    pthread_mutex_unlock(&curl_submit_mutex);
    clam(CLAM_WARN, "curl", "submit rejected: queue full (%u)",
        curl_cfg.max_queued);
    curl_request_release(req);
    return(FAIL);
  }

  req->state = CURL_REQ_QUEUED;
  req->next  = NULL;

  if(curl_submit_tail != NULL)
    curl_submit_tail->next = req;
  else
    curl_submit_head = req;

  curl_submit_tail = req;
  curl_submit_count++;

  pthread_mutex_unlock(&curl_submit_mutex);

  // Wake the multi loop.
  {
    uint64_t val = 1;
    (void)write(curl_wake_fd, &val, sizeof(val));
  }

  return(SUCCESS);
}

// Convenience functions

// Blocking variant of curl_request_submit. When the submit queue is
// full, waits on curl_slot_cond instead of returning FAIL. The drain
// thread broadcasts this condition when it clears the queue, so
// waiters wake naturally without polling and without generating the
// "queue full" WARN that the fast-fail path emits on every rejection.
//
// Spurious wakeups are handled by the while-loop; false is returned
// only when the subsystem has shut down.
//
// Does not call curl_request_submit (which would log the WARN on
// every transient queue-full condition). Duplicates the enqueue path
// instead — ~15 lines of acceptable duplication for clean semantics.
bool
curl_request_submit_wait(curl_request_t *req)
{
  if(req == NULL || req->state != CURL_REQ_CREATED)
    return(FAIL);

  pthread_mutex_lock(&curl_submit_mutex);

  while(curl_ready && curl_submit_count >= curl_cfg.max_queued)
    pthread_cond_wait(&curl_slot_cond, &curl_submit_mutex);

  if(!curl_ready)
  {
    pthread_mutex_unlock(&curl_submit_mutex);
    curl_request_release(req);
    return(FAIL);
  }

  req->state = CURL_REQ_QUEUED;
  req->next  = NULL;

  if(curl_submit_tail != NULL)
    curl_submit_tail->next = req;
  else
    curl_submit_head = req;

  curl_submit_tail = req;
  curl_submit_count++;

  pthread_mutex_unlock(&curl_submit_mutex);

  // Wake the multi loop (same as curl_request_submit).
  {
    uint64_t val = 1;
    (void)write(curl_wake_fd, &val, sizeof(val));
  }

  return(SUCCESS);
}

bool
curl_get(const char *url, curl_done_cb_t cb, void *user_data)
{
  curl_request_t *req;

  req = curl_request_create(CURL_METHOD_GET, url, cb, user_data);
  if(req == NULL)
    return(FAIL);

  return(curl_request_submit(req));
}

bool
curl_post(const char *url, const char *content_type,
    const char *body, size_t body_len,
    curl_done_cb_t cb, void *user_data)
{
  curl_request_t *req;

  req = curl_request_create(CURL_METHOD_POST, url, cb, user_data);
  if(req == NULL)
    return(FAIL);

  if(curl_request_set_body(req, content_type, body, body_len) != SUCCESS)
  {
    curl_request_release(req);
    return(FAIL);
  }

  return(curl_request_submit(req));
}

// Multi-loop internals

// Finish a completed transfer: build response, invoke callback, release.
static void
curl_finish_request(curl_request_t *req, CURLcode result)
{
  curl_response_t resp;

  memset(&resp, 0, sizeof(resp));
  resp.request   = req;
  resp.curl_code = (int)result;
  resp.user_data = req->cb_data;

  if(result == CURLE_OK)
  {
    char *ct = NULL;
    curl_off_t dl = 0, ul = 0;
    curl_off_t time_us = 0;

    curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &resp.status);

    curl_easy_getinfo(req->easy, CURLINFO_CONTENT_TYPE, &ct);
    resp.content_type = ct;

    // Conditional-GET headers captured during transfer. Empty buffer
    // means "not present"; surface NULL so callers can cheaply test
    // for presence.
    resp.etag = req->resp_etag[0]
        ? req->resp_etag : NULL;
    resp.last_modified = req->resp_last_modified[0]
        ? req->resp_last_modified : NULL;

    // Byte counters.
    curl_easy_getinfo(req->easy, CURLINFO_SIZE_DOWNLOAD_T, &dl);
    curl_easy_getinfo(req->easy, CURLINFO_SIZE_UPLOAD_T, &ul);
    curl_stat_in  += (uint64_t)dl;
    curl_stat_out += (uint64_t)ul;

    // Response time.
    curl_easy_getinfo(req->easy, CURLINFO_TOTAL_TIME_T, &time_us);
    curl_stat_time_ms += (uint64_t)(time_us / 1000);
  }

  else
  {
    resp.error = req->curl_errbuf[0] ? req->curl_errbuf
        : curl_easy_strerror(result);
    curl_stat_errors++;

    clam(CLAM_WARN, "curl", "%s %s: %s",
        curl_method_name(req->method), req->url,
        resp.error);
  }

  if(req->accumulate)
  {
    resp.body     = req->resp_body ? req->resp_body : "";
    resp.body_len = req->resp_body_len;
  }

  else
  {
    resp.body     = NULL;
    resp.body_len = 0;
  }

  curl_stat_total++;

  // Invoke the caller's completion callback.
  req->state = CURL_REQ_DONE;
  req->cb(&resp);

  // Clean up the easy handle.
  curl_multi_remove_handle(curl_multi_handle, req->easy);
  curl_easy_cleanup(req->easy);
  req->easy = NULL;

  curl_active_count--;

  curl_request_release(req);
}

// Drain the submit queue: build easy handles and add to multi.
// Re-enqueues excess requests when max_active is reached.
static void
curl_drain_queue(void)
{
  // Snapshot the queue under lock.
  curl_request_t *head;
  uint64_t val;

  pthread_mutex_lock(&curl_submit_mutex);

  head  = curl_submit_head;
  curl_submit_head  = NULL;
  curl_submit_tail  = NULL;
  curl_submit_count = 0;

  // Queue just dropped to empty — wake any blocking submitters so
  // they can take the freshly-opened slots. Broadcast (not signal)
  // because N waiters may race for the max_queued slots we just
  // freed; each will re-check the count under the mutex and either
  // enqueue or wait again.
  pthread_cond_broadcast(&curl_slot_cond);

  pthread_mutex_unlock(&curl_submit_mutex);

  // Consume the eventfd so it doesn't keep firing.
  (void)read(curl_wake_fd, &val, sizeof(val));

  while(head != NULL)
  {
    curl_request_t *req;
    CURL *easy;
    uint32_t timeout;
    const char *ua;
    CURLMcode mc;

    req = head;
    head = req->next;
    req->next = NULL;

    // Respect max_active: if at capacity, re-enqueue.
    if(curl_active_count >= curl_cfg.max_active)
    {
      pthread_mutex_lock(&curl_submit_mutex);

      req->next = curl_submit_head;
      curl_submit_head = req;

      if(curl_submit_tail == NULL)
        curl_submit_tail = req;

      curl_submit_count++;

      // Re-enqueue remaining chain too.
      while(head != NULL)
      {
        curl_request_t *r = head;
        head = r->next;
        r->next = NULL;

        curl_submit_tail->next = r;
        curl_submit_tail = r;
        curl_submit_count++;
      }

      pthread_mutex_unlock(&curl_submit_mutex);
      break;
    }

    // Allocate response buffer (skipped when streaming without accumulation).
    if(req->accumulate)
    {
      req->resp_body = mem_alloc("curl", "resp_body", CURL_RESP_INIT_CAP);
      req->resp_body[0]  = '\0';
      req->resp_body_len = 0;
      req->resp_body_cap = CURL_RESP_INIT_CAP;
    }

    // Create easy handle.
    easy = curl_easy_init();
    if(easy == NULL)
    {
      clam(CLAM_WARN, "curl", "curl_easy_init failed for %s", req->url);
      curl_request_release(req);
      continue;
    }

    req->easy = easy;

    // URL.
    curl_easy_setopt(easy, CURLOPT_URL, req->url);

    // Method.
    switch(req->method)
    {
      case CURL_METHOD_GET:
        break;   // GET is the default
      case CURL_METHOD_POST:
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        break;
      case CURL_METHOD_PUT:
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
        break;
      case CURL_METHOD_DELETE:
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
      case CURL_METHOD_PATCH:
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PATCH");
        break;
    }

    // Request body.
    if(req->req_body != NULL && req->req_body_len > 0)
    {
      curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req->req_body);
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
          (curl_off_t)req->req_body_len);
    }

    // Build curl_slist from custom headers.
    req->curl_headers = NULL;

    if(req->content_type[0] != '\0')
    {
      char ct_header[CURL_CT_SZ + 16];

      snprintf(ct_header, sizeof(ct_header), "Content-Type: %s",
          req->content_type);
      req->curl_headers = curl_slist_append(req->curl_headers, ct_header);
    }

    for(curl_hdr_t *h = req->headers; h != NULL; h = h->next)
      req->curl_headers = curl_slist_append(req->curl_headers, h->value);

    if(req->curl_headers != NULL)
      curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req->curl_headers);

    // Timeouts.
    timeout = req->timeout_secs > 0
        ? req->timeout_secs : curl_cfg.timeout;

    curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT,
        (long)curl_cfg.connect_timeout);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,
        req->follow_redirects ? 1L : 0L);

    // Write callback.
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);

    // Header callback — captures ETag + Last-Modified into req's fixed
    // buffers for conditional-GET support on the next fetch.
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, req);

    // Error buffer.
    req->curl_errbuf[0] = '\0';
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, req->curl_errbuf);

    // Cap redirects regardless of whether following is enabled
    // (CURLOPT_FOLLOWLOCATION is set above based on req->follow_redirects).
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);

    // Accept compressed responses.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");

    // Associate request with easy handle.
    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);

    // Verbose logging.
    if(curl_cfg.verbose)
      curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);

    // User-Agent: per-request override, or global default.
    ua = req->user_agent[0] != '\0'
        ? req->user_agent : curl_cfg.user_agent;

    curl_easy_setopt(easy, CURLOPT_USERAGENT, ua);

    // Add to multi handle.
    mc = curl_multi_add_handle(curl_multi_handle, easy);
    if(mc != CURLM_OK)
    {
      clam(CLAM_WARN, "curl", "multi_add_handle failed: %s",
          curl_multi_strerror(mc));
      curl_easy_cleanup(easy);
      req->easy = NULL;
      curl_request_release(req);
      continue;
    }

    req->state = CURL_REQ_ACTIVE;
    curl_active_count++;

    clam(CLAM_DEBUG2, "curl", "%s %s (active: %u)",
        curl_method_name(req->method), req->url, curl_active_count);
  }
}

static void
curl_multi_loop(task_t *t)
{
  struct curl_waitfd extra;

  curl_multi_handle = curl_multi_init();

  if(curl_multi_handle == NULL)
  {
    clam(CLAM_FATAL, "curl", "curl_multi_init failed");
    t->state = TASK_FATAL;
    return;
  }

  // Apply multi options.
  curl_multi_setopt(curl_multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS,
      (long)curl_cfg.max_conns);
  curl_multi_setopt(curl_multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS,
      (long)curl_cfg.max_host_conns);

  // Set up the wake eventfd as an extra poll fd.
  curl_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

  if(curl_wake_fd < 0)
  {
    clam(CLAM_FATAL, "curl", "eventfd failed: %s", strerror(errno));
    curl_multi_cleanup(curl_multi_handle);
    curl_multi_handle = NULL;
    t->state = TASK_FATAL;
    return;
  }

  extra.fd      = curl_wake_fd;
  extra.events  = CURL_WAIT_POLLIN;
  extra.revents = 0;

  while(!pool_shutting_down())
  {
    int numfds = 0;
    int running = 0;
    int msgs_left;
    CURLMsg *msg;

    // 1. Drain any newly submitted requests into multi.
    curl_drain_queue();

    // 2. Wait for activity.
    curl_multi_poll(curl_multi_handle, &extra, 1,
        (int)curl_cfg.poll_timeout, &numfds);

    // 3. Drive all transfers forward.
    curl_multi_perform(curl_multi_handle, &running);

    // 4. Check for completed transfers.
    while((msg = curl_multi_info_read(curl_multi_handle, &msgs_left))
        != NULL)
    {
      curl_request_t *req = NULL;

      if(msg->msg != CURLMSG_DONE)
        continue;

      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req);

      if(req != NULL)
        curl_finish_request(req, msg->data.result);
    }
  }

  // Shutdown: cancel all active transfers.
  {
    CURLMsg *msg;
    int msgs_left;

    // Remove any remaining active handles.
    while((msg = curl_multi_info_read(curl_multi_handle, &msgs_left))
        != NULL)
    {
      if(msg->msg == CURLMSG_DONE)
      {
        curl_request_t *req = NULL;

        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req);

        if(req != NULL)
          curl_finish_request(req, msg->data.result);
      }
    }
  }

  // Drain remaining submit queue without executing.
  {
    curl_request_t *head;

    pthread_mutex_lock(&curl_submit_mutex);

    head = curl_submit_head;
    curl_submit_head  = NULL;
    curl_submit_tail  = NULL;
    curl_submit_count = 0;

    pthread_mutex_unlock(&curl_submit_mutex);

    while(head != NULL)
    {
      curl_request_t *next = head->next;

      curl_request_release(head);
      head = next;
    }
  }

  close(curl_wake_fd);
  curl_wake_fd = -1;

  curl_multi_cleanup(curl_multi_handle);
  curl_multi_handle = NULL;

  t->state = TASK_ENDED;
}

// KV configuration

static void
curl_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  curl_load_config();
}

// Register all core.curl.* KV keys with their default values.
static void
curl_register_kv(void)
{
  kv_register("core.curl.timeout",         KV_UINT32, "30",
      curl_kv_changed, NULL, "HTTP request timeout in seconds");
  kv_register("core.curl.connect_timeout", KV_UINT32, "10",
      curl_kv_changed, NULL, "HTTP connect timeout in seconds");
  kv_register("core.curl.max_active",      KV_UINT32, "32",
      curl_kv_changed, NULL, "Maximum concurrent HTTP transfers");
  kv_register("core.curl.max_queued",      KV_UINT32, "256",
      curl_kv_changed, NULL, "Maximum queued HTTP requests");
  kv_register("core.curl.max_response_sz", KV_UINT32, "4194304",
      curl_kv_changed, NULL, "Maximum HTTP response body size in bytes");
  kv_register("core.curl.poll_timeout",    KV_UINT32, "500",
      curl_kv_changed, NULL, "Curl multi poll timeout in milliseconds");
  kv_register("core.curl.max_conns",       KV_UINT32, "64",
      curl_kv_changed, NULL, "Maximum total connections in the pool");
  kv_register("core.curl.max_host_conns",  KV_UINT32, "8",
      curl_kv_changed, NULL, "Maximum connections per host");
  kv_register("core.curl.verbose",         KV_BOOL, "false",
      curl_kv_changed, NULL, "Enable curl verbose debug output (true/false)");
  kv_register("core.curl.user_agent",      KV_STR,    CURL_DEF_USER_AGENT,
      curl_kv_changed, NULL, "HTTP User-Agent header string");
}

// Load all curl config values from KV into curl_cfg.
// Also applies connection pool settings to the multi handle if running.
static void
curl_load_config(void)
{
  const char *ua;

  curl_cfg.timeout         = (uint32_t)kv_get_uint("core.curl.timeout");
  curl_cfg.connect_timeout = (uint32_t)kv_get_uint("core.curl.connect_timeout");
  curl_cfg.max_active      = (uint32_t)kv_get_uint("core.curl.max_active");
  curl_cfg.max_queued      = (uint32_t)kv_get_uint("core.curl.max_queued");
  curl_cfg.max_response_sz = (uint32_t)kv_get_uint("core.curl.max_response_sz");
  curl_cfg.poll_timeout    = (uint32_t)kv_get_uint("core.curl.poll_timeout");
  curl_cfg.max_conns       = (uint32_t)kv_get_uint("core.curl.max_conns");
  curl_cfg.max_host_conns  = (uint32_t)kv_get_uint("core.curl.max_host_conns");
  curl_cfg.verbose         = (uint32_t)kv_get_uint("core.curl.verbose");

  ua = kv_get_str("core.curl.user_agent");

  if(ua != NULL && ua[0] != '\0')
    snprintf(curl_cfg.user_agent, sizeof(curl_cfg.user_agent), "%s", ua);
  else
    snprintf(curl_cfg.user_agent, sizeof(curl_cfg.user_agent),
        "%s", CURL_DEF_USER_AGENT);

  // Apply connection pool changes to multi handle if running.
  if(curl_multi_handle != NULL)
  {
    curl_multi_setopt(curl_multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS,
        (long)curl_cfg.max_conns);
    curl_multi_setopt(curl_multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS,
        (long)curl_cfg.max_host_conns);
  }
}

// Statistics and utility

void
curl_get_stats(curl_stats_t *out)
{
  memset(out, 0, sizeof(*out));

  out->active           = curl_active_count;
  out->total_requests   = curl_stat_total;
  out->total_errors     = curl_stat_errors;
  out->bytes_in         = curl_stat_in;
  out->bytes_out        = curl_stat_out;
  out->total_response_ms = curl_stat_time_ms;

  pthread_mutex_lock(&curl_submit_mutex);
  out->queued = curl_submit_count;
  pthread_mutex_unlock(&curl_submit_mutex);
}

// Iterate queued curl requests. Active requests are managed by the
// multi handle and not directly enumerable from outside the multi
// loop thread, so only queued requests are yielded individually.
// Use curl_get_stats() for aggregate active/queued counts.
// cb: iteration callback (must be fast — submit queue lock is held)
void
curl_iterate_active(curl_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&curl_submit_mutex);

  for(curl_request_t *r = curl_submit_head; r != NULL; r = r->next)
    cb(r->url, r->method, 0, data);

  pthread_mutex_unlock(&curl_submit_mutex);
}

const char *
curl_method_name(curl_method_t method)
{
  switch(method)
  {
    case CURL_METHOD_GET:    return("GET");
    case CURL_METHOD_POST:   return("POST");
    case CURL_METHOD_PUT:    return("PUT");
    case CURL_METHOD_DELETE: return("DELETE");
    case CURL_METHOD_PATCH:  return("PATCH");
    default:                 return("UNKNOWN");
  }
}

// Lifecycle

// Initialize the curl subsystem: global libcurl init, mutexes,
// default config, and spawn the multi-loop persist task.
// Must be called after pool_init(). No-op if already initialized.
void
curl_init(void)
{
  if(curl_ready)
    return;

  // Global libcurl init (must be called before any curl handle).
  curl_global_init(CURL_GLOBAL_DEFAULT);

  pthread_mutex_init(&curl_submit_mutex, NULL);
  pthread_mutex_init(&curl_req_mutex, NULL);
  pthread_cond_init(&curl_slot_cond, NULL);

  // Set defaults before KV may be available.
  curl_cfg.timeout         = CURL_DEF_TIMEOUT;
  curl_cfg.connect_timeout = CURL_DEF_CONNECT_TIMEOUT;
  curl_cfg.max_active      = CURL_DEF_MAX_ACTIVE;
  curl_cfg.max_queued      = CURL_DEF_MAX_QUEUED;
  curl_cfg.max_response_sz = CURL_DEF_MAX_RESP_SZ;
  curl_cfg.poll_timeout    = CURL_DEF_POLL_TIMEOUT;
  curl_cfg.max_conns       = CURL_DEF_MAX_CONNS;
  curl_cfg.max_host_conns  = CURL_DEF_MAX_HOST_CONNS;
  curl_cfg.verbose         = CURL_DEF_VERBOSE;

  // Spawn the multi-loop persist task.
  curl_task = task_add_persist("curl_multi", 50, curl_multi_loop, NULL);

  if(curl_task == NULL)
  {
    clam(CLAM_FATAL, "curl", "failed to spawn curl multi task");
    return;
  }

  curl_ready = true;

  clam(CLAM_INFO, "curl", "curl service initialized (libcurl %s)",
      curl_version());
}

// Register KV configuration keys and load their values.
// Must be called after kv_init() and plugin_init_all().
void
curl_register_config(void)
{
  curl_register_kv();
  curl_load_config();
}

// Shut down the curl subsystem: drain the request freelist,
// destroy mutexes, and call curl_global_cleanup().
// The multi worker thread must already be joined via pool_exit().
void
curl_exit(void)
{
  if(!curl_ready)
    return;

  curl_ready = false;

  // Wake any threads waiting in curl_request_submit_wait so they
  // notice the shutdown and exit cleanly instead of sleeping through
  // it forever.
  pthread_mutex_lock(&curl_submit_mutex);
  pthread_cond_broadcast(&curl_slot_cond);
  pthread_mutex_unlock(&curl_submit_mutex);

  // Free request freelist.
  pthread_mutex_lock(&curl_req_mutex);

  while(curl_req_free != NULL)
  {
    curl_request_t *req = curl_req_free;
    curl_req_free = req->next;
    mem_free(req);
  }

  pthread_mutex_unlock(&curl_req_mutex);

  pthread_cond_destroy(&curl_slot_cond);
  pthread_mutex_destroy(&curl_submit_mutex);
  pthread_mutex_destroy(&curl_req_mutex);

  curl_global_cleanup();

  clam(CLAM_INFO, "curl", "curl service shut down");
}
