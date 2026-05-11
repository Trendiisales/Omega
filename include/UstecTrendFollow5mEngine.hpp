#pragma once
// =============================================================================
//  UstecTrendFollow5mEngine.hpp -- USTEC 5m Donchian trend-follow
//                                  (S33d 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from edge_hunt.cpp results on 15 days of USTEC L2
//  (2026-04-22 -> 2026-05-08). Realistic bid/ask fill simulation,
//  $0.06/RT cost subtracted, 0.1 lot.
//
//  Cell:
//      Donchian N=20 sl2.0tp4.0
//      111 trades, 45% WR, net +$1120, $10.09/trade
//      2/2 months positive, BE cost $10.1/RT (170x margin over $0.06)
//
//  Plus convergent confirmation from 3 other unrelated signal families on
//  the same data (Donchian sl1.5_tp3.0 +$1021, ER_Trend +$1020, MA-cross
//  +$975). Donchian sl2.0_tp4.0 is the lead cell because of biggest WR,
//  largest per-trade edge, and cleanest backtest.
//
//  CAVEAT: only 2 months of L2 data so far. **DEPLOY SHADOW ONLY** for at
//  least 6 months while you collect more USTEC L2 capture. The 15-day
//  sample is striking but undersampled.
//
//  SAFETY
//      - shadow_mode = true by default.
//      - 0.1 lot cap (index baseline; USTEC pt_size = 0.1).
//      - Max 1 concurrent position.
//      - Spread cap = 5.0 (USTEC quotes; tune by observation).
//      - 1-bar cooldown after exit.
//
//  USAGE
//
//      // globals.hpp:
//      static omega::UstecTrendFollow5mEngine g_ustec_tf_5m;
//
//      // engine_init.hpp:
//      g_ustec_tf_5m.shadow_mode = kShadowDefault;
//      g_ustec_tf_5m.enabled     = true;
//      g_ustec_tf_5m.lot         = 0.1;
//      g_ustec_tf_5m.max_spread  = 5.0;
//      g_ustec_tf_5m.init();
//
//      // tick_indices.hpp inside 5m-close branch for USTEC:
//      omega::UstecTfBar bar5m{};
//      bar5m.bar_start_ms = s_bar5m_ms;
//      bar5m.open  = s_cur5m.open;
//      bar5m.high  = s_cur5m.high;
//      bar5m.low   = s_cur5m.low;
//      bar5m.close = s_cur5m.close;
//      g_ustec_tf_5m.on_5m_bar(bar5m, ustec_bid, ustec_ask,
//          ustec_atr14_5m, now_ms_g, bracket_on_close);
//
//      // tick_indices.hpp every USTEC tick:
//      g_ustec_tf_5m.on_tick(ustec_bid, ustec_ask, now_ms_g, bracket_on_close);
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"

namespace omega {

struct UstecTfBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct UstecTfPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

struct UstecTrendFollow5mEngine {
public:
    // Public knobs.
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.1;
    double max_spread  = 5.0;

    // Cell config: Donchian N=20, SL=2.0*ATR, TP=4.0*ATR (the validated cell)
    static constexpr int    kDonchianN  = 20;
    static constexpr double kSlMult     = 2.0;
    static constexpr double kTpMult     = 4.0;
    static constexpr int    kCooldownBars = 1;

    UstecTfPos pos{};

    // Bar history
    static constexpr int kBarHistory = 64;
    std::deque<UstecTfBar> bars_;

    // Wilder ATR14 local fallback
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        pos = {};
    }

    bool has_open_position() const noexcept { return pos.active; }

    // ============================================================
    //  on_5m_bar -- called when a 5m bar closes
    // ============================================================
    void on_5m_bar(const UstecTfBar& bar,
                   double bid, double ask,
                   double atr14_external,
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else _update_local_atr();

        if (pos.cooldown_bars > 0) --pos.cooldown_bars;
        if (pos.active) ++pos.bars_held;

        if ((int)bars_.size() < kDonchianN + 2) return;
        if (atr14_ <= 0.0) return;
        if (pos.active) return;
        if (pos.cooldown_bars > 0) return;
        if (ask - bid > max_spread) return;

        int side = _sig_donchian();
        if (side == 0) return;
        _fire_entry(side, bid, ask, now_ms);
        (void)on_close;
    }

    // ============================================================
    //  on_tick -- manage open position
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled || !pos.active) return;
        _manage_open(bid, ask, now_ms, on_close);
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        if (!pos.active) return;
        double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
    }

private:
    void _update_local_atr() noexcept {
        if ((int)bars_.size() < 2) { atr14_ = 0.0; return; }
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

    int _sig_donchian() const noexcept {
        const int N = kDonchianN;
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

    void _fire_entry(int side, double bid, double ask, int64_t now_ms) noexcept {
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = kSlMult * atr14_;
        double tp_dist = kTpMult * atr14_;
        pos.active        = true;
        pos.is_long       = (side > 0);
        pos.entry_px      = entry;
        pos.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        pos.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        pos.atr_at_entry  = atr14_;
        pos.entry_ts_ms   = now_ms;
        pos.bars_held     = 0;
        pos.cooldown_bars = 0;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
    }

    void _manage_open(double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        bool hit_tp = false, hit_sl = false;
        double exit_px = 0.0;
        if (pos.is_long) {
            if (bid <= pos.sl_px) { exit_px = pos.sl_px; hit_sl = true; }
            else if (bid >= pos.tp_px) { exit_px = pos.tp_px; hit_tp = true; }
        } else {
            if (ask >= pos.sl_px) { exit_px = pos.sl_px; hit_sl = true; }
            else if (ask <= pos.tp_px) { exit_px = pos.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;
        _close(exit_px, hit_tp ? "TP_HIT" : "SL_HIT", now_ms, on_close);
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;

        omega::TradeRecord tr;
        tr.symbol     = "USTEC";
        tr.engine     = "UstecTrendFollow5m";
        tr.side       = pos.is_long ? "BUY" : "SELL";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = pos.tp_px;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "Donchian_N20_sl2.0tp4.0";
        tr.shadow     = shadow_mode;

        if (on_close) on_close(tr);

        pos.active        = false;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
        pos.cooldown_bars = kCooldownBars;
    }
};

} // namespace omega
