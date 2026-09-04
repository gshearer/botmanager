// botmanager — MIT
// attack chatter: the pit's own narration, kept just long enough for a
// combatant to banter about it. Board #90.

#define ATTACK_INTERNAL
#include "attack.h"

#include "colors.h"

#include <stdio.h>
#include <string.h>

// The tag every line wears in the prompt. It exists to keep the pit's
// account of an event apart from the bot's own speech and from what
// people actually said to it: without it a combatant reads "hedgehogg
// swings a frying pan" as a line it produced. `[[situation]]` in
// personalities/base.txt tells the mind what it means and that it is
// never to be reproduced -- an unexplained tag is a tag a model repeats.
#define ATK_CHATTER_TAG "[pit] "

// One line the room saw. `round_id` 0 marks a slot nothing has been
// written to yet; a real round id is a BIGSERIAL and never 0.
typedef struct
{
  int64_t round_id;
  char    text[ATK_CHATTER_LINE_SZ];
} atk_chatter_ent_t;

// Guards the ring and the cursor, and nothing else -- never held across
// a database call, a send, or a clam(). atk_turn_lock is taken OUTSIDE
// this one on every writing path (the pit narrates under it, on
// purpose, so two turns cannot interleave); the reader takes this one
// alone, so the two orders cannot meet.
static pthread_mutex_t   atk_chatter_lock = PTHREAD_MUTEX_INITIALIZER;
static atk_chatter_ent_t atk_chatter_ring[ATK_CHATTER_MAX];
// Monotonic across the process, never wrapped back: `n % ATK_CHATTER_MAX`
// is the slot and `n` itself is the age ordering, which is what lets the
// reader walk backwards without a second field.
static uint64_t          atk_chatter_n = 0;

void
atk_chatter_note(int64_t round_id, const char *line)
{
  atk_chatter_ent_t *e;
  char               plain[ATK_CHATTER_LINE_SZ];

  if(round_id == 0 || line == NULL || line[0] == '\0')
    return;

  // Stripped, not translated: the mind is not a terminal and a marker
  // that reaches it comes back as a stray letter welded to a nickname.
  // Done before the lock -- it is the only work here that is not a copy.
  color_translate(plain, sizeof(plain), line, NULL);

  if(plain[0] == '\0')
    return;

  pthread_mutex_lock(&atk_chatter_lock);

  e = &atk_chatter_ring[atk_chatter_n % ATK_CHATTER_MAX];

  e->round_id = round_id;
  strlcpy(e->text, plain, sizeof e->text);

  atk_chatter_n++;

  pthread_mutex_unlock(&atk_chatter_lock);
}

size_t
atk_chatter_recent(int64_t round_id, char *out, size_t out_sz)
{
  uint32_t slot[ATK_CHATTER_SHOW];
  uint64_t age;
  uint64_t oldest;
  size_t   pos   = 0;
  size_t   want  = 0;
  uint32_t n     = 0;
  uint32_t fits  = 0;

  if(round_id == 0 || out == NULL || out_sz == 0)
    return(0);

  out[0] = '\0';

  pthread_mutex_lock(&atk_chatter_lock);

  // Backwards from the newest, so a busy pit in another room can never
  // cost this round its most recent lines -- it costs it the oldest,
  // which is what the ring is for.
  oldest = (atk_chatter_n > ATK_CHATTER_MAX)
      ? atk_chatter_n - ATK_CHATTER_MAX : 0;

  for(age = atk_chatter_n; age > oldest && n < ATK_CHATTER_SHOW; age--)
  {
    const uint32_t at = (uint32_t)((age - 1) % ATK_CHATTER_MAX);

    if(atk_chatter_ring[at].round_id == round_id)
      slot[n++] = at;
  }

  // How many of those the caller's buffer can take, counted from the
  // NEWEST outwards: a line that will not fit whole takes every older
  // line with it rather than reaching the mind cut in half.
  for(fits = 0; fits < n; fits++)
  {
    const size_t need = sizeof(ATK_CHATTER_TAG) - 1
        + strnlen(atk_chatter_ring[slot[fits]].text, ATK_CHATTER_LINE_SZ)
        + 1;

    if(want + need >= out_sz)
      break;

    want += need;
  }

  // Emitted oldest-first, which is the order the room heard them in.
  while(fits > 0)
  {
    fits--;

    pos += (size_t)snprintf(out + pos, out_sz - pos, "%s%s%s",
        (pos > 0) ? "\n" : "", ATK_CHATTER_TAG,
        atk_chatter_ring[slot[fits]].text);
  }

  pthread_mutex_unlock(&atk_chatter_lock);

  return(pos);
}
