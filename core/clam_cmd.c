// botmanager — MIT
// /clam subscribe|unsubscribe, /show clam: one shared subscriber, local fan-out.

#define CLAM_CMD_INTERNAL
#include "clam_cmd.h"

#include "alloc.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "method.h"
#include "userns.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Module state

static pthread_mutex_t   clam_cmd_mutex;
static clam_user_sub_t  *clam_cmd_subs = NULL;
static bool              clam_cmd_shared_registered = false;
static bool              clam_cmd_ready = false;

// Shared clam subscriber name. Registered lazily on the first /clam
// subscribe; unregistered in clam_cmd_exit().
#define CLAM_CMD_SHARED_NAME "clam_cmd"

// Subscription list helpers (caller holds clam_cmd_mutex)

static clam_user_sub_t *
sub_find_locked(const char *name)
{
  for(clam_user_sub_t *s = clam_cmd_subs; s != NULL; s = s->next)
    if(strncasecmp(s->name, name, CLAM_SUB_NAME_SZ) == 0)
      return(s);
  return(NULL);
}

static void
dest_close_file(ccd_dest_t *d)
{
  if(d->kind == CCD_FILE && d->fp != NULL)
  {
    fclose(d->fp);
    d->fp = NULL;
  }
}

static void
sub_free(clam_user_sub_t *s)
{
  if(s == NULL) return;

  if(s->has_regex)
    regfree(&s->regex_compiled);

  for(size_t i = 0; i < s->n_dests; i++)
    dest_close_file(&s->dests[i]);

  mem_free(s);
}

// Destination routing

// Compose a chat-friendly single-line render of a clam message.
// [DBG5 curl] create: https://...
static void
render_chat(const clam_msg_t *m, char *out, size_t out_sz)
{
  static const char *short_label[] = {
    "FATL", "WARN", "INFO", " DBG", "DBG2", "DBG3", "DBG4", "DBG5"
  };
  uint8_t sev = m->sev;
  if(sev > CLAM_DEBUG5) sev = CLAM_DEBUG5;

  snprintf(out, out_sz, "[%s %s] %s",
      short_label[sev], m->context, m->msg);
}

// File destinations get a fuller line with a timestamp so log files are
// self-explanatory.
static void
render_file(const clam_msg_t *m, char *out, size_t out_sz)
{
  static const char *short_label[] = {
    "FATL", "WARN", "INFO", " DBG", "DBG2", "DBG3", "DBG4", "DBG5"
  };
  struct tm tm;
  time_t now;
  uint8_t sev;

  sev = m->sev;
  if(sev > CLAM_DEBUG5) sev = CLAM_DEBUG5;

  now = time(NULL);
  localtime_r(&now, &tm);

  snprintf(out, out_sz,
      "%04d-%02d-%02d %02d:%02d:%02d %s %-*s %s\n",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec,
      short_label[sev], CLAM_CTX_SZ, m->context, m->msg);
}

static void
dest_dispatch(const ccd_dest_t *d, const clam_msg_t *m,
    const char *chat_line)
{
  method_inst_t *inst = NULL;
  const char    *target = NULL;

  switch(d->kind)
  {
    case CCD_HERE:
      inst   = method_find(d->method_name);
      target = d->target;
      break;

    case CCD_METHOD:
    {
      bot_inst_t *b = bot_find(d->bot_name);
      if(b == NULL) return;            // bot gone; silently skip
      inst   = bot_resolve_method(b, d->method_key);
      target = d->target;
      break;
    }

    case CCD_FILE:
    {
      char buf[CLAM_CTX_SZ + CLAM_MSG_SZ + 48];

      if(d->fp == NULL) return;

      render_file(m, buf, sizeof(buf));
      fputs(buf, d->fp);
      fflush(d->fp);
      return;
    }
  }

  if(inst == NULL || target == NULL || target[0] == '\0')
    return;

  method_send(inst, target, chat_line);
}

// Shared clam subscriber callback — fans out to every user subscription.
// Runs under clam_mutex on the publisher's thread; must be fast.

static void
clam_cmd_shared_cb(const clam_msg_t *m)
{
  char haystack[CLAM_CTX_SZ + CLAM_MSG_SZ + 2];
  bool haystack_built = false;

  char chat_line[CLAM_CTX_SZ + CLAM_MSG_SZ + 16];
  bool chat_built = false;

  pthread_mutex_lock(&clam_cmd_mutex);

  for(clam_user_sub_t *s = clam_cmd_subs; s != NULL; s = s->next)
  {
    if(m->sev > s->sev)
      continue;

    if(m->sev != CLAM_FATAL && s->has_regex)
    {
      if(!haystack_built)
      {
        snprintf(haystack, sizeof(haystack), "%s %s",
            m->context, m->msg);
        haystack_built = true;
      }
      if(regexec(&s->regex_compiled, haystack, 0, NULL, 0) != 0)
        continue;
    }

    if(!chat_built)
    {
      render_chat(m, chat_line, sizeof(chat_line));
      chat_built = true;
    }

    for(size_t i = 0; i < s->n_dests; i++)
      dest_dispatch(&s->dests[i], m, chat_line);
  }

  pthread_mutex_unlock(&clam_cmd_mutex);
}

static void
ensure_shared_subscriber_locked(void)
{
  if(clam_cmd_shared_registered)
    return;

  // Subscribe at DEBUG5 with no regex — we filter per-user in the
  // callback. clam_subscribe is safe to call with clam_cmd_mutex held
  // because clam's own mutex is independent.
  clam_subscribe(CLAM_CMD_SHARED_NAME, CLAM_DEBUG5, NULL,
      clam_cmd_shared_cb);
  clam_cmd_shared_registered = true;
}

static void
maybe_drop_shared_subscriber_locked(void)
{
  if(!clam_cmd_shared_registered)
    return;

  if(clam_cmd_subs != NULL)
    return;

  clam_unsubscribe(CLAM_CMD_SHARED_NAME);
  clam_cmd_shared_registered = false;
}

// Destination parsing

// Split a destination spec into its three ':' segments (bot:method:target).
// Returns true on a successful 3-part split. Empty segments are rejected.
// `spec` is not modified; pieces are copied out.
static bool
split_method_spec(const char *spec, char *bot, size_t bot_sz,
    char *method_key, size_t mk_sz, char *target, size_t tgt_sz)
{
  const char *c1;
  const char *c2;
  size_t n1, n2, n3;

  c1 = strchr(spec, ':');
  if(c1 == NULL || c1 == spec) return(false);

  c2 = strchr(c1 + 1, ':');
  if(c2 == NULL || c2 == c1 + 1) return(false);

  if(*(c2 + 1) == '\0') return(false);

  n1 = (size_t)(c1 - spec);
  n2 = (size_t)(c2 - c1 - 1);
  n3 = strlen(c2 + 1);

  if(n1 >= bot_sz || n2 >= mk_sz || n3 >= tgt_sz)
    return(false);

  memcpy(bot, spec, n1);  bot[n1] = '\0';
  memcpy(method_key, c1 + 1, n2); method_key[n2] = '\0';
  memcpy(target, c2 + 1, n3); target[n3] = '\0';
  return(true);
}

// Parse one destination token into a ccd_dest_t. Returns an error
// string (static) on failure, NULL on success.
//
// Three destination shapes:
//   CCD_HERE    — captured (method_name, target) from the dispatch
//                 context at subscribe time; re-resolved via
//                 method_find on every publish so a bounced method
//                 instance doesn't dangle.
//   CCD_METHOD  — (bot_name, method_key, target). Routes via
//                 bot_resolve_method at publish time. method_key
//                 accepts either a method instance name or a method
//                 kind; see bot_resolve_method() for resolution order.
//   CCD_FILE    — (path, FILE *fp). File is opened at subscribe time
//                 with O_APPEND | O_WRONLY | O_CREAT (mode 0600) and
//                 closed at unsubscribe or clam_cmd_exit().
static const char *
parse_one_dest(const char *tok, const cmd_ctx_t *ctx, ccd_dest_t *out)
{
  memset(out, 0, sizeof(*out));

  // "here" (or empty) — the dispatch context.
  if(tok[0] == '\0' || strcasecmp(tok, "here") == 0)
  {
    const char *mname;
    const char *target;

    if(ctx->msg == NULL || ctx->msg->inst == NULL)
      return("no dispatch context for 'here' destination");

    mname = method_inst_name(ctx->msg->inst);
    if(mname == NULL || mname[0] == '\0')
      return("dispatch method has no name");

    target = ctx->msg->channel[0] != '\0'
        ? ctx->msg->channel : ctx->msg->sender;
    if(target[0] == '\0')
      return("dispatch context has no reply target");

    out->kind = CCD_HERE;
    snprintf(out->method_name, sizeof(out->method_name), "%s", mname);
    snprintf(out->target,      sizeof(out->target),      "%s", target);
    return(NULL);
  }

  // "file:/abs/path"
  if(strncasecmp(tok, "file:", 5) == 0)
  {
    const char *path;
    int fd;
    FILE *fp;

    path = tok + 5;
    if(path[0] != '/')
      return("file destination must be an absolute path");
    if(strlen(path) >= sizeof(out->path))
      return("file path too long");

    fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if(fd < 0)
      return(strerror(errno));

    fp = fdopen(fd, "a");
    if(fp == NULL)
    {
      close(fd);
      return(strerror(errno));
    }

    out->kind = CCD_FILE;
    out->fp   = fp;
    snprintf(out->path, sizeof(out->path), "%s", path);
    return(NULL);
  }

  // "<bot>:<method>:<target>" — requires exactly three colon-separated
  // non-empty parts. Bot must exist and have a binding matching the
  // method segment (instance name or kind).
  {
    char bot_n[BOT_NAME_SZ];
    char mkey [METHOD_NAME_SZ];
    char tgt  [METHOD_CHANNEL_SZ];
    bot_inst_t *b;

    if(!split_method_spec(tok, bot_n, sizeof(bot_n),
          mkey, sizeof(mkey), tgt, sizeof(tgt)))
      return("bad destination (expected here|file:<path>|<bot>:<method>:<target>)");

    b = bot_find(bot_n);
    if(b == NULL)
      return("bot not found");

    if(bot_resolve_method(b, mkey) == NULL)
      return("bot has no binding matching method");

    out->kind = CCD_METHOD;
    snprintf(out->bot_name,   sizeof(out->bot_name),   "%s", bot_n);
    snprintf(out->method_key, sizeof(out->method_key), "%s", mkey);
    snprintf(out->target,     sizeof(out->target),     "%s", tgt);
    return(NULL);
  }
}

// Parse a comma-separated destination list into `out[]`. Empty or NULL
// list yields a single "here" entry. Returns an error string on failure.
static const char *
parse_dest_list(const char *list, const cmd_ctx_t *ctx,
    ccd_dest_t *out, size_t out_cap, size_t *n_out)
{
  const char *p;

  *n_out = 0;

  if(list == NULL || list[0] == '\0')
  {
    const char *err = parse_one_dest("here", ctx, &out[0]);

    if(err != NULL) return(err);
    *n_out = 1;
    return(NULL);
  }

  p = list;
  while(*p != '\0')
  {
    const char *q;
    size_t tlen;
    char tok[PATH_MAX + METHOD_CHANNEL_SZ + 16];

    while(*p == ' ' || *p == '\t' || *p == ',') p++;
    if(*p == '\0') break;

    q = p;
    while(*q != '\0' && *q != ',') q++;

    tlen = (size_t)(q - p);
    while(tlen > 0 && (p[tlen - 1] == ' ' || p[tlen - 1] == '\t'))
      tlen--;

    if(tlen == 0) { p = q; continue; }

    if(*n_out >= out_cap)
    {
      // Roll back any files we opened.
      for(size_t i = 0; i < *n_out; i++) dest_close_file(&out[i]);
      *n_out = 0;
      return("too many destinations");
    }

    if(tlen >= sizeof(tok))
    {
      for(size_t i = 0; i < *n_out; i++) dest_close_file(&out[i]);
      *n_out = 0;
      return("destination too long");
    }
    memcpy(tok, p, tlen); tok[tlen] = '\0';

    {
      const char *err = parse_one_dest(tok, ctx, &out[*n_out]);

      if(err != NULL)
      {
        for(size_t i = 0; i < *n_out; i++) dest_close_file(&out[i]);
        *n_out = 0;
        return(err);
      }
    }
    (*n_out)++;
    p = q;
  }

  if(*n_out == 0)
  {
    const char *err = parse_one_dest("here", ctx, &out[0]);

    if(err != NULL) return(err);
    *n_out = 1;
  }

  return(NULL);
}

// /clam subscribe

static const cmd_arg_desc_t ad_subscribe[] = {
  { "name",  CMD_ARG_ALNUM,  CMD_ARG_REQUIRED, CLAM_SUB_NAME_SZ - 1, NULL },
  { "sev",   CMD_ARG_DIGITS, CMD_ARG_REQUIRED, 2,                    NULL },
  { "regex", CMD_ARG_NONE,   CMD_ARG_OPTIONAL, CLAM_REGEX_SZ - 1,    NULL },
  { "dests", CMD_ARG_NONE,   CMD_ARG_OPTIONAL | CMD_ARG_REST,
      PATH_MAX + 128, NULL },
};

static void
cmd_clam_subscribe(const cmd_ctx_t *ctx)
{
  const char *name  = ctx->parsed->argv[0];
  const char *sev_s = ctx->parsed->argv[1];
  const char *regex_str;
  const char *dests;
  const char *err;
  unsigned long sev_u;
  clam_user_sub_t *s;
  char ack[256];

  regex_str = (ctx->parsed->argc > 2)
      ? ctx->parsed->argv[2] : "";
  dests = (ctx->parsed->argc > 3)
      ? ctx->parsed->argv[3] : "";

  // "-" means "no regex" — convenient when the user wants to skip the
  // third positional and jump straight to destinations.
  if(regex_str != NULL && strcmp(regex_str, "-") == 0)
    regex_str = "";

  sev_u = strtoul(sev_s, NULL, 10);
  if(sev_u > CLAM_DEBUG5)
  {
    cmd_reply(ctx, "sev must be 0-7");
    return;
  }

  pthread_mutex_lock(&clam_cmd_mutex);

  if(sub_find_locked(name) != NULL)
  {
    pthread_mutex_unlock(&clam_cmd_mutex);
    cmd_reply(ctx, "subscription name already in use");
    return;
  }

  s = mem_alloc("clam_cmd", "user_sub", sizeof(*s));
  if(s == NULL)
  {
    pthread_mutex_unlock(&clam_cmd_mutex);
    cmd_reply(ctx, "out of memory");
    return;
  }
  memset(s, 0, sizeof(*s));

  snprintf(s->name, sizeof(s->name), "%s", name);
  s->sev = (uint8_t)sev_u;

  if(ctx->username != NULL)
    snprintf(s->owner, sizeof(s->owner), "%s", ctx->username);

  if(regex_str != NULL && regex_str[0] != '\0')
  {
    snprintf(s->regex_str, sizeof(s->regex_str), "%s", regex_str);
    if(regcomp(&s->regex_compiled, s->regex_str,
          REG_EXTENDED | REG_NOSUB) != 0)
    {
      mem_free(s);
      pthread_mutex_unlock(&clam_cmd_mutex);
      cmd_reply(ctx, "invalid regex");
      return;
    }
    s->has_regex = true;
  }

  err = parse_dest_list(dests, ctx, s->dests,
      CLAM_CMD_MAX_DESTS, &s->n_dests);
  if(err != NULL)
  {
    char buf[256];

    if(s->has_regex) regfree(&s->regex_compiled);
    mem_free(s);
    pthread_mutex_unlock(&clam_cmd_mutex);
    snprintf(buf, sizeof(buf), "dest error: %s", err);
    cmd_reply(ctx, buf);
    return;
  }

  s->next = clam_cmd_subs;
  clam_cmd_subs = s;

  ensure_shared_subscriber_locked();

  pthread_mutex_unlock(&clam_cmd_mutex);

  snprintf(ack, sizeof(ack),
      "subscribed '%s' at sev %u with %zu destination%s",
      name, (unsigned)sev_u, s->n_dests, s->n_dests == 1 ? "" : "s");
  cmd_reply(ctx, ack);
}

// /clam unsubscribe

static const cmd_arg_desc_t ad_unsubscribe[] = {
  { "name", CMD_ARG_ALNUM, CMD_ARG_REQUIRED, CLAM_SUB_NAME_SZ - 1, NULL },
};

static void
cmd_clam_unsubscribe(const cmd_ctx_t *ctx)
{
  const char *name = ctx->parsed->argv[0];

  clam_user_sub_t **pp;
  clam_user_sub_t  *hit = NULL;
  char ack[128];

  pthread_mutex_lock(&clam_cmd_mutex);

  pp = &clam_cmd_subs;

  while(*pp != NULL)
  {
    if(strncasecmp((*pp)->name, name, CLAM_SUB_NAME_SZ) == 0)
    {
      hit = *pp;
      *pp = hit->next;
      break;
    }
    pp = &(*pp)->next;
  }

  if(hit == NULL)
  {
    pthread_mutex_unlock(&clam_cmd_mutex);
    cmd_reply(ctx, "subscription not found");
    return;
  }

  maybe_drop_shared_subscriber_locked();

  pthread_mutex_unlock(&clam_cmd_mutex);

  sub_free(hit);

  snprintf(ack, sizeof(ack), "unsubscribed '%s'", name);
  cmd_reply(ctx, ack);
}

// /clam root — just a hint.

static void
cmd_clam_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "clam: subscribe | unsubscribe");
}

// /show clam — list every user subscription.

static void
render_dest(const ccd_dest_t *d, char *out, size_t out_sz)
{
  switch(d->kind)
  {
    case CCD_HERE:
      snprintf(out, out_sz, "here(%s -> %s)",
          d->method_name, d->target);
      return;

    case CCD_METHOD:
      snprintf(out, out_sz, "%s:%s:%s",
          d->bot_name, d->method_key, d->target);
      return;

    case CCD_FILE:
      snprintf(out, out_sz, "file:%s", d->path);
      return;
  }
  out[0] = '\0';
}

static void
cmd_show_clam(const cmd_ctx_t *ctx)
{
  uint32_t n = 0;
  char foot[64];

  pthread_mutex_lock(&clam_cmd_mutex);

  if(clam_cmd_subs == NULL)
  {
    pthread_mutex_unlock(&clam_cmd_mutex);
    cmd_reply(ctx, "  (no user subscriptions)");
    return;
  }

  for(clam_user_sub_t *s = clam_cmd_subs; s != NULL; s = s->next)
  {
    char hdr[256];

    snprintf(hdr, sizeof(hdr),
        "  %-20s sev=%u regex=%s owner=%s",
        s->name, s->sev,
        s->has_regex ? s->regex_str : "*",
        s->owner[0] != '\0' ? s->owner : "-");
    cmd_reply(ctx, hdr);

    for(size_t i = 0; i < s->n_dests; i++)
    {
      char line[256];
      char dbuf[192];

      render_dest(&s->dests[i], dbuf, sizeof(dbuf));
      snprintf(line, sizeof(line), "      -> %s", dbuf);
      cmd_reply(ctx, line);
    }
    n++;
  }

  pthread_mutex_unlock(&clam_cmd_mutex);

  snprintf(foot, sizeof(foot), "%u subscription%s",
      (unsigned)n, n == 1 ? "" : "s");
  cmd_reply(ctx, foot);
}

// Lifecycle

void
clam_cmd_init(void)
{
  pthread_mutex_init(&clam_cmd_mutex, NULL);
  clam_cmd_ready = true;

  // /clam (admin container).
  cmd_register("clam_cmd", "clam",
      "clam <subcommand>",
      "Manage user subscriptions to the CLAM event bus",
      "Subscriptions route matching CLAM messages to one or more\n"
      "destinations. Subverbs: subscribe, unsubscribe. Reads go\n"
      "through /show clam.",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_clam_root, NULL, NULL, NULL, NULL, 0, NULL, NULL);

  cmd_register("clam_cmd", "subscribe",
      "clam subscribe <name> <sev 0-7> [<regex>|-] [<dests>]",
      "Subscribe to CLAM messages with optional filter and destinations",
      "Destinations are comma-separated; each is one of:\n"
      "  here                                 (the reply context)\n"
      "  <bot>:<method>:<target>              (e.g. lessclam:irc:#botman)\n"
      "  file:<absolute-path>                 (append-only log file)\n"
      "If no destinations are supplied, defaults to 'here'.\n"
      "Pass '-' for <regex> to skip the filter when specifying dests.\n"
      "The regex is POSIX ERE matched against \"<context> <msg>\".",
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_clam_subscribe, NULL, "clam", "sub",
      ad_subscribe,
      (uint8_t)(sizeof(ad_subscribe) / sizeof(ad_subscribe[0])), NULL, NULL);

  cmd_register("clam_cmd", "unsubscribe",
      "clam unsubscribe <name>",
      "Remove a CLAM subscription by name",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_clam_unsubscribe, NULL, "clam", "unsub",
      ad_unsubscribe,
      (uint8_t)(sizeof(ad_unsubscribe) / sizeof(ad_unsubscribe[0])), NULL, NULL);

  cmd_register("clam_cmd", "clam",
      "show clam",
      "List all CLAM subscriptions",
      NULL,
      USERNS_GROUP_ADMIN, 100, CMD_SCOPE_ANY, METHOD_T_ANY,
      cmd_show_clam, NULL, "show", NULL, NULL, 0, NULL, NULL);
}

// Must run before clam_exit() AND before bot_exit / method_exit so the
// shared clam subscriber unregisters cleanly and any open log files are
// flushed + closed. The callback is defensive regardless — between here
// and clam_exit() other subsystems continue to emit clam messages, but
// the shared subscriber is gone, so none reach us.
void
clam_cmd_exit(void)
{
  clam_user_sub_t *s;

  if(!clam_cmd_ready)
    return;

  pthread_mutex_lock(&clam_cmd_mutex);

  // Drop the shared subscriber first so no new messages arrive while
  // we're tearing down the list.
  if(clam_cmd_shared_registered)
  {
    pthread_mutex_unlock(&clam_cmd_mutex);
    clam_unsubscribe(CLAM_CMD_SHARED_NAME);
    pthread_mutex_lock(&clam_cmd_mutex);
    clam_cmd_shared_registered = false;
  }

  s = clam_cmd_subs;
  clam_cmd_subs = NULL;

  pthread_mutex_unlock(&clam_cmd_mutex);

  while(s != NULL)
  {
    clam_user_sub_t *next = s->next;

    sub_free(s);
    s = next;
  }

  clam_cmd_ready = false;
  pthread_mutex_destroy(&clam_cmd_mutex);
}
