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
// In LIVE it first tries a registered per-engine flatten callback keyed
// "symbol|engine"; absent that it falls back to the UNIVERSAL flatten hook
// (S-2026-07-20j: same opposing-MKT + close_matching path as the KILL-ALL panic
// button — registered once in engine_init, closes ANY registry position). Only if
// BOTH are missing does it log [CATASTROPHE-NO-HANDLER] for a manual cut.
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
    size_t flatten_count() const { return flatten_.size(); }

    // S-2026-07-20j: UNIVERSAL flatten fallback — one hook that can close ANY
    // g_open_positions snapshot (opposing MKT via send_live_order + close_matching,
    // the proven KILL-ALL path). Registered once in engine_init after the exec
    // layer is up. Returns true if the engine slot was cleared (closer wired).
    std::function<bool(const PositionSnapshot&)> universal_flatten;

    // Boot-time LIVE-readiness gate. Call once at startup AFTER engine init.
    // If we are LIVE but no per-engine flatten hooks are registered, the guard can
    // only LOG catastrophic breaches, not auto-close them -> warn loudly so the
    // operator wires register_flatten() before sizing up. Returns true if ready.
    bool warn_if_live_unhooked(bool is_live) const {
        if (!is_live) return true;                 // shadow: hooks not needed
        if (universal_flatten) {
            printf("[CATASTROPHE-GUARD] LIVE + UNIVERSAL flatten hook registered (+%zu per-engine) -- auto-flatten ARMED\n",
                   flatten_count());
            fflush(stdout); return true;
        }
        if (flatten_count() > 0) {
            printf("[CATASTROPHE-GUARD] LIVE + %zu flatten hooks registered -- auto-flatten ARMED\n", flatten_count());
            fflush(stdout); return true;
        }
        printf("\n*** [CATASTROPHE-GUARD] WARNING: mode=LIVE but ZERO flatten hooks registered. ***\n"
               "*** Universal catastrophe net can DETECT+LOG breaches but CANNOT auto-close.   ***\n"
               "*** Wire register_flatten(\"SYMBOL|Engine\", fn) per live engine BEFORE sizing  ***\n"
               "*** up, or breaches will only print [CATASTROPHE-NO-HANDLER] for manual cut.   ***\n\n");
        fflush(stdout); return false;
    }

    // Returns # of catastrophic positions seen this pass. now_s = epoch seconds (throttle).
    int check(int64_t now_s) {
        // g_open_positions extern-declared in OpenPositionRegistry.hpp (included above)
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
            } else if (universal_flatten) {
                printf("[CATASTROPHE] FLATTEN(universal) %s/%s %s entry=%.4f unr=$%.0f < -$%.0f\n",
                       p.symbol.c_str(), p.engine.c_str(), p.side.c_str(), p.entry, unr, cat); fflush(stdout);
                if (!universal_flatten(p)) {
                    printf("[CATASTROPHE][NO-CLOSER] %s/%s -- broker close SENT but engine slot NOT cleared\n",
                           p.symbol.c_str(), p.engine.c_str()); fflush(stdout);
                }
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
