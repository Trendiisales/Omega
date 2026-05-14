#pragma once
// =============================================================================
//  XauTrendFollow2hEngine.hpp -- XAU 2h trend-follow ensemble (S33k 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from Pass-8 deep_dive results. Same trend regime as
//  the 4h/D1 ensembles, denser cadence. Realistic bid/ask fills, $0.06/RT
//  cost, 0.01 lot. All cells positive in all 3 Duka years (2023 Q4 → 2025
//  Q3):
//
//      [A] Keltner   K=2.0      sl2.0tp4.0    n=179  net=+$950   BE=$5.37
//      [B] Donchian  N=20       sl2.0tp4.0    n=204  net=+$872   BE=$4.33
//      [C] Donchian  N=50       sl2.0tp4.0    n=142  net=+$833   BE=$5.93
//      [D] InsideBar             sl2.0tp4.0    n=239  net=+$725   BE=$3.09
//
//  Combined ~764 trades over 30 months, ~$3,380 historical net at 0.01 lot.
//  Roughly 25 trades/month combined across the 4 cells. Pure XAU
//  concentration -- this engine is correlated with XauTrendFollow4hEngine
//  and XauTrendFollowD1Engine; treat the three as one XAU multi-TF
//  position rather than independent diversification.
//
//  ARCHITECTURE
//
//  Takes H1 bars via on_h1_bar() (tick_gold.hpp already publishes these
//  to g_donchian and others). Internally groups 2 H1 bars into 1 H2 bar
//  by aligning on UTC even hours.
//
//  SAFETY
//      - shadow_mode = true by default
//      - 0.01 lot per cell, max 4 concurrent positions
//      - 1-bar cooldown per cell
//      - Spread cap 1.0 USD; no entries above
//      - No protected file touched
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauTrendFollow2hEngine g_xau_tf_2h;
//
//      // engine_init.hpp:
//      g_xau_tf_2h.shadow_mode = kShadowDefault;
//      g_xau_tf_2h.enabled     = true;
//      g_xau_tf_2h.lot         = 0.01;
//      g_xau_tf_2h.max_spread  = 1.0;
//      g_xau_tf_2h.init();
//
//      // tick_gold.hpp (in H1-close branch, after the donchian H1 dispatch):
//      omega::XauTf2hBar tf_h1{};
//      tf_h1.bar_start_ms = s_bar_h1_ms;
//      tf_h1.open  = s_cur_h1.open;
//      tf_h1.high  = s_cur_h1.high;
//      tf_h1.low   = s_cur_h1.low;
//      tf_h1.close = s_cur_h1.close;
//      g_xau_tf_2h.on_h1_bar(tf_h1, bid, ask, now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp per-tick:
//      g_xau_tf_2h.on_tick(bid, ask, now_ms_g, bracket_on_close);
// =============================================================================
//
//  S34 P1 FIXES (2026-05-12) -- close-path bug class from HANDOFF_S34.md §3.2,
//  §4.2. See XauTrendFollow4hEngine.hpp for full diagnosis. Bug numbers
//  below match the table in §3.2.
//
//    #1 tr.pnl was never assigned in _close. Fix: pts_move = (long?
//       exit-entry : entry-exit); tr.pnl = pts_move * lot. Downstream
//       handle_closed_trade applies tick_value_multiplier(symbol) for USD.
//    #3 tr.engine was bare "XauTrendFollow2h" for all 4 cells. Fix:
//       tr.engine = "XauTrendFollow2h_" + cell.name.
//    #4 tr.side was BUY/SELL. Fix: LONG/SHORT.
//    #5 MFE/MAE never tracked. Fix: mfe/mae fields on XauTf2hPos, updated
//       per tick in _manage_open with mid = (bid+ask)/2, propagated to
//       TradeRecord.mfe/.mae in _close.
//
//  Bug #2 (symbol key) NOT applicable per handoff §4.2 -- "XAUUSD" is
//  the right sizing-table key. S34-B guards NOT carried over (5m-USTEC
//  calibrated, meaningless on 2h XAU).
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

// Input: H1 bar (caller provides; engine builds 2h internally)
struct XauTf2hBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct XauTf2hPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    // S34 P1 fix #5: per-position MFE/MAE in price units (>=0).
    double      mfe                = 0.0;
    double      mae                = 0.0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

enum class XauTf2hFamily { Keltner20, Donchian20, Donchian50, InsideBar };
struct XauTf2hCellConfig {
    XauTf2hFamily family;
    double        sl_mult;
    double        tp_mult;
    const char*   name;
};

// 4 validated cells, all 3/3 Duka years positive at sl2.0/tp4.0
static constexpr XauTf2hCellConfig kXauTf2hCells[] = {
    { XauTf2hFamily::Keltner20,  2.0, 4.0, "2h_Keltner_K2_sl2.0tp4.0"   },
    { XauTf2hFamily::Donchian20, 2.0, 4.0, "2h_Donchian_N20_sl2.0tp4.0" },
    { XauTf2hFamily::Donchian50, 2.0, 4.0, "2h_Donchian_N50_sl2.0tp4.0" },
    { XauTf2hFamily::InsideBar,  2.0, 4.0, "2h_InsideBar_sl2.0tp4.0"    },
};
static constexpr int kXauTf2hNumCells =
    static_cast<int>(sizeof(kXauTf2hCells) / sizeof(kXauTf2hCells[0]));

// Internal 2h bar synthesised from two consecutive H1 bars
struct XauTf2hSynthBar {
    long long bucket_ms = 0;  // ts of bar start; aligned to even hours
    double    open  = 0.0;
    double    high  = 0.0;
    double    low   = 0.0;
    double    close = 0.0;
    int       h1_count = 0;
};

struct XauTrendFollow2hEngine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 1.0;

    // S63 2026-05-14 (part W): VWR-pattern in-flight protection (full trio).
    //   Defaults to ALL ZERO -- S63 is OFF on production until per-instrument
    //   backtest evidence justifies enabling. The XauTrendFollow trio
    //   (2h/4h/D1) is in STATE B (hooks declared + mgmt-path implemented +
    //   deliberately zeroed at class default) until the planned Phase 3
    //   walk-forward sweep (scripts/xtf_*_wf_t1.py + the dedicated
    //   XauTrendFollowBacktest harness) produces baseline-vs-tuned evidence.
    //   The S63-adverse pattern from VWR USTEC (S71 Phase 3) and UTF5m
    //   USTEC (S73 Phase 3) is the prior; the XTF trio sweep is the test
    //   of whether the pattern generalises to XAU trend-follow at higher
    //   timeframes, or whether XAU's tail shape flips the sign. Pre-commit
    //   pass criterion: aggregate PF >= 1.20 AND >=3/4 windows pass the
    //   per-window avg_pnl >= +0.001 threshold (same protocol as UTF5m S73).
    //
    //   LOSS_CUT_PCT  -- cut when adverse >= entry*pct/100. Phase 2 (cold-loss).
    //   BE_ARM_PCT    -- mfe % of entry that arms BE ratchet. Phase 1 (giveback).
    //   BE_BUFFER_PCT -- BE_CUT triggers when move <= entry*pct/100 after arm.
    //   Override via the backtest harness CLI for the sweep; set _PCT = 0.0
    //   to disable a phase entirely.
    double LOSS_CUT_PCT  = 0.0;
    double BE_ARM_PCT    = 0.0;
    double BE_BUFFER_PCT = 0.0;

    std::array<XauTf2hPos, kXauTf2hNumCells> pos{};

    // 2h-bar history (last 80; need 50 for Donchian N=50 + warmup)
    static constexpr int kBarHistory = 80;
    std::deque<XauTf2hSynthBar> bars_;

    XauTf2hSynthBar cur_2h_{};
    bool            cur_2h_open_ = false;

    // Indicators on 2h bars
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    static constexpr int kEmaPeriod = 20;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_.clear();
        cur_2h_ = {};
        cur_2h_open_ = false;
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        for (auto& p : pos) p = {};
    }

    bool any_open() const noexcept {
        for (const auto& p : pos) if (p.active) return true;
        return false;
    }
    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos) if (p.active) ++n; return n;
    }

    // ============================================================
    //  on_h1_bar -- aggregates 2 H1 bars into 1 H2 bar by even-hour
    //               UTC bucket, then fires when H2 closes.
    // ============================================================
    void on_h1_bar(const XauTf2hBar& bar,
                   double bid, double ask,
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;

        long long bucket = (bar.bar_start_ms / 7200000LL) * 7200000LL;  // 2h = 7200s

        if (!cur_2h_open_) {
            cur_2h_ = {};
            cur_2h_.bucket_ms = bucket;
            cur_2h_.open  = bar.open;
            cur_2h_.high  = bar.high;
            cur_2h_.low   = bar.low;
            cur_2h_.close = bar.close;
            cur_2h_.h1_count = 1;
            cur_2h_open_ = true;
            return;
        }

        if (bucket != cur_2h_.bucket_ms) {
            // 2h bar closed: finalise + evaluate
            _finalise_2h_and_evaluate(bid, ask, now_ms, on_close);
            // Start new 2h with current H1
            cur_2h_ = {};
            cur_2h_.bucket_ms = bucket;
            cur_2h_.open  = bar.open;
            cur_2h_.high  = bar.high;
            cur_2h_.low   = bar.low;
            cur_2h_.close = bar.close;
            cur_2h_.h1_count = 1;
            cur_2h_open_ = true;
            return;
        }

        // Same 2h bucket: extend current bar
        if (bar.high > cur_2h_.high) cur_2h_.high = bar.high;
        if (bar.low  < cur_2h_.low)  cur_2h_.low  = bar.low;
        cur_2h_.close = bar.close;
        ++cur_2h_.h1_count;
    }

    // Per-tick exit management
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    void _finalise_2h_and_evaluate(double bid, double ask,
                                   int64_t now_ms,
                                   OnCloseFn on_close) noexcept {
        if (!cur_2h_open_) return;
        bars_.push_back(cur_2h_);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        _update_atr14();
        _update_ema20();

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        if ((int)bars_.size() < 52) return;   // need 50 for Donchian + warmup
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    void _update_atr14() noexcept {
        if (bars_.size() < 2) { atr14_ = 0.0; return; }
        const auto& cur  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];
        double tr = std::max(cur.high - cur.low,
                             std::max(std::abs(cur.high - prev.close),
                                      std::abs(cur.low  - prev.close)));
        if (atr_warmup_count_ < kAtrPeriod) {
            atr14_ = (atr14_ * atr_warmup_count_ + tr) / (atr_warmup_count_ + 1);
            ++atr_warmup_count_;
        } else {
            atr14_ = (atr14_ * (kAtrPeriod - 1) + tr) / kAtrPeriod;
        }
    }

    void _update_ema20() noexcept {
        if (bars_.empty()) return;
        double c = bars_.back().close;
        if (!ema_initialised_) { ema20_ = c; ema_initialised_ = true; }
        else {
            double a = 2.0 / (kEmaPeriod + 1);
            ema20_ = a * c + (1.0 - a) * ema20_;
        }
    }

    int _evaluate_signal(int ci) const noexcept {
        switch (kXauTf2hCells[ci].family) {
            case XauTf2hFamily::Keltner20:  return _sig_keltner();
            case XauTf2hFamily::Donchian20: return _sig_donchian(20);
            case XauTf2hFamily::Donchian50: return _sig_donchian(50);
            case XauTf2hFamily::InsideBar:  return _sig_inside_bar();
        }
        return 0;
    }

    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if (bars_.size() < 22) return 0;
        const auto& cur = bars_.back();
        double up = ema20_ + 2.0 * atr14_;
        double lo = ema20_ - 2.0 * atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }

    int _sig_donchian(int N) const noexcept {
        if ((int)bars_.size() < N + 1) return 0;
        const int last = (int)bars_.size() - 1;
        double hi = bars_[last - N].high, lo = bars_[last - N].low;
        for (int k = last - N + 1; k <= last - 1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }

    int _sig_inside_bar() const noexcept {
        if (bars_.size() < 3) return 0;
        const int last = (int)bars_.size() - 1;
        const auto& a = bars_[last - 2];
        const auto& b = bars_[last - 1];
        const auto& c = bars_[last];
        if (!(b.high < a.high && b.low > a.low)) return 0;
        if (c.close > b.high) return +1;
        if (c.close < b.low)  return -1;
        return 0;
    }

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = kXauTf2hCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    "XAUUSD", spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }
        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        // S34 P1 fix #5: reset MFE/MAE per new entry.
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];

        // S34 P1 fix #5: update MFE/MAE on every tick using mid price.
        const double mid = (bid + ask) * 0.5;
        const double favourable = p.is_long ? (mid - p.entry_px)
                                            : (p.entry_px - mid);
        if (favourable > p.mfe) p.mfe = favourable;
        if (-favourable > p.mae) p.mae = -favourable;

        // S63 2026-05-14 (part W): VWR-pattern in-flight protection. Runs
        //   BEFORE the SL/TP exit check so cold-loss/giveback cuts take
        //   priority. Mirrors IndexMacroCrashEngine pattern at
        //   IndexFlowEngine.hpp:1123-1150. Both phases gated on their _PCT
        //   field being > 0.0 so defaulting all three to 0.0 (state B) is
        //   a structural no-op.
        // Phase 1: BE_RATCHET -- after mfe crosses arm threshold, exit at BE
        //          if current move falls back to within buffer.
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && p.entry_px > 0.0) {
            const double arm_pts    = p.entry_px * BE_ARM_PCT    / 100.0;
            const double buffer_pts = p.entry_px * BE_BUFFER_PCT / 100.0;
            if (p.mfe >= arm_pts && favourable <= buffer_pts) {
                const double exit_px = p.is_long ? bid : ask;
                _close(ci, exit_px, "BE_CUT", now_ms, on_close);
                return;
            }
        }
        // Phase 2: LOSS_CUT -- cold-loss cut for trades that go straight
        //          adverse without ever reaching the ATR-based SL.
        if (LOSS_CUT_PCT > 0.0 && p.entry_px > 0.0) {
            const double adverse       = -favourable;
            const double loss_cut_dist = p.entry_px * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px = p.is_long ? bid : ask;
                _close(ci, exit_px, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0;
        if (p.is_long) {
            if (bid <= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;
        _close(ci, xp, hit_tp ? "TP_HIT" : "SL_HIT", now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // S34 P1 fix #1: pts_move signed against direction so profit is +ve.
        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);

        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        // S34 P1 fix #3: per-cell engine string.
        tr.engine     = std::string("XauTrendFollow2h_") + kXauTf2hCells[ci].name;
        // S34 P1 fix #4: LONG/SHORT, not BUY/SELL.
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTf2hCells[ci].name;
        tr.shadow     = shadow_mode;
        // S34 P1 fix #1: gross pnl in price-points*lot; USD via tick_mult.
        tr.pnl        = pts_move * lot;
        // S34 P1 fix #5: propagate MFE/MAE.
        tr.mfe        = p.mfe;
        tr.mae        = p.mae;

        if (on_close) on_close(tr);

        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }
};

} // namespace omega
