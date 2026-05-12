// kraken_rest.h — Kraken REST mechanism + freelist.
//
// KR-3 ships only the lifecycle hooks so the descriptor in kraken.c
// can call kr_rest_init / kr_rest_deinit symmetrically with the other
// subsystems. The signed-request submitter, response classifier, and
// typed endpoint wrappers land in KR-4.

#ifndef BM_KRAKEN_REST_H
#define BM_KRAKEN_REST_H

// Lifecycle. Paired with kr_init / kr_deinit in kraken.c.
void    kr_rest_init(void);
void    kr_rest_deinit(void);

#endif // BM_KRAKEN_REST_H
