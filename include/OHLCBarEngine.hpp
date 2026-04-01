#pragma once
// =============================================================================
// OHLCBarEngine.hpp -- OHLC bar data from cTrader ProtoOA trendbar API
//
// Uses the SAME cTrader SSL connection as CTraderDepthClient.
// On startup (after auth): requests 200 M1 + 100 M5 historical bars.
// Subscribes to live bar close events (pt=2220).
//
// Computes per-bar for XAUUSD (and any subscribed symbol):
//   ? EMA9 / EMA21 / EMA50  (on M1 close)
//   ? RSI(14)               (on M1 close)
//   ? ATR(14)               (on M1 H-L range) -- replaces tick ATR
//   ? Bollinger(20,2)       (on M1 close)
//   ? Swing high/low        (3-bar pivot on M5)
//   ? Trend state           (HH/HL = UP, LH/LL = DOWN, else FLAT)
//
// NEW ENHANCEMENTS (2026-04-01):
//   ? BBW squeeze detector  -- Bollinger Band Width at N-bar low ? breakout imminent
//   ? ATR slope (3-bar)     -- expanding vs contracting volatility regime detection
//   ? RSI divergence        -- price/RSI divergence at confirmed swing pivots
//   ? VWAP slope            -- direction of EMA50 proxy (rising/flat/falling)
//   ? Rolling spread avg    -- baseline spread for wide-spread entry blocking
//   ? Tick speed/accel      -- ticks/sec + acceleration, real-time vol proxy
//   ? Volume delta          -- L2 imbalance converted to rolling buy/sell pressure
//
// All outputs exposed as atomics -- zero-lock hot path from FIX thread.
//
// ProtoOA trendbar message types (same framing as depth client):
//   2137 = GetTrendbarsReq    ? request historical bars
//   2138 = GetTrendbarsRes    ? response (repeated bars)
//   2220 = SubscribeLiveTrendbarReq ? subscribe live bar push
//   2221 = UnsubscribeLiveTrendbarReq
//   2155 = SpotEvent / live trendbar push (same pt as depth, check symbolId)
//   Actually: live trendbar push uses pt=2215 (ProtoOASpotEvent with bar)
//   Correct: ProtoOALiveTrendBar uses pt=2217
//
// ProtoOATrendbar fields (from OpenApiModelMessages.proto):
//   field 5 = volume (uint64)
//   field 6 = open   (int64, relative to close: close + open_delta)
//   field 7 = high   (int64, delta above close: close + high_delta)
//   field 8 = low    (int64, delta below close: close - low_delta; note: negative)
//   field 2 = utcTimestampInMinutes (uint64) -- bar open time in minutes since epoch
//   field 3 = close  (uint64, absolute price * 100000)
//
// All prices in cTrader are integers scaled by 100000.
// XAUUSD: 464254 = 4642.54
// =============================================================================

#include <array>
#include <deque>
#include <cmath>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <algorithm>
#include <chrono>

// =============================================================================
// OHLCBar -- one closed candle
// =============================================================================
struct OHLCBar {
    int64_t  ts_min = 0;   // open time in minutes since Unix epoch (UTC)
    double   open   = 0.0;
    double   high   = 0.0;
    double   low    = 0.0;
    double   close  = 0.0;
    uint64_t volume = 0;
};

// =============================================================================
// BarIndicators -- atomic outputs read by trading engines
// =============================================================================
struct BarIndicators {
    // ?? Original indicators ???????????????????????????????????????????????????
    std::atomic<double> ema9  {0.0};
    std::atomic<double> ema21 {0.0};
    std::atomic<double> ema50 {0.0};
    std::atomic<double> rsi14 {50.0}; // 0-100; 50 = neutral/uninitialised
    std::atomic<double> atr14 {0.0};  // true range ATR from bars (replaces tick ATR)
    std::atomic<double> bb_upper{0.0};// Bollinger upper band
    std::atomic<double> bb_mid  {0.0};// Bollinger mid (20-SMA)
    std::atomic<double> bb_lower{0.0};// Bollinger lower band
    std::atomic<double> bb_pct  {0.5};// 0=at lower, 0.5=at mid, 1=at upper
    std::atomic<double> vol_ratio{1.0};// last bar volume / 20-bar avg volume
    // M5 trend structure
    std::atomic<int>    trend_state{0}; // +1=UP(HH/HL), -1=DOWN(LH/LL), 0=FLAT
    std::atomic<double> swing_high{0.0};// last confirmed swing high
    std::atomic<double> swing_low {0.0};// last confirmed swing low
    // Bar availability flags
    std::atomic<bool>   m1_ready{false}; // true once >=50 M1 bars loaded
    std::atomic<bool>   m5_ready{false}; // true once >=20 M5 bars loaded

    // ?? NEW: BBW squeeze detector ?????????????????????????????????????????????
    // bb_width = (upper - lower) / mid  -- normalised band width
    // bb_squeeze = true when BBW is at its lowest in BB_SQUEEZE_LOOKBACK bars
    // A squeeze = volatility compression ? breakout imminent
    // Used by GoldStack to arm a "breakout mode" when bands are tight
    std::atomic<double> bb_width       {0.0};  // current normalised BBW
    std::atomic<double> bb_width_min   {0.0};  // N-bar min BBW (squeeze level)
    std::atomic<bool>   bb_squeeze     {false}; // true when BBW at N-bar low
    std::atomic<int>    bb_squeeze_bars{0};     // consecutive bars BBW has been contracting

    // ?? NEW: ATR slope (3-bar) ????????????????????????????????????????????????
    // atr_slope > 0: ATR expanding (breakout/trending regime)
    // atr_slope < 0: ATR contracting (mean-reversion / compression regime)
    // atr_expanding  = true when ATR increasing for 3 consecutive bars
    // atr_contracting = true when ATR decreasing for 3 consecutive bars
    std::atomic<double> atr_slope      {0.0};  // ATR change over last 3 bars (pts)
    std::atomic<bool>   atr_expanding  {false}; // true = consecutive ATR expansion
    std::atomic<bool>   atr_contracting{false}; // true = consecutive ATR contraction

    // ?? NEW: RSI divergence ???????????????????????????????????????????????????
    // rsi_bull_div = bullish divergence: price lower low, RSI higher low
    //   ? potential bounce / reversal up
    // rsi_bear_div = bearish divergence: price higher high, RSI lower high
    //   ? potential top / reversal down
    // rsi_div_strength = magnitude of RSI divergence in RSI points
    std::atomic<bool>   rsi_bull_div   {false}; // bullish price/RSI divergence active
    std::atomic<bool>   rsi_bear_div   {false}; // bearish price/RSI divergence active
    std::atomic<double> rsi_div_strength{0.0};  // magnitude of divergence (RSI pts)

    // ?? NEW: VWAP slope ???????????????????????????????????????????????????????
    // vwap_slope: EMA50 change over 3 bars -- proxy for rolling VWAP direction
    // vwap_direction: +1=rising, -1=falling, 0=flat
    // Rising VWAP + LONG signal = with-trend = higher confidence
    // Flat VWAP = mean-reversion setups active
    std::atomic<double> vwap_slope     {0.0};  // slope of EMA50 proxy (pts/3 bars)
    std::atomic<int>    vwap_direction {0};     // +1=rising, -1=falling, 0=flat

    // ?? NEW: Rolling spread avg + anomaly gate ????????????????????????????????
    // spread_avg: rolling 200-tick average spread -- session baseline
    // spread_ratio: current_spread / spread_avg
    //   > SPREAD_WIDE_RATIO (1.5): spread anomaly -- news/thin liquidity, block entries
    //   < 0.8: spread tightening (pre-move compression) -- normal entry zone
    // Updated per tick via update_tick_metrics()
    std::atomic<double> spread_avg     {0.0};  // rolling avg spread (pts)
    std::atomic<double> spread_ratio   {1.0};  // current / avg spread ratio

    // ?? NEW: Tick speed + acceleration ???????????????????????????????????????
    // tick_rate: ticks per second over last 2 seconds (rolling)
    // tick_accel: change in tick rate vs 5s ago -- positive = accelerating
    // tick_storm: true when tick_rate > TICK_STORM_THRESH (8/s) = momentum move
    // High tick rate = use tighter momentum confirmation; suppress mean-reversion
    // Low tick rate (<1/s) = dead tape / Asia -- normal filters still apply
    std::atomic<double> tick_rate      {0.0};  // ticks/sec (2s rolling window)
    std::atomic<double> tick_accel     {0.0};  // rate change vs 5s ago
    std::atomic<bool>   tick_storm     {false}; // true when tick_rate >= 8/s

    // ?? NEW: Volume delta (buy vs sell pressure from L2) ?????????????????????
    // vol_delta: current tick's L2 pressure converted to -1..+1
    //   +1 = fully bid-heavy (aggressive buying)
    //   -1 = fully ask-heavy (aggressive selling)
    // vol_delta_ratio: 100-tick rolling average of vol_delta
    //   Sustained positive = buy pressure building ? supports LONG
    //   Sustained negative = sell pressure building ? supports SHORT
    std::atomic<double> vol_delta      {0.0};  // current tick signed delta
    std::atomic<double> vol_delta_ratio{0.0};  // 100-tick rolling average

    BarIndicators() = default;
    BarIndicators(const BarIndicators&) = delete;
    BarIndicators& operator=(const BarIndicators&) = delete;
};

// =============================================================================
// OHLCBarEngine -- core computation (non-threaded, called from cTrader thread)
// =============================================================================
class OHLCBarEngine {
public:
    static constexpr int  EMA9_P    = 9;
    static constexpr int  EMA21_P   = 21;
    static constexpr int  EMA50_P   = 50;
    static constexpr int  RSI_P     = 14;
    static constexpr int  ATR_P     = 14;
    static constexpr int  BB_P      = 20;
    static constexpr int  SWING_P   = 3;    // bars each side for pivot
    static constexpr int  VOL_MA_P  = 20;
    static constexpr int  MIN_BARS  = 14;   // 14 bars minimum: enough for ATR14 + EMA warmup

    // ?? New constants ?????????????????????????????????????????????????????????
    // BBW squeeze: look back 20 bars to find band-width minimum
    static constexpr int    BB_SQUEEZE_LOOKBACK  = 20;
    // ATR slope: measure over 3 bars
    static constexpr int    ATR_SLOPE_BARS       = 3;
    // VWAP slope flat threshold: < 0.05pts/3bars = considered flat
    static constexpr double VWAP_SLOPE_FLAT_THR  = 0.05;
    // Spread average: 200-tick rolling window
    static constexpr int    SPREAD_AVG_TICKS     = 200;
    // Spread ratio threshold above which entry is blocked
    static constexpr double SPREAD_WIDE_RATIO    = 1.5;
    // Tick rate window: 2s for current rate, 5s for acceleration baseline
    static constexpr int    TICK_RATE_WINDOW_MS  = 2000;
    static constexpr int    TICK_ACCEL_WINDOW_MS = 5000;
    // Tick rate above which a "tick storm" (momentum move) is flagged
    static constexpr double TICK_STORM_THRESH    = 8.0;
    // Volume delta normalisation window
    static constexpr int    VOL_DELTA_WINDOW     = 100;
    // RSI divergence: compare last N swing pivots
    static constexpr int    RSI_DIV_LOOKBACK     = 5;

    // Indicators exposed to other threads
    BarIndicators ind;

    // ?????????????????????????????????????????????????????????????????????????
    // add_bar() -- call on every newly-closed bar
    // ?????????????????????????????????????????????????????????????????????????
    void add_bar(const OHLCBar& bar) {
        bars_.push_back(bar);
        if (bars_.size() > 300) bars_.pop_front();

        const int n = static_cast<int>(bars_.size());
        if (n < MIN_BARS) return;

        _update_ema();
        _update_rsi();
        _update_atr();
        _update_bollinger();
        _update_volume_ratio();
        _update_swing_and_trend();
        _update_bbw_squeeze();
        _update_atr_slope();
        _update_rsi_divergence();
        _update_vwap_slope();

        if (!ind.m1_ready.load() && n >= MIN_BARS) ind.m1_ready.store(true);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // seed() -- bulk load historical bars on startup
    // ?????????????????????????????????????????????????????????????????????????
    void seed(const std::vector<OHLCBar>& historical) {
        bars_.clear();
        for (const auto& b : historical) bars_.push_back(b);
        if (bars_.size() > 300) {
            while (bars_.size() > 300) bars_.pop_front();
        }
        const int n = static_cast<int>(bars_.size());
        if (n < MIN_BARS) return;
        _update_ema();
        _update_rsi();
        _update_atr();
        _update_bollinger();
        _update_volume_ratio();
        _update_swing_and_trend();
        _update_bbw_squeeze();
        _update_atr_slope();
        _update_rsi_divergence();
        _update_vwap_slope();
        _seed_atr_history();
        if (n >= MIN_BARS) ind.m1_ready.store(true);
    }

    int bar_count() const { return static_cast<int>(bars_.size()); }

    // ?????????????????????????????????????????????????????????????????????????
    // update_tick_metrics() -- call every tick from on_tick() in main.cpp
    //   spread : current ask - bid
    //   now_ms : current epoch milliseconds
    // Updates: spread_avg, spread_ratio, tick_rate, tick_accel, tick_storm
    // ?????????????????????????????????????????????????????????????????????????
    void update_tick_metrics(double spread, int64_t now_ms) noexcept {
        // ?? Rolling spread average ?????????????????????????????????????????????
        spread_window_.push_back(spread);
        if (static_cast<int>(spread_window_.size()) > SPREAD_AVG_TICKS)
            spread_window_.pop_front();
        if (!spread_window_.empty()) {
            double sum = 0.0;
            for (double s : spread_window_) sum += s;
            const double avg = sum / static_cast<double>(spread_window_.size());
            ind.spread_avg.store(avg, std::memory_order_relaxed);
            const double ratio = (avg > 1e-6) ? (spread / avg) : 1.0;
            ind.spread_ratio.store(ratio, std::memory_order_relaxed);
        }

        // ?? Tick speed + acceleration ??????????????????????????????????????????
        tick_timestamps_.push_back(now_ms);
        // Trim timestamps older than the longest window needed
        const int64_t oldest_needed = now_ms - static_cast<int64_t>(
            std::max(TICK_RATE_WINDOW_MS, TICK_ACCEL_WINDOW_MS));
        while (!tick_timestamps_.empty() && tick_timestamps_.front() < oldest_needed)
            tick_timestamps_.pop_front();

        // Current tick rate: count ticks in last TICK_RATE_WINDOW_MS
        int rate_count = 0;
        for (int64_t ts : tick_timestamps_) {
            if (ts >= now_ms - static_cast<int64_t>(TICK_RATE_WINDOW_MS))
                ++rate_count;
        }
        const double rate_now = static_cast<double>(rate_count)
                              / (TICK_RATE_WINDOW_MS / 1000.0);
        ind.tick_rate .store(rate_now, std::memory_order_relaxed);
        ind.tick_storm.store(rate_now >= TICK_STORM_THRESH, std::memory_order_relaxed);

        // Acceleration: compare current half-window vs older half-window
        // Old rate = ticks in (now - ACCEL_WINDOW, now - ACCEL_WINDOW/2)
        const int64_t accel_half = static_cast<int64_t>(TICK_ACCEL_WINDOW_MS / 2);
        int accel_old_count = 0;
        for (int64_t ts : tick_timestamps_) {
            if (ts >= now_ms - static_cast<int64_t>(TICK_ACCEL_WINDOW_MS) &&
                ts <  now_ms - accel_half)
                ++accel_old_count;
        }
        const double rate_old = static_cast<double>(accel_old_count)
                              / (TICK_ACCEL_WINDOW_MS / 2000.0);
        ind.tick_accel.store(rate_now - rate_old, std::memory_order_relaxed);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // update_volume_delta() -- call every tick from on_tick() when L2 is live
    //   l2_imb: 0..1 from L2Book::imbalance()
    //     > 0.5 = bid-heavy (buying pressure)
    //     < 0.5 = ask-heavy (selling pressure)
    //     = 0.5 = neutral / L2 unavailable
    // Updates: vol_delta (current tick), vol_delta_ratio (100-tick rolling avg)
    // ?????????????????????????????????????????????????????????????????????????
    void update_volume_delta(double l2_imb) noexcept {
        // Convert 0..1 imbalance to -1..+1 signed delta
        const double delta = (l2_imb - 0.5) * 2.0;
        vol_delta_window_.push_back(delta);
        if (static_cast<int>(vol_delta_window_.size()) > VOL_DELTA_WINDOW)
            vol_delta_window_.pop_front();

        ind.vol_delta.store(delta, std::memory_order_relaxed);

        if (!vol_delta_window_.empty()) {
            double sum = 0.0;
            for (double d : vol_delta_window_) sum += d;
            const double ratio = sum / static_cast<double>(vol_delta_window_.size());
            ind.vol_delta_ratio.store(ratio, std::memory_order_relaxed);
        }
    }

private:
    std::deque<OHLCBar> bars_;

    // ?? Original private state ????????????????????????????????????????????????
    double ema9_  = 0.0, ema21_ = 0.0, ema50_ = 0.0;
    bool   ema_init_ = false;
    double rsi_avg_gain_ = 0.0, rsi_avg_loss_ = 0.0;
    bool   rsi_init_ = false;
    double atr_avg_ = 0.0;
    bool   atr_init_ = false;

    // ?? New private state ?????????????????????????????????????????????????????
    // ATR history ring buffer for slope calculation
    std::deque<double> atr_history_;

    // RSI pivot history for divergence detection
    // Stores {price, rsi_value} at each confirmed swing high/low
    struct PivotRSI {
        double price = 0.0;
        double rsi   = 0.0;
    };
    std::deque<PivotRSI> pivot_highs_rsi_;
    std::deque<PivotRSI> pivot_lows_rsi_;

    // Per-tick metric state (called from update_tick_metrics/update_volume_delta)
    std::deque<double>  spread_window_;
    std::deque<int64_t> tick_timestamps_;
    std::deque<double>  vol_delta_window_;

    // ?????????????????????????????????????????????????????????????????????????
    // _update_ema()
    // ?????????????????????????????????????????????????????????????????????????
    void _update_ema() {
        const int n = static_cast<int>(bars_.size());
        if (!ema_init_ && n >= EMA50_P) {
            double s9=0, s21=0, s50=0;
            for (int i = 0; i < EMA50_P; ++i) s50 += bars_[i].close;
            for (int i = n-EMA9_P;  i < n; ++i) s9  += bars_[i].close;
            for (int i = n-EMA21_P; i < n; ++i) s21 += bars_[i].close;
            ema9_  = s9  / EMA9_P;
            ema21_ = s21 / EMA21_P;
            ema50_ = s50 / EMA50_P;
            const double a50 = 2.0 / (EMA50_P + 1.0);
            const double a21 = 2.0 / (EMA21_P + 1.0);
            const double a9  = 2.0 / (EMA9_P  + 1.0);
            for (int i = EMA50_P; i < n; ++i) {
                ema50_ += a50 * (bars_[i].close - ema50_);
                if (i >= n - EMA21_P) ema21_ += a21 * (bars_[i].close - ema21_);
                if (i >= n - EMA9_P)  ema9_  += a9  * (bars_[i].close - ema9_);
            }
            ema_init_ = true;
        } else if (ema_init_) {
            const double c = bars_.back().close;
            ema9_  += (2.0/(EMA9_P +1.0)) * (c - ema9_);
            ema21_ += (2.0/(EMA21_P+1.0)) * (c - ema21_);
            ema50_ += (2.0/(EMA50_P+1.0)) * (c - ema50_);
        }
        ind.ema9 .store(ema9_,  std::memory_order_relaxed);
        ind.ema21.store(ema21_, std::memory_order_relaxed);
        ind.ema50.store(ema50_, std::memory_order_relaxed);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // _update_rsi()
    // ?????????????????????????????????????????????????????????????????????????
    void _update_rsi() {
        const int n = static_cast<int>(bars_.size());
        if (n < RSI_P + 1) return;
        if (!rsi_init_) {
            double gain = 0, loss = 0;
            const int start = n - RSI_P - 1;
            for (int i = start + 1; i <= start + RSI_P; ++i) {
                const double d = bars_[i].close - bars_[i-1].close;
                if (d > 0) gain += d; else loss -= d;
            }
            rsi_avg_gain_ = gain / RSI_P;
            rsi_avg_loss_ = loss / RSI_P;
            rsi_init_ = true;
            for (int i = start + RSI_P + 1; i < n; ++i) {
                const double d = bars_[i].close - bars_[i-1].close;
                const double g = d > 0 ? d : 0.0;
                const double l = d < 0 ? -d : 0.0;
                rsi_avg_gain_ = (rsi_avg_gain_ * (RSI_P-1) + g) / RSI_P;
                rsi_avg_loss_ = (rsi_avg_loss_ * (RSI_P-1) + l) / RSI_P;
            }
        } else {
            const double d = bars_.back().close - bars_[bars_.size()-2].close;
            const double g = d > 0 ? d : 0.0;
            const double l = d < 0 ? -d : 0.0;
            rsi_avg_gain_ = (rsi_avg_gain_ * (RSI_P-1) + g) / RSI_P;
            rsi_avg_loss_ = (rsi_avg_loss_ * (RSI_P-1) + l) / RSI_P;
        }
        double rsi = 50.0;
        if (rsi_avg_loss_ < 1e-10) rsi = 100.0;
        else rsi = 100.0 - 100.0 / (1.0 + rsi_avg_gain_ / rsi_avg_loss_);
        ind.rsi14.store(rsi, std::memory_order_relaxed);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // _update_atr()
    // ?????????????????????????????????????????????????????????????????????????
    void _update_atr() {
        const int n = static_cast<int>(bars_.size());
        if (n < ATR_P + 1) return;
        if (!atr_init_) {
            double sum = 0;
            const int start = n - ATR_P - 1;
            for (int i = start + 1; i <= start + ATR_P; ++i) {
                const double tr = std::max({
                    bars_[i].high - bars_[i].low,
                    std::fabs(bars_[i].high - bars_[i-1].close),
                    std::fabs(bars_[i].low  - bars_[i-1].close)
                });
                sum += tr;
            }
            atr_avg_ = sum / ATR_P;
            atr_init_ = true;
            for (int i = start + ATR_P + 1; i < n; ++i) {
                const double tr = std::max({
                    bars_[i].high - bars_[i].low,
                    std::fabs(bars_[i].high - bars_[i-1].close),
                    std::fabs(bars_[i].low  - bars_[i-1].close)
                });
                atr_avg_ = (atr_avg_ * (ATR_P-1) + tr) / ATR_P;
            }
        } else {
            const int i = n - 1;
            const double tr = std::max({
                bars_[i].high - bars_[i].low,
                std::fabs(bars_[i].high - bars_[i-1].close),
                std::fabs(bars_[i].low  - bars_[i-1].close)
            });
            atr_avg_ = (atr_avg_ * (ATR_P-1) + tr) / ATR_P;
        }
        ind.atr14.store(atr_avg_, std::memory_order_relaxed);

        // Maintain ATR history for slope calculation
        atr_history_.push_back(atr_avg_);
        if (static_cast<int>(atr_history_.size()) > ATR_SLOPE_BARS + 3)
            atr_history_.pop_front();
    }

    // ?????????????????????????????????????????????????????????????????????????
    // _update_bollinger()
    // ?????????????????????????????????????????????????????????????????????????
    void _update_bollinger() {
        const int n = static_cast<int>(bars_.size());
        if (n < BB_P) return;
        double sum = 0;
        for (int i = n - BB_P; i < n; ++i) sum += bars_[i].close;
        const double mid = sum / BB_P;
        double sq_sum = 0;
        for (int i = n - BB_P; i < n; ++i) {
            const double d = bars_[i].close - mid;
            sq_sum += d * d;
        }
        const double sigma = std::sqrt(sq_sum / BB_P);
        const double upper = mid + 2.0 * sigma;
        const double lower = mid - 2.0 * sigma;
        const double cur   = bars_.back().close;
        const double pct   = (upper > lower) ? (cur - lower) / (upper - lower) : 0.5;
        ind.bb_upper.store(upper, std::memory_order_relaxed);
        ind.bb_mid  .store(mid,   std::memory_order_relaxed);
        ind.bb_lower.store(lower, std::memory_order_relaxed);
        ind.bb_pct  .store(std::max(0.0, std::min(1.0, pct)), std::memory_order_relaxed);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // _update_volume_ratio()
    // ?????????????????????????????????????????????????????????????????????????
    void _update_volume_ratio() {
        const int n = static_cast<int>(bars_.size());
        if (n < VOL_MA_P + 1) return;
        double sum = 0;
        for (int i = n - VOL_MA_P - 1; i < n - 1; ++i) sum += bars_[i].volume;
        const double avg = sum / VOL_MA_P;
        const double ratio = (avg > 0) ? bars_.back().volume / avg : 1.0;
        ind.vol_ratio.store(ratio, std::memory_order_relaxed);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // _update_swing_and_trend()
    // ?????????????????????????????????????????????????????????????????????????
    void _update_swing_and_trend() {
        const int n = static_cast<int>(bars_.size());
        if (n < SWING_P * 2 + 1) return;

        std::vector<double> sh, sl;
        const int end = n - SWING_P - 1;
        for (int i = SWING_P; i <= end; ++i) {
            bool is_sh = true, is_sl = true;
            for (int j = i - SWING_P; j <= i + SWING_P; ++j) {
                if (j == i) continue;
                if (bars_[j].high >= bars_[i].high) { is_sh = false; }
                if (bars_[j].low  <= bars_[i].low ) { is_sl = false; }
            }
            if (is_sh) sh.push_back(bars_[i].high);
            if (is_sl) sl.push_back(bars_[i].low);
        }

        if (sh.size() >= 2) ind.swing_high.store(sh.back(), std::memory_order_relaxed);
        if (sl.size() >= 2) ind.swing_low .store(sl.back(), std::memory_order_relaxed);

        if (sh.size() >= 2 && sl.size() >= 2) {
            const bool hh = sh.back() > sh[sh.size()-2];
            const bool hl = sl.back() > sl[sl.size()-2];
            const bool lh = sh.back() < sh[sh.size()-2];
            const bool ll = sl.back() < sl[sl.size()-2];
            int state = 0;
            if (hh && hl)      state = +1;
            else if (lh && ll) state = -1;
            ind.trend_state.store(state, std::memory_order_relaxed);
        }
    }

    // ?????????????????????????????????????????????????????????????????????????
    // NEW: _update_bbw_squeeze()
    // Bollinger Band Width = (upper - lower) / mid (normalised ratio)
    // Squeeze = current BBW is at its N-bar minimum ? coiling, breakout imminent
    // Called once per bar after _update_bollinger().
    //
    // The GoldStack "regime lag" issue from session notes: 16:35 surge was missed
    // because GoldStack was in MR regime during a band squeeze that preceded the
    // breakout. bb_squeeze = true should arm a "breakout watch" state in GoldStack.
    // ?????????????????????????????????????????????????????????????????????????
    void _update_bbw_squeeze() {
        const int n = static_cast<int>(bars_.size());
        if (n < BB_P + BB_SQUEEZE_LOOKBACK) return;

        const double upper = ind.bb_upper.load(std::memory_order_relaxed);
        const double lower = ind.bb_lower.load(std::memory_order_relaxed);
        const double mid   = ind.bb_mid.load(std::memory_order_relaxed);
        if (mid <= 0.0) return;

        const double bbw_now = (upper - lower) / mid;
        ind.bb_width.store(bbw_now, std::memory_order_relaxed);

        // Find minimum BBW over the last BB_SQUEEZE_LOOKBACK bars
        // Recompute Bollinger for each lookback step (once per bar, not per tick -- acceptable cost)
        double bbw_min = bbw_now;
        for (int k = 1; k <= BB_SQUEEZE_LOOKBACK; ++k) {
            const int end_idx = n - k;
            if (end_idx < BB_P) break;
            double s = 0.0;
            for (int i = end_idx - BB_P; i < end_idx; ++i) s += bars_[i].close;
            const double m_k = s / BB_P;
            if (m_k <= 0.0) continue;
            double sq = 0.0;
            for (int i = end_idx - BB_P; i < end_idx; ++i) {
                const double d = bars_[i].close - m_k;
                sq += d * d;
            }
            const double sig_k  = std::sqrt(sq / BB_P);
            const double bbw_k  = (4.0 * sig_k) / m_k;  // (upper-lower)/mid = 4*sigma/mid
            if (bbw_k < bbw_min) bbw_min = bbw_k;
        }
        ind.bb_width_min.store(bbw_min, std::memory_order_relaxed);

        // Squeeze: current BBW within 5% of N-bar minimum
        const bool squeeze = (bbw_now <= bbw_min * 1.05);
        ind.bb_squeeze.store(squeeze, std::memory_order_relaxed);

        if (squeeze) {
            const int prev = ind.bb_squeeze_bars.load(std::memory_order_relaxed);
            ind.bb_squeeze_bars.store(prev + 1, std::memory_order_relaxed);
        } else {
            ind.bb_squeeze_bars.store(0, std::memory_order_relaxed);
        }
    }

    // ?????????????????????????????????????????????????????????????????????????
    // NEW: _update_atr_slope()
    // ATR slope over last ATR_SLOPE_BARS (3) bars.
    // Positive = volatility expanding = breakout/momentum mode
    // Negative = volatility contracting = mean-reversion / compression mode
    // atr_expanding  = each of last 3 bars had higher ATR than previous
    // atr_contracting = each of last 3 bars had lower ATR than previous
    // ?????????????????????????????????????????????????????????????????????????
    void _update_atr_slope() {
        const int sz = static_cast<int>(atr_history_.size());
        if (sz < ATR_SLOPE_BARS + 1) return;

        const double atr_now  = atr_history_[sz - 1];
        const double atr_prev = atr_history_[sz - 1 - ATR_SLOPE_BARS];
        ind.atr_slope.store(atr_now - atr_prev, std::memory_order_relaxed);

        bool all_expanding   = true;
        bool all_contracting = true;
        for (int i = sz - ATR_SLOPE_BARS; i < sz; ++i) {
            const double diff = atr_history_[i] - atr_history_[i-1];
            if (diff <= 0.0) all_expanding   = false;
            if (diff >= 0.0) all_contracting = false;
        }
        ind.atr_expanding  .store(all_expanding,   std::memory_order_relaxed);
        ind.atr_contracting.store(all_contracting, std::memory_order_relaxed);
    }

    // Seed ATR history from full bar set on startup bulk load
    void _seed_atr_history() {
        if (!atr_init_) return;
        atr_history_.clear();
        const int n = static_cast<int>(bars_.size());
        // Walk last ATR_SLOPE_BARS+3 bars computing true ATR at each step
        const int start = std::max(1, n - (ATR_SLOPE_BARS + 3));
        double running = atr_avg_;
        for (int i = start; i < n; ++i) {
            const double tr = std::max({
                bars_[i].high - bars_[i].low,
                std::fabs(bars_[i].high - bars_[i-1].close),
                std::fabs(bars_[i].low  - bars_[i-1].close)
            });
            running = (running * (ATR_P-1) + tr) / ATR_P;
            atr_history_.push_back(running);
        }
    }

    // ?????????????????????????????????????????????????????????????????????????
    // NEW: _update_rsi_divergence()
    // Detects classic price/RSI divergence at confirmed M1 swing pivots.
    //
    // BEARISH divergence (bear div): price higher high, RSI lower high
    //   ? buying momentum weakening; expect reversal or correction
    //   ? used by Gate 3 to tighten short signal confirmation
    //   ? used as confluence bonus on SHORT signals
    //
    // BULLISH divergence (bull div): price lower low, RSI higher low
    //   ? selling momentum weakening; expect bounce
    //   ? used as confluence bonus on LONG signals
    //   ? especially reliable when RSI < 40 at the low
    //
    // Note: RSI at each pivot is the RSI value *at that bar*, not current RSI.
    // We approximate by storing current RSI when the pivot is detected --
    // since pivots require SWING_P (3) bars confirmation lag, the RSI stored
    // is the RSI from 3 bars after the pivot bar. This is a slight overestimate
    // of the actual pivot RSI but preserves the divergence sign reliably.
    // ?????????????????????????????????????????????????????????????????????????
    void _update_rsi_divergence() {
        const int n = static_cast<int>(bars_.size());
        if (n < SWING_P * 2 + RSI_P + 2) return;
        if (!rsi_init_) return;

        const double current_rsi  = ind.rsi14.load(std::memory_order_relaxed);
        const int    pivot_idx    = n - SWING_P - 1;
        if (pivot_idx < SWING_P) return;

        bool is_sh = true, is_sl = true;
        for (int j = pivot_idx - SWING_P; j <= pivot_idx + SWING_P; ++j) {
            if (j == pivot_idx) continue;
            if (j < 0 || j >= n) continue;
            if (bars_[j].high >= bars_[pivot_idx].high) is_sh = false;
            if (bars_[j].low  <= bars_[pivot_idx].low ) is_sl = false;
        }

        if (is_sh) {
            PivotRSI p;
            p.price = bars_[pivot_idx].high;
            p.rsi   = current_rsi;
            pivot_highs_rsi_.push_back(p);
            if (static_cast<int>(pivot_highs_rsi_.size()) > RSI_DIV_LOOKBACK)
                pivot_highs_rsi_.pop_front();
        }
        if (is_sl) {
            PivotRSI p;
            p.price = bars_[pivot_idx].low;
            p.rsi   = current_rsi;
            pivot_lows_rsi_.push_back(p);
            if (static_cast<int>(pivot_lows_rsi_.size()) > RSI_DIV_LOOKBACK)
                pivot_lows_rsi_.pop_front();
        }

        bool   bull_div    = false;
        bool   bear_div    = false;
        double div_strength = 0.0;

        // Bearish: compare last 2 confirmed swing highs
        if (static_cast<int>(pivot_highs_rsi_.size()) >= 2) {
            const auto& prev_h = pivot_highs_rsi_[pivot_highs_rsi_.size()-2];
            const auto& last_h = pivot_highs_rsi_[pivot_highs_rsi_.size()-1];
            const bool price_hh = (last_h.price > prev_h.price); // price made HH
            const bool rsi_lh   = (last_h.rsi   < prev_h.rsi);   // RSI made LH
            const bool rsi_ob   = (last_h.rsi > 55.0);            // at meaningful OB level
            if (price_hh && rsi_lh && rsi_ob) {
                bear_div    = true;
                div_strength = prev_h.rsi - last_h.rsi; // RSI pts of divergence
            }
        }

        // Bullish: compare last 2 confirmed swing lows
        if (static_cast<int>(pivot_lows_rsi_.size()) >= 2) {
            const auto& prev_l = pivot_lows_rsi_[pivot_lows_rsi_.size()-2];
            const auto& last_l = pivot_lows_rsi_[pivot_lows_rsi_.size()-1];
            const bool price_ll = (last_l.price < prev_l.price); // price made LL
            const bool rsi_hl   = (last_l.rsi   > prev_l.rsi);   // RSI made HL
            const bool rsi_os   = (last_l.rsi < 45.0);            // at meaningful OS level
            if (price_ll && rsi_hl && rsi_os) {
                bull_div    = true;
                div_strength = last_l.rsi - prev_l.rsi;
            }
        }

        ind.rsi_bull_div   .store(bull_div,    std::memory_order_relaxed);
        ind.rsi_bear_div   .store(bear_div,    std::memory_order_relaxed);
        ind.rsi_div_strength.store(div_strength, std::memory_order_relaxed);
    }

    // ?????????????????????????????????????????????????????????????????????????
    // NEW: _update_vwap_slope()
    // Uses EMA50 as rolling VWAP proxy -- volume data is not available in
    // the bar engine (cTrader doesn't send volume in trendbar messages reliably).
    // EMA50 is the slowest EMA we compute and closely tracks the session anchor.
    //
    // Slope is computed by unwinding the EMA50 EWM recursion 3 bars back:
    //   ema[t-1] = (ema[t] - alpha * close[t]) / (1 - alpha)
    // This is mathematically exact for the EWM formula used.
    //
    // slope > +VWAP_SLOPE_FLAT_THR: session VWAP proxy is rising
    //   ? uptrend context; SHORT entries need extra confirmation
    //   ? GoldFlow LONG entries have tailwind
    // slope < -VWAP_SLOPE_FLAT_THR: session VWAP proxy is falling
    //   ? downtrend context; LONG entries need extra confirmation
    // |slope| < VWAP_SLOPE_FLAT_THR: VWAP flat ? mean-reversion mode
    //   ? VWAPReversion entries have highest expected value
    // ?????????????????????????????????????????????????????????????????????????
    void _update_vwap_slope() {
        if (!ema_init_) return;
        const int n = static_cast<int>(bars_.size());
        if (n < EMA50_P + 4) return;

        const double a50 = 2.0 / (EMA50_P + 1.0);

        // Unwind EMA50 recursion 3 steps to approximate EMA50 3 bars ago
        // ema[t-1] = (ema[t] - alpha * close[t]) / (1 - alpha)
        double ema_approx = ema50_;
        for (int k = n - 1; k >= n - 3 && k >= 1; --k) {
            const double denom = 1.0 - a50;
            if (std::fabs(denom) < 1e-9) break;
            ema_approx = (ema_approx - a50 * bars_[k].close) / denom;
        }

        const double slope = ema50_ - ema_approx; // pts over 3 bars
        ind.vwap_slope.store(slope, std::memory_order_relaxed);

        int direction = 0;
        if      (slope >  VWAP_SLOPE_FLAT_THR) direction = +1;
        else if (slope < -VWAP_SLOPE_FLAT_THR) direction = -1;
        ind.vwap_direction.store(direction, std::memory_order_relaxed);
    }
};

// =============================================================================
// Per-symbol bar engine registry -- maps symbol name to engine + indicators
// =============================================================================
struct SymBarState {
    OHLCBarEngine m1;   // 1-minute bars
    OHLCBarEngine m5;   // 5-minute bars -- swing/trend use this
    OHLCBarEngine m15;  // 15-minute bars -- TrendPB swing trades (XAUUSD only)
    OHLCBarEngine h4;   // 4-hour bars   -- HTF regime gate for TrendPB (XAUUSD only)
                        //   EMA9/21/50 half-lives: 18h / 42h / 100h
                        //   trend_state gates M15 entry direction
                        //   m1_ready=true after 14 H4 bars = 56 hours (warm restart immediate)
    // Expose combined indicators:
    //   RSI, EMA, ATR, BB, BBW squeeze, ATR slope, RSI div, VWAP slope from M1
    //   trend_state, swing from M5
    //   trend_state, swing, EMA9/21/50 from M15 (gold TrendPB only)
    //   trend_state, EMA9/21/50 from H4  (gold HTF gate only)
};

// =============================================================================
// Global bar state registry -- accessed by trading engines
// =============================================================================
// Declare globally in main.cpp:
//   SymBarState g_bars_gold;   // XAUUSD
//   (add more symbols as needed)
//
// Access pattern (lock-free reads):
//   const double rsi          = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
//   const int    trend        = g_bars_gold.m5.ind.trend_state.load(std::memory_order_relaxed);
//   const double atr          = g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed);
//   const bool   squeeze      = g_bars_gold.m1.ind.bb_squeeze.load(std::memory_order_relaxed);
//   const int    sq_bars      = g_bars_gold.m1.ind.bb_squeeze_bars.load(std::memory_order_relaxed);
//   const bool   atr_expand   = g_bars_gold.m1.ind.atr_expanding.load(std::memory_order_relaxed);
//   const double atr_slope    = g_bars_gold.m1.ind.atr_slope.load(std::memory_order_relaxed);
//   const bool   bull_div     = g_bars_gold.m1.ind.rsi_bull_div.load(std::memory_order_relaxed);
//   const bool   bear_div     = g_bars_gold.m1.ind.rsi_bear_div.load(std::memory_order_relaxed);
//   const int    vwap_dir     = g_bars_gold.m1.ind.vwap_direction.load(std::memory_order_relaxed);
//   const double spread_ratio = g_bars_gold.m1.ind.spread_ratio.load(std::memory_order_relaxed);
//   const double tick_rate    = g_bars_gold.m1.ind.tick_rate.load(std::memory_order_relaxed);
//   const bool   tick_storm   = g_bars_gold.m1.ind.tick_storm.load(std::memory_order_relaxed);
//   const double vol_delta    = g_bars_gold.m1.ind.vol_delta_ratio.load(std::memory_order_relaxed);
//
// REQUIRED per-tick calls in on_tick() (main.cpp):
//   g_bars_gold.m1.update_tick_metrics(ask - bid, now_ms);
//   g_bars_gold.m1.update_volume_delta(g_macro_ctx.gold_l2_imbalance);
// =============================================================================
