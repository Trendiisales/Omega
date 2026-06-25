#pragma once
// ============================================================================
// TrendAccountingGuard.hpp  (S-2026-06-26)
//
// The "accounting engine" overlay for the SLOW trend-following engines (gold
// XauTrendFollow, index turtles). Operator: leave the wide ride as-is, but
// monitor each open trend position separately and CANCEL it the moment it has
// STALLED (dead money — entered but never moved) or is REVERSING (genuine
// momentum turn), to preserve capital. Does NOT touch the runners.
//
// WHY this works where a blanket giveback-floor / tight trail does NOT
// (validated freq_dd_frontier 2016-2026, faithful next-open + cost):
//   * blanket giveback-floor (lock 50% of peak): SPX PF 3.80->1.18, +91%->+8%
//     -- it cuts the RUNNERS too, destroying the edge.
//   * SELECTIVE stall+reversal supervisor: SPX PF 3.80->5.30 (+91%->+99%),
//     NDX 2.68->3.63 (+112%->+122%), DJ30 ~tie. Robust across the param sweep
//     (held>8..15, MFE<1.5..3%) AND both halves (H1 4.84/4.44/2.98, H2
//     3.64/3.00/1.62). It only cuts DEAD trades (low MFE) + REAL reversals;
//     high-MFE runners are never touched. PF up, return up, capital freed.
//
// Pure logic, no engine deps -> unit-testable + reusable across every trend
// engine. The engine feeds per-position state each bar; the guard returns the
// cut decision. The thresholds are in BAR units of the engine's own timeframe;
// the validated values (10-12 bars / 2-2.5% MFE / 9-21 EMA) are calibrated on
// DAILY bars (index turtles, gold D1). Faster TFs (gold 4h/2h/1h) MUST set
// stall_bars/EMA periods scaled to their bar size + re-validate before live.
// ============================================================================
#include <string>

namespace omega {

struct TrendAccountingGuard {
    // ---- config (validated daily defaults; opt-in via the owning engine) ----
    bool   enabled      = false;  // master gate (engine sets per-instance)
    int    stall_bars   = 11;     // held >= this many bars ...
    double stall_mfe    = 0.022;  // ... AND max-favourable-excursion < this frac -> dead money -> cut
    int    ema_fast     = 9;      // reversal = fast EMA crosses below slow EMA ...
    int    ema_slow     = 21;     // ... (caller supplies the two EMA values)
    bool   reversal_needs_profit = true;  // only fire REVERSAL once the trade has been positive (mfe>0)

    enum class Cut { NONE, STALL, REVERSAL };

    // Per-bar decision. `held_bars` = bars since entry; `mfe_frac` = peak
    // favourable excursion as a fraction of entry; `ema_fast_v`/`ema_slow_v` =
    // the engine's fast/slow EMA on its bar TF for this symbol.
    Cut decide(int held_bars, double mfe_frac, double ema_fast_v, double ema_slow_v) const {
        if (!enabled) return Cut::NONE;
        // STALL: capital tied up in a trade that entered and never went anywhere.
        if (held_bars >= stall_bars && mfe_frac < stall_mfe) return Cut::STALL;
        // REVERSAL: genuine momentum turn (fast<slow), only after it was in profit
        // so a deep winner pulling back to the fast line doesn't fire prematurely.
        if (ema_fast_v < ema_slow_v && (!reversal_needs_profit || mfe_frac > 0.0))
            return Cut::REVERSAL;
        return Cut::NONE;
    }

    static const char* reason(Cut c) {
        switch (c) { case Cut::STALL: return "STALL"; case Cut::REVERSAL: return "REVERSAL"; default: return ""; }
    }
};

} // namespace omega
