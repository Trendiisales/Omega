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
#include "BigCapMomoIbkr.hpp"   // omega::bigcap_momo_ibkr::{is_enabled,collect_positions,restore_position} (pure-std)
#include <string>
#include <vector>
#include <type_traits>
#include <utility>
#include <ctime>   // time() for the Survivor cell closer (S-2026-06-30)

namespace omega::persist {

// Detect a public `bool enabled` member at compile time. A DISABLED engine must
// never resurrect a stale persisted position — it would be restored then
// force-closed on every restart, booking phantom trades for an engine that is
// off (root cause of the IndexSession/GoldOversold phantom class, 2026-06-05).
// `if constexpr (has_enabled<E>::value)` guards only engines that expose the
// member; engines without it compile fine and simply skip the check.
template <class T, class = void> struct has_enabled : std::false_type {};
template <class T>
struct has_enabled<T, std::void_t<decltype(bool(std::declval<const T&>().enabled))>>
    : std::true_type {};
template <class E> inline bool restore_blocked_disabled(const E& eng) {
    if constexpr (has_enabled<E>::value) return !eng.enabled;
    else return false;
}

// ---- AccountingGuard Phase 2 enforcement helpers (S-2026-06-13u) --------
// A closer force-closes a runaway position through the engine's OWN force_close
// (which books via on_trade_record = handle_closed_trade AND clears the engine's
// internal slot -> no double-book) at the current book price.
inline bool acct_book_px(const char* sym, double& b, double& a) {
    std::lock_guard<std::mutex> lk(g_book_mtx);
    auto bi = g_bids.find(sym), ai = g_asks.find(sym);
    if (bi == g_bids.end() || ai == g_asks.end() || bi->second <= 0.0 || ai->second <= 0.0)
        return false;
    b = bi->second; a = ai->second; return true;
}
inline void acct_book_cb(const omega::TradeRecord& tr) { handle_closed_trade(tr); }

// Is this engine holding an open position? (open-accessor varies across engines)
template <class E>
inline bool acct_has_open(E& eng) {
    if      constexpr (requires { eng.has_open_position(); }) return eng.has_open_position();
    else if constexpr (requires { eng.pos.active; })          return eng.pos.active;
    else if constexpr (requires { eng.any_open(); })          return eng.any_open();
    else return false;
}
// Force-close via whichever force_close signature this engine exposes. C++20
// requires-dispatch keeps ONE generic closer correct across the engine zoo:
//   (b,a,cb,reason)      cross-asset / straddle / breakout
//   (b,a,now_ms,cb)      LivePos scalpers + FX session opens
// Returns false (engine left open -> guard logs UNENFORCEABLE) when neither
// matches (e.g. IndexFlow's (b,a,regime,cb&) or multicell force_close_all).
template <class E>
inline bool acct_try_close(E& eng, const char* sym, const char* reason) {
    if (!acct_has_open(eng)) return false;
    double b, a; if (!acct_book_px(sym, b, a)) return false;
    if      constexpr (requires { eng.force_close(b, a, acct_book_cb, reason); }) {
        eng.force_close(b, a, acct_book_cb, reason); return true;
    } else if constexpr (requires { eng.force_close(b, a, (int64_t)0, acct_book_cb); }) {
        eng.force_close(b, a, (int64_t)0, acct_book_cb); return true;
    } else return false;
}

// ---- LivePos archetype --------------------------------------------------
// 7 engines share the identical `LivePos pos` struct (public):
//   pos.active / is_long / entry / sl / tp / size / entry_ts (+ mfe/mae).
// One template covers all: emit full state on save, set it back on restore.
template <class E>
inline void wire_livepos(E& eng, const char* tag, const char* sym) {
    g_open_positions.register_closer(
        [&eng, tag, sym](const omega::PositionSnapshot& ps, const char* reason) -> bool {
            if (ps.engine != tag) return false;
            return acct_try_close(eng, sym, reason);
        });
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
        if (restore_blocked_disabled(eng)) return false;   // disabled engine -> no resurrect
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
    g_open_positions.register_closer(
        [&eng, tag, sym](const omega::PositionSnapshot& ps, const char* reason) -> bool {
            if (ps.engine != tag) return false;
            return acct_try_close(eng, sym, reason);
        });
    g_open_positions.register_persist_source([&eng, tag, sym]() {
        std::vector<omega::PositionSnapshot> out; omega::PositionSnapshot ps;
        if (eng.persist_save(tag, sym, ps)) out.push_back(ps);
        return out;
    });
    g_open_positions.register_restorer([&eng, tag](const omega::PositionSnapshot& ps) -> bool {
        if (ps.engine != tag) return false;
        if (restore_blocked_disabled(eng)) return false;   // disabled engine -> no resurrect
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
        if (restore_blocked_disabled(eng)) return false;   // disabled engine -> no resurrect
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
    // Closer (S-2026-06-30): Survivor cells had a persist-source + restorer but NO
    // closer -> close_matching() (and thus the KILL ALL panic flatten) could not
    // flatten a cell, so a cell position (e.g. XAU_4h_DonchN20) "sat there" with no
    // manual close path. Match ps.engine -> cell.cfg.tag, book the close at the live
    // mid via the cell's own exit path (emits a TradeRecord + clears the slot).
    g_open_positions.register_closer(
        [](const omega::PositionSnapshot& ps, const char* reason) -> bool {
            for (auto& c : g_survivor.cells) {
                if (!c.st.pos_active || ps.engine != c.cfg.tag) continue;
                double b, a; if (!acct_book_px(c.cfg.symbol, b, a)) return false;
                c.force_close_dup(b, a, (int64_t)time(nullptr), acct_book_cb, reason);
                return true;
            }
            return false;
        });

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
    wire_cross(g_idx_straddle_ger40_m30, "IdxStraddleGER40_M30", "GER40");
    wire_cross(g_idx_straddle_ger40_m15, "IdxStraddleGER40_M15", "GER40");
    wire_cross(g_idx_straddle_nas_m15,   "IdxStraddleNAS100_M15", "NAS100");
    wire_cross(g_idx_straddle_nas_m30,   "IdxStraddleNAS100_M30", "NAS100");
    wire_cross(g_idx_straddle_uk100_m30, "IdxStraddleUK100_M30",  "UK100");
    wire_cross(g_idx_straddle_uk100_m240,"IdxStraddleUK100_M240", "UK100");

    // ---- batch 5: gold/index single-pos engines ----
    wire_cross(g_xauusd_fvg,       "XauusdFvg",        "XAUUSD");
    wire_cross(g_xau_threebar_30m, "XauThreeBar30m",   "XAUUSD");
    wire_cross(g_ema_cross,        "EMACrossGold",     "XAUUSD");
    wire_cross(g_h1_swing_gold,    "H1SwingGold",      "XAUUSD");

    // ---- batch 5: multi-cell ensembles (per-cell, tag = base#idx/cellid) ----
    wire_multicell(g_xau_tf_1h,    "XauTrendFollow1h",  "XAUUSD");
    wire_multicell(g_xau_tf_2h,    "XauTrendFollow2h",  "XAUUSD");
    wire_multicell(g_xau_tf_4h,    "XauTrendFollow4h",  "XAUUSD");
    wire_multicell(g_xau_tf_d1,    "XauTrendFollowD1",  "XAUUSD");
    // S-2026-07-08c: MGC-venue XauTF instances (pre-live requirement). Decls moved
    // to globals.hpp to beat this header in main.cpp include order; without this a
    // restart dropped the open MGC leg + the boot replay re-booked closed trades.
    wire_multicell(g_mgc_tf_4h,    "MgcTF4h",           "MGC");
    wire_multicell(g_mgc_tf_2h,    "MgcTF2h",           "MGC");

    // ---- batch 6: tail — Breakout FX, NBM, FX turtles/scalp, pyramided (base) ----
    wire_cross(g_eng_eurusd,  "BreakoutEURUSD", "EURUSD");
    wire_cross(g_eng_gbpusd,  "BreakoutGBPUSD", "GBPUSD");
    wire_cross(g_eng_audusd,  "BreakoutAUDUSD", "AUDUSD");
    wire_cross(g_eng_nzdusd,  "BreakoutNZDUSD", "NZDUSD");
    wire_cross(g_eng_usdjpy,  "BreakoutUSDJPY", "USDJPY");
    // ---- IndexSession (intraday cash-session long, 5 indices) ----
    wire_cross(g_idxsess_sp,     "IndexSession_SP",     "US500.F");
    wire_cross(g_idxsess_nas,    "IndexSession_NAS",    "NAS100");
    wire_cross(g_idxsess_ger40,  "IndexSession_GER40",  "GER40");
    wire_cross(g_idxsess_uk100,  "IndexSession_UK100",  "UK100");
    wire_cross(g_idxsess_estx50, "IndexSession_ESTX50", "ESTX50");

    // ---- BigCapMomo / PumpScalp (multi-symbol cell managers: Cons + optional GB A/B) ----
    // S-2026-06-26: enabled but unpersisted -> per-symbol fills vanished on restart. Now wired per-cell
    // (tag "<base>#<symbol>"); restore_blocked_disabled gates the GB instance when OMEGA_BIGCAP_AB is off.
    wire_multicell(g_bigcap_momo,   "BigCapMomoCons", "");
    wire_multicell(g_bigcap_momo_b, "BigCapMomoGB",   "");

    // ---- D1 calendar/seasonal index book (S-2026-07-08): CalendarTom x6 +
    // IndexSeasonal x6 + IndexFomc x3 + CrossSectional x3. None persisted since
    // their 06-21 ship while restarts ran near-daily -> every restart orphaned
    // the open leg and the calendar/hold exit fired on nothing. CalendarTom
    // (~6-trading-day TOM hold) ledgered ZERO round-trips as a result; seasonal
    // (1 bar + weekend), FOMC (1 bar overnight) and XS (3-20 bar legs) are the
    // same class. Tags = the engines' ledger names.
    wire_cross(g_tom_us500,  "CalendarTom_US500.F", "US500.F");
    wire_cross(g_tom_ustec,  "CalendarTom_USTEC.F", "USTEC.F");
    wire_cross(g_tom_ger40,  "CalendarTom_GER40",   "GER40");
    wire_cross(g_tom_dj30,   "CalendarTom_DJ30.F",  "DJ30.F");
    wire_cross(g_tom_uk100,  "CalendarTom_UK100",   "UK100");
    wire_cross(g_tom_xau,    "CalendarTom_XAUUSD",  "XAUUSD");
    // S-2026-07-08c gold deep-dive engines. GoldTsmomD1V2 carries a signed WEIGHT
    // across ~monthly periods (snapshot: side+size=|w|*lot, entry=period anchor);
    // unpersisted, every restart would orphan the weight and the period close
    // would phantom-drop. MgcSlowDon holds multi-day Donchian rides on the MGC
    // feed (decl in globals.hpp to beat this header in include order).
    wire_cross(g_gold_tsmom_d1, "GoldTsmomD1V2",      "XAUUSD");
    wire_cross(g_mgc_slowdon,   "MgcSlowDonchian30m", "MGC");
    // S-2026-07-11 GOLD PHASE 1b: BOTH GoldVolBreakoutM30 instances. The spot
    // instance was LIVE (S-2026-07-01 cutover) yet still sat on the persistence
    // audit's "dormant" allowlist -- a stale exemption hiding a real gap; the
    // MGC instance (decl moved to globals.hpp) was the Phase-1 known residual.
    // Runner holds span days (MAX_HOLD 72 x M30 = 36h) -- restart-orphan real.
    wire_cross(g_gold_volbrk_m30, "GoldVolBreakoutM30", "XAUUSD");
    wire_cross(g_mgc_volbrk,      "MgcVolBreakoutM30",  "MGC");
    // S-2026-07-11 PHASE 1b: gaps found when the persistence audit was re-enabled
    // (it had sat BEHIND an unreachable `exit 0` in mac_canary_engines.sh, so the
    // "ENFORCED" claim below was dead code and these display engines accumulated
    // unpersisted). ConnorsRSI2/SpxTurtleD1 already had persist_save/restore
    // members (S-2026-07-07v added their displays believing the wire existed);
    // GoldPanicBounce gained the pair this commit. QndxSqf is allowlisted in the
    // audit instead (signal-state book: legs re-derive position from the daily
    // CSV replay + on-connect adopt -- self-restoring by construction).
    wire_cross(g_connors_nas,      "ConnorsRSI2",     "NAS100");
    wire_cross(g_spx_turtle_d1,    "SpxTurtleD1",     "US500.F");
    wire_cross(g_gold_panic_bounce,"GoldPanicBounce", "XAUUSD");
    wire_cross(g_idx_seas_us500,  "IndexSeasonal_US500.F", "US500.F");
    wire_cross(g_idx_seas_ustec,  "IndexSeasonal_USTEC.F", "USTEC.F");
    wire_cross(g_idx_seas_ger40,  "IndexSeasonal_GER40",   "GER40");
    wire_cross(g_idx_seas_dj30,   "IndexSeasonal_DJ30.F",  "DJ30.F");
    wire_cross(g_idx_seas_uk100,  "IndexSeasonal_UK100",   "UK100");
    wire_cross(g_idx_seas_estx50, "IndexSeasonal_ESTX50",  "ESTX50");
    wire_cross(g_idx_fomc_us500,  "IndexFomc_US500.F", "US500.F");
    wire_cross(g_idx_fomc_ustec,  "IndexFomc_USTEC.F", "USTEC.F");
    wire_cross(g_idx_fomc_dj30,   "IndexFomc_DJ30.F",  "DJ30.F");
    wire_multicell(g_xs_mom_long, "XsIndex_MomLong", "");
    wire_multicell(g_xs_mom_ls,   "XsIndex_MomLS",   "");
    wire_multicell(g_xs_mr_ls,    "XsIndex_MrLS",    "");

    // ---- IndexBearShort (risk-off SHORT, NAS100 + US500; real-engine PF1.59-1.60) ----
    // S-2026-06-26: showed positions but did NOT persist -> the 2 live SHORTs vanished with no ledger
    // close on restart (operator-reported). Now wired (persist_save/restore added to the engine).
    wire_cross(g_idx_bear_short_nas, "IndexBearShort",   "NAS100");
    wire_cross(g_idx_bear_short_sp,  "IndexBearShortSP", "US500.F");

    // ---- BigCapMomoIbkr / Luke (in-process IBKR engine; own Sym book, not a g_* global) ----
    // S-2026-06-26: this engine showed positions (register_source "BigCapMomoIbkr") but did NOT persist
    // -> Luke/surge fills vanished with no ledger close on every restart/deploy. collect_positions()
    // already carries full state (entry/sl/size); restore_position() re-adopts at boot. is_enabled()
    // gates both so a disabled engine can't resurrect a stale slot.
    g_open_positions.register_persist_source([]() {
        return omega::bigcap_momo_ibkr::is_enabled() ? omega::bigcap_momo_ibkr::collect_positions()
                                                      : std::vector<omega::PositionSnapshot>{};
    });
    g_open_positions.register_restorer([](const omega::PositionSnapshot& ps) -> bool {
        return omega::bigcap_momo_ibkr::restore_position(ps);
    });

    // (BeCascade/BrkCascade/XauUpJump multicell wires removed S-2026-07-13 code cull — families deleted.)

    // Coverage now: every position-holding engine on the dashboard persists/resumes.
    // Mandate: any NEW engine MUST add persist_save/persist_restore + a wire here
    // (like the warm-seed mandate). Partial persistence = silent position loss.
    // ENFORCED: scripts/persistence_audit.sh fails the build if a display source lacks a persist source.
}

} // namespace omega::persist
