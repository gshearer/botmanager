// botmanager — MIT
// On-demand personality reader: parses <personalitypath>/<name>.txt fresh.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "clam.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Strip leading/trailing ASCII whitespace in place. Returns s.
static char *
mp_trim(char *s)
{
  size_t n;
  char *p;

  if(s == NULL) return(s);

  p = s;

  while(*p && isspace((unsigned char)*p)) p++;
  if(p != s) memmove(s, p, strlen(p) + 1);

  n = strlen(s);

  while(n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
  return(s);
}

static char *
mp_slurp(const char *path)
{
  size_t n;
  char *buf;

  FILE *fp = fopen(path, "rb");

  if(fp == NULL) return(NULL);

  buf = mem_alloc("chatbot", "pfile", CHATBOT_PERSONALITY_BODY_SZ);

  if(buf == NULL)
  {
    fclose(fp);
    return(NULL);
  }

  n = fread(buf, 1, CHATBOT_PERSONALITY_BODY_SZ - 1, fp);

  buf[n] = '\0';
  fclose(fp);
  return(buf);
}

// Skip an optional "key: value" / "#..." header block terminated by a
// blank line or "---", returning a pointer to the first body byte.
// Used by the contract-include path where we want only the body, not
// any header keys.
static const char *
mp_skip_frontmatter(char *raw)
{
  char *p = raw;

  while(*p)
  {
    char  *eol  = strchr(p, '\n');
    size_t llen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

    char   line[512];
    size_t copy = (llen < sizeof(line) - 1) ? llen : sizeof(line) - 1;

    memcpy(line, p, copy);
    line[copy] = '\0';
    mp_trim(line);

    if(line[0] == '\0' || strcmp(line, "---") == 0)
      return((eol != NULL) ? eol + 1 : p + strlen(p));

    if(line[0] == '#')
    {
      p = (eol != NULL) ? eol + 1 : p + strlen(p);
      continue;
    }

    if(strchr(line, ':') == NULL)
      return(raw);  // No header markers at all.

    p = (eol != NULL) ? eol + 1 : p + strlen(p);
  }

  return(p);
}

// Resolve the configured personality directory. Public helper shared
// with personality_show.c. Always succeeds: absent / empty KV falls
// back to "./personalities".
bool
chatbot_personality_path(char *out_path, size_t sz)
{
  const char *p;

  if(out_path == NULL || sz == 0)
    return(false);

  p = kv_get_str("bot.chat.personalitypath");

  if(p == NULL || p[0] == '\0')
    p = "./personalities";

  snprintf(out_path, sz, "%s", p);
  return(true);
}

// Resolve `<dir>/<name>.txt` into out. Returns true on success.
static bool
mp_resolve_path(const char *name, char *out, size_t sz)
{
  char dir[PATH_MAX];

  if(name == NULL || name[0] == '\0')
    return(false);

  // Defence in depth: refuse paths with directory separators in the
  // stem so an attacker-controlled bot.<name>.behavior.personality
  // can't escape the personalitypath root.
  if(strchr(name, '/') != NULL)
    return(false);

  if(!chatbot_personality_path(dir, sizeof(dir)))
    return(false);

  snprintf(out, sz, "%s/%s.txt", dir, name);
  return(true);
}

// Parse the frontmatter of `raw` in place. On return `*body_out`
// points at the first body byte inside raw (no further allocation).
// Recognised keys: name, description, interests, version. Unknown
// keys (e.g. a legacy "contract:" line) are silently skipped.
//
// When err / err_sz is non-NULL, a short explanation of the first
// fatal parse problem is written there (used by the header-only
// reader to render broken rows without aborting the whole listing).
static bool
mp_parse_frontmatter(char *raw,
    char *name, size_t name_sz,
    char *desc, size_t desc_sz,
    char *interests, size_t interests_sz,
    char *version, size_t version_sz,
    const char **body_out,
    char *err, size_t err_sz)
{
  char *p;
  char *body;
  bool  in_header;

  if(name_sz > 0)     name[0]     = '\0';
  if(desc_sz > 0)     desc[0]     = '\0';
  if(interests != NULL && interests_sz > 0)
    interests[0] = '\0';
  if(version_sz > 0)  version[0]  = '\0';
  if(err_sz > 0)      err[0]      = '\0';

  p = raw;
  body = raw;
  in_header = true;

  // Swallow an optional leading "---" so the matching closing "---"
  // terminates the header rather than being treated as a zero-key line.
  {
    char  *eol  = strchr(p, '\n');
    size_t llen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
    char   tmp[8];

    if(llen <= sizeof(tmp) - 1)
    {
      memcpy(tmp, p, llen);
      tmp[llen] = '\0';
      mp_trim(tmp);

      if(strcmp(tmp, "---") == 0)
        p = (eol != NULL) ? eol + 1 : p + strlen(p);
    }
  }

  while(in_header && *p)
  {
    char  *eol  = strchr(p, '\n');
    size_t llen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

    char   line[512];
    size_t copy = (llen < sizeof(line) - 1) ? llen : sizeof(line) - 1;
    char *key;
    char *val;
    char *colon;

    memcpy(line, p, copy);
    line[copy] = '\0';
    mp_trim(line);

    if(line[0] == '\0' || strcmp(line, "---") == 0)
    {
      in_header = false;
      body      = (eol != NULL) ? eol + 1 : p + strlen(p);
      break;
    }

    if(line[0] == '#')
    {
      p = (eol != NULL) ? eol + 1 : p + strlen(p);
      continue;
    }

    colon = strchr(line, ':');

    if(colon == NULL)
    {
      // No header markers at all — treat entire file as body.
      in_header = false;
      body      = raw;
      break;
    }

    *colon = '\0';

    key = mp_trim(line);
    val = mp_trim(colon + 1);

    if(strcasecmp(key, "name") == 0 && val[0] != '\0')
      snprintf(name, name_sz, "%s", val);
    else if(strcasecmp(key, "description") == 0)
      snprintf(desc, desc_sz, "%s", val);
    else if(strcasecmp(key, "version") == 0 && version_sz > 0)
      snprintf(version, version_sz, "%s", val);
    else if(strcasecmp(key, "interests") == 0 && interests != NULL
        && interests_sz > 0)
    {
      char *scan;

      // Slurp the JSON block. Value may be inline on the same line
      // (`interests: [...]`) or span subsequent lines until the next
      // header terminator (blank line or `---`). Newlines are preserved
      // verbatim so json-c sees the original shape.
      size_t written   = 0;
      size_t first_len = strlen(val);

      if(first_len > 0 && written + first_len + 1 < interests_sz)
      {
        memcpy(interests + written, val, first_len);
        written += first_len;
        interests[written++] = '\n';
      }

      scan = (eol != NULL) ? eol + 1 : p + strlen(p);

      while(*scan != '\0')
      {
        char  *s_eol  = strchr(scan, '\n');
        size_t s_llen = (s_eol != NULL) ? (size_t)(s_eol - scan)
                                        : strlen(scan);

        // Probe is large enough to survive deep indentation (e.g.
        // an 8-space-indented `{"type":"rss", ...}` line inside a
        // nested `sources: [...]` array). A too-small probe trims
        // to empty and falsely terminates the slurp.
        char   probe[128];
        size_t probe_copy = (s_llen < sizeof(probe) - 1)
            ? s_llen : sizeof(probe) - 1;

        memcpy(probe, scan, probe_copy);
        probe[probe_copy] = '\0';
        mp_trim(probe);

        if(probe[0] == '\0' || strcmp(probe, "---") == 0)
          break;

        if(written + s_llen + 1 < interests_sz)
        {
          memcpy(interests + written, scan, s_llen);
          written += s_llen;
          interests[written++] = '\n';
        }

        scan = (s_eol != NULL) ? s_eol + 1 : scan + strlen(scan);
      }

      interests[written < interests_sz ? written : interests_sz - 1] = '\0';

      // Advance the outer scanner to the terminator so the
      // standard header-end handling fires on the next iteration.
      p = scan;
      continue;
    }

    p = (eol != NULL) ? eol + 1 : p + strlen(p);
  }

  *body_out = body;

  if(name[0] == '\0')
  {
    if(err_sz > 0)
      snprintf(err, err_sz, "missing name:");
    return(FAIL);
  }

  if((*body_out)[0] == '\0')
  {
    if(err_sz > 0)
      snprintf(err, err_sz, "empty body");
    return(FAIL);
  }

  return(SUCCESS);
}

void
chatbot_personality_free(chatbot_personality_t *p)
{
  if(p == NULL)
    return;

  if(p->body != NULL)           { mem_free(p->body);           p->body           = NULL; }
  if(p->interests_json != NULL) { mem_free(p->interests_json); p->interests_json = NULL; }
}

// Frontmatter shape:
//   optional leading "---"
//   "key: value" lines (name, description, interests, version)
//   terminator blank line or "---"
//   body
// `interests` may span multiple lines (json-c reads the original newlines);
// no YAML pipe form — see feedback_interests_parser.
bool
chatbot_personality_read(const char *name, chatbot_personality_t *out)
{
  char        pname[CHATBOT_PERSONALITY_NAME_SZ];
  char        pdesc[CHATBOT_PERSONALITY_DESC_SZ];
  char        version_str[32];
  const char *body_ptr;
  char *interests_buf;
  char *raw;
  char path[CHATBOT_PERSONALITY_PATH_SZ];

  if(name == NULL || name[0] == '\0' || out == NULL)
    return(FAIL);

  memset(out, 0, sizeof(*out));

  if(!mp_resolve_path(name, path, sizeof(path)))
  {
    clam(CLAM_WARN, "chatbot",
        "personality '%s' path resolution failed", name);
    return(FAIL);
  }

  raw = mp_slurp(path);

  if(raw == NULL)
  {
    clam(CLAM_WARN, "chatbot",
        "cannot read personality file '%s': %s",
        path, strerror(errno));
    return(FAIL);
  }

  body_ptr = NULL;

  interests_buf = mem_alloc("chatbot", "interests_buf",
      CHATBOT_INTERESTS_JSON_SZ);

  if(interests_buf == NULL)
  {
    mem_free(raw);
    return(FAIL);
  }

  if(mp_parse_frontmatter(raw,
      pname, sizeof(pname),
      pdesc, sizeof(pdesc),
      interests_buf, CHATBOT_INTERESTS_JSON_SZ,
      version_str, sizeof(version_str),
      &body_ptr, NULL, 0) != SUCCESS)
  {
    clam(CLAM_WARN, "chatbot", "malformed personality file '%s'", path);
    mem_free(interests_buf);
    mem_free(raw);
    return(FAIL);
  }

  while(*body_ptr == '\n' || *body_ptr == '\r') body_ptr++;

  snprintf(out->name,        sizeof(out->name),        "%s", pname);
  snprintf(out->description, sizeof(out->description), "%s", pdesc);
  out->version        = version_str[0] ? atoi(version_str) : 1;
  out->updated        = 0;
  out->body           = mem_strdup("chatbot", "body", body_ptr);
  out->interests_json = interests_buf;   // transfer ownership

  mem_free(raw);

  if(out->body == NULL)
  {
    chatbot_personality_free(out);
    return(FAIL);
  }

  return(SUCCESS);
}

// Resolve the configured contract directory. Public helper shared
// with contract_show.c. Always succeeds: absent / empty KV falls
// back to "./personalities/contracts".
bool
chatbot_contract_path(char *out_path, size_t sz)
{
  const char *p;

  if(out_path == NULL || sz == 0)
    return(false);

  p = kv_get_str("bot.chat.contractpath");

  if(p == NULL || p[0] == '\0')
    p = "./personalities/contracts";

  snprintf(out_path, sz, "%s", p);
  return(true);
}

bool
chatbot_contract_read(const char *name, char **out_body)
{
  const char *body_ptr;
  char *raw;
  char path[CHATBOT_PERSONALITY_PATH_SZ];
  char dir[PATH_MAX];

  if(name == NULL || name[0] == '\0' || out_body == NULL)
    return(FAIL);

  *out_body = NULL;

  // Defence in depth: forbid path separators in the stem so a
  // bot-configurable contract name can't escape the contract root.
  if(strchr(name, '/') != NULL)
  {
    clam(CLAM_WARN, "chatbot",
        "contract name '%s' contains '/' — rejected", name);
    return(FAIL);
  }

  if(!chatbot_contract_path(dir, sizeof(dir)))
    return(FAIL);

  snprintf(path, sizeof(path), "%s/%s.txt", dir, name);

  raw = mp_slurp(path);

  if(raw == NULL)
  {
    clam(CLAM_WARN, "chatbot",
        "cannot read contract file '%s': %s", path, strerror(errno));
    return(FAIL);
  }

  body_ptr = mp_skip_frontmatter(raw);

  while(*body_ptr == '\n' || *body_ptr == '\r')
    body_ptr++;

  if(body_ptr[0] == '\0')
  {
    clam(CLAM_WARN, "chatbot", "contract file '%s' is empty", path);
    mem_free(raw);
    return(FAIL);
  }

  *out_body = mem_strdup("chatbot", "contract", body_ptr);
  mem_free(raw);

  if(*out_body == NULL)
    return(FAIL);

  return(SUCCESS);
}

bool
chatbot_personality_read_header(const char *name, persona_header_t *hdr)
{
  char interests_buf[32];
  const char *body_ptr;
  bool rc;
  char *raw;
  struct stat sb;
  char path[CHATBOT_PERSONALITY_PATH_SZ];

  if(hdr == NULL)
    return(FAIL);

  memset(hdr, 0, sizeof(*hdr));

  if(name == NULL || name[0] == '\0')
  {
    snprintf(hdr->err, sizeof(hdr->err), "empty name");
    return(SUCCESS);
  }

  if(!mp_resolve_path(name, path, sizeof(path)))
  {
    snprintf(hdr->err, sizeof(hdr->err), "bad path");
    return(SUCCESS);
  }

  if(stat(path, &sb) == 0)
    hdr->bytes = sb.st_size;

  raw = mp_slurp(path);

  if(raw == NULL)
  {
    snprintf(hdr->err, sizeof(hdr->err),
        "cannot read: %s", strerror(errno));
    return(SUCCESS);
  }

  // Scratch interests buffer just large enough to disable spillover;
  // its contents are discarded for the header-only listing.
  body_ptr = NULL;

  rc = mp_parse_frontmatter(raw,
      hdr->name,        sizeof(hdr->name),
      hdr->description, sizeof(hdr->description),
      interests_buf,    sizeof(interests_buf),
      hdr->version,     sizeof(hdr->version),
      &body_ptr, hdr->err, sizeof(hdr->err));

  mem_free(raw);

  hdr->ok = (rc == SUCCESS);

  if(hdr->ok && hdr->name[0] == '\0')
    snprintf(hdr->name, sizeof(hdr->name), "%s", name);

  return(SUCCESS);
}

size_t
chatbot_personality_scan(chatbot_personality_visit_cb cb, void *data)
{
  size_t         count;
  struct dirent *ent;
  char dir[PATH_MAX];

  DIR *d;

  if(cb == NULL)
    return(0);

  if(!chatbot_personality_path(dir, sizeof(dir)))
    return(0);

  d = opendir(dir);

  if(d == NULL)
  {
    clam(CLAM_WARN, "chatbot",
        "personalitypath: cannot open '%s': %s",
        dir, strerror(errno));
    return(0);
  }

  count = 0;

  while((ent = readdir(d)) != NULL)
  {
    const char *nm   = ent->d_name;
    size_t      nlen = strlen(nm);
    char   stem[CHATBOT_PERSONALITY_NAME_SZ];
    size_t slen;

    if(nlen < 5)                                  // need at least x.txt
      continue;

    if(strcmp(nm + nlen - 4, ".txt") != 0)
      continue;

    slen = nlen - 4 < sizeof(stem) - 1
        ? nlen - 4
        : sizeof(stem) - 1;

    memcpy(stem, nm, slen);
    stem[slen] = '\0';

    cb(stem, data);
    count++;
  }

  closedir(d);
  return(count);
}

// Read only the frontmatter header of a contract file. Mirrors
// chatbot_personality_read_header but resolves against
// bot.chat.contractpath and ignores interests/version (contracts
// only declare name + description). persona_header_t.version and
// .bytes are left populated with "-" and file size for parity with
// the personality listing.
bool
chatbot_contract_read_header(const char *name, persona_header_t *hdr)
{
  char interests_buf[32];
  const char *body_ptr;
  bool rc;
  char *raw;
  struct stat sb;
  char path[CHATBOT_PERSONALITY_PATH_SZ];
  char dir[PATH_MAX];

  if(hdr == NULL)
    return(FAIL);

  memset(hdr, 0, sizeof(*hdr));

  if(name == NULL || name[0] == '\0')
  {
    snprintf(hdr->err, sizeof(hdr->err), "empty name");
    return(SUCCESS);
  }

  if(strchr(name, '/') != NULL)
  {
    snprintf(hdr->err, sizeof(hdr->err), "bad path");
    return(SUCCESS);
  }

  if(!chatbot_contract_path(dir, sizeof(dir)))
  {
    snprintf(hdr->err, sizeof(hdr->err), "contractpath unresolved");
    return(SUCCESS);
  }

  snprintf(path, sizeof(path), "%s/%s.txt", dir, name);

  if(stat(path, &sb) == 0)
    hdr->bytes = sb.st_size;

  raw = mp_slurp(path);

  if(raw == NULL)
  {
    snprintf(hdr->err, sizeof(hdr->err),
        "cannot read: %s", strerror(errno));
    return(SUCCESS);
  }

  // Scratch interests buffer is unused for contracts; kept small.
  body_ptr = NULL;

  rc = mp_parse_frontmatter(raw,
      hdr->name,        sizeof(hdr->name),
      hdr->description, sizeof(hdr->description),
      interests_buf,    sizeof(interests_buf),
      hdr->version,     sizeof(hdr->version),
      &body_ptr, hdr->err, sizeof(hdr->err));

  mem_free(raw);

  hdr->ok = (rc == SUCCESS);

  if(hdr->ok && hdr->name[0] == '\0')
    snprintf(hdr->name, sizeof(hdr->name), "%s", name);

  return(SUCCESS);
}

// Walk the contract directory and invoke cb() once per *.txt stem.
// Mirrors chatbot_personality_scan.
size_t
chatbot_contract_scan(chatbot_personality_visit_cb cb, void *data)
{
  size_t         count;
  struct dirent *ent;
  char dir[PATH_MAX];

  DIR *d;

  if(cb == NULL)
    return(0);

  if(!chatbot_contract_path(dir, sizeof(dir)))
    return(0);

  d = opendir(dir);

  if(d == NULL)
  {
    clam(CLAM_WARN, "chatbot",
        "contractpath: cannot open '%s': %s",
        dir, strerror(errno));
    return(0);
  }

  count = 0;

  while((ent = readdir(d)) != NULL)
  {
    const char *nm   = ent->d_name;
    size_t      nlen = strlen(nm);
    char   stem[CHATBOT_PERSONALITY_NAME_SZ];
    size_t slen;

    if(nlen < 5)
      continue;

    if(strcmp(nm + nlen - 4, ".txt") != 0)
      continue;

    slen = nlen - 4 < sizeof(stem) - 1
        ? nlen - 4
        : sizeof(stem) - 1;

    memcpy(stem, nm, slen);
    stem[slen] = '\0';

    cb(stem, data);
    count++;
  }

  closedir(d);
  return(count);
}
