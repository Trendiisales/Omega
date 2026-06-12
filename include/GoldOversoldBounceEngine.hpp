#pragma once
// ============================================================================
// GoldOversoldBounceEngine.hpp -- XAUUSD short-term capitulation bounce (S-2026-06-03)
// ----------------------------------------------------------------------------
// Edge (incidents/2026-06-02-x1-overlay-validation/ search, this session):
//   Daily RSI(14) deep-oversold (<30) on gold -> mean-reverting bounce. 18yr
//   GC=F (2008-2026, incl the 2013 -28% bear): entry RSI<30 long, exit RSI>50
//   or 20 D1 bars, -ATR stop:
//     t2.76 PF2.17 win73% n85, 14/19 years +, ann ~+4.8%/yr (non-overlapping).
//   REGIME-ROBUST -- the critical test: POSITIVE even in the 2013/15/22 bear
//   windows (+0.42% t1.75), where the NAIVE below-50ma dip-buy DIES (-0.48%
//   t-3.13, falling-knife). RSI<30 = short-term capitulation, bounces in bull
//   AND bear; the -ATR stop contains the bad bear years (-1..-5.6%).
//   UNCORRELATED with the trend book (buys WEAKNESS, not breakouts) and
//   GoldSeasonal (calendar). Low freq ~5 trades/yr -> portfolio diversifier.
//
// Long-only (no shorting gold). Tick-driven: aggregates its own UTC-daily bar;
// on a new day finalizes the prior close, updates RSI/ATR, then exits (RSI>exit
// or max-hold) and arms a new entry (RSI<entry). Intraday: hard -ATR stop +
// spread-gated fill (skips the gold ~21:00 UTC break / illiquid ticks).
// Warm-seed via seed_from_d1_csv. shadow default.
//
// Portable weekday not needed (no calendar gate). RSI is SMA-variant (sum of
// gains/losses over rsi_period) to match the validation harness exactly.
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <fstream>
#include <string>
#include <algorithm>
#include "OmegaTradeLedger.hpp"   // omega::TradeRecord
#include "RegimeState.hpp"        // 2026-06-12: shared price-based bull/bear gate
#include "OmegaCostGuard.hpp"     // ExecutionCostGuard::is_viable entry gate
#include "OpenPositionRegistry.hpp" // S-2026-06-03: omega::PositionSnapshot for persist

namespace omega {

struct GoldOversoldBounceEngine {
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double usd_per_pt  = 100.0;          // XAUUSD: $100 per 1.0 price-pt per lot
    double max_spread  = 1.0;            // skip break/illiquid fills
    // signal params (match the 18yr validation)
    double entry_rsi   = 30.0;           // enter long when daily RSI < this
    double exit_rsi    = 50.0;           // exit when daily RSI recovers above this
    int    rsi_period  = 14;
    int    atr_period  = 14;
    int    max_hold_days = 20;           // hard time cap
    double stop_atr_mult = 2.5;          // hard stop = entry - mult*ATR (~ -4%)

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    bool has_open_position() const noexcept { return pos_.active; }
    double  pos_entry() const noexcept { return pos_.entry_px; }
    double  pos_lot()   const noexcept { return pos_.lot; }
    double  pos_stop()  const noexcept { return pos_.stop_px; }
    int64_t pos_entry_ts_ms() const noexcept { return pos_.entry_ts; }

    // S-2026-06-03: open-position persistence across restart. Long-only; the hard
    // ATR stop maps to sl, no fixed TP (RSI/max-hold exit) -> tp=0. pos_ is private
    // but these members can touch it. pos_.entry_ts is epoch ms.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos_.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG";
        o.size = pos_.lot; o.entry = pos_.entry_px; o.sl = pos_.stop_px; o.tp = 0.0;
        o.entry_ts = pos_.entry_ts / 1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        // FIXED-LOT engine: re-assert the configured lot, NEVER trust the
        // snapshot size. A stale open_positions.dat once carried size=1.0 and
        // restored over the 0.01 config -> a +$1589 shadow trade on a +15.89pt
        // move (100x). The engine's lot is config, not position state. (2026-06-04)
        pos_.active = true; pos_.entry_px = ps.entry; pos_.lot = lot;
        pos_.stop_px = ps.sl; pos_.entry_ts = ps.entry_ts * 1000;
        return true;
    }

    const std::string symbol = "XAUUSD";

    void on_tick(double bid, double ask, int64_t ts_ms, OnCloseFn cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = 0.5 * (bid + ask);
        const int64_t day = ts_ms / 86400000LL;

        if (day != cur_day_) {
            // Only finalize a day we actually built from ticks. After warm-seed
            // (or first boot) day_close_==0 -> skip, else we'd push a garbage
            // (0,0,0) bar that corrupts RSI/ATR and fires a spurious entry.
            if (cur_day_ >= 0 && day_close_ > 0.0) _finalize_day();
            // ---- new UTC day: update indicators, manage, arm ----
            if (pos_.active) pos_.hold_days += 1;
            const double r = rsi();
            // exit on RSI recovery or time cap (at the fresh new-day tick)
            if (pos_.active && (r > exit_rsi || pos_.hold_days >= max_hold_days)) {
                _close(bid, ask, ts_ms,
                       (r > exit_rsi ? "RSI_RECOVER" : "MAX_HOLD"), cb);
            }
            // arm a new entry if deep-oversold and flat
            want_entry_ = enabled && !pos_.active && _ready() && (r < entry_rsi);
            // start the new day's bar
            cur_day_ = day; day_hi_ = day_lo_ = day_close_ = mid;
        } else {
            if (mid > day_hi_) day_hi_ = mid;
            if (mid < day_lo_) day_lo_ = mid;
            day_close_ = mid;
        }

        // intraday hard stop (mean-reversion can keep falling -> cap it)
        if (pos_.active && pos_.stop_px > 0.0 && bid <= pos_.stop_px) {
            _close(bid, ask, ts_ms, "ATR_STOP", cb);
            return;
        }
        // fill an armed entry on the first tradeable (non-break) tick
        if (want_entry_ && !pos_.active) {
            const double sp = ask - bid;
            if (sp > 0.0 && sp <= max_spread) { _open(ask, sp, ts_ms); want_entry_ = false; }
        }
    }

    void force_close(int64_t ts_ms, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double b = last_bid_ > 0 ? last_bid_ : pos_.entry_px;
        const double a = last_ask_ > 0 ? last_ask_ : pos_.entry_px;
        _close(b, a, ts_ms, "FORCE_CLOSE", cb);
    }

    // Warm-seed: replay a daily CSV (ts,o,h,l,c) directly into the indicator
    // deques so RSI/ATR are hot at boot. enabled forced off (no entries fire).
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] GoldOversoldBounce: cannot open %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string line; std::getline(f, line); size_t n = 0;
        while (std::getline(f, line)) {
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5) continue;
            if (c <= 0.0 || h <= 0.0 || l <= 0.0) continue;
            const int64_t ts_ms = (ts > 1e11) ? (int64_t)ts : (int64_t)(ts * 1000.0);
            _ingest_daily(h, l, c);
            cur_day_ = ts_ms / 86400000LL;
            ++n;
        }
        std::printf("[SEED][GoldOversoldBounce] %zu daily bars replayed, RSI=%.1f ATR=%.2f -- hot\n",
                    n, rsi(), atr());
        std::fflush(stdout);
        return n;
    }

private:
    struct Pos { bool active=false; double entry_px=0, lot=0, stop_px=0; int64_t entry_ts=0; int hold_days=0; } pos_;
    int64_t cur_day_ = -1;
    double  last_bid_ = 0, last_ask_ = 0;
    double  day_hi_ = 0, day_lo_ = 0, day_close_ = 0;
    double  prev_close_ = 0;
    bool    want_entry_ = false;
    std::deque<double> closes_;   // finalized daily closes
    std::deque<double> tr_;       // finalized daily true ranges
    std::string engine_name_ = "GoldOversoldBounce";

    void _finalize_day() noexcept { _ingest_daily(day_hi_, day_lo_, day_close_); }

    // push one finalized daily bar into the indicator deques
    void _ingest_daily(double hi, double lo, double close) noexcept {
        double tr = hi - lo;
        if (prev_close_ > 0.0) {
            tr = std::max(tr, std::fabs(hi - prev_close_));
            tr = std::max(tr, std::fabs(lo - prev_close_));
        }
        closes_.push_back(close);
        tr_.push_back(tr);
        prev_close_ = close;
        const size_t cap = (size_t)std::max(rsi_period, atr_period) + 5;
        while (closes_.size() > cap) closes_.pop_front();
        while (tr_.size()    > cap) tr_.pop_front();
    }

    bool _ready() const noexcept { return (int)closes_.size() >= rsi_period + 1; }

    double rsi() const noexcept {
        if (!_ready()) return 50.0;
        double g = 0, l = 0;
        const size_t start = closes_.size() - rsi_period;
        for (size_t k = start; k < closes_.size(); ++k) {
            const double d = closes_[k] - closes_[k - 1];
            if (d > 0) g += d; else l += -d;
        }
        const double rs = g / (l > 0 ? l : 1e-9);
        return 100.0 - 100.0 / (1.0 + rs);
    }

    double atr() const noexcept {
        if (tr_.empty()) return 0.0;
        const size_t n = std::min(tr_.size(), (size_t)atr_period);
        double s = 0; for (size_t k = tr_.size() - n; k < tr_.size(); ++k) s += tr_[k];
        return s / (double)n;
    }

    void _open(double ask, double spread, int64_t ts_ms) noexcept {
        // 2026-06-12 regime gate: don't catch a falling knife -- no oversold-bounce
        //   longs in a sustained gold bear. Backtest gold_regime_gate_bt (RSI-oversold
        //   long, XAU H1 2020-23): net -262->-172, bleed cut 35%. Inert in bull.
        if (omega::gold_regime().long_blocked()) return;
        const double a = atr();
        // cost gate: bounce target ~ stop distance (stop_atr_mult * ATR) as gross proxy
        if (a > 0.0 && !ExecutionCostGuard::is_viable(symbol.c_str(), spread, stop_atr_mult * a, lot, 1.5)) return;
        pos_ = Pos{}; pos_.active = true; pos_.entry_px = ask; pos_.lot = lot;
        pos_.entry_ts = ts_ms; pos_.hold_days = 0;
        pos_.stop_px = (a > 0.0) ? (ask - stop_atr_mult * a) : 0.0;
        std::printf("[GoldOversoldBounce] ENTRY LONG px=%.2f rsi=%.1f atr=%.2f stop=%.2f lot=%.3f%s\n",
                    ask, rsi(), a, pos_.stop_px, lot, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }
    void _close(double bid, double ask, int64_t ts_ms, const char* why, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double exit_px = bid;                              // long exits at bid
        // RAW pts*lot ONLY. handle_closed_trade (trade_lifecycle.hpp:192) does
        // tr.pnl *= tick_value_multiplier(symbol) (XAUUSD=100). Multiplying by
        // usd_per_pt HERE too = DOUBLE-100x -> the recurring -$1529 phantom
        // (root-caused 2026-06-05; the prior "stale size=1.0" diagnosis was a
        // misattribution). usd_per_pt is informational only; never use in pnl.
        const double pnl = (exit_px - pos_.entry_px) * pos_.lot;
        const double spread = std::fabs(ask - bid);
        const double cost = spread * pos_.lot;
        omega::TradeRecord tr{};
        tr.symbol = symbol; tr.side = "LONG"; tr.engine = engine_name_;
        tr.exitReason = why; tr.entryPrice = pos_.entry_px; tr.exitPrice = exit_px;
        tr.size = pos_.lot; tr.pnl = pnl; tr.net_pnl = pnl - cost;
        tr.sl = pos_.stop_px; tr.tp = 0.0;
        tr.entryTs = pos_.entry_ts / 1000LL; tr.exitTs = ts_ms / 1000LL;
        tr.spreadAtEntry = spread; tr.shadow = shadow_mode;
        std::printf("[GoldOversoldBounce] EXIT %s pnl=%.2f held=%dd%s\n",
                    why, pnl, pos_.hold_days, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (cb) cb(tr);
        pos_ = Pos{};
    }
};

} // namespace omega
