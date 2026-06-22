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
#include <fstream>         // S-2026-06-22 self-maintaining caps: ledger rebuild
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <vector>          // self-sufficient for the standalone cap harness
#include <unordered_map>
#include <cstdio>
#include <cstdint>

namespace omega_acct {

// Phase 2 ARMED 2026-06-13 (S-2026-06-13u, operator directive). Enforcement
// force-closes a runaway via g_open_positions.close_matching() -> the engine's
// own force_close (books + clears its slot). Surgical: ledger-proven to cut only
// genuine runaways (2 of 269 closes / 32d), never a normal stop. A breach must
// PERSIST >= ACCT_CONFIRM_SEC before it force-closes (a single tick spike past
// the cap does not trigger).
inline bool g_enforce = true;
inline constexpr int64_t ACCT_CONFIRM_SEC = 5;

// ── SELF-MAINTAINING CAPS (S-2026-06-22, operator mandate: "ensure the cap
// table can never go stale, always viable + available"). ───────────────────
// THREE-TIER, fail-safe by construction -- the change can only ever match-or-
// beat the prior hardcoded behaviour, never regress:
//   (1) DYNAMIC overlay (g_dyn_caps) -- rebuilt IN-PROCESS from the live cleaned
//       ledger every ACCT_REBUILD_THROTTLE_SEC. No external scheduler to fail.
//       An engine's dynamic cap is used ONLY when it has >= ACCT_CAP_MIN_N clean
//       losses AND the build is fresher than ACCT_CAP_TTL_SEC. Stale dynamic is
//       NEVER trusted -> auto-reverts to seed ("can never go stale").
//   (2) SEED map -- the curated 2026-06-13 cleaned-ledger derivation. Compiled
//       in, so it is ALWAYS available (cold start, thin data, file missing).
//   (3) DEFAULT_CAP -- catastrophe insurance for any engine in neither.
// cap_for() ALWAYS returns >= ACCT_CAP_FLOOR for any string ("always available").
// The artifact filter + median + [FLOOR,CEIL] clamp + min-n bound the blast
// radius of residual ledger pollution (see [[omega-tombstone-ledger-pollution]]).
// Python audit twin: tools/derive_acct_caps.py (must match this on same input).
inline constexpr double  ACCT_CAP_FLOOR = 25.0;
inline constexpr double  ACCT_CAP_CEIL  = 800.0;
inline constexpr double  ACCT_CAP_MULT  = 3.0;
inline constexpr int     ACCT_CAP_MIN_N = 20;
inline constexpr int64_t ACCT_CAP_TTL_SEC        = 14 * 86400;   // stale dynamic -> seed
inline constexpr int64_t ACCT_REBUILD_THROTTLE_SEC = 6 * 3600;   // rebuild cadence

inline double DEFAULT_CAP() { return 150.0; }

inline double seed_cap_for(const std::string& eng) {
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
    return DEFAULT_CAP();
}

// Dynamic overlay state (rebuilt in-process; default empty -> pure seed behaviour).
inline std::unordered_map<std::string, double> g_dyn_caps;
inline int64_t     g_caps_built_s    = 0;
inline std::string g_ledger_dir      = "logs/trades";  // cwd-relative; = C:\Omega\logs\trades on VPS

// Quote-aware single-line CSV split (engine/symbol fields are csv_quoted).
inline std::vector<std::string> acct_split_csv(const std::string& line) {
    std::vector<std::string> out; std::string cur; bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == ',' && !q) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// Rebuild g_dyn_caps from the cleaned cumulative close ledger. Fail-safe: on ANY
// problem the prior g_dyn_caps is left untouched (cap_for stays on seed). Mirrors
// tools/derive_acct_caps.py + the ledger_analytics.py:73-89 artifact filter.
inline void rebuild_caps_now(int64_t now_s) {
    namespace fs = std::filesystem;
    std::unordered_map<std::string, std::vector<double>> losses;
    bool read_any = false;
    std::error_code ec;
    if (!fs::exists(g_ledger_dir, ec) || ec) return;          // dir gone -> keep prior
    for (const auto& de : fs::directory_iterator(g_ledger_dir, ec)) {
        if (ec) return;
        const std::string fn = de.path().filename().string();
        if (fn.rfind("omega_trade_closes", 0) != 0) continue;  // must start with prefix
        if (fn.size() < 4 || fn.substr(fn.size() - 4) != ".csv") continue;
        if (fn.find(".bak") != std::string::npos)     continue;
        if (fn.find(".cleared") != std::string::npos) continue;
        std::ifstream f(de.path());
        if (!f.is_open()) continue;
        std::string line;
        if (!std::getline(f, line)) continue;                  // header
        const auto hdr = acct_split_csv(line);
        int ci_eng = -1, ci_net = -1, ci_sym = -1, ci_sz = -1, ci_hold = -1;
        for (int i = 0; i < (int)hdr.size(); ++i) {
            const std::string& h = hdr[i];
            if (h == "engine")   ci_eng = i;
            else if (h == "net_pnl")  ci_net = i;
            else if (h == "symbol")   ci_sym = i;
            else if (h == "size")     ci_sz = i;
            else if (h == "hold_sec") ci_hold = i;
        }
        if (ci_eng < 0 || ci_net < 0) continue;                // schema unknown -> skip file
        read_any = true;
        const int need = std::max({ci_eng, ci_net, ci_sym, ci_sz, ci_hold});
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const auto col = acct_split_csv(line);
            if ((int)col.size() <= need) continue;
            auto num = [&](int i) -> double {
                if (i < 0 || i >= (int)col.size()) return 0.0;
                try { return std::stod(col[i]); } catch (...) { return 0.0; }
            };
            // --- artifact filter (verbatim from ledger_analytics.py:87-88) ---
            if (ci_hold >= 0 && num(ci_hold) > 7.0 * 86400.0) continue;  // phantom hold
            const std::string sym = (ci_sym >= 0 && ci_sym < (int)col.size()) ? col[ci_sym] : "";
            if ((sym == "XAUUSD" || sym == "XAGUSD") && ci_sz >= 0 && num(ci_sz) > 0.05) continue; // 100x lot bug
            const double net = num(ci_net);
            if (net < 0.0) losses[col[ci_eng]].push_back(-net);          // store |loss|
        }
    }
    if (!read_any) return;                                     // nothing parsed -> keep prior
    std::unordered_map<std::string, double> fresh;
    for (auto& kv : losses) {
        auto& v = kv.second;
        if ((int)v.size() < ACCT_CAP_MIN_N) continue;          // thin -> engine keeps seed
        std::sort(v.begin(), v.end());
        const double med = v[v.size() / 2];                    // median |loss|
        double cap = ACCT_CAP_MULT * med;
        if (cap < ACCT_CAP_FLOOR) cap = ACCT_CAP_FLOOR;
        if (cap > ACCT_CAP_CEIL)  cap = ACCT_CAP_CEIL;
        fresh[kv.first] = cap;
    }
    g_dyn_caps.swap(fresh);
    g_caps_built_s = now_s;
    std::printf("[ACCT-GUARD] caps rebuilt: %zu dynamic, %d-loss min, ledger=%s\n",
                g_dyn_caps.size(), ACCT_CAP_MIN_N, g_ledger_dir.c_str());
    std::fflush(stdout);
}

// Throttled rebuild trigger -- safe to call every tick; does real work at most
// once per ACCT_REBUILD_THROTTLE_SEC (and once on first call / cold start).
inline void maybe_rebuild_caps(int64_t now_s) {
    static int64_t s_last = 0;
    if (s_last != 0 && (now_s - s_last) < ACCT_REBUILD_THROTTLE_SEC) return;
    s_last = now_s;
    rebuild_caps_now(now_s);
}

// Per-engine runaway cap. Dynamic if fresh + present, else compiled seed, else
// DEFAULT -- ALWAYS returns a sane value >= ACCT_CAP_FLOOR.
inline double cap_for(const std::string& eng, int64_t now_s) {
    if (g_caps_built_s > 0 && (now_s - g_caps_built_s) <= ACCT_CAP_TTL_SEC) {
        auto it = g_dyn_caps.find(eng);
        if (it != g_dyn_caps.end()) return it->second;
    }
    return seed_cap_for(eng);                                  // never stale: TTL falls through to seed
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
    maybe_rebuild_caps(now_s);   // S-2026-06-22: keep caps fresh in-process (throttled 6h)
    int breaches = 0;
    for (const auto& ps : open) {
        const double unr = unrealised_usd(ps);
        if (unr >= 0.0) continue;
        const double cap = cap_for(ps.engine, now_s);
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
            // S-2026-06-22 (operator-requested): surface the activation + reason in
            // the GUI. g_telemetry writes the shared-memory health_alert[64] banner
            // the Omega dashboard reads ([SYSTEM-ALERT] surface). closed=CUT (runaway
            // killed); !closed=RUNAWAY! (cut needed but no closer wired -- louder).
            char acct_alert[64];
            std::snprintf(acct_alert, sizeof acct_alert, "ACCT-GUARD %s %s loss $%.0f cap $%.0f",
                          closed ? "CUT" : "RUNAWAY!", ps.engine.c_str(), -unr, cap);
            g_telemetry.SetHealthAlert(acct_alert);
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
