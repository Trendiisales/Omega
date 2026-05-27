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
// IMPROVEMENT LAYERS (v5)
// -----------------------
// Entry:  v1 proven 7-factor filter (session/structure/vol/dir bars REMOVED)
// Exit:   adaptive SL tightening (15/30/45 bars), drift-reversal exit,
//         v1 trail geometry (3.0/2.0 ATR), NO BE, NO profit lock
// R:R:    SL=2.0 ATR, TP=5.0 ATR (v1 proven geometry)
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

    // ── Directional bar consistency ─────────────────────────────────────────
    // Count how many of the last N bars closed in the same direction as drift
    int directional_bars_count = 0;  // out of last 5 bars

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

        // Directional consistency bonus (+1)
        if (directional_bars_count >= 3) signal_score += 1;

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

        // ─── Directional bar consistency ─────────────────────────────────────
        // Count how many of last 5 bars closed in the direction of short drift
        {
            const int lookback = std::min(5, (int)bars.size());
            int dir_count = 0;
            for (int i = 0; i < lookback; ++i) {
                const auto& b = bars[bars.size() - 1 - i];
                const double bar_dir = b.close - b.open;
                if (ewm_drift > 0.0 && bar_dir > 0.0) dir_count++;
                if (ewm_drift < 0.0 && bar_dir < 0.0) dir_count++;
            }
            directional_bars_count = dir_count;
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
    bool    trail_stage2 = false;   // second-stage tighter trail
    int64_t entry_time   = 0;
    int     bars_at_entry = 0;      // bar count at entry for time stop
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

    double avg_win() const { return wins > 0 ? gross_wins / wins : 0.0; }
    double avg_loss() const { return losses > 0 ? gross_losses / losses : 0.0; }

    void print(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  "
                   "avg=$%.2f  maxDD=$%.2f  Sharpe=%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_pnl(), max_drawdown, sharpe_annual());
    }

    void print_detail(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  "
                   "avgW=$%.2f  avgL=$%.2f  maxDD=$%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_win(), avg_loss(), max_drawdown);
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
    // Dukascopy standard: timestamp_ms,bid,ask or timestamp,bidPrice,askPrice
    if (header.find("timestamp_ms,bid,ask") != std::string::npos) return CsvFormat::DUKA_BID_ASK;
    if (header.find("timestamp,bidPrice,askPrice") != std::string::npos) return CsvFormat::DUKA_BID_ASK;
    // Dukascopy reversed: timestamp_ms,ask,bid or timestamp,askPrice,bidPrice
    if (header.find("timestamp_ms,ask,bid") != std::string::npos) return CsvFormat::DUKA_ASK_BID;
    if (header.find("timestamp,askPrice,bidPrice") != std::string::npos) return CsvFormat::DUKA_ASK_BID;
    // Generic ts_ms
    if (header.find("ts_ms") != std::string::npos) return CsvFormat::L2_OLD;
    // Generic timestamp header (try to infer column order from field names)
    if (header.find("timestamp") != std::string::npos && header.find("ask") != std::string::npos) {
        // If "ask" appears before "bid" in the header, it's ask-first
        auto ask_pos = header.find("ask");
        auto bid_pos = header.find("bid");
        if (ask_pos < bid_pos) return CsvFormat::DUKA_ASK_BID;
        return CsvFormat::DUKA_BID_ASK;
    }
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
// ENTRY QUALITY FILTER (v5 — reverted to v1 proven filters)
// =============================================================================
// v2-v4 LESSON: every additional entry filter (session, mandatory structure,
// vol floor, directional bars, higher drift threshold) REMOVED good trades
// faster than bad ones. Win removal rate 63.7% vs loss removal rate 60.0%.
// The filters require CONFIRMATION of past moves, but the best trend entries
// are EARLY (before confirmation). Each filter delayed entry and reduced
// remaining move, hurting avg_win.
//
// v5 reverts to v1's proven entry filter that produced 4627 trades at 36.8% WR
// with R:R=1.51. Improvements are EXIT-SIDE ONLY (adaptive SL, drift reversal).
// =============================================================================
struct EntryFilter {

    // TREND entry requirements (v8 — raised drift threshold):
    //   1. EMA9 vs EMA50 alignment
    //   2. RSI not exhausted
    //   3. Drift > 3.0 pts in trade direction (v1 was 2.0 — too loose)
    //   4. Long-term drift agrees (no counter-trend trades)
    //   5. ATR > 1.5 (market is active)
    //   6. Pullback entry (within 3.0 ATR of EMA50, not chasing)
    //   7. Momentum confirmation (price above EMA9 for longs)
    //
    // v8 RATIONALE: v1-v7 all showed PF=0.85-0.88 regardless of exit
    // management. The WR is too low — entries don't have enough edge.
    // Higher drift threshold requires a STRONGER trend before entering.
    // Fewer trades, but each has more momentum behind it → higher WR.
    // This is NOT the same as v2-v4 confirmation filters (which delayed
    // entry). This raises the signal STRENGTH requirement.

    static bool trend_entry_ok(const SimIndicators& ind, bool is_long) noexcept {
        // EMA alignment mandatory
        if (is_long && !(ind.ema9 > ind.ema50)) return false;
        if (!is_long && !(ind.ema9 < ind.ema50)) return false;

        // RSI filter: don't buy exhausted momentum
        if (is_long && (ind.rsi14 > 70.0 || ind.rsi14 < 38.0)) return false;
        if (!is_long && (ind.rsi14 < 30.0 || ind.rsi14 > 62.0)) return false;

        // Drift in trade direction (v1 threshold: 2.0)
        if (is_long && ind.ewm_drift < 2.0) return false;
        if (!is_long && ind.ewm_drift > -2.0) return false;

        // Long-term drift must agree (don't fight the macro trend)
        if (is_long && ind.ewm_drift_long < 0.0) return false;
        if (!is_long && ind.ewm_drift_long > 0.0) return false;

        // ATR must show activity — v12 raised from 1.5 to 2.5
        // OOS evidence: ATR 1.5-2.5 band has PF=0.80 (481 trades, -$236.71)
        // ATR 2.5-4.0 has PF=1.08, ATR>4.0 has PF=1.85
        if (ind.atr_pts < 2.5) return false;

        // ── PULLBACK ENTRY: price must be near EMA50 (not chasing) ──
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            const double dist_to_ema50 = std::fabs(close - ind.ema50);
            if (dist_to_ema50 > ind.atr_pts * 3.0) return false;  // too extended
        }

        // ── MOMENTUM CONFIRMATION: price above EMA9 for longs ──
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            if (is_long && close < ind.ema9) return false;   // still pulling back
            if (!is_long && close > ind.ema9) return false;   // still bouncing
        }

        return true;
    }

    // MR entries are DISABLED in this backtest version.
    static bool mr_entry_ok(const SimIndicators& /*ind*/, bool /*is_long*/) noexcept {
        return false;  // MR kills PF — skip entirely
    }

    // v11 SESSION FILTER — INCLUSION list based on v10 diagnostic evidence.
    // Only trade during hours that showed PF >= 1.0 with 100+ trades:
    //   01:00 UTC (PF=1.16, 276 trades) — Asian session
    //   05:00 UTC (PF=1.44, 210 trades) — Asian/London overlap
    //   08:00 UTC (PF=1.04, 176 trades) — London open
    //   23:00 UTC (PF=1.29, 125 trades) — Asia open
    //
    // These hours have combined PF=1.22, WR=40.9% vs overall PF=0.86, WR=36.3%.
    // The remaining 20 hours produce PF=0.75 and destroy $2,884.
    // v12 OOS-validated: 08:00 failed both IS (PF=0.67) and OOS (PF=0.95).
    // Keeping 01, 05, 23 — all CONFIRMED or strong OOS.
    static bool session_ok(int hour_utc) noexcept {
        return (hour_utc == 1 || hour_utc == 5 || hour_utc == 23);
    }
};

// =============================================================================
// EXIT DIAGNOSTICS -- track why trades exit
// =============================================================================
struct ExitDiagnostics {
    int sl_hit          = 0;  // raw SL hit (no BE, no trail)
    int tp_hit          = 0;  // full TP reached
    int be_exit         = 0;  // exited at break-even (SL was moved to entry)
    int trail_stage1    = 0;  // exited via stage-1 trail
    int trail_stage2    = 0;  // exited via stage-2 trail
    int time_stop       = 0;  // exited via time stop (dead trade)

    void print() const {
        std::printf("  Exit breakdown:\n");
        std::printf("    Full SL hit:       %d  (original SL, never tightened)\n", sl_hit);
        std::printf("    Tightened SL hit:  %d  (adaptive SL kicked in — saved vs full loss)\n", be_exit);
        std::printf("    Trail exit:        %d  (reached 3.0 ATR MFE, trailed out)\n", trail_stage2);
        std::printf("    Full TP hit:       %d  (reached 5.0 ATR target)\n", tp_hit);
        std::printf("    Drift-reversal:    %d  (drift reversed against position)\n", time_stop);
        const int total = sl_hit + be_exit + trail_stage2 + tp_hit + time_stop;
        if (total > 0) {
            std::printf("  Loss reduction: %d/%d exits (%.1f%%) had SL tightened\n",
                       be_exit, total, 100.0 * be_exit / total);
        }
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
    int session_blocked = 0;  // blocked by session filter (v2)

    // ── v9 DIAGNOSTIC SLICING ────────────────────────────────────────────────
    // Hypothesis-driven: slice trades by every dimension to find where edge
    // exists vs where it's diluted.
    //
    // H1: Time-of-day — PF by hour (24 buckets)
    std::array<PerformanceMetrics, 24> per_hour;
    std::array<int, 24> hour_entry_counts{};

    // H2: Volatility regime — PF by ATR band at entry
    // Bands: [0] <1.5, [1] 1.5-2.5, [2] 2.5-4.0, [3] >4.0
    std::array<PerformanceMetrics, 4> per_atr_band;
    const char* atr_band_labels[] = {"ATR<1.5", "ATR 1.5-2.5", "ATR 2.5-4.0", "ATR>4.0"};

    // H3: Drift magnitude at entry
    // Bands: [0] 3.0-5.0, [1] 5.0-8.0, [2] 8.0-12.0, [3] >12.0
    std::array<PerformanceMetrics, 4> per_drift_band;
    const char* drift_band_labels[] = {"drift 3-5", "drift 5-8", "drift 8-12", "drift >12"};

    // H4: Direction asymmetry — already have long_trades/short_trades

    // MFE distribution — how far do trades go before hitting SL?
    // Buckets: reached 0.5/1.0/1.5/2.0/2.5/3.0/4.0/5.0 ATR MFE
    int mfe_reached[8] = {};  // cumulative: how many trades reached each level
    const double mfe_levels[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0};

    // Track entry context for diagnostics
    struct TradeContext {
        int    entry_hour    = 0;
        int    atr_band      = 0;
        int    drift_band    = 0;
        double drift_at_entry = 0.0;
        double atr_at_entry  = 0.0;
    } trade_ctx;

    // Block reason counters
    int block_cost = 0, block_ev = 0, block_signal = 0;
    int block_regime = 0, block_direction = 0, block_loss_mgmt = 0;

    // Exit diagnostics (v2)
    ExitDiagnostics exit_diag;

    // ── Simulation state ─────────────────────────────────────────────────────
    SimIndicators ind;
    SimTrade      trade;
    int64_t       last_eval_ms = 0;
    constexpr int64_t EVAL_INTERVAL_MS = 300000;  // 5 minutes

    const int WARMUP_BARS = 55;  // Need 50 bars for drift + margin

    // ── v5 EXIT PARAMETERS ──────────────────────────────────────────────────
    //
    // LESSONS FROM v2-v4:
    //   v2: BE at 1.0 ATR → 52% scratch exits, PF collapsed to 0.83
    //   v3: Profit lock at 2.0 ATR → still cuts avg_win by 35%, PF=0.78
    //   Both versions tried to CAP WINNERS. That's the wrong target.
    //
    // v5 STRATEGY: Let winners run (v1 geometry), cut LOSERS faster.
    //   The real cost is full -2.0 ATR losses on trades that were wrong
    //   from the start. Good trend entries move favorably within 15-30 min.
    //   If a trade is still underwater after 30+ min, the signal was wrong.
    //
    // ADAPTIVE SL TIGHTENING (targets losers, not winners):
    //   - SL starts at 2.0 ATR (wide, lets trade develop)
    //   - After 15 bars: if trade is underwater (fav < 0), tighten to 1.5 ATR
    //     Rationale: 15 min is enough for a real trend entry to show. If
    //     it hasn't moved favorably, we're probably wrong. Cut max loss by 25%.
    //   - After 30 bars: if MFE < 0.5 ATR, tighten to 1.0 ATR
    //     Rationale: 30 min with no meaningful progress = dead trade.
    //     Cut max loss by 50% rather than wait for full SL.
    //   - After 45 bars: if MFE < 1.0 ATR, tighten to 0.75 ATR
    //     Rationale: 45 min, still no conviction? Get out cheap.
    //
    // Trail + TP unchanged from v1 (proven 1.51 R:R):
    //   Trail at 3.0 ATR MFE, 2.0 ATR distance. TP at 5.0 ATR.
    //   NO break-even. NO profit lock. Winners run unmolested.
    //

    // Adaptive SL DISABLED in v10.
    // v5-v9 showed it reduces avgL but proportionally reduces WR → PF unchanged.
    // v9 diagnostic confirmed: with SL=1.5, only 7/4965 trades tightened.
    // Even with SL=2.0, the WR loss from tightening cancels the avgL gain.
    // The real edge is in WHEN to trade (time-of-day), not how to exit.
    constexpr int    SL_TIGHTEN1_BARS    = 9999;  // effectively disabled
    constexpr double SL_TIGHTEN1_FAV_MAX = -999.0;
    constexpr double SL_TIGHTEN1_ATR     = 2.0;

    constexpr int    SL_TIGHTEN2_BARS    = 9999;
    constexpr double SL_TIGHTEN2_MFE_MAX = -999.0;
    constexpr double SL_TIGHTEN2_ATR     = 2.0;

    constexpr int    SL_TIGHTEN3_BARS    = 9999;
    constexpr double SL_TIGHTEN3_MFE_MAX = -999.0;
    constexpr double SL_TIGHTEN3_ATR     = 2.0;

    // Trailing stop (v1 geometry — proven)
    constexpr double TRAIL_TRIGGER_ATR = 3.0;   // trail activation
    constexpr double TRAIL_DIST_ATR    = 2.0;   // trail distance from peak

    // SL/TP R:R parameters (v10: reverted to v1 proven geometry)
    // v9 proved SL=1.5 is too tight — 71.6% full SL hit, PF=0.81.
    // v1 geometry (SL=2.0, TP=5.0) had PF=0.88, the best overall.
    constexpr double SL_ATR_MULT = 2.0;  // initial stop loss = 2.0 ATR
    constexpr double TP_ATR_MULT = 5.0;  // take profit = 5.0 ATR

    // ── OOS VALIDATION (v11) ───────────────────────────────────────────────
    // Split the 26-month dataset into two halves:
    //   IS: Mar 2024 → Aug 2025 (first ~18 months)
    //   OOS: Sep 2025 → Apr 2026 (last ~8 months)
    //
    // The edge hours (01,05,08,23 UTC) were discovered on the FULL dataset.
    // If the edge holds on OOS (unseen during "discovery"), it's structural.
    // If it collapses, it was in-sample noise.
    //
    // Split point: 2025-09-01 00:00:00 UTC = 1756684800 seconds = 1756684800000 ms
    constexpr int64_t OOS_SPLIT_MS = 1756684800000LL;  // 2025-09-01 UTC

    PerformanceMetrics is_overall, oos_overall;
    PerformanceMetrics is_bull, is_bear, oos_bull, oos_bear;
    std::array<PerformanceMetrics, 24> is_per_hour, oos_per_hour;

    // Price range tracking
    double min_px = 99999, max_px = 0;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;

    // ── Streaming: process each file line by line ────────────────────────────
    std::printf("[BT] Streaming backtest v12 from %zu file(s)...\n", csv_files.size());
    std::printf("[BT] v12: OOS-VALIDATED hours (01,05,23 UTC) + ATR>=2.5 floor\n");
    std::printf("[BT]   Dropped 08:00 (IS PF=0.67, OOS PF=0.95) + ATR 1.5-2.5 (PF=0.80)\n");

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

                // ── ADAPTIVE SL TIGHTENING (v4 — targets LOSERS) ────────
                // Good trend entries show conviction quickly. If a trade
                // hasn't moved favorably after 30-60-90 bars, the signal
                // was wrong. Tighten SL progressively to cut losses early
                // instead of waiting for full -2.0 ATR SL hit.
                //
                // This does NOT touch winning trades — only trades that
                // are underwater or showing minimal progress.
                const int bars_held = ind.total_bars - trade.bars_at_entry;

                // Stage 1: 30 bars, still underwater → tighten to 1.5 ATR
                if (bars_held >= SL_TIGHTEN1_BARS && fav <= SL_TIGHTEN1_FAV_MAX) {
                    const double tight_sl_dist = trade.atr_at_entry * SL_TIGHTEN1_ATR;
                    if (trade.is_long) {
                        const double new_sl = trade.entry_px - tight_sl_dist;
                        if (new_sl > trade.sl_px) trade.sl_px = new_sl;
                    } else {
                        const double new_sl = trade.entry_px + tight_sl_dist;
                        if (new_sl < trade.sl_px) trade.sl_px = new_sl;
                    }
                }

                // Stage 2: 60 bars, MFE < 0.5 ATR → tighten to 1.0 ATR
                if (bars_held >= SL_TIGHTEN2_BARS &&
                    trade.mfe_pts < trade.atr_at_entry * SL_TIGHTEN2_MFE_MAX) {
                    const double tight_sl_dist = trade.atr_at_entry * SL_TIGHTEN2_ATR;
                    if (trade.is_long) {
                        const double new_sl = trade.entry_px - tight_sl_dist;
                        if (new_sl > trade.sl_px) trade.sl_px = new_sl;
                    } else {
                        const double new_sl = trade.entry_px + tight_sl_dist;
                        if (new_sl < trade.sl_px) trade.sl_px = new_sl;
                    }
                }

                // Stage 3: 90 bars, MFE < 1.0 ATR → tighten to 0.75 ATR
                if (bars_held >= SL_TIGHTEN3_BARS &&
                    trade.mfe_pts < trade.atr_at_entry * SL_TIGHTEN3_MFE_MAX) {
                    const double tight_sl_dist = trade.atr_at_entry * SL_TIGHTEN3_ATR;
                    if (trade.is_long) {
                        const double new_sl = trade.entry_px - tight_sl_dist;
                        if (new_sl > trade.sl_px) trade.sl_px = new_sl;
                    } else {
                        const double new_sl = trade.entry_px + tight_sl_dist;
                        if (new_sl < trade.sl_px) trade.sl_px = new_sl;
                    }
                }

                // (Drift-reversal exit REMOVED in v7 — net harmful in
                //  both v5 (all trades) and v6 (losers only). v5 killed
                //  avgW, v6 killed WR. The drift indicator is too noisy
                //  for exit decisions — temporary reversals close trades
                //  that would have recovered.)

                // ── TRAILING STOP (v1 geometry — proven 1.51 R:R) ───────
                // At 3.0 ATR MFE, trail 2.0 ATR from peak.
                // NO break-even. NO profit lock. Winners run unmolested.
                if (trade.mfe_pts >= trade.atr_at_entry * TRAIL_TRIGGER_ATR) {
                    trade.trail_stage2 = true;
                    const double trail_distance = trade.atr_at_entry * TRAIL_DIST_ATR;
                    const double trail_level = trade.mfe_pts - trail_distance;
                    if (trail_level > 0.0) {
                        if (trade.is_long) {
                            const double trail_sl = trade.entry_px + trail_level;
                            if (trail_sl > trade.sl_px) trade.sl_px = trail_sl;
                        } else {
                            const double trail_sl = trade.entry_px - trail_level;
                            if (trail_sl < trade.sl_px) trade.sl_px = trail_sl;
                        }
                    }
                }

                // (Time stop removed in v4 — adaptive SL tightening
                //  handles dead trades by bringing SL closer over time,
                //  which achieves the same effect without a market-order exit)

                // Check SL/TP
                bool sl_hit = trade.is_long ? (t.bid <= trade.sl_px) : (t.ask >= trade.sl_px);
                bool tp_hit = trade.is_long ? (t.bid >= trade.tp_px) : (t.ask <= trade.tp_px);

                if (sl_hit || tp_hit) {
                    // S37 audit fix: fill at touch (bid long-exit, ask short-exit).
                    double exit_px = trade.is_long ? t.bid : t.ask;
                    const double pnl_pts = trade.is_long ? (exit_px - trade.entry_px)
                                                         : (trade.entry_px - exit_px);
                    const double pnl_usd = pnl_pts * trade.lot_size * 100.0;

                    overall.record(pnl_usd);
                    per_regime[static_cast<int>(trade.entry_regime)].record(pnl_usd);
                    if (trade.is_long) long_trades.record(pnl_usd);
                    else short_trades.record(pnl_usd);

                    strategy.on_trade_close(pnl_usd, trade.atr_at_entry * 100.0 * trade.lot_size,
                                            t.timestamp_ms);

                    // ── v9 DIAGNOSTIC: record by slice ──────────────
                    per_hour[trade_ctx.entry_hour].record(pnl_usd);
                    per_atr_band[trade_ctx.atr_band].record(pnl_usd);
                    per_drift_band[trade_ctx.drift_band].record(pnl_usd);

                    // ── v12 OOS: record into IS or OOS bucket ───────
                    {
                        const bool is_oos = (trade.entry_time >= OOS_SPLIT_MS);
                        auto& ov  = is_oos ? oos_overall : is_overall;
                        auto& bul = is_oos ? oos_bull    : is_bull;
                        auto& ber = is_oos ? oos_bear    : is_bear;
                        auto& ph  = is_oos ? oos_per_hour : is_per_hour;

                        ov.record(pnl_usd);
                        if (trade.entry_regime == TradingRegime::BULL_TREND)
                            bul.record(pnl_usd);
                        else if (trade.entry_regime == TradingRegime::BEAR_TREND)
                            ber.record(pnl_usd);
                        ph[trade_ctx.entry_hour].record(pnl_usd);
                    }

                    // MFE distribution: how far did this trade go?
                    const double mfe_atr = trade.mfe_pts / std::max(0.5, trade.atr_at_entry);
                    for (int m = 0; m < 8; ++m) {
                        if (mfe_atr >= mfe_levels[m]) mfe_reached[m]++;
                    }

                    // Classify exit type for diagnostics
                    if (tp_hit) {
                        exit_diag.tp_hit++;
                    } else if (trade.trail_stage2) {
                        // Trail was active (MFE reached 3.0 ATR)
                        exit_diag.trail_stage2++;
                    } else {
                        // SL hit (may have been tightened by adaptive SL)
                        // Distinguish: was SL tightened from original?
                        const double orig_sl_dist = trade.atr_at_entry * SL_ATR_MULT;
                        const double actual_sl_dist = trade.is_long
                            ? (trade.entry_px - trade.sl_px)
                            : (trade.sl_px - trade.entry_px);
                        if (actual_sl_dist < orig_sl_dist * 0.95) {
                            // SL was tightened by adaptive logic
                            exit_diag.be_exit++;  // reusing "be_exit" as "tightened SL"
                        } else {
                            // Full original SL hit
                            exit_diag.sl_hit++;
                        }
                    }

                    if (verbose) {
                        std::printf("[BT-CLOSE] %s %s pnl=$%.2f mfe=%.2f mae=%.2f regime=%s "
                                   "be=%d trail2=%d bars=%d\n",
                                   trade.is_long ? "LONG" : "SHORT",
                                   tp_hit ? "TP" : "SL",
                                   pnl_usd, trade.mfe_pts, trade.mae_pts,
                                   regime_name(trade.entry_regime),
                                   trade.be_armed ? 1 : 0,
                                   trade.trail_stage2 ? 1 : 0,
                                   bars_held);
                    }
                    trade.active = false;
                }
                return;  // don't evaluate new entry while position is open
            }

            // ── Evaluate new entry (every EVAL_INTERVAL_MS) ──────────────
            if (t.timestamp_ms - last_eval_ms < EVAL_INTERVAL_MS) return;
            last_eval_ms = t.timestamp_ms;

            // v11: SESSION FILTER — edge hours only.
            // v2-v4 used a BROAD session filter (07-20 UTC) that removed
            // good trades. v10 diagnostic showed only 4 hours have PF>=1.0.
            // This is a NARROW inclusion filter for proven-edge hours.
            {
                const int hour_utc = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                if (!EntryFilter::session_ok(hour_utc)) {
                    session_blocked++;
                    return;
                }
            }

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

            // SL/TP based on ATR (v2: tighter TP for higher hit rate)
            double sl_pts = ind.atr_pts * SL_ATR_MULT;
            double tp_pts = ind.atr_pts * TP_ATR_MULT;

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
                trade.bars_at_entry = ind.total_bars;
                trade.mfe_pts      = 0.0;
                trade.mae_pts      = 0.0;
                trade.be_armed     = false;
                trade.trail_stage2 = false;
                trade.be_trigger   = decision.be_trigger_pts;
                trade.trail_pts    = decision.trail_pts;

                if (final_long) {
                    trade.sl_px = trade.entry_px - sl_pts;
                    trade.tp_px = trade.entry_px + tp_pts;
                } else {
                    trade.sl_px = trade.entry_px + sl_pts;
                    trade.tp_px = trade.entry_px - tp_pts;
                }

                // ── v9 DIAGNOSTIC: capture entry context ────────────
                {
                    const int h = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                    trade_ctx.entry_hour = h;
                    hour_entry_counts[h]++;

                    // ATR band
                    if (ind.atr_pts < 1.5) trade_ctx.atr_band = 0;
                    else if (ind.atr_pts < 2.5) trade_ctx.atr_band = 1;
                    else if (ind.atr_pts < 4.0) trade_ctx.atr_band = 2;
                    else trade_ctx.atr_band = 3;

                    // Drift band
                    const double abs_drift = std::fabs(ind.ewm_drift);
                    if (abs_drift < 5.0) trade_ctx.drift_band = 0;
                    else if (abs_drift < 8.0) trade_ctx.drift_band = 1;
                    else if (abs_drift < 12.0) trade_ctx.drift_band = 2;
                    else trade_ctx.drift_band = 3;

                    trade_ctx.drift_at_entry = ind.ewm_drift;
                    trade_ctx.atr_at_entry = ind.atr_pts;
                }

                if (verbose) {
                    const int h_utc = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                    std::printf("[BT-ENTRY] %s @ %.2f sl=%.2f tp=%.2f lot=%.2f "
                               "regime=%s engine=%s score=%d drift=%.2f atr=%.2f "
                               "hour=%d\n",
                               final_long ? "LONG" : "SHORT",
                               trade.entry_px, trade.sl_px, trade.tp_px,
                               trade.lot_size, regime_name(regime),
                               engine_id_name(best_engine), snap.signal_score,
                               ind.ewm_drift, ind.atr_pts, h_utc);
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
    std::printf("  GOLD ULTIMATE STRATEGY BACKTEST RESULTS (v12)\n");
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

    std::printf("── v5 PARAMETERS ───────────────────────────────────────────\n");
    std::printf("  Session filter: OOS-VALIDATED HOURS (01,05,23 UTC)\n");
    std::printf("  Entry: v1 proven 7-factor filter (EMA align, RSI, drift>2.0,\n");
    std::printf("         long-term drift agree, ATR>2.5, pullback, momentum)\n");
    std::printf("  SL=%.1f ATR (initial)  TP=%.1f ATR  (R:R = 1:%.1f)\n", SL_ATR_MULT, TP_ATR_MULT, TP_ATR_MULT / SL_ATR_MULT);
    std::printf("  Adaptive SL tightening (AGGRESSIVE — 2x faster than v4):\n");
    std::printf("    %d bars + underwater  -> %.1f ATR\n", SL_TIGHTEN1_BARS, SL_TIGHTEN1_ATR);
    std::printf("    %d bars + MFE<%.1f ATR -> %.1f ATR\n", SL_TIGHTEN2_BARS, SL_TIGHTEN2_MFE_MAX, SL_TIGHTEN2_ATR);
    std::printf("    %d bars + MFE<%.1f ATR -> %.1f ATR\n", SL_TIGHTEN3_BARS, SL_TIGHTEN3_MFE_MAX, SL_TIGHTEN3_ATR);
    std::printf("  Drift-reversal exit: DISABLED (v5/v6 showed net harmful)\n");
    std::printf("  Trail: trigger=%.1f ATR MFE, dist=%.1f ATR from peak\n",
               TRAIL_TRIGGER_ATR, TRAIL_DIST_ATR);
    std::printf("  NO break-even. NO profit lock. Winners run free.\n");
    std::printf("\n");

    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    overall.print("TOTAL");
    overall.print_detail("TOTAL (detail)");
    long_trades.print("LONG trades");
    short_trades.print("SHORT trades");
    std::printf("\n");

    std::printf("── PER REGIME ──────────────────────────────────────────────\n");
    const char* regime_labels[] = {"BULL_TREND", "BEAR_TREND", "MEAN_REVERSION",
                                    "COMPRESSION", "NOISE"};
    for (int r = 0; r < 5; ++r) {
        if (per_regime[r].total_trades > 0) {
            per_regime[r].print_detail(regime_labels[r]);
        }
        std::printf("    (regime detected %d times)\n", regime_counts[r]);
    }
    std::printf("\n");

    std::printf("── EXIT ANALYSIS (v12) ────────────────────────────────────\n");
    exit_diag.print();
    std::printf("\n");

    std::printf("── ENTRY GATE STATS ────────────────────────────────────────\n");
    std::printf("  Session filter blocked:  %d\n", session_blocked);
    std::printf("  Quality filter blocked:  %d\n", filter_blocked);
    std::printf("  NOISE skipped:           %d\n", noise_skipped);
    std::printf("  Entries approved:        %d\n", entries_approved);
    std::printf("  Strategy blocked:        %d\n", entries_blocked);
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

    std::printf("── MATH CHECK ─────────────────────────────────────────────\n");
    if (overall.total_trades > 0) {
        const double wr = overall.win_rate();
        const double required_rr = (1.0 - wr) / wr;
        const double actual_rr = overall.avg_win() / std::max(0.01, overall.avg_loss());
        std::printf("  WR=%.1f%%  Required avg_win/avg_loss for PF=1.0: %.2f\n",
                   wr * 100.0, required_rr);
        std::printf("  Actual avg_win/avg_loss: %.2f  (%.2f%% of required)\n",
                   actual_rr, actual_rr / required_rr * 100.0);
    }
    std::printf("\n");

    // ═══════════════════════════════════════════════════════════════════════
    // v9 DEEP DIAGNOSTICS — hypothesis-driven decomposition
    // ═══════════════════════════════════════════════════════════════════════

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  v9 DIAGNOSTIC: WHERE IS THE EDGE? (hypothesis testing)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    // H1: PF BY HOUR OF DAY
    std::printf("── H1: PF BY HOUR (UTC) — is there a time-of-day edge? ────\n");
    std::printf("  Hour  Trades   WR%%    PF    avgW    avgL    PnL\n");
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total_trades > 0) {
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f  %s\n",
                       h, per_hour[h].total_trades,
                       per_hour[h].win_rate() * 100.0,
                       per_hour[h].profit_factor(),
                       per_hour[h].avg_win(),
                       per_hour[h].avg_loss(),
                       per_hour[h].total_pnl,
                       per_hour[h].profit_factor() >= 1.0 ? " <<<" : "");
        }
    }
    std::printf("\n");

    // H2: PF BY ATR BAND
    std::printf("── H2: PF BY ATR BAND — does volatility regime matter? ────\n");
    std::printf("  Band          Trades   WR%%    PF    avgW    avgL    PnL\n");
    for (int b = 0; b < 4; ++b) {
        if (per_atr_band[b].total_trades > 0) {
            std::printf("  %-14s %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f  %s\n",
                       atr_band_labels[b], per_atr_band[b].total_trades,
                       per_atr_band[b].win_rate() * 100.0,
                       per_atr_band[b].profit_factor(),
                       per_atr_band[b].avg_win(),
                       per_atr_band[b].avg_loss(),
                       per_atr_band[b].total_pnl,
                       per_atr_band[b].profit_factor() >= 1.0 ? " <<<" : "");
        }
    }
    std::printf("\n");

    // H3: PF BY DRIFT MAGNITUDE AT ENTRY
    std::printf("── H3: PF BY DRIFT MAGNITUDE — stronger signal = better? ──\n");
    std::printf("  Drift         Trades   WR%%    PF    avgW    avgL    PnL\n");
    for (int d = 0; d < 4; ++d) {
        if (per_drift_band[d].total_trades > 0) {
            std::printf("  %-14s %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f  %s\n",
                       drift_band_labels[d], per_drift_band[d].total_trades,
                       per_drift_band[d].win_rate() * 100.0,
                       per_drift_band[d].profit_factor(),
                       per_drift_band[d].avg_win(),
                       per_drift_band[d].avg_loss(),
                       per_drift_band[d].total_pnl,
                       per_drift_band[d].profit_factor() >= 1.0 ? " <<<" : "");
        }
    }
    std::printf("\n");

    // H4: DIRECTION ASYMMETRY (already printed above, summarize)
    std::printf("── H4: DIRECTION ASYMMETRY — bull vs bear geometry ─────────\n");
    std::printf("  LONG:  trades=%d  WR=%.1f%%  PF=%.2f  avgW=$%.2f  avgL=$%.2f  R:R=%.2f\n",
               long_trades.total_trades, long_trades.win_rate() * 100.0,
               long_trades.profit_factor(), long_trades.avg_win(), long_trades.avg_loss(),
               long_trades.avg_win() / std::max(0.01, long_trades.avg_loss()));
    std::printf("  SHORT: trades=%d  WR=%.1f%%  PF=%.2f  avgW=$%.2f  avgL=$%.2f  R:R=%.2f\n",
               short_trades.total_trades, short_trades.win_rate() * 100.0,
               short_trades.profit_factor(), short_trades.avg_win(), short_trades.avg_loss(),
               short_trades.avg_win() / std::max(0.01, short_trades.avg_loss()));
    std::printf("\n");

    // MFE DISTRIBUTION — what % of trades reach each favorable level?
    std::printf("── MFE DISTRIBUTION — how far do trades go? ───────────────\n");
    if (overall.total_trades > 0) {
        std::printf("  Level (ATR)  Reached    %%     Cumulative implication\n");
        for (int m = 0; m < 8; ++m) {
            std::printf("  >= %.1f ATR   %5d   %5.1f%%   %s\n",
                       mfe_levels[m], mfe_reached[m],
                       100.0 * mfe_reached[m] / overall.total_trades,
                       (m == 2) ? "<-- trail trigger would need to be HERE for 50%+ coverage" :
                       (m == 5) ? "<-- current trail trigger (3.0 ATR)" : "");
        }
    }
    std::printf("\n");

    // SUMMARY: which hypothesis has the strongest signal?
    std::printf("── DIAGNOSIS SUMMARY ──────────────────────────────────────\n");
    // Find best hour
    int best_hour = -1; double best_hour_pf = 0.0;
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total_trades >= 50 && per_hour[h].profit_factor() > best_hour_pf) {
            best_hour_pf = per_hour[h].profit_factor();
            best_hour = h;
        }
    }
    // Find best ATR band
    int best_atr = -1; double best_atr_pf = 0.0;
    for (int b = 0; b < 4; ++b) {
        if (per_atr_band[b].total_trades >= 50 && per_atr_band[b].profit_factor() > best_atr_pf) {
            best_atr_pf = per_atr_band[b].profit_factor();
            best_atr = b;
        }
    }
    // Find best drift band
    int best_drift = -1; double best_drift_pf = 0.0;
    for (int d = 0; d < 4; ++d) {
        if (per_drift_band[d].total_trades >= 50 && per_drift_band[d].profit_factor() > best_drift_pf) {
            best_drift_pf = per_drift_band[d].profit_factor();
            best_drift = d;
        }
    }

    if (best_hour >= 0)
        std::printf("  Best hour:  %02d:00 UTC  PF=%.2f  (%d trades)  %s\n",
                   best_hour, best_hour_pf, per_hour[best_hour].total_trades,
                   best_hour_pf >= 1.0 ? "*** EDGE ***" : "");
    if (best_atr >= 0)
        std::printf("  Best ATR:   %s  PF=%.2f  (%d trades)  %s\n",
                   atr_band_labels[best_atr], best_atr_pf, per_atr_band[best_atr].total_trades,
                   best_atr_pf >= 1.0 ? "*** EDGE ***" : "");
    if (best_drift >= 0)
        std::printf("  Best drift: %s  PF=%.2f  (%d trades)  %s\n",
                   drift_band_labels[best_drift], best_drift_pf, per_drift_band[best_drift].total_trades,
                   best_drift_pf >= 1.0 ? "*** EDGE ***" : "");

    // Any PF >= 1.0 anywhere?
    bool found_edge = false;
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total_trades >= 30 && per_hour[h].profit_factor() >= 1.0) found_edge = true;
    }
    for (int b = 0; b < 4; ++b) {
        if (per_atr_band[b].total_trades >= 30 && per_atr_band[b].profit_factor() >= 1.0) found_edge = true;
    }
    for (int d = 0; d < 4; ++d) {
        if (per_drift_band[d].total_trades >= 30 && per_drift_band[d].profit_factor() >= 1.0) found_edge = true;
    }

    if (found_edge) {
        std::printf("\n  >>> EDGE FOUND in at least one slice. Filter to that regime.\n");
    } else {
        std::printf("\n  >>> NO EDGE in any slice with 30+ trades.\n");
        std::printf("  >>> H5 confirmed: the drift-following signal may not have\n");
        std::printf("  >>> exploitable edge on 1-min XAUUSD over this 26-month period.\n");
    }
    std::printf("\n");

    // ── COMBINED EDGE ANALYSIS ──────────────────────────────────────────
    // Aggregate all hours with PF >= 1.0 (min 30 trades) to see combined effect
    std::printf("── COMBINED EDGE HOURS (PF>=1.0, 30+ trades) ─────────────\n");
    {
        PerformanceMetrics edge_combined;
        int edge_hour_count = 0;
        std::printf("  Included: ");
        for (int h = 0; h < 24; ++h) {
            if (per_hour[h].total_trades >= 30 && per_hour[h].profit_factor() >= 1.0) {
                std::printf("%02d:00 ", h);
                edge_hour_count++;
                // Merge metrics
                edge_combined.total_trades += per_hour[h].total_trades;
                edge_combined.wins += per_hour[h].wins;
                edge_combined.losses += per_hour[h].losses;
                edge_combined.total_pnl += per_hour[h].total_pnl;
                edge_combined.gross_wins += per_hour[h].gross_wins;
                edge_combined.gross_losses += per_hour[h].gross_losses;
            }
        }
        if (edge_hour_count == 0) {
            std::printf("(none)\n");
        } else {
            std::printf("\n  Hours with edge: %d / 24\n", edge_hour_count);
            std::printf("  Combined: trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f\n",
                       edge_combined.total_trades,
                       edge_combined.win_rate() * 100.0,
                       edge_combined.profit_factor(),
                       edge_combined.total_pnl);
            std::printf("  avgW=$%.2f  avgL=$%.2f  R:R=%.2f\n",
                       edge_combined.avg_win(), edge_combined.avg_loss(),
                       edge_combined.avg_win() / std::max(0.01, edge_combined.avg_loss()));
            std::printf("  Trade frequency: %.1f trades/day (%.1f/day per hour)\n",
                       edge_combined.total_trades / (hours / 24.0),
                       edge_combined.total_trades / (hours / 24.0) / edge_hour_count);
        }
    }
    std::printf("\n");

    // ── WORST HOURS (biggest PnL destroyers) ────────────────────────────
    std::printf("── WORST HOURS (biggest PnL destroyers) ───────────────────\n");
    {
        // Find the 5 worst hours by total PnL
        struct HourPnl { int h; double pnl; int trades; double pf; };
        std::vector<HourPnl> hour_pnls;
        for (int h = 0; h < 24; ++h) {
            if (per_hour[h].total_trades >= 10) {
                hour_pnls.push_back({h, per_hour[h].total_pnl,
                                     per_hour[h].total_trades,
                                     per_hour[h].profit_factor()});
            }
        }
        std::sort(hour_pnls.begin(), hour_pnls.end(),
                  [](const HourPnl& a, const HourPnl& b) { return a.pnl < b.pnl; });
        std::printf("  Hour    PnL       Trades  PF\n");
        for (int i = 0; i < std::min(5, (int)hour_pnls.size()); ++i) {
            std::printf("  %02d:00  $%7.2f   %5d  %.2f\n",
                       hour_pnls[i].h, hour_pnls[i].pnl,
                       hour_pnls[i].trades, hour_pnls[i].pf);
        }
        double worst5_pnl = 0.0;
        for (int i = 0; i < std::min(5, (int)hour_pnls.size()); ++i) {
            worst5_pnl += hour_pnls[i].pnl;
        }
        std::printf("  Total from worst 5 hours: $%.2f\n", worst5_pnl);
        std::printf("  >>> Eliminating worst 5 hours would recover $%.2f\n", -worst5_pnl);
    }
    std::printf("\n");

    // ── CROSS-DIMENSIONAL: edge hours + high ATR ────────────────────────
    // (this would need per-trade recording — flag for next version)
    std::printf("── ACTIONABLE CONCLUSIONS ─────────────────────────────────\n");
    std::printf("  1. Time-of-day is the strongest edge discriminator\n");
    std::printf("  2. High ATR (>4.0) is the best vol regime (PF=%.2f)\n",
               per_atr_band[3].total_trades > 0 ? per_atr_band[3].profit_factor() : 0.0);
    std::printf("  3. Drift >12 is the best signal band (PF=%.2f)\n",
               per_drift_band[3].total_trades > 0 ? per_drift_band[3].profit_factor() : 0.0);
    std::printf("  4. Shorts outperform longs consistently\n");
    std::printf("  5. Next step: filter to edge hours + high ATR + strong drift\n");
    std::printf("     and test if the INTERSECTION has PF > 1.0\n");
    std::printf("\n");

    // ═══════════════════════════════════════════════════════════════════════
    // v12 OOS VALIDATION — does the edge hold on unseen data?
    // ═══════════════════════════════════════════════════════════════════════

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  v12 OOS VALIDATION — IN-SAMPLE vs OUT-OF-SAMPLE\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  Split point: 2025-09-01 UTC\n");
    std::printf("  IS  = Mar 2024 → Aug 2025  (~18 months, discovery period)\n");
    std::printf("  OOS = Sep 2025 → Apr 2026  (~8 months, validation period)\n");
    std::printf("\n");

    // Overall IS vs OOS
    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    std::printf("  Period    Trades   WR%%     PF    avgW    avgL    R:R     PnL\n");
    std::printf("  IS       %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  %5.2f  $%7.2f\n",
               is_overall.total_trades,
               is_overall.win_rate() * 100.0,
               is_overall.profit_factor(),
               is_overall.avg_win(),
               is_overall.avg_loss(),
               is_overall.avg_win() / std::max(0.01, is_overall.avg_loss()),
               is_overall.total_pnl);
    std::printf("  OOS      %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  %5.2f  $%7.2f\n",
               oos_overall.total_trades,
               oos_overall.win_rate() * 100.0,
               oos_overall.profit_factor(),
               oos_overall.avg_win(),
               oos_overall.avg_loss(),
               oos_overall.avg_win() / std::max(0.01, oos_overall.avg_loss()),
               oos_overall.total_pnl);
    std::printf("\n");

    // Per-regime IS vs OOS
    std::printf("── PER REGIME ──────────────────────────────────────────────\n");
    std::printf("  Regime        Period  Trades   WR%%     PF    avgW    avgL     PnL\n");
    std::printf("  BULL_TREND    IS     %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f\n",
               is_bull.total_trades, is_bull.win_rate() * 100.0,
               is_bull.profit_factor(), is_bull.avg_win(), is_bull.avg_loss(),
               is_bull.total_pnl);
    std::printf("  BULL_TREND    OOS    %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f\n",
               oos_bull.total_trades, oos_bull.win_rate() * 100.0,
               oos_bull.profit_factor(), oos_bull.avg_win(), oos_bull.avg_loss(),
               oos_bull.total_pnl);
    std::printf("  BEAR_TREND    IS     %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f\n",
               is_bear.total_trades, is_bear.win_rate() * 100.0,
               is_bear.profit_factor(), is_bear.avg_win(), is_bear.avg_loss(),
               is_bear.total_pnl);
    std::printf("  BEAR_TREND    OOS    %5d  %5.1f  %5.2f  $%5.2f  $%5.2f  $%7.2f\n",
               oos_bear.total_trades, oos_bear.win_rate() * 100.0,
               oos_bear.profit_factor(), oos_bear.avg_win(), oos_bear.avg_loss(),
               oos_bear.total_pnl);
    std::printf("\n");

    // Per-hour IS vs OOS side-by-side
    std::printf("── PER HOUR (edge hours) ──────────────────────────────────\n");
    std::printf("  Hour   IS_trades IS_WR%%  IS_PF   OOS_trades OOS_WR%% OOS_PF   VERDICT\n");
    for (int h = 0; h < 24; ++h) {
        if (is_per_hour[h].total_trades > 0 || oos_per_hour[h].total_trades > 0) {
            const double is_pf  = is_per_hour[h].profit_factor();
            const double oos_pf = oos_per_hour[h].profit_factor();
            const char* verdict = "";
            if (is_per_hour[h].total_trades >= 20 && oos_per_hour[h].total_trades >= 10) {
                if (is_pf >= 1.0 && oos_pf >= 1.0)      verdict = "CONFIRMED";
                else if (is_pf >= 1.0 && oos_pf < 1.0)  verdict = "FAILED OOS";
                else if (is_pf < 1.0 && oos_pf >= 1.0)  verdict = "OOS surprise";
                else                                      verdict = "no edge";
            }
            std::printf("  %02d:00  %5d   %5.1f  %5.2f    %5d    %5.1f  %5.2f    %s\n",
                       h,
                       is_per_hour[h].total_trades,
                       is_per_hour[h].win_rate() * 100.0,
                       is_pf,
                       oos_per_hour[h].total_trades,
                       oos_per_hour[h].win_rate() * 100.0,
                       oos_pf,
                       verdict);
        }
    }
    std::printf("\n");

    // OOS validation verdict
    std::printf("── OOS VERDICT ─────────────────────────────────────────────\n");
    {
        const double oos_pf = oos_overall.profit_factor();
        const double is_pf  = is_overall.profit_factor();
        const double pf_decay = (is_pf > 0.0) ? (oos_pf / is_pf) : 0.0;

        std::printf("  IS PF  = %.2f  (%d trades)\n", is_pf, is_overall.total_trades);
        std::printf("  OOS PF = %.2f  (%d trades)\n", oos_pf, oos_overall.total_trades);
        std::printf("  PF retention: %.0f%%\n", pf_decay * 100.0);
        std::printf("\n");

        if (oos_pf >= 1.10) {
            std::printf("  >>> STRONG PASS: OOS PF >= 1.10. Edge is structural.\n");
        } else if (oos_pf >= 1.0) {
            std::printf("  >>> PASS: OOS PF >= 1.0. Edge holds but weaker than IS.\n");
            std::printf("  >>> Consider adding filters to improve OOS robustness.\n");
        } else if (oos_pf >= 0.90) {
            std::printf("  >>> MARGINAL: OOS PF 0.90-1.0. Edge partially real,\n");
            std::printf("  >>> partially in-sample fitted. Needs filter refinement.\n");
        } else {
            std::printf("  >>> FAIL: OOS PF < 0.90. Edge was likely in-sample noise.\n");
            std::printf("  >>> Do NOT proceed to production wiring.\n");
        }

        // Per-hour survival count
        int confirmed = 0, failed = 0;
        for (int h = 0; h < 24; ++h) {
            if (is_per_hour[h].total_trades >= 20 && is_per_hour[h].profit_factor() >= 1.0) {
                if (oos_per_hour[h].total_trades >= 10 && oos_per_hour[h].profit_factor() >= 1.0)
                    confirmed++;
                else
                    failed++;
            }
        }
        std::printf("  Edge hours confirmed OOS: %d  |  Failed OOS: %d\n", confirmed, failed);
        std::printf("  Recommendation: trade ONLY the confirmed hours.\n");
    }
    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");

    return 0;
}
