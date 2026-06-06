#pragma once
// CatastrophicGuard.hpp — UNIVERSAL in-flight catastrophe net over ALL engines.
// Iterates g_open_positions (every engine, every symbol) and force-flattens any
// position whose unrealised loss exceeds a HARD catastrophe threshold -- a backstop
// BEYOND each engine's own stop. Edge-safe: it only fires on a catastrophic adverse
// move (default 3x the per-trade dollar stop), so normal trades + engine SLs are
// untouched. Covers the gap the gold-only on_tick dollar-stop misses (index/FX).
//
// SHADOW-safe: in mode!=LIVE there are no real broker positions, so flatten is a
// no-op log. Flatten uses registered per-engine flatten callbacks (engines opt in
// via register_flatten); symbols without a callback are LOGGED LOUDLY so the
// operator can flatten manually (matches the MicroScalper-no-posid pattern).
//
// Wire: call g_catastrophic_guard.check(now_ms) from the 250ms tick block, and
// maintain g_catastrophic_guard.last_price[symbol] = mid on each tick.
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <cstdio>
#include "OpenPositionRegistry.hpp"
#include "sizing.hpp"

namespace omega {

class CatastrophicGuard {
public:
    double per_trade_usd = 50.0;     // = dollar_stop_usd
    double catastrophe_x = 3.0;      // flatten if loss > catastrophe_x * per_trade_usd
    bool   live = false;             // g_cfg.mode == "LIVE"
    std::unordered_map<std::string,double> last_price;            // symbol -> mid
    std::unordered_map<std::string,std::function<void(const std::string&)>> flatten_; // symbol -> flatten fn

    void set_price(const std::string& sym, double mid){ if(mid>0) last_price[sym]=mid; }
    void register_flatten(const std::string& sym, std::function<void(const std::string&)> fn){ flatten_[sym]=fn; }

    // Returns # of positions flattened/flagged this pass.
    int check(int64_t /*now_ms*/) {
        extern omega::OpenPositionRegistry g_open_positions;
        const double cat = catastrophe_x * per_trade_usd;
        int hit=0;
        for (const auto& p : g_open_positions.snapshot_all()) {
            auto it = last_price.find(p.symbol);
            if (it==last_price.end() || it->second<=0 || p.entry<=0 || p.size<=0) continue;
            const double mid = it->second;
            const bool islong = (p.side=="LONG");
            const double pts = islong ? (mid - p.entry) : (p.entry - mid);
            const double unr = pts * p.size * tick_value_multiplier(p.symbol);
            if (unr >= -cat) continue;                            // within tolerance
            ++hit;
            if (!live) {
                printf("[CATASTROPHE-SHADOW] %s %s entry=%.4f mid=%.4f unr=$%.0f > -$%.0f (shadow: no real pos)\n",
                       p.symbol.c_str(), p.side.c_str(), p.entry, mid, unr, cat);
                continue;
            }
            auto fit = flatten_.find(p.symbol);
            if (fit != flatten_.end()) {
                printf("[CATASTROPHE] FLATTEN %s %s entry=%.4f mid=%.4f unr=$%.0f > -$%.0f\n",
                       p.symbol.c_str(), p.side.c_str(), p.entry, mid, unr, cat);
                fit->second(p.symbol);
            } else {
                printf("[CATASTROPHE-NO-HANDLER] %s %s unr=$%.0f > -$%.0f -- OPERATOR MUST FLATTEN MANUALLY\n",
                       p.symbol.c_str(), p.side.c_str(), unr, cat);
            }
        }
        return hit;
    }
};

} // namespace omega
