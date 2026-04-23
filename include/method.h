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

typedef enum
{
  METHOD_ENABLED,     // plugin loaded, instance configured
  METHOD_RUNNING,     // active and processing (e.g., connecting)
  METHOD_AVAILABLE    // connected, can interact with users
} method_state_t;

// Method type bitmask: each driver kind gets one bit.
// Used by cmd_def_t.methods to scope command visibility per method.
typedef uint32_t method_type_t;

#define METHOD_T_BOTMANCTL ((method_type_t)1U << 1)
#define METHOD_T_IRC       ((method_type_t)1U << 2)
#define METHOD_T_ANY       ((method_type_t)UINT32_MAX)

// Method capability bitmask: declares optional features a driver supports,
// so bots can tailor their behavior per method. Drivers set these in the
// driver vtable's .caps field.
typedef uint32_t method_cap_t;

#define METHOD_CAP_EMOTE   ((method_cap_t)1U << 0)  // third-person action (IRC CTCP ACTION, Discord italics, etc.)

// Discriminator for the kind of event a method_msg_t carries. The
// default (zero) is a normal chat/DM line. Other kinds describe
// protocol-level identity events that bots may want to observe but do
// not inject into chat history (e.g., IRC NICK changes that collapse
// into the same per-identity record at confidence 1.0).
typedef enum
{
  METHOD_MSG_MESSAGE     = 0,  // normal chat line (PRIVMSG, DM, etc.)
  METHOD_MSG_NICK_CHANGE = 1,  // sender renamed; see method_msg_t notes
} method_msg_kind_t;

typedef struct method_inst method_inst_t;

// Full message context delivered to subscribers. Created by method
// plugins when a message arrives, passed to bot callbacks. Contains
// everything needed for the bot to interpret the message and reply on
// the originating method.
typedef struct
{
  method_inst_t *inst;                    // originating method instance
  char           sender[METHOD_SENDER_SZ];    // sender identity (protocol-level)
  char           channel[METHOD_CHANNEL_SZ];  // channel/group (empty for DM)
  char           text[METHOD_TEXT_SZ];        // raw message text
  time_t         timestamp;                   // message timestamp
  char           metadata[METHOD_META_SZ];    // method-specific data
  bool           is_action;                   // true when the message is an emote/action (e.g., IRC CTCP ACTION)
  method_msg_kind_t kind;                     // default METHOD_MSG_MESSAGE
  // Previous protocol-level identity string, set on METHOD_MSG_NICK_CHANGE.
  // For IRC: the "oldnick!ident@host" prefix. metadata holds the new
  // identity ("newnick!ident@host"), sender is the old nick, text is
  // the new nick, and channel is empty. Unused for other kinds.
  char           prev_metadata[METHOD_META_SZ];
} method_msg_t;

// Invoked synchronously by method_deliver() for each matching
// subscriber. msg is valid for the duration of the callback.
typedef void (*method_msg_cb_t)(const method_msg_t *msg, void *data);

typedef void (*method_chan_member_cb_t)(const char *nick, void *data);

typedef void (*method_joined_channel_cb_t)(const char *channel, void *data);

// Functions a method plugin must implement. Stored in plugin_desc_t.ext
// for PLUGIN_METHOD plugins.
typedef struct
{
  const char *name;
  method_cap_t caps;            // optional capability bitmask (METHOD_CAP_*)
  const color_table_t *colors;  // abstract-to-native color mapping

  void *(*create)(const char *inst_name);
  void (*destroy)(void *handle);

  // Called when the method should transition toward AVAILABLE.
  bool (*connect)(void *handle);
  void (*disconnect)(void *handle);

  bool (*send)(void *handle, const char *target, const char *text);

  // Send a third-person action/emote. Optional — if NULL,
  // method_send_emote falls back to a plain send with the text wrapped
  // in asterisks. Drivers that implement this should also advertise
  // METHOD_CAP_EMOTE in the caps field above. `text` is already
  // stripped of any leading "/me ".
  bool (*send_emote)(void *handle, const char *target, const char *text);

  // Method context for authentication (e.g., hostname/IP from IRC).
  bool (*get_context)(void *handle, const char *sender,
      char *ctx, size_t ctx_sz);

  // List members of a channel. If not implemented (NULL), the channel
  // has no member tracking.
  void (*list_channel)(void *handle, const char *channel,
      method_chan_member_cb_t cb, void *data);

  // List the channels this bot is currently joined to. If not
  // implemented (NULL), the driver has no notion of multi-channel
  // membership. Implementations must guarantee that `cb` is invoked
  // synchronously and that the callback returns before the next
  // invocation — the caller may hold no locks across cb.
  void (*list_joined_channels)(void *handle,
      method_joined_channel_cb_t cb, void *data);

  // Get the bot's own identity on this method (e.g., current IRC nick).
  bool (*get_self)(void *handle, char *buf, size_t buf_sz);
} method_driver_t;

typedef struct
{
  uint32_t instances;     // registered method instances
  uint32_t subscribers;   // total subscribers across all instances
  uint64_t total_msg_in;  // sum of all instances' msg_in
  uint64_t total_msg_out; // sum of all instances' msg_out
} method_stats_t;

method_inst_t *method_register(const method_driver_t *drv, const char *name);

// Disconnects if running/available, invokes driver destroy(), removes
// all subscribers.
bool method_unregister(const char *name);

method_inst_t *method_find(const char *name);
const char *method_inst_name(const method_inst_t *inst);

// Driver kind of an instance (e.g., "irc", "botmanctl").
const char *method_inst_kind(const method_inst_t *inst);

// Returns 0 if the driver name is unrecognized.
method_type_t method_type_bit(const char *name);

// Returns NULL if the driver name is unrecognized.
const char *method_type_desc(const char *name);

// Returns 0 if unrecognized.
method_type_t method_inst_type(const method_inst_t *inst);

void *method_get_handle(const method_inst_t *inst);

// Does nothing if the driver does not support member listing.
void method_list_channel(method_inst_t *inst, const char *channel,
    method_chan_member_cb_t cb, void *data);

// Does nothing if the driver does not support joined-channel listing.
void method_list_joined_channels(method_inst_t *inst,
    method_joined_channel_cb_t cb, void *data);

bool method_get_self(method_inst_t *inst, char *buf, size_t buf_sz);

method_state_t method_get_state(const method_inst_t *inst);

// Typically called by the method plugin itself as it progresses
// through its lifecycle (connecting, connected, disconnected).
void method_set_state(method_inst_t *inst, method_state_t state);

const char *method_state_name(method_state_t s);

bool method_subscribe(method_inst_t *inst, const char *name,
    method_msg_cb_t cb, void *data);

bool method_unsubscribe(method_inst_t *inst, const char *name);

// Called by method plugins when a message arrives from the platform.
// msg->inst is set automatically.
void method_deliver(method_inst_t *inst, method_msg_t *msg);

// Routes through the driver's connect() callback. Typically called by
// bot_start() for on-demand instances.
bool method_connect(method_inst_t *inst);

bool method_send(method_inst_t *inst, const char *target, const char *text);

// Routes through the driver's send_emote() callback when available;
// otherwise falls back to a plain send with the text wrapped in
// asterisks so it still reads as an action on methods without native
// support. `text` must not include any leading "/me " prefix.
bool method_send_emote(method_inst_t *inst, const char *target,
    const char *text);

// METHOD_CAP_* bits, or 0 if inst/driver is NULL.
method_cap_t method_inst_caps(const method_inst_t *inst);

// Method context for a sender (e.g., hostname/IP). Used by the auth
// system as a second factor.
bool method_get_context(method_inst_t *inst, const char *sender,
    char *ctx, size_t ctx_sz);

typedef void (*method_type_iter_cb_t)(const char *name, method_type_t bit,
    const char *desc, void *data);

void method_iterate_types(method_type_iter_cb_t cb, void *data);

typedef void (*method_driver_iter_cb_t)(const char *kind, void *data);

// Each driver kind is yielded at most once.
void method_iterate_drivers(method_driver_iter_cb_t cb, void *data);

typedef void (*method_inst_iter_cb_t)(const char *name, const char *kind,
    method_state_t state, uint64_t msg_in, uint64_t msg_out,
    uint32_t sub_count, time_t connected_at, void *data);

// Locks method_mutex for the duration of the iteration.
void method_iterate_instances(method_inst_iter_cb_t cb, void *data);

void method_get_stats(method_stats_t *out);

void method_init(void);

// Unregisters all instances.
void method_exit(void);

#ifdef METHOD_INTERNAL

#include "common.h"
#include "clam.h"
#include "alloc.h"

#define METHOD_SUB_NAME_SZ  64
#define METHOD_MAX_SUBS     32

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

static method_inst_t   *method_list  = NULL;
static pthread_mutex_t  method_mutex;
static uint32_t         method_count = 0;
static bool             method_ready = false;

static method_sub_t    *method_sub_freelist = NULL;
static uint32_t         method_sub_free_count = 0;

#endif // METHOD_INTERNAL

#endif // BM_METHOD_H
