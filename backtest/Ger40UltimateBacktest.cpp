// =============================================================================
// Ger40UltimateBacktest.cpp -- Standalone backtest harness for GER40/DAX
// =============================================================================
//
// PURPOSE
// -------
// Discovers trend-following edge on GER40/DAX tick data using the same
// methodology proven on NAS100 (Nas100UltimateBacktest v4b).
//
// v1: Wide-open discovery — BOTH directions, ALL hours, loose thresholds.
//
// KEY DIFFERENCES FROM NAS100
// ---------------------------
//   - ATR scale: GER40 ATR ~10-50 pts on 1-min bars (NAS was ~50-150)
//   - Spread: ~1.3 pts European session, ~7.5 pts off-hours
//   - Price range: ~19850-24500 (2025 tape)
//   - Cost model: GER40 CFD, 0.1 lot = ~€0.10/pt ≈ $0.11/pt. Using $0.10/pt.
//   - DATA: only 1 year (Jan-Dec 2025) vs NAS's 830 days. Thinner OOS.
//   - TIMESTAMPS: EET (UTC+2 winter, UTC+3 summer). We subtract 2h for
//     approximate UTC. DST error is ±1h — acceptable for v1 discovery.
//
// DATA FORMAT
// -----------
// Auto-detects:
//   A) JForex/Dukascopy: "Time (EET),Ask,Bid,..." header, ask before bid
//   B) Harness:          YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol (no header)
//   C) HistData:         YYYYMMDD HHMMSSmmm,bid,ask,0
//   D) Dukascopy std:    timestamp_ms,ask,bid,ask_vol,bid_vol
//   E) L2 new:           ts_ms,mid,bid,ask,...
//
// COMPILATION
// -----------
//   cd omega_repo
//   g++ -std=c++17 -O2 -o backtest/ger40_bt backtest/Ger40UltimateBacktest.cpp
//   ./backtest/ger40_bt ~/Tick/GER40/DEUIDXEUR_Ticks_2025.01.01_2025.12.31.harness.csv
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

// =============================================================================
// INSTRUMENT CONFIG — all GER40-specific constants in one place
// =============================================================================
namespace cfg {
    // Cost model for GER40 CFD
    constexpr double SPREAD_PTS       = 1.3;     // typical spread (European session)
    constexpr double COMMISSION_PT    = 0.0;     // included in spread for CFDs
    constexpr double LOT_SIZE         = 0.10;    // 0.1 lot
    constexpr double PNL_PER_PT       = LOT_SIZE * 1.0;  // ~$0.10 per point at 0.1 lot

    // ATR scale (GER40: 10-50 pts typical on 1-min bars)
    constexpr double ATR_INIT         = 25.0;    // initial ATR estimate
    constexpr double ATR_MIN_CLAMP    = 3.0;     // minimum sensible ATR

    // v1 DISCOVERY: loose thresholds
    constexpr double DRIFT_ENTRY_MIN  = 15.0;    // loose: 15 pts minimum drift
    constexpr double DRIFT_LONG_AGREE = 0.0;     // long-term drift must agree (sign only)

    // RSI bounds (wide for discovery)
    constexpr double RSI_LONG_MAX     = 70.0;
    constexpr double RSI_LONG_MIN     = 38.0;
    constexpr double RSI_SHORT_MAX    = 62.0;
    constexpr double RSI_SHORT_MIN    = 30.0;

    // SL/TP geometry — same R:R as NAS (2:4 = 1:2)
    constexpr double SL_ATR_MULT      = 2.0;
    constexpr double TP_ATR_MULT      = 4.0;

    // Trail (ATR multiples)
    constexpr double TRAIL_TRIGGER_ATR = 3.0;
    constexpr double TRAIL_DIST_ATR    = 2.0;

    // Evaluation interval
    constexpr int64_t EVAL_INTERVAL_MS = 300000;  // 5 minutes

    // Warmup
    constexpr int WARMUP_BARS         = 55;     // bars before trading

    // v1: ALL HOURS — no session filter for discovery
    inline bool session_ok(int /*hour_utc*/) noexcept {
        return true;
    }

    // v1: NO ATR BAND FILTER
    constexpr double ATR_ENTRY_FLOOR   = 3.0;    // just above minimum clamp
    constexpr double ATR_ENTRY_CEILING = 999.0;

    // v1: BOTH DIRECTIONS
    constexpr bool SHORT_ONLY = false;

    // EET to UTC offset (approximate: using +2 for simplicity, DST adds ±1h error)
    constexpr int EET_TO_UTC_HOURS = 2;

    // Weekend gate
    inline bool is_weekend(int64_t ts_ms) noexcept {
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;  // 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat
        if (dow == 0 || dow == 6) return true;
        if (dow == 5) {
            const int hour = static_cast<int>((sec % 86400) / 3600);
            if (hour >= 21) return true;
        }
        return false;
    }
}

// =============================================================================
// 1-MINUTE BAR
// =============================================================================
struct Bar1m {
    int64_t open_ms  = 0;
    double  open     = 0.0;
    double  high     = 0.0;
    double  low      = 99999999.0;
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
};

// =============================================================================
// INDICATORS
// =============================================================================
struct SimIndicators {
    static constexpr int64_t BAR_PERIOD_MS   = 60000;
    static constexpr int     ATR_PERIOD      = 14;
    static constexpr int     RSI_PERIOD      = 14;
    static constexpr int     DRIFT_WINDOW    = 50;
    static constexpr int     DRIFT_LONG_WINDOW = 200;
    static constexpr int     STRUCTURE_WINDOW  = 20;

    Bar1m current_bar{};
    std::deque<Bar1m> bars;
    bool bar_active = false;
    int  total_bars = 0;

    double atr_pts      = cfg::ATR_INIT;
    double ewm_drift    = 0.0;
    double ewm_drift_long = 0.0;
    double rsi14        = 50.0;
    double vol_ratio    = 1.0;
    double ema9         = 0.0;
    double ema50        = 0.0;
    bool   higher_highs = false;
    bool   lower_lows   = false;

    double avg_gain = 0.0;
    double avg_loss = 0.0;
    std::deque<double> tr_history;
    std::deque<double> bar_returns;
    double recent_vol_ewm = cfg::ATR_INIT;
    double base_vol_ewm   = cfg::ATR_INIT;
    int directional_bars_count = 0;

    bool on_tick(double mid, int64_t ts_ms) noexcept {
        if (!bar_active) {
            current_bar.reset(ts_ms, mid);
            bar_active = true;
            return false;
        }
        if (ts_ms - current_bar.open_ms >= BAR_PERIOD_MS) {
            bars.push_back(current_bar);
            total_bars++;
            _on_bar_close();
            current_bar.reset(ts_ms, mid);
            while (bars.size() > 260) bars.pop_front();
            return true;
        }
        current_bar.update(mid);
        return false;
    }

private:
    void _on_bar_close() noexcept {
        if (bars.size() < 2) return;
        const Bar1m& bar = bars.back();
        const Bar1m& prev = bars[bars.size() - 2];

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
        atr_pts = std::max(cfg::ATR_MIN_CLAMP, atr_pts);

        const double bar_return = bar.close - prev.close;
        bar_returns.push_back(bar_return);
        if ((int)bar_returns.size() > DRIFT_LONG_WINDOW) bar_returns.pop_front();

        ewm_drift = 0.0;
        int short_count = 0;
        for (int i = (int)bar_returns.size() - 1;
             i >= 0 && short_count < DRIFT_WINDOW; --i, ++short_count) {
            ewm_drift += bar_returns[i];
        }
        ewm_drift_long = 0.0;
        for (double r : bar_returns) ewm_drift_long += r;

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
            if (avg_loss > 0.0001)
                rsi14 = 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
            else
                rsi14 = 100.0;
        }

        const double k9  = 2.0 / (9.0 + 1.0);
        const double k50 = 2.0 / (50.0 + 1.0);
        if (ema9 == 0.0)  ema9  = bar.close;
        if (ema50 == 0.0) ema50 = bar.close;
        ema9  = bar.close * k9  + ema9  * (1.0 - k9);
        ema50 = bar.close * k50 + ema50 * (1.0 - k50);

        recent_vol_ewm = 0.90 * recent_vol_ewm + 0.10 * tr;
        base_vol_ewm   = 0.99 * base_vol_ewm   + 0.01 * tr;
        vol_ratio = (base_vol_ewm > 0.01) ? recent_vol_ewm / base_vol_ewm : 1.0;

        if ((int)bars.size() >= STRUCTURE_WINDOW) {
            const int n = std::min((int)bars.size(), STRUCTURE_WINDOW);
            const int half = n / 2;
            double recent_high = 0.0, older_high = 0.0;
            double recent_low = 99999999.0, older_low = 99999999.0;
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
    }
};

// =============================================================================
// ENTRY FILTER
// =============================================================================
struct EntryFilter {
    static bool trend_entry_ok(const SimIndicators& ind, bool is_long) noexcept {
        if (is_long && !(ind.ema9 > ind.ema50)) return false;
        if (!is_long && !(ind.ema9 < ind.ema50)) return false;
        if (is_long && (ind.rsi14 > cfg::RSI_LONG_MAX || ind.rsi14 < cfg::RSI_LONG_MIN)) return false;
        if (!is_long && (ind.rsi14 < cfg::RSI_SHORT_MIN || ind.rsi14 > cfg::RSI_SHORT_MAX)) return false;
        if (is_long && ind.ewm_drift < cfg::DRIFT_ENTRY_MIN) return false;
        if (!is_long && ind.ewm_drift > -cfg::DRIFT_ENTRY_MIN) return false;
        if (is_long && ind.ewm_drift_long < cfg::DRIFT_LONG_AGREE) return false;
        if (!is_long && ind.ewm_drift_long > -cfg::DRIFT_LONG_AGREE) return false;
        if (ind.atr_pts < 5.0) return false;
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            const double dist_to_ema50 = std::fabs(close - ind.ema50);
            if (dist_to_ema50 > ind.atr_pts * 3.0) return false;
        }
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            if (is_long && close < ind.ema9) return false;
            if (!is_long && close > ind.ema9) return false;
        }
        return true;
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
    double  atr_at_entry = 25.0;
    bool    trail_active = false;
    int64_t entry_time   = 0;
    int     bars_at_entry = 0;
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
    std::vector<double> pnl_series;

    void record(double pnl) {
        total_trades++;
        total_pnl += pnl;
        pnl_series.push_back(pnl);
        if (pnl > 0) { wins++; gross_wins += pnl; }
        else         { losses++; gross_losses += std::fabs(pnl); }
        if (total_pnl > peak_equity) peak_equity = total_pnl;
        const double dd = peak_equity - total_pnl;
        if (dd > max_drawdown) max_drawdown = dd;
    }

    double win_rate() const { return total_trades > 0 ? (double)wins / total_trades : 0.0; }
    double profit_factor() const { return gross_losses > 0 ? gross_wins / gross_losses : 999.0; }
    double avg_pnl() const { return total_trades > 0 ? total_pnl / total_trades : 0.0; }
    double avg_win() const { return wins > 0 ? gross_wins / wins : 0.0; }
    double avg_loss() const { return losses > 0 ? gross_losses / losses : 0.0; }

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
                   "avg=$%.3f  maxDD=$%.2f  Sharpe=%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_pnl(), max_drawdown, sharpe_annual());
    }

    void print_detail(const char* label) const {
        std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  "
                   "avgW=$%.3f  avgL=$%.3f  maxDD=$%.2f\n",
                   label, total_trades,
                   win_rate() * 100.0, profit_factor(),
                   total_pnl, avg_win(), avg_loss(), max_drawdown);
    }
};

// =============================================================================
// EXIT DIAGNOSTICS
// =============================================================================
struct ExitDiagnostics {
    int sl_hit = 0, tp_hit = 0, trail_exit = 0;
    void print() const {
        std::printf("  Exit breakdown:\n");
        std::printf("    Full SL hit:   %d\n", sl_hit);
        std::printf("    Trail exit:    %d  (reached %.1f ATR MFE, trailed out)\n",
                   trail_exit, cfg::TRAIL_TRIGGER_ATR);
        std::printf("    Full TP hit:   %d\n", tp_hit);
    }
};

// =============================================================================
// CSV FORMAT DETECTION + PARSERS
// =============================================================================
struct Tick {
    int64_t timestamp_ms = 0;
    double  bid          = 0.0;
    double  ask          = 0.0;
};

enum class CsvFormat { JFOREX, HARNESS, HISTDATA, DUKA_ASK_BID, DUKA_BID_ASK, L2_NEW, L2_OLD, UNKNOWN };

// Convert YYYY.MM.DD + HH:MM:SS.mmm to epoch ms (treating input as EET, converting to UTC)
int64_t eet_to_epoch_ms(int year, int month, int day, int hour, int minute, int second, int ms) {
    static constexpr int64_t EPOCH_2025 = 1735689600LL * 1000LL;  // 2025-01-01 00:00:00 UTC
    static constexpr int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    int days = 0;
    if (year >= 2025) {
        for (int y = 2025; y < year; ++y)
            days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    } else {
        for (int y = year; y < 2025; ++y)
            days -= (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) {
        days += DAYS_IN_MONTH[m];
        if (m == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) days += 1;
    }
    days += (day - 1);

    // Subtract EET offset to get UTC
    int utc_hour = hour - cfg::EET_TO_UTC_HOURS;
    int day_adj = 0;
    if (utc_hour < 0) { utc_hour += 24; day_adj = -1; }

    return EPOCH_2025 + ((int64_t)days + day_adj) * 86400000LL
         + (int64_t)utc_hour * 3600000LL
         + (int64_t)minute * 60000LL
         + (int64_t)second * 1000LL
         + (int64_t)ms;
}

CsvFormat detect_format(const std::string& header) {
    // JForex header: "Time (EET),Ask,Bid,..."
    if (header.find("Time (EET)") != std::string::npos) return CsvFormat::JFOREX;

    // Harness: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol
    if (header.size() > 20 && header[4] == '.' && header[7] == '.' && header[10] == ',') {
        if (header[13] == ':' && header[16] == ':')
            return CsvFormat::HARNESS;
    }

    // HistData: YYYYMMDD HHMMSSmmm,bid,ask,0
    if (header.size() > 20 && header[0] >= '0' && header[0] <= '9' && header[8] == ' ') {
        bool all_digits = true;
        for (int i = 0; i < 8; ++i) if (header[i] < '0' || header[i] > '9') all_digits = false;
        if (all_digits && header[18] == ',') return CsvFormat::HISTDATA;
    }

    if (header.find("ts_ms,mid,bid,ask") != std::string::npos) return CsvFormat::L2_NEW;
    if (header.find("ts_ms,bid,ask") != std::string::npos) return CsvFormat::L2_OLD;
    if (header.find("timestamp_ms,ask,bid") != std::string::npos ||
        header.find("timestamp,askPrice,bidPrice") != std::string::npos) return CsvFormat::DUKA_ASK_BID;
    if (header.find("timestamp_ms,bid,ask") != std::string::npos ||
        header.find("timestamp,bidPrice,askPrice") != std::string::npos) return CsvFormat::DUKA_BID_ASK;

    return CsvFormat::UNKNOWN;
}

const char* format_name(CsvFormat fmt) {
    switch (fmt) {
        case CsvFormat::JFOREX:       return "JFOREX";
        case CsvFormat::HARNESS:      return "HARNESS";
        case CsvFormat::HISTDATA:     return "HISTDATA";
        case CsvFormat::L2_NEW:       return "L2_NEW";
        case CsvFormat::L2_OLD:       return "L2_OLD";
        case CsvFormat::DUKA_ASK_BID: return "DUKA_ASK_BID";
        case CsvFormat::DUKA_BID_ASK: return "DUKA_BID_ASK";
        default: return "UNKNOWN";
    }
}

// JForex: "2025.01.02 01:02:14.945,19857.645,19849.355,0.00001,0.00002"
// Fields: time_eet, ASK, BID, ask_vol, bid_vol
bool parse_jforex(const std::string& line, Tick& t) {
    int year, month, day, hour, minute, second, ms;
    double ask_val, bid_val;
    if (std::sscanf(line.c_str(), "%4d.%2d.%2d %2d:%2d:%2d.%3d,%lf,%lf",
                    &year, &month, &day, &hour, &minute, &second, &ms,
                    &ask_val, &bid_val) == 9) {
        t.timestamp_ms = eet_to_epoch_ms(year, month, day, hour, minute, second, ms);
        t.bid = bid_val;
        t.ask = ask_val;
        return true;
    }
    return false;
}

// Harness: "2025.01.02,01:02:14.945,19849.355,19857.645,0.00003"
// Fields: date, time, BID, ASK, vol
bool parse_harness(const std::string& line, Tick& t) {
    int year, month, day, hour, minute, second, ms;
    double bid_val, ask_val;
    if (std::sscanf(line.c_str(), "%4d.%2d.%2d,%2d:%2d:%2d.%3d,%lf,%lf",
                    &year, &month, &day, &hour, &minute, &second, &ms,
                    &bid_val, &ask_val) == 9) {
        t.timestamp_ms = eet_to_epoch_ms(year, month, day, hour, minute, second, ms);
        t.bid = bid_val;
        t.ask = ask_val;
        return true;
    }
    return false;
}

// HistData: "20240101 180000178,4770.867000,4771.384000,0"
bool parse_histdata(const std::string& line, Tick& t) {
    if (line.size() < 20) return false;
    int date_int = 0;
    for (int i = 0; i < 8; ++i) {
        if (line[i] < '0' || line[i] > '9') return false;
        date_int = date_int * 10 + (line[i] - '0');
    }
    if (line[8] != ' ') return false;
    int time_int = 0;
    int ti = 9;
    while (ti < (int)line.size() && line[ti] >= '0' && line[ti] <= '9') {
        time_int = time_int * 10 + (line[ti] - '0');
        ti++;
    }
    if (ti >= (int)line.size() || line[ti] != ',') return false;
    double bid_val, ask_val;
    if (std::sscanf(line.c_str() + ti + 1, "%lf,%lf", &bid_val, &ask_val) != 2) return false;

    const int year  = date_int / 10000;
    const int month = (date_int / 100) % 100;
    const int day   = date_int % 100;
    const int hour  = time_int / 10000000;
    const int minute = (time_int / 100000) % 100;
    const int second = (time_int / 1000) % 100;
    const int ms     = time_int % 1000;

    // HistData timestamps are typically UTC, no EET conversion
    static constexpr int64_t EPOCH_2025 = 1735689600LL * 1000LL;
    static constexpr int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = 0;
    for (int y = 2025; y < year; ++y)
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    for (int m = 1; m < month; ++m) {
        days += DAYS_IN_MONTH[m];
        if (m == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) days += 1;
    }
    days += (day - 1);
    t.timestamp_ms = EPOCH_2025 + (int64_t)days * 86400000LL
                   + (int64_t)hour * 3600000LL
                   + (int64_t)minute * 60000LL
                   + (int64_t)second * 1000LL
                   + (int64_t)ms;
    t.bid = bid_val;
    t.ask = ask_val;
    return true;
}

bool parse_duka_ask_bid(const std::string& line, Tick& t) {
    double ask_val = 0, bid_val = 0;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf",
                    (long long*)&t.timestamp_ms, &ask_val, &bid_val) == 3) {
        t.bid = bid_val; t.ask = ask_val; return true;
    }
    return false;
}

bool parse_duka_bid_ask(const std::string& line, Tick& t) {
    return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                       (long long*)&t.timestamp_ms, &t.bid, &t.ask) == 3;
}

bool parse_l2_new(const std::string& line, Tick& t) {
    double mid = 0.0;
    return std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf",
                       (long long*)&t.timestamp_ms, &mid, &t.bid, &t.ask) >= 4;
}

bool parse_l2_old(const std::string& line, Tick& t) {
    return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                       (long long*)&t.timestamp_ms, &t.bid, &t.ask) >= 3;
}

bool parse_line(CsvFormat fmt, const std::string& line, Tick& t) {
    switch (fmt) {
        case CsvFormat::JFOREX:       return parse_jforex(line, t);
        case CsvFormat::HARNESS:      return parse_harness(line, t);
        case CsvFormat::HISTDATA:     return parse_histdata(line, t);
        case CsvFormat::L2_NEW:       return parse_l2_new(line, t);
        case CsvFormat::L2_OLD:       return parse_l2_old(line, t);
        case CsvFormat::DUKA_ASK_BID: return parse_duka_ask_bid(line, t);
        case CsvFormat::DUKA_BID_ASK: return parse_duka_bid_ask(line, t);
        default: return false;
    }
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <tick_csv1> [tick_csv2 ...] [--verbose]\n", argv[0]);
        return 1;
    }

    std::vector<std::string> csv_files;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) { verbose = true; continue; }
        csv_files.push_back(argv[i]);
    }
    std::sort(csv_files.begin(), csv_files.end());

    PerformanceMetrics overall;
    PerformanceMetrics long_trades, short_trades;
    std::array<PerformanceMetrics, 24> per_hour;
    std::array<int, 24> hour_entry_counts{};

    // ATR bands for GER40: [0] <10, [1] 10-25, [2] 25-50, [3] >50
    std::array<PerformanceMetrics, 4> per_atr_band;
    const char* atr_band_labels[] = {"ATR<10", "ATR 10-25", "ATR 25-50", "ATR>50"};

    // Drift bands: [0] 15-40, [1] 40-80, [2] 80-150, [3] >150
    std::array<PerformanceMetrics, 4> per_drift_band;
    const char* drift_band_labels[] = {"drift 15-40", "drift 40-80", "drift 80-150", "drift >150"};

    int mfe_reached[8] = {};
    const double mfe_levels[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0};

    ExitDiagnostics exit_diag;
    int entries_approved = 0, filter_blocked = 0, weekend_blocked = 0;
    int session_blocked = 0, atr_blocked = 0;

    struct TradeContext { int entry_hour = 0, atr_band = 0, drift_band = 0; } trade_ctx;

    // OOS split: 365 days * 0.60 = 219 days
    PerformanceMetrics is_metrics, oos_metrics;
    int64_t oos_split_ts = 0;
    bool oos_split_set = false;

    SimIndicators ind;
    SimTrade      trade;
    int64_t       last_eval_ms = 0;

    double min_px = 99999999, max_px = 0;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;

    std::printf("[GER40-BT] Streaming GER40/DAX backtest v1 (DISCOVERY) from %zu file(s)...\n",
               csv_files.size());
    std::printf("[GER40-BT] Config: SL=%.1f*ATR  TP=%.1f*ATR  drift_min=%.0f  lot=%.2f\n",
               cfg::SL_ATR_MULT, cfg::TP_ATR_MULT, cfg::DRIFT_ENTRY_MIN, cfg::LOT_SIZE);
    std::printf("[GER40-BT] Session filter: ALL HOURS  |  Direction: %s\n",
               cfg::SHORT_ONLY ? "SHORT-ONLY" : "BOTH");
    std::printf("[GER40-BT] ATR band: %.0f-%.0f pts  |  Timestamps: EET-2h≈UTC\n\n",
               cfg::ATR_ENTRY_FLOOR, cfg::ATR_ENTRY_CEILING);

    for (const auto& path : csv_files) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "  WARNING: Cannot open %s\n", path.c_str());
            continue;
        }

        std::string first_line;
        std::getline(f, first_line);
        CsvFormat fmt = detect_format(first_line);
        std::printf("  %s: format=%s", path.c_str(), format_name(fmt));

        // JForex has header; Harness/HistData first line IS data
        bool first_is_data = (fmt != CsvFormat::JFOREX && fmt != CsvFormat::L2_NEW &&
                              fmt != CsvFormat::L2_OLD && fmt != CsvFormat::DUKA_ASK_BID &&
                              fmt != CsvFormat::DUKA_BID_ASK);

        size_t file_ticks = 0;
        auto process_tick_line = [&](const std::string& line) {
            if (line.empty()) return;
            Tick t{};
            if (!parse_line(fmt, line, t)) return;
            if (t.bid <= 0 || t.ask <= 0 || t.ask < t.bid) return;

            const double mid = (t.bid + t.ask) * 0.5;
            if (mid < 10000.0 || mid > 30000.0) return;  // GER40 sanity

            file_ticks++;
            total_ticks++;
            if (mid < min_px) min_px = mid;
            if (mid > max_px) max_px = mid;
            if (first_ts == 0) first_ts = t.timestamp_ms;
            last_ts = t.timestamp_ms;

            if (!oos_split_set && first_ts > 0) {
                // 365 days * 0.60 = 219 days
                oos_split_ts = first_ts + (int64_t)(219.0 * 24.0 * 3600.0 * 1000.0);
                oos_split_set = true;
            }

            if (cfg::is_weekend(t.timestamp_ms)) return;

            ind.on_tick(mid, t.timestamp_ms);
            if (ind.total_bars < cfg::WARMUP_BARS) return;

            // ── Manage open position ─────────────────────────────────
            if (trade.active) {
                const double fav = trade.is_long ? (mid - trade.entry_px)
                                                 : (trade.entry_px - mid);
                if (fav > trade.mfe_pts) trade.mfe_pts = fav;
                if (fav < trade.mae_pts) trade.mae_pts = fav;

                if (trade.mfe_pts >= trade.atr_at_entry * cfg::TRAIL_TRIGGER_ATR) {
                    trade.trail_active = true;
                    const double trail_distance = trade.atr_at_entry * cfg::TRAIL_DIST_ATR;
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

                bool sl_hit = trade.is_long ? (t.bid <= trade.sl_px) : (t.ask >= trade.sl_px);
                bool tp_hit = trade.is_long ? (t.bid >= trade.tp_px) : (t.ask <= trade.tp_px);

                if (sl_hit || tp_hit) {
                    double exit_px = tp_hit ? trade.tp_px : trade.sl_px;
                    const double pnl_pts = trade.is_long ? (exit_px - trade.entry_px)
                                                         : (trade.entry_px - exit_px);
                    const double pnl_usd = pnl_pts * cfg::PNL_PER_PT;

                    overall.record(pnl_usd);
                    if (trade.is_long) long_trades.record(pnl_usd);
                    else short_trades.record(pnl_usd);

                    if (trade.entry_time < oos_split_ts) is_metrics.record(pnl_usd);
                    else oos_metrics.record(pnl_usd);

                    per_hour[trade_ctx.entry_hour].record(pnl_usd);
                    per_atr_band[trade_ctx.atr_band].record(pnl_usd);
                    per_drift_band[trade_ctx.drift_band].record(pnl_usd);

                    const double mfe_atr = trade.mfe_pts / std::max(1.0, trade.atr_at_entry);
                    for (int m = 0; m < 8; ++m) {
                        if (mfe_atr >= mfe_levels[m]) mfe_reached[m]++;
                    }

                    if (tp_hit) exit_diag.tp_hit++;
                    else if (trade.trail_active) exit_diag.trail_exit++;
                    else exit_diag.sl_hit++;

                    if (verbose) {
                        std::printf("[GER40-CLOSE] %s %s pnl=$%.3f mfe=%.1f mae=%.1f atr=%.1f\n",
                                   trade.is_long ? "LONG" : "SHORT",
                                   tp_hit ? "TP" : "SL",
                                   pnl_usd, trade.mfe_pts, trade.mae_pts, trade.atr_at_entry);
                    }
                    trade.active = false;
                }
                return;
            }

            // ── Evaluate new entry ───────────────────────────────────
            if (t.timestamp_ms - last_eval_ms < cfg::EVAL_INTERVAL_MS) return;
            last_eval_ms = t.timestamp_ms;

            {
                const int hour_utc = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                if (!cfg::session_ok(hour_utc)) {
                    session_blocked++;
                    return;
                }
            }

            if (ind.atr_pts < cfg::ATR_ENTRY_FLOOR || ind.atr_pts > cfg::ATR_ENTRY_CEILING) {
                atr_blocked++;
                return;
            }

            const bool candidate_long = (ind.ewm_drift > 0.0);

            if (cfg::SHORT_ONLY && candidate_long) {
                filter_blocked++;
                return;
            }

            if (!EntryFilter::trend_entry_ok(ind, candidate_long)) {
                filter_blocked++;
                return;
            }

            entries_approved++;

            const double sl_pts = ind.atr_pts * cfg::SL_ATR_MULT;
            const double tp_pts = ind.atr_pts * cfg::TP_ATR_MULT;

            trade.active       = true;
            trade.is_long      = candidate_long;
            trade.entry_px     = candidate_long ? t.ask : t.bid;
            trade.atr_at_entry = ind.atr_pts;
            trade.entry_time   = t.timestamp_ms;
            trade.bars_at_entry = ind.total_bars;
            trade.mfe_pts      = 0.0;
            trade.mae_pts      = 0.0;
            trade.trail_active = false;

            if (candidate_long) {
                trade.sl_px = trade.entry_px - sl_pts;
                trade.tp_px = trade.entry_px + tp_pts;
            } else {
                trade.sl_px = trade.entry_px + sl_pts;
                trade.tp_px = trade.entry_px - tp_pts;
            }

            {
                const int h = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                trade_ctx.entry_hour = h;
                hour_entry_counts[h]++;

                if (ind.atr_pts < 10.0) trade_ctx.atr_band = 0;
                else if (ind.atr_pts < 25.0) trade_ctx.atr_band = 1;
                else if (ind.atr_pts < 50.0) trade_ctx.atr_band = 2;
                else trade_ctx.atr_band = 3;

                const double abs_drift = std::fabs(ind.ewm_drift);
                if (abs_drift < 40.0) trade_ctx.drift_band = 0;
                else if (abs_drift < 80.0) trade_ctx.drift_band = 1;
                else if (abs_drift < 150.0) trade_ctx.drift_band = 2;
                else trade_ctx.drift_band = 3;
            }

            if (verbose) {
                const int h_utc = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                std::printf("[GER40-ENTRY] %s @ %.1f sl=%.1f tp=%.1f drift=%.1f atr=%.1f hour=%d\n",
                           candidate_long ? "LONG" : "SHORT",
                           trade.entry_px, trade.sl_px, trade.tp_px,
                           ind.ewm_drift, ind.atr_pts, h_utc);
            }
        };

        if (first_is_data) process_tick_line(first_line);

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
    std::printf("  GER40/DAX BACKTEST RESULTS v1 (DISCOVERY — BOTH DIRECTIONS)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    std::printf("  Data: %zu file(s)\n", csv_files.size());
    std::printf("  Ticks: %zu  |  Period: %.1f hours (%.1f days)\n",
               total_ticks, hours, hours / 24.0);
    std::printf("  Price range: %.1f - %.1f (range=%.1f pts)\n", min_px, max_px, max_px - min_px);
    std::printf("  Bars generated: %d (1-minute)\n", ind.total_bars);
    std::printf("  Final: ATR=%.1f  drift_short=%.1f  drift_long=%.1f  RSI=%.1f\n",
               ind.atr_pts, ind.ewm_drift, ind.ewm_drift_long, ind.rsi14);
    std::printf("  NOTE: timestamps are EET-2h≈UTC (±1h DST error)\n\n");

    std::printf("── CONFIG ──────────────────────────────────────────────────\n");
    std::printf("  SL=%.1f*ATR  TP=%.1f*ATR  (R:R = 1:%.1f)\n",
               cfg::SL_ATR_MULT, cfg::TP_ATR_MULT, cfg::TP_ATR_MULT / cfg::SL_ATR_MULT);
    std::printf("  Trail: trigger=%.1f ATR MFE, dist=%.1f ATR\n",
               cfg::TRAIL_TRIGGER_ATR, cfg::TRAIL_DIST_ATR);
    std::printf("  Drift entry min: %.0f pts  |  Lot: %.2f  PnL/pt: $%.3f\n",
               cfg::DRIFT_ENTRY_MIN, cfg::LOT_SIZE, cfg::PNL_PER_PT);
    std::printf("  Direction: %s  |  Session: ALL HOURS (v1)\n\n",
               cfg::SHORT_ONLY ? "SHORT-ONLY" : "BOTH");

    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    overall.print("TOTAL");
    overall.print_detail("TOTAL (detail)");
    long_trades.print("LONG trades");
    short_trades.print("SHORT trades");
    std::printf("  Filter blocked: %d  |  Weekend blocked: %d\n", filter_blocked, weekend_blocked);
    std::printf("  Session blocked: %d  |  ATR blocked: %d\n", session_blocked, atr_blocked);
    std::printf("  Entries approved: %d\n\n", entries_approved);

    std::printf("── EXIT ANALYSIS ───────────────────────────────────────────\n");
    exit_diag.print();
    std::printf("\n");

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  DIAGNOSTIC: WHERE IS THE EDGE?\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    std::printf("── H1: PF BY HOUR (approx UTC) ───────────────────────────\n");
    std::printf("  Hour  Trades   WR%%    PF    avgW    avgL      PnL\n");
    for (int h = 0; h < 24; ++h) {
        if (per_hour[h].total_trades > 0) {
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%6.3f  $%6.3f  $%8.3f  %s\n",
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

    std::printf("── H2: PF BY ATR BAND ─────────────────────────────────────\n");
    std::printf("  Band          Trades   WR%%    PF    avgW    avgL      PnL\n");
    for (int b = 0; b < 4; ++b) {
        if (per_atr_band[b].total_trades > 0) {
            std::printf("  %-14s %5d  %5.1f  %5.2f  $%6.3f  $%6.3f  $%8.3f  %s\n",
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

    std::printf("── H3: PF BY DRIFT MAGNITUDE ─────────────────────────────\n");
    std::printf("  Drift         Trades   WR%%    PF    avgW    avgL      PnL\n");
    for (int d = 0; d < 4; ++d) {
        if (per_drift_band[d].total_trades > 0) {
            std::printf("  %-14s %5d  %5.1f  %5.2f  $%6.3f  $%6.3f  $%8.3f  %s\n",
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

    std::printf("── H4: DIRECTION ──────────────────────────────────────────\n");
    std::printf("  LONG:  trades=%d  WR=%.1f%%  PF=%.2f  avgW=$%.3f  avgL=$%.3f  PnL=$%.2f\n",
               long_trades.total_trades, long_trades.win_rate() * 100.0,
               long_trades.profit_factor(), long_trades.avg_win(), long_trades.avg_loss(),
               long_trades.total_pnl);
    std::printf("  SHORT: trades=%d  WR=%.1f%%  PF=%.2f  avgW=$%.3f  avgL=$%.3f  PnL=$%.2f\n",
               short_trades.total_trades, short_trades.win_rate() * 100.0,
               short_trades.profit_factor(), short_trades.avg_win(), short_trades.avg_loss(),
               short_trades.total_pnl);
    std::printf("\n");

    std::printf("── MFE DISTRIBUTION ───────────────────────────────────────\n");
    if (overall.total_trades > 0) {
        std::printf("  Level (ATR)  Reached    %%\n");
        for (int m = 0; m < 8; ++m) {
            std::printf("  >= %.1f ATR   %5d   %5.1f%%\n",
                       mfe_levels[m], mfe_reached[m],
                       100.0 * mfe_reached[m] / overall.total_trades);
        }
    }
    std::printf("\n");

    if (overall.total_trades > 0) {
        std::printf("── MATH CHECK ─────────────────────────────────────────────\n");
        const double wr = overall.win_rate();
        const double required_rr = (wr > 0 && wr < 1) ? (1.0 - wr) / wr : 999.0;
        const double actual_rr = overall.avg_win() / std::max(0.001, overall.avg_loss());
        std::printf("  WR=%.1f%%  Required R:R for PF=1.0: %.2f  |  Actual R:R: %.2f\n",
                   wr * 100.0, required_rr, actual_rr);
        std::printf("  Spread cost per trade: ~$%.3f (%.1f pts * $%.3f/pt)\n",
                   cfg::SPREAD_PTS * cfg::PNL_PER_PT, cfg::SPREAD_PTS, cfg::PNL_PER_PT);
    }
    std::printf("\n");

    // OOS
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  IN-SAMPLE / OUT-OF-SAMPLE VALIDATION (60/40 split)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    {
        const int64_t split_sec = oos_split_ts / 1000;
        int y = 1970, remaining = (int)(split_sec / 86400);
        while (true) {
            int diy = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
            if (remaining < diy) break;
            remaining -= diy; y++;
        }
        static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int m = 0;
        while (m < 12) {
            int d = dim[m];
            if (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) d = 29;
            if (remaining < d) break;
            remaining -= d; m++;
        }
        std::printf("  Split date: ~%04d-%02d-%02d (219 days from tape start)\n\n", y, m + 1, remaining + 1);
    }

    std::printf("── IN-SAMPLE (first 60%%) ─────────────────────────────────\n");
    is_metrics.print("IS TOTAL");
    is_metrics.print_detail("IS (detail)");
    std::printf("\n");

    std::printf("── OUT-OF-SAMPLE (last 40%%) ──────────────────────────────\n");
    oos_metrics.print("OOS TOTAL");
    oos_metrics.print_detail("OOS (detail)");
    std::printf("\n");

    const bool oos_pass = (oos_metrics.total_trades >= 8 && oos_metrics.profit_factor() >= 1.20);
    std::printf("  OOS VERDICT: %s\n",
               oos_pass ? "PASS — edge persists out-of-sample."
                        : "REVIEW — needs further filtering or insufficient OOS trades.");
    std::printf("    IS:  PF=%.2f  trades=%d  PnL=$%.2f\n",
               is_metrics.profit_factor(), is_metrics.total_trades, is_metrics.total_pnl);
    std::printf("    OOS: PF=%.2f  trades=%d  PnL=$%.2f\n",
               oos_metrics.profit_factor(), oos_metrics.total_trades, oos_metrics.total_pnl);
    if (oos_metrics.total_trades > 0 && is_metrics.total_trades > 0) {
        const double pf_decay = 1.0 - oos_metrics.profit_factor() / is_metrics.profit_factor();
        std::printf("    PF decay IS->OOS: %.0f%% %s\n", pf_decay * 100.0,
                   pf_decay > 0.50 ? "(WARNING: >50% decay)" : (pf_decay > 0.30 ? "(moderate)" : "(healthy)"));
    }
    std::printf("\n");

    return 0;
}
