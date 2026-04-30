// =============================================================================
//  TsmomStrategy.hpp -- Phase 2 of CellEngine refactor (plan §3.5).
//
//  Status: Phase 2 -- ADDITIVE ONLY. No production paths touched.
//
//  Implements the CellStrategy concept (CellEngine.hpp) for the 5 long
//  TSMOM cells (H1/H2/H4/H6/D1 long XAUUSD). Logic mirrors
//  phase1/signal_discovery/post_cut_revalidate_all.py::sig_tsmom and the
//  shipped TsmomCell in TsmomEngine.hpp byte-for-byte at
//  max_positions_per_cell=1.
//
//  Signal:
//      ret_n = close[t] - close[t - lookback]
//      side  = +1 if ret_n > 0, -1 if ret_n < 0, 0 otherwise
//
//  Exit:
//      Hard SL: entry - direction * hard_sl_atr * ATR14_at_signal
//      Time exit: p.bars_held >= hold_bars (no TP)
//
//  Plan §7.3 hybrid sizing: a per-cell `risk_pct_override` /
//  `max_lot_cap_override` of 0.0 means "use portfolio default"; positive
//  values override. Tsmom defaults are 0.005 / 0.05 -- matching production.
//
//  Phase 2a check: with max_positions_per_cell=1, V2 ledger MUST equal V1
//  ledger byte-for-byte over the 1-year tsmom_warmup_H1 corpus. Any
//  divergence is a refactor bug, not a policy change.
// =============================================================================
#pragma once

#include <cmath>
#include <cstddef>
#include <deque>

#include "CellEngine.hpp"
#include "CellPrimitives.hpp"

namespace omega::cell {

struct TsmomStrategy {
    // -----------------------------------------------------------------------
    //  Per-cell config. Defaults match the shipped TsmomCell.
    //
    //  risk_pct_override / max_lot_cap_override are plan §7.3 hybrid hooks:
    //  0.0 = use portfolio default. Strategy::sizing() returns the resolved
    //  pair. Phase 2a does not exercise the override path -- portfolio
    //  values still drive sizing via CellPortfolio.size_for() -- but the
    //  hook is in place for §7.3 rollout in a later session.
    // -----------------------------------------------------------------------
    struct Config {
        int    lookback             = 20;
        int    hold_bars            = 12;
        double hard_sl_atr          = 3.0;
        double risk_pct_override    = 0.0;   // 0 = use portfolio default
        double max_lot_cap_override = 0.0;   // 0 = use portfolio default
    };

    // Code defaults (visible, code-reviewed, never amplified by config).
    static constexpr double DEFAULT_RISK_PCT    = 0.005;
    static constexpr double DEFAULT_MAX_LOT_CAP = 0.05;

    // -----------------------------------------------------------------------
    //  Per-cell running state. Just the rolling closes window.
    //  Capacity = lookback + 1 (we read closes_[size - 1 - lookback]).
    // -----------------------------------------------------------------------
    struct State {
        std::deque<double> closes_;
    };

    // -----------------------------------------------------------------------
    //  on_bar_update -- push the new close into the rolling window and
    //  trim from the front so size <= lookback + 1.
    //
    //  Mirrors TsmomCell::on_bar lines 258-260 exactly.
    // -----------------------------------------------------------------------
    static void on_bar_update(State& s, const Bar& b, const Config& cfg) noexcept {
        s.closes_.push_back(b.close);
        const std::size_t cap = static_cast<std::size_t>(cfg.lookback) + 1;
        while (s.closes_.size() > cap) s.closes_.pop_front();
    }

    // -----------------------------------------------------------------------
    //  evaluate -- TSMOM signal: sign of (close[t] - close[t - lookback]).
    //  Returns +1 / -1 / 0. Required-direction filtering happens in CellBase.
    //
    //  Mirrors TsmomCell::on_bar lines 269, 277-285 exactly.
    // -----------------------------------------------------------------------
    static int evaluate(const State& s, const Config& cfg, int /*direction*/) noexcept {
        if (static_cast<int>(s.closes_.size()) < cfg.lookback + 1) return 0;
        const double cur     = s.closes_.back();
        const double earlier = s.closes_[s.closes_.size() - 1 - static_cast<std::size_t>(cfg.lookback)];
        const double ret_n   = cur - earlier;
        if (ret_n > 0.0) return +1;
        if (ret_n < 0.0) return -1;
        return 0;
    }

    // -----------------------------------------------------------------------
    //  sl_distance_pts -- price-point SL distance for sizing.
    //  Used by CellPortfolio.size_for() to compute lot.
    //
    //  Mirrors TsmomPortfolio._drive_cell line `sl_pts = atr14 * cell.hard_sl_atr`.
    // -----------------------------------------------------------------------
    static double sl_distance_pts(double atr14, const Config& cfg) noexcept {
        return atr14 * cfg.hard_sl_atr;
    }

    // -----------------------------------------------------------------------
    //  initial_sl -- absolute SL price at entry.
    //
    //  Mirrors TsmomCell::on_bar lines 289-290:
    //      sl_pts = atr * hard_sl_atr; sl_px = entry - direction * sl_pts;
    // -----------------------------------------------------------------------
    static double initial_sl(double entry, int direction,
                             double atr14, const Config& cfg) noexcept {
        return entry - static_cast<double>(direction) * (atr14 * cfg.hard_sl_atr);
    }

    // -----------------------------------------------------------------------
    //  initial_tp -- TSMOM has no TP (time-exit only).
    // -----------------------------------------------------------------------
    static double initial_tp(double /*entry*/, int /*direction*/,
                             double /*atr14*/, const Config& /*cfg*/) noexcept {
        return 0.0;
    }

    // -----------------------------------------------------------------------
    //  on_tick_manage -- TSMOM does no per-tick adjustment.
    // -----------------------------------------------------------------------
    static bool on_tick_manage(Position& /*p*/, double /*bid*/, double /*ask*/,
                               const State& /*s*/, const Config& /*cfg*/) noexcept {
        return false;
    }

    // -----------------------------------------------------------------------
    //  time_exit -- bars_held >= hold_bars (default 12).
    //
    //  Mirrors TsmomCell::on_bar lines 249-253 exactly.
    // -----------------------------------------------------------------------
    static bool time_exit(const Position& p, const Config& cfg) noexcept {
        return p.bars_held >= cfg.hold_bars;
    }

    // -----------------------------------------------------------------------
    //  sizing -- plan §7.3 hybrid: code defaults, optional config overrides.
    //
    //  Phase 2a does NOT consult this hook (CellPortfolio uses
    //  portfolio-level risk_pct / max_lot_cap to match V1's
    //  TsmomPortfolio.size_for behaviour exactly). Wired for §7.3 rollout.
    // -----------------------------------------------------------------------
    static SizingParams sizing(const Config& cfg) noexcept {
        return SizingParams{
            (cfg.risk_pct_override    > 0.0) ? cfg.risk_pct_override    : DEFAULT_RISK_PCT,
            (cfg.max_lot_cap_override > 0.0) ? cfg.max_lot_cap_override : DEFAULT_MAX_LOT_CAP,
        };
    }
};

// Concept satisfaction at instantiation point. Build will fail here with a
// readable diagnostic if any hook above drifts from the contract.
static_assert(CellStrategy<TsmomStrategy>,
              "TsmomStrategy must satisfy the CellStrategy concept");

// =============================================================================
//  TsmomPortfolioV2 -- generic portfolio specialised on TsmomStrategy.
//  Drop-in shadow target for the legacy TsmomPortfolio in TsmomEngine.hpp.
// =============================================================================
using TsmomPortfolioV2 = CellPortfolio<TsmomStrategy>;

// -----------------------------------------------------------------------------
//  build_default_tsmom_topology -- registers the 5 long TSMOM cells
//  (H1/H2/H4/H6/D1 long) with the same Config defaults TsmomPortfolio uses
//  in production. Call AFTER setting portfolio-level config but BEFORE
//  init() / warmup_from_csv().
// -----------------------------------------------------------------------------
inline void build_default_tsmom_topology(TsmomPortfolioV2& port) {
    using Spec = TsmomPortfolioV2::CellSpec;
    TsmomStrategy::Config cfg{};   // lookback=20, hold_bars=12, hard_sl_atr=3.0

    port.add_cell(Spec{TsmomPortfolioV2::TF_H1, +1, "Tsmom_H1_long", cfg});
    port.add_cell(Spec{TsmomPortfolioV2::TF_H2, +1, "Tsmom_H2_long", cfg});
    port.add_cell(Spec{TsmomPortfolioV2::TF_H4, +1, "Tsmom_H4_long", cfg});
    port.add_cell(Spec{TsmomPortfolioV2::TF_H6, +1, "Tsmom_H6_long", cfg});
    port.add_cell(Spec{TsmomPortfolioV2::TF_D1, +1, "Tsmom_D1_long", cfg});
}

}  // namespace omega::cell
