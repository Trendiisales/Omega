#pragma once
// =============================================================================
//  UstecTrendFollow5mEngine.hpp -- USTEC 5m trend-follow ensemble
//                                  (S33e 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from edge_hunt.cpp + edge_hunt v3 results on 15 days
//  of USTEC L2 (2026-04-22 → 2026-05-08). Realistic bid/ask fill
//  simulation, $0.06/RT cost subtracted, 0.1 lot.
//
//  Cells (each independently positive, convergent across signal families):
//
//      [A] Donchian N=20  sl2.0tp4.0    n=111  WR=45%  net=+$1120  $10.09/trade
//      [B] Keltner  K=2.0 sl2.0tp4.0    n= 97  WR=45%  net=+$1291  $13.31/trade  ★ top cell
//
//  Plus convergent confirmation from ER_Trend ($1020) and MA-cross ($975)
//  on the same data, which we keep as research signals only (the two we
//  ship are uncorrelated enough mechanics to give decorrelated PnL).
//
//  CAVEAT: only 2 months of L2 data so far. **DEPLOY SHADOW ONLY** for at
//  least 6 months while you collect more USTEC L2 capture. Per-cell n is
//  modest (97-111 trades).
//
//  SAFETY
//      - shadow_mode = true by default (HARD shadow).
//      - 0.1 lot per cell (USTEC pt_size = 0.1).
//      - Max 2 concurrent positions (one per cell).
//      - Spread cap = 5.0 USD; refuse entries above this.
//      - 1-bar cooldown after exit (per cell).
//
//  USAGE
//
//      // globals.hpp:
//      static omega::UstecTrendFollow5mEngine g_ustec_tf_5m;
//
//      // engine_init.hpp:
//      g_ustec_tf_5m.shadow_mode = true;
//      g_ustec_tf_5m.enabled     = true;
//      g_ustec_tf_5m.lot         = 0.1;
//      g_ustec_tf_5m.max_spread  = 5.0;
//      g_ustec_tf_5m.init();
//
//      // tick_indices.hpp 5m-bar-close dispatch:
//      omega::UstecTfBar bar5m{};
//      bar5m.bar_start_ms = s_nq5_start;
//      bar5m.open  = s_nq5.open;
//      bar5m.high  = s_nq5.high;
//      bar5m.low   = s_nq5.low;
//      bar5m.close = s_nq5.close;
//      g_ustec_tf_5m.on_5m_bar(bar5m, bid, ask, atr_5m_external,
//                              now_ms, ca_on_close);
//
//      // tick_indices.hpp every USTEC tick:
//      g_ustec_tf_5m.on_tick(bid, ask, now_ms, ca_on_close);
// =============================================================================

#include <algorithm>
#include <array>
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

// ---------- Cell config
enum class UstecTfFamily { Donchian20, Keltner20 };
struct UstecTfCellConfig {
    UstecTfFamily family;
    double        sl_mult;
    double        tp_mult;
    const char*   name;
};

// 2 validated cells (15-day L2 sample, all positive)
static constexpr UstecTfCellConfig kUstecTfCells[] = {
    { UstecTfFamily::Donchian20, 2.0, 4.0, "Donchian_N20_sl2.0tp4.0" },
    { UstecTfFamily::Keltner20,  2.0, 4.0, "Keltner_K2_sl2.0tp4.0"   },
};
static constexpr int kUstecTfNumCells =
    static_cast<int>(sizeof(kUstecTfCells) / sizeof(kUstecTfCells[0]));

struct UstecTrendFollow5mEngine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.1;
    double max_spread  = 5.0;

    std::array<UstecTfPos, kUstecTfNumCells> pos{};

    static constexpr int kBarHistory = 64;
    std::deque<UstecTfBar> bars_;

    // Local indicators
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    static constexpr int kEmaPeriod = 20;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        for (auto& p : pos) p = {};
    }

    bool has_open_position() const noexcept {
        for (const auto& p : pos) if (p.active) return true;
        return false;
    }
    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos) if (p.active) ++n; return n;
    }

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
        _update_ema20();

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        if ((int)bars_.size() < 22) return;
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        for (int ci = 0; ci < kUstecTfNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kUstecTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kUstecTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
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
        switch (kUstecTfCells[ci].family) {
            case UstecTfFamily::Donchian20: return _sig_donchian();
            case UstecTfFamily::Keltner20:  return _sig_keltner();
        }
        return 0;
    }

    int _sig_donchian() const noexcept {
        const int N = 20;
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

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = kUstecTfCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
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
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
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
        omega::TradeRecord tr;
        tr.symbol     = "USTEC";
        tr.engine     = "UstecTrendFollow5m";
        tr.side       = p.is_long ? "BUY" : "SELL";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kUstecTfCells[ci].name;
        tr.shadow     = shadow_mode;
        if (on_close) on_close(tr);
        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }
};

} // namespace omega
