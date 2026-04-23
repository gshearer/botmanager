// botmanager -- MIT
// Chat-plugin identity registry: one entry per method_kind bundles the
// signer, pair-scorer, and token-scorer published by the method plugin
// via plugin_dlsym. Replaces the in-core method_driver_t dossier hooks
// retired in ND4.

#include "dossier.h"
#include "identity.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clam.h"
#include "common.h"

#define CHAT_IDENTITY_MAX    16
#define CHAT_IDENTITY_JSON_SZ 2048

typedef struct
{
  char                         method_kind[DOSSIER_METHOD_KIND_SZ];
  chat_identity_signer_t       signer;
  chat_identity_scorer_t       scorer;
  chat_identity_token_scorer_t token_scorer;
  bool                         used;
} chat_identity_slot_t;

static chat_identity_slot_t chat_identity_reg[CHAT_IDENTITY_MAX];
static pthread_mutex_t      chat_identity_mutex;
static bool                 chat_identity_ready;

// Registry helpers. Caller holds chat_identity_mutex for every _locked
// function; the _lookup wrappers take the lock and release before
// returning the function pointer (which is stable after registration).

static chat_identity_slot_t *
chat_identity_find_locked(const char *method_kind)
{
  for(uint32_t i = 0; i < CHAT_IDENTITY_MAX; i++)
  {
    chat_identity_slot_t *s = &chat_identity_reg[i];

    if(s->used && strncmp(s->method_kind, method_kind,
           sizeof(s->method_kind)) == 0)
      return(s);
  }

  return(NULL);
}

bool
chat_identity_register(const char *method_kind,
    chat_identity_signer_t       signer,
    chat_identity_scorer_t       scorer,
    chat_identity_token_scorer_t token_scorer)
{
  chat_identity_slot_t *slot;

  if(!chat_identity_ready || method_kind == NULL || method_kind[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&chat_identity_mutex);

  slot = chat_identity_find_locked(method_kind);

  if(slot != NULL)
  {
    slot->signer       = signer;
    slot->scorer       = scorer;
    slot->token_scorer = token_scorer;
    pthread_mutex_unlock(&chat_identity_mutex);
    return(SUCCESS);
  }

  for(uint32_t i = 0; i < CHAT_IDENTITY_MAX; i++)
  {
    if(!chat_identity_reg[i].used)
    {
      chat_identity_reg[i].used         = true;
      chat_identity_reg[i].signer       = signer;
      chat_identity_reg[i].scorer       = scorer;
      chat_identity_reg[i].token_scorer = token_scorer;
      snprintf(chat_identity_reg[i].method_kind,
          sizeof(chat_identity_reg[i].method_kind), "%s", method_kind);
      pthread_mutex_unlock(&chat_identity_mutex);

      clam(CLAM_INFO, "identity",
          "registered method_kind='%s' signer=%s scorer=%s token=%s",
          method_kind,
          signer       != NULL ? "yes" : "no",
          scorer       != NULL ? "yes" : "no",
          token_scorer != NULL ? "yes" : "no");
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&chat_identity_mutex);

  clam(CLAM_WARN, "identity",
      "registry full (max %u); '%s' not registered",
      CHAT_IDENTITY_MAX, method_kind);
  return(FAIL);
}

void
chat_identity_unregister(const char *method_kind)
{
  chat_identity_slot_t *slot;

  if(!chat_identity_ready || method_kind == NULL)
    return;

  pthread_mutex_lock(&chat_identity_mutex);

  slot = chat_identity_find_locked(method_kind);

  if(slot != NULL)
  {
    slot->used          = false;
    slot->signer        = NULL;
    slot->scorer        = NULL;
    slot->token_scorer  = NULL;
    slot->method_kind[0] = '\0';
  }

  pthread_mutex_unlock(&chat_identity_mutex);
}

chat_identity_scorer_t
chat_identity_scorer_lookup(const char *method_kind)
{
  chat_identity_scorer_t fn = NULL;
  chat_identity_slot_t  *slot;

  if(!chat_identity_ready || method_kind == NULL)
    return(NULL);

  pthread_mutex_lock(&chat_identity_mutex);
  slot = chat_identity_find_locked(method_kind);
  if(slot != NULL) fn = slot->scorer;
  pthread_mutex_unlock(&chat_identity_mutex);

  return(fn);
}

chat_identity_token_scorer_t
chat_identity_token_scorer_lookup(const char *method_kind)
{
  chat_identity_token_scorer_t fn = NULL;
  chat_identity_slot_t        *slot;

  if(!chat_identity_ready || method_kind == NULL)
    return(NULL);

  pthread_mutex_lock(&chat_identity_mutex);
  slot = chat_identity_find_locked(method_kind);
  if(slot != NULL) fn = slot->token_scorer;
  pthread_mutex_unlock(&chat_identity_mutex);

  return(fn);
}

static chat_identity_signer_t
chat_identity_signer_lookup(const char *method_kind)
{
  chat_identity_signer_t fn = NULL;
  chat_identity_slot_t  *slot;

  if(!chat_identity_ready || method_kind == NULL)
    return(NULL);

  pthread_mutex_lock(&chat_identity_mutex);
  slot = chat_identity_find_locked(method_kind);
  if(slot != NULL) fn = slot->signer;
  pthread_mutex_unlock(&chat_identity_mutex);

  return(fn);
}

bool
chat_identity_signature_build(const method_msg_t *msg,
    char *out_json, size_t out_sz)
{
  chat_identity_signer_t  fn;
  const char             *method_kind;

  if(msg == NULL || msg->inst == NULL || out_json == NULL || out_sz == 0)
    return(FAIL);

  out_json[0] = '\0';

  method_kind = method_inst_kind(msg->inst);
  if(method_kind == NULL || method_kind[0] == '\0')
    return(FAIL);

  fn = chat_identity_signer_lookup(method_kind);
  if(fn == NULL)
    return(FAIL);

  return(fn(msg, out_json, out_sz));
}

dossier_id_t
chat_user_dossier_id(const method_msg_t *msg, uint32_t ns_id,
    const char *display_label, bool create_if_missing)
{
  dossier_sig_t  sig;
  char           sig_json[CHAT_IDENTITY_JSON_SZ];
  const char    *method_kind;

  if(msg == NULL || msg->inst == NULL)
    return(0);

  method_kind = method_inst_kind(msg->inst);
  if(method_kind == NULL || method_kind[0] == '\0')
    return(0);

  if(chat_identity_signature_build(msg, sig_json, sizeof(sig_json))
      != SUCCESS)
    return(0);

  sig.method_kind = method_kind;
  sig.sig_json    = sig_json;

  return(dossier_resolve(ns_id, &sig,
      display_label != NULL ? display_label : "", create_if_missing));
}

void
chat_identity_init(void)
{
  if(chat_identity_ready)
    return;

  pthread_mutex_init(&chat_identity_mutex, NULL);
  memset(chat_identity_reg, 0, sizeof(chat_identity_reg));
  chat_identity_ready = true;

  clam(CLAM_INFO, "identity", "chat identity registry initialized");
}

void
chat_identity_exit(void)
{
  if(!chat_identity_ready)
    return;

  chat_identity_ready = false;
  pthread_mutex_destroy(&chat_identity_mutex);
  memset(chat_identity_reg, 0, sizeof(chat_identity_reg));

  clam(CLAM_INFO, "identity", "chat identity registry shut down");
}
