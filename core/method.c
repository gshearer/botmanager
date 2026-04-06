#define METHOD_INTERNAL
#include "method.h"

// -----------------------------------------------------------------------
// Method type registry — maps driver names to bitmasks and descriptions.
// -----------------------------------------------------------------------

typedef struct
{
  const char   *name;
  method_type_t bit;
  const char   *desc;
} method_type_entry_t;

static const method_type_entry_t method_type_table[] = {
  { "console",   METHOD_T_CONSOLE,   "Interactive console for operator control" },
  { "botmanctl", METHOD_T_BOTMANCTL, "Unix socket interface for scripted control" },
  { "irc",       METHOD_T_IRC,       "Internet Relay Chat protocol" },
};

#define METHOD_TYPE_COUNT \
    (sizeof(method_type_table) / sizeof(method_type_table[0]))

// -----------------------------------------------------------------------
// Subscriber freelist management.
// -----------------------------------------------------------------------

// Get a subscriber struct from the freelist or allocate a new one.
// returns: zeroed subscriber struct
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

// Return a subscriber struct to the freelist.
// s: subscriber to recycle
static void
sub_put(method_sub_t *s)
{
  s->next = method_sub_freelist;
  method_sub_freelist = s;
  method_sub_free_count++;
}

// -----------------------------------------------------------------------
// Instance management
// -----------------------------------------------------------------------

// Register a new method instance.
// drv: driver interface (must not be NULL)
// name: unique instance name
// returns: instance pointer, or NULL on failure
method_inst_t *
method_register(const method_driver_t *drv, const char *name)
{
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

  method_inst_t *inst = mem_alloc("method", "instance", sizeof(*inst));
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

// Unregister a method instance.
// name: instance name
// returns: SUCCESS or FAIL
bool
method_unregister(const char *name)
{
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&method_mutex);

  method_inst_t *inst = NULL;
  method_inst_t *prev = NULL;

  for(inst = method_list; inst != NULL; prev = inst, inst = inst->next)
  {
    if(strncasecmp(inst->name, name, METHOD_NAME_SZ) == 0)
      break;
  }

  if(inst == NULL)
  {
    pthread_mutex_unlock(&method_mutex);
    clam(CLAM_WARN, "method_unregister", "not found: '%s'", name);
    return(FAIL);
  }

  // Disconnect if running or available.
  if(inst->state == METHOD_RUNNING || inst->state == METHOD_AVAILABLE)
  {
    if(inst->driver->disconnect != NULL)
      inst->driver->disconnect(inst->handle);
  }

  // Remove all subscribers.
  method_sub_t *s = inst->subs;

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

// Find an instance by name.
// name: instance name
// returns: instance pointer, or NULL
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

// Get the name of an instance.
// inst: method instance
// returns: instance name string
const char *
method_inst_name(const method_inst_t *inst)
{
  if(inst == NULL)
    return("(null)");

  return(inst->name);
}

// Get the driver kind of an instance (e.g., "irc", "console").
// inst: method instance
// returns: driver name string
const char *
method_inst_kind(const method_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL)
    return("(null)");

  return(inst->driver->name);
}

// Map a driver name to its method type bit.
// returns: type bit, or 0 if the driver name is unrecognized
// name: driver name string (e.g., "console", "irc")
method_type_t
method_type_bit(const char *name)
{
  if(name == NULL)
    return(0);

  for(size_t i = 0; i < METHOD_TYPE_COUNT; i++)
  {
    if(strcmp(name, method_type_table[i].name) == 0)
      return(method_type_table[i].bit);
  }

  return(0);
}

// Get the human-friendly description for a method type by driver name.
// returns: description string, or NULL if the driver name is unrecognized
// name: driver name string (e.g., "console", "irc")
const char *
method_type_desc(const char *name)
{
  if(name == NULL)
    return(NULL);

  for(size_t i = 0; i < METHOD_TYPE_COUNT; i++)
  {
    if(strcmp(name, method_type_table[i].name) == 0)
      return(method_type_table[i].desc);
  }

  return(NULL);
}

// Get the method type bitmask for a method instance.
// returns: type bit derived from driver name, or 0 if unrecognized
// inst: method instance
method_type_t
method_inst_type(const method_inst_t *inst)
{
  if(inst == NULL || inst->driver == NULL)
    return(0);

  return(method_type_bit(inst->driver->name));
}

// Get the driver-specific handle for an instance.
// returns: opaque handle pointer, or NULL
// inst: method instance
void *
method_get_handle(const method_inst_t *inst)
{
  if(inst == NULL)
    return(NULL);

  return(inst->handle);
}

// -----------------------------------------------------------------------
// State management
// -----------------------------------------------------------------------

// Get current state of an instance.
// inst: method instance
// returns: current state
method_state_t
method_get_state(const method_inst_t *inst)
{
  if(inst == NULL)
    return(METHOD_ENABLED);

  return(inst->state);
}

// Set the state of an instance.
// inst: method instance
// state: new state
void
method_set_state(method_inst_t *inst, method_state_t state)
{
  if(inst == NULL)
    return;

  method_state_t old = inst->state;
  inst->state = state;

  // Track connection timestamp for uptime reporting.
  if(state == METHOD_AVAILABLE && old != METHOD_AVAILABLE)
    inst->connected_at = time(NULL);

  else if(state != METHOD_AVAILABLE)
    inst->connected_at = 0;

  clam(CLAM_DEBUG, "method_set_state", "'%s': %s -> %s",
      inst->name, method_state_name(old), method_state_name(state));
}

// returns: human-readable name of a method state
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

// -----------------------------------------------------------------------
// Message delivery
// -----------------------------------------------------------------------

// Subscribe to messages from a method instance.
// inst: method instance
// name: unique subscriber name
// cb: callback invoked for each message
// data: opaque user data passed to callback
// returns: SUCCESS or FAIL
bool
method_subscribe(method_inst_t *inst, const char *name,
    method_msg_cb_t cb, void *data)
{
  if(inst == NULL || name == NULL || name[0] == '\0' || cb == NULL)
  {
    clam(CLAM_WARN, "method_subscribe", "invalid arguments");
    return(FAIL);
  }

  pthread_mutex_lock(&method_mutex);

  // Check for duplicate subscriber name on this instance.
  for(method_sub_t *s = inst->subs; s != NULL; s = s->next)
  {
    if(strncasecmp(s->name, name, METHOD_SUB_NAME_SZ) == 0)
    {
      pthread_mutex_unlock(&method_mutex);
      clam(CLAM_WARN, "method_subscribe",
          "duplicate subscriber '%s' on '%s'", name, inst->name);
      return(FAIL);
    }
  }

  method_sub_t *s = sub_get();
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

// Unsubscribe from a method instance.
// inst: method instance
// name: subscriber name
// returns: SUCCESS or FAIL
bool
method_unsubscribe(method_inst_t *inst, const char *name)
{
  if(inst == NULL || name == NULL || name[0] == '\0')
    return(FAIL);

  pthread_mutex_lock(&method_mutex);

  method_sub_t *s, *prev = NULL;

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

// Deliver a message to all subscribers of a method instance.
// inst: method instance the message arrived on
// msg: message context (inst field is set automatically)
void
method_deliver(method_inst_t *inst, method_msg_t *msg)
{
  if(inst == NULL || msg == NULL)
    return;

  // Stamp the originating instance.
  msg->inst = inst;

  if(msg->timestamp == 0)
    msg->timestamp = time(NULL);

  pthread_mutex_lock(&method_mutex);

  for(method_sub_t *s = inst->subs; s != NULL; s = s->next)
  {
    s->count++;
    s->cb(msg, s->data);
  }

  inst->msg_in++;

  pthread_mutex_unlock(&method_mutex);
}

// -----------------------------------------------------------------------
// Connection management
// -----------------------------------------------------------------------

// Connect a method instance.
// inst: method instance
// returns: SUCCESS or FAIL
bool
method_connect(method_inst_t *inst)
{
  if(inst == NULL)
    return(FAIL);

  if(inst->driver->connect == NULL)
    return(SUCCESS);

  return(inst->driver->connect(inst->handle));
}

// -----------------------------------------------------------------------
// Outbound messaging
// -----------------------------------------------------------------------

// Send a message via a method instance.
// inst: method instance
// target: destination (channel, user, etc.)
// text: message text
// returns: SUCCESS or FAIL
bool
method_send(method_inst_t *inst, const char *target, const char *text)
{
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

  char buf[METHOD_TEXT_SZ];

  color_translate(buf, sizeof(buf), text, inst->driver->colors);

  bool rc = inst->driver->send(inst->handle, target, buf);

  if(rc == SUCCESS)
    inst->msg_out++;

  return(rc);
}

// -----------------------------------------------------------------------
// Authentication context
// -----------------------------------------------------------------------

// Get method context for a sender.
// inst: method instance
// sender: protocol-level sender identity
// ctx: buffer to fill
// ctx_sz: buffer size
// returns: SUCCESS or FAIL
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

// -----------------------------------------------------------------------
// Type registry iteration
// -----------------------------------------------------------------------

// Iterate the static method type registry. Calls cb for each known
// method type with its name, bitmask, and description.
// cb: callback invoked for each registered type
// data: opaque user data passed to cb
void
method_iterate_types(method_type_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  for(size_t i = 0; i < METHOD_TYPE_COUNT; i++)
  {
    cb(method_type_table[i].name, method_type_table[i].bit,
        method_type_table[i].desc, data);
  }
}

// -----------------------------------------------------------------------
// Instance iteration
// -----------------------------------------------------------------------

// Iterate all registered method instances. Locks method_mutex and
// calls cb for each instance with a snapshot of its state.
// cb: callback invoked for each instance
// data: opaque user data passed to cb
void
method_iterate_instances(method_inst_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  pthread_mutex_lock(&method_mutex);

  for(method_inst_t *m = method_list; m != NULL; m = m->next)
  {
    const char *kind = (m->driver != NULL && m->driver->name != NULL)
        ? m->driver->name : "(unknown)";

    cb(m->name, kind, m->state, m->msg_in, m->msg_out,
        m->sub_count, m->connected_at, data);
  }

  pthread_mutex_unlock(&method_mutex);
}

// -----------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------

// Iterate unique method driver kinds across all registered instances.
// Each driver kind is yielded at most once. Collects up to 16 unique
// driver names from the instance list.
// cb: callback invoked for each unique driver kind
// data: opaque user data passed to cb
void
method_iterate_drivers(method_driver_iter_cb_t cb, void *data)
{
  if(cb == NULL)
    return;

  // Collect unique driver names under the lock.
  #define MAX_DRIVER_KINDS 16
  char kinds[MAX_DRIVER_KINDS][METHOD_NAME_SZ];
  uint32_t count = 0;

  pthread_mutex_lock(&method_mutex);

  for(method_inst_t *m = method_list; m != NULL; m = m->next)
  {
    if(m->driver == NULL || m->driver->name == NULL)
      continue;

    // Check for duplicate.
    bool dup = false;

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

// Get method subsystem statistics.
// out: destination for the snapshot
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

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

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
  method_sub_t *s = method_sub_freelist;

  while(s != NULL)
  {
    method_sub_t *next = s->next;
    mem_free(s);
    s = next;
  }

  method_sub_freelist = NULL;
  method_sub_free_count = 0;

  pthread_mutex_destroy(&method_mutex);
}
