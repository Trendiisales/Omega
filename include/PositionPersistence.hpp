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

// Multi-cell engines: persist_save_all emits one snapshot per active cell tagged
// "<base>#<cellkey>"; restorer routes by the base prefix to the owning engine.
template <class E>
inline void wire_multicell(E& eng, const char* base, const char* sym) {
    g_open_positions.register_persist_source([&eng, base, sym]() {
        std::vector<omega::PositionSnapshot> out; eng.persist_save_all(base, sym, out); return out;
    });
    g_open_positions.register_restorer([&eng, base](const omega::PositionSnapshot& ps) -> bool {
        std::string e = ps.engine; auto h = e.find('#');
        if (h == std::string::npos || e.substr(0, h) != base) return false;
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

    // ---- batch 5: gold/index single-pos engines ----
    wire_cross(g_xauusd_fvg,       "XauusdFvg",        "XAUUSD");
    wire_cross(g_pdhl_rev,         "PDHLReversion",    "XAUUSD");
    wire_cross(g_rsi_reversal,     "RSIReversal",      "XAUUSD");
    wire_cross(g_minimal_h4_gold,  "MinimalH4Gold",    "XAUUSD");
    wire_cross(g_minimal_h4_us30,  "MinimalH4US30",    "DJ30.F");
    wire_cross(g_minimal_h4_ger40, "MinimalH4GER40",   "GER40");
    wire_cross(g_xau_threebar_30m, "XauThreeBar30m",   "XAUUSD");
    wire_cross(g_ema_cross,        "EMACrossGold",     "XAUUSD");
    wire_cross(g_ger40_turtle_h4,  "Ger40TurtleH4",    "GER40");
    wire_cross(g_gold_seasonal,    "GoldSeasonal",     "XAUUSD");
    wire_cross(g_gold_oversold,    "GoldOversoldBounce","XAUUSD");
    wire_cross(g_h1_swing_gold,    "H1SwingGold",      "XAUUSD");
    wire_cross(g_h4_regime_gold,   "H4RegimeGold",     "XAUUSD");

    // ---- batch 5: multi-cell ensembles (per-cell, tag = base#idx/cellid) ----
    wire_multicell(g_xau_tf_1h,    "XauTrendFollow1h",  "XAUUSD");
    wire_multicell(g_xau_tf_2h,    "XauTrendFollow2h",  "XAUUSD");
    wire_multicell(g_xau_tf_4h,    "XauTrendFollow4h",  "XAUUSD");
    wire_multicell(g_xau_tf_d1,    "XauTrendFollowD1",  "XAUUSD");
    wire_multicell(g_ustec_tf_5m,  "UstecTrendFollow5m","USTEC.F");
    wire_multicell(g_ustec_tf_htf, "UstecTrendFollowHtf","USTEC.F");
    wire_multicell(g_c1_retuned,   "C1Retuned",         "XAUUSD");

    // ---- batch 6: tail — Breakout FX, NBM, FX turtles/scalp, pyramided (base) ----
    wire_cross(g_eng_eurusd,  "BreakoutEURUSD", "EURUSD");
    wire_cross(g_eng_gbpusd,  "BreakoutGBPUSD", "GBPUSD");
    wire_cross(g_eng_audusd,  "BreakoutAUDUSD", "AUDUSD");
    wire_cross(g_eng_nzdusd,  "BreakoutNZDUSD", "NZDUSD");
    wire_cross(g_eng_usdjpy,  "BreakoutUSDJPY", "USDJPY");
    wire_cross(g_nbm_gold_london, "NoiseBandMomentumGoldLdn", "XAUUSD");
    wire_cross(g_gold_scalp_pyramid, "GoldScalpPyramid", "XAUUSD");
    wire_cross(g_gold_regime_daily,  "GoldRegimeDaily",  "XAUUSD");
    wire_cross(g_macro_crash,        "MacroCrash",       "XAUUSD");
    wire_cross(g_eurusd_turtle_h4, "FxTurtleH4_EURUSD", "EURUSD");
    wire_cross(g_gbpusd_turtle_h4, "FxTurtleH4_GBPUSD", "GBPUSD");
    wire_cross(g_audusd_turtle_h4, "FxTurtleH4_AUDUSD", "AUDUSD");
    wire_cross(g_nzdusd_turtle_h4, "FxTurtleH4_NZDUSD", "NZDUSD");
    wire_cross(g_usdjpy_turtle_h4, "FxTurtleH4_USDJPY", "USDJPY");
    wire_cross(g_fx_scalp_eurusd, "FxScalpPyramid_EURUSD", "EURUSD");
    wire_cross(g_fx_scalp_usdjpy, "FxScalpPyramid_USDJPY", "USDJPY");
    wire_cross(g_fx_scalp_gbpusd, "FxScalpPyramid_GBPUSD", "GBPUSD");
    wire_cross(g_fx_scalp_usdcad, "FxScalpPyramid_USDCAD", "USDCAD");
    wire_cross(g_fx_scalp_audusd, "FxScalpPyramid_AUDUSD", "AUDUSD");
    // ---- IndexSession (intraday cash-session long, 5 indices) ----
    wire_cross(g_idxsess_sp,     "IndexSession_SP",     "US500.F");
    wire_cross(g_idxsess_nas,    "IndexSession_NAS",    "NAS100");
    wire_cross(g_fvgcont_nas,    "FvgContinuation",     "NAS100");   // 2026-06-04 FVG continuation (long+short)
    wire_cross(g_idxsess_ger40,  "IndexSession_GER40",  "GER40");
    wire_cross(g_idxsess_uk100,  "IndexSession_UK100",  "UK100");
    wire_cross(g_idxsess_estx50, "IndexSession_ESTX50", "ESTX50");

    // Coverage now: every position-holding engine on the dashboard persists/resumes.
    // Mandate: any NEW engine MUST add persist_save/persist_restore + a wire here
    // (like the warm-seed mandate). Partial persistence = silent position loss.
}

} // namespace omega::persist
