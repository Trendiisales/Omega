#pragma once
// =============================================================================
//  FxCrossRevEngine.hpp -- FX cross-pair D1 mean-reversion (S43, 2nd FX edge)
//
//  Crosses of macro-correlated majors (EURGBP, AUDNZD) carry no drift and
//  mean-revert -- the two legs share drivers so the cross ranges. Trade the
//  cross DIRECTLY (one leg, real spread), z-score fade, long AND short.
//
//  VALIDATION (backtest/fx_cross_reversion.cpp, REAL Dukascopy D1 2019-2026):
//    EURGBP: broad robust cluster, center w60/zin2.0/zout0.3/h20 = PF 2.01
//            +2663bp WR69% H1 1.86/H2 2.29 5/6 blocks. Dozens *** ROBUST.
//    AUDNZD (3bp cost): w40/zin2.5/zout0.3/h20 PF 3.09 +1490bp 5/6 (H1-loaded).
//    D1 works where H1-synth failed (PF1.03): multi-day reversion >> cross
//    spread = cost-survivable (same principle as carry). Trust cluster center.
//
//  DESIGN -- one instance per cross symbol (mirrors FxCarryEngine). D1-driven
//  (on_tick aggregates UTC-day bars). Rolling z-window of log-closes:
//    z = (logclose - mean_w) / sd_w   over the last z_window closed bars
//    enter SHORT if z > +z_in,  LONG if z < -z_in  (optional reversion hook:
//      only enter once |z| < |z_prev| -- no falling-knife)
//    exit  when |z| < z_out (revert to mean) OR |z| >= z_stop OR hold timeout
//    size  = vol-target  lot = clamp(target_vol_bps/ATR_bps * lot, .., max)
//  No fixed TP (state-driven exit). shadow_mode default true.
//
//  WARM-SEED (mandate): seed_from_d1_csv() replays a bundled D1 close CSV with
//  enabled=false so the z-window + ATR warm without firing.
//
//  USAGE
//    static omega::FxCrossRevEngine g_fx_xrev_eurgbp("EURGBP");
//    g_fx_xrev_eurgbp.shadow_mode = true; g_fx_xrev_eurgbp.enabled = true;
//    g_fx_xrev_eurgbp.p.z_window = 60; g_fx_xrev_eurgbp.p.z_in = 2.0;
//    g_fx_xrev_eurgbp.seed_from_d1_csv("phase1/signal_discovery/warmup_EURGBP_D1.csv");
//    g_fx_xrev_eurgbp.on_tick(bid, ask, now_ms, ca_on_close);
// =============================================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

struct FxCrossRevParams {
    int     z_window       = 60;
    double  z_in           = 2.0;
    double  z_out          = 0.4;
    double  z_stop         = 3.5;
    int     hold_timeout   = 20;        // D1 bars
    bool    require_hook   = false;     // require |z| reverting before entry
    double  target_vol_bps = 50.0;
    double  max_lot        = 0.10;
    int     atr_period     = 14;
    double  usd_per_pt     = 100000.0;
};

class FxCrossRevEngine {
public:
    bool             shadow_mode = true;
    bool             enabled     = false;
    double           lot         = 0.01;
    FxCrossRevParams p;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit FxCrossRevEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN") { engine_name_ = "FxXRev_" + symbol_; }

    const std::string& symbol() const noexcept { return symbol_; }
    bool has_open_position() const noexcept { return pos_.active; }

    // S-2026-06-09 visibility fix: read-only accessors so the dashboard
    // register_source() can surface this engine's open position (pos_ is
    // private). entry_ts is stored in ms (set from day_ms in open_position).
    bool    pos_is_long()  const noexcept { return pos_.is_long; }
    double  pos_entry()    const noexcept { return pos_.entry_px; }
    double  pos_lot()      const noexcept { return pos_.lot; }
    int64_t pos_entry_ts() const noexcept { return pos_.entry_ts; }

    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = (bid + ask) * 0.5;
        const int64_t day = (now_ms / 86400000LL) * 86400000LL;
        if (!acc_open_) { acc_open_ = true; acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid; return; }
        if (day != acc_day_) {
            on_d1_bar(acc_h_, acc_l_, acc_c_, bid, ask, acc_day_, on_close);
            acc_day_ = day; acc_h_ = acc_l_ = acc_c_ = mid;
        } else {
            if (mid > acc_h_) acc_h_ = mid;
            if (mid < acc_l_) acc_l_ = mid;
            acc_c_ = mid;
        }
    }

    void on_d1_bar(double h, double l, double c, double bid, double ask,
                   int64_t day_ms, OnCloseFn on_close) noexcept {
        last_bid_ = bid; last_ask_ = ask;
        update_atr(h, l, c);
        prev_close_ = c;

        // Compute z from the CLOSED-bar window BEFORE pushing today (no lookahead).
        double z = 0.0; bool have_z = false;
        if ((int)lp_.size() >= p.z_window) {
            double sum = 0.0; for (double v : lp_) sum += v;
            const double mean = sum / lp_.size();
            double var = 0.0; for (double v : lp_) { double d = v - mean; var += d * d; }
            var /= (lp_.size() - 1);
            const double sd = std::sqrt(var);
            if (sd > 0.0) { z = (std::log(c) - mean) / sd; have_z = true; }
        }

        // Manage open position on the z just computed.
        if (pos_.active && have_z) {
            ++pos_.bars_held;
            { double fav=(pos_.is_long?(c-pos_.entry_px):(pos_.entry_px-c)); if(fav>pos_.mfe)pos_.mfe=fav; if(-fav>pos_.mae)pos_.mae=-fav; }  // side-aware excursion
            bool ex = false; const char* why = "";
            if (pos_.is_long) {
                if (z >= -p.z_out)      { ex = true; why = "Z_OUT"; }
                else if (z <= -p.z_stop){ ex = true; why = "Z_STOP"; }
            } else {
                if (z <=  p.z_out)      { ex = true; why = "Z_OUT"; }
                else if (z >=  p.z_stop){ ex = true; why = "Z_STOP"; }
            }
            if (!ex && pos_.bars_held >= p.hold_timeout) { ex = true; why = "TIMEOUT"; }
            if (ex) close_position(bid, ask, day_ms, why, on_close);
        }

        // Entry (only if flat, warm, enabled).
        if (enabled && !pos_.active && have_z && atr_ > 0.0) {
            const bool reverting = std::fabs(z) < std::fabs(z_prev_);
            bool go_long = false, go_short = false;
            if (z >  p.z_in && (!p.require_hook || reverting)) go_short = true;
            if (z < -p.z_in && (!p.require_hook || reverting)) go_long  = true;
            if (go_long || go_short) open_position(go_long, c, bid, ask, day_ms, z);
        }

        // Push today's close into the window.
        lp_.push_back(std::log(c));
        while ((int)lp_.size() > p.z_window) lp_.pop_front();
        if (have_z) z_prev_ = z;
        ++day_count_;
    }

    void force_close(int64_t day_ms, OnCloseFn on_close) noexcept {
        if (!pos_.active) return;
        const double bid = (last_bid_ > 0.0) ? last_bid_ : prev_close_;
        const double ask = (last_ask_ > 0.0) ? last_ask_ : prev_close_;
        close_position(bid, ask, day_ms, "FORCE_CLOSE", on_close);
    }
    void cancel() noexcept { pos_ = Pos{}; }

    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[SEED-FATAL] FxXRev %s: cannot open %s\n", symbol_.c_str(), path.c_str());
            std::fflush(stdout); return 0;
        }
        const bool was = enabled; enabled = false;
        auto null_cb = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line);  // header
        size_t n = 0;
        while (std::getline(f, line)) {
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5) continue;
            if (c <= 0.0) continue;
            int64_t day_ms = (ts > 1e11) ? (int64_t)ts : (int64_t)(ts * 1000.0);
            { int64_t dd=day_ms/86400000LL; int wd=(int)(((dd%7)+4+7)%7); if(wd==6) continue; }  // drop flat Sat (portable, no gmtime_r)
            day_ms = (day_ms / 86400000LL) * 86400000LL;
            const double sp = c * 0.00005;
            on_d1_bar(h, l, c, c - sp, c + sp, day_ms, null_cb);
            ++n;
        }
        enabled = was;
        std::printf("[SEED][FxXRev-%s] %zu D1 bars replayed  atr=%.6f win=%zu -- hot\n",
                    symbol_.c_str(), n, atr_, lp_.size());
        std::fflush(stdout);
        return n;
    }

private:
    struct Pos {
        bool    active     = false;
        bool    is_long    = false;
        double  entry_px   = 0.0;
        double  lot        = 0.0;
        int64_t entry_ts   = 0;
        int     bars_held  = 0;
        double  z_at_entry = 0.0;
        double  mfe = 0.0, mae = 0.0;
    } pos_;

    void update_atr(double h, double l, double c) noexcept {
        if (prev_close_ <= 0.0) { prev_close_ = c; return; }
        const double tr = std::fmax(h - l, std::fmax(std::fabs(h - prev_close_), std::fabs(l - prev_close_)));
        if (atr_warm_ < p.atr_period) { atr_sum_ += tr; if (++atr_warm_ == p.atr_period) atr_ = atr_sum_ / p.atr_period; }
        else atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;
    }
    double sized_lot(double price) const noexcept {
        if (atr_ <= 0.0 || price <= 0.0) return lot;
        const double atr_bps = atr_ / price * 10000.0;
        if (atr_bps <= 0.0) return lot;
        double L = (p.target_vol_bps / atr_bps) * lot;
        if (L < 0.01) L = 0.01; if (L > p.max_lot) L = p.max_lot;
        return L;
    }
    void open_position(bool is_long, double close_px, double bid, double ask,
                       int64_t day_ms, double z) noexcept {
        const double L = sized_lot(close_px);
        // cost gate: 1-ATR expected reversion proxy
        if (atr_ > 0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(), ask - bid, atr_, L, 1.5)) return;
        pos_ = Pos{};
        pos_.active = true; pos_.is_long = is_long;
        pos_.entry_px = is_long ? ask : bid;
        pos_.lot = L;
        pos_.entry_ts = day_ms; pos_.z_at_entry = z;
        std::printf("[FxXRev-%s] ENTRY %s z=%+.2f px=%.5f lot=%.3f%s\n",
                    symbol_.c_str(), is_long ? "LONG" : "SHORT", z, pos_.entry_px, pos_.lot,
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }
    void close_position(double bid, double ask, int64_t day_ms, const char* why,
                        OnCloseFn on_close) noexcept {
        if (!pos_.active) return;
        const double exit_px = pos_.is_long ? bid : ask;
        const double dir = pos_.is_long ? 1.0 : -1.0;
        const double price_bp = dir * (exit_px - pos_.entry_px) / pos_.entry_px * 10000.0;
        const double notional = pos_.lot * p.usd_per_pt;
        const double pnl_usd  = price_bp / 10000.0 * notional;
        const double spread   = std::fabs(ask - bid);
        const double cost_usd = spread / pos_.entry_px * notional;
        std::printf("[FxXRev-%s] EXIT %s reason=%s z_in=%+.2f price_bp=%+.1f pnl=%.2f bars=%d%s\n",
                    symbol_.c_str(), pos_.is_long ? "LONG" : "SHORT", why, pos_.z_at_entry,
                    price_bp, pnl_usd, pos_.bars_held, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);

        omega::TradeRecord tr{};
        tr.symbol = symbol_; tr.side = pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos_.entry_px; tr.exitPrice = exit_px;
        tr.size = pos_.lot; tr.pnl = pnl_usd; tr.net_pnl = pnl_usd - cost_usd;
        tr.entryTs = pos_.entry_ts / 1000; tr.exitTs = day_ms / 1000;
        tr.engine = engine_name_; tr.exitReason = why;
        tr.spreadAtEntry = spread; tr.shadow = shadow_mode;
        tr.mfe = pos_.mfe; tr.mae = pos_.mae;
        if (on_close) on_close(tr);
        pos_ = Pos{};
    }

    std::string symbol_, engine_name_;
    std::deque<double> lp_;            // rolling log-closes
    double z_prev_ = 0.0;
    bool    acc_open_ = false; int64_t acc_day_ = 0;
    double  acc_h_ = 0, acc_l_ = 0, acc_c_ = 0, last_bid_ = 0, last_ask_ = 0;
    double  atr_ = 0.0, atr_sum_ = 0.0; int atr_warm_ = 0;
    double  prev_close_ = 0.0; int day_count_ = 0;
};

} // namespace omega
