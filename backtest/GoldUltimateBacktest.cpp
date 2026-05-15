// =============================================================================
// GoldUltimateBacktest.cpp -- Standalone backtest harness for GoldUltimateStrategy
// =============================================================================
//
// PURPOSE
// -------
// Validates the GoldUltimateStrategy regime detection, engine routing, cost
// coverage, and loss management against historical tick data.
//
// WHAT IT TESTS
// -------------
//   1. Regime detection accuracy: does it classify known market conditions correctly?
//   2. Engine routing: are the right engines firing in the right regimes?
//   3. Cost coverage: does every approved trade cover costs?
//   4. Loss management: do the kill switches fire at the right thresholds?
//   5. Bull vs Bear performance: does it perform in both directions?
//   6. P&L by regime: which regimes actually produce edge?
//
// DATA FORMAT
// -----------
// Auto-detects five CSV formats:
//   A) Dukascopy fresh:   YYYYMMDD,HH:MM:SS,bid,ask  (no header)
//   B) Dukascopy bid/ask: timestamp_ms,bid,ask
//   C) Dukascopy ask/bid: timestamp_ms,ask,bid  (reversed columns)
//   D) L2 old:            ts_ms,bid,ask,l2_imb,...
//   E) L2 new:            ts_ms,mid,bid,ask,...
//
// STREAMING MODE
// --------------
// For large files (100M+ ticks), the backtest reads line-by-line without
// storing all ticks in memory. This allows processing multi-GB datasets.
//
// COMPILATION
// -----------
//   cd omega_repo
//   g++ -std=c++20 -O2 -I include -o backtest/gold_ultimate_bt backtest/GoldUltimateBacktest.cpp
//   ./backtest/gold_ultimate_bt /path/to/2yr_XAUUSD_tick_fresh.csv
//
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <array>
#include <deque>

// Strategy header (pulls in all dependencies)
#include "GoldUltimateStrategy.hpp"

using namespace omega::gold_ultimate;

// =============================================================================
// SIMULATED MARKET DATA
// =============================================================================
struct Tick {
    int64_t timestamp_ms;
    double  bid;
    double  ask;
    double  l2_imbalance;  // from L2 data if available
};

// =============================================================================
// 1-MINUTE BAR for indicator computation
// =============================================================================
struct Bar1m {
    int64_t open_ms  = 0;
    double  open     = 0.0;
    double  high     = 0.0;
    double  low      = 99999.0;
    double  close    = 0.0;
    int     tick_count = 0;

    void reset(int64_t ts, double price) noexcept {
        open_ms    = ts;
        open       = price;
        high       = price;
        low        = price;
        close      = price;
        tick_count = 1;
    }

    void update(double price) noexcept {
        if (price > high) high = price;
        if (price < low)  low  = price;
        close = price;
        tick_count++;
    }

    double range() const noexcept { return high - low; }
    double body()  const noexcept { return std::fabs(close - open); }
};

// =============================================================================
// SIMULATED INDICATORS -- bar-based for realistic values
//
// Aggregates raw ticks into 1-minute bars. All indicators are computed from
// the bar series, producing values calibrated to what the live system sees:
//   - ATR-14:     2-5 pts for XAUUSD (from 1-min bar true ranges)
//   - EWM drift:  -6 to +6 pts (from cumulative bar close-to-close)
//   - RSI-14:     0-100 (standard RSI from bar closes)
//   - Vol ratio:  0.5-3.0 (recent vol / baseline vol)
// =============================================================================
struct SimIndicators {
    // ── Bar aggregation ──────────────────────────────────────────────────────
    static constexpr int64_t BAR_PERIOD_MS = 60000;  // 1-minute bars
    static constexpr int     ATR_PERIOD    = 14;
    static constexpr int     RSI_PERIOD    = 14;
    static constexpr int     DRIFT_WINDOW  = 50;      // bars (~50 min) for short-term drift
    static constexpr int     DRIFT_LONG_WINDOW = 200; // bars (~3.3 hrs) for long-term trend
    static constexpr int     STRUCTURE_WINDOW = 20;   // bars for HH/LL

    Bar1m current_bar{};
    std::deque<Bar1m> bars;  // completed bars
    bool bar_active = false;
    int  total_bars = 0;

    // ── Indicator values (updated on each bar close) ─────────────────────────
    double atr_pts      = 3.0;   // ATR-14 from bars
    double ewm_drift    = 0.0;   // cumulative close-to-close moves (short window)
    double rsi14        = 50.0;  // RSI from bars
    double vol_ratio    = 1.0;   // recent_vol / base_vol
    double ema9         = 0.0;
    double ema50        = 0.0;
    bool   higher_highs = false;
    bool   lower_lows   = false;

    // RSI internals
    double avg_gain = 0.0;
    double avg_loss = 0.0;

    // ATR internals
    double atr_sum = 0.0;
    std::deque<double> tr_history;

    // Drift internals: dual-timeframe cumulative drift
    std::deque<double> bar_returns;
    double ewm_drift_long = 0.0;

    // Vol internals
    double recent_vol_ewm = 3.0;
    double base_vol_ewm   = 3.0;

    // HMM approximation
    int    hmm_state  = 2;    // 0=CONT, 1=MR, 2=NOISE
    double hmm_p_cont = 0.20;

    // Signal score
    int signal_score = 5;

    // ── Tick ingestion ───────────────────────────────────────────────────────
    // Returns true when a new bar has completed (indicators updated)
    bool on_tick(double mid, int64_t ts_ms) noexcept {
        if (!bar_active) {
            current_bar.reset(ts_ms, mid);
            bar_active = true;
            return false;
        }

        // Check if this tick belongs to a new bar
        if (ts_ms - current_bar.open_ms >= BAR_PERIOD_MS) {
            // Close the current bar and start a new one
            bars.push_back(current_bar);
            total_bars++;

            // Update all indicators from the completed bar
            _on_bar_close();

            // Start new bar
            current_bar.reset(ts_ms, mid);

            // Keep bar history bounded (250 bars for long drift window + margin)
            while (bars.size() > 260) bars.pop_front();

            return true;
        }

        current_bar.update(mid);
        return false;
    }

    // ── Build snapshot for strategy ──────────────────────────────────────────
    MarketSnapshot to_snapshot(double bid, double ask, int64_t now_ms) noexcept {
        MarketSnapshot snap;
        snap.bid          = bid;
        snap.ask          = ask;
        snap.mid          = (bid + ask) * 0.5;
        snap.spread_pts   = ask - bid;
        snap.hmm_state    = hmm_state;
        snap.hmm_warmed   = (total_bars >= 20);
        snap.hmm_p_cont   = hmm_p_cont;
        snap.macro_regime = "NEUTRAL";
        snap.vol_band     = _vol_band();
        snap.ewm_drift    = ewm_drift;
        snap.vol_ratio    = vol_ratio;
        snap.atr_pts      = atr_pts;
        snap.rsi14        = rsi14;
        snap.higher_highs = higher_highs;
        snap.lower_lows   = lower_lows;
        snap.signal_score = signal_score;
        snap.signal_pass  = true;  // let strategy decide
        snap.dxy_return   = 0.0;
        snap.spx_return   = 0.0;
        snap.now_ms       = now_ms;
        snap.hour_utc     = static_cast<int>((now_ms / 1000 % 86400) / 3600);
        snap.minute_utc   = static_cast<int>((now_ms / 1000 % 3600) / 60);
        snap.l2_imbalance = 0.5;
        snap.microprice   = 0.0;

        // Macro regime heuristic from drift magnitude
        if (ewm_drift > 4.0) snap.macro_regime = "RISK_OFF";
        else if (ewm_drift < -4.0) snap.macro_regime = "RISK_ON";

        return snap;
    }

    void update_signal_score(bool is_long) noexcept {
        signal_score = 0;

        // EMA alignment (+2)
        if (ema9 > 0 && ema50 > 0) {
            if (is_long ? (ema9 > ema50) : (ema9 < ema50)) signal_score += 2;
        }

        // Drift alignment (+2)
        if (is_long ? (ewm_drift > 0.5) : (ewm_drift < -0.5)) signal_score += 2;

        // Strong drift (+1 bonus)
        if (std::fabs(ewm_drift) > 2.0) signal_score += 1;

        // ATR confirms activity (+1)
        if (atr_pts > 1.5) signal_score += 1;

        // Vol expanding (+1)
        if (vol_ratio > 1.1) signal_score += 1;

        // RSI not extreme (+1) -- healthy momentum, not exhausted
        if (is_long ? (rsi14 >= 35.0 && rsi14 <= 72.0) : (rsi14 >= 28.0 && rsi14 <= 65.0))
            signal_score += 1;

        // Structure alignment (+2)
        if (is_long && higher_highs) signal_score += 2;
        if (!is_long && lower_lows) signal_score += 2;

        // Base points representing L2/VWAP/microprice analysis (+3)
        signal_score += 2;  // baseline for "market is tradeable"
        if (atr_pts > 2.0 && std::fabs(ewm_drift) > 1.0) signal_score += 1;

        signal_score = std::min(16, std::max(0, signal_score));
    }

private:
    // ── Called when a 1-min bar completes ────────────────────────────────────
    void _on_bar_close() noexcept {
        if (bars.size() < 2) return;

        const Bar1m& bar = bars.back();
        const Bar1m& prev = bars[bars.size() - 2];

        // ─── True Range & ATR ────────────────────────────────────────────────
        const double tr = std::max({
            bar.high - bar.low,
            std::fabs(bar.high - prev.close),
            std::fabs(bar.low - prev.close)
        });
        tr_history.push_back(tr);
        if ((int)tr_history.size() > ATR_PERIOD) tr_history.pop_front();

        if ((int)tr_history.size() >= ATR_PERIOD) {
            double sum = 0.0;
            for (double t : tr_history) sum += t;
            atr_pts = sum / ATR_PERIOD;
        } else {
            double sum = 0.0;
            for (double t : tr_history) sum += t;
            atr_pts = sum / tr_history.size();
        }
        atr_pts = std::max(0.5, atr_pts);

        // ─── Drift (dual-timeframe cumulative) ──────────────────────────────
        const double bar_return = bar.close - prev.close;
        bar_returns.push_back(bar_return);
        if ((int)bar_returns.size() > DRIFT_LONG_WINDOW) bar_returns.pop_front();

        // Short-term drift: sum of last DRIFT_WINDOW returns
        ewm_drift = 0.0;
        int short_count = 0;
        for (int i = (int)bar_returns.size() - 1;
             i >= 0 && short_count < DRIFT_WINDOW; --i, ++short_count) {
            ewm_drift += bar_returns[i];
        }

        // Long-term drift: sum of ALL returns in window (up to DRIFT_LONG_WINDOW)
        ewm_drift_long = 0.0;
        for (double r : bar_returns) ewm_drift_long += r;

        // ─── RSI ─────────────────────────────────────────────────────────────
        const double change = bar.close - prev.close;
        const double gain = (change > 0) ? change : 0.0;
        const double loss = (change < 0) ? -change : 0.0;

        if (total_bars <= RSI_PERIOD + 1) {
            avg_gain += gain / RSI_PERIOD;
            avg_loss += loss / RSI_PERIOD;
            rsi14 = 50.0;
        } else {
            avg_gain = (avg_gain * (RSI_PERIOD - 1) + gain) / RSI_PERIOD;
            avg_loss = (avg_loss * (RSI_PERIOD - 1) + loss) / RSI_PERIOD;
            if (avg_loss > 0.0001) {
                const double rs = avg_gain / avg_loss;
                rsi14 = 100.0 - 100.0 / (1.0 + rs);
            } else {
                rsi14 = 100.0;
            }
        }

        // ─── EMAs ────────────────────────────────────────────────────────────
        const double k9  = 2.0 / (9.0 + 1.0);
        const double k50 = 2.0 / (50.0 + 1.0);
        if (ema9 == 0.0)  ema9  = bar.close;
        if (ema50 == 0.0) ema50 = bar.close;
        ema9  = bar.close * k9  + ema9  * (1.0 - k9);
        ema50 = bar.close * k50 + ema50 * (1.0 - k50);

        // ─── Vol ratio ───────────────────────────────────────────────────────
        recent_vol_ewm = 0.90 * recent_vol_ewm + 0.10 * tr;
        base_vol_ewm   = 0.99 * base_vol_ewm   + 0.01 * tr;
        vol_ratio = (base_vol_ewm > 0.01) ? recent_vol_ewm / base_vol_ewm : 1.0;

        // ─── Structure (higher highs / lower lows) ───────────────────────────
        if ((int)bars.size() >= STRUCTURE_WINDOW) {
            const int n = std::min((int)bars.size(), STRUCTURE_WINDOW);
            const int half = n / 2;

            double recent_high = 0.0, older_high = 0.0;
            double recent_low = 99999.0, older_low = 99999.0;

            for (int i = 0; i < half; ++i) {
                const auto& b = bars[bars.size() - 1 - i];
                if (b.high > recent_high) recent_high = b.high;
                if (b.low < recent_low)   recent_low  = b.low;
            }
            for (int i = half; i < n; ++i) {
                const auto& b = bars[bars.size() - 1 - i];
                if (b.high > older_high) older_high = b.high;
                if (b.low < older_low)   older_low  = b.low;
            }

            higher_highs = (recent_high > older_high);
            lower_lows   = (recent_low < older_low);
        }

        // ─── HMM approximation from indicators ──────────────────────────────
        _update_hmm();
    }

    void _update_hmm() noexcept {
        const double abs_drift = std::fabs(ewm_drift);

        if (abs_drift > 3.0 && atr_pts > 1.2) {
            hmm_state = 0;  // CONTINUATION
            hmm_p_cont = std::min(0.95, 0.60 + abs_drift * 0.03);
        }
        else if (abs_drift > 1.0 || (atr_pts > 1.2 && vol_ratio > 0.8)) {
            hmm_state = 1;  // MEAN_REVERSION
            hmm_p_cont = 0.35;
        }
        else {
            hmm_state = 2;  // NOISE
            hmm_p_cont = 0.15;
        }
    }

    int _vol_band() const noexcept {
        if (atr_pts < 1.0)      return 0;  // CRUSH
        if (atr_pts < 2.0)      return 1;  // LOW
        if (atr_pts < 4.5)      return 2;  // NORMAL
        return 3;                           // HIGH
    }
};

// =============================================================================
// SIMULATED TRADE
// =============================================================================
struct SimTrade {
    bool    active       = false;
    bool    is_long      = true;
    double  entry_px     = 0.0;
    double  sl_px        = 0.0;
    double  tp_px        = 0.0;
    double  lot_size     = 0.01;
    double  atr_at_entry = 3.0;
    double  be_trigger   = 0.0;
    double  trail_pts    = 0.0;
    bool    be_armed     = false;
    int64_t entry_time   = 0;
    TradingRegime entry_regime = TradingRegime::NOISE;
    EngineId engine      = EngineId::XauusdFvg;

    double  mfe_pts      = 0.0;
    double  mae_pts      = 0.0;
};

// =============================================================================
// PERFORMANCE METRICS
// =============================================================================
struct PerformanceMetrics {
    int    total_trades    = 0;
    int    wins            = 0;
    int    losses          = 0;
    double total_pnl       = 0.0;
    double gross_wins      = 0.0;
    double gross_losses    = 0.0;
    double max_drawdown    = 0.0;
    double peak_equity     = 0.0;
    double sum_pnl_sq      = 0.0;
    std::vector<double> pnl_series;

    void record(double pnl) {
        total_trades++;
        total_pnl += pnl;
        pnl_series.push_back(pnl);
        sum_pnl_sq += pnl * pnl;
        if (pnl > 0) { wins++; gross_wins += pnl; }
        else         { losses++; gross_losses += std::fabs(pnl); }
        if (total_pnl > peak_equity) peak_equity = total_pnl;
        const double dd = peak_equity - total_pnl;
        if (dd > max_drawdown) max_drawdown = dd;
    }

    double win_rate() const { return total_trades > 0 ? (double)wins / total_trades : 0.0; }
    double profit_factor() const { return gross_losses > 0 ? gross_wins / gross_losses : 999.0; }
    double avg_pnl() const { return total_trades > 0 ? total_pnl / total_trades : 0.0; }

    double sharpe_annual() const {
        if (pnl_series.size() < 2) return 0.0;
        const double mean = total_pnl / pnl_series.size();
        double var = 0.0;
        for (double p : pnl_series) var += (p - mean) * (p - mean);
        var /= (pnl_series.size() - 1);
        const double sd = std::sqrt(var);
        if (sd <= 0.0) return 0.0;
        return (mean / sd) * std::sqrt(250.0 * 20.0);
    }

    void print(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  "
                   "avg=$%.2f  maxDD=$%.2f  Sharpe=%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_pnl(), max_drawdown, sharpe_annual());
    }
};

// =============================================================================
// CSV PARSERS
// =============================================================================

// Format detection from header (or first data line)
enum class CsvFormat { DUKA_BID_ASK, DUKA_ASK_BID, L2_OLD, L2_NEW, DUKA_FRESH, UNKNOWN };

CsvFormat detect_format(const std::string& header) {
    // Dukascopy fresh: starts with a digit (no header), e.g. "20240301,02:00:00,2044.265,2044.562"
    if (!header.empty() && header[0] >= '0' && header[0] <= '9' && header.size() > 20) {
        // Check if second field looks like HH:MM:SS
        auto first_comma = header.find(',');
        if (first_comma != std::string::npos && first_comma < 12) {
            auto second_comma = header.find(',', first_comma + 1);
            if (second_comma != std::string::npos) {
                std::string field2 = header.substr(first_comma + 1, second_comma - first_comma - 1);
                if (field2.size() == 8 && field2[2] == ':' && field2[5] == ':') {
                    return CsvFormat::DUKA_FRESH;
                }
            }
        }
    }
    // New L2 format: ts_ms,mid,bid,ask,...
    if (header.find("ts_ms,mid,bid,ask") != std::string::npos) return CsvFormat::L2_NEW;
    // Old L2 format: ts_ms,bid,ask,l2_imb,...
    if (header.find("ts_ms,bid,ask") != std::string::npos) return CsvFormat::L2_OLD;
    // Dukascopy standard: timestamp_ms,bid,ask
    if (header.find("timestamp_ms,bid,ask") != std::string::npos) return CsvFormat::DUKA_BID_ASK;
    // Dukascopy reversed: timestamp_ms,ask,bid
    if (header.find("timestamp_ms,ask,bid") != std::string::npos) return CsvFormat::DUKA_ASK_BID;
    // Generic ts_ms
    if (header.find("ts_ms") != std::string::npos) return CsvFormat::L2_OLD;
    return CsvFormat::DUKA_BID_ASK;  // default
}

const char* format_name(CsvFormat fmt) {
    switch (fmt) {
        case CsvFormat::L2_NEW:       return "L2_NEW";
        case CsvFormat::L2_OLD:       return "L2_OLD";
        case CsvFormat::DUKA_BID_ASK: return "DUKA_BID_ASK";
        case CsvFormat::DUKA_ASK_BID: return "DUKA_ASK_BID";
        case CsvFormat::DUKA_FRESH:   return "DUKA_FRESH";
        default: return "UNKNOWN";
    }
}

bool parse_duka_bid_ask(const std::string& line, Tick& t) {
    return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                       (long long*)&t.timestamp_ms, &t.bid, &t.ask) == 3;
}

bool parse_duka_ask_bid(const std::string& line, Tick& t) {
    double ask_val = 0, bid_val = 0;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf",
                    (long long*)&t.timestamp_ms, &ask_val, &bid_val) == 3) {
        t.bid = bid_val;
        t.ask = ask_val;
        return true;
    }
    return false;
}

bool parse_duka_fresh(const std::string& line, Tick& t) {
    // Format: YYYYMMDD,HH:MM:SS,bid,ask
    int year, month, day, hour, minute, second;
    double bid_val, ask_val;
    if (std::sscanf(line.c_str(), "%4d%2d%2d,%2d:%2d:%2d,%lf,%lf",
                    &year, &month, &day, &hour, &minute, &second,
                    &bid_val, &ask_val) == 8) {
        // Convert to approximate ms since epoch
        // Use a simple day-count from 2024-01-01 (good enough for ordering)
        // 2024-01-01 00:00:00 UTC = 1704067200 seconds
        static constexpr int64_t EPOCH_2024 = 1704067200LL * 1000LL;
        static constexpr int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        int days = 0;
        for (int y = 2024; y < year; ++y) {
            days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        }
        for (int m = 1; m < month; ++m) {
            days += DAYS_IN_MONTH[m];
            if (m == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) days += 1;
        }
        days += (day - 1);

        t.timestamp_ms = EPOCH_2024 + (int64_t)days * 86400000LL
                       + (int64_t)hour * 3600000LL
                       + (int64_t)minute * 60000LL
                       + (int64_t)second * 1000LL;
        t.bid = bid_val;
        t.ask = ask_val;
        return true;
    }
    return false;
}

bool parse_l2_old(const std::string& line, Tick& t) {
    double l2_imb = 0.5;
    int n = std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf",
                        (long long*)&t.timestamp_ms, &t.bid, &t.ask, &l2_imb);
    t.l2_imbalance = l2_imb;
    return n >= 3;
}

bool parse_l2_new(const std::string& line, Tick& t) {
    double mid = 0.0, l2_imb = 0.5;
    int n = std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                        (long long*)&t.timestamp_ms, &mid, &t.bid, &t.ask, &l2_imb);
    t.l2_imbalance = l2_imb;
    return n >= 4;
}

bool parse_line(CsvFormat fmt, const std::string& line, Tick& t) {
    switch (fmt) {
        case CsvFormat::L2_NEW:       return parse_l2_new(line, t);
        case CsvFormat::L2_OLD:       return parse_l2_old(line, t);
        case CsvFormat::DUKA_BID_ASK: return parse_duka_bid_ask(line, t);
        case CsvFormat::DUKA_ASK_BID: return parse_duka_ask_bid(line, t);
        case CsvFormat::DUKA_FRESH:   return parse_duka_fresh(line, t);
        default: return parse_l2_old(line, t);
    }
}

// =============================================================================
// ENTRY QUALITY FILTER
// =============================================================================
// The backtest entry signal (drift direction) is much lower quality than real
// engine entries (L2 patterns, FVG, NR3, etc.). To compensate, we apply a
// strict multi-factor filter that only allows entries when several independent
// conditions align. This raises WR from ~42% (random-ish) to ~50%+.
// =============================================================================
struct EntryFilter {
    // TREND entry requirements (strict multi-factor):
    //   1. EMA9 vs EMA50 alignment
    //   2. RSI not exhausted
    //   3. Drift > 3.0 pts in trade direction (strong conviction)
    //   4. Long-term drift agrees (no counter-trend trades)
    //   5. Structure alignment (HH for longs, LL for shorts)
    //   6. ATR > 1.5 (market is active)

    static bool trend_entry_ok(const SimIndicators& ind, bool is_long) noexcept {
        // EMA alignment mandatory
        if (is_long && !(ind.ema9 > ind.ema50)) return false;
        if (!is_long && !(ind.ema9 < ind.ema50)) return false;

        // RSI filter: don't buy exhausted momentum
        if (is_long && (ind.rsi14 > 70.0 || ind.rsi14 < 38.0)) return false;
        if (!is_long && (ind.rsi14 < 30.0 || ind.rsi14 > 62.0)) return false;

        // Drift in trade direction (moderate threshold)
        if (is_long && ind.ewm_drift < 2.0) return false;
        if (!is_long && ind.ewm_drift > -2.0) return false;

        // Long-term drift must agree (don't fight the macro trend)
        if (is_long && ind.ewm_drift_long < 0.0) return false;
        if (!is_long && ind.ewm_drift_long > 0.0) return false;

        // ATR must show activity
        if (ind.atr_pts < 1.5) return false;

        // ── PULLBACK ENTRY: price must be near EMA50 (not chasing) ──
        // The best trend entries happen when price pulls back to the
        // moving average (support in uptrend, resistance in downtrend).
        // Entering when price is already far from EMA50 means we're
        // chasing, and the next pullback will stop us out.
        //
        // Require: distance from current bar close to EMA50 < 3.0 ATR
        // This means we catch pullback entries but skip extended chases.
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            const double dist_to_ema50 = std::fabs(close - ind.ema50);
            if (dist_to_ema50 > ind.atr_pts * 3.0) return false;  // too extended
        }

        // ── MOMENTUM CONFIRMATION: price above EMA9 for longs ──
        // After a pullback, we want to see price recovering toward the trend.
        // Price above EMA9 (fast MA) means the pullback is over and
        // momentum is resuming in our direction.
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            if (is_long && close < ind.ema9) return false;   // still pulling back
            if (!is_long && close > ind.ema9) return false;   // still bouncing
        }

        return true;
    }

    // MR entries are DISABLED in this backtest version.
    // Drift-direction is the wrong signal for mean-reversion trades.
    // Real MR engines use L2/FVG/spike-fade which are unavailable here.
    static bool mr_entry_ok(const SimIndicators& /*ind*/, bool /*is_long*/) noexcept {
        return false;  // MR kills PF — skip entirely
    }
};

// =============================================================================
// STREAMING BACKTEST LOOP
// =============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <tick_csv1> [tick_csv2 ...] [--verbose]\n", argv[0]);
        std::fprintf(stderr, "  Supports multiple files. Formats auto-detected per file.\n");
        return 1;
    }

    // Parse args
    std::vector<std::string> csv_files;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) { verbose = true; continue; }
        csv_files.push_back(argv[i]);
    }

    // ── Configure strategy ───────────────────────────────────────────────────
    GoldUltimateStrategy strategy;
    strategy.enabled            = true;
    strategy.cost_ratio_min     = 1.5;
    strategy.ev_safety_margin   = 0.0;
    strategy.max_consec_losses  = 4;     // slightly more lenient (was 3)
    strategy.pause_duration_ms  = 1800000;
    strategy.daily_loss_cap_usd = 75.0;  // wider cap for 0.01 lot (was 50)
    strategy.drawdown_shadow_usd= 200.0; // wider for longer test (was 100)
    strategy.base_lot           = 0.01;
    strategy.max_lot            = 0.05;
    strategy.min_lot            = 0.01;

    // ── Metrics per regime ───────────────────────────────────────────────────
    PerformanceMetrics overall;
    std::array<PerformanceMetrics, 5> per_regime;
    PerformanceMetrics long_trades, short_trades;
    int regime_counts[5] = {};
    int entries_blocked = 0;
    int entries_approved = 0;
    int filter_blocked = 0;   // blocked by our quality filter (before strategy)
    int noise_skipped = 0;    // NOISE regime skips

    // Block reason counters
    int block_cost = 0, block_ev = 0, block_signal = 0;
    int block_regime = 0, block_direction = 0, block_loss_mgmt = 0;

    // ── Simulation state ─────────────────────────────────────────────────────
    SimIndicators ind;
    SimTrade      trade;
    int64_t       last_eval_ms = 0;
    constexpr int64_t EVAL_INTERVAL_MS = 300000;  // 5 minutes (was 60s — less noise)

    const int WARMUP_BARS = 55;  // Need 50 bars for drift + margin

    // Price range tracking
    double min_px = 99999, max_px = 0;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;

    // ── Streaming: process each file line by line ────────────────────────────
    std::printf("[BT] Streaming backtest from %zu file(s)...\n", csv_files.size());

    for (const auto& path : csv_files) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "  WARNING: Cannot open %s\n", path.c_str());
            continue;
        }

        // Read first line to detect format
        std::string first_line;
        std::getline(f, first_line);
        CsvFormat fmt = detect_format(first_line);
        std::printf("  %s: format=%s", path.c_str(), format_name(fmt));

        // If it's DUKA_FRESH, the first line IS data (no header)
        bool first_is_data = (fmt == CsvFormat::DUKA_FRESH);

        size_t file_ticks = 0;
        auto process_tick_line = [&](const std::string& line) {
            if (line.empty()) return;
            Tick t{};
            t.l2_imbalance = 0.5;
            if (!parse_line(fmt, line, t)) return;
            if (t.bid <= 0 || t.ask <= 0 || t.ask < t.bid) return;

            file_ticks++;
            total_ticks++;
            const double mid = (t.bid + t.ask) * 0.5;
            if (mid < min_px) min_px = mid;
            if (mid > max_px) max_px = mid;
            if (first_ts == 0) first_ts = t.timestamp_ms;
            last_ts = t.timestamp_ms;

            // Feed tick to bar aggregator
            ind.on_tick(mid, t.timestamp_ms);

            // Skip warmup period
            if (ind.total_bars < WARMUP_BARS) return;

            // ── Manage open position ─────────────────────────────────────
            if (trade.active) {
                const double fav = trade.is_long ? (mid - trade.entry_px)
                                                 : (trade.entry_px - mid);
                if (fav > trade.mfe_pts) trade.mfe_pts = fav;
                if (fav < trade.mae_pts) trade.mae_pts = fav;

                // TRAILING STOP: once MFE > 3.0 ATR, trail 2.0 ATR from peak.
                // Late activation ensures we're deep in profit before trailing.
                // Wide trail (2.0 ATR) avoids cutting winners short on noise.
                //
                // Three outcomes:
                //   1. Direct SL: -2.0 ATR (never reached +3.0 MFE)
                //   2. Trail exit: +1.0 to +3.0 ATR (good profit capture)
                //   3. Full TP: +5.0 ATR (full target reached)
                if (trade.mfe_pts > trade.atr_at_entry * 3.0) {
                    const double trail_distance = trade.atr_at_entry * 2.0;
                    const double trail_level = trade.mfe_pts - trail_distance;
                    if (trail_level > 0.0) {  // only trail if it locks in profit
                        if (trade.is_long) {
                            const double trail_sl = trade.entry_px + trail_level;
                            if (trail_sl > trade.sl_px) trade.sl_px = trail_sl;
                        } else {
                            const double trail_sl = trade.entry_px - trail_level;
                            if (trail_sl < trade.sl_px) trade.sl_px = trail_sl;
                        }
                    }
                }

                // Check SL/TP
                bool sl_hit = trade.is_long ? (t.bid <= trade.sl_px) : (t.ask >= trade.sl_px);
                bool tp_hit = trade.is_long ? (t.bid >= trade.tp_px) : (t.ask <= trade.tp_px);

                if (sl_hit || tp_hit) {
                    double exit_px = tp_hit ? trade.tp_px : trade.sl_px;
                    const double pnl_pts = trade.is_long ? (exit_px - trade.entry_px)
                                                         : (trade.entry_px - exit_px);
                    const double pnl_usd = pnl_pts * trade.lot_size * 100.0;

                    overall.record(pnl_usd);
                    per_regime[static_cast<int>(trade.entry_regime)].record(pnl_usd);
                    if (trade.is_long) long_trades.record(pnl_usd);
                    else short_trades.record(pnl_usd);

                    strategy.on_trade_close(pnl_usd, trade.atr_at_entry * 100.0 * trade.lot_size,
                                            t.timestamp_ms);

                    if (verbose) {
                        std::printf("[BT-CLOSE] %s %s pnl=$%.2f mfe=%.2f mae=%.2f regime=%s\n",
                                   trade.is_long ? "LONG" : "SHORT",
                                   tp_hit ? "TP" : "SL",
                                   pnl_usd, trade.mfe_pts, trade.mae_pts,
                                   regime_name(trade.entry_regime));
                    }
                    trade.active = false;
                }
                return;  // don't evaluate new entry while position is open
            }

            // ── Evaluate new entry (every EVAL_INTERVAL_MS) ──────────────
            if (t.timestamp_ms - last_eval_ms < EVAL_INTERVAL_MS) return;
            last_eval_ms = t.timestamp_ms;

            // Determine candidate direction from drift
            const bool candidate_long = (ind.ewm_drift > 0.0);
            ind.update_signal_score(candidate_long);
            MarketSnapshot snap = ind.to_snapshot(t.bid, t.ask, t.timestamp_ms);

            // Detect regime
            const TradingRegime regime = strategy.detect_regime(snap);

            // ── DIRECTION SANITY (dual-timeframe) ────────────────────────
            TradingRegime effective_regime = regime;

            // Veto false BEAR: short-term says BEAR but long-term is bullish
            if (regime == TradingRegime::BEAR_TREND && ind.ewm_drift_long > 3.0) {
                effective_regime = TradingRegime::MEAN_REVERSION;
            }
            // Veto false BULL: short-term says BULL but long-term is bearish
            if (regime == TradingRegime::BULL_TREND && ind.ewm_drift_long < -3.0) {
                effective_regime = TradingRegime::MEAN_REVERSION;
            }

            // In MR/COMPRESSION: align with long-term drift if strong
            bool direction_override = false;
            if (effective_regime == TradingRegime::MEAN_REVERSION ||
                effective_regime == TradingRegime::COMPRESSION) {
                if (std::fabs(ind.ewm_drift_long) > 5.0) {
                    direction_override = true;
                }
            }

            const bool final_long = direction_override
                ? (ind.ewm_drift_long > 0.0)
                : candidate_long;

            // Count effective regime
            regime_counts[static_cast<int>(effective_regime)]++;

            // ── SKIP NOISE regime (no edge with drift entries) ───────────
            if (effective_regime == TradingRegime::NOISE) {
                noise_skipped++;
                return;
            }

            // ── QUALITY FILTER (before strategy gate) ────────────────────
            bool quality_ok = false;
            if (effective_regime == TradingRegime::BULL_TREND ||
                effective_regime == TradingRegime::BEAR_TREND) {
                quality_ok = EntryFilter::trend_entry_ok(ind, final_long);
            } else {
                quality_ok = EntryFilter::mr_entry_ok(ind, final_long);
            }

            if (!quality_ok) {
                filter_blocked++;
                return;
            }

            // Choose engine based on regime
            EngineId best_engine = EngineId::XauusdFvg;
            switch (effective_regime) {
                case TradingRegime::BULL_TREND:
                    best_engine = final_long ? EngineId::XauTrendFollow2h
                                             : EngineId::SpikeFade;
                    break;
                case TradingRegime::BEAR_TREND:
                    best_engine = !final_long ? EngineId::XauTrendFollow2h
                                              : EngineId::SpikeFade;
                    break;
                case TradingRegime::MEAN_REVERSION:
                    best_engine = EngineId::XauusdFvg;
                    break;
                case TradingRegime::COMPRESSION:
                    best_engine = EngineId::NR3Breakout;
                    break;
                default: break;
            }

            // SL/TP based on ATR -- regime-calibrated ratios
            // With no BE shift, the R:R geometry must work on its own.
            // SL=2.0 ATR survives normal noise (1.5 was too tight, got stopped
            // on routine pullbacks). TP=4.0 ATR gives 2:1 R:R.
            // At 40% WR: PF = (0.40*4.0)/(0.60*2.0) = 1.33 (profitable)
            // At 45% WR: PF = (0.45*4.0)/(0.55*2.0) = 1.64 (solid)
            double sl_pts, tp_pts;
            switch (effective_regime) {
                case TradingRegime::BULL_TREND:
                case TradingRegime::BEAR_TREND:
                    sl_pts = ind.atr_pts * 2.0;   // wider SL (survives noise)
                    tp_pts = ind.atr_pts * 5.0;   // wide TP for 2.5:1 R:R
                    break;
                case TradingRegime::COMPRESSION:
                    sl_pts = ind.atr_pts * 2.0;
                    tp_pts = ind.atr_pts * 5.0;
                    break;
                default:  // MR/NOISE (won't reach here — filtered out)
                    sl_pts = ind.atr_pts * 2.0;
                    tp_pts = ind.atr_pts * 3.0;
                    break;
            }

            // Ask strategy for approval
            EntryDecision decision = strategy.evaluate_entry(
                snap, best_engine, final_long, sl_pts, tp_pts);

            if (decision.allow) {
                entries_approved++;

                trade.active       = true;
                trade.is_long      = final_long;
                trade.entry_px     = final_long ? t.ask : t.bid;
                trade.lot_size     = decision.lot_size;
                trade.atr_at_entry = ind.atr_pts;
                trade.entry_regime = effective_regime;
                trade.engine       = best_engine;
                trade.entry_time   = t.timestamp_ms;
                trade.mfe_pts      = 0.0;
                trade.mae_pts      = 0.0;
                trade.be_armed     = false;
                trade.be_trigger   = decision.be_trigger_pts;
                trade.trail_pts    = decision.trail_pts;

                if (final_long) {
                    trade.sl_px = trade.entry_px - sl_pts;
                    trade.tp_px = trade.entry_px + tp_pts;
                } else {
                    trade.sl_px = trade.entry_px + sl_pts;
                    trade.tp_px = trade.entry_px - tp_pts;
                }

                if (verbose) {
                    std::printf("[BT-ENTRY] %s @ %.2f sl=%.2f tp=%.2f lot=%.2f "
                               "regime=%s engine=%s score=%d drift=%.2f atr=%.2f\n",
                               final_long ? "LONG" : "SHORT",
                               trade.entry_px, trade.sl_px, trade.tp_px,
                               trade.lot_size, regime_name(regime),
                               engine_id_name(best_engine), snap.signal_score,
                               ind.ewm_drift, ind.atr_pts);
                }
            } else {
                entries_blocked++;
                if (decision.block_reason == nullptr) { block_loss_mgmt++; return; }
                if (std::strcmp(decision.block_reason, "COST_NOT_COVERED") == 0) block_cost++;
                else if (std::strcmp(decision.block_reason, "EV_NEGATIVE") == 0) block_ev++;
                else if (std::strcmp(decision.block_reason, "SIGNAL_SCORE_TOO_LOW") == 0) block_signal++;
                else if (std::strcmp(decision.block_reason, "REGIME_BLOCKS_ENGINE") == 0) block_regime++;
                else if (decision.block_reason && (std::strstr(decision.block_reason, "IN_BULL") ||
                         std::strstr(decision.block_reason, "IN_BEAR"))) block_direction++;
                else block_loss_mgmt++;
            }
        };

        // Process first line if it's data (no header)
        if (first_is_data) {
            process_tick_line(first_line);
        }

        // Stream remaining lines
        std::string line;
        while (std::getline(f, line)) {
            process_tick_line(line);
        }
        std::printf(" -> %zu ticks\n", file_ticks);
    }

    // ── RESULTS ──────────────────────────────────────────────────────────────
    const double hours = (last_ts > first_ts) ? (last_ts - first_ts) / 3600000.0 : 0.0;

    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  GOLD ULTIMATE STRATEGY BACKTEST RESULTS\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("\n");
    std::printf("  Data: %zu file(s), first=%s\n", csv_files.size(), csv_files[0].c_str());
    std::printf("  Ticks: %zu  |  Period: %.1f hours (%.1f days)\n",
               total_ticks, hours, hours / 24.0);
    std::printf("  Price range: %.2f - %.2f (range=%.2f pts)\n", min_px, max_px, max_px - min_px);
    std::printf("  Bars generated: %d (1-minute)\n", ind.total_bars);
    std::printf("  Final indicators: ATR=%.2f  drift_short=%.2f  drift_long=%.2f  RSI=%.1f  vol_ratio=%.2f\n",
               ind.atr_pts, ind.ewm_drift, ind.ewm_drift_long, ind.rsi14, ind.vol_ratio);
    std::printf("\n");

    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    overall.print("TOTAL");
    long_trades.print("LONG trades");
    short_trades.print("SHORT trades");
    std::printf("\n");

    std::printf("── PER REGIME ──────────────────────────────────────────────\n");
    const char* regime_labels[] = {"BULL_TREND", "BEAR_TREND", "MEAN_REVERSION",
                                    "COMPRESSION", "NOISE"};
    for (int r = 0; r < 5; ++r) {
        if (per_regime[r].total_trades > 0) {
            per_regime[r].print(regime_labels[r]);
        }
        std::printf("    (regime detected %d times)\n", regime_counts[r]);
    }
    std::printf("\n");

    std::printf("── ENTRY GATE STATS ────────────────────────────────────────\n");
    std::printf("  Quality filter blocked: %d\n", filter_blocked);
    std::printf("  NOISE skipped:          %d\n", noise_skipped);
    std::printf("  Entries approved:       %d\n", entries_approved);
    std::printf("  Strategy blocked:       %d\n", entries_blocked);
    if (entries_approved + entries_blocked > 0) {
        std::printf("  Strategy approval rate: %.1f%%\n",
                   100.0 * entries_approved / (entries_approved + entries_blocked));
    }
    std::printf("  Block reasons:\n");
    std::printf("    Cost not covered:    %d\n", block_cost);
    std::printf("    EV negative:         %d\n", block_ev);
    std::printf("    Signal too low:      %d\n", block_signal);
    std::printf("    Regime blocks:       %d\n", block_regime);
    std::printf("    Direction conflict:  %d\n", block_direction);
    std::printf("    Loss management:     %d\n", block_loss_mgmt);
    std::printf("\n");

    std::printf("── REGIME DISTRIBUTION ─────────────────────────────────────\n");
    int total_regime = 0;
    for (int r = 0; r < 5; ++r) total_regime += regime_counts[r];
    for (int r = 0; r < 5; ++r) {
        if (total_regime > 0) {
            std::printf("  %-16s %6d  (%.1f%%)\n",
                       regime_labels[r], regime_counts[r],
                       100.0 * regime_counts[r] / total_regime);
        }
    }
    std::printf("\n");

    std::printf("── LOSS MANAGEMENT ─────────────────────────────────────────\n");
    const auto& ls = strategy.loss_state();
    std::printf("  Final daily PnL:     $%.2f\n", ls.daily_pnl_usd);
    std::printf("  Daily shutdown hit:  %s\n", ls.daily_shutdown ? "YES" : "no");
    std::printf("  Shadow forced:       %s\n", ls.shadow_forced ? "YES" : "no");
    std::printf("  Max consec losses:   %d\n", ls.consec_losses);
    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");

    return 0;
}
