// botmanager — MIT
// The canonical fact vocabulary: which keys mean the same thing twice.
#include "fact_vocab.h"

#include <strings.h>

// Ordered for the prompt: attributes (what someone IS) before
// preferences (what someone LIKES), and within each group the ones a
// conversation produces most often first. The operator strikes a row by
// deleting it here — that is the whole edit, since the prompt block and
// the /remember validator are both rendered from this table.
static const fact_vocab_t fact_vocab[] =
{
  { "location",       MEM_FACT_ATTRIBUTE,
    "where they permanently live, as \"City, ST\" or \"City, Country\" —"
    " never a bare state or country on its own" },
  { "postal_code",    MEM_FACT_ATTRIBUTE,
    "their home zip or postal code, the code alone" },
  { "hometown",       MEM_FACT_ATTRIBUTE,
    "where they grew up, if different from where they live now" },
  { "birthday",       MEM_FACT_ATTRIBUTE,
    "their own birthday as \"MM-DD\", zero-padded, no year" },
  { "timezone",       MEM_FACT_ATTRIBUTE,
    "the timezone they live in" },
  { "occupation",     MEM_FACT_ATTRIBUTE,
    "what they do for a living" },
  { "employer",       MEM_FACT_ATTRIBUTE,
    "who they work for" },
  { "vehicle",        MEM_FACT_ATTRIBUTE,
    "what they drive" },
  { "partner",        MEM_FACT_ATTRIBUTE,
    "their spouse or partner" },
  { "children",       MEM_FACT_ATTRIBUTE,
    "the children they have, e.g. \"a son and a daughter\"" },
  { "pets",           MEM_FACT_ATTRIBUTE,
    "the animals they live with" },

  { "favorite_color", MEM_FACT_PREFERENCE, "their favourite colour" },
  { "favorite_movie", MEM_FACT_PREFERENCE, "their favourite film" },
  { "favorite_game",  MEM_FACT_PREFERENCE, "their favourite game" },
  { "favorite_food",  MEM_FACT_PREFERENCE, "their favourite food" },
  { "favorite_music", MEM_FACT_PREFERENCE,
    "the music or band they love" },
  { "favorite_team",  MEM_FACT_PREFERENCE,
    "the team they follow" },
  { "drink",          MEM_FACT_PREFERENCE,
    "what they drink — coffee, tea, a beer" },
  { "hobby",          MEM_FACT_PREFERENCE,
    "what they do for fun" }
};

static const size_t fact_vocab_n =
    sizeof(fact_vocab) / sizeof(fact_vocab[0]);

const fact_vocab_t *
fact_vocab_lookup(const char *key)
{
  if(key == NULL || key[0] == '\0')
    return(NULL);

  for(size_t i = 0; i < fact_vocab_n; i++)
    if(strcasecmp(fact_vocab[i].key, key) == 0)
      return(&fact_vocab[i]);

  return(NULL);
}

size_t
fact_vocab_count(void)
{
  return(fact_vocab_n);
}

const fact_vocab_t *
fact_vocab_at(size_t i)
{
  if(i >= fact_vocab_n)
    return(NULL);

  return(&fact_vocab[i]);
}
