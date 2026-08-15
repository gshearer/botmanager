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

// Quad-tuple identity field bounds. Each method driver fills whatever it
// has, leaves the rest empty. The chat plugin's identity scorer treats
// nickname/username/hostname as a similarity tuple and verified_id as
// an authoritative short-circuit (server-attested, unforgeable handle:
// IRC SASL account, Slack user_id, Matrix MXID, etc.).
#define METHOD_NICKNAME_SZ    64
#define METHOD_USERNAME_SZ    64
#define METHOD_HOSTNAME_SZ    128
#define METHOD_VERIFIED_ID_SZ 128

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
#define METHOD_T_REACHY    ((method_type_t)1U << 3)
#define METHOD_T_ANY       ((method_type_t)UINT32_MAX)

// Method capability bitmask: declares optional features a driver supports,
// so bots can tailor their behavior per method. Drivers set these in the
// driver vtable's .caps field.
typedef uint32_t method_cap_t;

#define METHOD_CAP_EMOTE   ((method_cap_t)1U << 0)  // third-person action (IRC CTCP ACTION, Discord italics, etc.)
#define METHOD_CAP_EJECT   ((method_cap_t)1U << 1)  // can remove a participant from a room
#define METHOD_CAP_SPOKEN  ((method_cap_t)1U << 2)  // replies are read aloud (TTS); fixed-width or columnar output is unusable, so data-bearing command output must be rephrased as prose

// Discriminator for the kind of event a method_msg_t carries. The
// default (zero) is a normal chat/DM line. Other kinds describe
// method-level identity events that bots may want to observe but do
// not inject into chat history (e.g., IRC NICK changes that collapse
// into the same per-identity record at confidence 1.0).
typedef enum
{
  METHOD_MSG_MESSAGE     = 0,  // normal chat line (PRIVMSG, DM, etc.)
  METHOD_MSG_NICK_CHANGE = 1,  // sender renamed; see method_msg_t notes
} method_msg_kind_t;

// Whether the METHOD knows this line was addressed to the bot.
//
// Addressing is not a property of text — that is only how IRC happens to
// express it. A microphone knows by a spoken name, a wake phrase or a
// button; a workspace method knows by a user id it was handed. The layer
// that knows says so here, and the bot believes it rather than trying to
// re-derive the answer from English.
//
// UNKNOWN is the honest default and the zero value, so a method that has
// no such knowledge says nothing by saying nothing: the bot falls back to
// its own text classifier, exactly as it always has.
typedef enum
{
  METHOD_ADDR_UNKNOWN = 0,  // no opinion — the bot classifies the text itself
  METHOD_ADDR_DIRECT  = 1,  // the method knows this was said TO the bot
  METHOD_ADDR_AMBIENT = 2,  // the method knows it was not — overheard, not addressed
} method_addressing_t;

typedef struct method_inst method_inst_t;

// Full message context delivered to subscribers. Created by method
// plugins when a message arrives, passed to bot callbacks. Contains
// everything needed for the bot to interpret the message and reply on
// the originating method.
typedef struct
{
  method_inst_t *inst;                    // originating method instance
  char           sender[METHOD_SENDER_SZ];    // sender identity (method-level)
  char           channel[METHOD_CHANNEL_SZ];  // channel/group (empty for DM)
  char           text[METHOD_TEXT_SZ];        // raw message text
  time_t         timestamp;                   // message timestamp
  char           metadata[METHOD_META_SZ];    // method-specific data (diagnostic / auth context)
  bool           is_action;                   // true when the message is an emote/action (e.g., IRC CTCP ACTION)

  // What the method knows about who this line was aimed at. Left
  // UNKNOWN by any method that cannot tell (see method_addressing_t);
  // an AMBIENT line is one the bot may hear but was never said to.
  method_addressing_t addressing;

  method_msg_kind_t kind;                     // default METHOD_MSG_MESSAGE

  // Reply-sink divert (see cmd.h §Reply sinks). Non-zero routes every
  // cmd_reply() of a command dispatched from this message to the
  // registered collector instead of the wire. Consumed by cmd_reply()
  // only — method drivers neither read nor set it. An id rather than a
  // pointer: async commands deep-copy the whole message by value, so
  // the id survives every hop for free, and a stale id after the owner
  // unregistered falls through to normal delivery.
  uint64_t reply_sink_id;

  // Generic method-level identity for the sender. The method plugin
  // populates whatever it has; consumers (chat, audit, etc.) treat the
  // tuple uniformly. nickname is the user's current display label;
  // username is their client-claimed login; hostname is the origin /
  // homeserver / workspace identifier; verified_id is the server-
  // attested unforgeable handle (IRC SASL account, Slack user_id,
  // Matrix MXID, ...) and is empty when the method cannot attest.
  char nickname    [METHOD_NICKNAME_SZ];
  char username    [METHOD_USERNAME_SZ];
  char hostname    [METHOD_HOSTNAME_SZ];
  char verified_id [METHOD_VERIFIED_ID_SZ];

  // Previous method-level identity, set on METHOD_MSG_NICK_CHANGE.
  // The new identity lives in the fields above and metadata holds the
  // new raw prefix; sender is the old display label, text is the new
  // display label, channel is empty. Unused for other kinds.
  char prev_metadata    [METHOD_META_SZ];
  char prev_nickname    [METHOD_NICKNAME_SZ];
  char prev_username    [METHOD_USERNAME_SZ];
  char prev_hostname    [METHOD_HOSTNAME_SZ];
  char prev_verified_id [METHOD_VERIFIED_ID_SZ];
} method_msg_t;

// Invoked synchronously by method_deliver() for each matching
// subscriber. msg is valid for the duration of the callback.
typedef void (*method_msg_cb_t)(const method_msg_t *msg, void *data);

typedef void (*method_chan_member_cb_t)(const char *nick, void *data);

typedef void (*method_joined_channel_cb_t)(const char *channel, void *data);

// How forcefully a driver can remove a participant. Ordered: a numerically
// greater value is a strictly harsher removal, so callers may compare.
typedef enum
{
  METHOD_EJECT_NONE   = 0,  // no removal possible right now
  METHOD_EJECT_ROOM   = 1,  // out of this room only (IRC KICK)
  METHOD_EJECT_SERVER = 2,  // off the platform entirely (IRC KILL)
} method_eject_t;

// Functions a method plugin must implement — how a bot meets humans.
// Stored in plugin_desc_t.ext for PLUGIN_METHOD plugins (IRC, voice,
// Slack, etc.).
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
  // Implementations must answer from live state, never from a cache
  // with a lifetime of its own — see method_get_context() below for why
  // a stale answer here authenticates the wrong person. Answering FAIL
  // is always safe; guessing is not.
  bool (*get_context)(void *handle, const char *sender,
      char *ctx, size_t ctx_sz);

  // List members of a channel. If not implemented (NULL), the channel
  // has no member tracking. `cb` may call back into this driver — asking
  // about a member is the usual reason to enumerate them — so snapshot
  // the roster and invoke `cb` with no driver lock held.
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

  // Strongest removal this driver could apply to `target` in `channel`
  // right now. A pure read-only probe: it emits no wire traffic and
  // changes no state. METHOD_EJECT_NONE when the driver has no notion of
  // removal, the bot lacks the privilege, or the target is not present.
  // Optional — NULL means the driver can never eject.
  method_eject_t (*eject_probe)(void *handle, const char *channel,
      const char *target);

  // Remove `target` from `channel`. `force` must not exceed what
  // eject_probe just reported; the driver clamps if it does. `reason` is
  // short, human-readable, and may be shown to other participants.
  // Optional — NULL means the driver can never eject.
  bool (*eject)(void *handle, const char *channel, const char *target,
      method_eject_t force, const char *reason);
} method_driver_t;

typedef struct
{
  uint32_t instances;     // registered method instances
  uint32_t subscribers;   // total subscribers across all instances
  uint64_t total_msg_in;  // sum of all instances' msg_in
  uint64_t total_msg_out; // sum of all instances' msg_out
} method_stats_t;

// Reference counting. Every function that hands back a method_inst_t *
// hands back a reference along with it — method_register(),
// method_find(), and bot.h's bot_first_method() / bot_resolve_method()
// — and the caller owes each one a method_release().
//
// The instance outlives method_unregister() for exactly as long as
// somebody still holds it. It cannot simply wait for them: a delivery
// runs the whole bot turn (method_deliver -> bot_msg_handler -> the
// chat plugin, seconds of it), and method_unregister runs from
// bot_stop with method_mutex to take. So the registry hands its own
// reference back at unregister, the instance is refused from that
// moment on (no handle, not AVAILABLE), and the last holder to leave
// frees it.
//
// method_hold() is a SECOND reference, never the first: the caller must
// already hold one, or otherwise guarantee the instance is live.
void method_hold(method_inst_t *inst);
void method_release(method_inst_t *inst);

// Returns a NEW reference; release it with method_release().
method_inst_t *method_register(const method_driver_t *drv, const char *name);

// Disconnects if running/available, invokes driver destroy(), removes
// all subscribers. Gives back the registry's own reference — a holder
// keeps the instance alive past this call.
bool method_unregister(const char *name);

// Returns a NEW reference, or NULL if no instance of that name is
// registered. Release it with method_release().
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

// METHOD_EJECT_NONE if the driver does not implement ejection.
method_eject_t method_eject_probe(method_inst_t *inst, const char *channel,
    const char *target);

// FAIL if the driver does not implement ejection or the removal was not
// issued. Does not wait for the platform to confirm the removal.
bool method_eject(method_inst_t *inst, const char *channel,
    const char *target, method_eject_t force, const char *reason);

method_state_t method_get_state(const method_inst_t *inst);

// Typically called by the method plugin itself as it progresses
// through its lifecycle (connecting, connected, disconnected).
void method_set_state(method_inst_t *inst, method_state_t state);

const char *method_state_name(method_state_t s);

bool method_subscribe(method_inst_t *inst, const char *name,
    method_msg_cb_t cb, void *data);

bool method_unsubscribe(method_inst_t *inst, const char *name);

// Called by method plugins when a message arrives from the platform.
// msg->inst is set automatically, and holds a reference for the length
// of the fan-out — a subscriber may read it long after the driver that
// delivered it was told to go away. A subscriber that keeps msg->inst
// past its own callback (an async command copies the whole message)
// must take its own method_hold().
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
//
// This is an authentication surface, not a diagnostic one.
// bot_identity_resolve() falls back to it whenever a caller passes no
// metadata, builds "<sender>!<ctx>" and matches that against the
// namespace's MFA patterns — so whatever this returns is what the
// caller is treated AS. A driver that answers from a stale cache grants
// a departed session's privileges to whoever holds the name now, and
// the divergence hides well: the message-carried path stays correct, so
// only the metadata-less callers are wrong. Prefer FAIL over a guess;
// callers read it as anonymous.
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

// Audit hook: yields each instance's retained driver vtable pointer.
// method_iterate_drivers/_instances deliberately expose only names —
// this one exposes the pointer, which is what a post-dlclose dangle
// test needs.
//
// Invoked UNDER method_mutex: the callback must be fast and must not
// re-enter any method_* API.
typedef void (*method_audit_cb_t)(const char *subject, const char *field,
    const void *ptr, void *data);

void method_audit_iterate(method_audit_cb_t cb, void *data);

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
//
// `refs` counts the registry plus every outstanding holder — a bot's
// binding, a driver's own back-pointer, an in-flight delivery, an async
// command's copied message. It is guarded by method_mutex, never
// atomic on its own: the count and the list membership that publishes
// it change together. method_unregister() unlinks the instance and
// hands back the registry's reference; whoever leaves last frees.
struct method_inst
{
  char                    name[METHOD_NAME_SZ];
  const method_driver_t  *driver;
  void                   *handle;    // driver-specific state
  method_state_t          state;
  uint32_t                refs;
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
