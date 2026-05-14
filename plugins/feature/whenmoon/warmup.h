// warmup.h — WM-WARMUP-1 per-grain REST history warmup at strategy
// attach. Internal; WHENMOON_INTERNAL-gated.
//
// When a strategy attaches to a running market, fetch each subscribed
// grain's declared min_history directly from the exchange at that
// grain and seed the aggregator's per-grain ring — so the strategy's
// indicators are warm at cold start instead of waiting for the live
// 1m->cascade to accumulate hundreds of higher-grain bars. Critical
// for Kraken-backed markets: Kraken's REST 1m history is too shallow
// to ever cascade-fill deep 5m/1h/4h/1d rings.

#ifndef BM_WHENMOON_WARMUP_H
#define BM_WHENMOON_WARMUP_H

#ifdef WHENMOON_INTERNAL

struct whenmoon_state;
struct whenmoon_market;
typedef struct loaded_strategy loaded_strategy_t;

// Kick one async REST candle fetch per subscribed grain that the
// strategy declares a min_history for and whose ring is not already
// deep enough. Best-effort and lock-free at the call site: a grain
// whose fetch fails — or whose exchange lacks that native granularity
// — warms via the live cascade instead. Takes mk->lock only briefly
// per grain for the depth check. Called from wm_strategy_attach after
// both the registry lock and mk->lock have been released.
void wm_warmup_for_attachment(struct whenmoon_state *st,
    struct whenmoon_market *mk, const loaded_strategy_t *ls);

#endif // WHENMOON_INTERNAL

#endif // BM_WHENMOON_WARMUP_H
