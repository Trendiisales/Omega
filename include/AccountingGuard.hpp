#pragma once
// ============================================================================
// AccountingGuard.hpp -- S-2026-06-13h: independent accounting oversight of
// EVERY open position (operator mandate 2026-06-13: "let the normal trade play
// out BUT when our accounting check detects a runaway trade, cut it -- without
// touching the trade logic").
//
// WHAT IT IS NOT: a below-BE / early-loss killer. That trigger is tombstoned
// (89% of MAE-measurable winners go red before winning; Chimera S36 gained
// +1.07M bp by DISABLING early_kill; Omega BE tombstone; gvb study). Normal
// adverse excursion is the engine's business, not ours.
//
// WHAT IT IS: a catastrophe net at 3x the engine's own median loss (floor
// $25). The engine's stop always fires first in normal operation; the guard
// triggers only when the engine's protection has FAILED -- dead SL state
// (March GFE bug class), sizing errors (GoldOrbRetrace 100x rows: design loss
// ~$4, bled -$425/-$359 -- guard exits at -$25), immortal/zombie positions.
// Ledger-verified 2026-06-13: every profitable engine's worst NORMAL loss
// sits inside its cap (StraddleM15 median $30 / cap $91 / worst normal $48).
//
// PHASED ROLLOUT (operator-approved): Phase 1 = LOG-ONLY (this commit) --
// detect + [ACCT-GUARD] log + telemetry health alert; ENFORCE=false. Phase 2
// flips enforcement per-engine via close hooks once log-only output is
// reviewed (target: reuse the midnight-rollover force-close surface).
//
// PARENT-TU HEADER (row-12 pattern): zero local includes. Needs (from the
// including TU, see on_tick.hpp): PositionSnapshot/OpenPositionRegistry,
// tick_value_multiplier, <string>/<unordered_map>/<vector>/<cmath>/<cstdio>.
// Called from the on_tick 250ms universal-publisher block -- same thread that
// publishes the snapshots.
// ============================================================================

namespace omega_acct {

// Phase 2 flips this to true (per-engine close hooks land with it).
inline bool g_enforce = false;

// Per-engine runaway cap in USD = max(3 x median realized loss, $25).
// Derived 2026-06-13 from the cleaned cumulative ledger (442 closes). Engines
// not listed get DEFAULT_CAP -- pure catastrophe insurance.
inline double cap_for(const std::string& eng) {
    static const std::unordered_map<std::string, double> caps = {
        {"XauStraddleM15",       91.0},
        {"XauStraddleM30",      108.0},
        {"Tsmom_H1_long",       108.0},
        {"VWAPReversion",        25.0},
        {"GER_15m_MA_10_30",     25.0},
        {"GER_1h_DonchN100",     34.0},
        {"IndexSwing",           25.0},
        {"XauusdFvg",            42.0},
        {"GoldScalpPyramid",     38.0},
        {"FvgContinuation",     290.0},
        {"FvgCont30m",          218.0},
        {"FvgCont10m",          218.0},
        {"PeachyOrb",           219.0},
        {"Ger40TurtleH4",        74.0},
        {"AdaptiveHullGER",     293.0},
        {"DonchianBreakout",     25.0},
        {"GbpusdLondonOpen",     50.0},
        {"EmaPullback_H1_long", 106.0},
        {"BigCapMomo",          150.0},  // design loss = 5% trail x $1k = $50; 3x
        {"GoldOrbRetrace",       25.0},  // design loss ~$4 at 0.01 lot; the 100x bug class
    };
    auto it = caps.find(eng);
    if (it != caps.end()) return it->second;
    static const double DEFAULT_CAP = 150.0;
    return DEFAULT_CAP;
}

// Estimate unrealised USD for a snapshot. Prefer the source-provided value;
// fall back to (current-entry) x dir x size x tick_value when the snapshotter
// left it 0 but did provide a current price.
inline double unrealised_usd(const omega::PositionSnapshot& ps) {
    if (ps.unrealized_pnl != 0.0) return ps.unrealized_pnl;
    if (ps.current <= 0.0 || ps.entry <= 0.0 || ps.size <= 0.0) return 0.0;
    const double dir = (ps.side == "SHORT") ? -1.0 : 1.0;
    return (ps.current - ps.entry) * dir * ps.size * tick_value_multiplier(ps.symbol);
}

// Run one oversight pass. Phase 1: throttled breach logging only.
// Returns the number of positions currently in breach.
inline int check(const std::vector<omega::PositionSnapshot>& open, int64_t now_s) {
    static std::unordered_map<std::string, int64_t> s_last_log;  // key -> last log ts
    int breaches = 0;
    for (const auto& ps : open) {
        const double unr = unrealised_usd(ps);
        if (unr >= 0.0) continue;
        const double cap = cap_for(ps.engine);
        if (-unr < cap) continue;
        ++breaches;
        const std::string key = ps.engine + "|" + ps.symbol + "|" + ps.side;
        int64_t& last = s_last_log[key];
        if (now_s - last >= 60) {
            last = now_s;
            std::printf("[ACCT-GUARD] BREACH %s %s %s entry=%.4f cur=%.4f unr=$%.2f cap=$%.2f -- %s\n",
                        ps.engine.c_str(), ps.symbol.c_str(), ps.side.c_str(),
                        ps.entry, ps.current, unr, cap,
                        g_enforce ? "FORCE-CLOSING" : "LOG-ONLY (phase 1; enforcement off)");
            std::fflush(stdout);
        }
        // Phase 2: per-engine close hook fires here when g_enforce==true.
    }
    return breaches;
}

} // namespace omega_acct
