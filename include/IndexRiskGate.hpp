#pragma once
// =============================================================================
//  IndexRiskGate.hpp -- portfolio-level macro RISK-OFF gate for index engines (S44)
//
//  Single shared regime read that ALL index engines consult to suppress NEW
//  entries when the macro backdrop is hostile. Three research-backed signals,
//  any of which trips risk-off (entry-only -- never touches open positions):
//    1. VIX term structure  : VIX/VIX3M >= 1.05  (deep backwardation; precedes
//       ~every major drawdown). Lifts the seasonal sleeve Sharpe 0.69->0.80.
//    2. CREDIT deterioration : HYG/LQD 20d momentum < 0 (credit leads equity).
//    3. DOLLAR strength      : DXY 20d momentum > 0 (USD tightening = equity headwind).
//  The credit&dollar COMBO is the validated overlay (index_macro_regime.cpp):
//  6-index EW basket Sharpe 0.63->1.02, maxDD -61%; day-of-week seasonal sleeve
//  1.13->1.27, maxDD -67%. CAVEAT: aggressive (in-market ~37%) and on a 7yr / ~2
//  -regime sample partly = "avoid 2020+2022" -> all index engines are SHADOW;
//  observe in the ledger before trusting live. Components individually toggleable.
//
//  Feed: data/index_regime.txt  ("epoch_sec,vix_ratio,credit_mom,dxy_mom"),
//  refreshed daily by tools/fetch_macro_regime.py (broker FIX lacks VIX3M/credit).
//  Back-compat: also accepts the legacy 2-field "epoch_sec,ratio" (VIX only).
//  GRACEFUL DEGRADE: missing / stale (>4d) / malformed -> all signals invalid ->
//  risk_off()=false, so a dead feed NEVER latches risk-off or halts trading.
//
//  Self-contained (no globals.hpp dep) so engine headers include it without the
//  circular dependency (globals.hpp includes the engines).
// =============================================================================
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>

namespace omega {

// component toggles (flip to false to disable a leg without a rebuild change elsewhere)
inline bool gate_use_vix    = true;
inline bool gate_use_credit = true;
inline bool gate_use_dollar = true;
inline double gate_vix_thr  = 1.05;

inline std::atomic<double>  g_vix_term_ratio{-1.0};   // VIX/VIX3M (-1 = unknown)
inline std::atomic<double>  g_index_credit_mom{0.0};  // HYG/LQD 20d momentum (0 = unknown/neutral)
inline std::atomic<double>  g_index_dxy_mom{0.0};     // DXY 20d momentum
inline std::atomic<int64_t> g_index_regime_read_s{0};
inline std::atomic<bool>    g_index_regime_valid{false};
inline std::atomic<bool>    g_index_risk_off_state{false};

// Refresh the shared regime from the feed file, throttled to once/hour. Safe to
// call every tick. Logs only on risk-off state TRANSITIONS (no spam).
inline void refresh_index_vix_ratio(int64_t now_s) noexcept {
    const int64_t last = g_index_regime_read_s.load(std::memory_order_relaxed);
    if (now_s - last < 3600) return;
    g_index_regime_read_s.store(now_s, std::memory_order_relaxed);

    double vix = -1.0, credit = 0.0, dxy = 0.0; bool valid = false;
    auto parse = [&](const char* path)->bool {
        std::ifstream f(path); if (!f.is_open()) return false;
        std::string line, last_line;
        while (std::getline(f, line))
            if (!line.empty() && (line[0]=='-' || (line[0]>='0'&&line[0]<='9'))) last_line = line;
        if (last_line.empty()) return false;
        double ts=0, a=0, b=0, c=0;
        int n = std::sscanf(last_line.c_str(), "%lf,%lf,%lf,%lf", &ts, &a, &b, &c);
        if (n < 2 || ts <= 0.0 || (now_s - (int64_t)ts) > 4*86400) return false;
        vix = (a > 0.0) ? a : -1.0;
        if (n >= 4) { credit = b; dxy = c; }   // full regime file
        return true;
    };
    if (parse("data/index_regime.txt") || parse("data/vix_term_ratio.txt")) valid = true;

    g_vix_term_ratio.store(vix, std::memory_order_relaxed);
    g_index_credit_mom.store(credit, std::memory_order_relaxed);
    g_index_dxy_mom.store(dxy, std::memory_order_relaxed);
    g_index_regime_valid.store(valid, std::memory_order_relaxed);

    bool now_off = false;
    if (valid) {
        if (gate_use_vix    && vix    > 0.0 && vix    >= gate_vix_thr) now_off = true;
        if (gate_use_credit && credit < 0.0)                          now_off = true;
        if (gate_use_dollar && dxy    > 0.0)                          now_off = true;
    }
    const bool was_off = g_index_risk_off_state.exchange(now_off, std::memory_order_relaxed);
    if (now_off != was_off) {
        std::printf("[INDEX-RISK-GATE] %s vix=%.3f credit=%+.4f dxy=%+.4f -- new index entries %s\n",
                    now_off ? "RISK-OFF" : "risk-on", vix, credit, dxy, now_off ? "SUPPRESSED" : "allowed");
        std::fflush(stdout);
    }
}

// True when the macro backdrop is risk-off (any enabled leg trips). Index
// engines call this to block NEW entries only. Graceful: invalid feed => false.
inline bool index_risk_off() noexcept {
    if (!g_index_regime_valid.load(std::memory_order_relaxed)) return false;
    const double vix = g_vix_term_ratio.load(std::memory_order_relaxed);
    const double cr  = g_index_credit_mom.load(std::memory_order_relaxed);
    const double dx  = g_index_dxy_mom.load(std::memory_order_relaxed);
    if (gate_use_vix    && vix > 0.0 && vix >= gate_vix_thr) return true;
    if (gate_use_credit && cr  < 0.0)                        return true;
    if (gate_use_dollar && dx  > 0.0)                        return true;
    return false;
}

} // namespace omega
