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
#include <unordered_set>   // enforcement breach-tracking (Phase 2); std headers only
#include <string>

namespace omega_acct {

// Phase 2 ARMED 2026-06-13 (S-2026-06-13u, operator directive). Enforcement
// force-closes a runaway via g_open_positions.close_matching() -> the engine's
// own force_close (books + clears its slot). Surgical: ledger-proven to cut only
// genuine runaways (2 of 269 closes / 32d), never a normal stop. A breach must
// PERSIST >= ACCT_CONFIRM_SEC before it force-closes (a single tick spike past
// the cap does not trigger).
inline bool g_enforce = true;
inline constexpr int64_t ACCT_CONFIRM_SEC = 5;

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
    static std::unordered_map<std::string, int64_t> s_last_log;     // key -> last log ts
    static std::unordered_map<std::string, int64_t> s_breach_since; // key -> first-seen breach ts
    static std::unordered_set<std::string>          s_seen_this;    // keys breaching this pass
    s_seen_this.clear();
    int breaches = 0;
    for (const auto& ps : open) {
        const double unr = unrealised_usd(ps);
        if (unr >= 0.0) continue;
        const double cap = cap_for(ps.engine);
        if (-unr < cap) continue;
        ++breaches;
        // position identity includes entry_ts so a NEW trade in the same engine
        // is a fresh breach window, not a stale carry-over.
        const std::string key = ps.engine + "|" + ps.symbol + "|" + ps.side + "|" +
                                std::to_string(ps.entry_ts);
        s_seen_this.insert(key);
        int64_t& since = s_breach_since[key];
        if (since == 0) since = now_s;                 // first tick over cap
        const bool confirmed = (now_s - since) >= ACCT_CONFIRM_SEC;

        int64_t& last = s_last_log[key];
        if (now_s - last >= 60 || (g_enforce && confirmed)) {
            last = now_s;
            const char* disp = !g_enforce ? "LOG-ONLY (phase 1)"
                             : !confirmed  ? "watching (confirm window)"
                                           : "ENFORCE -> force-closing";
            std::printf("[ACCT-GUARD] BREACH %s %s %s entry=%.4f cur=%.4f unr=$%.2f cap=$%.2f -- %s\n",
                        ps.engine.c_str(), ps.symbol.c_str(), ps.side.c_str(),
                        ps.entry, ps.current, unr, cap, disp);
            std::fflush(stdout);
        }
        // Phase 2 enforcement: on a SUSTAINED breach, force-close via the engine's
        // own closer (books + clears its slot). One shot per position identity.
        if (g_enforce && confirmed) {
            const bool closed = g_open_positions.close_matching(ps, "ACCT_GUARD_RUNAWAY");
            std::printf("[ACCT-GUARD] %s %s %s -- %s\n",
                        closed ? "FORCE-CLOSED" : "UNENFORCEABLE (no closer wired for)",
                        ps.engine.c_str(), ps.symbol.c_str(),
                        closed ? "runaway cut" : "position left open -- needs a closer");
            std::fflush(stdout);
            s_breach_since[key] = now_s + 3600;  // suppress re-fire on this identity for 1h
        }
    }
    // GC: drop breach timers for positions no longer breaching (they recovered/closed)
    for (auto it = s_breach_since.begin(); it != s_breach_since.end(); ) {
        if (s_seen_this.find(it->first) == s_seen_this.end()) it = s_breach_since.erase(it);
        else ++it;
    }
    return breaches;
}

} // namespace omega_acct
