// botmanager — MIT
// Name resolution helpers: map bot/user/method tokens to records.
#define RESOLVE_INTERNAL
#include "resolve.h"

// Freelist helpers

static resolve_request_t *
resolve_req_alloc(void)
{
  resolve_request_t *req;

  pthread_mutex_lock(&resolve_req_lock);

  if(resolve_req_free != NULL)
  {
    req = resolve_req_free;
    resolve_req_free = req->next;
  }

  else
    req = mem_alloc("resolve", "request", sizeof(resolve_request_t));

  resolve_pending++;
  pthread_mutex_unlock(&resolve_req_lock);

  memset(req, 0, sizeof(*req));
  return(req);
}

static void
resolve_req_release(resolve_request_t *req)
{
  pthread_mutex_lock(&resolve_req_lock);
  req->next = resolve_req_free;
  resolve_req_free = req;
  resolve_pending--;
  pthread_mutex_unlock(&resolve_req_lock);
}

// KV configuration

// Load resolver configuration from the KV store, applying defaults
// for any unset values.
static void
resolve_load_config(void)
{
  resolve_cfg.timeout     = (uint32_t)kv_get_uint("core.resolve.timeout");
  resolve_cfg.max_pending = (uint32_t)kv_get_uint("core.resolve.max_pending");

  if(resolve_cfg.timeout == 0)
    resolve_cfg.timeout = 10;

  if(resolve_cfg.max_pending == 0)
    resolve_cfg.max_pending = 64;
}

static void
resolve_kv_changed(const char *key, void *data)
{
  (void)key;
  (void)data;
  resolve_load_config();
}

// A/AAAA resolution via getaddrinfo

static void
resolve_via_getaddrinfo(resolve_request_t *req, resolve_result_t *result)
{
  struct addrinfo hints, *res, *rp;
  int              rc;
  time_t           elapsed;
  uint32_t         count;

  memset(&hints, 0, sizeof(hints));

  if(req->qtype == RESOLVE_A)
    hints.ai_family = AF_INET;
  else if(req->qtype == RESOLVE_AAAA)
    hints.ai_family = AF_INET6;
  else
    hints.ai_family = AF_UNSPEC;

  hints.ai_socktype = SOCK_STREAM;

  rc = getaddrinfo(req->name, NULL, &hints, &res);

  // Check timeout.
  if(resolve_cfg.timeout > 0)
  {
    elapsed = time(NULL) - req->submitted;

    if(elapsed > (time_t)resolve_cfg.timeout)
    {
      if(rc == 0)
        freeaddrinfo(res);

      result->status = ETIMEDOUT;
      result->error  = "DNS resolution timed out";
      return;
    }
  }

  if(rc != 0)
  {
    result->status = rc;
    result->error  = gai_strerror(rc);
    return;
  }

  // Count results.
  count = 0;

  for(rp = res; rp != NULL; rp = rp->ai_next)
    if(rp->ai_family == AF_INET || rp->ai_family == AF_INET6)
      count++;

  if(count == 0)
  {
    freeaddrinfo(res);
    result->status = EAI_NONAME;
    result->error  = "no address records found";
    return;
  }

  if(count > RESOLVE_MAX_RECORDS)
    count = RESOLVE_MAX_RECORDS;

  result->records = mem_alloc("resolve", "records",
      count * sizeof(resolve_record_t));
  result->count = 0;

  for(rp = res; rp != NULL && result->count < count; rp = rp->ai_next)
  {
    resolve_record_t *rec = &result->records[result->count];
    memset(rec, 0, sizeof(*rec));

    if(rp->ai_family == AF_INET)
    {
      struct sockaddr_in *sa4 = (struct sockaddr_in *)rp->ai_addr;

      rec->type = RESOLVE_A;
      inet_ntop(AF_INET, &sa4->sin_addr,
          rec->a.addr, sizeof(rec->a.addr));
      memcpy(&rec->a.sa, rp->ai_addr, rp->ai_addrlen);
      rec->a.sa_len = rp->ai_addrlen;
      result->count++;
    }

    else if(rp->ai_family == AF_INET6)
    {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)rp->ai_addr;

      rec->type = RESOLVE_AAAA;
      inet_ntop(AF_INET6, &sa6->sin6_addr,
          rec->aaaa.addr, sizeof(rec->aaaa.addr));
      memcpy(&rec->aaaa.sa, rp->ai_addr, rp->ai_addrlen);
      rec->aaaa.sa_len = rp->ai_addrlen;
      result->count++;
    }
  }

  freeaddrinfo(res);
  result->status = 0;
}

// All other types via res_query

static int
resolve_to_qtype_ns(resolve_type_t qtype)
{
  switch(qtype)
  {
    case RESOLVE_A:     return(ns_t_a);
    case RESOLVE_AAAA:  return(ns_t_aaaa);
    case RESOLVE_MX:    return(ns_t_mx);
    case RESOLVE_TXT:   return(ns_t_txt);
    case RESOLVE_CNAME: return(ns_t_cname);
    case RESOLVE_NS:    return(ns_t_ns);
    case RESOLVE_PTR:   return(ns_t_ptr);
    case RESOLVE_SRV:   return(ns_t_srv);
    case RESOLVE_SOA:   return(ns_t_soa);
  }

  return(-1);
}

static void
resolve_via_res_query(resolve_request_t *req, resolve_result_t *result)
{
  unsigned char answer[RESOLVE_ANSWER_SZ];
  int           qtype_ns;
  int           len;
  time_t        elapsed;
  ns_msg        msg;
  int           an_count;

  qtype_ns = resolve_to_qtype_ns(req->qtype);

  if(qtype_ns < 0)
  {
    result->status = -1;
    result->error  = "unsupported record type";
    return;
  }

  len = res_query(req->name, ns_c_in, qtype_ns, answer, sizeof(answer));

  // Check timeout.
  if(resolve_cfg.timeout > 0)
  {
    elapsed = time(NULL) - req->submitted;

    if(elapsed > (time_t)resolve_cfg.timeout)
    {
      result->status = ETIMEDOUT;
      result->error  = "DNS resolution timed out";
      return;
    }
  }

  if(len < 0)
  {
    result->status = h_errno;

    switch(h_errno)
    {
      case HOST_NOT_FOUND: result->error = "host not found";       break;
      case NO_DATA:        result->error = "no records found";     break;
      case NO_RECOVERY:    result->error = "non-recoverable error"; break;
      case TRY_AGAIN:      result->error = "temporary failure";    break;
      default:             result->error = "DNS query failed";     break;
    }

    return;
  }

  // Parse the DNS response.
  if(ns_initparse(answer, len, &msg) < 0)
  {
    result->status = -1;
    result->error  = "failed to parse DNS response";
    return;
  }

  an_count = ns_msg_count(msg, ns_s_an);

  if(an_count <= 0)
  {
    result->status = 0;
    result->count  = 0;
    return;
  }

  if((uint32_t)an_count > RESOLVE_MAX_RECORDS)
    an_count = RESOLVE_MAX_RECORDS;

  result->records = mem_alloc("resolve", "records",
      (uint32_t)an_count * sizeof(resolve_record_t));
  result->count = 0;

  for(int i = 0; i < an_count; i++)
  {
    ns_rr                rr;
    unsigned             rr_type;
    resolve_record_t    *rec;
    const unsigned char *rdata;
    uint16_t             rdlen;
    char                 dname[RESOLVE_NAME_SZ];

    if(ns_parserr(&msg, ns_s_an, i, &rr) < 0)
      continue;

    // Skip records that don't match the queried type.
    rr_type = ns_rr_type(rr);

    if(rr_type != (unsigned)qtype_ns)
      continue;

    rec = &result->records[result->count];
    memset(rec, 0, sizeof(*rec));
    rec->type = req->qtype;
    rec->ttl  = ns_rr_ttl(rr);

    rdata = ns_rr_rdata(rr);
    rdlen = ns_rr_rdlen(rr);

    switch(req->qtype)
    {
      case RESOLVE_A:
        if(rdlen >= 4)
        {
          struct sockaddr_in *sa4;

          inet_ntop(AF_INET, rdata, rec->a.addr, sizeof(rec->a.addr));

          sa4 = (struct sockaddr_in *)&rec->a.sa;
          sa4->sin_family = AF_INET;
          memcpy(&sa4->sin_addr, rdata, 4);
          rec->a.sa_len = sizeof(struct sockaddr_in);
        }

        break;

      case RESOLVE_AAAA:
        if(rdlen >= 16)
        {
          struct sockaddr_in6 *sa6;

          inet_ntop(AF_INET6, rdata, rec->aaaa.addr, sizeof(rec->aaaa.addr));

          sa6 = (struct sockaddr_in6 *)&rec->aaaa.sa;
          sa6->sin6_family = AF_INET6;
          memcpy(&sa6->sin6_addr, rdata, 16);
          rec->aaaa.sa_len = sizeof(struct sockaddr_in6);
        }

        break;

      case RESOLVE_MX:
        if(rdlen >= 2)
        {
          rec->mx.priority = ntohs(*(const uint16_t *)rdata);

          if(dn_expand(answer, answer + len,
              rdata + 2, dname, sizeof(dname)) >= 0)
            snprintf(rec->mx.exchange, RESOLVE_NAME_SZ, "%s", dname);
        }

        break;

      case RESOLVE_TXT:
      {
        // TXT records: one or more <length><data> chunks.
        size_t off = 0;
        size_t pos = 0;

        while(off < rdlen && pos < RESOLVE_TXT_SZ - 1)
        {
          uint8_t chunk_len = rdata[off++];
          size_t copy = chunk_len;

          if(copy > rdlen - off)
            copy = rdlen - off;

          if(copy > RESOLVE_TXT_SZ - 1 - pos)
            copy = RESOLVE_TXT_SZ - 1 - pos;

          memcpy(rec->txt.text + pos, rdata + off, copy);
          pos += copy;
          off += chunk_len;
        }

        rec->txt.text[pos] = '\0';
        break;
      }

      case RESOLVE_CNAME:
        if(dn_expand(answer, answer + len,
            rdata, dname, sizeof(dname)) >= 0)
          snprintf(rec->cname.name, RESOLVE_NAME_SZ, "%s", dname);

        break;

      case RESOLVE_NS:
        if(dn_expand(answer, answer + len,
            rdata, dname, sizeof(dname)) >= 0)
          snprintf(rec->ns.name, RESOLVE_NAME_SZ, "%s", dname);

        break;

      case RESOLVE_PTR:
        if(dn_expand(answer, answer + len,
            rdata, dname, sizeof(dname)) >= 0)
          snprintf(rec->ptr.name, RESOLVE_NAME_SZ, "%s", dname);

        break;

      case RESOLVE_SRV:
        if(rdlen >= 6)
        {
          rec->srv.priority = ntohs(*(const uint16_t *)(rdata));
          rec->srv.weight   = ntohs(*(const uint16_t *)(rdata + 2));
          rec->srv.port     = ntohs(*(const uint16_t *)(rdata + 4));

          if(dn_expand(answer, answer + len,
              rdata + 6, dname, sizeof(dname)) >= 0)
            snprintf(rec->srv.target, RESOLVE_NAME_SZ, "%s", dname);
        }

        break;

      case RESOLVE_SOA:
      {
        const unsigned char *p = rdata;

        int n = dn_expand(answer, answer + len, p, dname, sizeof(dname));

        if(n < 0) break;

        snprintf(rec->soa.mname, RESOLVE_NAME_SZ, "%s", dname);
        p += n;

        n = dn_expand(answer, answer + len, p, dname, sizeof(dname));

        if(n < 0) break;

        snprintf(rec->soa.rname, RESOLVE_NAME_SZ, "%s", dname);
        p += n;

        if(p + 20 <= rdata + rdlen)
        {
          rec->soa.serial  = ntohl(*(const uint32_t *)(p));
          rec->soa.refresh = ntohl(*(const uint32_t *)(p + 4));
          rec->soa.retry   = ntohl(*(const uint32_t *)(p + 8));
          rec->soa.expire  = ntohl(*(const uint32_t *)(p + 12));
          rec->soa.minimum = ntohl(*(const uint32_t *)(p + 16));
        }

        break;
      }
    }

    result->count++;
  }

  result->status = 0;
}

// Task callback

static void
resolve_task(task_t *t)
{
  resolve_request_t *req = t->data;

  resolve_result_t result;

  memset(&result, 0, sizeof(result));
  snprintf(result.name, RESOLVE_NAME_SZ, "%s", req->name);
  result.qtype     = req->qtype;
  result.user_data = req->user_data;

  if(req->qtype == RESOLVE_A || req->qtype == RESOLVE_AAAA)
    resolve_via_getaddrinfo(req, &result);
  else
    resolve_via_res_query(req, &result);

  if(result.status != 0)
    clam(CLAM_DEBUG, "resolve", "%s %s: %s",
        resolve_type_name(req->qtype), req->name,
        result.error ? result.error : "unknown error");

  else
    clam(CLAM_DEBUG, "resolve", "%s %s: %u record(s)",
        resolve_type_name(req->qtype), req->name, result.count);

  // Deliver to caller.
  req->cb(&result);

  // Cleanup.
  if(result.records != NULL)
    mem_free(result.records);

  resolve_req_release(req);
  t->state = TASK_ENDED;
}

// Public API

// Submit an asynchronous DNS lookup. The callback is invoked on a
// worker thread once the query completes or times out.
// returns: SUCCESS, or FAIL if not ready, bad params, or at capacity
// name: hostname to resolve (must be non-empty)
// cb: completion callback (invoked on a worker thread)
bool
resolve_lookup(const char *name, resolve_type_t qtype,
    resolve_cb_t cb, void *user_data)
{
  bool               at_cap;
  resolve_request_t *req;

  if(!resolve_ready || name == NULL || cb == NULL)
    return(FAIL);

  if(name[0] == '\0')
    return(FAIL);

  // Capacity check.
  pthread_mutex_lock(&resolve_req_lock);
  at_cap = (resolve_pending >= resolve_cfg.max_pending);
  pthread_mutex_unlock(&resolve_req_lock);

  if(at_cap)
  {
    clam(CLAM_WARN, "resolve",
        "at capacity (%u pending), rejecting lookup for '%s'",
        resolve_cfg.max_pending, name);
    return(FAIL);
  }

  req = resolve_req_alloc();
  strncpy(req->name, name, RESOLVE_NAME_SZ - 1);
  req->qtype     = qtype;
  req->cb        = cb;
  req->user_data = user_data;
  req->submitted = time(NULL);

  task_add("resolve", TASK_THREAD, 100, resolve_task, req);
  return(SUCCESS);
}

const char *
resolve_type_name(resolve_type_t type)
{
  switch(type)
  {
    case RESOLVE_A:     return("A");
    case RESOLVE_AAAA:  return("AAAA");
    case RESOLVE_MX:    return("MX");
    case RESOLVE_TXT:   return("TXT");
    case RESOLVE_CNAME: return("CNAME");
    case RESOLVE_NS:    return("NS");
    case RESOLVE_PTR:   return("PTR");
    case RESOLVE_SRV:   return("SRV");
    case RESOLVE_SOA:   return("SOA");
  }

  return("UNKNOWN");
}

// Parse a DNS record type string into its enum value (case-insensitive).
bool
resolve_type_parse(const char *str, resolve_type_t *out)
{
  char   upper[16];
  size_t i;

  if(str == NULL || out == NULL)
    return(FAIL);

  // Case-insensitive comparison.

  for(i = 0; str[i] != '\0' && i < sizeof(upper) - 1; i++)
    upper[i] = (char)toupper((unsigned char)str[i]);

  upper[i] = '\0';

  if(strcmp(upper, "A")     == 0) { *out = RESOLVE_A;     return(SUCCESS); }
  if(strcmp(upper, "AAAA")  == 0) { *out = RESOLVE_AAAA;  return(SUCCESS); }
  if(strcmp(upper, "MX")    == 0) { *out = RESOLVE_MX;    return(SUCCESS); }
  if(strcmp(upper, "TXT")   == 0) { *out = RESOLVE_TXT;   return(SUCCESS); }
  if(strcmp(upper, "CNAME") == 0) { *out = RESOLVE_CNAME; return(SUCCESS); }
  if(strcmp(upper, "NS")    == 0) { *out = RESOLVE_NS;    return(SUCCESS); }
  if(strcmp(upper, "PTR")   == 0) { *out = RESOLVE_PTR;   return(SUCCESS); }
  if(strcmp(upper, "SRV")   == 0) { *out = RESOLVE_SRV;   return(SUCCESS); }
  if(strcmp(upper, "SOA")   == 0) { *out = RESOLVE_SOA;   return(SUCCESS); }

  return(FAIL);
}

// User command: !resolve

static bool
resolve_validate_target(const char *str)
{
  struct in_addr  v4;
  struct in6_addr v6;

  if(str == NULL || str[0] == '\0')
    return false;

  if(inet_pton(AF_INET, str, &v4) == 1)
    return true;

  if(inet_pton(AF_INET6, str, &v6) == 1)
    return true;

  return validate_hostname(str);
}

static bool
resolve_validate_verbose(const char *str)
{
  if(str == NULL)
    return false;

  return(strcmp(str, "-v") == 0);
}

static int
resolve_ipv4_to_arpa(const char *ip, char *buf, size_t sz)
{
  struct in_addr  addr;
  uint8_t        *o;

  if(inet_pton(AF_INET, ip, &addr) != 1)
    return(0);

  o = (uint8_t *)&addr.s_addr;

  return(snprintf(buf, sz, "%u.%u.%u.%u.in-addr.arpa",
      o[3], o[2], o[1], o[0]));
}

// Convert an IPv6 address to ip6.arpa form for reverse lookup.
// buf: output buffer (must be at least 74 bytes)
static int
resolve_ipv6_to_arpa(const char *ip, char *buf, size_t sz)
{
  struct in6_addr  addr;
  char            *p;

  if(inet_pton(AF_INET6, ip, &addr) != 1)
    return(0);

  if(sz < 74)
    return(0);

  p = buf;

  for(int i = 15; i >= 0; i--)
  {
    uint8_t byte = addr.s6_addr[i];

    *p++ = "0123456789abcdef"[byte & 0x0f];
    *p++ = '.';
    *p++ = "0123456789abcdef"[(byte >> 4) & 0x0f];

    if(i > 0)
      *p++ = '.';
  }

  memcpy(p, ".ip6.arpa", 9);
  p += 9;
  *p = '\0';

  return((int)(p - buf));
}

static resolve_cmd_request_t *
resolve_cmd_req_alloc(void)
{
  resolve_cmd_request_t *r = NULL;

  pthread_mutex_lock(&resolve_cmd_free_mu);

  if(resolve_cmd_free != NULL)
  {
    r = resolve_cmd_free;
    resolve_cmd_free = r->next;
  }

  pthread_mutex_unlock(&resolve_cmd_free_mu);

  if(r == NULL)
    r = mem_alloc("resolve", "cmd_request", sizeof(*r));

  memset(r, 0, sizeof(*r));
  pthread_mutex_init(&r->mu, NULL);

  return(r);
}

static void
resolve_cmd_req_release(resolve_cmd_request_t *r)
{
  pthread_mutex_destroy(&r->mu);

  pthread_mutex_lock(&resolve_cmd_free_mu);
  r->next = resolve_cmd_free;
  resolve_cmd_free = r;
  pthread_mutex_unlock(&resolve_cmd_free_mu);
}

static int
resolve_fmt_ttl(char *buf, size_t sz, uint32_t ttl)
{
  if(ttl >= 86400)
    return(snprintf(buf, sz, "%ud%uh", ttl / 86400, (ttl % 86400) / 3600));

  if(ttl >= 3600)
    return(snprintf(buf, sz, "%uh%um", ttl / 3600, (ttl % 3600) / 60));

  if(ttl >= 60)
    return(snprintf(buf, sz, "%um%us", ttl / 60, ttl % 60));

  return(snprintf(buf, sz, "%us", ttl));
}

static void
resolve_cmd_format(resolve_cmd_request_t *r)
{
  char line[RESOLVE_CMD_REPLY_SZ];
  char ttl_buf[32];

  // Display records in type order for clean grouping.
  static const resolve_type_t type_order[] = {
    RESOLVE_A, RESOLVE_AAAA, RESOLVE_CNAME, RESOLVE_MX,
    RESOLVE_NS, RESOLVE_TXT, RESOLVE_PTR, RESOLVE_SRV, RESOLVE_SOA
  };

  // Header.
  if(r->is_reverse)
    snprintf(line, sizeof(line), CLR_BOLD "PTR" CLR_RESET " %s", r->target);
  else
    snprintf(line, sizeof(line), CLR_BOLD "DNS" CLR_RESET " %s", r->target);

  cmd_reply(&r->ctx, line);

  if(r->count == 0)
  {
    cmd_reply(&r->ctx, CLR_GRAY "  No records found" CLR_RESET);
    return;
  }

  for(uint32_t t = 0; t < sizeof(type_order) / sizeof(type_order[0]); t++)
  {
    resolve_type_t want = type_order[t];

    for(uint32_t i = 0; i < r->count; i++)
    {
      resolve_record_t *rec = &r->records[i];

      if(rec->type != want)
        continue;

      resolve_fmt_ttl(ttl_buf, sizeof(ttl_buf), rec->ttl);

      switch(rec->type)
      {
        case RESOLVE_A:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_GREEN "%s" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "A", rec->a.addr, ttl_buf);
          break;

        case RESOLVE_AAAA:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_GREEN "%s" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "AAAA", rec->aaaa.addr, ttl_buf);
          break;

        case RESOLVE_CNAME:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_YELLOW "%s" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "CNAME", rec->cname.name, ttl_buf);
          break;

        case RESOLVE_MX:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_GRAY "%u " CLR_RESET
              CLR_YELLOW "%s" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "MX", rec->mx.priority, rec->mx.exchange, ttl_buf);
          break;

        case RESOLVE_NS:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_BLUE "%s" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "NS", rec->ns.name, ttl_buf);
          break;

        case RESOLVE_TXT:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_PURPLE "\"%s\"" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "TXT", rec->txt.text, ttl_buf);
          break;

        case RESOLVE_PTR:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_GREEN "%s" CLR_RESET
              " " CLR_GRAY "(%s)" CLR_RESET,
              "PTR", rec->ptr.name, ttl_buf);
          break;

        case RESOLVE_SRV:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_GRAY "%u %u " CLR_RESET
              CLR_ORANGE "%s" CLR_RESET
              CLR_GRAY ":%u (%s)" CLR_RESET,
              "SRV", rec->srv.priority, rec->srv.weight,
              rec->srv.target, rec->srv.port, ttl_buf);
          break;

        case RESOLVE_SOA:
          snprintf(line, sizeof(line),
              "  " CLR_CYAN "%-6s" CLR_RESET " "
              CLR_ORANGE "%s %s" CLR_RESET " "
              CLR_GRAY "serial %u (%s)" CLR_RESET,
              "SOA", rec->soa.mname, rec->soa.rname,
              rec->soa.serial, ttl_buf);
          break;
      }

      cmd_reply(&r->ctx, line);
    }
  }
}

// Resolver callback for the !resolve command.  Accumulates records
// across parallel queries.  When the last query completes, formats
// and sends results to the user.
// result: resolver result (valid only during callback)
static void
resolve_cmd_cb(const resolve_result_t *result)
{
  resolve_cmd_request_t *r = result->user_data;
  bool                   done;

  pthread_mutex_lock(&r->mu);

  if(result->status == 0 && result->records != NULL)
  {
    for(uint32_t i = 0; i < result->count; i++)
    {
      if(r->count >= RESOLVE_CMD_MAX_RECORDS)
        break;

      memcpy(&r->records[r->count], &result->records[i],
          sizeof(resolve_record_t));
      r->count++;
    }
  }

  else if(result->status != 0)
    r->errors++;

  r->pending--;
  done = (r->pending == 0);

  pthread_mutex_unlock(&r->mu);

  if(done)
  {
    if(r->count == 0 && r->errors > 0)
    {
      char line[RESOLVE_CMD_REPLY_SZ];

      snprintf(line, sizeof(line),
          CLR_RED "Lookup failed for %s" CLR_RESET, r->target);
      cmd_reply(&r->ctx, line);
    }

    else
      resolve_cmd_format(r);

    resolve_cmd_req_release(r);
  }
}

static void
resolve_cmd_resolve(const cmd_ctx_t *ctx)
{
  static const resolve_type_t qtypes_all[] = {
    RESOLVE_A, RESOLVE_AAAA, RESOLVE_MX, RESOLVE_TXT,
    RESOLVE_NS, RESOLVE_CNAME, RESOLVE_SOA
  };

  static const resolve_type_t qtypes_default[] = {
    RESOLVE_A
  };

  const char            *target  = ctx->parsed->argv[0];
  bool                   verbose = (ctx->parsed->argc > 1
      && ctx->parsed->argv[1] != NULL
      && strcmp(ctx->parsed->argv[1], "-v") == 0);
  resolve_cmd_request_t *req     = resolve_cmd_req_alloc();
  struct in_addr         v4;
  struct in6_addr        v6;
  char                   arpa[RESOLVE_NAME_SZ];
  const resolve_type_t  *qtypes;
  uint8_t                qcount;
  uint8_t                submitted;

  snprintf(req->target, sizeof(req->target), "%s", target);
  req->verbose = verbose;

  memcpy(&req->msg, ctx->msg, sizeof(req->msg));
  req->ctx.bot      = ctx->bot;
  req->ctx.msg      = &req->msg;
  req->ctx.args     = NULL;
  req->ctx.username = NULL;

  // Detect input type.

  if(inet_pton(AF_INET, target, &v4) == 1)
  {
    req->is_reverse = true;

    if(resolve_ipv4_to_arpa(target, arpa, sizeof(arpa)) == 0)
    {
      cmd_reply(ctx, CLR_RED "Error: failed to format reverse lookup"
          CLR_RESET);
      resolve_cmd_req_release(req);
      return;
    }

    req->pending = 1;

    if(resolve_lookup(arpa, RESOLVE_PTR, resolve_cmd_cb, req) != SUCCESS)
    {
      cmd_reply(ctx, CLR_RED "Error: resolver busy" CLR_RESET);
      resolve_cmd_req_release(req);
    }

    return;
  }

  if(inet_pton(AF_INET6, target, &v6) == 1)
  {
    req->is_reverse = true;

    if(resolve_ipv6_to_arpa(target, arpa, sizeof(arpa)) == 0)
    {
      cmd_reply(ctx, CLR_RED "Error: failed to format reverse lookup"
          CLR_RESET);
      resolve_cmd_req_release(req);
      return;
    }

    req->pending = 1;

    if(resolve_lookup(arpa, RESOLVE_PTR, resolve_cmd_cb, req) != SUCCESS)
    {
      cmd_reply(ctx, CLR_RED "Error: resolver busy" CLR_RESET);
      resolve_cmd_req_release(req);
    }

    return;
  }

  // Hostname -- query A only by default, all types with -v.
  if(verbose)
  {
    qtypes = qtypes_all;
    qcount = RESOLVE_CMD_HOST_QTYPES;
  }

  else
  {
    qtypes = qtypes_default;
    qcount = 1;
  }

  req->is_reverse = false;
  req->pending    = qcount;

  submitted = 0;

  for(uint8_t i = 0; i < qcount; i++)
    if(resolve_lookup(target, qtypes[i], resolve_cmd_cb, req) == SUCCESS)
      submitted++;

  if(submitted == 0)
  {
    cmd_reply(ctx, CLR_RED "Error: resolver busy" CLR_RESET);
    resolve_cmd_req_release(req);
    return;
  }

  // Adjust pending if some submissions failed.
  if(submitted < qcount)
  {
    pthread_mutex_lock(&req->mu);
    req->pending = submitted;
    pthread_mutex_unlock(&req->mu);
  }
}

// Register the !resolve user command.
void
resolve_register_commands(void)
{
  pthread_mutex_init(&resolve_cmd_free_mu, NULL);

  cmd_register("resolve", "resolve",
      "resolve <target> [-v]",
      "DNS lookup for a hostname or IP address",
      "Performs a DNS lookup on the given target.\n"
      "\n"
      "By default, returns only A records. Use -v (verbose) to query\n"
      "all record types: A, AAAA, MX, TXT, NS, CNAME, and SOA.\n"
      "For IPv4 or IPv6 addresses, performs a reverse (PTR) lookup.\n"
      "\n"
      "Examples:\n"
      "  !resolve google.com\n"
      "  !resolve google.com -v\n"
      "  !resolve 8.8.8.8\n"
      "  !resolve 2001:4860:4860::8888",
      USERNS_GROUP_EVERYONE, 0, CMD_SCOPE_ANY, METHOD_T_ANY, resolve_cmd_resolve, NULL, NULL, "res",
      NULL, 0, NULL, NULL);
}

// Statistics

void
resolve_get_stats(resolve_stats_t *out)
{
  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));
}

// Lifecycle

// Initialize the resolver subsystem. Sets up the freelist mutex,
// applies default configuration, and marks the subsystem as ready.
void
resolve_init(void)
{
  pthread_mutex_init(&resolve_req_lock, NULL);
  resolve_pending = 0;

  // Defaults (overridden by KV after kv_load).
  resolve_cfg.timeout     = 10;
  resolve_cfg.max_pending = 64;

  resolve_ready = true;
  clam(CLAM_INFO, "resolve", "resolver subsystem initialized");
}

// Register resolver KV keys and load initial configuration.
// Call after kv_load() so that stored values override defaults.
void
resolve_register_config(void)
{
  kv_register("core.resolve.timeout",     KV_UINT32, "10",
      resolve_kv_changed, NULL,
      "DNS resolution timeout in seconds");
  kv_register("core.resolve.max_pending", KV_UINT32, "64",
      resolve_kv_changed, NULL,
      "Maximum concurrent pending DNS lookups");

  resolve_load_config();
}

// Shut down the resolver subsystem. Drains the freelist and
// destroys the mutex.
void
resolve_exit(void)
{
  resolve_ready = false;

  // Drain freelist.
  pthread_mutex_lock(&resolve_req_lock);

  while(resolve_req_free != NULL)
  {
    resolve_request_t *req = resolve_req_free;
    resolve_req_free = req->next;
    mem_free(req);
  }

  pthread_mutex_unlock(&resolve_req_lock);
  pthread_mutex_destroy(&resolve_req_lock);

  // Drain command request freelist.
  pthread_mutex_lock(&resolve_cmd_free_mu);

  while(resolve_cmd_free != NULL)
  {
    resolve_cmd_request_t *r = resolve_cmd_free;
    resolve_cmd_free = r->next;
    mem_free(r);
  }

  pthread_mutex_unlock(&resolve_cmd_free_mu);
  pthread_mutex_destroy(&resolve_cmd_free_mu);

  cmd_unregister("resolve");

  clam(CLAM_INFO, "resolve", "resolver subsystem shut down");
}
