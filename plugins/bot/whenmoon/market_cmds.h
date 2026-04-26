// market_cmds.h — /bot <name> market start|stop verbs.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_MARKET_CMDS_H
#define BM_WHENMOON_MARKET_CMDS_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>

// Registers:
//   /bot <name> market <verb>            (parent)
//   /bot <name> market start <pair>
//   /bot <name> market stop  <pair>
// Invoked once from whenmoon_init after wm_dl_register_verbs.
bool wm_market_register_verbs(void);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_MARKET_CMDS_H
