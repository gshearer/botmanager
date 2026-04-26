// dl_trades.h — trade-history page loop for the whenmoon downloader.
// Internal; WHENMOON_INTERNAL-gated.

#ifndef BM_WHENMOON_DL_TRADES_H
#define BM_WHENMOON_DL_TRADES_H

#ifdef WHENMOON_INTERNAL

#include "dl_jobtable.h"

// Dispatches one /products/<sym>/trades request on behalf of `j`.
// Caller has already set j->in_flight=true under the jobtable lock.
// Returns SUCCESS if the request was submitted (the completion
// callback will eventually clear in_flight); FAIL on submit error, in
// which case in_flight is cleared by the caller (wm_dl_kick).
bool wm_dl_trades_dispatch_one(dl_jobtable_t *t, dl_job_t *j);

#endif // WHENMOON_INTERNAL
#endif // BM_WHENMOON_DL_TRADES_H
