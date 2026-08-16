// botmanager — MIT
// Bootstrap config loader: parses botman.conf into the KV store.
#define BCONF_INTERNAL
#include "bconf.h"

// argv and the environment are outside bytes: a path that does not fit
// names a different file, so a truncated one is refused rather than
// opened. FAIL leaves `out` unusable.
static bool
resolve_path(const char *path, char *out, size_t out_sz)
{
  const char *base;
  const char *home;
  int         n;

  if(path != NULL)
    return(strlcpy(out, path, out_sz) < out_sz ? SUCCESS : FAIL);

  base = getenv("XDG_CONFIG_HOME");

  if(base != NULL && base[0] != '\0')
    n = snprintf(out, out_sz, "%s/botmanager/botman.conf", base);

  else
  {
    home = getenv("HOME");

    if(home != NULL && home[0] != '\0')
      n = snprintf(out, out_sz, "%s/.config/botmanager/botman.conf", home);

    else
      n = snprintf(out, out_sz, ".config/botmanager/botman.conf");
  }

  return(n > 0 && (size_t)n < out_sz ? SUCCESS : FAIL);
}

// Both strings arrive already bounded — parse_line refuses a key or a
// value that does not fit — so neither copy below can truncate.
static void
store(const char *key, const char *val)
{
  // Check for existing key (case-insensitive) — overwrite.
  for(uint32_t i = 0; i < entry_count; i++)
  {
    if(strncasecmp(entries[i].key, key, BCONF_KEY_SZ) == 0)
    {
      strlcpy(entries[i].val, val, BCONF_VAL_SZ);
      return;
    }
  }

  if(entry_count >= BCONF_MAX)
  {
    clam(CLAM_WARN, "bconf", "max entries reached (%u), ignoring '%s'",
        BCONF_MAX, key);
    return;
  }

  strlcpy(entries[entry_count].key, key, BCONF_KEY_SZ);
  strlcpy(entries[entry_count].val, val, BCONF_VAL_SZ);
  entry_count++;
}

static bool
parse_line(const char *line, uint32_t linenum)
{
  const char *eq;
  char        key[BCONF_KEY_SZ];
  size_t      klen;
  const char *vstart;
  const char *vend;
  char        val[BCONF_VAL_SZ];
  size_t      vlen;

  // Skip leading whitespace.
  while(*line == ' ' || *line == '\t')
    line++;

  // Skip empty lines and comments.
  if(*line == '\0' || *line == '\n' || *line == '#')
    return(false);

  // Find '='.
  eq = strchr(line, '=');

  if(eq == NULL || eq == line)
  {
    clam(CLAM_WARN, "bconf", "line %u: missing '='", linenum);
    return(false);
  }

  // Extract key (trim trailing whitespace before '=').
  klen = (size_t)(eq - line);

  while(klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
    klen--;

  if(klen == 0 || klen >= BCONF_KEY_SZ)
  {
    clam(CLAM_WARN, "bconf", "line %u: invalid key", linenum);
    return(false);
  }

  memcpy(key, line, klen);
  key[klen] = '\0';

  // Value must be quoted: ="VALUE"
  vstart = eq + 1;

  while(*vstart == ' ' || *vstart == '\t')
    vstart++;

  if(*vstart != '"')
  {
    clam(CLAM_WARN, "bconf", "line %u: value must be quoted (\"%s\"=\"...\")",
        linenum, key);
    return(false);
  }

  vstart++;  // skip opening quote
  vend = strchr(vstart, '"');

  if(vend == NULL)
  {
    clam(CLAM_WARN, "bconf", "line %u: missing closing quote for '%s'",
        linenum, key);
    return(false);
  }

  vlen = (size_t)(vend - vstart);

  // A shortened value is a different setting — a database URL missing
  // its tail, a key missing its last characters — and every consumer
  // downstream would treat it as what the operator wrote.
  if(vlen >= BCONF_VAL_SZ)
  {
    clam(CLAM_WARN, "bconf", "line %u: value for '%s' exceeds %d bytes, "
        "ignoring", linenum, key, BCONF_VAL_SZ - 1);
    return(false);
  }

  memcpy(val, vstart, vlen);
  val[vlen] = '\0';

  store(key, val);
  return(true);
}

// Public API

bool
bconf_init(const char *path)
{
  char     filepath[PATH_MAX];
  FILE    *fp;
  char     line[BCONF_LINE_SZ];
  uint32_t linenum = 0;
  uint32_t parsed  = 0;

  entries = mem_alloc("bconf", "entries", sizeof(bconf_entry_t) * BCONF_MAX);
  entry_count = 0;

  if(resolve_path(path, filepath, sizeof(filepath)) != SUCCESS)
  {
    clam(CLAM_WARN, "bconf_init", "config path exceeds %d bytes", PATH_MAX - 1);
    bconf_ready = true;
    return(FAIL);
  }

  fp = fopen(filepath, "r");

  if(fp == NULL)
  {
    clam(CLAM_WARN, "bconf_init", "cannot open '%s': %s",
        filepath, strerror(errno));
    bconf_ready = true;
    return(FAIL);
  }

  while(fgets(line, BCONF_LINE_SZ, fp) != NULL)
  {
    linenum++;

    if(parse_line(line, linenum))
      parsed++;
  }

  fclose(fp);
  bconf_ready = true;

  clam(CLAM_INFO, "bconf_init", "loaded %u entries from '%s'",
      parsed, filepath);

  return(SUCCESS);
}

const char *
bconf_get(const char *key)
{
  for(uint32_t i = 0; i < entry_count; i++)
    if(strncasecmp(entries[i].key, key, BCONF_KEY_SZ) == 0)
      return(entries[i].val);

  return(NULL);
}

int
bconf_get_int(const char *key, int def)
{
  const char *val = bconf_get(key);
  char       *end;
  long        n;

  if(val == NULL)
    return(def);

  n = strtol(val, &end, 10);

  if(end == val || *end != '\0')
    return(def);

  return((int)n);
}

void
bconf_exit(void)
{
  if(!bconf_ready)
    return;

  clam(CLAM_DEBUG, "bconf_exit", "freeing %u entries", entry_count);

  mem_free(entries);
  entries = NULL;
  entry_count = 0;
  bconf_ready = false;
}
