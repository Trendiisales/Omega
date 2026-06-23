#pragma once
// =============================================================================
// MacroGoldGate.hpp -- macro-regime "hostile" de-risk flag for the gold book.
//
// Build #1 from the 2026-06-17 "best gold traders" research deep-dive. Successful
// gold traders trade the MACRO regime (real yields = dominant inverse driver,
// the dollar, COT positioning); Omega's RegimeState is deliberately PRICE-only.
// This gate layers a macro-hostile signal ON TOP of the price core as an
// asymmetric BEAR-INSURANCE tightening -- exactly the "price core + macro tighten"
// design RegimeState.hpp documents (never a replacement, only a further block).
//
// PRODUCER: tools/macro_gold_gate.py (daily cron) reads FRED 10y real-yield
// (DFII10) + broad-dollar (DTWEXBGS), computes the macro score
//   score = 2*sign(-d20(real_yield)) + 1*sign(-d20(dollar))   (range -3..+3)
// and writes a one-line flat file logs/macro/macro_gold_gate.tsv:
//   # macro_gold_gate  score  hostile  real_yield  dollar  stamp_ms
//   MACRO  -3  1  2.31  121.4  1781679000000
// hostile=1 when score <= HOSTILE_THRESH (-2): real yields rising hard.
//
// SAFETY -- FAILS SAFE (hostile=false) in every uncertain case, so a dead/stale
// macro feed can NEVER wrongly suppress the gold book; the price core still
// protects in a real bear:
//   - master enabled_ flag off              -> not hostile
//   - file missing / unreadable / malformed -> not hostile
//   - stamp older than max_stale_ms (3 days) -> not hostile (revert to price core)
//   - explicit fresh hostile=1              -> hostile
// now_ms passed in (epoch ms) to avoid time-shim coupling. Mirrors AuroraGate.
//
// CONSUMPTION: fed once per gold tick in tick_gold.hpp:
//   omega::gold_regime().set_macro_hostile(g_macro_gold_gate.hostile(now_ms));
// so all 8 long-only gold engines that already gate on gold_regime().long_blocked()
// inherit the tightening with no call-site change. Kill instantly:
//   g_macro_gold_gate.enabled_ = false;
//
// Validation (backtest/macro_gold_regime.py, daily-overlay, cross-regime
// bull 2024-26 + bear 2011-16): marginal value OVER the existing price gate --
// bull near-free (Sharpe 1.11->1.14, maxDD -20.2%->-14.7%), bear flips trend
// -2.6%->+7.8% maxDD -35.9%->-29.1%. Discovery-grade; faithful-arbiter pending.
// =============================================================================
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <mutex>

namespace omega {

struct MacroGoldGate {
    bool        enabled_      = true;
    std::string path_         = "logs/macro/macro_gold_gate.tsv";
    int64_t     max_stale_ms_ = 36LL * 60 * 60 * 1000;  // 36h -> fail safe (S-2026-06-23 audit: tightened from 3 days. The macro_gold_gate.py cron writes daily; 36h = "missed a full day + jitter margin" so a dead cron reverts to price-core next day instead of holding a 3-day-stale macro verdict. Fails SAFE either way: stale -> not hostile -> price core protects.)
    int64_t     reload_ms_    = 60000;                       // re-read at most every 60s

    bool     hostile_   = false;
    double   score_     = 0.0;
    int64_t  stamp_ms_  = 0;
    int64_t  last_check_ = 0;
    std::mutex mtx_;

    void set_path(const std::string& p) { path_ = p; }
    void set_enabled(bool e)            { enabled_ = e; }

    // Re-read the flat file at most once per reload_ms_. Cheap; safe to call per tick.
    void maybe_reload(int64_t now_ms) {
        if (now_ms - last_check_ < reload_ms_) return;
        last_check_ = now_ms;
        std::ifstream f(path_);
        if (!f.is_open()) { hostile_ = false; stamp_ms_ = 0; return; }
        std::string line; bool got = false;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string tag; double score = 0; int host = 0;
            double ry = 0, dx = 0; long long stamp = 0;
            if (ss >> tag >> score >> host >> ry >> dx >> stamp) {
                score_ = score; hostile_ = (host != 0); stamp_ms_ = (int64_t)stamp; got = true;
            }
        }
        if (!got) { hostile_ = false; stamp_ms_ = 0; }
    }

    // The only query engines need (via RegimeState). FAILS SAFE to false.
    bool hostile(int64_t now_ms) {
        if (!enabled_) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        maybe_reload(now_ms);
        if (stamp_ms_ <= 0) return false;
        if (now_ms - stamp_ms_ > max_stale_ms_) return false;  // stale -> revert to price core
        return hostile_;
    }
};

} // namespace omega
