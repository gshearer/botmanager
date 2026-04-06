#ifndef BM_METHOD_H
#define BM_METHOD_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "colors.h"

#define METHOD_NAME_SZ     64
#define METHOD_SENDER_SZ   128
#define METHOD_CHANNEL_SZ  128
#define METHOD_TEXT_SZ     2048
#define METHOD_META_SZ     512

// Method instance states visible to the rest of the system.
typedef enum
{
  METHOD_ENABLED,     // plugin loaded, instance configured
  METHOD_RUNNING,     // active and processing (e.g., connecting)
  METHOD_AVAILABLE    // connected, can interact with users
} method_state_t;

// Method type bitmask: each driver kind gets one bit.
// Used by cmd_def_t.methods to scope command visibility per method.
typedef uint32_t method_type_t;

#define METHOD_T_CONSOLE   ((method_type_t)1U << 0)
#define METHOD_T_BOTMANCTL ((method_type_t)1U << 1)
#define METHOD_T_IRC       ((method_type_t)1U << 2)
#define METHOD_T_ANY       ((method_type_t)UINT32_MAX)

// Full message context delivered to subscribers.
// Created by method plugins when a message arrives, passed to bot
// callbacks. Contains everything needed for the bot to interpret the
// message and reply on the originating method.
typedef struct method_inst method_inst_t;

typedef struct
{
  method_inst_t *inst;                    // originating method instance
  char           sender[METHOD_SENDER_SZ];    // sender identity (protocol-level)
  char           channel[METHOD_CHANNEL_SZ];  // channel/group (empty for DM)
  char           text[METHOD_TEXT_SZ];        // raw message text
  time_t         timestamp;                   // message timestamp
  char           metadata[METHOD_META_SZ];    // method-specific data
  bool           console_origin;              // true = bypass privilege checks
} method_msg_t;

// Subscriber callback for incoming messages. Invoked synchronously by
// method_deliver() for each matching subscriber.
// msg: the message context (valid for the duration of the callback)
// data: opaque user data registered with method_subscribe()
typedef void (*method_msg_cb_t)(const method_msg_t *msg, void *data);

// Driver interface: functions a method plugin must implement.
// Stored in plugin_desc_t.ext for PLUGIN_METHOD plugins.
typedef struct
{
  const char *name;
  const color_table_t *colors;  // abstract-to-native color mapping

  // Create instance-specific state. Returns opaque handle stored in
  // the method_inst_t. Called during method_register().
  // inst_name: unique instance name
  // returns: opaque handle, or NULL on failure
  void *(*create)(const char *inst_name);

  // Destroy instance-specific state. Called during method_unregister().
  // handle: opaque handle from create()
  void (*destroy)(void *handle);

  // Connect to the platform / start processing.
  // Called when the method should transition toward AVAILABLE.
  // returns: SUCCESS or FAIL
  // handle: opaque handle from create()
  bool (*connect)(void *handle);

  // Disconnect from the platform / stop processing.
  // handle: opaque handle from create()
  void (*disconnect)(void *handle);

  // Send a message to a target (channel or user).
  // returns: SUCCESS or FAIL
  // handle: opaque handle from create()
  // target: destination identifier (channel name, user, etc.)
  // text: message text to send
  bool (*send)(void *handle, const char *target, const char *text);

  // Get method context for authentication (e.g., hostname/IP from IRC).
  // returns: SUCCESS or FAIL (context unavailable)
  // handle: opaque handle from create()
  // sender: protocol-level sender identity
  // ctx: buffer to fill with context string
  // ctx_sz: size of ctx buffer
  bool (*get_context)(void *handle, const char *sender,
      char *ctx, size_t ctx_sz);
} method_driver_t;

// Method subscriber statistics.
typedef struct
{
  uint32_t instances;     // registered method instances
  uint32_t subscribers;   // total subscribers across all instances
  uint64_t total_msg_in;  // sum of all instances' msg_in
  uint64_t total_msg_out; // sum of all instances' msg_out
} method_stats_t;

// -----------------------------------------------------------------------
// Instance management
// -----------------------------------------------------------------------

// Register a new method instance. The driver's create() callback is
// invoked to produce instance-specific state.
// returns: instance pointer, or NULL on failure
// drv: driver interface (must not be NULL)
// name: unique instance name
method_inst_t *method_register(const method_driver_t *drv, const char *name);

// Unregister a method instance. Disconnects if running/available,
// invokes driver destroy(), and removes all subscribers.
// returns: SUCCESS or FAIL (not found)
// name: instance name
bool method_unregister(const char *name);

// Find an instance by name.
// returns: instance pointer, or NULL if not found
// name: instance name
method_inst_t *method_find(const char *name);

// Get the name of an instance.
// returns: instance name string
// inst: method instance
const char *method_inst_name(const method_inst_t *inst);

// Get the driver kind of an instance (e.g., "irc", "console").
// returns: driver name string
// inst: method instance
const char *method_inst_kind(const method_inst_t *inst);

// Map a driver name to its method type bit.
// returns: type bit, or 0 if the driver name is unrecognized
// name: driver name string (e.g., "console", "irc")
method_type_t method_type_bit(const char *name);

// Get the human-friendly description for a method type by driver name.
// returns: description string, or NULL if the driver name is unrecognized
// name: driver name string (e.g., "console", "irc")
const char *method_type_desc(const char *name);

// Get the method type bitmask for a method instance.
// returns: type bit derived from driver name, or 0 if unrecognized
// inst: method instance
method_type_t method_inst_type(const method_inst_t *inst);

// Get the driver-specific handle for an instance.
// returns: opaque handle pointer, or NULL
// inst: method instance
void *method_get_handle(const method_inst_t *inst);

// -----------------------------------------------------------------------
// State management
// -----------------------------------------------------------------------

// Get current state of an instance.
// returns: current method state
// inst: method instance
method_state_t method_get_state(const method_inst_t *inst);

// Set the state of an instance. Typically called by the method plugin
// itself as it progresses through its lifecycle (connecting, connected,
// disconnected).
// inst: method instance
// state: new state
void method_set_state(method_inst_t *inst, method_state_t state);

// returns: human-readable name of a method state
// s: method state enum value
const char *method_state_name(method_state_t s);

// -----------------------------------------------------------------------
// Message delivery
// -----------------------------------------------------------------------

// Subscribe to messages from a method instance.
// returns: SUCCESS or FAIL (duplicate name, instance not found)
// inst: method instance to subscribe to
// name: unique subscriber name
// cb: callback invoked for each message
// data: opaque user data passed to callback
bool method_subscribe(method_inst_t *inst, const char *name,
    method_msg_cb_t cb, void *data);

// Unsubscribe from a method instance.
// returns: SUCCESS or FAIL (not found)
// inst: method instance
// name: subscriber name
bool method_unsubscribe(method_inst_t *inst, const char *name);

// Deliver a message to all subscribers of a method instance.
// Called by method plugins when a message arrives from the platform.
// inst: method instance the message arrived on
// msg: message context (inst field is set automatically)
void method_deliver(method_inst_t *inst, method_msg_t *msg);

// -----------------------------------------------------------------------
// Connection management
// -----------------------------------------------------------------------

// Connect a method instance. Routes through the driver's connect()
// callback. Typically called by bot_start() for on-demand instances.
// returns: SUCCESS or FAIL
// inst: method instance
bool method_connect(method_inst_t *inst);

// -----------------------------------------------------------------------
// Outbound messaging
// -----------------------------------------------------------------------

// Send a message via a method instance. Routes through the driver's
// send() callback.
// returns: SUCCESS or FAIL
// inst: method instance to send on
// target: destination (channel, user, etc.)
// text: message text
bool method_send(method_inst_t *inst, const char *target, const char *text);

// -----------------------------------------------------------------------
// Authentication context
// -----------------------------------------------------------------------

// Get method context for a sender (e.g., hostname/IP). Routes through
// the driver's get_context() callback. Used by the auth system as a
// second factor.
// returns: SUCCESS or FAIL
// inst: method instance
// sender: protocol-level sender identity
// ctx: buffer to fill
// ctx_sz: buffer size
bool method_get_context(method_inst_t *inst, const char *sender,
    char *ctx, size_t ctx_sz);

// -----------------------------------------------------------------------
// Type registry iteration
// -----------------------------------------------------------------------

// Callback for method type table iteration.
// name: driver kind string (e.g., "console", "irc")
// bit: method_type_t bitmask for this type
// desc: human-friendly description
// data: opaque user data
typedef void (*method_type_iter_cb_t)(const char *name, method_type_t bit,
    const char *desc, void *data);

// Iterate the static method type registry.
// cb: callback invoked for each registered type
// data: opaque user data passed to cb
void method_iterate_types(method_type_iter_cb_t cb, void *data);

// -----------------------------------------------------------------------
// Driver iteration
// -----------------------------------------------------------------------

// Callback for driver name iteration.
// kind: driver kind string (e.g., "irc", "console")
// data: opaque user data
typedef void (*method_driver_iter_cb_t)(const char *kind, void *data);

// Iterate unique method driver kinds across all registered instances.
// Each driver kind is yielded at most once.
// cb: callback invoked for each unique driver kind
// data: opaque user data passed to cb
void method_iterate_drivers(method_driver_iter_cb_t cb, void *data);

// -----------------------------------------------------------------------
// Instance iteration
// -----------------------------------------------------------------------

// Callback for method instance iteration.
// name: instance name
// kind: driver kind string
// state: current instance state
// msg_in: total inbound messages delivered
// msg_out: total outbound messages sent
// sub_count: current subscriber count
// connected_at: timestamp when instance became AVAILABLE (0 if not)
// data: opaque user data
typedef void (*method_inst_iter_cb_t)(const char *name, const char *kind,
    method_state_t state, uint64_t msg_in, uint64_t msg_out,
    uint32_t sub_count, time_t connected_at, void *data);

// Iterate all registered method instances. Locks method_mutex for
// the duration of the iteration.
// cb: callback invoked for each instance
// data: opaque user data passed to cb
void method_iterate_instances(method_inst_iter_cb_t cb, void *data);

// -----------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------

// Get method subsystem statistics (thread-safe snapshot).
// out: destination for the snapshot
void method_get_stats(method_stats_t *out);

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

// Initialize the method subsystem.
void method_init(void);

// Shut down the method subsystem. Unregisters all instances.
void method_exit(void);

#ifdef METHOD_INTERNAL

#include "common.h"
#include "clam.h"
#include "mem.h"

#define METHOD_SUB_NAME_SZ  64
#define METHOD_MAX_SUBS     32

// Per-instance message subscriber.
typedef struct method_sub
{
  char               name[METHOD_SUB_NAME_SZ];
  method_msg_cb_t    cb;
  void              *data;
  uint64_t           count;   // messages delivered
  struct method_sub *next;
} method_sub_t;

// Method instance: registered by a method plugin, holds driver state
// and subscriber chain.
struct method_inst
{
  char                    name[METHOD_NAME_SZ];
  const method_driver_t  *driver;
  void                   *handle;    // driver-specific state
  method_state_t          state;
  method_sub_t           *subs;      // subscriber list
  uint32_t                sub_count;
  uint64_t                msg_in;    // total messages delivered
  uint64_t                msg_out;   // total messages sent
  time_t                  connected_at; // timestamp of METHOD_AVAILABLE, 0 if not
  struct method_inst     *next;
};

// Module state.
static method_inst_t   *method_list  = NULL;
static pthread_mutex_t  method_mutex;
static uint32_t         method_count = 0;
static bool             method_ready = false;

// Subscriber freelist for reuse.
static method_sub_t    *method_sub_freelist = NULL;
static uint32_t         method_sub_free_count = 0;

#endif // METHOD_INTERNAL

#endif // BM_METHOD_H
