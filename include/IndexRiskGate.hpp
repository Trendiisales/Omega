#pragma once
// =============================================================================
//  IndexRiskGate.hpp -- portfolio-level VIX term-structure risk-off gate (S44)
//
//  Single shared VIX/VIX3M ratio that ALL index engines consult to suppress NEW
//  entries during deep backwardation (ratio >= 1.05). Backwardation has preceded
//  ~every major equity drawdown (Quantpedia/VolatilityBox) and the same threshold
//  validated on the seasonal sleeve (Sharpe 0.69->0.80, maxDD halved). This is a
//  RISK OVERLAY (entry-only -- never touches open positions / exits / management).
//
//  Feed: data/vix_term_ratio.txt ("epoch_sec,ratio"), refreshed daily by
//  tools/fetch_vix_ratio.py (broker FIX has no VIX3M). GRACEFUL DEGRADE: a
//  missing / stale (>4d) / malformed file -> ratio invalid -> risk_off()=false,
//  so a dead feed NEVER latches risk-off and never silently halts trading.
//
//  Self-contained (no globals.hpp dep) so engine headers can include it without
//  the circular dependency (globals.hpp includes the engines).
// =============================================================================
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>

namespace omega {

inline std::atomic<double>  g_vix_term_ratio{-1.0};
inline std::atomic<int64_t> g_vix_term_read_s{0};
inline std::atomic<bool>    g_index_risk_off_state{false};

// Refresh the shared ratio from the feed file, throttled to once/hour. Safe to
// call on every tick. Logs only on risk-off state TRANSITIONS (no spam).
inline void refresh_index_vix_ratio(int64_t now_s) noexcept {
    const int64_t last = g_vix_term_read_s.load(std::memory_order_relaxed);
    if (now_s - last < 3600) return;
    g_vix_term_read_s.store(now_s, std::memory_order_relaxed);

    double ratio = -1.0;
    std::ifstream f("data/vix_term_ratio.txt");
    if (f.is_open()) {
        std::string line, last_line;
        while (std::getline(f, line))
            if (!line.empty() && (line[0]=='-' || (line[0]>='0'&&line[0]<='9'))) last_line = line;
        double ts=0, r=0;
        if (!last_line.empty() && std::sscanf(last_line.c_str(), "%lf,%lf", &ts, &r) == 2
            && r > 0.0 && ts > 0.0 && (now_s - (int64_t)ts) <= 4*86400)
            ratio = r;
    }
    g_vix_term_ratio.store(ratio, std::memory_order_relaxed);

    const bool now_off = (ratio > 0.0 && ratio >= 1.05);
    const bool was_off = g_index_risk_off_state.exchange(now_off, std::memory_order_relaxed);
    if (now_off != was_off) {
        std::printf("[INDEX-RISK-GATE] %s ratio=%.4f (VIX/VIX3M, thr=1.05) -- new index entries %s\n",
                    now_off ? "RISK-OFF" : "risk-on", ratio, now_off ? "SUPPRESSED" : "allowed");
        std::fflush(stdout);
    }
}

// True when the VIX term structure is in deep backwardation (fresh ratio>=thr).
// Index engines call this to block NEW entries only.
inline bool index_risk_off(double thr = 1.05) noexcept {
    const double r = g_vix_term_ratio.load(std::memory_order_relaxed);
    return r > 0.0 && r >= thr;
}

} // namespace omega
