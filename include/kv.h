#ifndef BM_KV_H
#define BM_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nl.h"

// Longest key the tree's own grammar can compose, not a round number:
// the IRC driver's per-channel admin keys are
// "bot." + BOT_NAME_SZ + ".irc.chan." + IRC_CHAN_SZ + ".admin.kick_unident_delay"
// = 166 bytes at the limits its own argument validators allow. At 128 those
// keys truncated silently, and two long channel names sharing a prefix
// collapsed onto one row. Nothing persists this size — the `kv.key` column
// is unbounded `text` — so it costs only the in-memory cache entry.
#define KV_KEY_SZ  192
#define KV_STR_SZ  256

// Substituted for any secret KV value when read without an active admin
// context ("[CENSORED]"). Pointer-stable string literal — callers must
// not free.
extern const char *KV_REDACTED_VALUE;

// True iff any non-tail dot-separated segment of key equals "creds".
// This is the single credential convention: every stored credential
// lives under a `.creds.` segment — the API key itself is always
// `.creds.apikey`, and any related material (tokens, key names, private
// keys, passphrases) sits beside it under the same `.creds.` prefix.
// "plugin.foo.creds.apikey" is secret; "plugin.foo.creds" is not.
bool kv_is_secret_key(const char *key);

bool kv_set_secret(const char *key, const char *val);

// Like kv_get_str but for a secret key without admin context, returns
// KV_REDACTED_VALUE. Always non-NULL on a registered KV_STR key.
const char *kv_get_secret(const char *key);

// Read a credential (`.creds.*`) value bypassing redaction — for a plugin
// fetching its OWN stored credential to authenticate an outbound request.
// This is a strictly internal-use accessor: NEVER route its result into a
// user-facing reply, only the botmanctl display path may show credentials.
// Returns NULL if unset. Non-secret keys read exactly as kv_get_str.
const char *kv_get_creds(const char *key);

// Thread-local admin-context flag. Command dispatch sets this around the
// callback ONLY for admin commands arriving over the botmanctl control
// socket (see cmd_creds_visible), so credential values de-redact solely
// for the operator's local Unix-socket channel; every other method — IRC,
// chat, ... — sees KV_REDACTED_VALUE even for an admin. Plugins that need
// to read their own credential to sign a request arm this flag directly
// for the duration of that read.
void kv_admin_context_set(bool active);
bool kv_admin_context_active(void);

typedef enum
{
  KV_INT8,    KV_UINT8,
  KV_INT16,   KV_UINT16,
  KV_INT32,   KV_UINT32,
  KV_INT64,   KV_UINT64,
  KV_FLOAT,   KV_DOUBLE,   KV_LDOUBLE,
  KV_STR,
  KV_BOOL
} kv_type_t;

// Invoked when a value changes.
typedef void (*kv_cb_t)(const char *key, void *data);

// Fails if the key already exists or the default is bad for the type.
// help must be static storage; cb may be NULL.
bool kv_register(const char *key, kv_type_t type, const char *default_val,
    kv_cb_t cb, void *cb_data, const char *help);

// Same registration, with ownership stated rather than inferred. The
// public form above attributes the entry to its call site, which is what
// a plugin registering its own key wants; this form is for code
// registering a key *on another module's behalf* (the loader's kv_schema
// pass, plugin_kv_group_register, the per-bot instance schemas, whenmoon
// registering a strategy's params), where the call site is not the owner
// and the owner is whoever supplied the cb/help pointers. `owner_pc` is
// any address inside that plugin's mapping -- the declaration the
// registration was built from is the natural one.
bool kv_register_owned(const char *key, kv_type_t type,
    const char *default_val, kv_cb_t cb, void *cb_data, const char *help,
    const void *owner_pc);

// Returns 0 for missing or type-mismatched keys.
int64_t kv_get_int(const char *key);

// Returns 0 for missing or type-mismatched keys.
uint64_t kv_get_uint(const char *key);

// Returns 0.0 for missing or type-mismatched keys.
double kv_get_double(const char *key);

// Returns 0.0 for missing or type-mismatched keys.
long double kv_get_ldouble(const char *key);

// Returns an interned string that lives for the process, or NULL.
//
// The pointer never dangles and the bytes behind it never change. A
// KV string value is immutable: kv_set installs a *different* interned
// string rather than writing over this one, and dropping the entry
// (kv_unregister, a plugin unload's kv_reclaim_owned) does not touch
// it. So a held pointer can only go STALE — it keeps answering with
// the value the key had when it was read. Re-read the key wherever
// that matters; there is no lifetime to manage and nothing to copy.
//
// ⚠ NULL also means "registered, but not KV_STR" — this reads only
// string keys, so it is no use for asking whether a UINT32 or BOOL key
// has a value. There is no such question to ask: a registered key
// always answers with its default, so the typed getter above IS the
// value, and 0 from it means the key says 0.
const char *kv_get_str(const char *key);

bool kv_set(const char *key, const char *val);

// Fails on key not found, type mismatch, or out of range.
bool kv_set_int(const char *key, int64_t val);

// Fails on key not found, type mismatch, or out of range.
bool kv_set_uint(const char *key, uint64_t val);

// Fails on key not found or type mismatch.
bool kv_set_float(const char *key, long double val);

bool kv_set_str(const char *key, const char *val);

bool kv_exists(const char *key);

// Returns an interned help string, or NULL — for a key that has none as
// much as for a key that does not exist.
//
// kv_register interns whatever it is handed, so the caller's storage is
// its own business: a literal, a stack buffer or a plugin's .rodata are
// all copied in, and the pointer this returns outlives every one of them
// (OBS-14). Help is never rewritten in place and never re-read from the
// database, so unlike kv_get_str's answer it cannot even go stale.
const char *kv_get_help(const char *key);

// Returns NULL if key not found.
const char *kv_get_type_name(const char *key);

// Serialize a KV entry's current value into buf as a string.
bool kv_get_val_str(const char *key, char *buf, size_t bufsz);

// Key must already be registered. cb NULL clears.
bool kv_set_cb(const char *key, kv_cb_t cb, void *cb_data);

// --- per-bot keys ------------------------------------------------------
//
// One bot instance's settings live under a composed key, and the
// grammar is core's own: driver-scoped is "bot.<name>.<suffix>",
// method-scoped is "bot.<name>.<kind>.<suffix>". Those are exactly the
// two forms bot_register_driver_kv() and bot_register_method_kv() build
// when they lay a plugin's instance schema down over a bot, so `suffix`
// here is the schema entry's own bare key ("behavior.chat.enabled",
// "nick") and nothing else needs spelling.
//
// Read these through the accessors below rather than composing the key
// at the call site. The composition has one silent failure — a name and
// suffix that together overrun KV_KEY_SZ address a *different*, shorter
// key, which then answers with somebody else's value or with the unset
// 0 — and it is the failure this project has already paid for once (see
// KV_KEY_SZ above, where two long channel names collapsed onto one
// row). Composed in one place it is caught and logged instead; a
// truncated key reads as unset and writes nothing.
//
// An absent bot name, kind or suffix reads as unset for the same
// reason: there is no key to name, and a caller that could not identify
// its bot has nothing different to do about it.

// Returns 0 for missing, type-mismatched or uncomposable keys.
uint64_t kv_get_bot_uint(const char *name, const char *suffix);

// Returns 0 for missing, type-mismatched or uncomposable keys.
uint64_t kv_get_bot_method_uint(const char *name, const char *kind,
    const char *suffix);

// Storage pointer with kv_get_str's lifetime and its NULL semantics.
const char *kv_get_bot_str(const char *name, const char *suffix);

// Storage pointer with kv_get_str's lifetime and its NULL semantics.
const char *kv_get_bot_method_str(const char *name, const char *kind,
    const char *suffix);

// Fails on key not found, type mismatch, out of range, or a key that
// does not compose.
bool kv_set_bot_uint(const char *name, const char *suffix, uint64_t val);

// WARNING: invoked under the KV lock — do NOT call kv_* functions.
typedef void (*kv_iter_cb_t)(const char *key, kv_type_t type,
    const char *value_str, void *data);

// Returns the number of entries visited.
uint32_t kv_iterate_prefix(const char *prefix, kv_iter_cb_t cb, void *data);

// Also deletes matching rows from the database if available. Returns
// the number of entries deleted.
uint32_t kv_delete_prefix(const char *prefix);

// Delete exactly one key (whole-key match) from the live registry and its
// persisted row. The DB row is dropped unconditionally, so a DB-only orphan
// is swept even when it isn't in memory. Returns true iff a live in-memory
// entry was removed. Intended for admin janitoring (see /db delete kv).
bool kv_delete(const char *key);

// In-memory removal of a registered KV entry — drops the kv_entry_t
// (including its cached cb pointer, cb_data, and help pointer, all of
// which may live in plugin .text/.rodata) and any attached NL
// responder. Does NOT touch the database row; if a plugin reloads and
// re-registers the same key, the next kv_load() picks the value back
// up. Returns SUCCESS if the entry existed, FAIL otherwise.
bool kv_unregister(const char *key);

// Drop every entry registered by code inside the address range [lo,hi) --
// one loaded object's mapping -- along with any NL responder whose hint
// lives in that same range. KV entries are Class A (see root TODO.md
// §PLIFE-3): core reclaims whatever a plugin's deinit() left, because a
// retained cb pointer into an unmapped .so is a crash, and the persisted
// row survives to rehydrate the key on reload. (help is no longer among
// them -- kv_register interns it. See kv_get_help.)
// Returns the number of entries removed.
uint32_t kv_reclaim_owned(uintptr_t lo, uintptr_t hi);

const char *kv_type_name(kv_type_t type);

// Natural-language responder metadata attached to a KV. A KV becomes
// NL-bridge visible (/kv <suffix>) only when an nl_t is attached via
// kv_register_nl. All strings and arrays are static / caller-owned;
// the registry stores pointers only and never copies.
//
// ⚠ This is a graph of pointers, not a string, so it gets none of the
// lifetime kv_get_str and kv_get_help promise -- interning it would mean
// copying the graph. "Static" here means static *in the owning object*,
// and a plugin unload unmaps it: core drops the registration
// (kv_reclaim_owned), but a reader that RETAINS a `const kv_nl_t *`
// across that unload is holding a dead page. Read it, use it, drop it --
// or keep the hint in the same object as the code that holds it, which
// is why the arrangement is safe today (every hint in the tree is chat's
// and every reader of one is chat's). A second consumer inherits no such
// guarantee. OBS-14.
typedef struct
{
  const char         *when;              // REQUIRED — LLM cue
  const nl_example_t *examples;          // REQUIRED — >=1 entry
  uint8_t             example_count;

  // Response template with literal "$value" substitution for the KV's
  // serialized value. NULL = emit the value verbatim.
  const char         *response_template;
} kv_nl_t;

// Attach NL metadata to an already-registered KV. Fails (and logs) when
// the key has not been registered. nl storage must be static for the
// process lifetime — the registry retains the pointer.
bool kv_register_nl(const char *key, const kv_nl_t *nl);

// Return the NL metadata attached to a key, or NULL if the key has no
// NL responder attached (covers unregistered keys too).
const kv_nl_t *kv_get_nl(const char *key);

// Callback type for kv_iterate_nl. Invoked once per NL-capable KV.
// The callback is invoked UNDER NO LOCK — it may safely call other
// kv_* APIs (the registry is snapshotted before dispatch).
typedef void (*kv_nl_iter_cb_t)(const char *key, const kv_nl_t *nl,
    void *data);

// Iterate every KV that has an NL responder attached. See the callback
// type above for locking semantics.
void kv_iterate_nl(kv_nl_iter_cb_t cb, void *data);

// Audit hook: yields every pointer the KV registry retains, one
// invocation per (entry, field), then one per attached NL responder.
// `subject` is the key. `ptr` may be NULL (the callback filters).
//
// Invoked UNDER the registry locks: the callback must be fast and must
// not re-enter any kv_* API.
typedef void (*kv_audit_cb_t)(const char *subject, const char *field,
    const void *ptr, void *data);

void kv_audit_iterate(kv_audit_cb_t cb, void *data);

bool kv_load(void);

// Invoked once per orphaned persisted row. Runs under NO lock and may
// call other kv_* APIs; `value` is the raw stored text, redaction is the
// caller's business (see kv_is_secret_key).
typedef void (*kv_orphan_cb_t)(const char *key, kv_type_t type,
    const char *value, void *data);

// Visit every persisted row that no live registry entry claims — the
// residue of an unloaded plugin, or of a key a schema change retired.
// Strictly read-only: it never registers, modifies or deletes a row, so it
// is the reporting counterpart of kv_claim_orphans() below and safe to run
// while a plugin is unloaded. Returns the number of orphans visited.
uint32_t kv_iterate_orphans(kv_orphan_cb_t cb, void *data);

// Materialize persisted DB rows that no live entry claims — dynamic keys
// with no static schema (e.g. per-channel IRC configuration). Call after
// bot restore. Returns the number of entries materialized.
// ⚠ Boot-time only: after an unload an orphan is a *dropped binding*, and
// materializing it would resurrect the key as live-but-unowned. Reporting
// after an unload is kv_iterate_orphans()'s job.
uint32_t kv_claim_orphans(void);

bool kv_flush(void);

// Must be called after mem_init().
void kv_init(void);

// Flushes if DB is available, then frees all entries.
void kv_exit(void);

#ifdef KV_INTERNAL

#include "common.h"
#include "clam.h"
#include "db.h"
#include "alloc.h"

#include <limits.h>

#define KV_BUCKETS        64
#define KV_INTERN_BUCKETS 128
#define KV_VAL_BUF        300    // serialization buffer

// A string value is a pointer into the intern table below, never inline
// storage: that is what lets kv_get_str hand the pointer out and walk
// away. long double is the widest member, so this is 16 bytes.
typedef union
{
  int8_t      i8;
  uint8_t     u8;
  int16_t     i16;
  uint16_t    u16;
  int32_t     i32;
  uint32_t    u32;
  int64_t     i64;
  uint64_t    u64;
  float       f;
  double      d;
  long double ld;
  const char *str;
} kv_val_t;

typedef struct kv_entry
{
  char             key[KV_KEY_SZ];
  kv_type_t        type;
  kv_val_t         val;
  kv_cb_t          cb;
  void            *cb_data;
  const char      *help;     // human-readable help (static, may be NULL)
  const void      *owner_pc; // registration call site; identifies the owning object
  bool             dirty;
  bool             secret;   // value redacted from non-admin readers
  struct kv_entry *next;     // hash chain
} kv_entry_t;

// The intern table: every distinct string a KV value has ever held,
// stored once and never freed until kv_exit(). It is what makes the
// pointer kv_get_str returns outlive both the value and the entry —
// nothing here is ever rewritten in place or unlinked, so a reader
// holding one needs no lock, no copy and no lifetime.
//
// It grows by one node per *distinct* string ever stored, which in this
// tree is an operator setting a key, not a hot path: repeat values cost
// nothing. Guarded by its own mutex, taken while kv_mutex may be held
// and never the other way round; it calls nothing, least of all clam().
typedef struct kv_str_node
{
  struct kv_str_node *next;
  size_t              len;
  char                s[];
} kv_str_node_t;

static kv_entry_t       *kv_table[KV_BUCKETS];
static kv_str_node_t    *kv_intern_table[KV_INTERN_BUCKETS];
static pthread_mutex_t   kv_intern_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t   kv_mutex;
static uint32_t          kv_count = 0;
static uint32_t          kv_intern_count = 0;
static bool              kv_ready = false;
static bool              kv_loaded = false;  // true once kv_load() completes

#endif // KV_INTERNAL

#endif // BM_KV_H
