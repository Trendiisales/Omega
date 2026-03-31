#pragma once
// =============================================================================
// OHLCBarEngine.hpp — OHLC bar data from cTrader ProtoOA trendbar API
//
// Uses the SAME cTrader SSL connection as CTraderDepthClient.
// On startup (after auth): requests 200 M1 + 100 M5 historical bars.
// Subscribes to live bar close events (pt=2220).
//
// Computes per-bar for XAUUSD (and any subscribed symbol):
//   • EMA9 / EMA21 / EMA50  (on M1 close)
//   • RSI(14)               (on M1 close)
//   • ATR(14)               (on M1 H-L range) — replaces tick ATR
//   • Bollinger(20,2)       (on M1 close)
//   • Swing high/low        (3-bar pivot on M5)
//   • Trend state           (HH/HL = UP, LH/LL = DOWN, else FLAT)
//
// All outputs exposed as atomics — zero-lock hot path from FIX thread.
//
// ProtoOA trendbar message types (same framing as depth client):
//   2137 = GetTrendbarsReq    → request historical bars
//   2138 = GetTrendbarsRes    → response (repeated bars)
//   2220 = SubscribeLiveTrendbarReq → subscribe live bar push
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
//   field 2 = utcTimestampInMinutes (uint64) — bar open time in minutes since epoch
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

// =============================================================================
// OHLCBar — one closed candle
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
// BarIndicators — atomic outputs read by trading engines
// =============================================================================
struct BarIndicators {
    // M1 indicators
    std::atomic<double> ema9  {0.0};
    std::atomic<double> ema21 {0.0};
    std::atomic<double> ema50 {0.0};
    std::atomic<double> rsi14 {50.0}; // 0–100; 50 = neutral/uninitialised
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
    std::atomic<bool>   m1_ready{false}; // true once ≥50 M1 bars loaded
    std::atomic<bool>   m5_ready{false}; // true once ≥20 M5 bars loaded

    BarIndicators() = default;
    BarIndicators(const BarIndicators&) = delete;
    BarIndicators& operator=(const BarIndicators&) = delete;
};

// =============================================================================
// OHLCBarEngine — core computation (non-threaded, called from cTrader thread)
// =============================================================================
class OHLCBarEngine {
public:
    static constexpr int  EMA9_P    = 9;
    static constexpr int  EMA21_P   = 21;
    static constexpr int  EMA50_P   = 50;
    static constexpr int  RSI_P     = 14;
    static constexpr int  ATR_P     = 14;
    static constexpr int  BB_P      = 20;
    static constexpr int  SWING_P   = 3;   // bars each side for pivot
    static constexpr int  VOL_MA_P  = 20;
    static constexpr int  MIN_BARS  = 52;  // need at least 52 bars before emitting

    // Indicators exposed to other threads
    BarIndicators ind;

    // Add a newly-closed bar and recompute all indicators
    void add_bar(const OHLCBar& bar) {
        bars_.push_back(bar);
        if (bars_.size() > 300) bars_.pop_front(); // keep rolling 300 bars

        const int n = static_cast<int>(bars_.size());
        if (n < MIN_BARS) return;

        _update_ema();
        _update_rsi();
        _update_atr();
        _update_bollinger();
        _update_volume_ratio();
        _update_swing_and_trend();

        if (!ind.m1_ready.load() && n >= MIN_BARS) ind.m1_ready.store(true);
    }

    // Seed from historical bars (bulk load, no incremental emit)
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
        if (n >= MIN_BARS) ind.m1_ready.store(true);
    }

    int bar_count() const { return static_cast<int>(bars_.size()); }

private:
    std::deque<OHLCBar> bars_;

    // EMA state — stored for incremental update
    double ema9_  = 0.0, ema21_ = 0.0, ema50_ = 0.0;
    bool   ema_init_ = false;
    double rsi_avg_gain_ = 0.0, rsi_avg_loss_ = 0.0;
    bool   rsi_init_ = false;
    double atr_avg_ = 0.0;
    bool   atr_init_ = false;

    void _update_ema() {
        const int n = static_cast<int>(bars_.size());
        if (!ema_init_ && n >= EMA50_P) {
            // Seed EMAs with SMA of first N bars
            double s9=0, s21=0, s50=0;
            for (int i = 0; i < EMA50_P; ++i) s50 += bars_[i].close;
            for (int i = n-EMA9_P;  i < n; ++i) s9  += bars_[i].close;
            for (int i = n-EMA21_P; i < n; ++i) s21 += bars_[i].close;
            ema9_  = s9  / EMA9_P;
            ema21_ = s21 / EMA21_P;
            ema50_ = s50 / EMA50_P;
            // Advance ema50 from its seed point to current
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

    void _update_rsi() {
        const int n = static_cast<int>(bars_.size());
        if (n < RSI_P + 1) return;
        if (!rsi_init_) {
            double gain = 0, loss = 0;
            // Seed: average gain/loss over first RSI_P changes
            const int start = n - RSI_P - 1;
            for (int i = start + 1; i <= start + RSI_P; ++i) {
                const double d = bars_[i].close - bars_[i-1].close;
                if (d > 0) gain += d; else loss -= d;
            }
            rsi_avg_gain_ = gain / RSI_P;
            rsi_avg_loss_ = loss / RSI_P;
            rsi_init_ = true;
            // Advance to current bar
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
            // Advance to current
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
    }

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

    void _update_volume_ratio() {
        const int n = static_cast<int>(bars_.size());
        if (n < VOL_MA_P + 1) return;
        double sum = 0;
        for (int i = n - VOL_MA_P - 1; i < n - 1; ++i) sum += bars_[i].volume;
        const double avg = sum / VOL_MA_P;
        const double ratio = (avg > 0) ? bars_.back().volume / avg : 1.0;
        ind.vol_ratio.store(ratio, std::memory_order_relaxed);
    }

    void _update_swing_and_trend() {
        const int n = static_cast<int>(bars_.size());
        if (n < SWING_P * 2 + 1) return;

        // Detect swing highs and lows: bar[i] is a swing high if it has the
        // highest high of [i-SWING_P .. i+SWING_P]. Use confirmed bars only.
        std::vector<double> sh, sl; // swing highs, swing lows
        const int end = n - SWING_P - 1; // last confirmed pivot
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

        // Need at least 2 swing highs and 2 swing lows to determine trend
        if (sh.size() >= 2) ind.swing_high.store(sh.back(), std::memory_order_relaxed);
        if (sl.size() >= 2) ind.swing_low .store(sl.back(), std::memory_order_relaxed);

        if (sh.size() >= 2 && sl.size() >= 2) {
            const bool hh = sh.back() > sh[sh.size()-2]; // higher high
            const bool hl = sl.back() > sl[sl.size()-2]; // higher low
            const bool lh = sh.back() < sh[sh.size()-2]; // lower high
            const bool ll = sl.back() < sl[sl.size()-2]; // lower low

            int state = 0;
            if (hh && hl)       state = +1; // uptrend
            else if (lh && ll)  state = -1; // downtrend
            // hh && ll = expanding (unclear), lh && hl = contracting (flat)
            ind.trend_state.store(state, std::memory_order_relaxed);
        }
    }
};

// =============================================================================
// Per-symbol bar engine registry — maps symbol name to engine + indicators
// =============================================================================
struct SymBarState {
    OHLCBarEngine m1;   // 1-minute bars
    OHLCBarEngine m5;   // 5-minute bars — swing/trend use this
    // Expose combined indicators:
    //   RSI, EMA, ATR, BB from M1
    //   trend_state, swing from M5
};

// =============================================================================
// PB helpers for trendbar messages (added to PB namespace logic inline here)
// =============================================================================
namespace PB {

// ProtoOAGetTrendbarsReq (pt=2137)
// field 2: ctidTraderAccountId (uint64)
// field 3: symbolId (uint64)
// field 4: period (uint32): 1=M1, 2=M2, 3=M3, 5=M5, 15=M15, 30=M30, 60=H1...
//          Actually: ProtoOATrendbarPeriod: M1=1, M2=2, M3=3, M4=4, M5=5, M10=6,
//                    M15=7, M30=8, H1=9, H4=10, H12=11, D1=12, W1=13, MN1=14
// field 5: fromTimestamp (uint64) — epoch millis, start of range
// field 6: toTimestamp (uint64) — epoch millis, end of range
// field 7: count (uint32) — max bars to return (overrides from/to if set)
inline std::vector<uint8_t> get_trendbars_req(
    int64_t ctid, int64_t sym_id, uint32_t period,
    uint32_t count = 200,
    int64_t from_ms = 0, int64_t to_ms = 0)
{
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    write_field_varint(inner, 4, uint64_t(period));
    if (from_ms > 0) write_field_varint(inner, 5, uint64_t(from_ms));
    if (to_ms   > 0) write_field_varint(inner, 6, uint64_t(to_ms));
    if (count   > 0) write_field_varint(inner, 7, uint64_t(count));
    return frame_msg(2137, inner);
}

// ProtoOASubscribeLiveTrendbarReq (pt=2220)
// field 2: ctidTraderAccountId (uint64)
// field 3: symbolId (uint64)
// field 4: period (uint32)
inline std::vector<uint8_t> subscribe_trendbar_req(
    int64_t ctid, int64_t sym_id, uint32_t period)
{
    std::vector<uint8_t> inner;
    write_field_varint(inner, 2, uint64_t(ctid));
    write_field_varint(inner, 3, uint64_t(sym_id));
    write_field_varint(inner, 4, uint64_t(period));
    return frame_msg(2220, inner);
}

// Parse ProtoOATrendbar from bytes
// field 2: utcTimestampInMinutes (uint64)
// field 3: close    (uint64, price * 100000)
// field 5: volume   (uint64, in cents)
// field 6: openDelta  (int64 as sint64 — zigzag: close + openDelta = open)
// field 7: highDelta  (uint64 — close + highDelta = high)
// field 8: lowDelta   (uint64 — close - lowDelta = low)
// Note: deltas are relative to close price
inline OHLCBar parse_trendbar(const std::vector<uint8_t>& bytes, double price_scale = 100000.0) {
    OHLCBar bar;
    const auto f = parse(bytes);
    const uint64_t ts_min     = get_varint(f, 2);
    const uint64_t close_raw  = get_varint(f, 3);
    const uint64_t vol        = get_varint(f, 5);
    // Deltas: openDelta and lowDelta use sint64 (zigzag), highDelta uses uint64
    // In the actual proto: openDelta is sint64, highDelta and lowDelta are uint64
    // openDelta: zigzag decode
    int64_t open_delta = 0;
    for (const auto& fi : f) {
        if (fi.field_num == 6 && fi.wire_type == 0) {
            // sint64 zigzag decode
            const uint64_t zz = fi.varint;
            open_delta = static_cast<int64_t>((zz >> 1) ^ -(zz & 1));
        }
    }
    const uint64_t high_delta = get_varint(f, 7);
    const uint64_t low_delta  = get_varint(f, 8);

    bar.ts_min = int64_t(ts_min);
    bar.close  = double(close_raw)              / price_scale;
    bar.open   = double(int64_t(close_raw) + open_delta) / price_scale;
    bar.high   = double(close_raw + high_delta) / price_scale;
    bar.low    = double(int64_t(close_raw) - int64_t(low_delta)) / price_scale;
    bar.volume = vol;
    return bar;
}

} // namespace PB (additions)

// =============================================================================
// Global bar state registry — accessed by trading engines
// =============================================================================
// Declare globally in main.cpp:
//   SymBarState g_bars_gold;   // XAUUSD
//   (add more symbols as needed)
//
// Access pattern (lock-free reads):
//   const double rsi = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
//   const int trend  = g_bars_gold.m5.ind.trend_state.load(std::memory_order_relaxed);
//   const double atr = g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed);
// =============================================================================
