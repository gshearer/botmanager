// botmanager — MIT
// Claude Code bridge (misc plugin, registers /claude).

#define CLAUDE_INTERNAL
#include "claude.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

// ------------------------------------------------------------------ //
// Forward declarations                                                //
// ------------------------------------------------------------------ //

static void  claude_cmd(const cmd_ctx_t *ctx);
static bool  claude_init(void);
static void  claude_deinit(void);

static bool  claude_run_and_wait(const char *const *argv, const char *cwd,
                 const char *const *envp,
                 unsigned timeout_sec, size_t stdout_cap,
                 int *out_status, int *out_signalled,
                 char **out_buf, size_t *out_len);
static void  claude_on_exit(int status, int signalled,
                 const char *stdout_buf, size_t stdout_len, void *user);
static void  claude_reply_multi(method_inst_t *inst, const char *target,
                 const char *text);
static bool  claude_load_preamble(const char *cwd, const char *rel_path,
                 char *buf, size_t bufsz, size_t *out_len);
static void  claude_pending_clear(void);
static void  claude_pending_deliver(task_t *t);

// ------------------------------------------------------------------ //
// File-static state                                                   //
// ------------------------------------------------------------------ //

// Retry schedule (seconds) for post-restart delivery. Length MUST
// equal CLAUDE_DELIVERY_MAX_ATTEMPTS.
static const int CLAUDE_DELIVERY_RETRY_DELAYS[] = { 8, 20, 45, 90 };

// Global concurrency gate. trylock-only: a second caller gets a
// "busy" reply rather than queueing.
static pthread_mutex_t claude_session_lock    = PTHREAD_MUTEX_INITIALIZER;
static time_t          claude_session_started = 0;

// Argument descriptor. CMD_ARG_REST satisfies the "required" check;
// the callback reads ctx->args directly so the full METHOD_TEXT_SZ
// payload is seen (CMD_ARG_REST's per-token copy caps at CMD_ARG_SZ).
static const cmd_arg_desc_t claude_cmd_args[] = {
  { "prompt", CMD_ARG_NONE,
    CMD_ARG_REQUIRED | CMD_ARG_REST, 0, NULL },
};

// ------------------------------------------------------------------ //
// Reply helper -- split on LF, send each line                         //
// ------------------------------------------------------------------ //

static void
claude_reply_multi(method_inst_t *inst, const char *target,
    const char *text)
{
  const char *p;
  if(inst == NULL || target == NULL || text == NULL)
    return;

  p = text;

  while(*p != '\0')
  {
    const char *nl  = strchr(p, '\n');
    size_t      len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

    if(len == 0)
      method_send(inst, target, " ");

    else
    {
      char   buf[METHOD_TEXT_SZ];
      size_t copy = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;

      memcpy(buf, p, copy);
      buf[copy] = '\0';

      method_send(inst, target, buf);
    }

    if(nl == NULL)
      break;

    p = nl + 1;
  }
}

// ------------------------------------------------------------------ //
// Preamble loader                                                     //
// ------------------------------------------------------------------ //

// Read the preamble file into buf (NUL-terminated), truncating if
// it exceeds bufsz - 1 and warning once. An empty rel_path, a
// missing file, or a zero-byte file is not an error: the caller
// proceeds with *out_len = 0 (no preamble).
//          successfully (possibly truncated); FAIL on non-empty
//          path with I/O errors.
static bool
claude_load_preamble(const char *cwd, const char *rel_path,
    char *buf, size_t bufsz, size_t *out_len)
{
  bool trunc;
  char path[512];
  size_t have;
  int fd;
  size_t cap;
  *out_len = 0;
  if(bufsz > 0) buf[0] = '\0';

  if(rel_path == NULL || rel_path[0] == '\0')
    return(SUCCESS);


  if(rel_path[0] == '/' || cwd == NULL || cwd[0] == '\0')
    snprintf(path, sizeof(path), "%s", rel_path);
  else
    snprintf(path, sizeof(path), "%s/%s", cwd, rel_path);

  fd = open(path, O_RDONLY | O_CLOEXEC);

  if(fd < 0)
  {
    clam(CLAM_WARN, CLAUDE_CTX,
        "preamble: cannot open '%s' (proceeding without preamble)",
        path);
    return(SUCCESS);
  }

  cap = (bufsz > 0) ? (bufsz - 1) : 0;
  have = 0;
  trunc = false;

  while(have < cap)
  {
    ssize_t n = read(fd, buf + have, cap - have);

    if(n < 0)
    {
      close(fd);
      clam(CLAM_WARN, CLAUDE_CTX, "preamble: read error on '%s'", path);
      buf[have] = '\0';
      *out_len  = have;
      return(FAIL);
    }

    if(n == 0)
      break;

    have += (size_t)n;
  }

  // Drain any trailing bytes we couldn't store; used to know whether
  // we truncated.
  if(have == cap)
  {
    char   drain[1024];
    ssize_t n;

    while((n = read(fd, drain, sizeof(drain))) > 0)
      trunc = true;
  }

  close(fd);

  buf[have] = '\0';
  *out_len  = have;

  if(trunc)
    clam(CLAM_WARN, CLAUDE_CTX,
        "preamble: '%s' exceeds %zu bytes; truncating", path, cap);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// proc_spawn completion callback                                      //
// ------------------------------------------------------------------ //

// Fired exactly once from the waiter thread in core/proc.c after the
// child has exited and the reader has drained stdout. Copies results
// under the wait mutex and wakes the caller.
// status:     WEXITSTATUS when !signalled, else signal number
// signalled:  non-zero if the child was terminated by a signal
// stdout_buf: aliases the proc_handle's capture buffer (valid until proc_free)
static void
claude_on_exit(int status, int signalled,
    const char *stdout_buf, size_t stdout_len, void *user)
{
  claude_wait_t *w = user;

  pthread_mutex_lock(w->m);
  w->status    = status;
  w->signalled = signalled;
  w->buf       = stdout_buf;
  w->len       = stdout_len;
  w->done      = true;
  pthread_cond_broadcast(w->c);
  pthread_mutex_unlock(w->m);
}

// ------------------------------------------------------------------ //
// Subprocess wait helper                                              //
// ------------------------------------------------------------------ //

// Spawn a subprocess via proc_spawn, block the calling thread until
// on_exit posts, then hand back exit status + a heap copy of the
// captured stdout.
//
// On SUCCESS: *out_status, *out_signalled, *out_buf (NUL-terminated,
// owned by the caller, freed via mem_free), and *out_len are set;
// *out_buf is "" (not NULL) when the child produced no output.
// On FAIL: proc_spawn failed; output pointers are NOT touched.
//
//          proc_spawn itself failed.
static bool
claude_run_and_wait(const char *const *argv, const char *cwd,
    const char *const *envp,
    unsigned timeout_sec, size_t stdout_cap,
    int *out_status, int *out_signalled,
    char **out_buf, size_t *out_len)
{
  char *copy;
  size_t len;
  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
  claude_wait_t   w = {
    .m         = &m,
    .c         = &c,
    .done      = false,
    .status    = 0,
    .signalled = 0,
    .buf       = NULL,
    .len       = 0,
  };

  proc_spec_t spec = {
    .argv        = argv,
    .cwd         = cwd,
    .envp        = envp,
    .stdout_cap  = stdout_cap,
    .timeout_sec = timeout_sec,
    .on_exit     = claude_on_exit,
    .user        = &w,
  };

  proc_handle_t *h = proc_spawn(&spec);

  if(h == NULL)
  {
    pthread_mutex_destroy(&m);
    pthread_cond_destroy(&c);
    return(FAIL);
  }

  pthread_mutex_lock(&m);
  while(!w.done)
    pthread_cond_wait(&c, &m);
  pthread_mutex_unlock(&m);

  *out_status    = w.status;
  *out_signalled = w.signalled;

  len = w.len;
  copy = mem_alloc(CLAUDE_CTX, "subproc-out", len + 1);

  if(copy != NULL)
  {
    if(len > 0 && w.buf != NULL)
      memcpy(copy, w.buf, len);
    copy[len] = '\0';
  }

  else
    // Out-of-memory on the copy is treated as "no output" -- the
    // caller still gets valid status and proc_free reclaims the
    // handle's buffer.
    len = 0;

  *out_buf = copy;
  *out_len = len;

  proc_free(h);

  pthread_mutex_destroy(&m);
  pthread_cond_destroy(&c);

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Pending-reply KV helpers (cross-restart delivery)                   //
// ------------------------------------------------------------------ //

static void
claude_pending_clear(void)
{
  // Snapshot the path BEFORE we clear it so we can unlink the file.
  char path[512] = { 0 };
  const char *s  = kv_get_str("plugin.claude.pending.text_path");

  if(s != NULL)
    snprintf(path, sizeof(path), "%s", s);

  kv_set_int("plugin.claude.pending.ts", 0);
  kv_set("plugin.claude.pending.text_path", "");
  kv_set("plugin.claude.pending.target",    "");
  kv_set("plugin.claude.pending.network",   "");

  if(path[0] != '\0' && unlink(path) != 0)
    clam(CLAM_DEBUG, CLAUDE_CTX,
        "pending: unlink '%s' failed (harmless if already gone)",
        path);
}

// Deferred-task callback: attempt one post-restart delivery and
// either clear the stash on success or reschedule at the next
// CLAUDE_DELIVERY_RETRY_DELAYS slot. The retry context is freed
// Slurp the pending-reply file into a freshly-allocated buffer.
// Returns true on success (sets *out_text + *out_len, caller must
// mem_free *out_text), false on any failure (missing, unreadable, empty).
static bool
claude_pending_read_file(const char *path, char **out_text, size_t *out_len)
{
  int fd;
  char   *text;
  char   *chunk;
  size_t  cap  = 4096;
  size_t  have = 0;
  bool    ok = false;

  *out_text = NULL;
  *out_len  = 0;

  if(path == NULL || path[0] == '\0')
    return(false);

  fd = open(path, O_RDONLY | O_CLOEXEC);

  if(fd < 0)
    return(false);

  text = mem_alloc(CLAUDE_CTX, "pending-text", cap);

  if(text == NULL)
  {
    close(fd);
    return(false);
  }

  for(;;)
  {
    ssize_t n;

    if(have + 1 >= cap)
    {
      size_t ncap = cap * 2;

      chunk = mem_alloc(CLAUDE_CTX, "pending-text", ncap);

      if(chunk == NULL)
        break;

      memcpy(chunk, text, have);
      mem_free(text);
      text = chunk;
      cap  = ncap;
    }

    n = read(fd, text + have, cap - 1 - have);

    if(n < 0)
      break;

    if(n == 0)
    {
      ok = true;
      break;
    }

    have += (size_t)n;
  }

  close(fd);

  if(ok && have > 0)
  {
    text[have] = '\0';
    *out_text  = text;
    *out_len   = have;
    return(true);
  }

  mem_free(text);
  return(false);
}

// Reschedule the next delivery attempt, or give up and clear the stash.
// Sets t->state = TASK_ENDED and frees r on the give-up path.
static void
claude_pending_send_chunks(task_t *t, claude_pending_retry_ctx_t *r,
    const char *target)
{
  int next = r->attempt + 1;
  int delay_sec;

  if(next >= CLAUDE_DELIVERY_MAX_ATTEMPTS)
  {
    clam(CLAM_WARN, CLAUDE_CTX,
        "deliver-failed: giving up on pending delivery to %s after"
        " %d attempts",
        target[0] != '\0' ? target : "<unset>",
        CLAUDE_DELIVERY_MAX_ATTEMPTS);

    claude_pending_clear();
    mem_free(r);
    t->state = TASK_ENDED;
    return;
  }

  delay_sec = CLAUDE_DELIVERY_RETRY_DELAYS[next];

  clam(CLAM_DEBUG, CLAUDE_CTX,
      "deliver-retry: attempt %d failed, rescheduling in %ds",
      r->attempt, delay_sec);

  r->attempt = next;
  task_add_deferred("claude_deliver", TASK_ANY, 100,
      (uint32_t)delay_sec * 1000U, claude_pending_deliver, r);

  t->state = TASK_ENDED;
}

// Triggered as a deferred task. Decides: deliver now, retry later, or give
// up. Must set t->state = TASK_ENDED and free r on every terminal path.
static void
claude_pending_deliver(task_t *t)
{
  claude_pending_retry_ctx_t *r = t->data;
  char network[METHOD_SENDER_SZ];
  char target[METHOD_CHANNEL_SZ];
  char path[512];
  char *text = NULL;
  size_t text_len = 0;
  method_inst_t *inst;
  const char *s;
  int64_t ts;
  int64_t ttl;
  int64_t now;
  bool file_ok;

  ts = kv_get_int("plugin.claude.pending.ts");

  if(ts == 0)
  {
    clam(CLAM_DEBUG, CLAUDE_CTX,
        "deliver: pending vanished before attempt %d", r->attempt);
    mem_free(r);
    t->state = TASK_ENDED;
    return;
  }

  ttl = kv_get_int("plugin.claude.pending_ttl_sec");

  if(ttl <= 0)
    ttl = 600;

  now = (int64_t)time(NULL);

  if(now - ts > ttl)
  {
    clam(CLAM_WARN, CLAUDE_CTX,
        "deliver-stale: dropping pending reply (%lds old, ttl=%lds)",
        (long)(now - ts), (long)ttl);
    claude_pending_clear();
    mem_free(r);
    t->state = TASK_ENDED;
    return;
  }

  // Snapshot KV strings into locals — kv_get_str hands back internal
  // storage that an operator /set kv could invalidate mid-send.
  network[0] = '\0';
  target[0]  = '\0';
  path[0]    = '\0';

  s = kv_get_str("plugin.claude.pending.network");
  if(s != NULL) snprintf(network, sizeof(network), "%s", s);

  s = kv_get_str("plugin.claude.pending.target");
  if(s != NULL) snprintf(target, sizeof(target), "%s", s);

  s = kv_get_str("plugin.claude.pending.text_path");
  if(s != NULL) snprintf(path, sizeof(path), "%s", s);

  inst = (network[0] != '\0') ? method_find(network) : NULL;

  file_ok = claude_pending_read_file(path, &text, &text_len);

  if(file_ok && inst != NULL && target[0] != '\0' && text_len > 0)
  {
    claude_reply_multi(inst, target, text);
    clam(CLAM_INFO, CLAUDE_CTX,
        "deliver: delivered %zu bytes to %s post-restart (attempt %d)",
        text_len, target, r->attempt);
    claude_pending_clear();
    mem_free(text);
    mem_free(r);
    t->state = TASK_ENDED;
    return;
  }

  if(text != NULL)
    mem_free(text);

  if(!file_ok)
  {
    clam(CLAM_WARN, CLAUDE_CTX,
        "deliver-failed: reply file '%s' unreadable or empty; clearing",
        path[0] != '\0' ? path : "<unset>");
    claude_pending_clear();
    mem_free(r);
    t->state = TASK_ENDED;
    return;
  }

  // File readable but method/target not ready yet → retry with backoff.
  claude_pending_send_chunks(t, r, target);
}

// ------------------------------------------------------------------ //
// /claude command                                                     //
// ------------------------------------------------------------------ //

// KV-snapshot + parsed knobs used by claude_cmd. Lives on the stack of the
// caller so /set kv during a session can't mutate argv strings we hold.
typedef struct {
  char    cli_path[256];
  char    model[128];
  char    cwd[512];
  char    preamble_path[256];
  char    bctl_rel[256];
  int64_t yolo;
  int64_t timeout;
  int64_t cap;
} claude_session_t;

static void
claude_load_session(claude_session_t *s)
{
  const char *v;

  snprintf(s->cli_path,      sizeof(s->cli_path),      "%s", "claude");
  snprintf(s->model,         sizeof(s->model),         "%s", "claude-opus-4-7");
  s->cwd[0] = '\0';
  snprintf(s->preamble_path, sizeof(s->preamble_path), "%s", "prompt.txt");
  snprintf(s->bctl_rel,      sizeof(s->bctl_rel),      "%s",
      "build/tools/botmanctl");

  v = kv_get_str("plugin.claude.cli_path");
  if(v != NULL && v[0] != '\0')
    snprintf(s->cli_path, sizeof(s->cli_path), "%s", v);

  v = kv_get_str("plugin.claude.model");
  if(v != NULL && v[0] != '\0')
    snprintf(s->model, sizeof(s->model), "%s", v);

  v = kv_get_str("plugin.claude.cwd");
  if(v != NULL)
    snprintf(s->cwd, sizeof(s->cwd), "%s", v);

  v = kv_get_str("plugin.claude.preamble_path");
  if(v != NULL)
    snprintf(s->preamble_path, sizeof(s->preamble_path), "%s", v);

  v = kv_get_str("plugin.claude.botmanctl_path");
  if(v != NULL && v[0] != '\0')
    snprintf(s->bctl_rel, sizeof(s->bctl_rel), "%s", v);

  s->yolo    = kv_get_int("plugin.claude.yolo");
  s->timeout = kv_get_int("plugin.claude.timeout_sec");
  s->cap     = kv_get_int("plugin.claude.stdout_cap_bytes");

  if(s->timeout < 0) s->timeout = 0;
  if(s->cap     < 0) s->cap     = 0;
}

// Build child environment: inherit ours, append BOTMAN_* vars. Returns a
// malloc'd envp the caller must mem_free, or NULL on allocation failure.
static char **
claude_build_envp(const claude_session_t *s, const char *network,
    const char *target, char *env_method, size_t env_method_sz,
    char *env_target, size_t env_target_sz,
    char *env_bctl, size_t env_bctl_sz)
{
  char bctl_path[768];
  size_t env_n;
  char **envp;

  snprintf(env_method, env_method_sz, "BOTMAN_REPLY_METHOD=%s",
      network != NULL ? network : "");
  snprintf(env_target, env_target_sz, "BOTMAN_REPLY_TARGET=%s",
      target  != NULL ? target  : "");

  if(s->bctl_rel[0] == '/' || s->cwd[0] == '\0')
    snprintf(bctl_path, sizeof(bctl_path), "%s", s->bctl_rel);
  else
    snprintf(bctl_path, sizeof(bctl_path), "%s/%s", s->cwd, s->bctl_rel);

  snprintf(env_bctl, env_bctl_sz, "BOTMAN_CTL=%s", bctl_path);

  env_n = 0;

  while(environ[env_n] != NULL)
    env_n++;

  envp = mem_alloc(CLAUDE_CTX, "envp", (env_n + 4) * sizeof(*envp));

  if(envp == NULL)
    return(NULL);

  for(size_t i = 0; i < env_n; i++)
    envp[i] = environ[i];

  envp[env_n]     = env_method;
  envp[env_n + 1] = env_target;
  envp[env_n + 2] = env_bctl;
  envp[env_n + 3] = NULL;

  return(envp);
}

// Route the captured subprocess output back to the caller. Returns without
// sending when a restart stashed the reply for post-restart delivery.
static void
claude_route_reply(const cmd_ctx_t *ctx, method_inst_t *inst,
    const char *target, int status, int signalled,
    const char *buf, size_t buflen, time_t session_t0)
{
  int64_t pending_ts;

  pending_ts = kv_get_int("plugin.claude.pending.ts");

  if(pending_ts >= (int64_t)session_t0)
  {
    clam(CLAM_INFO, CLAUDE_CTX,
        "restart in progress (pending.ts=%lld); suppressing live reply",
        (long long)pending_ts);
    return;
  }

  if(signalled)
  {
    char err[128];

    snprintf(err, sizeof(err), "claude: killed by signal %d", status);
    cmd_reply(ctx, err);
    clam(CLAM_WARN, CLAUDE_CTX,
        "killed by signal %d (captured %zu bytes)", status, buflen);
    return;
  }

  if(status != 0)
  {
    char err[128];

    snprintf(err, sizeof(err), "claude: exit %d%s",
        status, buflen > 0 ? " (output follows)" : "");
    cmd_reply(ctx, err);

    if(buflen > 0)
      claude_reply_multi(inst, target, buf);

    clam(CLAM_WARN, CLAUDE_CTX,
        "exit %d (captured %zu bytes)", status, buflen);
    return;
  }

  if(buflen == 0)
  {
    cmd_reply(ctx, "claude: (no output)");
    clam(CLAM_INFO, CLAUDE_CTX, "no output captured");
    return;
  }

  claude_reply_multi(inst, target, buf);
  clam(CLAM_INFO, CLAUDE_CTX,
      "reply sent (%zu bytes) to %s", buflen, target);
}

// Assemble the full prompt (preamble + user_prompt). Warns on truncation.
static void
claude_assemble_prompt(const claude_session_t *s, const char *user_prompt,
    char *prompt, size_t prompt_sz, size_t *out_preamble_len)
{
  char preamble[CLAUDE_PREAMBLE_MAX];
  size_t preamble_len = 0;
  int written;

  claude_load_preamble(s->cwd, s->preamble_path,
      preamble, sizeof(preamble), &preamble_len);

  if(preamble_len > 0)
    written = snprintf(prompt, prompt_sz, "%s\n%s", preamble, user_prompt);
  else
    written = snprintf(prompt, prompt_sz, "%s", user_prompt);

  if(written < 0 || (size_t)written >= prompt_sz)
    clam(CLAM_WARN, CLAUDE_CTX,
        "prompt: assembled prompt exceeds %zu bytes; truncated",
        prompt_sz - 1);

  *out_preamble_len = preamble_len;
}

// Build argv for the claude CLI. argv[] must have at least 8 slots.
static void
claude_build_argv(const claude_session_t *s, const char *prompt,
    const char **argv)
{
  int a = 0;

  argv[a++] = s->cli_path;
  argv[a++] = "-p";
  argv[a++] = prompt;

  if(s->yolo != 0)
    argv[a++] = "--dangerously-skip-permissions";

  argv[a++] = "--model";
  argv[a++] = s->model;
  argv[a]   = NULL;
}

static void
claude_cmd(const cmd_ctx_t *ctx)
{
  claude_session_t s;
  size_t preamble_len = 0;
  char prompt[CLAUDE_PROMPT_SZ];
  char env_bctl[1024];
  char env_target[METHOD_CHANNEL_SZ + 32];
  char env_method[METHOD_SENDER_SZ  + 32];
  int status = 0;
  int signalled = 0;
  char *buf = NULL;
  size_t buflen = 0;
  const char *argv[10];
  const char *network;
  const char *target;
  char **envp;
  bool spawn_ok;
  time_t session_t0;
  const char *user_prompt = ctx->args != NULL ? ctx->args : "";

  while(*user_prompt == ' ' || *user_prompt == '\t')
    user_prompt++;

  if(*user_prompt == '\0')
  {
    cmd_reply(ctx, "claude: empty prompt");
    return;
  }

  // Concurrency gate — trylock, reply busy on contention.
  if(pthread_mutex_trylock(&claude_session_lock) != 0)
  {
    char busy[128];

    snprintf(busy, sizeof(busy), "claude: busy (%lds elapsed)",
        (long)(time(NULL) - claude_session_started));
    cmd_reply(ctx, busy);
    return;
  }

  session_t0 = time(NULL);
  claude_session_started = session_t0;

  claude_load_session(&s);
  claude_assemble_prompt(&s, user_prompt, prompt, sizeof(prompt),
      &preamble_len);

  network = method_inst_name(ctx->msg->inst);
  target  = ctx->msg->channel[0] != '\0'
      ? ctx->msg->channel : ctx->msg->sender;

  envp = claude_build_envp(&s, network, target,
      env_method, sizeof(env_method),
      env_target, sizeof(env_target),
      env_bctl,   sizeof(env_bctl));

  if(envp == NULL)
  {
    cmd_reply(ctx, "claude: out of memory preparing environment");
    goto release;
  }

  claude_build_argv(&s, prompt, argv);

  clam(CLAM_INFO, CLAUDE_CTX,
      "spawn: %s -p <prompt=%zu bytes, preamble=%zu bytes>"
      " --model %s%s for %s@%s",
      s.cli_path, strlen(prompt), preamble_len, s.model,
      s.yolo != 0 ? " --dangerously-skip-permissions" : "",
      target != NULL ? target : "<unknown>",
      network != NULL ? network : "<unknown>");

  spawn_ok = claude_run_and_wait(argv, s.cwd[0] != '\0' ? s.cwd : NULL,
      (const char *const *)envp,
      (unsigned)s.timeout, (size_t)s.cap,
      &status, &signalled, &buf, &buflen);

  mem_free(envp);

  if(!spawn_ok)
  {
    clam(CLAM_WARN, CLAUDE_CTX, "proc_spawn failed");
    cmd_reply(ctx, "claude: spawn failed");
    goto release;
  }

  // Trim trailing newlines/CRs; the multi-line sender preserves internal
  // structure.
  while(buflen > 0 && (buf[buflen - 1] == '\n' || buf[buflen - 1] == '\r'))
    buf[--buflen] = '\0';

  claude_route_reply(ctx, ctx->msg->inst, target,
      status, signalled, buf, buflen, session_t0);

release:
  if(buf != NULL)
    mem_free(buf);

  claude_session_started = 0;
  pthread_mutex_unlock(&claude_session_lock);
}

// ------------------------------------------------------------------ //
// Plugin lifecycle                                                    //
// ------------------------------------------------------------------ //

static bool
claude_init(void)
{
  int64_t ts;
  if(cmd_register(CLAUDE_CTX, CLAUDE_CTX,
      "claude <prompt>",
      "Run the claude CLI with <prompt>, reply with its stdout",
      "Owner-only bridge to the claude CLI. The prompt is prefixed"
      " by the preamble at plugin.claude.preamble_path (default:"
      " prompt.txt in the project root) and passed as"
      " `claude -p <preamble+prompt>`. Captured stdout replies on"
      " the originating method (IRC channel, DM, or botmanctl).\n"
      "\n"
      "If the preamble instructs the CLI to restart the daemon, it"
      " invokes scripts/botman-restart.sh, which runs ninja, stashes"
      " the reply in KV, and asks botman to quit. The supervisor"
      " relaunches; the stashed reply is delivered to the original"
      " channel automatically.\n"
      "\n"
      "Configuration lives under plugin.claude.*. See"
      " plugins/cmd/claude/AGENTS.md.",
      USERNS_GROUP_OWNER, USERNS_OWNER_LEVEL,
      CMD_SCOPE_ANY, METHOD_T_ANY,
      claude_cmd, NULL,
      NULL, NULL,
      claude_cmd_args,
      sizeof(claude_cmd_args) / sizeof(claude_cmd_args[0]),
      NULL, NULL) != SUCCESS)
    return(FAIL);

  ts = kv_get_int("plugin.claude.pending.ts");

  if(ts > 0)
  {
    int64_t age;
    int64_t ttl = kv_get_int("plugin.claude.pending_ttl_sec");

    if(ttl <= 0)
      ttl = 600;

    age = (int64_t)time(NULL) - ts;

    if(age > ttl)
    {
      clam(CLAM_WARN, CLAUDE_CTX,
          "deliver-stale: dropping pending reply (%lds old, ttl=%lds)",
          (long)age, (long)ttl);
      claude_pending_clear();
    }

    else
    {
      claude_pending_retry_ctx_t *r = mem_alloc(CLAUDE_CTX, "retry",
          sizeof(*r));

      if(r == NULL)
      {
        clam(CLAM_WARN, CLAUDE_CTX,
            "pending retry ctx alloc failed; dropping stash");
        claude_pending_clear();
      }

      else
      {
        int delay_sec;
        r->attempt = 0;

        delay_sec = CLAUDE_DELIVERY_RETRY_DELAYS[0];

        clam(CLAM_INFO, CLAUDE_CTX,
            "deliver: pending found (%lds old); first attempt in %ds",
            (long)age, delay_sec);

        task_add_deferred("claude_deliver", TASK_ANY, 100,
            (uint32_t)delay_sec * 1000U, claude_pending_deliver, r);
      }
    }
  }

  clam(CLAM_INFO, CLAUDE_CTX, "claude plugin initialized");
  return(SUCCESS);
}

static void
claude_deinit(void)
{
  cmd_unregister(CLAUDE_CTX);
  clam(CLAM_INFO, CLAUDE_CTX, "claude plugin deinitialized");
}

// ------------------------------------------------------------------ //
// Plugin descriptor                                                   //
// ------------------------------------------------------------------ //

const plugin_desc_t bm_plugin_desc = {
  .api_version     = PLUGIN_API_VERSION,
  .name            = "claude",
  .version         = "1.1",
  .type            = PLUGIN_MISC,
  .kind            = "claude",
  .provides        = { { .name = "misc_claude" } },
  .provides_count  = 1,
  .requires        = {
    { .name = "core_kv"   },
    { .name = "core_task" },
  },
  .requires_count  = 2,
  .kv_schema       = claude_kv_schema,
  .kv_schema_count = sizeof(claude_kv_schema) / sizeof(claude_kv_schema[0]),
  .init            = claude_init,
  .start           = NULL,
  .stop            = NULL,
  .deinit          = claude_deinit,
  .ext             = NULL,
};
