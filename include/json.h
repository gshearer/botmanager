#ifndef BM_JSON_H
#define BM_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <json-c/json.h>

// JSON helpers for botmanager.
//
// Two layers:
//
//   1. Byte-level primitives (json_escape / json_unescape) — no parser
//      dependency. Used by the LLM streaming scanner and anywhere else a
//      full DOM would be overkill.
//
//   2. DOM-based extraction (json_extract + json_get_*) — built on
//      json-c, which is a runtime dep of the core. Service plugins
//      parsing REST responses should prefer this over hand-rolling
//      json_object_object_get_ex() chains.
//
// The core links json-c directly; plugins may use json-c freely because
// only one copy is ever mapped into the process regardless of how many
// plugins pull it in.

// Escape `in` as a JSON string body (without surrounding quotes) into
// `out`. snprintf-style: always NUL-terminates when out_cap > 0, and
// returns the number of bytes that *would* be written (excluding the
// NUL), so callers can size a buffer with a two-pass call.
size_t json_escape(const char *in, char *out, size_t out_cap);

// Unescape a JSON-quoted string body (the bytes between the quotes) of
// length `len` into `out`. `out` must be at least `len + 1` bytes.
// Handles \" \\ \/ \b \f \n \r \t and \uXXXX BMP codepoints.
size_t json_unescape(const char *in, size_t len, char *out);

// Declarative extraction:
// Describe what you want out of a JSON object as a table of json_spec_t
// entries, call json_extract(root, base, spec, ctx), and the extractor
// walks the DOM filling your struct. All data addresses are expressed as
// byte offsets relative to a caller-provided `base` pointer; this lets
// the same spec be reused when iterating an array of homogeneous
// elements (JSON_OBJ_ARRAY).
//
// Example (a coin entry from CoinMarketCap's Listings Latest response):
//
//   static const json_spec_t usd_spec[] = {
//     { JSON_DOUBLE, "price",              true,  offsetof(cmc_coin_t, price) },
//     { JSON_DOUBLE, "market_cap",         false, offsetof(cmc_coin_t, market_cap) },
//     { JSON_DOUBLE, "volume_24h",         false, offsetof(cmc_coin_t, volume_24h) },
//     { JSON_DOUBLE, "percent_change_1h",  false, offsetof(cmc_coin_t, pct_1h) },
//     { JSON_END }
//   };
//
//   static const json_spec_t quote_spec[] = {
//     // Note: JSON_OBJ without .offset descends into the named object
//     // but keeps writing into the outer struct at the stated offsets.
//     // Use .offset when the named object maps to an embedded sub-struct.
//     { JSON_OBJ, "USD", false, 0, .sub = usd_spec },
//     { JSON_END }
//   };
//
//   static const json_spec_t coin_spec[] = {
//     { JSON_INT,   "id",       true, offsetof(cmc_coin_t, id) },
//     { JSON_INT,   "cmc_rank", true, offsetof(cmc_coin_t, cmc_rank) },
//     { JSON_STR,   "name",     true, offsetof(cmc_coin_t, name),
//                                      .len = sizeof(((cmc_coin_t *)0)->name) },
//     { JSON_OBJ,   "quote",    true, 0, .sub = quote_spec },
//     { JSON_END }
//   };
//
//   cmc_coin_t coin;
//   json_extract(item, &coin, coin_spec, "coinmarketcap:listing");
//
// For homogeneous arrays, use JSON_OBJ_ARRAY:
//
//   static const json_spec_t daily_spec[] = {
//     { JSON_INT64, "dt",      true, offsetof(ow_daily_t, dt) },
//     { JSON_STR,   "summary", false, offsetof(ow_daily_t, summary),
//                                      .len = sizeof(((ow_daily_t *)0)->summary) },
//     { JSON_END }
//   };
//
//   static const json_spec_t root_spec[] = {
//     { JSON_OBJ_ARRAY, "daily", false, offsetof(ow_result_t, daily),
//       .sub       = daily_spec,
//       .stride    = sizeof(ow_daily_t),
//       .max_count = OW_MAX_DAYS,
//       .count_off = offsetof(ow_result_t, num_day) },
//     { JSON_END }
//   };
//
// For heterogeneous arrays or index-specific access, capture the
// json_object* handle with JSON_ARRAY_REF / JSON_OBJ_REF and iterate
// manually.

typedef enum
{
  JSON_END        = 0,   // spec terminator

  // Scalar writers. `offset` is the byte offset of the destination
  // field within `base`.
  JSON_BOOL,             // dest: bool
  JSON_INT,              // dest: int32_t
  JSON_INT64,            // dest: int64_t
  JSON_TIME,             // dest: time_t (read as int64)
  JSON_FLOAT,            // dest: float
  JSON_DOUBLE,           // dest: double

  // Dest is a fixed-size char buffer with capacity in `.len`. Always
  // NUL-terminates.
  JSON_STR,

  // The JSON value is a *string* whose contents parse as a number.
  // Crypto exchange APIs use this heavily (price: "12345.67").
  JSON_INT_STR,          // dest: int32_t
  JSON_INT64_STR,        // dest: int64_t
  JSON_DOUBLE_STR,       // dest: double

  // Hand the json-c handle back to the caller. Borrowed; valid only as
  // long as the root object is alive (caller must not json_object_put).
  // Useful for heterogeneous arrays or arrays accessed by index.
  // dest: struct json_object *
  JSON_OBJ_REF,
  JSON_ARRAY_REF,

  // Descend into a nested object, applying `.sub`. If `offset` is
  // nonzero, writes from `.sub` are based at base+offset (use this when
  // the nested object maps to an embedded sub-struct). If offset is
  // zero, `.sub` writes continue into the outer base — handy for
  // "flatten this JSON shape into my flat struct".
  JSON_OBJ,

  // Iterate a homogeneous array of objects, applying `.sub` to each
  // element. `offset` = start of the element storage in base; `.stride`
  // = sizeof(one element); `.max_count` = array capacity; `.count_off`
  // = offset (relative to base) of the int32_t that receives the actual
  // count written. Elements beyond max_count are silently skipped.
  JSON_OBJ_ARRAY,
} json_type_t;

typedef struct json_spec
{
  json_type_t             type;
  const char             *name;     // key name; NULL only for JSON_END
  bool                    required; // true: log and fail if missing
  size_t                  offset;   // byte offset from base

  size_t                  len;      // JSON_STR: dest buffer capacity

  // JSON_OBJ_ARRAY only:
  size_t                  stride;
  size_t                  max_count;
  size_t                  count_off;

  const struct json_spec *sub;      // JSON_OBJ / JSON_OBJ_ARRAY
} json_spec_t;

// Extract fields from `root` into the struct at `base` per `spec`.
// `ctx` is a short string included in log messages (e.g.
// "coinmarketcap:listing") — never NULL.
// Returns false if any required field was missing or had the wrong type.
bool json_extract(struct json_object *root, void *base,
    const json_spec_t *spec, const char *ctx);

// One-shot field accessors:
// For plugins that prefer to reach into the DOM ad-hoc rather than
// describe the whole response shape. Replaces hand-rolled
// json_object_object_get_ex + json_object_get_* chains.
//
// All of these are tolerant: missing key -> returns false / NULL and
// leaves *out untouched (for the scalar variants). Pass required
// through json_extract if you want required-field enforcement.

bool json_get_bool   (struct json_object *obj, const char *key, bool    *out);
bool json_get_int    (struct json_object *obj, const char *key, int32_t *out);
bool json_get_int64  (struct json_object *obj, const char *key, int64_t *out);
bool json_get_double (struct json_object *obj, const char *key, double  *out);

// Copies into `out` (NUL-terminated, truncated to cap-1). Returns true
// if the key existed and had a string value.
bool json_get_str(struct json_object *obj, const char *key,
    char *out, size_t cap);

// Borrowed handle or NULL. Does not increment ref.
struct json_object *json_get_obj  (struct json_object *obj, const char *key);
struct json_object *json_get_array(struct json_object *obj, const char *key);

// Convenience wrapper: parse `buf` (length `len`) as JSON. Returns an
// owned root object or NULL on parse error. Caller must json_object_put.
// `ctx` is used in log messages on parse failure.
struct json_object *json_parse_buf(const char *buf, size_t len,
    const char *ctx);

#endif // BM_JSON_H
