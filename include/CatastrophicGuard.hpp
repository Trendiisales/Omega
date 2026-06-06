#pragma once
// CatastrophicGuard.hpp — UNIVERSAL in-flight catastrophe net over ALL engines.
// Closes the gap the gold-only on_tick dollar-stop (XAUUSD-only, per-engine) misses:
// index / FX / any-symbol positions had NO central emergency cut.
//
// Iterates g_open_positions (every engine, every symbol) and acts on any position
// whose unrealised USD loss exceeds a HARD catastrophe threshold (default 3x the
// per-trade dollar stop) — a backstop BEYOND each engine's own SL. Edge-safe: it
// only fires on a catastrophic adverse move, so normal trades + engine SLs are
// untouched; it can't shrink an edge because it never acts inside the noise band.
//
// Uses PositionSnapshot.unrealized_pnl (already USD, maintained by each source) — so
// it needs no price feed of its own. unrealized_pnl==0 = "unknown" -> skipped.
//
// SHADOW-safe: when live==false (g_cfg.mode != "LIVE") there is no real broker
// position, so it LOGS the breach ([CATASTROPHE-SHADOW]) and never flattens — the
// engine's own simulated SL must run (matches the dollar-stop shadow-skip rule).
// In LIVE it calls a registered per-engine flatten callback keyed "symbol|engine";
// symbols without a registered flatten are LOGGED LOUDLY ([CATASTROPHE-NO-HANDLER])
// so the operator can flatten manually.
//
// Wire: once per 250ms in on_tick, call g_catastrophic_guard.check(now_s) after the
// existing dollar-stop block. Set .live / .per_trade_usd from config first.
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdio>
#include <cstdint>
#include "OpenPositionRegistry.hpp"

namespace omega {

class CatastrophicGuard {
public:
    double  per_trade_usd = 50.0;   // = g_cfg.dollar_stop_usd
    double  catastrophe_x = 3.0;    // act when loss > catastrophe_x * per_trade_usd
    bool    live          = false;  // g_cfg.mode == "LIVE"
    int64_t log_every_s   = 10;     // throttle repeat logs per key

    // key = "symbol|engine" -> flatten action (LIVE only)
    void register_flatten(const std::string& key, std::function<void()> fn){ flatten_[key]=fn; }

    // Returns # of catastrophic positions seen this pass. now_s = epoch seconds (throttle).
    int check(int64_t now_s) {
        extern omega::OpenPositionRegistry g_open_positions;
        const double cat = catastrophe_x * per_trade_usd;
        if (cat <= 0.0) return 0;
        int hit = 0;
        for (const auto& p : g_open_positions.snapshot_all()) {
            if (p.size <= 0.0) continue;
            const double unr = p.unrealized_pnl;     // USD; 0 = unknown -> skip
            if (unr >= -cat) continue;
            ++hit;
            const std::string key = p.symbol + "|" + p.engine;
            int64_t& last = last_log_[key];
            const bool do_log = (now_s - last) >= log_every_s;
            if (do_log) last = now_s;

            if (!live) {
                if (do_log) { printf("[CATASTROPHE-SHADOW] %s/%s %s entry=%.4f unr=$%.0f < -$%.0f -- engine SL must run (shadow)\n",
                                     p.symbol.c_str(), p.engine.c_str(), p.side.c_str(), p.entry, unr, cat); fflush(stdout); }
                continue;
            }
            auto it = flatten_.find(key);
            if (it != flatten_.end()) {
                printf("[CATASTROPHE] FLATTEN %s/%s %s entry=%.4f unr=$%.0f < -$%.0f\n",
                       p.symbol.c_str(), p.engine.c_str(), p.side.c_str(), p.entry, unr, cat); fflush(stdout);
                it->second();
            } else if (do_log) {
                printf("[CATASTROPHE-NO-HANDLER] %s/%s %s unr=$%.0f < -$%.0f -- OPERATOR MUST FLATTEN MANUALLY\n",
                       p.symbol.c_str(), p.engine.c_str(), p.side.c_str(), unr, cat); fflush(stdout);
            }
        }
        return hit;
    }
private:
    std::unordered_map<std::string,std::function<void()>> flatten_;
    std::unordered_map<std::string,int64_t> last_log_;
};

} // namespace omega
