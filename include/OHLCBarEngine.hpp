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
    std::atomic<bool>   m1_ready{false};     // true once RSI_P+1 bars loaded -- RSI is real
    std::atomic<bool>   m1_ema_live{false};  // true once RSI_P+1 LIVE bars closed since startup
                                             // EMA crossover direction only valid when this is true.
                                             // m1_ready can be set by load_indicators (disk state) --
                                             // EMA loaded from disk reflects prior session direction,
                                             // not current. m1_ema_live requires actual live bar closes.
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

    // ?? NEW: ADX(14) -- Wilder-smoothed directional movement index ???????????
    // adx14: 0-100 trend strength. >25 = trending, >40 = strong trend.
    // adx_rising: ADX increasing (trend strengthening)
    // adx_trending: ADX >= ADX_TREND_THRESHOLD (25)
    // adx_strong:   ADX >= ADX_STRONG_THRESHOLD (40) -- conviction boost
    std::atomic<double> adx14         {0.0};   // current ADX value
    std::atomic<bool>   adx_rising    {false}; // ADX increasing bar-over-bar
    std::atomic<bool>   adx_trending  {false}; // ADX >= 25
    std::atomic<bool>   adx_strong    {false}; // ADX >= 40

    // ?? NEW: EWMA realised volatility (RiskMetrics) ??????????????????????????????????
    // ewma_vol_20:  short-lambda (0.94) annualised vol -- reactive to recent moves
    // ewma_vol_100: long-lambda  (0.97) annualised vol -- stable baseline
    // vol_ratio_ewma: short / long -- >1.2 = vol expanding, <0.8 = vol compressing
    // vol_target_mult: VOL_TARGET(20%) / ewma_vol_20, clamped [0.25, 1.50]
    //   Use as position size multiplier: high vol = reduce size, low vol = increase
    std::atomic<double> ewma_vol_20   {0.0};   // short EWMA annualised vol
    std::atomic<double> ewma_vol_100  {0.0};   // long  EWMA annualised vol
    std::atomic<double> vol_ratio_ewma{1.0};   // short/long vol ratio
    std::atomic<double> vol_target_mult{1.0};  // sizing multiplier clamped [0.25,1.50]

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

    // ?? ADX constants ????????????????????????????????????????????????????????
    static constexpr int    ADX_P                = 14;   // Wilder period
    static constexpr double ADX_TREND_THRESHOLD  = 25.0; // trending regime
    static constexpr double ADX_STRONG_THRESHOLD = 40.0; // strong trend / conviction boost

    // ?? EWMA vol constants ???????????????????????????????????????????????????
    static constexpr double EWMA_LAMBDA_SHORT    = 0.94; // RiskMetrics short (reactive)
    static constexpr double EWMA_LAMBDA_LONG     = 0.97; // RiskMetrics long  (stable)
    static constexpr double VOL_TARGET_ANNUAL    = 0.20; // 20% annualised vol target
    static constexpr double VOL_MULT_MIN         = 0.25; // floor on vol_target_mult
    static constexpr double VOL_MULT_MAX         = 1.50; // ceiling on vol_target_mult
    // XAUUSD M1: 252 trading days * 24h * 60min = 362880 bars/year (24h market)
    // Use 365*24*60 = 525600 for continuous futures approximation
    static constexpr double BARS_PER_YEAR        = 525600.0;

    // Indicators exposed to other threads
    BarIndicators ind;

    // ?????????????????????????????????????????????????????????????????????????
    // add_bar() -- call on every newly-closed bar
    // ?????????????????????????????????????????????????????????????????????????
    void add_bar(const OHLCBar& bar) {
        bars_.push_back(bar);
        if (bars_.size() > 300) bars_.pop_front();

        const int n = static_cast<int>(bars_.size());

        // EMA and ATR work from bar 1 -- real data, no synthetic seed needed
        _update_ema();
        _update_atr();

        // Other indicators need minimum bars for mathematical validity
        if (n >= RSI_P + 1)  _update_rsi();
        if (n >= BB_P)       _update_bollinger();
        if (n >= VOL_MA_P)   _update_volume_ratio();
        if (n >= SWING_P*2+1) _update_swing_and_trend();
        if (n >= BB_SQUEEZE_LOOKBACK) _update_bbw_squeeze();
        if (n >= ATR_SLOPE_BARS+1)    _update_atr_slope();
        if (n >= RSI_DIV_LOOKBACK+RSI_P) _update_rsi_divergence();
        if (n >= 3)          _update_vwap_slope();
        if (n >= ADX_P + 1)  _update_adx();
        if (n >= 2)          _update_ewma_vol();

        // Ready once RSI has real values -- needs RSI_P+1 bars (15 min from ticks).
        if (!ind.m1_ready.load() && n >= RSI_P + 1) ind.m1_ready.store(true);
        // EMA live: requires RSI_P+1 LIVE bar closes (never set by load_indicators).
        // EMA direction loaded from disk reflects prior session -- not current market.
        // Only trust EMA crossover for trend gating once live bars have updated it.
        if (!ind.m1_ema_live.load() && n >= RSI_P + 1) ind.m1_ema_live.store(true);
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
        if (n < 1) return;
        _update_ema();
        _update_atr();
        if (n >= RSI_P + 1)  _update_rsi();
        if (n >= BB_P)       _update_bollinger();
        if (n >= VOL_MA_P)   _update_volume_ratio();
        if (n >= SWING_P*2+1) _update_swing_and_trend();
        if (n >= BB_SQUEEZE_LOOKBACK) _update_bbw_squeeze();
        if (n >= ATR_SLOPE_BARS+1)    _update_atr_slope();
        if (n >= RSI_DIV_LOOKBACK+RSI_P) _update_rsi_divergence();
        if (n >= 3)          _update_vwap_slope();
        if (n >= ADX_P + 1)  _update_adx();
        if (n >= 2)          _update_ewma_vol();
        _seed_atr_history();
        if (n >= 1) ind.m1_ready.store(true);
    }

    int bar_count() const { return static_cast<int>(bars_.size()); }

    // get_bars(): read-only access for seeding external channel windows on startup
    const std::deque<OHLCBar>& get_bars() const noexcept { return bars_; }

    // last_bar() / prev_bar(): access last two completed bars for candle analysis.
    // Returns a zero-initialised bar if not enough bars available.
    OHLCBar last_bar() const noexcept {
        if (bars_.empty()) return OHLCBar{};
        return bars_.back();
    }
    OHLCBar prev_bar() const noexcept {
        if (bars_.size() < 2) return OHLCBar{};
        return bars_[bars_.size() - 2];
    }


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

    // ?? ADX private state ????????????????????????????????????????????????????
    // Wilder-smoothed +DM, -DM, TR accumulators (period = ADX_P)
    double adx_plus_dm_smooth_  = 0.0;
    double adx_minus_dm_smooth_ = 0.0;
    double adx_tr_smooth_       = 0.0;
    double adx_dx_smooth_       = 0.0;  // smoothed DX -> ADX
    double adx_prev_            = 0.0;  // previous bar ADX for rising detection
    bool   adx_init_            = false;

    // ?? EWMA vol private state ???????????????????????????????????????????????
    double ewma_short_ = 0.0;  // lambda=0.94 variance accumulator
    double ewma_long_  = 0.0;  // lambda=0.97 variance accumulator
    bool   ewma_init_  = false;

    // ?????????????????????????????????????????????????????????????????????????
    // _update_ema() -- works from bar 1 using all available history
    // ?????????????????????????????????????????????????????????????????????????
    // Standard approach: seed EMA at average of available bars, then apply
    // EWM updates forward. With n bars available:
    //   EMA9:  seed = avg(min(9,n) bars), then apply remaining updates
    //   EMA21: seed = avg(min(21,n) bars), then apply remaining updates
    //   EMA50: seed = avg(min(50,n) bars), then apply remaining updates
    // This gives real data from bar 1. Convergence accuracy:
    //   1 bar:  directional bias correct (trend up/down)
    //   3 bars: EMA9 stable, EMA21 directionally reliable
    //   10 bars: EMA50 directionally reliable for swing trades
    //   50 bars: EMA50 fully converged (precise values)
    // For H4 and M15 trend gates we need direction, not precision -- bar 1 is good enough.
    void _update_ema() {
        const int n = static_cast<int>(bars_.size());
        if (n < 1) return;
        if (!ema_init_) {
            // Seed each EMA at the mean of however many bars we have, up to its period
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
        if (n < 1) return;
        if (!atr_init_) {
            // Use all available bars (up to ATR_P) -- works from bar 1
            // With 1 bar: ATR = high-low (no prev close, use bar range only)
            // With 2+ bars: true range including prev close gaps
            double sum = 0;
            int count = 0;
            for (int i = std::max(1, n - ATR_P); i < n; ++i) {
                const double tr = std::max({
                    bars_[i].high - bars_[i].low,
                    std::fabs(bars_[i].high - bars_[i-1].close),
                    std::fabs(bars_[i].low  - bars_[i-1].close)
                });
                sum += tr; ++count;
            }
            if (count == 0) {
                // Single bar: use high-low range
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
                // FIX: use strict > / < (not >= / <=).
                // Old >= caused equal-high neighbours to disqualify valid swing pivots --
                // e.g. a double-top at $4500 produced bars_[j].high == bars_[i].high,
                // setting is_sh=false even though bar i IS the pivot.
                // Strict > means: bar i qualifies as swing high if NO neighbour is
                // strictly higher -- ties are fine, the first bar in the tie wins.
                if (bars_[j].high > bars_[i].high) { is_sh = false; }
                if (bars_[j].low  < bars_[i].low ) { is_sl = false; }
            }
            if (is_sh) sh.push_back(bars_[i].high);
            if (is_sl) sl.push_back(bars_[i].low);
        }

        if (sh.size() >= 2) ind.swing_high.store(sh.back(), std::memory_order_relaxed);
        if (sl.size() >= 2) ind.swing_low .store(sl.back(), std::memory_order_relaxed);

        // FIX: allow partial trend detection when only one side has 2+ confirmed swings.
        // Old code required BOTH sh.size()>=2 AND sl.size()>=2 before updating trend_state.
        // In a strong trend one side often dominates: a sharp uptrend makes rapid HHs
        // but the HL side takes many more bars to confirm (price barely pulls back).
        // Requiring both sides caused trend_state=0 (FLAT) throughout a clear uptrend,
        // blocking all OHLC-gated entries that check trend_state == +1.
        //
        // New logic: if only one side has 2+ swings, infer trend from that side alone:
        //   sh>=2, sl<2: HH -> probable uptrend, LH -> probable downtrend
        //   sl>=2, sh<2: HL -> probable uptrend, LL -> probable downtrend
        // Both sides confirmed: full HH+HL=UP, LH+LL=DOWN (unchanged, higher confidence).
        {
            int state = 0;
            if (sh.size() >= 2 && sl.size() >= 2) {
                // Full confirmation: both sides have 2+ swings
                const bool hh = sh.back() > sh[sh.size()-2];
                const bool hl = sl.back() > sl[sl.size()-2];
                const bool lh = sh.back() < sh[sh.size()-2];
                const bool ll = sl.back() < sl[sl.size()-2];
                if      (hh && hl) state = +1;
                else if (lh && ll) state = -1;
                // hh+ll or lh+hl = conflicting structure = FLAT (0)
            } else if (sh.size() >= 2) {
                // Only swing highs confirmed -- use them alone
                const bool hh = sh.back() > sh[sh.size()-2];
                const bool lh = sh.back() < sh[sh.size()-2];
                if      (hh) state = +1;  // higher highs = uptrend
                else if (lh) state = -1;  // lower highs  = downtrend
            } else if (sl.size() >= 2) {
                // Only swing lows confirmed -- use them alone
                const bool hl = sl.back() > sl[sl.size()-2];
                const bool ll = sl.back() < sl[sl.size()-2];
                if      (hl) state = +1;  // higher lows = uptrend
                else if (ll) state = -1;  // lower lows  = downtrend
            }
            // Only update when state changes -- avoids redundant atomic stores
            if (state != ind.trend_state.load(std::memory_order_relaxed))
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

    // ?????????????????????????????????????????????????????????????????????????
    // NEW: _update_adx()
    // Full Wilder-smoothed ADX(14).
    //
    // Algorithm:
    //   1. Compute raw +DM, -DM, TR for current bar vs previous bar
    //   2. Wilder-smooth each over ADX_P bars:
    //        smooth[t] = smooth[t-1] - smooth[t-1]/ADX_P + raw[t]
    //      (equivalent to EMA with alpha=1/ADX_P -- Wilder's convention)
    //   3. +DI14 = 100 * +DM_smooth / TR_smooth
    //      -DI14 = 100 * -DM_smooth / TR_smooth
    //   4. DX    = 100 * |+DI14 - -DI14| / (+DI14 + -DI14)
    //   5. ADX   = Wilder-smooth of DX over ADX_P bars
    //
    // Initialisation: first ADX_P bars accumulate raw sums, then switch to
    // Wilder recursion from bar ADX_P+1 onward (standard Wilder bootstrap).
    //
    // Exposes: adx14, adx_rising, adx_trending (>=25), adx_strong (>=40)
    // ?????????????????????????????????????????????????????????????????????????
    void _update_adx() {
        const int n = static_cast<int>(bars_.size());
        if (n < 2) return;

        const OHLCBar& cur  = bars_[n - 1];
        const OHLCBar& prev = bars_[n - 2];

        // Raw directional movement
        const double up   = cur.high - prev.high;
        const double down = prev.low  - cur.low;
        const double plus_dm  = (up > down && up > 0.0) ? up   : 0.0;
        const double minus_dm = (down > up && down > 0.0) ? down : 0.0;

        // True range
        const double tr = std::max({
            cur.high - cur.low,
            std::fabs(cur.high - prev.close),
            std::fabs(cur.low  - prev.close)
        });

        if (!adx_init_) {
            // Accumulate raw sums for first ADX_P bars
            adx_plus_dm_smooth_  += plus_dm;
            adx_minus_dm_smooth_ += minus_dm;
            adx_tr_smooth_       += tr;

            // Bootstrap complete once we have ADX_P bars of raw data
            // bars_ has n entries; we need n-1 diffs. At n == ADX_P+1 we have ADX_P diffs.
            if (n < ADX_P + 1) return;

            // Compute initial DX from bootstrapped sums
            const double pdi = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_plus_dm_smooth_  / adx_tr_smooth_ : 0.0;
            const double mdi = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_minus_dm_smooth_ / adx_tr_smooth_ : 0.0;
            const double denom = pdi + mdi;
            const double dx  = (denom > 1e-10) ? 100.0 * std::fabs(pdi - mdi) / denom : 0.0;
            adx_dx_smooth_ = dx;  // seed ADX = first DX (will smooth going forward)
            adx_prev_      = dx;
            adx_init_      = true;
            ind.adx14.store(dx, std::memory_order_relaxed);
            return;
        }

        // Wilder smoothing: smooth[t] = smooth[t-1] - smooth[t-1]/P + raw[t]
        adx_plus_dm_smooth_  = adx_plus_dm_smooth_  - adx_plus_dm_smooth_  / ADX_P + plus_dm;
        adx_minus_dm_smooth_ = adx_minus_dm_smooth_ - adx_minus_dm_smooth_ / ADX_P + minus_dm;
        adx_tr_smooth_       = adx_tr_smooth_       - adx_tr_smooth_       / ADX_P + tr;

        const double pdi = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_plus_dm_smooth_  / adx_tr_smooth_ : 0.0;
        const double mdi = (adx_tr_smooth_ > 1e-10) ? 100.0 * adx_minus_dm_smooth_ / adx_tr_smooth_ : 0.0;
        const double denom = pdi + mdi;
        const double dx  = (denom > 1e-10) ? 100.0 * std::fabs(pdi - mdi) / denom : 0.0;

        // Wilder-smooth DX -> ADX
        adx_dx_smooth_ = adx_dx_smooth_ - adx_dx_smooth_ / ADX_P + dx / ADX_P;
        // Note: Wilder ADX recursion is: ADX[t] = (ADX[t-1]*(P-1) + DX[t]) / P
        //       which is identical to: ADX[t] = ADX[t-1] - ADX[t-1]/P + DX[t]/P
        // We store the result directly:
        const double adx_val = adx_dx_smooth_;

        const bool rising   = (adx_val > adx_prev_);
        const bool trending = (adx_val >= ADX_TREND_THRESHOLD);
        const bool strong   = (adx_val >= ADX_STRONG_THRESHOLD);

        ind.adx14       .store(adx_val, std::memory_order_relaxed);
        ind.adx_rising  .store(rising,  std::memory_order_relaxed);
        ind.adx_trending.store(trending,std::memory_order_relaxed);
        ind.adx_strong  .store(strong,  std::memory_order_relaxed);

        adx_prev_ = adx_val;
    }

    // ?????????????????????????????????????????????????????????????????????????
    // NEW: _update_ewma_vol()
    // RiskMetrics EWMA realised volatility -- two lambdas (short=0.94, long=0.97).
    //
    // Algorithm:
    //   log_return[t] = ln(close[t] / close[t-1])
    //   var_short[t]  = lambda_short * var_short[t-1] + (1 - lambda_short) * r^2
    //   var_long[t]   = lambda_long  * var_long[t-1]  + (1 - lambda_long)  * r^2
    //   vol_annual    = sqrt(var * BARS_PER_YEAR)   -- annualise from per-bar variance
    //
    // vol_ratio_ewma = ewma_vol_20 / ewma_vol_100
    //   > 1.2: vol expanding (recent moves > baseline) -- reduce size
    //   < 0.8: vol compressing (quiet tape) -- normal or larger size
    //
    // vol_target_mult = VOL_TARGET_ANNUAL / ewma_vol_20
    //   Clamped [VOL_MULT_MIN, VOL_MULT_MAX] = [0.25, 1.50]
    //   At vol=20%: mult=1.00 (no adjustment)
    //   At vol=40%: mult=0.50 (half size -- extremely volatile)
    //   At vol=10%: mult=1.50 (cap -- don't over-size quiet sessions)
    // ?????????????????????????????????????????????????????????????????????????
    void _update_ewma_vol() {
        const int n = static_cast<int>(bars_.size());
        if (n < 2) return;

        const double c_prev = bars_[n - 2].close;
        const double c_cur  = bars_[n - 1].close;
        if (c_prev <= 0.0 || c_cur <= 0.0) return;

        const double log_ret = std::log(c_cur / c_prev);
        const double r2      = log_ret * log_ret;

        if (!ewma_init_) {
            // Seed both variances at the squared return of the first available pair
            ewma_short_ = r2;
            ewma_long_  = r2;
            ewma_init_  = true;
        } else {
            ewma_short_ = EWMA_LAMBDA_SHORT * ewma_short_ + (1.0 - EWMA_LAMBDA_SHORT) * r2;
            ewma_long_  = EWMA_LAMBDA_LONG  * ewma_long_  + (1.0 - EWMA_LAMBDA_LONG)  * r2;
        }

        // Annualise: vol = sqrt(var_per_bar * bars_per_year)
        const double vol_s = std::sqrt(ewma_short_ * BARS_PER_YEAR);
        const double vol_l = std::sqrt(ewma_long_  * BARS_PER_YEAR);

        const double ratio = (vol_l > 1e-10) ? vol_s / vol_l : 1.0;

        // vol_target_mult: target_vol / current_vol, clamped
        const double raw_mult = (vol_s > 1e-10) ? VOL_TARGET_ANNUAL / vol_s : 1.0;
        const double mult = std::max(VOL_MULT_MIN, std::min(VOL_MULT_MAX, raw_mult));

        ind.ewma_vol_20   .store(vol_s, std::memory_order_relaxed);
        ind.ewma_vol_100  .store(vol_l, std::memory_order_relaxed);
        ind.vol_ratio_ewma.store(ratio, std::memory_order_relaxed);
        ind.vol_target_mult.store(mult, std::memory_order_relaxed);
    }

    // =========================================================================
public:
    // Persistence -- save/load indicator state so restart is instant.
    // Eliminates the need for any tick data request at startup.
    // All bar-computed indicators (EMA, ATR, RSI, trend, swing) are restored
    // from disk and m1_ready is set true immediately on load.
    // =========================================================================
    void save_indicators(const std::string& path) const noexcept {
        if (!ind.m1_ready.load()) return;  // don't save cold state

        // Sanity check: reject flat/holiday state before writing to disk.
        // Flat state happens when bars are built from a holiday session with no
        // price movement — all EMAs identical, RSI=100, BB bands collapsed.
        // Loading this on next restart gives garbage indicators worse than cold start.
        const double e9  = ind.ema9 .load();
        const double e50 = ind.ema50.load();
        const double atr = ind.atr14.load();
        const double rsi = ind.rsi14.load();
        const double bbu = ind.bb_upper.load();
        const double bbl = ind.bb_lower.load();
        // Reject if: EMAs identical (flat), RSI pegged at 0/100, ATR too small,
        // or Bollinger bands collapsed to a single line
        const bool flat_emas   = (std::fabs(e9 - e50) < 0.01 && e9 > 0.0);
        const bool pegged_rsi  = (rsi < 5.0 || rsi > 95.0);
        // ATR floor: 2.0pt minimum for gold instruments (XAUUSD price ~$4700).
        // Previous floor of 1.0pt allowed Sunday/holiday M1 ATR of 1.03pt to be
        // saved, producing stale bar state that seeded engines with wrong volatility.
        // Gold M1 ATR in a real session is never below 2pt. M15 ATR is never below 5pt.
        // H1 ATR is never below 6pt. H4 ATR is never below 15pt.
        // This floor applies to all timeframes via the same save path -- the 2pt floor
        // is conservative enough for M1 (real floor ~3pt) and is not restrictive for HTF.
        const bool tiny_atr    = (atr < 2.0 && atr > 0.0);
        const bool flat_bb     = (std::fabs(bbu - bbl) < 0.01 && bbu > 0.0);
        if (flat_emas || pegged_rsi || tiny_atr || flat_bb) {
            printf("[OHLC] save_indicators SKIPPED -- flat/holiday state detected "
                   "(e9=%.2f e50=%.2f atr=%.3f rsi=%.1f bb_range=%.3f). "
                   "Will retry when real market data arrives.\n",
                   e9, e50, atr, rsi, bbu - bbl);
            fflush(stdout);
            return;
        }

        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        fprintf(f, "saved_ts=%lld\n",  (long long)std::time(nullptr));
        fprintf(f, "ema9=%.6f\n",      e9);
        fprintf(f, "ema21=%.6f\n",     ind.ema21.load());
        fprintf(f, "ema50=%.6f\n",     e50);
        fprintf(f, "atr14=%.6f\n",     atr);
        fprintf(f, "rsi14=%.4f\n",     rsi);
        fprintf(f, "bb_upper=%.4f\n",  bbu);
        fprintf(f, "bb_mid=%.4f\n",    ind.bb_mid.load());
        fprintf(f, "bb_lower=%.4f\n",  bbl);
        fprintf(f, "bb_pct=%.4f\n",    ind.bb_pct.load());
        fprintf(f, "trend_state=%d\n", ind.trend_state.load());
        fprintf(f, "swing_high=%.4f\n",ind.swing_high.load());
        fprintf(f, "swing_low=%.4f\n", ind.swing_low.load());
        fclose(f);
    }

    // Returns true if state loaded successfully and m1_ready set.
    bool load_indicators(const std::string& path) noexcept {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return false;
        char line[128];
        double e9=0, e21=0, e50=0, atr=0, rsi=50, bbu=0, bbm=0, bbl=0, bbp=0.5;
        double swhi=0, swlo=0;
        int trend=0;
        long long saved_ts = 0;
        while (fgets(line, sizeof(line), f)) {
            char key[32]; double val=0;
            if (sscanf(line, "%31[^=]=%lf", key, &val) == 2) {
                std::string k(key);
                if      (k=="saved_ts")   saved_ts = (long long)val;
                else if (k=="ema9")       e9    = val;
                else if (k=="ema21")      e21   = val;
                else if (k=="ema50")      e50   = val;
                else if (k=="atr14")      atr   = val;
                else if (k=="rsi14")      rsi   = val;
                else if (k=="bb_upper")   bbu   = val;
                else if (k=="bb_mid")     bbm   = val;
                else if (k=="bb_lower")   bbl   = val;
                else if (k=="bb_pct")     bbp   = val;
                else if (k=="trend_state")trend = (int)val;
                else if (k=="swing_high") swhi  = val;
                else if (k=="swing_low")  swlo  = val;
            }
        }
        fclose(f);
        const long long now_ts = (long long)std::time(nullptr);
        const long long age    = now_ts - saved_ts;
        // Reject if: invalid timestamp, future timestamp, or older than 12 hours
        // WHY 12h not 4h: bars saved at end of NY session (22:00 UTC) must still
        // be valid at London open (07:00 UTC next day = 9 hours later). With 4h
        // limit the file was rejected every morning, forcing a cold tick-data
        // request that times out -> m1_ready=false all session -> bars never seed.
        // Corruption check: zero/negative timestamp or future timestamp = file is corrupt.
        // Delete corrupt files only -- never delete on age or missing EMAs.
        // Root cause: deleting on e9<=0 meant any session where bars never fully seeded
        // (EMAs cold) would destroy the file, forcing cold start every subsequent restart.
        // Widened age limit 24h->36h: bars saved at NY close (22:00 UTC) must survive
        // through London open (07:00 UTC) + a missed session = 33h max gap.
        const bool corrupt = (saved_ts <= 0 || saved_ts > now_ts);
        const bool too_old = (age > 36 * 3600);
        const bool no_emas = (e9 <= 0 || e50 <= 0);
        if (corrupt) {
            remove(path.c_str());
            printf("[OHLC] Bar state CORRUPT (saved_ts=%lld) -- deleted, cold start\n", saved_ts);
            return false;
        }
        if (too_old || no_emas) {
            // Keep the file -- do not delete. On next restart it may be valid again,
            // or it may be overwritten by a successful save. Deleting causes cold start.
            printf("[OHLC] Bar state SKIPPED (age=%llds e9=%.2f e50=%.2f) -- cold start, file kept\n",
                   age, e9, e50);
            return false;
        }
        // Restore all indicators
        ind.ema9       .store(e9,    std::memory_order_relaxed);
        ind.ema21      .store(e21,   std::memory_order_relaxed);
        ind.ema50      .store(e50,   std::memory_order_relaxed);
        ind.atr14      .store(atr,   std::memory_order_relaxed);
        ind.rsi14      .store(rsi,   std::memory_order_relaxed);
        ind.bb_upper   .store(bbu,   std::memory_order_relaxed);
        ind.bb_mid     .store(bbm,   std::memory_order_relaxed);
        ind.bb_lower   .store(bbl,   std::memory_order_relaxed);
        ind.bb_pct     .store(bbp,   std::memory_order_relaxed);
        ind.trend_state.store(trend, std::memory_order_relaxed);
        ind.swing_high .store(swhi,  std::memory_order_relaxed);
        ind.swing_low  .store(swlo,  std::memory_order_relaxed);
        // Mark ready -- this is the key: unblocks all bar-gated entries immediately
        ind.m1_ready   .store(true,  std::memory_order_relaxed);
        printf("[OHLC] Bar state loaded: EMA50=%.2f ATR=%.2f RSI=%.1f trend=%d age=%llds\n",
               e50, atr, rsi, trend, age);
        fflush(stdout);
        return true;
    }
};
// Per-symbol bar engine registry -- maps symbol name to engine + indicators
// =============================================================================
struct SymBarState {
    OHLCBarEngine m1;   // 1-minute bars
    OHLCBarEngine m5;   // 5-minute bars -- swing/trend use this
    OHLCBarEngine m15;  // 15-minute bars -- TrendPB swing trades (XAUUSD only)
    OHLCBarEngine h1;   // 1-hour bars   -- HTF swing context for SP/NQ/gold IndexSwingEngine
                        //   EMA9/21/50 half-lives: 4.5h / 10.5h / 25h
                        //   trend_state drives IndexSwingEngine H1 direction gate
                        //   m1_ready=true after 14 H1 bars = 14 hours (warm restart immediate)
    OHLCBarEngine h4;   // 4-hour bars   -- HTF regime gate for TrendPB (XAUUSD only)
                        //   EMA9/21/50 half-lives: 18h / 42h / 100h
                        //   trend_state gates M15 entry direction
                        //   m1_ready=true after 14 H4 bars = 56 hours (warm restart immediate)
    // Expose combined indicators:
    //   RSI, EMA, ATR, BB, BBW squeeze, ATR slope, RSI div, VWAP slope from M1
    //   trend_state, swing from M5
    //   trend_state, swing, EMA9/21/50 from M15 (gold TrendPB only)
    //   trend_state, EMA9/21/50 from H1  (SP/NQ/gold swing context)
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

