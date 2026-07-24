// market_cmds.h — /whenmoon market verbs + /show whenmoon indicators.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_MARKET_CMDS_H
#define BM_WHENMOON_MARKET_CMDS_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>

// Registers:
//   /whenmoon market <verb>                       (parent)
//   /whenmoon market start <exch>-<base>-<quote>
//   /whenmoon market stop  <exch>-<base>-<quote>
//   /show whenmoon indicators <id> <gran> latest
// Invoked once from whenmoon_init after wm_dl_register_verbs.
bool wm_market_register_verbs(void);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_MARKET_CMDS_H
