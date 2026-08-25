#ifndef BM_WIKIMEDIA_API_H
#define BM_WIKIMEDIA_API_H

// Public mechanism API for the wikimedia service plugin — keyless access
// to Wikidata (structured claims) and Wikipedia (prose), which are two
// APIs on two hosts joined by one identifier: the QID. Consumers include
// this header and resolve the symbols at runtime via
// plugin_dlsym_cached(WIKIMEDIA_CTX, …, (void **)&cached) — the plugin is
// loaded RTLD_LOCAL. Pure mechanism: this plugin fetches, ranks and
// normalizes; every byte of user-facing presentation belongs to the
// consumer.
//
// Three gates, as searxng_api.h has:
//
//   - default             types + abort-on-miss dlsym shims. For a
//                         consumer that treats a missing wikimedia
//                         plugin as a startup misconfiguration.
//   - WIKIMEDIA_INTERNAL  types + the real prototypes, no shims. The
//                         plugin's own translation units.
//   - WIKIMEDIA_TYPES_ONLY types alone. For a consumer that degrades
//                         rather than dies — it runs its own
//                         plugin_dlsym with WARN-and-skip semantics.
//
// ----------------------------------------------------------------------
// What this plugin will not do
// ----------------------------------------------------------------------
//
// SPARQL (query.wikidata.org) is refused as a transport, on latency
// variance rather than etiquette: five samples of one trivial three-IRI
// query drew 0.098s, 0.33s, 1.87s, 4.29s and 108.0s, where the action
// API answered in 0.144–0.181s across the same session. Everything here
// rides api.php.
//
// ----------------------------------------------------------------------
// The backoff, and why a refusal is not an answer
// ----------------------------------------------------------------------
//
// Wikimedia rate-limits by IP at the edge and says so plainly: HTTP 429
// with `x-envoy-ratelimited: true` and a `Retry-After` in seconds. A
// handful of !wiki commands inside a minute is enough to earn one.
//
// That Retry-After is honoured. The first 429 arms a process-wide
// backoff, and until it expires every verb here refuses IN PLACE with
// WM_RATE_LIMITED and puts nothing on the wire — a request sent inside
// the window earns a longer one. The backoff is deliberately not
// per-host: the limit is applied to our address rather than to an
// endpoint, so Wikidata's refusal is Wikipedia's too. A cache hit is
// still served; it costs no traffic.
//
// ⛔ WM_RATE_LIMITED, like WM_TRANSPORT, is the ABSENCE of an answer and
// never one. Reading either as "no value recorded" is how a channel gets
// told a living person is dead.
//
// Nothing here retries for the caller. A backoff outlives most single
// requests, and the only party that knows whether anyone is still
// waiting for this answer is the caller — so the service reports the
// wait and lets whoever asked decide.
//
// ⛔⛔ The same rule binds every fan-out INSIDE this service, and it is
// the one that is easy to get wrong: WM_NOT_FOUND may be reported only
// when EVERY leg answered. Three verbs fan out — the merged resolver's
// two searchers, the claims candidate set, the reverse candidate set —
// and each of them used to report an empty result as WM_NOT_FOUND once
// ANY leg had answered. A refused leg looked at nothing, so its silence
// is not evidence of absence: half a candidate window came back as
// Wikidata's considered opinion that no such thing exists, and a
// three-property fan-out with one property fetched came back as "this
// item records no height".
//
// ⚠ WM_OK is 0 and the work is zeroed, so "nobody answered yet" has to
// be written down: every fan-out seeds its per-leg status slots to
// WM_TRANSPORT BEFORE its first launch — after one, a leg completing on
// a curl worker would race its own answer back to unread — and each leg
// writes only its own slot, which is also what keeps two legs off one
// shared flag.
//
// ----------------------------------------------------------------------
// Normalization the service owns (the consumer only presents)
// ----------------------------------------------------------------------
//
// Absent, unknown and none are THREE answers, never two. A claim whose
// snak carries a value is a fact; WM_VAL_UNKNOWN means the statement
// exists and the value is not recorded; WM_VAL_NONE means it is recorded
// as having none; and a property with no statements at all comes back
// WM_NOT_FOUND, meaning nothing is known. Collapsing any pair of those
// reproduces the defect that killed the original !isdead design, where
// an empty death date meant both "alive" and "nobody filled this in".
//
// Dates carry their precision and are never padded. Hypatia's death is
// 0415-03 at precision 10 — March 415, not the 1st of March.
//
// Deprecated statements are dropped, preferred ones sort ahead of
// normal ones, and a property that repeats the same value on several
// statements is delivered once.
//
// Every payload is valid for the duration of the callback only;
// wm_prose_res_t.text in particular points into the transfer buffer.
// Consumers that outlive the call deep-copy.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "async.h"

// The plugin's name, and so both its dlsym handle and the root of its
// clam contexts. Public because the shims below resolve against it.
#define WIKIMEDIA_CTX "wikimedia"

// ----------------------------------------------------------------------
// Fixed sizes. Fixed-width strings let consumers stack-allocate results
// and sidestep inner-string lifetime ambiguity.
// ----------------------------------------------------------------------

#define WM_QID_SZ         16   // "Q47871", "P569"
#define WM_LABEL_SZ       96
#define WM_DESC_SZ        192
#define WM_TITLE_SZ       128  // a wiki article title
#define WM_VALUE_SZ       160  // one rendered statement value
#define WM_DTYPE_SZ       32   // "wikibase-item", "time", "external-id", …
#define WM_DATE_SZ        16   // "-0044-03-15" and every shorter precision
#define WM_MSG_SZ         128
#define WM_PAGE_URL_SZ    640  // an article's own address: the host plus
                               // an encoded WM_TITLE_SZ title, with room

#define WM_CANDIDATES_MAX 20   // the merged window, and a reverse query's
#define WM_CLAIMS_MAX     12   // statements carried for one property
#define WM_QUALS_MAX      3    // qualifiers carried per statement
#define WM_MENU_MAX       32   // properties carried from a P1963 menu

// An English word resolves to a small ranked set of properties, not to
// one answer, and which of them wins is decided by the subject rather
// than by the word. Three is where the measured misses live: "height"
// puts the wanted P2048 at rank 2, behind P2044 (elevation).
#define WM_PROP_CANDIDATES 3

// A fact block: how many properties of an entity it carries, and how
// many statements of each. Both are display budgets rather than limits
// of the data -- wm_fact_t.total says what was left behind.
#define WM_FACTS_MAX        24
#define WM_FACT_VALUES_MAX  3

typedef enum
{
  WM_OK = 0,
  WM_NOT_FOUND,    // resolved cleanly to nothing — an empty, not a bad guess
  WM_TRANSPORT,    // the wire, or an answer this plugin could not parse
  WM_RATE_LIMITED, // Wikimedia is throttling us; wm_retry_after() says for
                   // how long, and NOTHING reached the wire to earn it
  WM_UNAVAILABLE   // refused locally: the plugin is stopping
} wm_status_t;

// ----------------------------------------------------------------------
// A resolved candidate. `sitelinks` is the count of wikis carrying an
// article for this item, and it is the ranking key — the merged window
// is sorted by it, so hits[0] is the winner.
// ----------------------------------------------------------------------

typedef struct
{
  char    qid[WM_QID_SZ];
  char    label[WM_LABEL_SZ];
  char    description[WM_DESC_SZ];
  char    title[WM_TITLE_SZ];   // the local-language article ("" if none)
  int32_t sitelinks;
} wm_candidate_t;

// ----------------------------------------------------------------------
// A statement value.
// ----------------------------------------------------------------------

typedef enum
{
  WM_VAL_UNKNOWN = 0,  // snaktype somevalue — recorded as not known
  WM_VAL_NONE,         // snaktype novalue — recorded as having none
  WM_VAL_ITEM,
  WM_VAL_TIME,
  WM_VAL_QUANTITY,
  WM_VAL_COORD,
  WM_VAL_TEXT          // string, monolingual text, URL, external id
} wm_value_kind_t;

typedef struct
{
  char    text[WM_DATE_SZ];  // "1879-03-14", "0415-03", "1982" — never padded
  uint8_t precision;         // 11 day, 10 month, 9 year, coarser below
} wm_time_t;

typedef struct
{
  wm_value_kind_t kind;

  // The rendered value, always the field a consumer prints. For an item
  // it is the English label (or the QID, if the label lookup could not
  // reach it); for a quantity it carries the unit; for a time it is
  // `time.text`. "" for UNKNOWN and NONE — those two say everything in
  // `kind`.
  char            text[WM_VALUE_SZ];

  char            qid[WM_QID_SZ];   // ITEM: the item
  wm_time_t       time;             // TIME
  double          amount;           // QUANTITY
  char            unit[WM_LABEL_SZ];// QUANTITY: unit label ("" if unitless)
  double          lat;              // COORD
  double          lon;              // COORD
} wm_value_t;

typedef struct
{
  char       property[WM_QID_SZ];
  char       label[WM_LABEL_SZ];   // "start time" ("" if unresolved)
  wm_value_t value;
} wm_qualifier_t;

typedef struct
{
  wm_value_t     value;
  bool           preferred;        // rank == preferred
  wm_qualifier_t quals[WM_QUALS_MAX];
  uint8_t        n_quals;
} wm_claim_t;

// ----------------------------------------------------------------------
// One entry of a class's P1963 property menu — "what can be asked about
// a thing of this type", curated by Wikidata and priority-ordered.
// `external-id` properties are dropped on the way through: an identifier
// is a cross-reference key, never an answer.
// ----------------------------------------------------------------------

typedef struct
{
  char property[WM_QID_SZ];
  char label[WM_LABEL_SZ];
  char datatype[WM_DTYPE_SZ];
} wm_property_t;

// ----------------------------------------------------------------------
// Result envelopes. `message` carries a human-readable detail on
// failure and is "" on success.
// ----------------------------------------------------------------------

typedef struct
{
  wm_status_t    status;
  char           message[WM_MSG_SZ];
  wm_candidate_t hits[WM_CANDIDATES_MAX];  // winner first
  uint8_t        n;

  // Matches the searcher claimed, which is the count wm_reverse_async
  // was asked about rather than the `n` it could carry. 0 where no
  // searcher reported one, which is every wm_resolve_async answer.
  int32_t        total;
} wm_resolve_res_t;

typedef struct
{
  wm_status_t status;
  char        message[WM_MSG_SZ];
  char        entity[WM_QID_SZ];    // the item asked about
  char        property[WM_QID_SZ];  // the property that answered
  char        label[WM_LABEL_SZ];   // its English label
  char        datatype[WM_DTYPE_SZ];
  wm_claim_t  claims[WM_CLAIMS_MAX];
  uint8_t     n;
} wm_claims_res_t;

typedef struct
{
  wm_status_t   status;
  char          message[WM_MSG_SZ];
  char          class_qid[WM_QID_SZ];
  wm_property_t props[WM_MENU_MAX];
  uint8_t       n;
} wm_menu_res_t;

// One English word's ranked property candidates -- the step
// wm_claims_async takes internally, exposed because a consumer parsing
// free text has to know whether a word NAMES a property before it can
// decide which words were the subject.
typedef struct
{
  wm_status_t   status;
  char          message[WM_MSG_SZ];
  char          word[WM_LABEL_SZ];   // as normalized
  wm_property_t cand[WM_PROP_CANDIDATES];
  uint8_t       n;
} wm_property_res_t;

// One property of an entity and the values it holds. `total` is what
// the entity actually has, so a consumer can say "+7" rather than
// pretend the list is complete.
typedef struct
{
  char       property[WM_QID_SZ];
  char       label[WM_LABEL_SZ];
  char       datatype[WM_DTYPE_SZ];
  wm_value_t values[WM_FACT_VALUES_MAX];
  uint8_t    n;
  uint8_t    total;
} wm_fact_t;

// An entity's facts, ordered by the P1963 menu of its type and
// restricted to properties it has values for. Qualifiers are NOT
// carried: a fact block is a summary, and the statement a consumer
// wants qualified is the one it asked for by name through
// wm_claims_async.
typedef struct
{
  wm_status_t status;
  char        message[WM_MSG_SZ];
  char        qid[WM_QID_SZ];
  char        label[WM_LABEL_SZ];
  char        description[WM_DESC_SZ];
  char        class_qid[WM_QID_SZ];    // the P31 whose menu ordered this
  char        class_label[WM_LABEL_SZ];
  wm_fact_t   facts[WM_FACTS_MAX];
  uint8_t     n;
} wm_facts_res_t;

typedef struct
{
  wm_status_t status;
  char        message[WM_MSG_SZ];
  char        title[WM_TITLE_SZ];
  char        qid[WM_QID_SZ];          // the article's item ("" if none)
  char        description[WM_DESC_SZ]; // summary only

  // Where this article lives, built from the title the wiki settled on
  // rather than the one that was asked for -- a redirect answers under
  // its target. The host is this service's business (the language is a
  // knob), which is why the consumer is handed the address instead of
  // assembling one, and a consumer filing the prose somewhere needs it:
  // an article's URL is what makes a stored copy citable and what tells
  // a re-fetch of the same page from a fetch of a different one.
  char        url[WM_PAGE_URL_SZ];

  const char *text;                    // borrowed: callback lifetime only
  size_t      len;
} wm_prose_res_t;

// Callbacks run on the curl-multi worker thread owned by core — they
// must be fast and non-blocking.
typedef void (*wm_resolve_cb_t)(const wm_resolve_res_t *, void *user);
typedef void (*wm_claims_cb_t)(const wm_claims_res_t *, void *user);
typedef void (*wm_menu_cb_t)(const wm_menu_res_t *, void *user);
typedef void (*wm_prose_cb_t)(const wm_prose_res_t *, void *user);
typedef void (*wm_property_cb_t)(const wm_property_res_t *, void *user);
typedef void (*wm_facts_cb_t)(const wm_facts_res_t *, void *user);

// ----------------------------------------------------------------------
// Real function declarations — visible only inside the wikimedia plugin.
// External consumers go through the shims below.
//
// Every call here returns ASYNC_AIRBORNE or ASYNC_FAILED_UNDELIVERED and
// never ASYNC_FAILED_DELIVERED: a refusal (bad argument, URL overflow,
// a stopping plugin, a request that could not be submitted) always
// leaves `user` yours to free. See include/async.h.
//
// ⚠ ASYNC_AIRBORNE does not mean "later". Every verb can answer IN
// PLACE, before the call returns — off a warm cache, or off an armed
// backoff, which refuses all of them — so a caller holding a lock
// across one of them can re-enter itself through its own callback. Take
// that seriously or call from a task worker. Per verb: wm_resolve_async and
// wm_menu_async cache their whole result, and wm_property_async is that
// same cache read directly, so it answers a known word in place every
// time; wm_claims_async caches only the word→property step, so a hit
// there still goes to the wire; wm_reverse_async and wm_facts_async
// reach the wire on every call, and wm_prose_async does not cache at
// all and is always deferred.
// ----------------------------------------------------------------------

#ifdef WIKIMEDIA_INTERNAL

// Name → a ranked window of candidates, winner first. Merges a
// relevance-ranked CirrusSearch pass with an alias/typo-tolerant
// wbsearchentities pass and re-ranks the union by sitelink count:
// neither searcher alone is sufficient and their failure modes are
// complementary — the first is typo-intolerant ("robert duval" never
// reaches Robert Duvall), the second is prominence-blind (a Viennese
// merchant outranks Ludwig Wittgenstein). The whole window is returned
// because every consumer needs the alternates.
//
// ⛔ Default relevance only — `srqiprofile` popularity profiles were
// measured and REFUSED, not overlooked. `popular_inclinks_pv` is tuned
// for Wikipedia's link graph, where scholarly items do not dominate: it
// answered "Paris" with a ramp in the 14th arrondissement and "Python"
// with scientific articles. Do not re-propose them.
//
// ⚠ The measured score is 10 of 11 on names the resolver was TUNED on,
// so it is a floor and not an estimate; the standing miss is a
// transposed typo ("einstien", "beehtoven"), which wbsearchentities'
// alias tolerance does not cover. `tests/wikimedia/` scores this on
// names it has never seen.
async_rc_t wm_resolve_async(const char *name, wm_resolve_cb_t cb,
    void *user);

// (item, property) → its statements, with qualifiers.
//
// `property` is either a P-id ("P569") or an English word ("date of
// birth"), and the word form is where the value lives: it resolves to
// the top property candidates and answers from the FIRST that actually
// has a statement on this item. That fall-through is what separates
// "height" on a person — where the literal label match is P2044,
// elevation, which people do not have — from P2048, which they do.
// WM_NOT_FOUND means the word named no property, or none of its
// candidates had anything to say about this item.
async_rc_t wm_claims_async(const char *qid, const char *property,
    wm_claims_cb_t cb, void *user);

// Class item (a P31 value: Q5, Q11424, …) → its P1963 property menu.
async_rc_t wm_menu_async(const char *class_qid, wm_menu_cb_t cb,
    void *user);

// Article title, or a QID, → Wikipedia plaintext. `full` selects the
// whole article (tens of KB) over the lead summary (~2 KB).
async_rc_t wm_prose_async(const char *title, bool full, wm_prose_cb_t cb,
    void *user);

// English word → the properties it might name, best first.
//
// This is the first half of wm_claims_async published on its own, for
// the one job it cannot do: deciding, with no subject in hand, whether
// a run of words names a property at all. A consumer splitting free
// text asks that of each candidate tail and keeps the ones that answer
// WM_OK; which of them is RIGHT is still settled by value presence,
// which is wm_claims_async's business and must not be rebuilt.
// WM_NOT_FOUND means the word named nothing.
async_rc_t wm_property_async(const char *word, wm_property_cb_t cb,
    void *user);

// Item → the facts worth reading about it, ordered by the P1963 menu
// of its type where there is one and by the item's own statement order
// where there is not. Wikidata curates that menu for broad classes and
// not for narrow ones -- Q5 human has 103 entries, "rock band" has none
// and neither does "band" above it -- so the fallback is the common
// path rather than the exception.
//
// One entity-wide claims fetch, so the payload is the whole item (a
// heavily-edited person runs past 250 KB) and the answer is bounded by
// WM_FACTS_MAX rather than by what came back.
//
// WM_NOT_FOUND means the item has no P31 at all.
async_rc_t wm_facts_async(const char *qid, wm_facts_cb_t cb, void *user);

// (property word, value item) → the items that hold that statement.
//
// The mirror of wm_claims_async: that one asks what Ridley Scott
// directed, this one asks what was directed BY him. The value arrives
// already resolved because a consumer that had to name it in words had
// to resolve it anyway, and its own answer -- which of several people
// called that -- is the consumer's to report. Candidates come back
// ranked by sitelink count exactly as wm_resolve_async's do, and
// `total` carries how many matched before the window.
async_rc_t wm_reverse_async(const char *property, const char *value_qid,
    wm_resolve_cb_t cb, void *user);

// Seconds left on the Wikimedia backoff, or 0 when none is armed.
//
// Read it when a call answers WM_RATE_LIMITED, to tell whoever asked how
// long to wait. It is a live remainder rather than a snapshot taken when
// the refusal happened, so 0 is a real answer — it means the window has
// already passed, and it presents as "try again now", never as "0s".
uint32_t wm_retry_after(void);

#endif // WIKIMEDIA_INTERNAL

// ----------------------------------------------------------------------
// dlsym shim helpers (consumer side)
// ----------------------------------------------------------------------

#if !defined(WIKIMEDIA_INTERNAL) && !defined(WIKIMEDIA_TYPES_ONLY)

#include "clam.h"
#include "plugin.h"

#include <stdlib.h>  // abort

static inline async_rc_t
wm_resolve_async(const char *name, wm_resolve_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, wm_resolve_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_resolve_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_resolve_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(name, cb, user));
}

static inline async_rc_t
wm_claims_async(const char *qid, const char *property, wm_claims_cb_t cb,
    void *user)
{
  typedef async_rc_t (*fn_t)(const char *, const char *, wm_claims_cb_t,
      void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_claims_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_claims_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(qid, property, cb, user));
}

static inline async_rc_t
wm_menu_async(const char *class_qid, wm_menu_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, wm_menu_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_menu_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_menu_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(class_qid, cb, user));
}

static inline async_rc_t
wm_prose_async(const char *title, bool full, wm_prose_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, bool, wm_prose_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_prose_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_prose_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(title, full, cb, user));
}

static inline async_rc_t
wm_property_async(const char *word, wm_property_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, wm_property_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_property_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_property_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(word, cb, user));
}

static inline async_rc_t
wm_facts_async(const char *qid, wm_facts_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, wm_facts_cb_t, void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_facts_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_facts_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(qid, cb, user));
}

static inline async_rc_t
wm_reverse_async(const char *property, const char *value_qid,
    wm_resolve_cb_t cb, void *user)
{
  typedef async_rc_t (*fn_t)(const char *, const char *, wm_resolve_cb_t,
      void *);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_reverse_async",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_reverse_async");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn(property, value_qid, cb, user));
}

static inline uint32_t
wm_retry_after(void)
{
  typedef uint32_t (*fn_t)(void);
  static fn_t cached = NULL;
  fn_t        fn     = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

  if(fn == NULL)
  {
    union { void *obj; fn_t fn; } u;

    u.obj = plugin_dlsym_cached(WIKIMEDIA_CTX, "wm_retry_after",
        (void **)&cached);
    if(u.obj == NULL)
    {
      clam(CLAM_FATAL, WIKIMEDIA_CTX, "dlsym failed: wm_retry_after");
      abort();
    }
    fn = u.fn;
    __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  }
  return(fn());
}

#endif // !WIKIMEDIA_INTERNAL && !WIKIMEDIA_TYPES_ONLY

#endif // BM_WIKIMEDIA_API_H
