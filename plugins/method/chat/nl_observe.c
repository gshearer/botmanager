// botmanager — MIT
// NL-bridge post-dispatch slot observers.
//
// PL5: generic observer for typed NL slots. Runs inside the chat plugin
// so chat-specific side effects (dossier facts) stay out of service +
// command-surface plugins. Today only CMD_NL_ARG_LOCATION is observed
// — if it resolves to a real place via openweather's sync geocoder, we
// attach a `city_of_interest:<canon>` fact to the sender's dossier.

#define CHATBOT_INTERNAL
#include "chatbot.h"

#include "alloc.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "dossier.h"
#include "identity.h"
#include "memory.h"
#include "method.h"
#include "plugin.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define OBS_CTX   "nl_observe"
#define OBS_CONF  0.60f

typedef struct
{
  uint32_t        ns_id;
  char            sender[METHOD_SENDER_SZ];
  char            channel[METHOD_CHANNEL_SZ];
  char            metadata[METHOD_META_SZ];
  char            user_label[128];
  method_inst_t  *inst;
  bot_inst_t     *bot;
} nl_observe_task_data_t;

typedef bool (*geocode_city_fn_t)(const char *, char *, size_t);

static geocode_city_fn_t  fn_geocode_city_sync;
static bool               geocode_resolved;

static bool
resolve_openweather_geocode(void)
{
  if(!geocode_resolved)
  {
    union { void *obj; geocode_city_fn_t fn; } u;

    u.obj = plugin_dlsym("openweather",
        "openweather_geocode_city_sync");
    fn_geocode_city_sync = u.fn;
    geocode_resolved = true;
  }

  return(fn_geocode_city_sync != NULL);
}

// Lowercase + underscore-collapse a user-supplied location so the fact
// key stays stable across casing and punctuation variants.
static void
canonicalize_location(const char *in, char *out, size_t out_sz)
{
  size_t i;
  size_t j;

  if(out_sz == 0) return;

  for(i = 0, j = 0; in[i] != '\0' && j + 1 < out_sz; i++)
  {
    unsigned char c = (unsigned char)in[i];

    if(c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');

    if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      out[j++] = (char)c;
    else if(j > 0 && out[j - 1] != '_')
      out[j++] = '_';
  }

  while(j > 0 && out[j - 1] == '_') j--;
  out[j] = '\0';
}

static void
nl_observe_task(task_t *t)
{
  nl_observe_task_data_t *d = t->data;
  method_msg_t            synth;
  mem_dossier_fact_t      fact;
  char                    zip[32];
  char                    canon[128];
  int64_t                 did;
  time_t                  now;

  if(d == NULL)
  {
    t->state = TASK_ENDED;
    return;
  }

  if(!resolve_openweather_geocode())
  {
    clam(CLAM_DEBUG, OBS_CTX, "openweather not loaded; skip");
    goto done;
  }

  zip[0] = '\0';

  if(fn_geocode_city_sync(d->user_label, zip, sizeof(zip)) != SUCCESS)
  {
    clam(CLAM_DEBUG, OBS_CTX,
        "geocode '%s' FAIL (hallucinated or misspelled)",
        d->user_label);
    goto done;
  }

  canonicalize_location(d->user_label, canon, sizeof(canon));

  if(canon[0] == '\0')
  {
    clam(CLAM_DEBUG, OBS_CTX,
        "canonicalized '%s' to empty; skip", d->user_label);
    goto done;
  }

  memset(&synth, 0, sizeof(synth));
  synth.inst = d->inst;
  snprintf(synth.sender,   sizeof(synth.sender),   "%s", d->sender);
  snprintf(synth.channel,  sizeof(synth.channel),  "%s", d->channel);
  snprintf(synth.metadata, sizeof(synth.metadata), "%s", d->metadata);

  did = chat_user_dossier_id(&synth, d->ns_id, d->sender, true);

  if(did <= 0)
  {
    clam(CLAM_DEBUG, OBS_CTX,
        "chat_user_dossier_id returned %lld", (long long)did);
    goto done;
  }

  memset(&fact, 0, sizeof(fact));
  fact.dossier_id = did;
  fact.kind       = MEM_FACT_ATTRIBUTE;
  snprintf(fact.fact_key,   sizeof(fact.fact_key),
      "city_of_interest:%s", canon);
  snprintf(fact.fact_value, sizeof(fact.fact_value), "%s",
      d->user_label);
  snprintf(fact.source,     sizeof(fact.source),     "%s", OBS_CTX);
  snprintf(fact.channel,    sizeof(fact.channel),    "%s", d->channel);
  fact.confidence = OBS_CONF;

  now = time(NULL);
  fact.observed_at = now;
  fact.last_seen   = now;

  if(memory_upsert_dossier_fact(&fact, MEM_MERGE_HIGHER_CONF) != SUCCESS)
    clam(CLAM_DEBUG, OBS_CTX,
        "upsert city_of_interest='%s' FAIL did=%lld",
        d->user_label, (long long)did);
  else
    clam(CLAM_INFO, OBS_CTX,
        "noted city_of_interest='%s' did=%lld",
        d->user_label, (long long)did);

done:
  mem_free(d);
  t->state = TASK_ENDED;
}

void
chatbot_nl_observe_location_slot(bot_inst_t *bot,
    method_inst_t *inst, uint32_t ns_id, const char *sender,
    const char *channel, const char *metadata,
    const cmd_nl_t *nl, const char *args_post_subst)
{
  // v1 observes only the single-slot CMD_NL_ARG_LOCATION case — the only
  // current consumer (/weather). Multi-slot tokenisation is a future
  // extension when a second observer-eligible command appears.

  const char             *value;
  nl_observe_task_data_t *d;
  task_t                 *t;

  if(nl == NULL || nl->slots == NULL || nl->slot_count != 1)
    return;

  if(nl->slots[0].type != CMD_NL_ARG_LOCATION)
    return;

  if(sender == NULL || sender[0] == '\0' || args_post_subst == NULL)
    return;

  value = args_post_subst;
  while(*value == ' ' || *value == '\t') value++;

  if(value[0] == '\0')
    return;

  d = mem_alloc(OBS_CTX, "task_data", sizeof(*d));
  if(d == NULL) return;

  memset(d, 0, sizeof(*d));
  d->ns_id = ns_id;
  d->inst  = inst;
  d->bot   = bot;
  snprintf(d->sender,     sizeof(d->sender),     "%s", sender);
  snprintf(d->channel,    sizeof(d->channel),    "%s",
      channel != NULL ? channel : "");
  snprintf(d->metadata,   sizeof(d->metadata),   "%s",
      metadata != NULL ? metadata : "");
  snprintf(d->user_label, sizeof(d->user_label), "%s", value);

  t = task_add(OBS_CTX, TASK_ANY, 200, nl_observe_task, d);

  if(t == NULL)
  {
    mem_free(d);
    clam(CLAM_WARN, OBS_CTX,
        "task spawn failed sender=%s value='%.40s'", sender, value);
  }
}
