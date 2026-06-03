#pragma once
// =============================================================================
//  Ger40KeltnerH1Engine.hpp -- GER40 H1 long-only Keltner trend engine (S41)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-30 (S41) from the cross-symbol edge hunt. The 3 robust XAU
//  trend families were scanned across 12 instruments; only GER40 produced a
//  second, independently deploy-grade edge -- and only with a SLOWER trend
//  filter than gold (bull_LB=200 H1 bars vs gold's 120) and the EMA20 Keltner
//  channel (not gold's EMA50). Validated in backtest/edge_validate_s41.cpp:
//
//    GER40 H1  Keltner EMA20 k2.0 sl3.0  bull_LB=200
//      param PLATEAU: k{1.5,2.0,2.5} x sl{2.5,3.0,3.5} -> ROBUST cluster
//        around k2.0 (k2.0/sl2.5, k2.0/sl3.0, k2.0/sl3.5 all *** ROBUST 5/6)
//      COST-STRESS:  k2.0/sl3.0 ROBUST at 1x/2x/3x half-spread
//        (PF 1.85 / 1.79 / 1.73 -- survives triple cost)
//      WF: both halves PF>1 (H1 1.96 / H2 1.71); 6-block 5/6 positive.
//
//  This is the FIRST non-gold robust trend edge found. It is a SEPARATE engine
//  (not a cell graft) because GER40 needs its own warm-seed CSV, its own slower
//  bull filter, and its own instrument plumbing. Long-only by design: the XAU
//  short-side mirror was tested and is catastrophic (0/6 blocks), and the same
//  bull-only character is expected on an index.
//
//  SIGNAL (single cell)
//    bull gate : close > close[200 bars ago]   (slow uptrend)
//    entry     : close > EMA20 + 2.0*ATR14      (Keltner upper-channel break)
//    exit      : close < EMA20 (channel mid)  OR  stop at entry - 3.0*ATR14
//    no TP     : runner; exits on the channel-mid signal or the ATR stop.
//
//  SAFETY (mirrors XauTrendFollow1hEngine)
//    - shadow_mode = true by default; engine_init.hpp sets the live value.
//    - DOES NOT touch any protected core file.
//    - 0.01 lot; max 1 concurrent position (single cell).
//    - max_spread cap refuses entries at bad fills.
//    - cooldown 1 bar after each close.
//    - long-only; broker-aware fill: long on ask, exit on bid.
//    - ExecutionCostGuard::is_viable() gates every entry.
//    - warmup_from_csv primes EMA20/ATR14/close-ring before first live tick
//      (warm-seed mandate -- needs >=201 bars to arm the bull_LB gate).
//
//  USAGE
//      // globals.hpp:       static omega::Ger40KeltnerH1Engine g_ger40_kelt;
//      // engine_init.hpp:
//      g_ger40_kelt.shadow_mode = kShadowDefault;
//      g_ger40_kelt.enabled     = true;
//      g_ger40_kelt.lot         = 0.01;
//      g_ger40_kelt.max_spread  = 5.0;   // GER40 points
//      g_ger40_kelt.warmup_csv_path = "phase1/signal_discovery/warmup_GER40_H1.csv";
//      g_ger40_kelt.init();
//      g_ger40_kelt.warmup_from_csv(g_ger40_kelt.warmup_csv_path);
//      // tick_ger40.hpp at H1 close:  g_ger40_kelt.on_h1_bar(bar, bid, ask, atr14, now_ms, cb);
//      // every tick:                  g_ger40_kelt.on_tick(bid, ask, now_ms, cb);
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"   // S-2026-06-03: omega::PositionSnapshot for persist
#include "IndexRiskGate.hpp"      // S44 portfolio VIX risk-off gate (entry-only)

namespace omega {

struct Ger40KBar {
    int64_t bar_start_ms = 0;
    double  open=0.0, high=0.0, low=0.0, close=0.0;
};

struct Ger40KPos {
    bool    active        = false;
    double  entry_px      = 0.0;
    double  sl_px         = 0.0;
    double  atr_at_entry  = 0.0;
    int64_t entry_ts_ms   = 0;
    int     bars_held     = 0;
    int     cooldown_bars = 0;
    double  mfe = 0.0, mae = 0.0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

struct Ger40KeltnerH1Engine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 5.0;   // GER40 points

    // Validated params (edge_validate_s41.cpp). Public so engine_init can
    // override, but defaults ARE the deploy-grade config.
    int    kBullLB   = 200;     // close > close[kBullLB] bull gate
    int    kEmaP     = 20;      // Keltner channel EMA period
    double kChanK    = 2.0;     // upper channel = EMA + kChanK*ATR
    double kSlAtr    = 3.0;     // stop = entry - kSlAtr*ATR

    Ger40KPos pos{};

    static constexpr int kBarHistory = 280;   // >= kBullLB + EMA/ATR warmup
    std::deque<Ger40KBar> bars_;

    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    double ema_ = 0.0;
    bool   ema_initialised_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;
    bool warmup_active_ = false;
    std::string warmup_csv_path;

    // S42: self-contained H1 tick->bar aggregator (there is NO g_bars_ger40 in
    // the codebase; GER40 arrives only as raw ticks in tick_indices.hpp, same
    // as Ger40TurtleH4Engine). feed_tick() buckets ticks by the UTC hour and,
    // on each H1 rollover, synthesizes the just-closed bar and dispatches it to
    // on_h1_bar(). Position management still runs every tick. Backtests bypass
    // this and call on_h1_bar() directly with clean OHLC -- so the validated
    // signal path is unchanged; only the live bar SOURCE differs (tick-built).
    struct H1Accum {
        bool    active    = false;
        int64_t bucket_ms = 0;
        double  open=0.0, high=0.0, low=0.0, close=0.0;
    } h1_acc_;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0; atr_warmup_count_ = 0;
        ema_ = 0.0; ema_initialised_ = false;
        warmup_active_ = false;
        h1_acc_ = {};
        pos = {};
    }

    bool any_open() const noexcept { return pos.active; }

    // S-2026-06-03: persistence batch 4 (long-only; no tp -> ema/channel exit).
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG";
        o.size = lot; o.entry = pos.entry_px; o.sl = pos.sl_px; o.tp = 0.0;
        o.entry_ts = pos.entry_ts_ms / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos.active = true; pos.entry_px = ps.entry; pos.sl_px = ps.sl;
        pos.entry_ts_ms = ps.entry_ts * 1000;
        return true;
    }

    void on_h1_bar(const Ger40KBar& bar, double bid, double ask,
                   double atr14_external, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled) return;

        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else                       _update_local_atr();
        _update_ema();

        if (pos.cooldown_bars > 0) --pos.cooldown_bars;
        if (pos.active) ++pos.bars_held;

        // Channel-mid exit on bar close takes priority over a new entry.
        if (pos.active && ema_initialised_ && bar.close < ema_) {
            _close(bar.close, "KELT_MID_EXIT", now_ms, on_close);
        }

        if ((int)bars_.size() < kBullLB + 2) return;   // bull gate not armed
        if (atr14_ <= 0.0 || !ema_initialised_) return;
        if (ask - bid > max_spread) return;
        if (pos.active || pos.cooldown_bars > 0) return;

        if (_signal() > 0) _fire_entry(bid, ask, now_ms);
        (void)on_close;
    }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled || !pos.active) return;
        _manage_open(bid, ask, now_ms, on_close);
    }

    // S42: LIVE entry point -- aggregate ticks to H1 + manage open pos. This is
    // what tick_indices.hpp calls. Mirrors Ger40TurtleH4Engine::on_tick's
    // self-aggregation (H4 there, H1 here). on_h1_bar() handles all signal /
    // entry / channel-exit logic when a bar closes.
    void feed_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;

        // manage open position every tick (SL hit etc.)
        if (pos.active) _manage_open(bid, ask, now_ms, on_close);

        const int64_t bucket = (now_ms / 3600000LL) * 3600000LL;   // UTC hour
        if (!h1_acc_.active) {
            h1_acc_.active=true; h1_acc_.bucket_ms=bucket;
            h1_acc_.open=h1_acc_.high=h1_acc_.low=h1_acc_.close=mid;
            return;
        }
        if (bucket != h1_acc_.bucket_ms) {
            // H1 rollover: dispatch the just-closed bar through the signal path.
            Ger40KBar bar;
            bar.bar_start_ms = h1_acc_.bucket_ms;
            bar.open = h1_acc_.open; bar.high = h1_acc_.high;
            bar.low  = h1_acc_.low;  bar.close = h1_acc_.close;
            on_h1_bar(bar, bid, ask, 0.0, now_ms, on_close);
            // start the new bucket
            h1_acc_.bucket_ms=bucket;
            h1_acc_.open=h1_acc_.high=h1_acc_.low=h1_acc_.close=mid;
        } else {
            if (mid > h1_acc_.high) h1_acc_.high = mid;
            if (mid < h1_acc_.low)  h1_acc_.low  = mid;
            h1_acc_.close = mid;
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        if (!pos.active) return;
        _close(bid, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        (void)ask;
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

    void _update_ema() noexcept {
        if (bars_.empty()) return;
        const double c = bars_.back().close;
        if (!ema_initialised_) { ema_ = c; ema_initialised_ = true; }
        else { const double a = 2.0 / (kEmaP + 1); ema_ = a * c + (1.0 - a) * ema_; }
    }

    // long-only: +1 when bull gate AND upper-channel break, else 0.
    int _signal() const noexcept {
        const int n = (int)bars_.size();
        if (n < kBullLB + 1) return 0;
        const double c     = bars_[n - 1].close;
        const double c_lb  = bars_[n - 1 - kBullLB].close;
        if (!(c > c_lb)) return 0;                           // bull gate
        if (c > ema_ + kChanK * atr14_) return 1;            // Keltner upper break
        return 0;
    }

    void _fire_entry(double bid, double ask, int64_t now_ms) noexcept {
        if (warmup_active_) return;
        if (omega::index_risk_off()) return;   // S44 portfolio VIX risk-off: no new entry
        const double entry = ask;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        const double sl_dist = kSlAtr * atr14_;
        const double sl_px   = entry - sl_dist;

        {   // cost gate; no fixed TP -> use the stop distance as the viability span
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable("GER40", spread_pts, sl_dist, lot, 1.5)) return;
        }

        pos.active        = true;
        pos.entry_px      = entry;
        pos.sl_px         = sl_px;
        pos.atr_at_entry  = atr14_;
        pos.entry_ts_ms   = now_ms;
        pos.bars_held     = 0;
        pos.cooldown_bars = 0;
        pos.mfe = pos.mae = 0.0;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
    }

    void _manage_open(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;
        const double mid = (bid + ask) * 0.5;
        const double fav = mid - pos.entry_px;   // long-only
        if (fav > pos.mfe) pos.mfe = fav;
        if (-fav > pos.mae) pos.mae = -fav;
        if (bid <= pos.sl_px) { _close(pos.sl_px, "SL_HIT", now_ms, on_close); return; }
        (void)ask;
    }

    void _close(double exit_px, const char* reason, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;
        const double pts_move = exit_px - pos.entry_px;   // long-only

        omega::TradeRecord tr;
        tr.symbol     = "GER40";
        tr.engine     = "Ger40KeltnerH1_EMA20_k2.0_sl3.0_LB200_S41";
        tr.side       = "LONG";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = 0.0;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "Keltner_EMA20_k2.0_LB200";
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * lot;
        tr.mfe        = pos.mfe;
        tr.mae        = pos.mae;

        if (on_close) on_close(tr);

        pos.active = false;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
        pos.cooldown_bars = 1;
    }

public:
    // CSV format: bar_start_ms,open,high,low,close   (matches XauTF1h warmup).
    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) { printf("[GER40Kelt-WARMUP] skipped -- disabled\n"); fflush(stdout); return 0; }
        if (path.empty()) { printf("[GER40Kelt-WARMUP] skipped -- no path (cold start)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[GER40Kelt-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;

        int fed = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == 'b') continue;
            char* p1; long long ms = std::strtoll(line.c_str(), &p1, 10);
            if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1+1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2+1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3+1, &p4); if (!p4 || *p4 != ',') continue;
            char* p5; double c = std::strtod(p4+1, &p5);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;

            Ger40KBar bar; bar.bar_start_ms = ms; bar.open=o; bar.high=h; bar.low=l; bar.close=c;
            on_h1_bar(bar, c, c, 0.0, ms + 3600LL*1000, OnCloseFn{});
            ++fed; (void)p5;
        }
        warmup_active_ = false;
        printf("[GER40Kelt-WARMUP] fed=%d bars, atr=%.4f ema=%.4f bars=%d path='%s'\n",
               fed, atr14_, ema_, (int)bars_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega
