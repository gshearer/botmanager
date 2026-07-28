// botmanager — MIT
// Method-plugin registry and subscriber chain into bot drivers.
#define METHOD_INTERNAL
#include "method.h"

// Method type registry — maps driver names to bitmasks and descriptions.

typedef struct
{
  const char   *name;
  method_type_t bit;
  const char   *desc;
} method_type_entry_t;

static const method_type_entry_t method_type_table[] = {
  { "botmanctl", METHOD_T_BOTMANCTL, "Unix socket interface for scripted control" },
  { "irc",       METHOD_T_IRC,       "Internet Relay Chat protocol" },
};

#define METHOD_TYPE_COUNT \
    (sizeof(method_type_table) / sizeof(method_type_table[0]))

// Subscriber freelist management.

static method_sub_t *
sub_get(void)
{
  method_sub_t *s;

  if(method_sub_freelist != NULL)
  {
    s = method_sub_freelist;
    method_sub_freelist = s->next;
    method_sub_free_count--;
    memset(s, 0, sizeof(*s));
    return(s);
  }

  s = mem_alloc("method", "subscriber", sizeof(*s));
  memset(s, 0, sizeof(*s));
  return(s);
}

static void
sub_put(method_sub_t *s)
{
  s->next = method_sub_freelist;
  method_sub_freelist = s;
  method_sub_free_count++;
}

// Instance management

// Register a new method instance.
// drv: driver interface (must not be NULL)
method_inst_t *
method_register(const method_driver_t *drv, const char *name)
{
  method_inst_t *inst;

  if(drv == NULL || name == NULL || name[0] == '\0')
  {
    clam(CLAM_WARN, "method_register", "invalid arguments");
    return(NULL);
  }

  pthread_mutex_lock(&method_mutex);

  // Check for duplicate name.
  for(method_inst_t *m = method_list; m != NULL; m = m->next)
  {
    if(strncasecmp(m->name, name, METHOD_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&method_mutex);
      clam(CLAM_WARN, "method_register",
          "duplicate instance name: '%s'", name);
      return(NULL);
    }
  }

  inst = mem_alloc("method", "instance", sizeof(*inst));
  memset(inst, 0, sizeof(*inst));
  strncpy(inst->name, name, METHOD_NAME_SZ - 1);
  inst->driver = drv;
  inst->state = METHOD_ENABLED;

  // Call driver create() if provided.
  if(drv->create != NULL)
  {
    inst->handle = drv->create(name);

    if(inst->handle == NULL)
    {
      pthread_mutex_unlock(&method_mutex);
      clam(CLAM_WARN, "method_register",
          "driver create() failed for '%s'", name);
      mem_free(inst);
      return(NULL);
    }
  }

  // Prepend to list.
  inst->next = method_list;
  method_list = inst;
  method_count++;

  pthread_mutex_unlock(&method_mutex);

  clam(CLAM_INFO, "method_register",
      "registered '%s' (driver: %s)", name, drv->name);
  return(inst);
}

bool
method_unregister(const char *name)
{
  method_inst_t *inst = NULL;
  method_inst_t *prev = NULL;
  method_sub_t  *s;

  if(name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&method_mutex);

  for(inst = method_list; inst != NULL; prev = inst, inst = inst->next)
    if(strncasecmp(inst->name, name, METHOD_NAME_SZ) == 0)
      break;

  if(inst == NULL)
  {
    pthread_mutex_unlock(&method_mutex);
    clam(CLAM_WARN, "method_unregister", "not found: '%s'", name);
    return(FAIL);
  }

  // Disconnect if running or available.
  if((inst->state == METHOD_RUNNING || inst->state == METHOD_AVAILABLE)
      && inst->driver->disconnect != NULL)
    inst->driver->disconnect(inst->handle);

  // Remove all subscribers.
  s = inst->subs;

  while(s != NULL)
  {
    method_sub_t *next = s->next;
    sub_put(s);
    s = next;
  }

  inst->subs = NULL;
  inst->sub_count = 0;

  // Call driver destroy().
  if(inst->driver->destroy != NULL && inst->handle != NULL)
    inst->driver->destroy(inst->handle);

  // Unlink from list.
  if(prev != NULL)
    prev->next = inst->next;
  else
    method_list = inst->next;

  method_count--;

  pthread_mutex_unlock(&method_mutex);

  clam(CLAM_INFO, "method_unregister", "unregistered '%s'", name);
  mem_free(inst);
  return(SUCCESS);
}

method_inst_t *
method_find(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(NULL);

  pthread_mutex_lock(&method_mutex);

  for(method_inst_t *m = method_list; m != NULL; m = m->next)
  {
    if(strncasecmp(m->name, name, METHOD_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&method_mutex);
      return(m);
    }
  }

  pthread_mutex_unlock(&method_mutex);
  return(NULL);
}

const char *
method_inst_name(const method_inst_t *inst)
{
  if(inst == NULL)
    return("(null)");

  return(inst->name);
}

const char *
method_inst_kind(const method_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL)
    return("(null)");

  return(inst->driver->name);
}

method_type_t
method_type_bit(const char *name)
{
  if(name == NULL)
    return(0);

  for(size_t i = 0; i < METHOD_TYPE_COUNT; i++)
    if(strcmp(name, method_type_table[i].name) == 0)
      return(method_type_table[i].bit);

  return(0);
}

const char *
method_type_desc(const char *name)
{
  if(name == NULL)
    return(NULL);

  for(size_t i = 0; i < METHOD_TYPE_COUNT; i++)
    if(strcmp(name, method_type_table[i].name) == 0)
      return(method_type_table[i].desc);

  return(NULL);
}

method_type_t
method_inst_type(const method_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL)
    return(0);

  return(method_type_bit(inst->driver->name));
}

void *
method_get_handle(const method_inst_t *inst)
{
  if(inst == NULL)
    return(NULL);

  return(inst->handle);
}

// Channel member listing

void
method_list_channel(method_inst_t *inst, const char *channel,
    method_chan_member_cb_t cb, void *data)
{
  if(inst == NULL || channel == NULL || cb == NULL)
    return;

  if(inst->driver->list_channel == NULL)
    return;

  inst->driver->list_channel(inst->handle, channel, cb, data);
}

void
method_list_joined_channels(method_inst_t *inst,
    method_joined_channel_cb_t cb, void *data)
{
  if(inst == NULL || cb == NULL)
    return;

  if(inst->driver->list_joined_channels == NULL)
    return;

  inst->driver->list_joined_channels(inst->handle, cb, data);
}

// Get the bot's own identity on a method instance.
bool
method_get_self(method_inst_t *inst, char *buf, size_t buf_sz)
{
  if(inst == NULL || buf == NULL || buf_sz == 0)
    return(FAIL);

  if(inst->driver->get_self == NULL)
    return(FAIL);

  return(inst->driver->get_self(inst->handle, buf, buf_sz));
}

// Participant removal

// Read-only probe: what the driver could do to `target` right now. No
// wire traffic, no state change.
method_eject_t
method_eject_probe(method_inst_t *inst, const char *channel,
    const char *target)
{
  if(inst == NULL || inst->driver == NULL || channel == NULL ||
      target == NULL)
    return(METHOD_EJECT_NONE);

  if(inst->driver->eject_probe == NULL)
    return(METHOD_EJECT_NONE);

  return(inst->driver->eject_probe(inst->handle, channel, target));
}

// Issue the removal. Success means the request reached the platform, not
// that the participant is gone — drivers do not wait for confirmation.
bool
method_eject(method_inst_t *inst, const char *channel, const char *target,
    method_eject_t force, const char *reason)
{
  bool rc;

  if(inst == NULL || inst->driver == NULL || channel == NULL ||
      target == NULL)
    return(FAIL);

  if(inst->driver->eject == NULL)
    return(FAIL);

  rc = inst->driver->eject(inst->handle, channel, target, force,
      reason != NULL ? reason : "");

  // The driver may clamp `force` down to what it can actually do, and it
  // does not report back which rung it used — so log the request, not an
  // outcome core cannot observe.
  if(rc == SUCCESS)
    clam(CLAM_INFO, "method_eject", "'%s': ejected %s from %s (force %d requested)",
        inst->name, target, channel, (int)force);

  return(rc);
}

// State management

method_state_t
method_get_state(const method_inst_t *inst)
{
  if(inst == NULL)
    return(METHOD_ENABLED);

  return(inst->state);
}

void
method_set_state(method_inst_t *inst, method_state_t state)
{
  method_state_t old;

  if(inst == NULL)
    return;

  old = inst->state;
  inst->state = state;

  // Track connection timestamp for uptime reporting.
  if(state == METHOD_AVAILABLE && old != METHOD_AVAILABLE)
    inst->connected_at = time(NULL);

  else if(state != METHOD_AVAILABLE)
    inst->connected_at = 0;

  clam(CLAM_DEBUG, "method_set_state", "'%s': %s -> %s",
      inst->name, method_state_name(old), method_state_name(state));
}

const char *
method_state_name(method_state_t s)
{
  switch(s)
  {
    case METHOD_ENABLED:   return("ENABLED");
    case METHOD_RUNNING:   return("RUNNING");
    case METHOD_AVAILABLE: return("AVAILABLE");
    default:               return("UNKNOWN");
  }
}

// Message delivery

bool
method_subscribe(method_inst_t *inst, const char *name,
    method_msg_cb_t cb, void *data)
{
  method_sub_t *s;

  if(inst == NULL || name == NULL || name[0] == '\0' || cb == NULL)
  {
    clam(CLAM_WARN, "method_subscribe", "invalid arguments");
    return(FAIL);
  }

  pthread_mutex_lock(&method_mutex);

  // Check for duplicate subscriber name on this instance.
  for(method_sub_t *it = inst->subs; it != NULL; it = it->next)
  {
    if(strncasecmp(it->name, name, METHOD_SUB_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&method_mutex);
      clam(CLAM_WARN, "method_subscribe",
          "duplicate subscriber '%s' on '%s'", name, inst->name);
      return(FAIL);
    }
  }

  // Cap the subscriber count so method_deliver's fixed-size snapshot
  // array can hold the whole list (no silent truncation on dispatch).
  if(inst->sub_count >= METHOD_MAX_SUBS)
  {
    pthread_mutex_unlock(&method_mutex);
    clam(CLAM_WARN, "method_subscribe",
        "'%s' subscriber cap (%d) reached on '%s'",
        name, METHOD_MAX_SUBS, inst->name);
    return(FAIL);
  }

  s = sub_get();
  strncpy(s->name, name, METHOD_SUB_NAME_SZ - 1);
  s->cb = cb;
  s->data = data;

  // Prepend to subscriber list.
  s->next = inst->subs;
  inst->subs = s;
  inst->sub_count++;

  pthread_mutex_unlock(&method_mutex);

  clam(CLAM_DEBUG, "method_subscribe",
      "'%s' subscribed to '%s'", name, inst->name);
  return(SUCCESS);
}

bool
method_unsubscribe(method_inst_t *inst, const char *name)
{
  method_sub_t *s, *prev = NULL;

  if(inst == NULL || name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&method_mutex);

  for(s = inst->subs; s != NULL; prev = s, s = s->next)
  {
    if(strncasecmp(s->name, name, METHOD_SUB_NAME_SZ) == 0)
    {
      if(prev != NULL)
        prev->next = s->next;
      else
        inst->subs = s->next;

      inst->sub_count--;
      sub_put(s);
      pthread_mutex_unlock(&method_mutex);

      clam(CLAM_DEBUG, "method_unsubscribe",
          "'%s' unsubscribed from '%s'", name, inst->name);
      return(SUCCESS);
    }
  }

  pthread_mutex_unlock(&method_mutex);
  clam(CLAM_DEBUG, "method_unsubscribe",
      "not found: '%s' on '%s'", name, inst->name);
  return(FAIL);
}

void
method_deliver(method_inst_t *inst, method_msg_t *msg)
{
  method_msg_cb_t  cbs[METHOD_MAX_SUBS];
  void            *datas[METHOD_MAX_SUBS];
  size_t           n = 0;
  size_t           i;

  if(inst == NULL || msg == NULL)
    return;

  // Stamp the originating instance.
  msg->inst = inst;

  if(msg->timestamp == 0)
    msg->timestamp = time(NULL);

  // Snapshot the subscriber callbacks under the lock, then release it
  // before invoking them. A callback may re-enter the method subsystem
  // on the same thread: a delivery handler can emit a clam() event that
  // a command-bot clam subscriber routes back out via method_find /
  // method_send. method_mutex is non-recursive, so holding it across
  // s->cb() self-deadlocks that thread (and, because clam() holds
  // clam_mutex across its own dispatch loop, wedges every other thread
  // that logs). The cap is enforced in method_subscribe, so the list
  // never outruns the snapshot array.
  pthread_mutex_lock(&method_mutex);

  for(method_sub_t *s = inst->subs; s != NULL && n < METHOD_MAX_SUBS;
      s = s->next)
  {
    s->count++;
    cbs[n]   = s->cb;
    datas[n] = s->data;
    n++;
  }

  inst->msg_in++;

  pthread_mutex_unlock(&method_mutex);

  for(i = 0; i < n; i++)
    cbs[i](msg, datas[i]);
}

// Connection management

bool
method_connect(method_inst_t *inst)
{
  if(inst == NULL)
    return(FAIL);

  if(inst->driver->connect == NULL)
    return(SUCCESS);

  return(inst->driver->connect(inst->handle));
}

// Outbound messaging

bool
method_send(method_inst_t *inst, const char *target, const char *text)
{
  char buf[METHOD_TEXT_SZ];
  bool rc;

  if(inst == NULL || target == NULL || text == NULL)
    return(FAIL);

  if(inst->driver->send == NULL)
  {
    clam(CLAM_WARN, "method_send",
        "'%s': driver does not implement send()", inst->name);
    return(FAIL);
  }

  if(inst->state != METHOD_AVAILABLE)
  {
    clam(CLAM_WARN, "method_send",
        "'%s': not available (state: %s)",
        inst->name, method_state_name(inst->state));
    return(FAIL);
  }

  color_translate(buf, sizeof(buf), text, inst->driver->colors);

  rc = inst->driver->send(inst->handle, target, buf);

  if(rc == SUCCESS)
    inst->msg_out++;

  return(rc);
}

// Send an action/emote via a method instance. Drivers with native support
// (e.g., IRC CTCP ACTION) implement send_emote in their vtable. When a
// driver does not provide it, fall back to a plain send() with the text
// wrapped in asterisks so it still reads as an action.
bool
method_send_emote(method_inst_t *inst, const char *target, const char *text)
{
  char buf[METHOD_TEXT_SZ];
  bool rc;

  if(inst == NULL || target == NULL || text == NULL)
    return(FAIL);

  if(inst->state != METHOD_AVAILABLE)
  {
    clam(CLAM_WARN, "method_send_emote",
        "'%s': not available (state: %s)",
        inst->name, method_state_name(inst->state));
    return(FAIL);
  }

  color_translate(buf, sizeof(buf), text, inst->driver->colors);

  if(inst->driver->send_emote != NULL)
    rc = inst->driver->send_emote(inst->handle, target, buf);

  else if(inst->driver->send != NULL)
  {
    // Fallback: "*text*" via plain send.
    char wrapped[METHOD_TEXT_SZ];
    snprintf(wrapped, sizeof(wrapped), "*%s*", buf);
    rc = inst->driver->send(inst->handle, target, wrapped);
  }

  else
  {
    clam(CLAM_WARN, "method_send_emote",
        "'%s': driver has neither send_emote() nor send()", inst->name);
    return(FAIL);
  }

  if(rc == SUCCESS)
    inst->msg_out++;

  return(rc);
}

// Get capability bitmask for an instance's driver.
method_cap_t
method_inst_caps(const method_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL)
    return(0);
  return(inst->driver->caps);
}

// Authentication context

bool
method_get_context(method_inst_t *inst, const char *sender,
    char *ctx, size_t ctx_sz)
{
  if(inst == NULL || sender == NULL || ctx == NULL || ctx_sz == 0)
    return(FAIL);

  if(inst->driver->get_context == NULL)
    return(FAIL);

  return(inst->driver->get_context(inst->handle, sender, ctx, ctx_sz));
}

// Type registry iteration

void
method_iterate_types(method_type_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  for(size_t i = 0; i < METHOD_TYPE_COUNT; i++)
    cb(method_type_table[i].name, method_type_table[i].bit,
        method_type_table[i].desc, data);
}

// Instance iteration

// Per-instance snapshot row for iterate_instances. Lets the iteration
// invoke callbacks with method_mutex released — a callback may emit
// (cmd_reply -> method_send -> clam() -> clam_cmd_shared_cb ->
// method_find), which re-locks method_mutex on the same thread; calling
// the callback under the lock self-deadlocks. Mirrors the snapshot
// pattern already used by method_iterate_drivers.
typedef struct
{
  char           name[METHOD_NAME_SZ];
  char           kind[METHOD_NAME_SZ];
  method_state_t state;
  uint64_t       msg_in;
  uint64_t       msg_out;
  uint32_t       sub_count;
  time_t         connected_at;
} method_inst_snap_t;

void
method_iterate_instances(method_inst_iter_cb_t cb, void *data)
{
  // Cap is generous — method instances are inherently few (one per
  // protocol driver). Excess is dropped silently, matching
  // method_iterate_drivers' MAX_DRIVER_KINDS behaviour.
  #define MAX_ITER_INSTANCES 64

  method_inst_snap_t snap[MAX_ITER_INSTANCES];
  uint32_t           count = 0;
  uint32_t           i;

  if(cb == NULL)
    return;

  pthread_mutex_lock(&method_mutex);

  for(method_inst_t *m = method_list;
      m != NULL && count < MAX_ITER_INSTANCES; m = m->next)
  {
    method_inst_snap_t *s = &snap[count];
    const char         *kind = (m->driver != NULL && m->driver->name != NULL)
        ? m->driver->name : "(unknown)";

    snprintf(s->name, sizeof(s->name), "%s", m->name);
    snprintf(s->kind, sizeof(s->kind), "%s", kind);
    s->state        = m->state;
    s->msg_in       = m->msg_in;
    s->msg_out      = m->msg_out;
    s->sub_count    = m->sub_count;
    s->connected_at = m->connected_at;
    count++;
  }

  pthread_mutex_unlock(&method_mutex);

  for(i = 0; i < count; i++)
    cb(snap[i].name, snap[i].kind, snap[i].state, snap[i].msg_in,
        snap[i].msg_out, snap[i].sub_count, snap[i].connected_at, data);

  #undef MAX_ITER_INSTANCES
}

// Statistics

void
method_iterate_drivers(method_driver_iter_cb_t cb, void *data)
{
  #define MAX_DRIVER_KINDS 16
  char     kinds[MAX_DRIVER_KINDS][METHOD_NAME_SZ];
  uint32_t count = 0;

  if(cb == NULL)
    return;

  // Collect unique driver names under the lock.
  pthread_mutex_lock(&method_mutex);

  for(method_inst_t *m = method_list; m != NULL; m = m->next)
  {
    bool dup = false;

    if(m->driver == NULL || m->driver->name == NULL)
      continue;

    // Check for duplicate.
    for(uint32_t i = 0; i < count; i++)
    {
      if(strcmp(kinds[i], m->driver->name) == 0)
      {
        dup = true;
        break;
      }
    }

    if(!dup && count < MAX_DRIVER_KINDS)
    {
      strncpy(kinds[count], m->driver->name, METHOD_NAME_SZ - 1);
      kinds[count][METHOD_NAME_SZ - 1] = '\0';
      count++;
    }
  }

  pthread_mutex_unlock(&method_mutex);

  // Invoke callbacks outside the lock.
  for(uint32_t i = 0; i < count; i++)
    cb(kinds[i], data);

  #undef MAX_DRIVER_KINDS
}

void
method_audit_iterate(method_audit_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&method_mutex);

  for(method_inst_t *m = method_list; m != NULL; m = m->next)
    cb(m->name, "driver", m->driver, data);

  pthread_mutex_unlock(&method_mutex);
}

void
method_get_stats(method_stats_t *out)
{
  if(out == NULL)
    return;

  pthread_mutex_lock(&method_mutex);

  out->instances     = method_count;
  out->subscribers   = 0;
  out->total_msg_in  = 0;
  out->total_msg_out = 0;

  for(method_inst_t *m = method_list; m != NULL; m = m->next)
  {
    out->subscribers   += m->sub_count;
    out->total_msg_in  += m->msg_in;
    out->total_msg_out += m->msg_out;
  }

  pthread_mutex_unlock(&method_mutex);
}

// Lifecycle

// Initialize the method subsystem.
// Sets up the mutex and marks the subsystem ready.
void
method_init(void)
{
  pthread_mutex_init(&method_mutex, NULL);
  method_ready = true;

  clam(CLAM_INFO, "method_init", "method subsystem initialized");
}

// Shut down the method subsystem.
// Unregisters all instances, frees the subscriber freelist,
// and destroys the mutex.
void
method_exit(void)
{
  if(!method_ready)
    return;

  clam(CLAM_INFO, "method_exit",
      "shutting down (%u instances, %u freelisted subs)",
      method_count, method_sub_free_count);

  method_ready = false;

  // Unregister all instances. Walk the list carefully since
  // method_unregister modifies it, so we always remove the head.
  while(method_list != NULL)
  {
    char name[METHOD_NAME_SZ];

    strncpy(name, method_list->name, METHOD_NAME_SZ - 1);
    name[METHOD_NAME_SZ - 1] = '\0';
    method_unregister(name);
  }

  // Free the subscriber freelist.
  {
    method_sub_t *s = method_sub_freelist;

    while(s != NULL)
    {
      method_sub_t *next = s->next;
      mem_free(s);
      s = next;
    }
  }

  method_sub_freelist = NULL;
  method_sub_free_count = 0;

  pthread_mutex_destroy(&method_mutex);
}
