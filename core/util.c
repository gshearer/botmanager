#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "clam.h"

// -----------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------

void
util_init(void)
{
  unsigned int seed;

  if(getrandom(&seed, sizeof(seed), 0) == sizeof(seed))
  {
    srand(seed);
    clam(CLAM_INFO, "util", "PRNG seeded via getrandom()");
  }
  else
  {
    seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    srand(seed);
    clam(CLAM_WARN, "util",
        "getrandom() unavailable, PRNG seeded from time/pid");
  }
}

// -----------------------------------------------------------------------
// Random number generation
// -----------------------------------------------------------------------

int
util_rand(int upper)
{
  return(rand() % upper);
}

// -----------------------------------------------------------------------
// Formatting helpers
// -----------------------------------------------------------------------

void
util_fmt_bytes(size_t bytes, char *buf, size_t sz)
{
  if(bytes < 1024)
    snprintf(buf, sz, "%zuB", bytes);
  else if(bytes < 1024 * 1024)
    snprintf(buf, sz, "%.1fK", (double)bytes / 1024.0);
  else if(bytes < 1024 * 1024 * 1024)
    snprintf(buf, sz, "%.1fM", (double)bytes / (1024.0 * 1024.0));
  else
    snprintf(buf, sz, "%.1fG", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

void
util_fmt_duration(time_t secs, char *buf, size_t sz)
{
  if(secs < 0)      secs = 0;

  if(secs < 60)
    snprintf(buf, sz, "%lds", (long)secs);
  else if(secs < 3600)
    snprintf(buf, sz, "%ldm%lds", (long)(secs / 60), (long)(secs % 60));
  else if(secs < 86400)
    snprintf(buf, sz, "%ldh%ldm",
        (long)(secs / 3600), (long)((secs % 3600) / 60));
  else
    snprintf(buf, sz, "%ldd%ldh",
        (long)(secs / 86400), (long)((secs % 86400) / 3600));
}

// -----------------------------------------------------------------------
// Hashing
// -----------------------------------------------------------------------

uint32_t
util_fnv1a(const char *s)
{
  uint32_t h = 2166136261u;

  for(; *s != '\0'; s++)
    h = (h ^ (uint8_t)*s) * 16777619u;

  return(h);
}

uint32_t
util_fnv1a_ci(const char *s)
{
  uint32_t h = 2166136261u;

  for(; *s != '\0'; s++)
    h = (h ^ (uint8_t)tolower((unsigned char)*s)) * 16777619u;

  return(h);
}
