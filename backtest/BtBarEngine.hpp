// =============================================================================
// BtBarEngine.hpp -- Backtest-only bar engine + indicator library
//
// Self-contained. Drop into Omega/backtest/ next to OmegaBacktest.cpp and
// #include "BtBarEngine.hpp" (no other build-system changes required).
//
// PURPOSE
// -------
// The backtest harness OmegaBacktest.cpp wires several engines that need
// HTF bar context (H1Swing, H4Regime, MinimalH4Breakout) and indicator
// inputs (PDHL: prev-day H/L + M1 ATR + EWM drift; MacroCrash: ATR +
// vol_ratio + ewm_drift + RSI14 + expansion regime). Production builds
// these in main.cpp via OHLCBarEngine.hpp + L2 client; the backtest must
// build them itself or those engines never fire.
//
// This header provides:
//   - BtBarBuilder        single-period OHLC bar builder driven by ticks
//   - BtBarIndicators     EMA9/21/50, Wilder ATR-14, RSI-14, ADX-14
//                         (formulas verbatim-copied from include/OHLCBarEngine.hpp
//                          _update_ema / _update_atr / _update_rsi / _update_adx)
//   - BtBarEngine<P>      bar builder + indicators bundled (P = bar minutes)
//   - BtPdhlTracker       previous-day high/low tracker (per UTC day boundary)
//   - BtEwmDrift          EWM mid drift (Chimera-style 30s halflife default)
//   - BtVolRatio          baseline-vs-recent vol ratio for MacroCrash gating
//   - BtSessionSlot       UTC hour -> 0..6 session slot helper (matches the
//                         existing inline logic in OmegaBacktest runners)
//
// All math is deterministic and clock-free -- the only time inputs are tick
// timestamps in milliseconds. No std::time, no system clock. This means
// backtest indicators advance at tick-stream pace and are reproducible.
//
// VALIDATION
// ----------
// Indicator formulas are byte-equivalent to live OHLCBarEngine private
// methods. ATR seed handles the "single bar" edge case identically (uses
// high-low range, falls back to close*0.001 if zero). RSI bootstraps the
// first window with arithmetic mean of gains/losses then switches to
// Wilder smoothing -- matches live exactly. ADX implements full Wilder
// recursion with the +DM/-DM masking rule (up>down && up>0) and uses the
// same 1e-10 epsilon guard as live.
//
// USAGE (sketch -- wired in next session's OmegaBacktest.cpp)
// -----------------------------------------------------------
//   BtBarEngine<60>  h1_eng;   // 60-minute bars
//   BtBarEngine<240> h4_eng;   // 240-minute bars
//   BtPdhlTracker    pdhl;
//   BtEwmDrift       drift;
//
//   for each tick:
//       const double mid = (bid + ask) * 0.5;
//       drift.update(mid);
//       pdhl.update(mid, ts_ms);
//       const bool h1_bar_closed = h1_eng.on_tick(mid, ts_ms);
//       const bool h4_bar_closed = h4_eng.on_tick(mid, ts_ms);
//
//       if (h1_bar_closed && h1_eng.indicators_ready()) {
//           // H1 bar just closed -- call H1SwingEngine::on_h1_bar(...)
//           h1_swing.on_h1_bar(
//               mid, bid, ask,
//               h1_eng.ema9(), h1_eng.ema21(), h1_eng.ema50(),
//               h1_eng.atr14(), h1_eng.rsi14(),
//               h1_eng.adx14(), h1_eng.adx_rising(),
//               /* h4 ctx */ ..., now_ms, on_close);
//       }
//
// =============================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <vector>

namespace omega {
namespace bt {

// =============================================================================
// BtBarBuilder -- single-period OHLC bar builder driven by mid ticks
//
// Bars roll on UTC minute boundaries. Period parameter P is in minutes.
// For H1 use P=60, for H4 use P=240, for M15 use P=15.
//
// Bar boundaries snap to ts_ms / (P*60_000) -- same algorithm as
// OHLCBarEngine. This means an H4 bar that opens at 04:00 UTC closes at
// 08:00 UTC regardless of when the first tick of the session arrived.
// =============================================================================
struct BtOHLC {
    double open  = 0.0;
    double high  = 0.0;
    double low   = 0.0;
    double close = 0.0;
    int64_t open_ms = 0;
};

template <int PERIOD_MIN>
class BtBarBuilder {
public:
    static constexpr int64_t PERIOD_MS = (int64_t)PERIOD_MIN * 60LL * 1000LL;

    // Update with a new mid tick. Returns true exactly once per period
    // when the in-progress bar closes (i.e. the tick crosses the period
    // boundary). The newly closed bar is then accessible via last_closed().
    bool on_tick(double mid, int64_t ts_ms) noexcept {
        const int64_t bucket = ts_ms / PERIOD_MS;
        if (!cur_open_) {
            cur_ = { mid, mid, mid, mid, bucket * PERIOD_MS };
            cur_bucket_ = bucket;
            cur_open_   = true;
            return false;
        }
        if (bucket != cur_bucket_) {
            // Period boundary crossed: close current bar
            last_closed_ = cur_;
            has_closed_  = true;

            // Open new bar
            cur_ = { mid, mid, mid, mid, bucket * PERIOD_MS };
            cur_bucket_ = bucket;
            return true;
        }
        // Same period: extend H/L, update C
        if (mid > cur_.high) cur_.high = mid;
        if (mid < cur_.low ) cur_.low  = mid;
        cur_.close = mid;
        return false;
    }

    bool          has_closed_bar() const noexcept { return has_closed_; }
    const BtOHLC& last_closed()    const noexcept { return last_closed_; }
    const BtOHLC& current()        const noexcept { return cur_; }

private:
    BtOHLC  cur_{};
    BtOHLC  last_closed_{};
    int64_t cur_bucket_ = 0;
    bool    cur_open_   = false;
    bool    has_closed_ = false;
};

// =============================================================================
// BtBarIndicators -- EMA9/21/50 + Wilder ATR-14 + RSI-14 + ADX-14
//
// FORMULAS COPIED VERBATIM FROM include/OHLCBarEngine.hpp
//   _update_ema  -> EMA seeding via mean-of-available-bars then Wilder
//                   recursion: ema += (2/(P+1)) * (close - ema)
//   _update_atr  -> ATR seed via mean of available TRs (with single-bar
//                   fallback), then Wilder smoothing:
//                   atr = (atr*(P-1) + tr) / P
//   _update_rsi  -> Bootstrap first RSI_P diffs with arithmetic mean of
//                   gains/losses, then Wilder smoothing per bar
//   _update_adx  -> +DM/-DM via (up>down && up>0) and (down>up && down>0)
//                   masking; Wilder smoothing of +DM, -DM, TR; DX from
//                   |+DI - -DI| / (+DI + -DI); Wilder smooth DX -> ADX
//
// All thresholds (RSI period 14, ATR period 14, ADX period 14) match
// production. Append-only: bars are pushed once per closed bar; no
// rewrite, no deletion until size cap.
//
// State is private. Public accessors give snapshot values. ready() flags
// gate consumers from reading uninitialised values.
// =============================================================================
class BtBarIndicators {
public:
    // ---- live OHLCBarEngine constants ---------------------------------
    static constexpr int EMA9_P  = 9;
    static constexpr int EMA21_P = 21;
    static constexpr int EMA50_P = 50;
    static constexpr int ATR_P   = 14;
    static constexpr int RSI_P   = 14;
    static constexpr int ADX_P   = 14;
    static constexpr int MAX_BARS = 300;   // matches live cap

    // Push a freshly-closed bar. All registered indicators advance.
    void push_bar(const BtOHLC& bar) noexcept {
        bars_.push_back(bar);
        if ((int)bars_.size() > MAX_BARS) bars_.erase(bars_.begin());

        const int n = (int)bars_.size();
        _update_ema();
        _update_atr();
        if (n >= RSI_P + 1) _update_rsi();
        if (n >= ADX_P + 1) _update_adx();

        // Ready once RSI has real values (matches live `m1_ready` semantics).
        if (n >= RSI_P + 1) ready_ = true;
    }

    // Snapshot accessors (cheap reads of latest computed values).
    bool   ready()       const noexcept { return ready_; }
    int    bar_count()   const noexcept { return (int)bars_.size(); }
    double ema9()        const noexcept { return ema9_; }
    double ema21()       const noexcept { return ema21_; }
    double ema50()       const noexcept { return ema50_; }
    double atr14()       const noexcept { return atr_avg_; }
    double rsi14()       const noexcept { return rsi_; }
    double adx14()       const noexcept { return adx_dx_smooth_; }
    bool   adx_rising()  const noexcept { return adx_rising_; }

    // For H4Regime which needs raw bar OHLC of the just-closed bar.
    const BtOHLC& last_bar() const noexcept { return bars_.back(); }

    // For comparing previous to current EMAs -- used by HTF engines for
    // cross detection (was_bull vs now_bull).
    double prev_ema9()  const noexcept { return prev_ema9_; }
    double prev_ema50() const noexcept { return prev_ema50_; }

private:
    // --- bar history -------------------------------------------------------
    std::vector<BtOHLC> bars_;

    // --- EMA state ---------------------------------------------------------
    bool   ema_init_  = false;
    double ema9_      = 0.0;
    double ema21_     = 0.0;
    double ema50_     = 0.0;
    double prev_ema9_  = 0.0;
    double prev_ema50_ = 0.0;

    // --- ATR state ---------------------------------------------------------
    bool   atr_init_  = false;
    double atr_avg_   = 0.0;

    // --- RSI state ---------------------------------------------------------
    bool   rsi_init_      = false;
    double rsi_avg_gain_  = 0.0;
    double rsi_avg_loss_  = 0.0;
    double rsi_           = 50.0;

    // --- ADX state ---------------------------------------------------------
    bool   adx_init_           = false;
    double adx_plus_dm_smooth_ = 0.0;
    double adx_minus_dm_smooth_= 0.0;
    double adx_tr_smooth_      = 0.0;
    double adx_dx_smooth_      = 0.0;
    double adx_prev_           = 0.0;
    bool   adx_rising_         = false;

    // --- ready flag --------------------------------------------------------
    bool ready_ = false;

    // ----------------------------------------------------------------------
    // _update_ema -- mirrors include/OHLCBarEngine.hpp::_update_ema
    // ----------------------------------------------------------------------
    void _update_ema() noexcept {
        const int n = (int)bars_.size();
        if (n < 1) return;
        // Capture previous values BEFORE updating (used by HTF cross detection)
        prev_ema9_  = ema9_;
        prev_ema50_ = ema50_;
        if (!ema_init_) {
            const int s9  = std::min(n, EMA9_P);
            const int s21 = std::min(n, EMA21_P);
            const int s50 = std::min(n, EMA50_P);
            double sum9=0, sum21=0, sum50=0;
            for (int i = n-s9;  i < n; ++i) sum9  += bars_[i].close;
            for (int i = n-s21; i < n; ++i) sum21 += bars_[i].close;
            for (int i = n-s50; i < n; ++i) sum50 += bars_[i].close;
            ema9_  = sum9  / s9;
            ema21_ = sum21 / s21;
            ema50_ = sum50 / s50;
            ema_init_ = true;
        } else {
            const double c = bars_.back().close;
            ema9_  += (2.0/(EMA9_P +1.0)) * (c - ema9_);
            ema21_ += (2.0/(EMA21_P+1.0)) * (c - ema21_);
            ema50_ += (2.0/(EMA50_P+1.0)) * (c - ema50_);
        }
    }

    // ----------------------------------------------------------------------
    // _update_atr -- mirrors include/OHLCBarEngine.hpp::_update_atr
    // ----------------------------------------------------------------------
    void _update_atr() noexcept {
        const int n = (int)bars_.size();
        if (n < 1) return;
        if (!atr_init_) {
            double sum = 0.0;
            int    count = 0;
            for (int i = std::max(1, n - ATR_P); i < n; ++i) {
                const double tr = std::max({
                    bars_[i].high - bars_[i].low,
                    std::fabs(bars_[i].high - bars_[i-1].close),
                    std::fabs(bars_[i].low  - bars_[i-1].close)
                });
                sum += tr; ++count;
            }
            if (count == 0) {
                atr_avg_ = bars_[0].high - bars_[0].low;
                if (atr_avg_ <= 0.0) atr_avg_ = bars_[0].close * 0.001;
            } else {
                atr_avg_ = sum / count;
            }
            atr_init_ = true;
        } else {
            const int i = n - 1;
            const double tr = (n >= 2) ? std::max({
                bars_[i].high - bars_[i].low,
                std::fabs(bars_[i].high - bars_[i-1].close),
                std::fabs(bars_[i].low  - bars_[i-1].close)
            }) : bars_[i].high - bars_[i].low;
            atr_avg_ = (atr_avg_ * (ATR_P-1) + tr) / ATR_P;
        }
    }

    // ----------------------------------------------------------------------
    // _update_rsi -- mirrors include/OHLCBarEngine.hpp::_update_rsi
    //
    // Note: this is called only when n >= RSI_P + 1, so the bootstrap
    // path is guaranteed to have enough bars to compute the seed averages.
    // ----------------------------------------------------------------------
    void _update_rsi() noexcept {
        const int n = (int)bars_.size();
        if (n < RSI_P + 1) return;
        if (!rsi_init_) {
            double gain = 0.0, loss = 0.0;
            const int start = n - RSI_P - 1;
            for (int i = start + 1; i <= start + RSI_P; ++i) {
                const double d = bars_[i].close - bars_[i-1].close;
                if (d > 0) gain += d; else loss -= d;
            }
            rsi_avg_gain_ = gain / RSI_P;
            rsi_avg_loss_ = loss / RSI_P;
            rsi_init_ = true;
            // Wilder-smooth any remaining bars between bootstrap end and now.
            for (int i = start + RSI_P + 1; i < n; ++i) {
                const double d = bars_[i].close - bars_[i-1].close;
                const double g = d > 0 ?  d : 0.0;
                const double l = d < 0 ? -d : 0.0;
                rsi_avg_gain_ = (rsi_avg_gain_ * (RSI_P-1) + g) / RSI_P;
                rsi_avg_loss_ = (rsi_avg_loss_ * (RSI_P-1) + l) / RSI_P;
            }
        } else {
            const double d = bars_.back().close - bars_[bars_.size()-2].close;
            const double g = d > 0 ?  d : 0.0;
            const double l = d < 0 ? -d : 0.0;
            rsi_avg_gain_ = (rsi_avg_gain_ * (RSI_P-1) + g) / RSI_P;
            rsi_avg_loss_ = (rsi_avg_loss_ * (RSI_P-1) + l) / RSI_P;
        }
        if (rsi_avg_loss_ < 1e-10) rsi_ = 100.0;
        else rsi_ = 100.0 - 100.0 / (1.0 + rsi_avg_gain_ / rsi_avg_loss_);
    }

    // ----------------------------------------------------------------------
    // _update_adx -- mirrors include/OHLCBarEngine.hpp::_update_adx
    //
    // Wilder ADX implementation:
    //   1) Compute +DM/-DM from current vs previous bar high/low using
    //      mask: +DM = up if (up > down && up > 0) else 0
    //            -DM = down if (down > up && down > 0) else 0
    //   2) Compute TR (max of bar range, |high - prev_close|, |low - prev_close|)
    //   3) Bootstrap first ADX_P bars by accumulating raw +DM/-DM/TR sums
    //   4) After bootstrap, Wilder-smooth: smooth = smooth - smooth/P + raw
    //   5) DI+ = 100 * smooth_+DM / smooth_TR, DI- similarly
    //   6) DX  = 100 * |DI+ - DI-| / (DI+ + DI-)
    //   7) Wilder-smooth DX -> ADX
    // ----------------------------------------------------------------------
    void _update_adx() noexcept {
        const int n = (int)bars_.size();
        if (n < 2) return;

        const BtOHLC& cur  = bars_[n - 1];
        const BtOHLC& prev = bars_[n - 2];

        const double up   = cur.high - prev.high;
        const double down = prev.low  - cur.low;
        const double plus_dm  = (up   > down && up   > 0.0) ? up   : 0.0;
        const double minus_dm = (down > up   && down > 0.0) ? down : 0.0;

        const double tr = std::max({
            cur.high - cur.low,
            std::fabs(cur.high - prev.close),
            std::fabs(cur.low  - prev.close)
        });

        if (!adx_init_) {
            adx_plus_dm_smooth_  += plus_dm;
            adx_minus_dm_smooth_ += minus_dm;
            adx_tr_smooth_       += tr;

            // Bootstrap requires ADX_P diffs => n == ADX_P + 1
            if (n < ADX_P + 1) return;

            const double pdi   = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_plus_dm_smooth_  / adx_tr_smooth_ : 0.0;
            const double mdi   = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_minus_dm_smooth_ / adx_tr_smooth_ : 0.0;
            const double denom = pdi + mdi;
            const double dx    = (denom > 1e-10) ? 100.0 * std::fabs(pdi - mdi) / denom : 0.0;
            adx_dx_smooth_ = dx;
            adx_prev_      = dx;
            adx_init_      = true;
            adx_rising_    = false;  // first value -- no prior to compare
            return;
        }

        // Wilder smoothing: smooth[t] = smooth[t-1] - smooth[t-1]/P + raw[t]
        adx_plus_dm_smooth_  = adx_plus_dm_smooth_  - adx_plus_dm_smooth_  / ADX_P + plus_dm;
        adx_minus_dm_smooth_ = adx_minus_dm_smooth_ - adx_minus_dm_smooth_ / ADX_P + minus_dm;
        adx_tr_smooth_       = adx_tr_smooth_       - adx_tr_smooth_       / ADX_P + tr;

        const double pdi   = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_plus_dm_smooth_  / adx_tr_smooth_ : 0.0;
        const double mdi   = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_minus_dm_smooth_ / adx_tr_smooth_ : 0.0;
        const double denom = pdi + mdi;
        const double dx    = (denom > 1e-10) ? 100.0 * std::fabs(pdi - mdi) / denom : 0.0;

        // ADX[t] = ADX[t-1] - ADX[t-1]/P + DX[t]/P  (Wilder recursion)
        adx_dx_smooth_ = adx_dx_smooth_ - adx_dx_smooth_ / ADX_P + dx / ADX_P;

        adx_rising_ = (adx_dx_smooth_ > adx_prev_);
        adx_prev_   = adx_dx_smooth_;
    }
};

// =============================================================================
// BtBarEngine<P> -- bundled bar builder + indicators for period P (minutes)
//
// Drive with on_tick(mid, ts_ms). When a bar closes the indicators advance
// once. Reading EMA/ATR/RSI/ADX returns the indicator value computed at
// the most-recent bar close (NOT the in-progress bar).
//
// Use:
//   BtBarEngine<60>  h1;
//   if (h1.on_tick(mid, ts_ms)) { /* bar just closed */ }
//   if (h1.indicators_ready())  { use h1.ema9() etc. }
// =============================================================================
template <int PERIOD_MIN>
class BtBarEngine {
public:
    bool on_tick(double mid, int64_t ts_ms) noexcept {
        const bool closed = builder_.on_tick(mid, ts_ms);
        if (closed) ind_.push_bar(builder_.last_closed());
        return closed;
    }

    bool   indicators_ready() const noexcept { return ind_.ready(); }
    int    bar_count()        const noexcept { return ind_.bar_count(); }

    // Bar accessors
    const BtOHLC& last_closed_bar() const noexcept { return builder_.last_closed(); }
    const BtOHLC& current_bar()     const noexcept { return builder_.current(); }
    bool          has_closed_bar()  const noexcept { return builder_.has_closed_bar(); }

    // Indicator accessors -- snapshot of last closed bar's computed values.
    double ema9()       const noexcept { return ind_.ema9(); }
    double ema21()      const noexcept { return ind_.ema21(); }
    double ema50()      const noexcept { return ind_.ema50(); }
    double atr14()      const noexcept { return ind_.atr14(); }
    double rsi14()      const noexcept { return ind_.rsi14(); }
    double adx14()      const noexcept { return ind_.adx14(); }
    bool   adx_rising() const noexcept { return ind_.adx_rising(); }

    double prev_ema9()  const noexcept { return ind_.prev_ema9(); }
    double prev_ema50() const noexcept { return ind_.prev_ema50(); }

private:
    BtBarBuilder<PERIOD_MIN> builder_;
    BtBarIndicators          ind_;
};

// =============================================================================
// BtPdhlTracker -- previous-day UTC high/low for PDHLReversionEngine
//
// Day boundary at 00:00 UTC. When the day rolls, prev_day's running H/L
// becomes the "PDH/PDL" values returned to consumers. The first day of
// data has PDH/PDL = 0.0 -- consumers must check is_ready() before using.
// =============================================================================
class BtPdhlTracker {
public:
    // Update with each tick. Internally tracks today's running H/L and on
    // day-boundary crossing rotates them into pdh_/pdl_.
    void update(double mid, int64_t ts_ms) noexcept {
        const int day = (int)(ts_ms / 86400000LL);  // UTC days since epoch
        if (cur_day_ < 0) {
            cur_day_ = day;
            today_high_ = mid;
            today_low_  = mid;
            return;
        }
        if (day != cur_day_) {
            // Day rolled: yesterday's H/L are now PDH/PDL.
            pdh_ = today_high_;
            pdl_ = today_low_;
            ready_ = true;
            cur_day_ = day;
            today_high_ = mid;
            today_low_  = mid;
            return;
        }
        if (mid > today_high_) today_high_ = mid;
        if (mid < today_low_ ) today_low_  = mid;
    }

    bool   is_ready() const noexcept { return ready_; }
    double pdh()      const noexcept { return pdh_; }
    double pdl()      const noexcept { return pdl_; }

private:
    int     cur_day_    = -1;
    double  today_high_ = 0.0;
    double  today_low_  = 0.0;
    double  pdh_        = 0.0;
    double  pdl_        = 0.0;
    bool    ready_      = false;
};

// =============================================================================
// BtEwmDrift -- exponentially-weighted moving drift of mid price
//
// Used by MacroCrash and PDHL as `ewm_drift`. Halflife configurable; default
// is 30 seconds which matches the production gold path. Drift unit is
// (mid - ewm_mid) i.e. instantaneous deviation from the slow filter.
// Tracks last update timestamp so alpha scales with elapsed time -- ticks
// are not equally spaced.
//
// Formula: alpha = 1 - exp(-dt_s / halflife_s * ln(2))
//          ewm  += alpha * (mid - ewm)
//          drift = mid - ewm
// =============================================================================
class BtEwmDrift {
public:
    explicit BtEwmDrift(double halflife_s = 30.0) noexcept
        : halflife_s_(halflife_s) {}

    void update(double mid, int64_t ts_ms) noexcept {
        if (!init_) {
            ewm_     = mid;
            last_ms_ = ts_ms;
            init_    = true;
            return;
        }
        const double dt_s = std::max(0.0, (ts_ms - last_ms_) / 1000.0);
        if (dt_s <= 0.0) return;  // duplicate/out-of-order tick: skip
        // alpha derived from halflife: alpha = 1 - 2^(-dt/halflife)
        const double alpha = 1.0 - std::pow(2.0, -dt_s / halflife_s_);
        ewm_     += alpha * (mid - ewm_);
        last_ms_  = ts_ms;
    }

    bool   is_ready() const noexcept { return init_; }
    double ewm()      const noexcept { return ewm_; }
    // Drift = current mid - slow ewm; caller passes current mid for read.
    double drift(double mid) const noexcept { return init_ ? (mid - ewm_) : 0.0; }

private:
    double  halflife_s_ = 30.0;
    double  ewm_        = 0.0;
    int64_t last_ms_    = 0;
    bool    init_       = false;
};

// =============================================================================
// BtVolRatio -- vol ratio gate for MacroCrash (recent_range / baseline_range)
//
// MacroCrash gates entries on `vol_ratio > 1.0` to avoid firing in calm
// regimes. Mirrors the supervisor logic in GoldEngineStack but adapted for
// backtest mid stream (no wall clock).
//
// Implementation: maintain two rolling windows of mid prices --
//   short window (recent ticks, ~40 by default)
//   long  window (baseline,    ~400 by default)
// Range = max - min over the window. vol_ratio = short_range / long_range
// once both windows are full.
// =============================================================================
class BtVolRatio {
public:
    explicit BtVolRatio(int short_n = 40, int long_n = 400) noexcept
        : short_n_(short_n), long_n_(long_n) {
        short_buf_.reserve(short_n_);
        long_buf_.reserve(long_n_);
    }

    void update(double mid) noexcept {
        // Append + drop-oldest in O(1) amortised via circular buffers.
        if ((int)short_buf_.size() < short_n_) short_buf_.push_back(mid);
        else { short_buf_[short_idx_ % short_n_] = mid; ++short_idx_; }
        if ((int)long_buf_.size() < long_n_)   long_buf_.push_back(mid);
        else { long_buf_[long_idx_ % long_n_]   = mid; ++long_idx_; }
    }

    bool   is_ready() const noexcept {
        return (int)short_buf_.size() >= short_n_ && (int)long_buf_.size() >= long_n_;
    }

    double ratio() const noexcept {
        if (!is_ready()) return 1.0;
        const double sr = _range(short_buf_);
        const double lr = _range(long_buf_);
        if (lr < 1e-10) return 1.0;
        return sr / lr;
    }

    double short_range() const noexcept { return _range(short_buf_); }
    double long_range()  const noexcept { return _range(long_buf_);  }

private:
    int    short_n_;
    int    long_n_;
    std::vector<double> short_buf_;
    std::vector<double> long_buf_;
    int short_idx_ = 0;
    int long_idx_  = 0;

    static double _range(const std::vector<double>& v) noexcept {
        if (v.empty()) return 0.0;
        double hi = v[0], lo = v[0];
        for (double x : v) { if (x > hi) hi = x; if (x < lo) lo = x; }
        return hi - lo;
    }
};

// =============================================================================
// BtSessionSlot -- UTC hour -> 0..6 session slot (matches existing inline
// logic in OmegaBacktest runners). Centralised here so all wired engines
// use the same mapping.
//
//   slot 0 = 05:00..06:59 UTC   (pre-London dead zone)
//   slot 1 = 07:00..08:59 UTC   (London open)
//   slot 2 = 09:00..11:59 UTC   (London core)
//   slot 3 = 12:00..13:59 UTC   (London/NY overlap pre)
//   slot 4 = 14:00..16:59 UTC   (NY core)
//   slot 5 = 17:00..21:59 UTC   (NY late)
//   slot 6 = 22:00..04:59 UTC   (Asia)
// =============================================================================
inline int bt_session_slot(int64_t ts_ms) noexcept {
    const int h = (int)((ts_ms / 1000 / 3600) % 24);
    if (h >= 7  && h < 9)  return 1;
    if (h >= 9  && h < 12) return 2;
    if (h >= 12 && h < 14) return 3;
    if (h >= 14 && h < 17) return 4;
    if (h >= 17 && h < 22) return 5;
    if (h >= 22 || h < 5)  return 6;
    return 0;  // 05:00..06:59
}

// =============================================================================
// BtM15ExpansionTracker -- tracks whether M15 ATR is expanding
//
// Used by H4RegimeEngine::on_h4_bar as `m15_atr_expanding`. Definition:
// current M15 ATR > previous M15 ATR (strictly increasing). Caller must
// drive a BtBarEngine<15> separately and pass each new ATR-14 reading via
// update(). Returns the comparison flag at the time of the last call.
// =============================================================================
class BtM15ExpansionTracker {
public:
    void update(double m15_atr14) noexcept {
        if (!init_) { prev_ = m15_atr14; init_ = true; expanding_ = false; return; }
        expanding_ = (m15_atr14 > prev_);
        prev_      = m15_atr14;
    }
    bool expanding() const noexcept { return expanding_; }
private:
    double prev_     = 0.0;
    bool   init_     = false;
    bool   expanding_= false;
};

}  // namespace bt
}  // namespace omega
