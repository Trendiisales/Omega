// =============================================================================
//  CellEngine.hpp -- Phase 2 of CellEngine refactor.
//
//  Source of truth: docs/CELL_ENGINE_REFACTOR_PLAN.md (APPROVED 2026-04-30).
//  Status:          Phase 2 -- ADDITIVE ONLY. No production paths touched.
//
//  Defines the generic, header-only template:
//      template <CellStrategy Strategy>
//      struct CellBase      -- single cell, multi-position, per-strategy
//      struct CellPortfolio -- 5 cells (H1/H2/H4/H6/D1), drives bar-synth
//                              + ATR per timeframe, runs warmup CSV.
//
//  Compile-time contract is the C++20 `concept CellStrategy` declared below
//  per plan §7.2. Missing or wrong-typed strategy methods produce a clean
//  diagnostic at instantiation time instead of a 200-line template trace.
//
//  Phase 2a goal: a CellBase<TsmomStrategy> with max_positions_per_cell=1
//  must produce byte-identical TradeRecord output to TsmomCell at max=1
//  for at least 5 trading days. The structural ordering inside on_bar
//  MIRRORS TsmomCell::on_bar exactly:
//      1) ++bar_count_
//      2) iterate positions:
//          a) ++p.bars_held
//          b) update MFE/MAE from bar high/low
//          c) SL fill check on bar high/low
//          d) (after SL) Strategy::time_exit hook
//      3) Strategy::on_bar_update (e.g. push close into rolling window)
//      4) decrement cooldown_left_
//      5) pre-fire gates (enabled, n_open<max, cooldown==0,
//         atr finite>0, spread finite>=0, spread<=max_spread, size>0)
//      6) Strategy::evaluate -> sig_dir; require sig_dir == direction
//      7) open Position via Strategy::initial_sl + Strategy::initial_tp
//
//  Any deviation from this ordering invalidates the byte-identical check
//  and must be justified in the plan before being merged.
//
//  Logging: CellBase does NOT printf. The OnCloseCb (TradeRecord) is the
//  ground truth for shadow comparison. Production deployment can wrap the
//  OnCloseCb with a printf-emitting decorator if it wants matching log
//  spam to TsmomCell. The backtest harness compares the CSV-serialised
//  TradeRecord stream, not stdout.
//
//  Standard required: C++20 (concept, designated init).
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "CellPrimitives.hpp"
#include "OmegaTradeLedger.hpp"

namespace omega::cell {

// =============================================================================
//  CellStrategy concept (plan §7.2).
//
//  A Strategy is a struct exposing:
//      Config        nested type -- per-cell knobs
//      State         nested type -- running indicator state
//      on_bar_update(state, bar, cfg)          -> void
//      evaluate(state, cfg, direction)         -> int   (+1 / -1 / 0)
//      sl_distance_pts(atr14, cfg)             -> double  (sizing input)
//      initial_sl(entry, dir, atr14, cfg)      -> double  (absolute price)
//      initial_tp(entry, dir, atr14, cfg)      -> double  (0.0 = no TP)
//      on_tick_manage(p, bid, ask, state, cfg) -> bool    (true => close)
//      time_exit(p, cfg)                       -> bool
//      sizing(cfg)                             -> SizingParams
//
//  sl_distance_pts is a small extension of plan §3.5: the portfolio needs
//  the SL distance in price points BEFORE opening the position (to compute
//  the position size). Deriving it from initial_sl(entry=0, ...) works only
//  for strategies whose SL is "entry - dir * f(atr, cfg)". A direct hook
//  is unambiguous and adds one line per strategy.
// =============================================================================
template <typename S>
concept CellStrategy = requires(typename S::State&         state,
                                const typename S::State&   cstate,
                                const typename S::Config&  cfg,
                                const Bar&                 bar,
                                Position&                  pos,
                                const Position&            cpos,
                                double bid, double ask, double atr14,
                                double entry,
                                int    direction)
{
    typename S::Config;
    typename S::State;

    { S::on_bar_update(state, bar, cfg)              } -> std::same_as<void>;
    { S::evaluate(cstate, cfg, direction)            } -> std::same_as<int>;
    { S::sl_distance_pts(atr14, cfg)                 } -> std::same_as<double>;
    { S::initial_sl(entry, direction, atr14, cfg)    } -> std::same_as<double>;
    { S::initial_tp(entry, direction, atr14, cfg)    } -> std::same_as<double>;
    { S::on_tick_manage(pos, bid, ask, cstate, cfg)  } -> std::same_as<bool>;
    { S::time_exit(cpos, cfg)                        } -> std::same_as<bool>;
    { S::sizing(cfg)                                 } -> std::same_as<SizingParams>;
};

// =============================================================================
//  CellBase<Strategy> -- one cell, multi-position, byte-compatible with
//  TsmomCell at max_positions_per_cell=1 when wrapped by TsmomStrategy.
// =============================================================================
template <CellStrategy Strategy>
struct CellBase {
    using OnCloseCb = std::function<void(const ::omega::TradeRecord&)>;
    using Cfg       = typename Strategy::Config;
    using State     = typename Strategy::State;

    // ---- Identity ----------------------------------------------------------
    int         direction              = 1;
    std::string timeframe              = "H1";
    std::string symbol                 = "XAUUSD";
    std::string cell_id                = "Cell_H1_long";

    // ---- Common config -----------------------------------------------------
    bool        shadow_mode            = true;
    bool        enabled                = true;
    int         max_positions_per_cell = 10;
    int         cooldown_bars          = 0;
    double      max_spread_pt          = 1.5;
    std::string regime_label           = "CELL";

    // ---- Strategy config + state ------------------------------------------
    Cfg         cfg{};
    State       state{};

    // ---- Multi-position state ---------------------------------------------
    std::vector<Position> positions_;
    int     trade_id_      = 0;
    int     bar_count_     = 0;
    int     cooldown_left_ = 0;

    bool has_open_position() const noexcept { return !positions_.empty(); }
    int  n_open()             const noexcept { return static_cast<int>(positions_.size()); }

    // -----------------------------------------------------------------------
    //  on_bar -- bar-driven dispatch. Mirrors TsmomCell::on_bar exactly.
    //
    //  Returns 1 if a NEW position opened this bar, else 0.
    // -----------------------------------------------------------------------
    int on_bar(const Bar&    b,
               double        bid,
               double        ask,
               double        atr14_at_signal,
               int64_t       now_ms,
               double        size_lot,
               OnCloseCb     on_close) noexcept
    {
        ++bar_count_;

        // ----- 1. Manage open positions (in-place, removing closed) --------
        for (auto it = positions_.begin(); it != positions_.end(); ) {
            Position& p = *it;
            ++p.bars_held;

            const double max_move_this_bar = (direction == 1)
                ? (b.high - p.entry) : (p.entry - b.low);
            const double min_move_this_bar = (direction == 1)
                ? (b.low  - p.entry) : (p.entry - b.high);
            if (max_move_this_bar > p.mfe) p.mfe = max_move_this_bar;
            if (min_move_this_bar < p.mae) p.mae = min_move_this_bar;

            const bool sl_hit = (direction == 1)
                ? (b.low  <= p.sl) : (b.high >= p.sl);
            if (sl_hit) {
                _close(p, p.sl, "SL_HIT", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }
            if (Strategy::time_exit(p, cfg)) {
                _close(p, b.close, "TIME_EXIT", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }
            ++it;
        }

        // ----- 2. Strategy state update (e.g. push close into rolling win) -
        Strategy::on_bar_update(state, b, cfg);

        // ----- 3. Decrement cooldown (legacy throttle; default 0) ----------
        if (cooldown_left_ > 0) --cooldown_left_;

        // ----- 4. Pre-fire gates -------------------------------------------
        if (!enabled)                                                   return 0;
        if (n_open() >= max_positions_per_cell)                         return 0;
        if (cooldown_left_ > 0)                                         return 0;
        if (!std::isfinite(atr14_at_signal) || atr14_at_signal <= 0.0)  return 0;
        const double spread_pt = ask - bid;
        if (!std::isfinite(spread_pt) || spread_pt < 0.0)               return 0;
        if (spread_pt > max_spread_pt)                                  return 0;
        if (size_lot <= 0.0)                                            return 0;

        // ----- 5. Strategy entry signal ------------------------------------
        const int sig_dir = Strategy::evaluate(state, cfg, direction);
        if (sig_dir == 0)             return 0;
        if (sig_dir != direction)     return 0;

        // ----- 6. Open the position ----------------------------------------
        const double entry_px  = (direction == 1) ? ask : bid;
        const double sl_px     = Strategy::initial_sl(entry_px, direction, atr14_at_signal, cfg);
        const double tp_px     = Strategy::initial_tp(entry_px, direction, atr14_at_signal, cfg);

        Position p;
        p.entry      = entry_px;
        p.sl         = sl_px;
        p.initial_sl = sl_px;
        p.tp         = tp_px;
        p.size       = size_lot;
        p.atr        = atr14_at_signal;
        p.entry_ms   = now_ms;
        p.bars_held  = 0;
        p.mfe        = 0.0;
        p.mae        = 0.0;
        p.spread_at  = spread_pt;
        p.id         = ++trade_id_;
        positions_.push_back(p);

        if (cooldown_bars > 0) cooldown_left_ = cooldown_bars;

        return 1;
    }

    // -----------------------------------------------------------------------
    //  on_tick -- intrabar SL fill + Strategy::on_tick_manage (trail/etc).
    //  Mirrors TsmomCell::on_tick: per-position MFE/MAE update, then
    //  Strategy hook (TsmomStrategy returns false), then SL fill check.
    // -----------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb on_close) noexcept
    {
        for (auto it = positions_.begin(); it != positions_.end(); ) {
            Position& p = *it;

            const double signed_move = (direction == 1)
                ? (bid - p.entry) : (p.entry - ask);
            if (signed_move > p.mfe) p.mfe = signed_move;
            if (signed_move < p.mae) p.mae = signed_move;

            if (Strategy::on_tick_manage(p, bid, ask, state, cfg)) {
                const double exit_px = (direction == 1) ? bid : ask;
                _close(p, exit_px, "STRATEGY_CLOSE", now_ms, on_close);
                it = positions_.erase(it);
                continue;
            }

            bool hit = false;
            if (direction == 1) {
                if (bid <= p.sl) {
                    _close(p, bid, "SL_HIT", now_ms, on_close);
                    hit = true;
                }
            } else {
                if (ask >= p.sl) {
                    _close(p, ask, "SL_HIT", now_ms, on_close);
                    hit = true;
                }
            }
            if (hit) it = positions_.erase(it);
            else     ++it;
        }
    }

    // -----------------------------------------------------------------------
    //  force_close -- shutdown / weekend-gap exit for all open positions.
    // -----------------------------------------------------------------------
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseCb on_close) noexcept
    {
        const double exit_px = (direction == 1) ? bid : ask;
        for (Position& p : positions_) {
            _close(p, exit_px, "FORCE_CLOSE", now_ms, on_close);
        }
        positions_.clear();
    }

private:
    void _close(Position& p,
                double          exit_px,
                const char*     reason,
                int64_t         now_ms,
                OnCloseCb       on_close) noexcept
    {
        const double pnl_pts = (exit_px - p.entry) * direction * p.size;

        if (on_close) {
            ::omega::TradeRecord tr;
            tr.id            = p.id;
            tr.symbol        = symbol;
            tr.side          = (direction == 1) ? "LONG" : "SHORT";
            tr.engine        = cell_id;
            tr.entryPrice    = p.entry;
            tr.exitPrice     = exit_px;
            tr.sl            = p.sl;
            tr.tp            = p.tp;
            tr.size          = p.size;
            tr.pnl           = pnl_pts;
            tr.mfe           = p.mfe * p.size;
            tr.mae           = p.mae * p.size;
            tr.entryTs       = p.entry_ms / 1000;
            tr.exitTs        = now_ms / 1000;
            tr.exitReason    = reason;
            tr.regime        = regime_label;
            tr.atr_at_entry  = p.atr;
            tr.spreadAtEntry = p.spread_at;
            tr.shadow        = shadow_mode;
            on_close(tr);
        }
    }
};

// =============================================================================
//  CellPortfolio<Strategy> -- 5-cell harness (H1, H2, H4, H6, D1) driving
//  bar synthesisers, per-TF ATR14, equity tracking, and the warmup CSV
//  loader. Mirrors TsmomPortfolio's public surface for a drop-in replacement.
//
//  add_cell(spec) registers a cell at one of the 5 timeframes. The default
//  TSMOM topology = 5 long cells, one per TF -- but the harness is generic:
//  add only the cells you want, in any direction, to back any subset of
//  the V1 portfolios.
// =============================================================================
template <CellStrategy Strategy>
struct CellPortfolio {
    using OnCloseCb = std::function<void(const ::omega::TradeRecord&)>;
    using Cell      = CellBase<Strategy>;
    using Cfg       = typename Strategy::Config;

    enum TfIdx { TF_H1 = 0, TF_H2 = 1, TF_H4 = 2, TF_H6 = 3, TF_D1 = 4, TF_COUNT = 5 };

    struct CellSpec {
        TfIdx       tf;          // which timeframe this cell trades on
        int         direction;   // +1 long, -1 short
        std::string cell_id;     // logging + TradeRecord.engine
        Cfg         cfg{};       // per-cell config (lookback, hold_bars, etc.)
    };

    // ---- Configuration -----------------------------------------------------
    bool        enabled                = true;
    bool        shadow_mode            = true;
    int         max_concurrent         = 50;
    int         max_positions_per_cell = 10;
    double      risk_pct               = 0.005;
    double      start_equity           = 10000.0;
    double      margin_call            = 1000.0;
    double      max_lot_cap            = 0.05;
    double      usd_per_pt_per_lot     = 100.0;   // XAUUSD baseline
    bool        block_on_risk_off      = true;
    std::string symbol                 = "XAUUSD";
    std::string regime_label           = "CELL";

    std::string warmup_csv_path        = "";

    // ---- Cells (one per registered spec, any number of cells per TF) ------
    std::vector<Cell>  cells_;
    std::vector<TfIdx> cell_tf_;     // parallel array: cell_tf_[i] = tf of cells_[i]

    // ---- Bar synthesisers (H1 -> H2/H4/H6/D1) -----------------------------
    BarSynth synth_h2_;
    BarSynth synth_h4_;
    BarSynth synth_h6_;
    BarSynth synth_d1_;

    // ---- Per-TF ATR14 ------------------------------------------------------
    ATR14 atr_h1_, atr_h2_, atr_h4_, atr_h6_, atr_d1_;

    // ---- Equity / drawdown tracking ---------------------------------------
    double      equity_                 = 10000.0;
    double      peak_equity_            = 10000.0;
    double      max_dd_pct_             = 0.0;
    int         open_count_             = 0;
    int         blocked_max_concurrent_ = 0;
    int         blocked_risk_off_       = 0;
    std::string macro_regime_           = "NEUTRAL";

    void set_macro_regime(const std::string& r) noexcept { macro_regime_ = r; }

    // ---- Cell registration -------------------------------------------------
    void add_cell(const CellSpec& spec) {
        Cell c;
        c.direction              = spec.direction;
        c.cell_id                = spec.cell_id;
        c.symbol                 = symbol;
        c.regime_label           = regime_label;
        c.shadow_mode            = shadow_mode;
        c.max_positions_per_cell = max_positions_per_cell;
        c.cooldown_bars          = 0;
        c.cfg                    = spec.cfg;
        switch (spec.tf) {
            case TF_H1: c.timeframe = "H1"; break;
            case TF_H2: c.timeframe = "H2"; break;
            case TF_H4: c.timeframe = "H4"; break;
            case TF_H6: c.timeframe = "H6"; break;
            case TF_D1: c.timeframe = "D1"; break;
            default: c.timeframe = "??"; break;
        }
        cells_.push_back(std::move(c));
        cell_tf_.push_back(spec.tf);
    }

    // ---- Open-position aggregate ------------------------------------------
    int total_open() const noexcept {
        int n = 0;
        for (const auto& c : cells_) n += c.n_open();
        return n;
    }

    // ---- Init ---------------------------------------------------------------
    void init() noexcept {
        synth_h2_.stride =  2;
        synth_h4_.stride =  4;
        synth_h6_.stride =  6;
        synth_d1_.stride = 24;

        equity_      = start_equity;
        peak_equity_ = start_equity;

        // Propagate portfolio-level config to every cell. add_cell() snapshots
        // shadow/max_positions at registration time, so a config change after
        // add_cell() needs to be re-broadcast here.
        for (auto& c : cells_) {
            c.shadow_mode            = shadow_mode;
            c.max_positions_per_cell = max_positions_per_cell;
            c.symbol                 = symbol;
            c.regime_label           = regime_label;
        }
    }

    // ---- Sizing (mirrors TsmomPortfolio::size_for) -------------------------
    double size_for(double sl_pts) const noexcept {
        if (sl_pts <= 0.0) return 0.0;
        const double risk_target  = equity_ * risk_pct;
        const double risk_per_lot = sl_pts * usd_per_pt_per_lot;
        if (risk_per_lot <= 0.0) return 0.0;
        double lot = risk_target / risk_per_lot;
        lot = std::floor(lot / 0.01) * 0.01;
        return std::max(0.01, std::min(max_lot_cap, lot));
    }

    bool can_open() const noexcept {
        if (!enabled)                                                  return false;
        if (equity_ < margin_call)                                     return false;
        if (block_on_risk_off && macro_regime_ == "RISK_OFF")          return false;
        return total_open() < max_concurrent;
    }

    // ---- Equity-tracking close-callback wrapper ---------------------------
    OnCloseCb wrap(OnCloseCb runtime_cb) {
        return [this, runtime_cb](const ::omega::TradeRecord& tr) {
            const double pnl_usd = tr.pnl * usd_per_pt_per_lot;
            equity_ += pnl_usd;
            if (equity_ > peak_equity_) peak_equity_ = equity_;
            const double dd = (peak_equity_ > 0.0)
                ? (equity_ - peak_equity_) / peak_equity_ : 0.0;
            if (dd < max_dd_pct_) max_dd_pct_ = dd;
            if (runtime_cb) runtime_cb(tr);
        };
    }

    // ---- Per-cell driver ---------------------------------------------------
    void _drive_cell(Cell&        cell,
                     const Bar&   b,
                     double       bid,
                     double       ask,
                     double       atr14,
                     int64_t      now_ms,
                     OnCloseCb    runtime_cb) noexcept
    {
        const bool gate_block =
            !can_open() || cell.n_open() >= cell.max_positions_per_cell;
        const double sl_pts = Strategy::sl_distance_pts(atr14, cell.cfg);
        const double lot    = gate_block ? 0.0 : size_for(sl_pts);

        if (gate_block) {
            if (block_on_risk_off && macro_regime_ == "RISK_OFF")
                ++blocked_risk_off_;
            else
                ++blocked_max_concurrent_;
        }

        (void)cell.on_bar(b, bid, ask, atr14, now_ms, lot, wrap(runtime_cb));
    }

    // ---- H1 bar entrypoint -------------------------------------------------
    void on_h1_bar(const Bar&    h1,
                   double        bid,
                   double        ask,
                   double        h1_atr14,
                   int64_t       now_ms,
                   OnCloseCb     runtime_cb) noexcept
    {
        if (!enabled) return;

        atr_h1_.on_bar(h1);
        const double eff_h1_atr =
            (std::isfinite(h1_atr14) && h1_atr14 > 0.0) ? h1_atr14 : atr_h1_.value();

        // H1 cells
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            if (cell_tf_[i] != TF_H1) continue;
            if (eff_h1_atr > 0.0)
                _drive_cell(cells_[i], h1, bid, ask, eff_h1_atr, now_ms, runtime_cb);
            else
                (void)cells_[i].on_bar(h1, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb));
        }

        // H2 cells
        synth_h2_.on_h1_bar(h1, [&](const Bar& b) {
            atr_h2_.on_bar(b);
            const double a = atr_h2_.value();
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_H2) continue;
                if (atr_h2_.ready())
                    _drive_cell(cells_[i], b, bid, ask, a, now_ms, runtime_cb);
                else
                    (void)cells_[i].on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb));
            }
        });
        // H4 cells
        synth_h4_.on_h1_bar(h1, [&](const Bar& b) {
            atr_h4_.on_bar(b);
            const double a = atr_h4_.value();
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_H4) continue;
                if (atr_h4_.ready())
                    _drive_cell(cells_[i], b, bid, ask, a, now_ms, runtime_cb);
                else
                    (void)cells_[i].on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb));
            }
        });
        // H6 cells
        synth_h6_.on_h1_bar(h1, [&](const Bar& b) {
            atr_h6_.on_bar(b);
            const double a = atr_h6_.value();
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_H6) continue;
                if (atr_h6_.ready())
                    _drive_cell(cells_[i], b, bid, ask, a, now_ms, runtime_cb);
                else
                    (void)cells_[i].on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb));
            }
        });
        // D1 cells
        synth_d1_.on_h1_bar(h1, [&](const Bar& b) {
            atr_d1_.on_bar(b);
            const double a = atr_d1_.value();
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_D1) continue;
                if (atr_d1_.ready())
                    _drive_cell(cells_[i], b, bid, ask, a, now_ms, runtime_cb);
                else
                    (void)cells_[i].on_bar(b, bid, ask, 0.0, now_ms, 0.0, wrap(runtime_cb));
            }
        });

        open_count_ = total_open();
    }

    // ---- Tick entrypoint (intrabar SL + Strategy::on_tick_manage) ---------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseCb runtime_cb) noexcept
    {
        if (!enabled) return;
        for (auto& c : cells_) c.on_tick(bid, ask, now_ms, wrap(runtime_cb));
    }

    void force_close_all(double bid, double ask, int64_t now_ms,
                         OnCloseCb runtime_cb) noexcept
    {
        for (auto& c : cells_) c.force_close(bid, ask, now_ms, wrap(runtime_cb));
    }

    // -----------------------------------------------------------------------
    //  Status snapshot.
    // -----------------------------------------------------------------------
    struct Status {
        bool        enabled;
        bool        shadow_mode;
        double      equity;
        double      peak;
        double      max_dd_pct;
        int         open_count;
        int         blocked_max_concurrent;
        int         blocked_risk_off;
        std::string macro_regime;
    };
    Status status() const noexcept {
        return Status{
            enabled, shadow_mode,
            equity_, peak_equity_, max_dd_pct_,
            total_open(), blocked_max_concurrent_, blocked_risk_off_,
            macro_regime_,
        };
    }

    // -----------------------------------------------------------------------
    //  warmup_from_csv -- mirrors TsmomPortfolio::warmup_from_csv exactly.
    //
    //  CSV format (5 columns, no header, '#' comment lines allowed):
    //      bar_start_ms,open,high,low,close
    //
    //  Each H1 bar drives the synthesisers + ATRs + cell on_bar with
    //  bid=ask=close (zero spread) and size_lot=0 -- so warmup never
    //  opens a position; it only populates Strategy::State (closes_,
    //  EMAs, channels) and the per-TF ATRs.
    // -----------------------------------------------------------------------
    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) {
            printf("[CELL-WARMUP] skipped -- portfolio disabled\n");
            fflush(stdout);
            return 0;
        }
        if (path.empty()) {
            printf("[CELL-WARMUP] skipped -- warmup_csv_path empty (cold start)\n");
            fflush(stdout);
            return 0;
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[CELL-WARMUP] FAIL -- cannot open '%s' (cold start)\n", path.c_str());
            fflush(stdout);
            return 0;
        }

        int     fed = 0, rejected = 0;
        int64_t first_ms = 0, last_ms = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            std::array<std::string, 5> tok;
            int idx = 0;
            std::stringstream ss(line);
            std::string field;
            while (idx < 5 && std::getline(ss, field, ',')) tok[idx++] = field;
            if (idx < 5) { ++rejected; continue; }

            char* endp = nullptr;
            const long long ms_ll = std::strtoll(tok[0].c_str(), &endp, 10);
            if (endp == tok[0].c_str() || *endp != '\0') { ++rejected; continue; }

            char* ep_o = nullptr; const double o = std::strtod(tok[1].c_str(), &ep_o);
            char* ep_h = nullptr; const double h = std::strtod(tok[2].c_str(), &ep_h);
            char* ep_l = nullptr; const double l = std::strtod(tok[3].c_str(), &ep_l);
            char* ep_c = nullptr; const double c = std::strtod(tok[4].c_str(), &ep_c);
            if (ep_o == tok[1].c_str() || ep_h == tok[2].c_str()
                || ep_l == tok[3].c_str() || ep_c == tok[4].c_str()) { ++rejected; continue; }
            if (!std::isfinite(o) || !std::isfinite(h)
                || !std::isfinite(l) || !std::isfinite(c))           { ++rejected; continue; }

            Bar b;
            b.bar_start_ms = static_cast<int64_t>(ms_ll);
            b.open  = o; b.high = h; b.low = l; b.close = c;
            if (fed == 0) first_ms = b.bar_start_ms;
            last_ms = b.bar_start_ms;

            _feed_warmup_h1_bar(b);
            ++fed;
        }

        printf("[CELL-WARMUP] fed=%d rejected=%d first_ms=%lld last_ms=%lld path='%s'\n",
               fed, rejected,
               static_cast<long long>(first_ms),
               static_cast<long long>(last_ms),
               path.c_str());
        fflush(stdout);
        return fed;
    }

private:
    void _feed_warmup_h1_bar(const Bar& h1) noexcept {
        OnCloseCb noop_cb{};
        const double bid = h1.close;
        const double ask = h1.close;
        const int64_t now_ms = h1.bar_start_ms;

        atr_h1_.on_bar(h1);
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            if (cell_tf_[i] != TF_H1) continue;
            (void)cells_[i].on_bar(h1, bid, ask, atr_h1_.value(), now_ms, 0.0, noop_cb);
        }
        synth_h2_.on_h1_bar(h1, [&](const Bar& b) {
            atr_h2_.on_bar(b);
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_H2) continue;
                (void)cells_[i].on_bar(b, bid, ask, atr_h2_.value(), now_ms, 0.0, noop_cb);
            }
        });
        synth_h4_.on_h1_bar(h1, [&](const Bar& b) {
            atr_h4_.on_bar(b);
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_H4) continue;
                (void)cells_[i].on_bar(b, bid, ask, atr_h4_.value(), now_ms, 0.0, noop_cb);
            }
        });
        synth_h6_.on_h1_bar(h1, [&](const Bar& b) {
            atr_h6_.on_bar(b);
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_H6) continue;
                (void)cells_[i].on_bar(b, bid, ask, atr_h6_.value(), now_ms, 0.0, noop_cb);
            }
        });
        synth_d1_.on_h1_bar(h1, [&](const Bar& b) {
            atr_d1_.on_bar(b);
            for (std::size_t i = 0; i < cells_.size(); ++i) {
                if (cell_tf_[i] != TF_D1) continue;
                (void)cells_[i].on_bar(b, bid, ask, atr_d1_.value(), now_ms, 0.0, noop_cb);
            }
        });
    }
};

}  // namespace omega::cell
