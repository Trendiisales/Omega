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

// ---- CrossPosition / IdxOpenPosition archetype -------------------------
// 14 engines hold a PRIVATE position member (VWAPReversion/TrendPullback use
// CrossPosition pos_; IndexFlow/IndexMacroCrash use IdxOpenPosition pos_ or
// scattered base_*_ fields). Because the member is private, each owning class
// exposes public persist_save()/persist_restore() (added S-2026-06-03). This
// template wires any engine that exposes that pair — save emits full state,
// restore routes by tag.
template <class E>
inline void wire_cross(E& eng, const char* tag, const char* sym) {
    g_open_positions.register_persist_source([&eng, tag, sym]() {
        std::vector<omega::PositionSnapshot> out; omega::PositionSnapshot ps;
        if (eng.persist_save(tag, sym, ps)) out.push_back(ps);
        return out;
    });
    g_open_positions.register_restorer([&eng, tag](const omega::PositionSnapshot& ps) -> bool {
        if (ps.engine != tag) return false;
        return eng.persist_restore(ps);
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

    // ---- CrossPosition / IdxOpenPosition archetype (14) ----
    // Globals + symbols + tag strings mirror the GUI register_source() labels in
    // engine_init.hpp exactly, so save/restore keys match the live position keys.
    wire_cross(g_vwap_rev_sp,      "VWAPReversionSP",     "US500.F");
    wire_cross(g_vwap_rev_nq,      "VWAPReversionNQ",     "USTEC.F");
    wire_cross(g_vwap_rev_ger40,   "VWAPReversionGER40",  "GER40");
    wire_cross(g_vwap_rev_eurusd,  "VWAPReversionEURUSD", "EURUSD");
    wire_cross(g_trend_pb_gold,    "TrendPullbackGold",   "XAUUSD");
    wire_cross(g_trend_pb_nq,      "TrendPullbackNQ",     "USTEC.F");
    wire_cross(g_iflow_sp,         "IndexFlowSP",         "US500.F");
    wire_cross(g_iflow_nq,         "IndexFlowNQ",         "USTEC.F");
    wire_cross(g_iflow_nas,        "IndexFlowNAS",        "NAS100");
    wire_cross(g_iflow_us30,       "IndexFlowUS30",       "DJ30.F");
    wire_cross(g_imacro_sp,        "IndexMacroCrashSP",   "US500.F");
    wire_cross(g_imacro_nq,        "IndexMacroCrashNQ",   "USTEC.F");
    wire_cross(g_imacro_nas,       "IndexMacroCrashNAS",  "NAS100");
    wire_cross(g_imacro_us30,      "IndexMacroCrashUS30", "DJ30.F");

    // ---- batch 4: straddles (gold + index, shared XauStraddleM30Engine class)
    // + Ger40KeltnerH1 (the engines wiped on the 2026-06-03 MGC-deploy restart) ----
    wire_cross(g_xau_straddle_m30,      "XauStraddleM30",      "XAUUSD");
    wire_cross(g_xau_straddle_m15,      "XauStraddleM15",      "XAUUSD");
    wire_cross(g_idx_straddle_ger40_m30, "IdxStraddleGER40_M30", "GER40");
    wire_cross(g_idx_straddle_ger40_m15, "IdxStraddleGER40_M15", "GER40");
    wire_cross(g_idx_straddle_nas_m15,   "IdxStraddleNAS100_M15", "NAS100");
    wire_cross(g_idx_straddle_nas_m30,   "IdxStraddleNAS100_M30", "NAS100");
    wire_cross(g_idx_straddle_uk100_m30, "IdxStraddleUK100_M30",  "UK100");
    wire_cross(g_idx_straddle_uk100_m240,"IdxStraddleUK100_M240", "UK100");
    wire_cross(g_ger40_kelt,             "Ger40KeltnerH1",        "GER40");

    // TODO (remaining): multi-cell XauTrendFollow + Ustec arrays (fill-free-cell
    // by tag), C1Retuned cells, gold single-pos publics (Fvg/PDHL/RSIRev/...).
}

} // namespace omega::persist
