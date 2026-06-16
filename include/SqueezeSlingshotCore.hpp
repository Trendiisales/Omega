#pragma once
// SqueezeSlingshotCore.hpp
// Dependency-free C++17 signal core for the "Slingshot Squeeze" = TTM Squeeze
// (Bollinger-inside-Keltner compression + linreg momentum) gated by an EMA trend stack.
//
// This is the STRATEGY LOGIC ONLY. It is intentionally decoupled from the Omega CRTP
// harness: it owns no fills, PnL, or position state. The CRTP engine wrapper feeds it
// closed bars via update() and acts on the emitted signals. Every value returned by
// update() is computed strictly from bars up to and including the current one, so a
// signal on bar t never depends on t+1 (verified by the accompanying lookahead test).
//
// Faithful to LazyBear's "Squeeze Momentum" formulation, with Carter's TTM Squeeze Pro
// multi-Keltner compression tiers:
//   BB:        basis = SMA(close, bb_length); band = bb_mult * stdev_pop(close, bb_length)
//   KC range:  rangema = SMA(TR or High-Low, kc_length)
//   KC bands:  SMA(close, kc_length) +/- mult * rangema, for mult in {low, mid, high}
//   squeeze:   lowerBB > lowerKC AND upperBB < upperKC (tighter KC mult => higher tier)
//   momentum:  linreg( close - avg(donchian_mid(mom_length), SMA(close, mom_length)), mom_length )
//   trend:     EMA(fast) > EMA(a) > EMA(b) > EMA(slow)  => +1 up  (reverse => -1)
//
// stdev is population (divisor n) to match Bollinger/Pine ta.stdev. linreg follows Pine
// ta.linreg(src, length, 0): the fitted value at the most recent bar.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>

namespace squeeze {

struct Bar {
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

struct Params {
    // Bollinger Bands
    int bb_length = 20;
    double bb_mult = 2.0;

    // Keltner Channels (compression tiers; tighter mult = harder to be inside = higher tier)
    int kc_length = 20;
    double kc_mult_low = 2.0;   // tier 1
    double kc_mult_mid = 1.5;   // tier 2
    double kc_mult_high = 1.0;  // tier 3 (rarest / strongest compression)
    bool kc_use_true_range = true;

    // Momentum (linreg of close minus midline)
    int mom_length = 20;

    // EMA trend stack
    int ema_fast = 8;
    int ema_a = 21;
    int ema_b = 34;
    int ema_slow = 55;

    // Entry gating
    bool require_squeeze_on = true;            // must still be compressed at entry
    int min_tier = 1;                          // minimum compression tier (1..3)
    bool require_momo_below_zero_for_long = false;  // false: momentum simply rising (sensible default).
    // true => stricter "before the fire": momentum still < 0 but rising (mirror > 0 falling for shorts).
    // This is deliberately a sweep parameter; the strict variant tends to over-constrain trades.

    // ATR for stops/targets (Wilder); levels are applied by the harness to the fill price
    int atr_length = 14;
    double atr_stop_mult = 2.0;
    double atr_target_mult = 4.0;  // 0 disables target

    // Exit hints (the harness owns the actual position; these are convenience signals)
    bool exit_on_momo_rollover = true;
    bool exit_on_squeeze_fire = false;
    int max_hold_bars = 0;  // 0 disables; enforced by the harness
};

enum class Signal { None = 0, EnterLong = 1, EnterShort = -1 };

struct BarSignal {
    bool warm = false;  // false until enough history for every component

    // Bollinger
    double basis = 0.0, upperBB = 0.0, lowerBB = 0.0;

    // Keltner
    double kc_mid = 0.0, rangema = 0.0;
    double upperKC_low = 0.0, lowerKC_low = 0.0;
    double upperKC_mid = 0.0, lowerKC_mid = 0.0;
    double upperKC_high = 0.0, lowerKC_high = 0.0;

    int squeeze_tier = 0;     // 0 none, 1 low(2.0), 2 mid(1.5), 3 high(1.0)
    bool squeeze_on = false;

    // Momentum
    double momentum = 0.0;
    double momentum_prev = 0.0;
    bool momo_up = false;     // momentum > previous momentum

    // Trend
    double ema_fast = 0.0, ema_a = 0.0, ema_b = 0.0, ema_slow = 0.0;
    int trend = 0;            // +1 up stack, -1 down stack, 0 none

    // Volatility
    double atr = 0.0;

    // Decision (evaluated on THIS closed bar; execute at next bar's open)
    Signal entry = Signal::None;
};

namespace detail {

// Incremental EMA seeded with an SMA of the first `n` samples.
struct Ema {
    int n = 1;
    double value = 0.0;
    bool ready = false;
    int count = 0;
    double seed_sum = 0.0;

    void reset(int length) {
        n = length < 1 ? 1 : length;
        value = 0.0;
        ready = false;
        count = 0;
        seed_sum = 0.0;
    }

    void update(double x) {
        if (!ready) {
            seed_sum += x;
            ++count;
            if (count >= n) {
                value = seed_sum / static_cast<double>(n);
                ready = true;
            }
            return;
        }
        const double k = 2.0 / (static_cast<double>(n) + 1.0);
        value = x * k + value * (1.0 - k);
    }
};

// Wilder ATR seeded with an SMA of the first `n` true ranges.
struct WilderAtr {
    int n = 1;
    double value = 0.0;
    bool ready = false;
    int count = 0;
    double seed_sum = 0.0;

    void reset(int length) {
        n = length < 1 ? 1 : length;
        value = 0.0;
        ready = false;
        count = 0;
        seed_sum = 0.0;
    }

    void update(double tr) {
        if (!ready) {
            seed_sum += tr;
            ++count;
            if (count >= n) {
                value = seed_sum / static_cast<double>(n);
                ready = true;
            }
            return;
        }
        value = (value * (static_cast<double>(n) - 1.0) + tr) / static_cast<double>(n);
    }
};

inline double sma_back(const std::deque<double>& q, int n) {
    const int m = static_cast<int>(q.size());
    if (m < n) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for (int i = m - n; i < m; ++i) s += q[static_cast<std::size_t>(i)];
    return s / static_cast<double>(n);
}

inline double stdev_pop_back(const std::deque<double>& q, int n, double mean) {
    const int m = static_cast<int>(q.size());
    if (m < n) return std::numeric_limits<double>::quiet_NaN();
    double s2 = 0.0;
    for (int i = m - n; i < m; ++i) {
        const double d = q[static_cast<std::size_t>(i)] - mean;
        s2 += d * d;
    }
    return std::sqrt(s2 / static_cast<double>(n));
}

inline double highest_back(const std::deque<double>& q, int n) {
    const int m = static_cast<int>(q.size());
    double h = -std::numeric_limits<double>::infinity();
    for (int i = m - n; i < m; ++i) h = std::max(h, q[static_cast<std::size_t>(i)]);
    return h;
}

inline double lowest_back(const std::deque<double>& q, int n) {
    const int m = static_cast<int>(q.size());
    double l = std::numeric_limits<double>::infinity();
    for (int i = m - n; i < m; ++i) l = std::min(l, q[static_cast<std::size_t>(i)]);
    return l;
}

// Pine ta.linreg(src, length, 0): least-squares fit over the last `length` points
// (x = 0 oldest .. length-1 newest), returning the fitted value at the newest point.
inline double linreg_last(const std::deque<double>& q, int n) {
    const int m = static_cast<int>(q.size());
    if (m < n) return std::numeric_limits<double>::quiet_NaN();
    const double N = static_cast<double>(n);
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        const double y = q[static_cast<std::size_t>(m - n + i)];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double denom = N * sxx - sx * sx;
    if (std::fabs(denom) < 1e-12) return sy / N;
    const double slope = (N * sxy - sx * sy) / denom;
    const double intercept = (sy - slope * sx) / N;
    return intercept + slope * (N - 1.0);
}

}  // namespace detail

class Evaluator {
public:
    explicit Evaluator(const Params& p) : p_(p) {
        if (p_.bb_length < 2 || p_.kc_length < 2 || p_.mom_length < 2 || p_.atr_length < 1) {
            throw std::invalid_argument("SqueezeSlingshotCore: lengths must be >= 2 (atr >= 1).");
        }
        ema_fast_.reset(p_.ema_fast);
        ema_a_.reset(p_.ema_a);
        ema_b_.reset(p_.ema_b);
        ema_slow_.reset(p_.ema_slow);
        atr_.reset(p_.atr_length);

        hist_cap_ = std::max({p_.bb_length, p_.kc_length, p_.mom_length, p_.atr_length}) + 2;
        // momentum needs mom_length raw values, each of which needs mom_length history.
        warmup_needed_ = std::max({p_.bb_length, p_.kc_length, p_.ema_slow, p_.atr_length,
                                   2 * p_.mom_length});
    }

    BarSignal update(const Bar& b) {
        // True range (first bar: high - low).
        const double tr = have_prev_close_
                              ? std::max({b.high - b.low,
                                          std::fabs(b.high - prev_close_),
                                          std::fabs(b.low - prev_close_)})
                              : (b.high - b.low);

        const double range_src = p_.kc_use_true_range ? tr : (b.high - b.low);

        push_(closes_, b.close);
        push_(highs_, b.high);
        push_(lows_, b.low);
        push_(ranges_, range_src);

        ema_fast_.update(b.close);
        ema_a_.update(b.close);
        ema_b_.update(b.close);
        ema_slow_.update(b.close);
        atr_.update(tr);

        // Per-bar momentum raw = close - avg(donchian_mid(mom_length), SMA(close, mom_length)).
        double mom_raw = std::numeric_limits<double>::quiet_NaN();
        if (static_cast<int>(closes_.size()) >= p_.mom_length) {
            const double dch_mid =
                (detail::highest_back(highs_, p_.mom_length) +
                 detail::lowest_back(lows_, p_.mom_length)) * 0.5;
            const double sma_c = detail::sma_back(closes_, p_.mom_length);
            mom_raw = b.close - 0.5 * (dch_mid + sma_c);
        }
        if (std::isfinite(mom_raw)) {
            push_(mom_raw_, mom_raw);
        }

        prev_close_ = b.close;
        have_prev_close_ = true;
        ++bar_count_;

        BarSignal s;
        s.warm = (bar_count_ >= warmup_needed_) && ema_slow_.ready && atr_.ready &&
                 static_cast<int>(mom_raw_.size()) >= p_.mom_length;

        s.ema_fast = ema_fast_.value;
        s.ema_a = ema_a_.value;
        s.ema_b = ema_b_.value;
        s.ema_slow = ema_slow_.value;
        s.atr = atr_.value;

        if (!s.warm) {
            prev_momentum_ = s.momentum;
            have_prev_momentum_ = true;
            long_cond_prev_ = false;
            short_cond_prev_ = false;
            return s;
        }

        // Bollinger.
        s.basis = detail::sma_back(closes_, p_.bb_length);
        const double dev = p_.bb_mult * detail::stdev_pop_back(closes_, p_.bb_length, s.basis);
        s.upperBB = s.basis + dev;
        s.lowerBB = s.basis - dev;

        // Keltner (three compression tiers).
        s.kc_mid = detail::sma_back(closes_, p_.kc_length);
        s.rangema = detail::sma_back(ranges_, p_.kc_length);
        s.upperKC_low = s.kc_mid + p_.kc_mult_low * s.rangema;
        s.lowerKC_low = s.kc_mid - p_.kc_mult_low * s.rangema;
        s.upperKC_mid = s.kc_mid + p_.kc_mult_mid * s.rangema;
        s.lowerKC_mid = s.kc_mid - p_.kc_mult_mid * s.rangema;
        s.upperKC_high = s.kc_mid + p_.kc_mult_high * s.rangema;
        s.lowerKC_high = s.kc_mid - p_.kc_mult_high * s.rangema;

        auto inside = [&](double up_kc, double lo_kc) {
            return (s.lowerBB > lo_kc) && (s.upperBB < up_kc);
        };
        if (inside(s.upperKC_high, s.lowerKC_high)) s.squeeze_tier = 3;
        else if (inside(s.upperKC_mid, s.lowerKC_mid)) s.squeeze_tier = 2;
        else if (inside(s.upperKC_low, s.lowerKC_low)) s.squeeze_tier = 1;
        else s.squeeze_tier = 0;
        s.squeeze_on = s.squeeze_tier >= 1;

        // Momentum.
        s.momentum = detail::linreg_last(mom_raw_, p_.mom_length);
        s.momentum_prev = have_prev_momentum_ ? prev_momentum_ : s.momentum;
        s.momo_up = s.momentum > s.momentum_prev;

        // Trend stack.
        if (s.ema_fast > s.ema_a && s.ema_a > s.ema_b && s.ema_b > s.ema_slow) s.trend = 1;
        else if (s.ema_fast < s.ema_a && s.ema_a < s.ema_b && s.ema_b < s.ema_slow) s.trend = -1;
        else s.trend = 0;

        // Entry conditions (rising-edge triggered).
        const bool tier_ok = s.squeeze_tier >= p_.min_tier;
        const bool sqz_ok = (!p_.require_squeeze_on) || s.squeeze_on;

        const bool long_cond =
            sqz_ok && tier_ok && (s.trend == 1) && s.momo_up &&
            (!p_.require_momo_below_zero_for_long || s.momentum < 0.0);

        const bool short_cond =
            sqz_ok && tier_ok && (s.trend == -1) && (s.momentum < s.momentum_prev) &&
            (!p_.require_momo_below_zero_for_long || s.momentum > 0.0);

        if (long_cond && !long_cond_prev_) s.entry = Signal::EnterLong;
        else if (short_cond && !short_cond_prev_) s.entry = Signal::EnterShort;
        else s.entry = Signal::None;

        long_cond_prev_ = long_cond;
        short_cond_prev_ = short_cond;
        prev_momentum_ = s.momentum;
        have_prev_momentum_ = true;
        return s;
    }

private:
    void push_(std::deque<double>& q, double v) {
        q.push_back(v);
        while (static_cast<int>(q.size()) > hist_cap_) q.pop_front();
    }

    Params p_;
    std::deque<double> closes_, highs_, lows_, ranges_, mom_raw_;
    detail::Ema ema_fast_, ema_a_, ema_b_, ema_slow_;
    detail::WilderAtr atr_;

    double prev_close_ = 0.0;
    bool have_prev_close_ = false;
    double prev_momentum_ = 0.0;
    bool have_prev_momentum_ = false;
    bool long_cond_prev_ = false;
    bool short_cond_prev_ = false;

    long long bar_count_ = 0;
    int hist_cap_ = 64;
    int warmup_needed_ = 64;
};

// ---- Convenience level/exit helpers (the harness applies these to the actual fill) ----

inline double long_stop_price(double fill, double atr, const Params& p) {
    return fill - atr * p.atr_stop_mult;
}
inline double long_target_price(double fill, double atr, const Params& p) {
    return p.atr_target_mult > 0.0 ? fill + atr * p.atr_target_mult
                                   : std::numeric_limits<double>::quiet_NaN();
}
inline double short_stop_price(double fill, double atr, const Params& p) {
    return fill + atr * p.atr_stop_mult;
}
inline double short_target_price(double fill, double atr, const Params& p) {
    return p.atr_target_mult > 0.0 ? fill - atr * p.atr_target_mult
                                   : std::numeric_limits<double>::quiet_NaN();
}

// Momentum-rollover / squeeze-fire exit hint for an open long (mirror logic for shorts).
inline bool slingshot_long_exit(const BarSignal& prev, const BarSignal& cur, const Params& p) {
    bool exit = false;
    if (p.exit_on_momo_rollover && cur.momentum < prev.momentum) exit = true;
    if (p.exit_on_squeeze_fire && prev.squeeze_on && !cur.squeeze_on) exit = true;
    return exit;
}
inline bool slingshot_short_exit(const BarSignal& prev, const BarSignal& cur, const Params& p) {
    bool exit = false;
    if (p.exit_on_momo_rollover && cur.momentum > prev.momentum) exit = true;
    if (p.exit_on_squeeze_fire && prev.squeeze_on && !cur.squeeze_on) exit = true;
    return exit;
}

}  // namespace squeeze
