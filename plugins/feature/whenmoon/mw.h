// mw.h — marketwatch subsystem (MW-2). Periodic bulk-ticker poller
// living inside whenmoon. Substrate for MW-3..5 detectors + CLAM
// emission; this chunk ships only the ring + lifecycle + KV surface.
//
// Public surface is the bare lifecycle hooks plus runtime enable /
// disable / status renderers. Everything else (state struct, ring
// shape, per-pair table, callbacks) lives inside mw.c.

#ifndef BM_WM_MW_H
#define BM_WM_MW_H

#ifdef WHENMOON_INTERNAL

#include <stdbool.h>

#include "method.h"

#define MW_CTX  "whenmoon mw"

bool mw_init(void);
bool mw_start(void);
void mw_stop(void);
void mw_deinit(void);

bool mw_enable_exch(const char *name);
bool mw_disable_exch(const char *name);
bool mw_set_global_enabled(bool on);

// Operator-visible renderers. Output is multi-line; both helpers walk
// state under their own locking and emit via method_send (thread-safe).
// mw_render_status_exch returns FAIL when the named exchange is not
// registered or not tracked.
void mw_render_status(method_inst_t *inst, const char *target);
bool mw_render_status_exch(method_inst_t *inst, const char *target,
    const char *exch);

// Registers /whenmoon mw + /show whenmoon mw verb tree. Called from
// whenmoon_init after mw_init succeeds.
bool mw_cmds_register(void);

#endif // WHENMOON_INTERNAL

#endif // BM_WM_MW_H
