// LukeEntryQuality.hpp — pure-std entry-quality detectors mined + faithfully
// validated from the Martin Luke tight-stop system (YouTube NnCiWy7f84w;
// see Memory-Omega/wiki/concepts/luke-tight-stop-system.md).
//
// NO TWS / no Omega-engine includes — only <vector>/<cmath> so this compiles on
// Mac (canary) and is unit-testable standalone. The live engine feeds it a
// candidate's DAILY OHLC history (the engine already pulls SPY daily for its
// regime gate; per-name daily is the same reqHistoricalData call) and uses the
// returned signal as an OPT-IN entry-QUALITY gate layered on the existing 5m
// ignition trigger — the video's own multi-timeframe logic (daily = WHERE is a
// valid setup, the engine's 5m = WHEN to pull the trigger).
//
// FAITHFUL-BT BASIS (backtest/luke_system/, 30 high-ADR stocks 2024-26, 2592-cfg
// sweep): setups A (pullback-to-rising-9/21-EMA) + C (inside-day/micro-VCP
// breakout) = champion PF2.63 +75% maxDD21.5%, survives 2x cost, 1156/2592
// robust. Tight <=6% stop-width selectivity monotonically raises PF (2.11->2.63).
// Setup B (anchored-VWAP cluster) FAILED robustness -> deliberately NOT included.
//
// CAVEAT (do not skip): the edge is DAILY-resolution. Before this gate may flip
// from SHADOW to live-size it must be revalidated at the engine's 5m entry
// resolution (the daily signal picks the name; 5m fill quality is unproven).
#pragma once
#include <vector>
#include <cmath>

namespace omega::luke {

struct Bar { double open, high, low, close; };

// Wilder/standard EMA of closes, last value.
inline double ema(const std::vector<Bar>& b, int n, int end) {
    if (end < n) return std::nan("");
    const double k = 2.0 / (n + 1.0);
    double e = b[end - n + 1].close;
    for (int i = end - n + 2; i <= end; ++i) e = b[i].close * k + e * (1 - k);
    return e;
}

// Average daily range % over `win` bars ending at `end`.
inline double adr_pct(const std::vector<Bar>& b, int win, int end) {
    if (end < win) return std::nan("");
    double s = 0; int c = 0;
    for (int i = end - win + 1; i <= end; ++i) {
        if (b[i].close > 0) { s += (b[i].high - b[i].low) / b[i].close * 100.0; ++c; }
    }
    return c ? s / c : std::nan("");
}

struct Knobs {
    double touch_buf   = 0.015;  // pullback "near EMA" band
    double base_buf    = 0.06;   // prior-strength: within X% of 20d high
    double stop_buf    = 0.003;  // stop placed this far below structure
    double adr_min     = 4.0;    // high-ADR universe floor (%)
    double max_stopw   = 0.06;   // SELECTIVITY: reject setups whose structural stop > this (the video's core lever)
    double min_stopw   = 0.005;
};

// Result: setup found ('A','C', or 0), the breakout trigger price (prior-bar
// high) and the structural stop. The engine times the actual 5m fill on a break
// of `trig`; sizing = risk$ / (fill - stop).
struct Signal { char setup = 0; double trig = 0, stop = 0, stopw = 0, adr = 0; };

// Evaluate on close of bar `end` (act next session). modeA / modeC toggle setups.
inline Signal evaluate(const std::vector<Bar>& b, int end, const Knobs& K,
                       bool modeA = true, bool modeC = true) {
    Signal s;
    if (end < 55) return s;
    const double e9 = ema(b, 9, end), e21 = ema(b, 21, end), e50 = ema(b, 50, end);
    if (std::isnan(e50)) return s;
    const double adr = adr_pct(b, 20, end);
    if (std::isnan(adr) || adr < K.adr_min) return s;
    const double e21_prev = ema(b, 21, end - 5);
    const bool up   = !std::isnan(e21_prev) && e21 > e21_prev;       // rising 21EMA
    const bool lead = e9 > e21 && e21 > e50;                          // bullish stack (LEAD tier)
    const Bar& c = b[end];
    double hh20 = 0; for (int i = end - 19; i <= end; ++i) hh20 = std::max(hh20, b[i].high);

    // --- A: first pullback to rising 9/21 EMA in a constructive base ---
    if (modeA && lead && up) {
        const bool near21 = c.low <= e21 * (1 + K.touch_buf) && c.close > e21;
        const bool near9  = c.low <= e9  * (1 + K.touch_buf) && c.close > e9;
        const bool strength = c.close >= hh20 * (1 - K.base_buf);
        if ((near21 || near9) && strength) {
            s.setup = 'A'; s.trig = c.high;
            s.stop = std::min(c.low, near21 ? e21 : e9) * (1 - K.stop_buf);
        }
    }
    // --- C: inside-day / micro-VCP range-contraction breakout ---
    if (!s.setup && modeC && up && e9 > e21 && c.close > e21) {
        const bool inside1 = end >= 1 && c.high < b[end-1].high && c.low > b[end-1].low;
        const bool inside2 = inside1 && end >= 2 && b[end-1].high < b[end-2].high && b[end-1].low > b[end-2].low;
        if (inside1 || inside2) { s.setup = 'C'; s.trig = c.high; s.stop = c.low * (1 - K.stop_buf); }
    }
    if (!s.setup || s.trig <= s.stop) { s.setup = 0; return s; }
    s.stopw = (s.trig - s.stop) / s.trig;
    s.adr = adr;
    if (s.stopw < K.min_stopw || s.stopw > K.max_stopw) { s.setup = 0; return s; }  // tight-stop selectivity
    return s;
}

} // namespace omega::luke
