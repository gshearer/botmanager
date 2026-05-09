// order_cmds.h — /whenmoon order + /show whenmoon orders|exchange (WM-OR-1).
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_ORDER_CMDS_H
#define BM_WHENMOON_ORDER_CMDS_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>

// Registers:
//   /whenmoon order <verb>                              (parent + 'or' alias)
//   /whenmoon order buy <market> <limit|market> <qty> [<price>]
//   /whenmoon order sell <market> <limit|market> <qty> [<price>]
//   /whenmoon order cancel <exchange> <order-id>
// Invoked once from whenmoon_init after wm_trade_register_verbs.
bool wm_order_register_verbs(void);

// Registers:
//   /show whenmoon orders [exchange]
//   /show whenmoon exchange [name]                       ('exch' alias)
// Invoked once from whenmoon_init alongside wm_order_register_verbs.
bool wm_exch_register_verbs(void);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_ORDER_CMDS_H
