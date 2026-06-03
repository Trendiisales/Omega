#pragma once
// ============================================================================
// PositionPersistence.hpp  (S-2026-06-03)
//
// Single authority for open-position persist/resume across restart+deploy.
// Reads FULL position state (incl sl/tp) DIRECTLY from engine internals and
// registers matching restorers — independent of the GUI snapshot sources in
// engine_init.hpp (many of which omit sl/tp) and of any in-flight WIP there.
//
// Mechanism reuses the round-trip-tested OpenPositionRegistry:
//   * register_persist_source(fn)  -> serialize()/save() capture full state
//   * register_restorer(fn)        -> restore() routes each saved line by tag
//
// Coverage is added in archetype batches. This file is the one place new
// engines get wired (a persist-source + a restorer); nothing in engine_init.
//
// MUST be included AFTER globals.hpp in the TU (engine globals + g_open_positions).
// Do NOT #include "globals.hpp" here — it has no include guard (defined-once by
// design); a second include redefines every global (C2086). main.cpp includes
// globals.hpp (line ~98) before this header (~111), so g_* are already visible.
// ============================================================================
#include "OpenPositionRegistry.hpp"
#include <string>
#include <vector>

namespace omega::persist {

// ---- LivePos archetype --------------------------------------------------
// 7 engines share the identical `LivePos pos` struct (public):
//   pos.active / is_long / entry / sl / tp / size / entry_ts (+ mfe/mae).
// One template covers all: emit full state on save, set it back on restore.
template <class E>
inline void wire_livepos(E& eng, const char* tag, const char* sym) {
    g_open_positions.register_persist_source([&eng, tag, sym]() {
        std::vector<omega::PositionSnapshot> out;
        if (eng.pos.active) {
            omega::PositionSnapshot ps;
            ps.engine   = tag;
            ps.symbol   = sym;
            ps.side     = eng.pos.is_long ? "LONG" : "SHORT";
            ps.size     = eng.pos.size;
            ps.entry    = eng.pos.entry;
            ps.sl       = eng.pos.sl;
            ps.tp       = eng.pos.tp;
            ps.entry_ts = eng.pos.entry_ts;
            out.push_back(ps);
        }
        return out;
    });
    g_open_positions.register_restorer([&eng, tag](const omega::PositionSnapshot& ps) -> bool {
        if (ps.engine != tag) return false;
        eng.pos.active   = true;
        eng.pos.is_long  = (ps.side == "LONG");
        eng.pos.entry    = ps.entry;
        eng.pos.sl       = ps.sl;
        eng.pos.tp       = ps.tp;
        eng.pos.size     = ps.size;
        eng.pos.entry_ts = ps.entry_ts;
        eng.pos.mfe      = 0.0;
        eng.pos.mae      = 0.0;
        return true;
    });
}

// Register every engine's persist-source + restorer. Call once at boot, BEFORE
// g_open_positions.restore(). Idempotent engines (adopt won't double an
// already-open slot) keep restore safe to re-run.
inline void register_position_persistence() {
    // ---- SurvivorPortfolio (cell book; source already carries full state) ----
    g_open_positions.register_persist_source([]() {
        std::vector<omega::PositionSnapshot> out;
        for (const auto& c : g_survivor.cells) {
            if (!c.st.pos_active) continue;
            omega::PositionSnapshot ps;
            ps.engine   = c.cfg.tag;
            ps.symbol   = c.cfg.symbol;
            ps.side     = c.st.pos_side > 0 ? "LONG" : "SHORT";
            ps.size     = c.cfg.lot;
            ps.entry    = c.st.pos_entry;
            ps.sl       = c.st.pos_sl;
            ps.tp       = c.st.pos_tp;
            ps.entry_ts = c.st.pos_entry_ts;
            out.push_back(ps);
        }
        return out;
    });
    g_open_positions.register_restorer(
        [](const omega::PositionSnapshot& ps) -> bool { return g_survivor.adopt(ps); });

    // ---- LivePos archetype: scalpers + FX session-open engines (7) ----
    wire_livepos(g_gold_midscalper,     "MidScalperGold",   "XAUUSD");
    wire_livepos(g_gold_microscalper,   "MicroScalperGold", "XAUUSD");
    wire_livepos(g_eurusd_london_open,  "EurusdLondonOpen", "EURUSD");
    wire_livepos(g_usdjpy_asian_open,   "UsdjpyAsianOpen",  "USDJPY");
    wire_livepos(g_gbpusd_london_open,  "GbpusdLondonOpen", "GBPUSD");
    wire_livepos(g_audusd_sydney_open,  "AudusdSydneyOpen", "AUDUSD");
    wire_livepos(g_nzdusd_asian_open,   "NzdusdAsianOpen",  "NZDUSD");

    // TODO (next batches): CrossPosition engines (VWAP/TrendPB/IndexFlow/
    // IndexMacroCrash — private pos_, add class adopt), multi-cell XauTrendFollow
    // + Ustec arrays (fill-free-cell by tag), OCO straddles, C1Retuned cells,
    // gold single-pos publics (Fvg/PDHL/RSIRev/MinimalH4/ThreeBar/EMACross/...).
}

} // namespace omega::persist
