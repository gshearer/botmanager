#ifndef BM_NL_H
#define BM_NL_H

#include <stddef.h>

// Shared natural-language example type. Used by both cmd_nl_t (command
// registrations) and kv_nl_t (KV responders). Both strings must be
// static / caller-owned — the registry keeps pointers, never copies.
typedef struct
{
  const char *utterance;   // illustrative user line
  const char *invocation;  // canonical command the bot should emit
} nl_example_t;

#endif // BM_NL_H
