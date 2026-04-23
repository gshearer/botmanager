// botmanager — MIT
// JSON parsing and formatting helpers layered on json-c.
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clam.h"

// =======================================================================
// Byte-level helpers (no json-c use)
// =======================================================================

// Append one byte to a snprintf-style output buffer. Always advances w.
static inline void
json_put(char *out, size_t out_cap, size_t *w, char c)
{
  if(out != NULL && *w + 1 < out_cap)
    out[*w] = c;
  (*w)++;
}

static inline void
json_puts(char *out, size_t out_cap, size_t *w, const char *s)
{
  for(; *s != '\0'; s++)
    json_put(out, out_cap, w, *s);
}

size_t
json_escape(const char *in, char *out, size_t out_cap)
{
  size_t w = 0;

  for(const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++)
  {
    unsigned char c = *p;

    switch(c)
    {
      case '"':  json_puts(out, out_cap, &w, "\\\""); break;
      case '\\': json_puts(out, out_cap, &w, "\\\\"); break;
      case '\b': json_puts(out, out_cap, &w, "\\b");  break;
      case '\f': json_puts(out, out_cap, &w, "\\f");  break;
      case '\n': json_puts(out, out_cap, &w, "\\n");  break;
      case '\r': json_puts(out, out_cap, &w, "\\r");  break;
      case '\t': json_puts(out, out_cap, &w, "\\t");  break;
      default:
        if(c < 0x20)
        {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          json_puts(out, out_cap, &w, esc);
        }

        else
          json_put(out, out_cap, &w, (char)c);
        break;
    }
  }

  if(out != NULL && out_cap > 0)
    out[w < out_cap ? w : out_cap - 1] = '\0';

  return(w);
}

size_t
json_unescape(const char *in, size_t len, char *out)
{
  size_t w = 0;

  for(size_t i = 0; i < len; i++)
  {
    char c = in[i];
    char n;

    if(c != '\\' || i + 1 >= len)
    {
      out[w++] = c;
      continue;
    }

    n = in[++i];

    switch(n)
    {
      case '"':  out[w++] = '"';  break;
      case '\\': out[w++] = '\\'; break;
      case '/':  out[w++] = '/';  break;
      case 'b':  out[w++] = '\b'; break;
      case 'f':  out[w++] = '\f'; break;
      case 'n':  out[w++] = '\n'; break;
      case 'r':  out[w++] = '\r'; break;
      case 't':  out[w++] = '\t'; break;
      case 'u':
        if(i + 4 < len)
        {
          unsigned int cp = 0;
          for(int k = 0; k < 4; k++)
          {
            char h = in[i + 1 + k];
            cp <<= 4;
            if(h >= '0' && h <= '9')      cp |= (unsigned int)(h - '0');
            else if(h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
            else if(h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
          }
          i += 4;

          // Minimal UTF-8 emit (no surrogate pair handling).
          if(cp < 0x80)
            out[w++] = (char)cp;
          else if(cp < 0x800)
          {
            out[w++] = (char)(0xC0 | (cp >> 6));
            out[w++] = (char)(0x80 | (cp & 0x3F));
          }

          else
          {
            out[w++] = (char)(0xE0 | (cp >> 12));
            out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[w++] = (char)(0x80 | (cp & 0x3F));
          }
        }
        break;
      default:
        out[w++] = n;
        break;
    }
  }

  out[w] = '\0';
  return(w);
}

// =======================================================================
// One-shot accessors
// =======================================================================

static inline struct json_object *
json_lookup(struct json_object *obj, const char *key)
{
  struct json_object *v = NULL;

  if(obj == NULL || key == NULL)
    return(NULL);

  return(json_object_object_get_ex(obj, key, &v) ? v : NULL);
}

bool
json_get_bool(struct json_object *obj, const char *key, bool *out)
{
  struct json_object *v = json_lookup(obj, key);

  if(v == NULL)
    return(false);

  *out = json_object_get_boolean(v) ? true : false;
  return(true);
}

bool
json_get_int(struct json_object *obj, const char *key, int32_t *out)
{
  struct json_object *v = json_lookup(obj, key);

  if(v == NULL)
    return(false);

  *out = (int32_t)json_object_get_int(v);
  return(true);
}

bool
json_get_int64(struct json_object *obj, const char *key, int64_t *out)
{
  struct json_object *v = json_lookup(obj, key);

  if(v == NULL)
    return(false);

  *out = (int64_t)json_object_get_int64(v);
  return(true);
}

bool
json_get_double(struct json_object *obj, const char *key, double *out)
{
  struct json_object *v = json_lookup(obj, key);

  if(v == NULL)
    return(false);

  *out = json_object_get_double(v);
  return(true);
}

bool
json_get_str(struct json_object *obj, const char *key, char *out, size_t cap)
{
  struct json_object *v = json_lookup(obj, key);
  const char         *s;

  if(v == NULL || cap == 0)
    return(false);

  s = json_object_get_string(v);

  if(s == NULL)
  {
    out[0] = '\0';
    return(false);
  }

  snprintf(out, cap, "%s", s);
  return(true);
}

struct json_object *
json_get_obj(struct json_object *obj, const char *key)
{
  struct json_object *v = json_lookup(obj, key);

  return((v != NULL && json_object_is_type(v, json_type_object)) ? v : NULL);
}

struct json_object *
json_get_array(struct json_object *obj, const char *key)
{
  struct json_object *v = json_lookup(obj, key);

  return((v != NULL && json_object_is_type(v, json_type_array)) ? v : NULL);
}

struct json_object *
json_parse_buf(const char *buf, size_t len, const char *ctx)
{
  struct json_tokener    *tok;
  struct json_object     *root;
  enum json_tokener_error err;

  if(buf == NULL || len == 0)
    return(NULL);

  tok = json_tokener_new();

  if(tok == NULL)
    return(NULL);

  root = json_tokener_parse_ex(tok, buf, (int)len);
  err  = json_tokener_get_error(tok);

  json_tokener_free(tok);

  if(root == NULL || err != json_tokener_success)
  {
    clam(CLAM_WARN, ctx != NULL ? ctx : "json", "parse error: %s",
        json_tokener_error_desc(err));

    if(root != NULL)
      json_object_put(root);

    return(NULL);
  }

  return(root);
}

// =======================================================================
// Declarative extraction
// =======================================================================

static inline void *
json_dest(void *base, size_t offset)
{
  return((char *)base + offset);
}

// Parse a number that arrived as a JSON string. Returns true on full
// parse (trailing whitespace allowed), false otherwise.
static bool
json_parse_numeric_str(const char *s, long long *out_i, double *out_d,
    bool is_float)
{
  char *endp = NULL;

  if(s == NULL)
    return(false);

  errno = 0;

  if(is_float)
  {
    double d = strtod(s, &endp);

    if(endp == s || errno != 0)
      return(false);

    while(*endp == ' ' || *endp == '\t' || *endp == '\n' || *endp == '\r')
      endp++;

    if(*endp != '\0')
      return(false);

    *out_d = d;
  }

  else
  {
    long long ll = strtoll(s, &endp, 10);

    if(endp == s || errno != 0)
      return(false);

    while(*endp == ' ' || *endp == '\t' || *endp == '\n' || *endp == '\r')
      endp++;

    if(*endp != '\0')
      return(false);

    *out_i = ll;
  }

  return(true);
}

// Apply one spec entry's writeback rules for a located json value.
// Returns true on success, false on type mismatch / parse failure.
static bool
json_apply_scalar(const json_spec_t *s, struct json_object *v, void *base,
    const char *ctx)
{
  void *dest = json_dest(base, s->offset);

  switch(s->type)
  {
    case JSON_BOOL:
      *(bool *)dest = json_object_get_boolean(v) ? true : false;
      return(true);

    case JSON_INT:
      *(int32_t *)dest = (int32_t)json_object_get_int(v);
      return(true);

    case JSON_INT64:
      *(int64_t *)dest = (int64_t)json_object_get_int64(v);
      return(true);

    case JSON_TIME:
      *(time_t *)dest = (time_t)json_object_get_int64(v);
      return(true);

    case JSON_FLOAT:
      *(float *)dest = (float)json_object_get_double(v);
      return(true);

    case JSON_DOUBLE:
      *(double *)dest = json_object_get_double(v);
      return(true);

    case JSON_STR:
    {
      const char *str;

      if(s->len == 0)
      {
        clam(CLAM_WARN, ctx, "spec '%s': JSON_STR with len=0", s->name);
        return(false);
      }

      str = json_object_get_string(v);
      snprintf((char *)dest, s->len, "%s", str != NULL ? str : "");
      return(true);
    }

    case JSON_INT_STR:
    case JSON_INT64_STR:
    case JSON_DOUBLE_STR:
    {
      const char *str;
      long long   ll       = 0;
      double      d        = 0.0;
      bool        is_float = (s->type == JSON_DOUBLE_STR);

      if(!json_object_is_type(v, json_type_string))
      {
        clam(CLAM_WARN, ctx, "spec '%s': expected string-encoded number",
            s->name);
        return(false);
      }

      str = json_object_get_string(v);

      if(!json_parse_numeric_str(str, &ll, &d, is_float))
      {
        clam(CLAM_WARN, ctx, "spec '%s': cannot parse '%s' as number",
            s->name, str != NULL ? str : "");
        return(false);
      }

      if(s->type == JSON_INT_STR)        *(int32_t *)dest = (int32_t)ll;
      else if(s->type == JSON_INT64_STR) *(int64_t *)dest = (int64_t)ll;
      else                               *(double  *)dest = d;

      return(true);
    }

    case JSON_OBJ_REF:
      if(!json_object_is_type(v, json_type_object))
      {
        clam(CLAM_WARN, ctx, "spec '%s': expected object", s->name);
        return(false);
      }
      *(struct json_object **)dest = v;
      return(true);

    case JSON_ARRAY_REF:
      if(!json_object_is_type(v, json_type_array))
      {
        clam(CLAM_WARN, ctx, "spec '%s': expected array", s->name);
        return(false);
      }
      *(struct json_object **)dest = v;
      return(true);

    default:
      return(false);
  }
}

bool
json_extract(struct json_object *root, void *base,
    const json_spec_t *spec, const char *ctx)
{
  if(root == NULL || spec == NULL || base == NULL)
    return(false);

  if(ctx == NULL)
    ctx = "json";

  if(!json_object_is_type(root, json_type_object))
  {
    clam(CLAM_WARN, ctx, "json_extract: root is not an object");
    return(false);
  }

  for(const json_spec_t *s = spec; s->type != JSON_END; s++)
  {
    struct json_object *v = NULL;

    if(!json_object_object_get_ex(root, s->name, &v) || v == NULL)
    {
      if(s->required)
      {
        clam(CLAM_WARN, ctx, "required key '%s' missing", s->name);
        return(false);
      }

      continue;
    }

    if(s->type == JSON_OBJ)
    {
      void *child_base;

      if(!json_object_is_type(v, json_type_object))
      {
        clam(CLAM_WARN, ctx, "spec '%s': expected object", s->name);
        if(s->required) return(false);
        continue;
      }

      if(s->sub == NULL)
      {
        clam(CLAM_WARN, ctx, "spec '%s': JSON_OBJ without .sub", s->name);
        return(false);
      }

      child_base = json_dest(base, s->offset);

      if(!json_extract(v, child_base, s->sub, ctx))
      {
        if(s->required) return(false);
      }

      continue;
    }

    if(s->type == JSON_OBJ_ARRAY)
    {
      size_t arr_len;
      size_t n;
      char  *elements;
      size_t written = 0;

      if(!json_object_is_type(v, json_type_array))
      {
        clam(CLAM_WARN, ctx, "spec '%s': expected array", s->name);
        if(s->required) return(false);
        continue;
      }

      if(s->sub == NULL || s->stride == 0 || s->max_count == 0)
      {
        clam(CLAM_WARN, ctx,
            "spec '%s': JSON_OBJ_ARRAY needs .sub, .stride, .max_count",
            s->name);
        return(false);
      }

      arr_len  = (size_t)json_object_array_length(v);
      n        = arr_len < s->max_count ? arr_len : s->max_count;
      elements = (char *)json_dest(base, s->offset);

      for(size_t i = 0; i < n; i++)
      {
        struct json_object *item = json_object_array_get_idx(v, (int)i);
        void               *slot;

        if(item == NULL || !json_object_is_type(item, json_type_object))
          continue;

        slot = elements + (written * s->stride);

        if(!json_extract(item, slot, s->sub, ctx))
          continue;

        written++;
      }

      *(int32_t *)json_dest(base, s->count_off) = (int32_t)written;
      continue;
    }

    if(!json_apply_scalar(s, v, base, ctx))
    {
      if(s->required) return(false);
    }
  }

  return(true);
}
