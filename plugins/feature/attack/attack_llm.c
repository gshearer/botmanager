// botmanager — MIT
// attack: LLM-authored combat flavour — availability gating, the pools,
// and the background refill. The model never runs on the turn path; a
// blow pops a pre-generated line or falls back to the static tables.

#define ATTACK_INTERNAL
#include "attack.h"

#include "inference.h"

#include "alloc.h"
#include "kv.h"
#include "task.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------------ //
// The pools                                                           //
// ------------------------------------------------------------------ //

// One category's stock of unspoken lines. `count` only ever falls on the
// turn path and only ever rises on a refill, so the whole structure is
// memcpy-scale work under atk_pool_lock.
typedef struct
{
  char     lines[ATK_LLM_POOL_MAX][ATK_LLM_TMPL_SZ];
  uint16_t count;
  bool     inflight;              // a refill task/request is outstanding
  time_t   next_try;              // backoff floor after a failure; 0 = free
  uint64_t served;                // templates handed to the renderer
  uint64_t rejected;              // model lines the sanitiser threw away
  char     last_error[128];
} atk_pool_t;

// Lock ordering is atk_turn_lock -> atk_pool_lock, never the
// reverse. atk_pool_lock is never held across file I/O, a task_add, or
// an LLM submit.
//
// Static footprint, stated so nobody has to discover it: 7 categories x
// ATK_LLM_POOL_MAX (64) x ATK_LLM_TMPL_SZ (256) = 112 KB of BSS. That
// is deliberate — the pools are the whole reason no model runs on the
// turn path.
static atk_pool_t      atk_pools[ATK_FLAV__COUNT];
static pthread_mutex_t atk_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        atk_fallbacks;   // static-table renders

// Bumped whenever the operator changes what the model was told. Every
// in-flight refill carries the generation it was submitted under; a
// response that comes back stale is discarded rather than installed,
// because its lines were written by a preamble that no longer applies.
static uint32_t        atk_pool_gen;

// The only tokens a model-authored line may carry. Indexed nowhere —
// the sanitiser and the expander each name them explicitly — but kept
// together so the two can never drift apart unnoticed.
#define ATK_TOK_ATTACKER "attacker"
#define ATK_TOK_TARGET   "target"
#define ATK_TOK_DAMAGE   "damage"
#define ATK_TOK_AFFLICT  "affliction"

static const char *const atk_flav_name[ATK_FLAV__COUNT] = {
  "minor", "medium", "major", "critical", "death", "dot_tick", "dot_death"
};

// How many times each token must appear in a line of each category —
// exactly, no more and no less. This table IS the rule: one loop over it
// replaced the death-line special case, and a further category is one
// more row rather than another branch.
//
// A death line carries no tally, because the blow that killed already
// said the number; an affliction line names its wound, and a blow line
// must not.
static const uint8_t atk_tok_req[ATK_FLAV__COUNT][4] = {
  //                    attacker target damage affliction
  [ATK_FLAV_MINOR]     = { 1, 1, 1, 0 },
  [ATK_FLAV_MEDIUM]    = { 1, 1, 1, 0 },
  [ATK_FLAV_MAJOR]     = { 1, 1, 1, 0 },
  [ATK_FLAV_CRITICAL]  = { 1, 1, 1, 0 },
  [ATK_FLAV_DEATH]     = { 1, 1, 0, 0 },
  [ATK_FLAV_DOT_TICK]  = { 1, 1, 1, 1 },
  [ATK_FLAV_DOT_DEATH] = { 1, 1, 0, 1 },
};

static const char *const atk_tok_name[4] = {
  ATK_TOK_ATTACKER, ATK_TOK_TARGET, ATK_TOK_DAMAGE, ATK_TOK_AFFLICT
};

static bool atk_llm_sanitize(atk_flavour_t, const char *, char *,
    size_t);

// ------------------------------------------------------------------ //
// Availability                                                        //
// ------------------------------------------------------------------ //

// The plugin_find() gate is load-bearing and must stay first: every
// llm_* entry point in inference.h is a dlsym shim that logs CLAM_FATAL
// and abort()s when the inference plugin is not loaded. Attack declares
// only `requires = method_text` — flavour is cosmetic, and the pit has
// to stay deployable on a daemon that has no LLM at all.
bool
atk_llm_enabled(const atk_tunables_t *t)
{
  llm_kind_t kind = LLM_KIND_CHAT;

  if(t == NULL || t->llm_model[0] == '\0')
    return(false);

  if(plugin_find("inference") == NULL)
    return(false);

  if(!llm_model_exists(t->llm_model))
    return(false);

  // llm_model_kind follows the project's SUCCESS(=false)/FAIL(=true)
  // convention while llm_model_exists above returns a natural bool.
  // A plain `!llm_model_kind(...)` inverts and rejects every real model.
  if(llm_model_kind(t->llm_model, &kind) != SUCCESS)
    return(false);

  return(kind == LLM_KIND_CHAT);
}

// Why the pit is speaking from its static tables, or NULL when a model
// is authoring. `show attack llm` says this out loud rather than leaving
// an operator to guess which of the four gates above closed.
const char *
atk_llm_offreason(const atk_tunables_t *t)
{
  llm_kind_t kind = LLM_KIND_CHAT;

  if(t == NULL || t->llm_model[0] == '\0')
    return("no model configured");

  if(plugin_find("inference") == NULL)
    return("the inference plugin is not loaded");

  if(!llm_model_exists(t->llm_model))
    return("no such model");

  if(llm_model_kind(t->llm_model, &kind) != SUCCESS)
    return("the model's kind could not be read");

  if(kind != LLM_KIND_CHAT)
    return("not a chat model");

  return(NULL);
}

// ------------------------------------------------------------------ //
// The sanitiser — the one place model text is trusted in              //
// ------------------------------------------------------------------ //

static bool
atk_tok_is(const char *tok, size_t len, const char *name)
{
  return(strlen(name) == len && strncmp(tok, name, len) == 0);
}

// Vet one model-authored line and normalise it into `out`. SUCCESS only
// when every rule below holds; the hot path then performs no validation
// of its own, because there is nothing left to validate.
static bool
atk_llm_sanitize(atk_flavour_t cat, const char *in, char *out,
    size_t cap)
{
  const char *why   = NULL;
  const char *end   = NULL;
  const char *p     = NULL;
  size_t      len   = 0;
  uint32_t    seen[4] = { 0 };
  uint32_t    tok;
  char        miscount[64];

  if(in == NULL || out == NULL || cap == 0 || cat >= ATK_FLAV__COUNT)
    return(FAIL);

  // 1. Trim.
  while(*in == ' ' || *in == '\t')
    in++;

  end = in + strlen(in);

  while(end > in && (end[-1] == ' ' || end[-1] == '\t'))
    end--;

  // 2. A leading list marker, which models add despite being told not
  //    to: "- ", "* ", "• ", "1. ", "3) ".
  if(end - in >= 2 && (in[0] == '-' || in[0] == '*') && in[1] == ' ')
    in += 2;

  else if(end - in >= 4 && strncmp(in, "\xe2\x80\xa2", 3) == 0 && in[3] == ' ')
    in += 4;

  else
  {
    p = in;

    while(p < end && *p >= '0' && *p <= '9')
      p++;

    if(p > in && p + 1 < end && (*p == '.' || *p == ')') && p[1] == ' ')
      in = p + 2;
  }

  while(in < end && (*in == ' ' || *in == '\t'))
    in++;

  // 3. One matched pair of surrounding quotes.
  if(end - in >= 2 && (*in == '"' || *in == '\'') && end[-1] == *in)
  {
    in++;
    end--;
  }

  len = (size_t)(end - in);

  // 4. Empty, or too long to survive expansion.
  if(len == 0)
    why = "empty";

  else if(len >= cap)
    why = "too long";

  // 5. Control bytes. This kills \r and \n — IRC protocol injection —
  //    and also \x01, the abstract colour marker atk_vis_len() treats
  //    as zero width: a model-supplied one would silently skew every
  //    column of `show attack`.
  for(p = in; why == NULL && p < end; p++)
  {
    if((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
      why = "control byte";
  }

  // 6. A percent sign. The template is passed as DATA and never reaches
  //    a printf conversion — but this costs one scan and removes the
  //    whole class of hazard from any future refactor that gets it
  //    wrong. Do not "simplify" this away.
  if(why == NULL && memchr(in, '%', len) != NULL)
    why = "percent sign";

  // 7. Every {…} must be one of the four known tokens, closed, and
  //    opened.
  for(p = in; why == NULL && p < end; p++)
  {
    const char *close = NULL;
    size_t      tlen  = 0;
    bool        known = false;

    if(*p == '}')
    {
      why = "unopened }";
      break;
    }

    if(*p != '{')
      continue;

    close = memchr(p + 1, '}', (size_t)(end - p - 1));
    if(close == NULL)
    {
      why = "unclosed {";
      break;
    }

    tlen = (size_t)(close - p - 1);

    for(tok = 0; tok < 4; tok++)
    {
      if(atk_tok_is(p + 1, tlen, atk_tok_name[tok]))
      {
        seen[tok]++;
        known = true;
        break;
      }
    }

    if(!known)
    {
      why = "unknown token";
      break;
    }

    p = close;
  }

  // 8. …and the observed count must equal what this category requires,
  //    for every token. One loop, no special cases: a {damage} in a
  //    death line and an {affliction} in a blow line are both simply
  //    counts that do not match the table.
  for(tok = 0; why == NULL && tok < 4; tok++)
  {
    if(seen[tok] != atk_tok_req[cat][tok])
    {
      snprintf(miscount, sizeof(miscount), "{%s} x%u, wanted x%u",
          atk_tok_name[tok], seen[tok],
          (unsigned)atk_tok_req[cat][tok]);
      why = miscount;
    }
  }

  if(why != NULL)
  {
    // CLAM_DEBUG, never CLAM_WARN: a chatty model would flood the log
    // one line at a time. atk_llm_done() logs the tally instead.
    clam(CLAM_DEBUG, ATK_CTX, "flavour %s: rejected line (%s)",
        atk_flav_name[cat], why);
    return(FAIL);
  }

  memcpy(out, in, len);
  out[len] = '\0';

  return(SUCCESS);
}

// ------------------------------------------------------------------ //
// Pool access                                                         //
// ------------------------------------------------------------------ //

bool
atk_pool_take(atk_flavour_t cat, char *out, size_t cap,
    uint32_t refill_at, bool *need_refill)
{
  atk_pool_t *pool = NULL;
  bool        got  = FAIL;

  if(need_refill != NULL)
    *need_refill = false;

  if(cat >= ATK_FLAV__COUNT || out == NULL || cap == 0)
    return(FAIL);

  pool = &atk_pools[cat];

  pthread_mutex_lock(&atk_pool_lock);

  // Swap-remove: draining rather than sampling with replacement is the
  // point — within one pool generation the pit never repeats itself,
  // and the drain is what signals the refill.
  if(pool->count > 0)
  {
    uint16_t idx = (uint16_t)util_rand((int)pool->count);

    snprintf(out, cap, "%s", pool->lines[idx]);
    pool->count--;

    if(idx != pool->count)
      memcpy(pool->lines[idx], pool->lines[pool->count],
          sizeof(pool->lines[idx]));

    pool->served++;
    got = SUCCESS;
  }

  if(need_refill != NULL && pool->count <= refill_at)
    *need_refill = true;

  pthread_mutex_unlock(&atk_pool_lock);

  return(got);
}

// Bumped by the renderer whenever the pool came up dry and the static
// tables spoke instead; `show attack llm` reports the tally.
void
atk_pool_fallback(void)
{
  pthread_mutex_lock(&atk_pool_lock);
  atk_fallbacks++;
  pthread_mutex_unlock(&atk_pool_lock);
}

// A read-only snapshot for `show attack llm`. Takes atk_pool_lock and
// nothing else — the views never touch atk_turn_lock.
void
atk_pool_stats(atk_flavour_t cat, atk_pool_stat_t *out)
{
  const atk_pool_t *pool = NULL;
  time_t            now  = time(NULL);

  if(out == NULL)
    return;

  memset(out, 0, sizeof(*out));

  if(cat >= ATK_FLAV__COUNT)
    return;

  pool = &atk_pools[cat];

  pthread_mutex_lock(&atk_pool_lock);
  out->depth    = pool->count;
  out->served   = pool->served;
  out->rejected = pool->rejected;
  out->inflight = pool->inflight;
  out->wait     = (pool->next_try > now) ? (int64_t)(pool->next_try - now) : 0;
  snprintf(out->last_error, sizeof(out->last_error), "%s", pool->last_error);
  pthread_mutex_unlock(&atk_pool_lock);
}

uint64_t
atk_pool_fallbacks(void)
{
  uint64_t n;

  pthread_mutex_lock(&atk_pool_lock);
  n = atk_fallbacks;
  pthread_mutex_unlock(&atk_pool_lock);

  return(n);
}

// ------------------------------------------------------------------ //
// Token expansion                                                     //
// ------------------------------------------------------------------ //

void
atk_tmpl_expand(char *out, size_t cap, const char *tmpl,
    const char *attacker, const char *target, const char *damage,
    const char *affliction)
{
  size_t used = 0;

  if(out == NULL || cap == 0)
    return;

  out[0] = '\0';

  if(tmpl == NULL)
    return;

  // A NULL substitution is a token the sanitiser guaranteed absent from
  // this category — reachable only through a bug, and then harmlessly:
  // it expands to nothing rather than leaving a brace token on screen.
  if(attacker   == NULL) attacker   = "";
  if(target     == NULL) target     = "";
  if(damage     == NULL) damage     = "";
  if(affliction == NULL) affliction = "";

  // One forward scan over the TEMPLATE. The substituted values are
  // written straight out and are never re-examined, so a `{` inside a
  // nickname passes through as the literal byte it is. That is
  // structural here, not incidental — keep it that way.
  while(*tmpl != '\0' && used + 1 < cap)
  {
    const char *sub   = NULL;
    const char *close = NULL;
    size_t      tlen  = 0;
    size_t      slen  = 0;

    if(*tmpl == '{' && (close = strchr(tmpl + 1, '}')) != NULL)
    {
      tlen = (size_t)(close - tmpl - 1);

      if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_ATTACKER))
        sub = attacker;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_TARGET))
        sub = target;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_DAMAGE))
        sub = damage;

      else if(atk_tok_is(tmpl + 1, tlen, ATK_TOK_AFFLICT))
        sub = affliction;
    }

    if(sub == NULL)
    {
      out[used++] = *tmpl++;
      continue;
    }

    slen = strlen(sub);

    if(slen > cap - used - 1)
      slen = cap - used - 1;

    memcpy(out + used, sub, slen);
    used += slen;
    tmpl  = close + 1;
  }

  // Truncation is silent and acceptable: the sanitiser already bounded
  // the template, and this is the last line of defence, not the first.
  out[used] = '\0';
}

// ------------------------------------------------------------------ //
// The refill                                                          //
// ------------------------------------------------------------------ //

// The machine-readable half of the prompt. It lives in C, not in the
// persona file, so an operator rewriting the persona can never break
// parsing. %u is attack's own format string, interpolated into a scratch
// buffer; the model's output never reaches a conversion.
//
// The four damage tiers share every rule but the last one, which is the
// whole point of the tiering: the force described has to match the
// number the roll produced. The shared half is a macro rather than four
// copies so the rules cannot drift apart tier by tier.
#define ATK_FMT_BLOW_COMMON \
  "Write exactly %u lines of combat flavour. One line per line of output.\n" \
  "\n" \
  "Every line MUST contain the three placeholder tokens {attacker}, {target}\n" \
  "and {damage}, spelled exactly like that in curly braces, each appearing\n" \
  "exactly once. A number is substituted for {damage}, so write it so it\n" \
  "reads naturally -- for example \"for {damage} damage\".\n" \
  "\n" \
  "Rules, all mandatory:\n" \
  "- Output ONLY the lines themselves. No numbering, no bullets, no blank\n" \
  "  lines, no preamble, no commentary, no quotation marks around lines.\n" \
  "- One sentence per line, at most 150 characters.\n" \
  "- Plain text only. No markdown, no emoji, no percent signs, and no curly\n" \
  "  braces other than the three tokens named above.\n" \
  "- Every line must differ from every other line.\n"

static const char *const atk_fmt_minor =
  ATK_FMT_BLOW_COMMON
  "- These are GLANCING blows: the weakest hits in the game. A graze, a\n"
  "  scuff, a stinging insult of a strike. The target is barely\n"
  "  inconvenienced and is never in danger.\n"
  "- Match the words to the number: nothing here may sound like a wound\n"
  "  that would end a fight.\n";

static const char *const atk_fmt_medium =
  ATK_FMT_BLOW_COMMON
  "- These are SOLID blows: a clean, ordinary hit that hurts and does real\n"
  "  damage, but that a fighter shrugs off and keeps going through.\n"
  "- Match the words to the number: no dismemberment, no bones through\n"
  "  skin, no talk of dying.\n";

static const char *const atk_fmt_major =
  ATK_FMT_BLOW_COMMON
  "- These are HEAVY blows: bone, blood and stagger. The target is badly\n"
  "  hurt and visibly losing, but SURVIVES the hit.\n"
  "- Match the words to the number: this must sound worse than an ordinary\n"
  "  hit and less than a killing one.\n";

static const char *const atk_fmt_critical =
  ATK_FMT_BLOW_COMMON
  "- These are DEVASTATING blows: the heaviest damage in the game,\n"
  "  unusually brutal -- but still NON-fatal. The target survives.\n"
  "- End each line with the damage clause so it lands hard.\n";

static const char *const atk_fmt_death =
  "Write exactly %u lines of combat flavour. One line per line of output.\n"
  "\n"
  "Every line MUST contain the two placeholder tokens {attacker} and\n"
  "{target}, spelled exactly like that in curly braces, each appearing\n"
  "exactly once.\n"
  "\n"
  "Rules, all mandatory:\n"
  "- Output ONLY the lines themselves. No numbering, no bullets, no blank\n"
  "  lines, no preamble, no commentary, no quotation marks around lines.\n"
  "- One sentence per line, at most 150 characters.\n"
  "- Plain text only. No markdown, no emoji, no percent signs, and no curly\n"
  "  braces other than the two tokens named above.\n"
  "- Every line must differ from every other line.\n"
  "- These are KILLING blows. {attacker} has just killed {target}. Do not\n"
  "  mention a damage number and do not use a {damage} token; the round is\n"
  "  already over.\n";

// An affliction names itself through {affliction}, which the model does
// not choose and must not gloss: the noun is substituted whole, so a
// line that writes "the venom {affliction}" reads as nonsense the moment
// the wound is spores.
static const char *const atk_fmt_dot_tick =
  "Write exactly %u lines of combat flavour. One line per line of output.\n"
  "\n"
  "Every line MUST contain the four placeholder tokens {attacker},\n"
  "{target}, {damage} and {affliction}, spelled exactly like that in curly\n"
  "braces, each appearing exactly once.\n"
  "\n"
  "Rules, all mandatory:\n"
  "- Output ONLY the lines themselves. No numbering, no bullets, no blank\n"
  "  lines, no preamble, no commentary, no quotation marks around lines.\n"
  "- One sentence per line, at most 150 characters.\n"
  "- Plain text only. No markdown, no emoji, no percent signs, and no curly\n"
  "  braces other than the four tokens named above.\n"
  "- Every line must differ from every other line.\n"
  "- {affliction} is replaced by a NOUN PHRASE naming a lingering wound --\n"
  "  for example \"open wound\", \"spider venom\", \"myconid spores\". You do\n"
  "  not choose it and you must not describe it: write \"the {affliction}\n"
  "  eats at {target}\", never \"the venom {affliction}\". It must read\n"
  "  correctly for any such noun.\n"
  "- Nobody is swinging. {attacker} left this wound EARLIER and may be\n"
  "  across the room; the wound itself is doing the work now.\n"
  "- {target} SURVIVES this line. It hurts and it is grim, but it does not\n"
  "  kill.\n";

static const char *const atk_fmt_dot_death =
  "Write exactly %u lines of combat flavour. One line per line of output.\n"
  "\n"
  "Every line MUST contain the three placeholder tokens {attacker},\n"
  "{target} and {affliction}, spelled exactly like that in curly braces,\n"
  "each appearing exactly once.\n"
  "\n"
  "Rules, all mandatory:\n"
  "- Output ONLY the lines themselves. No numbering, no bullets, no blank\n"
  "  lines, no preamble, no commentary, no quotation marks around lines.\n"
  "- One sentence per line, at most 150 characters.\n"
  "- Plain text only. No markdown, no emoji, no percent signs, and no curly\n"
  "  braces other than the three tokens named above.\n"
  "- Every line must differ from every other line.\n"
  "- {affliction} is replaced by a NOUN PHRASE naming a lingering wound --\n"
  "  for example \"open wound\", \"spider venom\", \"myconid spores\". You do\n"
  "  not choose it and you must not describe it. It must read correctly for\n"
  "  any such noun.\n"
  "- This is the moment {target} DIES, killed by the {affliction} that\n"
  "  {attacker} left in them some time ago -- not by a blow. Do not mention\n"
  "  a damage number and do not use a {damage} token; the round is over.\n";

static const char *
atk_fmt_for(atk_flavour_t cat)
{
  switch(cat)
  {
    case ATK_FLAV_MINOR:     return(atk_fmt_minor);
    case ATK_FLAV_MEDIUM:    return(atk_fmt_medium);
    case ATK_FLAV_MAJOR:     return(atk_fmt_major);
    case ATK_FLAV_CRITICAL:  return(atk_fmt_critical);
    case ATK_FLAV_DEATH:     return(atk_fmt_death);
    case ATK_FLAV_DOT_TICK:  return(atk_fmt_dot_tick);
    case ATK_FLAV_DOT_DEATH: return(atk_fmt_dot_death);
    default:                   return(atk_fmt_medium);
  }
}

// What the background task needs. Heap-owned by the task and freed by
// it; the tunables travel by value so the task reads no KV of its own.
typedef struct
{
  atk_flavour_t   cat;
  atk_tunables_t  t;
} atk_refill_t;

// What the LLM round-trip needs. Separately allocated from the task's
// own payload, because the task frees itself long before the model
// answers.
typedef struct
{
  atk_flavour_t  cat;
  uint32_t       pool_size;
  uint32_t       retry_secs;
  uint32_t       gen;   // generation this request was submitted under
} atk_fill_ctx_t;

static void
atk_pool_fail(atk_flavour_t cat, const char *why, uint32_t retry_secs)
{
  pthread_mutex_lock(&atk_pool_lock);
  snprintf(atk_pools[cat].last_error, sizeof(atk_pools[cat].last_error),
      "%s", why != NULL ? why : "unknown error");
  atk_pools[cat].next_try = time(NULL) + (time_t)retry_secs;
  atk_pools[cat].inflight = false;
  pthread_mutex_unlock(&atk_pool_lock);
}

// Runs on a curl worker. Light work only: split, sanitise, install.
static void
atk_llm_done(const llm_chat_response_t *resp)
{
  atk_fill_ctx_t *fc       = NULL;
  atk_pool_t     *pool     = NULL;
  const char     *p        = NULL;
  uint32_t        accepted = 0;
  uint32_t        rejected = 0;
  uint16_t        depth    = 0;
  bool            stale    = false;

  if(resp == NULL || resp->user_data == NULL)
    return;

  fc   = resp->user_data;
  pool = &atk_pools[fc->cat];

  // These lines were written under a preamble that no longer applies, so
  // nothing here may touch the pool's contents, its backoff, or its
  // last_error — a reset pool must not inherit a discarded request's
  // failure. Reading a plain uint32_t under the lock is the project's
  // discipline, not a performance question.
  pthread_mutex_lock(&atk_pool_lock);

  if(fc->gen != atk_pool_gen)
  {
    pool->inflight = false;
    pool->next_try = 0;
    stale = true;
  }

  pthread_mutex_unlock(&atk_pool_lock);

  if(stale)
  {
    const atk_flavour_t cat = fc->cat;
    atk_tunables_t      t;

    clam(CLAM_INFO, ATK_CTX,
        "flavour %s: refill discarded (preamble changed under it)",
        atk_flav_name[cat]);

    // The tunables are read FRESH, not taken from fc->t. fc->t is what
    // was in force when this request was submitted — the very preamble
    // the answer was just discarded for — so re-kicking with it would
    // regenerate the category under the persona the operator has just
    // moved away from, and the pit would keep speaking in the old voice
    // with the KV insisting otherwise. Cost is one kv read on a curl
    // worker: a mutex, no I/O; the persona file is not touched until
    // the refill task runs on its own thread.
    atk_tunables_load(&t);

    // Copy `cat` before the free, kick after it. The kick is a queue
    // insert and is safe here; it re-arms the category that
    // atk_llm_invalidate() deliberately left alone, and it cannot
    // storm — the generation only moves on an operator action.
    mem_free(fc);
    atk_llm_refill_kick(cat, &t);
    return;
  }

  if(!resp->ok || resp->content == NULL)
  {
    clam(CLAM_WARN, ATK_CTX, "flavour %s: refill failed (%s)",
        atk_flav_name[fc->cat],
        resp->error != NULL ? resp->error : "no response");
    atk_pool_fail(fc->cat, resp->error, fc->retry_secs);
    mem_free(fc);
    return;
  }

  // `resp` is valid only for the duration of this callback, so every
  // accepted line is copied into the pool before we return.
  for(p = resp->content; *p != '\0'; )
  {
    const char *nl  = strchr(p, '\n');
    size_t      seg = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
    char        raw [ATK_LLM_TMPL_SZ * 2];
    char        tmpl[ATK_LLM_TMPL_SZ];

    if(seg > 0 && p[seg - 1] == '\r')   // provider CRLF
      seg--;

    if(seg >= sizeof(raw))
      seg = sizeof(raw) - 1;

    memcpy(raw, p, seg);
    raw[seg] = '\0';

    p = (nl != NULL) ? nl + 1 : p + strlen(p);

    if(raw[0] == '\0')
      continue;

    if(atk_llm_sanitize(fc->cat, raw, tmpl, sizeof(tmpl)) != SUCCESS)
    {
      rejected++;
      continue;
    }

    pthread_mutex_lock(&atk_pool_lock);

    if(pool->count < fc->pool_size && pool->count < ATK_LLM_POOL_MAX)
    {
      snprintf(pool->lines[pool->count], ATK_LLM_TMPL_SZ, "%s", tmpl);
      pool->count++;
      accepted++;
    }

    pthread_mutex_unlock(&atk_pool_lock);
  }

  // A model that ignores the format block must not be retried in a
  // tight loop, so an all-rejected answer counts as a failure.
  if(accepted == 0)
  {
    clam(CLAM_WARN, ATK_CTX,
        "flavour %s: refill produced nothing usable (%u rejected, "
        "finish_reason %s)", atk_flav_name[fc->cat], rejected,
        resp->finish_reason != NULL ? resp->finish_reason : "?");
    atk_pool_fail(fc->cat, "model returned no usable lines",
        fc->retry_secs);
    mem_free(fc);
    return;
  }

  pthread_mutex_lock(&atk_pool_lock);
  pool->rejected += rejected;
  pool->inflight  = false;
  pool->next_try  = 0;
  pool->last_error[0] = '\0';
  depth = pool->count;
  pthread_mutex_unlock(&atk_pool_lock);

  // A finish_reason of "length" means max_tokens truncated the final
  // line, which then simply failed sanitisation — the first thing to
  // suspect when accepted counts run one short.
  clam(CLAM_INFO, ATK_CTX,
      "flavour %s: refilled +%u (rejected %u, depth %u, finish %s)",
      atk_flav_name[fc->cat], accepted, rejected, (unsigned)depth,
      resp->finish_reason != NULL ? resp->finish_reason : "?");

  mem_free(fc);
}

// Reads the persona off disk and submits. This is why the refill is a
// task and not a bare submit: llm_chat_submit is already async, but
// building the request is blocking file I/O and must never happen on a
// command worker, let alone under a lock.
static void
atk_llm_refill_task(task_t *t)
{
  atk_refill_t     *r    = NULL;
  atk_fill_ctx_t   *fc   = NULL;
  llm_message_t     msgs[2] = {0};   // MUST be zero-initialised: the
                                     // .blocks pointer would otherwise
                                     // read stack garbage.
  llm_chat_params_t p;
  char              persona[4096];
  char              sys[8192];
  char              usr[64];
  char              rules[2048];
  size_t            n = 0;
  FILE             *fp = NULL;

  if(t == NULL || t->data == NULL)
  {
    if(t != NULL)
      t->state = TASK_ENDED;

    return;
  }

  r          = t->data;
  persona[0] = '\0';

  if(r->t.llm_prompt[0] != '\0')
    fp = fopen(r->t.llm_prompt, "r");

  if(fp != NULL)
  {
    n = fread(persona, 1, sizeof(persona) - 1, fp);
    fclose(fp);
    persona[n] = '\0';

    while(n > 0 && (persona[n - 1] == '\n' || persona[n - 1] == '\r'))
      persona[--n] = '\0';
  }

  // A missing persona file is not fatal — the pit then speaks in the
  // model's own voice — but it is almost always a typo, and two shipped
  // personas mean twice the chance of one. CLAM_WARN, not CLAM_DEBUG:
  // this fires once per refill, not once per line.
  else if(r->t.llm_prompt[0] != '\0')
    clam(CLAM_WARN, ATK_CTX, "persona file unreadable: '%s' (the model "
        "will write in its own voice)", r->t.llm_prompt);

  snprintf(rules, sizeof(rules), atk_fmt_for(r->cat), r->t.llm_pool);
  snprintf(sys, sizeof(sys), "%s%s%s", persona,
      persona[0] != '\0' ? "\n\n" : "", rules);
  snprintf(usr, sizeof(usr), "Write %u lines now.", r->t.llm_pool);

  msgs[0].role = LLM_ROLE_SYSTEM;
  msgs[0].content = sys;
  msgs[1].role = LLM_ROLE_USER;
  msgs[1].content = usr;

  memset(&p, 0, sizeof(p));
  p.temperature  = (float)r->t.llm_temp_pct / 100.0f;
  p.max_tokens   = r->t.llm_max_tokens;
  p.timeout_secs = r->t.llm_timeout;
  p.stream       = false;

  fc = mem_alloc(ATK_CTX, "atk_fill_ctx", sizeof(*fc));
  fc->cat        = r->cat;
  fc->pool_size  = r->t.llm_pool;
  fc->retry_secs = r->t.llm_retry;

  pthread_mutex_lock(&atk_pool_lock);
  fc->gen = atk_pool_gen;
  pthread_mutex_unlock(&atk_pool_lock);

  // msgs[] and both prompt buffers may be stack-local: llm_chat_submit
  // copies everything before it returns.
  if(llm_chat_submit(r->t.llm_model, &p, msgs, 2, atk_llm_done, NULL,
        fc) != SUCCESS)
  {
    clam(CLAM_WARN, ATK_CTX, "flavour %s: submit refused",
        atk_flav_name[r->cat]);
    atk_pool_fail(r->cat, "submit refused", r->t.llm_retry);
    mem_free(fc);
  }

  mem_free(r);
  t->state = TASK_ENDED;
}

// Idempotent and cheap: a no-op when the feature is off, when a refill
// is already in flight, or while the failure backoff is still running.
// Safe to call from the turn path — task_add is a queue insert, never
// I/O — but the turn path deliberately calls it only after it has
// dropped atk_turn_lock.
void
atk_llm_refill_kick(atk_flavour_t cat, const atk_tunables_t *t)
{
  atk_refill_t *r    = NULL;
  bool          arm  = false;

  if(cat >= ATK_FLAV__COUNT || !atk_llm_enabled(t))
    return;

  pthread_mutex_lock(&atk_pool_lock);

  if(!atk_pools[cat].inflight &&
     atk_pools[cat].next_try <= time(NULL) &&
     atk_pools[cat].count < t->llm_pool)
  {
    atk_pools[cat].inflight = true;
    arm = true;
  }

  pthread_mutex_unlock(&atk_pool_lock);

  if(!arm)
    return;

  r      = mem_alloc(ATK_CTX, "atk_refill", sizeof(*r));
  r->cat = cat;
  r->t   = *t;

  // Priority 200 matches the other opportunistic background work. The
  // failure path below is mandatory: miss it and this category never
  // refills again for the life of the daemon.
  if(task_add("attack.llm.fill", TASK_THREAD, 200, atk_llm_refill_task,
        r) == NULL)
  {
    mem_free(r);

    pthread_mutex_lock(&atk_pool_lock);
    atk_pools[cat].inflight = false;
    pthread_mutex_unlock(&atk_pool_lock);

    clam(CLAM_WARN, ATK_CTX, "flavour %s: could not queue refill",
        atk_flav_name[cat]);
  }
}

// ------------------------------------------------------------------ //
// Invalidation                                                        //
// ------------------------------------------------------------------ //

// Between the clear and the first refill landing the pools are empty and
// the pit speaks from the static tables. That is correct — it is what
// the fallback is for — and `show attack llm` will tick its fallbacks
// counter while it lasts.
void
atk_llm_invalidate(const char *why)
{
  atk_tunables_t t;
  atk_flavour_t  cat;

  pthread_mutex_lock(&atk_pool_lock);

  for(cat = ATK_FLAV_MINOR; cat < ATK_FLAV__COUNT; cat++)
  {
    atk_pools[cat].count         = 0;
    atk_pools[cat].next_try      = 0;
    atk_pools[cat].last_error[0] = '\0';

    // `inflight` is deliberately left alone: that request is already at
    // the provider and its callback owns the flag. The generation bump
    // below is what makes its answer harmless.
  }

  atk_pool_gen++;
  pthread_mutex_unlock(&atk_pool_lock);

  clam(CLAM_INFO, ATK_CTX, "flavour: pools cleared (%s), regenerating",
      why != NULL ? why : "reason unstated");

  // Outside the lock. Categories with a request still in flight will not
  // re-arm here; their discarded-response path re-arms them instead,
  // which is the entire reason that path kicks.
  atk_tunables_load(&t);
  atk_llm_prime(&t);
}

// kv fires this outside the KV lock and only on an actual value change,
// so it may read KV and queue tasks freely. Verified against core/kv.c:
// apply_val() releases kv_mutex before firing and gates on val_changed();
// kv_load() does the same, and these callbacks are installed in start(),
// which runs after kv_load() — so boot never fires them.
static void
atk_llm_kv_changed(const char *key, void *data)
{
  (void)data;

  atk_llm_invalidate(key != NULL ? key : "configuration changed");
}

void
atk_llm_watch(bool on)
{
  kv_set_cb(ATK_KV_LLM_PROMPT, on ? atk_llm_kv_changed : NULL, NULL);
  kv_set_cb(ATK_KV_LLM_MODEL,  on ? atk_llm_kv_changed : NULL, NULL);
}

// Fill all five pools before the first blow rather than after it. That
// is five concurrent chat requests against whatever service the operator
// configured; they are deliberately not staggered, and a provider that
// rate-limits us will show up as failed refills in `show attack llm`.
void
atk_llm_prime(const atk_tunables_t *t)
{
  atk_flavour_t cat;

  if(!atk_llm_enabled(t))
    return;

  clam(CLAM_INFO, ATK_CTX, "flavour: priming from model '%s'",
      t->llm_model);

  for(cat = ATK_FLAV_MINOR; cat < ATK_FLAV__COUNT; cat++)
    atk_llm_refill_kick(cat, t);
}
