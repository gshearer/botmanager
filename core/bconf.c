#define BCONF_INTERNAL
#include "bconf.h"

// -----------------------------------------------------------------------
// Resolve the config file path.
// path: explicit path or NULL for default
// out: destination buffer
// out_sz: size of destination buffer
// -----------------------------------------------------------------------
static void
resolve_path(const char *path, char *out, size_t out_sz)
{
  if(path != NULL)
  {
    strncpy(out, path, out_sz - 1);
    out[out_sz - 1] = '\0';
    return;
  }

  const char *base = getenv("XDG_CONFIG_HOME");

  if(base != NULL && base[0] != '\0')
  {
    snprintf(out, out_sz, "%s/botmanager/botman.conf", base);
  }

  else
  {
    const char *home = getenv("HOME");

    if(home != NULL && home[0] != '\0')
      snprintf(out, out_sz, "%s/.config/botmanager/botman.conf", home);

    else
      snprintf(out, out_sz, ".config/botmanager/botman.conf");
  }
}

// -----------------------------------------------------------------------
// Store or overwrite a key-value pair.
// key: configuration key
// val: configuration value
// -----------------------------------------------------------------------
static void
store(const char *key, const char *val)
{
  // Check for existing key (case-insensitive) — overwrite.
  for(uint32_t i = 0; i < entry_count; i++)
  {
    if(strncasecmp(entries[i].key, key, BCONF_KEY_SZ) == 0)
    {
      strncpy(entries[i].val, val, BCONF_VAL_SZ - 1);
      entries[i].val[BCONF_VAL_SZ - 1] = '\0';
      return;
    }
  }

  if(entry_count >= BCONF_MAX)
  {
    clam(CLAM_WARN, "bconf", "max entries reached (%u), ignoring '%s'",
        BCONF_MAX, key);
    return;
  }

  strncpy(entries[entry_count].key, key, BCONF_KEY_SZ - 1);
  entries[entry_count].key[BCONF_KEY_SZ - 1] = '\0';
  strncpy(entries[entry_count].val, val, BCONF_VAL_SZ - 1);
  entries[entry_count].val[BCONF_VAL_SZ - 1] = '\0';
  entry_count++;
}

// -----------------------------------------------------------------------
// Parse one line. Expected format: KEY="VALUE"
// returns: true if the line was parsed, false if skipped
// line: input line to parse
// linenum: line number for diagnostics
// -----------------------------------------------------------------------
static bool
parse_line(const char *line, uint32_t linenum)
{
  // Skip leading whitespace.
  while(*line == ' ' || *line == '\t')
    line++;

  // Skip empty lines and comments.
  if(*line == '\0' || *line == '\n' || *line == '#')
    return(false);

  // Find '='.
  const char *eq = strchr(line, '=');

  if(eq == NULL || eq == line)
  {
    clam(CLAM_WARN, "bconf", "line %u: missing '='", linenum);
    return(false);
  }

  // Extract key (trim trailing whitespace before '=').
  char key[BCONF_KEY_SZ];
  size_t klen = (size_t)(eq - line);

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
  const char *vstart = eq + 1;

  while(*vstart == ' ' || *vstart == '\t')
    vstart++;

  if(*vstart != '"')
  {
    clam(CLAM_WARN, "bconf", "line %u: value must be quoted (\"%s\"=\"...\")",
        linenum, key);
    return(false);
  }

  vstart++;  // skip opening quote
  const char *vend = strchr(vstart, '"');

  if(vend == NULL)
  {
    clam(CLAM_WARN, "bconf", "line %u: missing closing quote for '%s'",
        linenum, key);
    return(false);
  }

  char val[BCONF_VAL_SZ];
  size_t vlen = (size_t)(vend - vstart);

  if(vlen >= BCONF_VAL_SZ)
    vlen = BCONF_VAL_SZ - 1;

  memcpy(val, vstart, vlen);
  val[vlen] = '\0';

  store(key, val);
  return(true);
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// returns: SUCCESS or FAIL (file not found or unreadable)
// path: config file path, or NULL for default location
bool
bconf_init(const char *path)
{
  char filepath[PATH_MAX];

  resolve_path(path, filepath, sizeof(filepath));

  entries = mem_alloc("bconf", "entries", sizeof(bconf_entry_t) * BCONF_MAX);
  entry_count = 0;

  FILE *fp = fopen(filepath, "r");

  if(fp == NULL)
  {
    clam(CLAM_WARN, "bconf_init", "cannot open '%s': %s",
        filepath, strerror(errno));
    bconf_ready = true;
    return(FAIL);
  }

  char line[BCONF_LINE_SZ];
  uint32_t linenum = 0;
  uint32_t parsed = 0;

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

// returns: value string or NULL if not found
// key: configuration key (case-insensitive lookup)
const char *
bconf_get(const char *key)
{
  for(uint32_t i = 0; i < entry_count; i++)
  {
    if(strncasecmp(entries[i].key, key, BCONF_KEY_SZ) == 0)
      return(entries[i].val);
  }

  return(NULL);
}

// returns: integer value, or def if not found or invalid
// key: configuration key
// def: default value
int
bconf_get_int(const char *key, int def)
{
  const char *val = bconf_get(key);

  if(val == NULL)
    return(def);

  char *end;
  long n = strtol(val, &end, 10);

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
