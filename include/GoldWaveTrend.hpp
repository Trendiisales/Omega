#pragma once
// ============================================================================
// GoldWaveTrend.hpp -- live WaveTrend momentum-confirm gate for gold (S-2026-06-03)
// ----------------------------------------------------------------------------
// Ports the validated X1 momentum-confirm FILTER (incidents/2026-06-02-x1-overlay-
// validation) into the live engine. Computes the LazyBear WaveTrend oscillator +
// EMA21/EMA55 regime on the gold M1 series, tags momentum_up/down (wt cross AND
// regime aligned), and exposes confirms(is_long): was there a confirming momentum
// tag in the last `lookback` M1 bars? Validated edge: gold confirmed-trend winners
// 71.9% vs losers 51.5% (+12pp within-trend, ~2 SE). GOLD-ONLY (indices invert).
//
// Math is byte-for-byte the offline validator (x1_validate.py):
//   ap   = hlc3
//   esa  = ema(ap, 10);  d = ema(|ap-esa|, 10)
//   ci   = (ap - esa) / (0.015 * d);  wt1 = ema(ci, 21);  wt2 = sma(wt1, 4)
//   regime_up = ema(close,21) > ema(close,55)
//   momentum_up   = (wt1 x-up wt2)   AND regime_up
//   momentum_down = (wt1 x-down wt2) AND !regime_up
//
// Gate semantics: confirms() FAILS OPEN (returns true) until warmup completes, so
// a restart never blocks every gold entry for ~55 bars. Shadow-safe; gate only
// FILTERS entries when g_gold_wt.gate_enabled is set.
// ============================================================================
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace omega {

struct GoldWaveTrend {
    // --- config ---
    bool enabled      = true;   // compute the oscillator
    bool gate_enabled = true;   // actually filter entries on confirms()
    int  lookback     = 10;     // M1 bars before entry to look for a confirming tag
    int  warmup_bars  = 60;     // confirms() fails OPEN until this many M1 bars seen

    // --- state (guarded by mu_) ---
    double esa_ = 0.0, dd_ = 0.0, wt1_ = 0.0;
    double ema_fast_ = 0.0, ema_slow_ = 0.0;       // regime EMAs on close
    std::array<double, 4> wt1_hist_{{0, 0, 0, 0}}; // for wt2 = sma(wt1,4)
    int    wt1_n_ = 0;
    double prev_wt1_ = 0.0, prev_wt2_ = 0.0;
    bool   init_ = false;
    long   bars_ = 0;

    static constexpr int RING = 16;                // >= lookback
    std::array<bool, RING> mom_up_{};
    std::array<bool, RING> mom_dn_{};
    int ring_head_ = 0;

    mutable std::mutex mu_;

    // --- gate counters (for the daily summary) ---
    std::atomic<long> n_pass_{0};   // entries the gate allowed (confirmed)
    std::atomic<long> n_skip_{0};   // entries the gate blocked (no confirm)
    void record_pass() noexcept { n_pass_.fetch_add(1, std::memory_order_relaxed); }
    void record_skip() noexcept { n_skip_.fetch_add(1, std::memory_order_relaxed); }
    long passes() const noexcept { return n_pass_.load(std::memory_order_relaxed); }
    long skips()  const noexcept { return n_skip_.load(std::memory_order_relaxed); }

    static double ema_step(double prev, double x, int n) {
        const double k = 2.0 / (n + 1);
        return prev + k * (x - prev);
    }

    // Feed one CLOSED gold M1 bar (high, low, close).
    void on_m1_close(double h, double l, double c) noexcept {
        if (!enabled) return;
        std::lock_guard<std::mutex> lk(mu_);
        const double ap = (h + l + c) / 3.0;
        if (!init_) {
            esa_ = ap; dd_ = 0.0; wt1_ = 0.0;
            ema_fast_ = c; ema_slow_ = c;
            wt1_hist_.fill(0.0); wt1_n_ = 0;
            prev_wt1_ = 0.0; prev_wt2_ = 0.0;
            init_ = true; bars_ = 1;
            return;
        }
        esa_ = ema_step(esa_, ap, 10);
        dd_  = ema_step(dd_, std::fabs(ap - esa_), 10);
        const double d  = dd_ > 1e-12 ? dd_ : 1e-12;
        const double ci = (ap - esa_) / (0.015 * d);
        wt1_ = ema_step(wt1_, ci, 21);

        // wt2 = sma(wt1, 4)
        wt1_hist_[bars_ % 4] = wt1_;
        if (wt1_n_ < 4) ++wt1_n_;
        double sum = 0.0; for (int i = 0; i < wt1_n_; ++i) sum += wt1_hist_[i];
        const double wt2 = sum / wt1_n_;

        ema_fast_ = ema_step(ema_fast_, c, 21);
        ema_slow_ = ema_step(ema_slow_, c, 55);
        const bool regime_up = ema_fast_ > ema_slow_;

        const bool cross_up = (wt1_ > wt2) && (prev_wt1_ <= prev_wt2_);
        const bool cross_dn = (wt1_ < wt2) && (prev_wt1_ >= prev_wt2_);
        const bool mom_up = cross_up && regime_up;
        const bool mom_dn = cross_dn && !regime_up;

        ring_head_ = (ring_head_ + 1) % RING;
        mom_up_[ring_head_] = mom_up;
        mom_dn_[ring_head_] = mom_dn;

        prev_wt1_ = wt1_; prev_wt2_ = wt2;
        ++bars_;
    }

    // Was a confirming momentum tag present in the last `lookback` M1 bars?
    // FAILS OPEN (true) during warmup so it never blocks all entries on restart.
    bool confirms(bool is_long) const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        if (bars_ < warmup_bars) return true;
        const int n = lookback < RING ? lookback : RING - 1;
        for (int i = 0; i < n; ++i) {
            const int idx = ((ring_head_ - i) % RING + RING) % RING;
            if (is_long ? mom_up_[idx] : mom_dn_[idx]) return true;
        }
        return false;
    }

    // For per-entry logging.
    double wt1() const noexcept { std::lock_guard<std::mutex> lk(mu_); return wt1_; }
    bool regime_up() const noexcept { std::lock_guard<std::mutex> lk(mu_); return ema_fast_ > ema_slow_; }
    long bars_seen() const noexcept { std::lock_guard<std::mutex> lk(mu_); return bars_; }
    bool warm() const noexcept { std::lock_guard<std::mutex> lk(mu_); return bars_ >= warmup_bars; }
};

// Single shared instance, accessor pattern (mirrors gold_d1_trend()). Lets any
// engine header reference the gate via #include without an include-order or
// global-linkage dependency (and keeps the canary standalone-compile happy).
inline GoldWaveTrend& gold_wt() {
    static GoldWaveTrend inst;
    return inst;
}

} // namespace omega
