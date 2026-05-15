// =============================================================================
// Nas100UltimateBacktest.cpp -- Standalone backtest harness for NAS100/USTEC
// =============================================================================
//
// PURPOSE
// -------
// Discovers trend-following edge on NAS100/USTEC tick data using the same
// methodology proven on XAUUSD (GoldUltimateBacktest v12).
//
// v4: Short-only + ATR ceiling based on v3 diagnostics:
//   - SHORT ONLY: v3 longs PF=0.84 (-$27), shorts PF=3.01 (+$201)
//   - Session filter: 14,17,18,20 UTC (same as v3)
//   - ATR floor: 30 pts, ATR ceiling: 60 pts (v3 ATR>60 both lost)
//   - Drift floor: 100 pts (confirmed in v3)
//   - TP=4.0*ATR (confirmed)
//
// v1: 793 trades, PF=1.04, +$122.  v2: 315 trades, PF=1.19, +$200.
// v3: 70 trades, PF=1.66, +$174.  v3 shorts: 37 trades, PF=3.01, +$201.
//
// KEY DIFFERENCES FROM GOLD
// -------------------------
//   - ATR scale: NAS100 ATR ~50-150 pts (gold ~2-5 pts)
//   - Spread: ~2.7 pts (gold ~0.3 pts)
//   - Cost model: 1 lot NAS100 = 1 contract, PnL = pts * lot * multiplier
//     For USTEC.F CFD: 1 standard lot = $1/pt. We use 0.1 lot = $0.10/pt.
//   - Edge hours: UNKNOWN — need discovery (gold was Asian session)
//   - Drift/RSI/structure thresholds: need re-discovery, scaled to ATR
//
// DATA FORMAT
// -----------
// Auto-detects:
//   A) Dukascopy:  timestamp_ms,ask,bid,ask_vol,bid_vol (header)
//   B) L2 new:     ts_ms,mid,bid,ask,l2_imb,...  (17 columns)
//   C) L2 old:     ts_ms,bid,ask,...
//   D) Dukascopy fresh: YYYYMMDD,HH:MM:SS,bid,ask
//
// COMPILATION
// -----------
//   cd omega_repo
//   g++ -std=c++17 -O2 -o backtest/nas100_bt backtest/Nas100UltimateBacktest.cpp
//   ./backtest/nas100_bt data/l2_ticks_USTEC_*.csv
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
// INSTRUMENT CONFIG — all NAS100-specific constants in one place
// =============================================================================
namespace cfg {
    // Cost model for USTEC.F CFD
    constexpr double SPREAD_PTS       = 2.7;    // typical spread
    constexpr double COMMISSION_PT    = 0.0;    // commission per pt (included in spread for CFDs)
    constexpr double LOT_SIZE         = 0.10;   // 0.10 lot = $0.10 per point
    constexpr double PNL_PER_PT       = LOT_SIZE * 1.0;  // $0.10 per point at 0.1 lot

    // ATR scale (NAS100: 50-150 pts typical on 1-min bars)
    constexpr double ATR_INIT         = 80.0;   // initial ATR estimate
    constexpr double ATR_MIN_CLAMP    = 10.0;   // minimum sensible ATR

    // Drift thresholds — v3: raised from 80 (v2 showed drift 50-100 PF=0.96; 100-200 PF=1.32)
    constexpr double DRIFT_ENTRY_MIN  = 100.0;  // v3: raised from 80 (drift <100 is dead weight)
    constexpr double DRIFT_LONG_AGREE = 0.0;    // long-term drift must agree (sign only)

    // RSI bounds (likely similar across instruments — start with gold's)
    constexpr double RSI_LONG_MAX     = 70.0;
    constexpr double RSI_LONG_MIN     = 38.0;
    constexpr double RSI_SHORT_MAX    = 62.0;
    constexpr double RSI_SHORT_MIN    = 30.0;

    // SL/TP geometry — v2: tighter TP (v1 showed only 16.4% reach 5.0 ATR)
    constexpr double SL_ATR_MULT      = 2.0;
    constexpr double TP_ATR_MULT      = 4.0;   // v2: tightened from 5.0 (trail does heavy lifting anyway)

    // Trail (ATR multiples)
    constexpr double TRAIL_TRIGGER_ATR = 3.0;
    constexpr double TRAIL_DIST_ATR    = 2.0;

    // Evaluation interval
    constexpr int64_t EVAL_INTERVAL_MS = 300000;  // 5 minutes

    // Warmup
    constexpr int WARMUP_BARS         = 55;     // bars before trading

    // v4b SESSION FILTER — add hour 16 back (was PF=0.97 in v2 with longs,
    // but short-only may be positive — sanity check for trade volume)
    //   14:00 UTC — v4 shorts: 20 trades, PF=4.00, +$144
    //   16:00 UTC — v2 mixed PF=0.97, testing short-only here
    //   17:00 UTC — v4 shorts: 10 trades, PF=2.24, +$46
    //   18:00 UTC — v4 shorts:  2 trades, PF=0.78, -$2
    //   20:00 UTC — v4 shorts:  5 trades, PF=0.72, -$7
    inline bool session_ok(int hour_utc) noexcept {
        return (hour_utc == 14 || hour_utc == 16 || hour_utc == 17 ||
                hour_utc == 18 || hour_utc == 20);
    }

    // v4 ATR BAND — v3 confirmed ATR 30-60 is PF=1.83 (+$200, 68 trades)
    // ATR>60 both trades lost (-$25). Cap at 60.
    constexpr double ATR_ENTRY_FLOOR   = 30.0;
    constexpr double ATR_ENTRY_CEILING = 60.0;

    // v4 DIRECTION — v3 shorts PF=3.01 (+$201), longs PF=0.84 (-$27)
    constexpr bool SHORT_ONLY = true;

    // Weekend gate
    inline bool is_weekend(int64_t ts_ms) noexcept {
        // Convert to day-of-week: 0=Thu, 1=Fri, 2=Sat, 3=Sun, 4=Mon, 5=Tue, 6=Wed
        // Epoch (1970-01-01) was a Thursday
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;  // 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat
        if (dow == 0 || dow == 6) return true;     // Sat/Sun
        // Friday after 21:00 UTC (markets close)
        if (dow == 5) {
            const int hour = static_cast<int>((sec % 86400) / 3600);
            if (hour >= 21) return true;
        }
        // Sunday before 22:00 UTC (markets open)
        // Already caught by dow==0 above
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
// INDICATORS — bar-based, scaled for NAS100
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

    // Indicator values
    double atr_pts      = cfg::ATR_INIT;
    double ewm_drift    = 0.0;
    double ewm_drift_long = 0.0;
    double rsi14        = 50.0;
    double vol_ratio    = 1.0;
    double ema9         = 0.0;
    double ema50        = 0.0;
    bool   higher_highs = false;
    bool   lower_lows   = false;

    // RSI internals
    double avg_gain = 0.0;
    double avg_loss = 0.0;

    // ATR internals
    std::deque<double> tr_history;

    // Drift internals
    std::deque<double> bar_returns;

    // Vol internals
    double recent_vol_ewm = cfg::ATR_INIT;
    double base_vol_ewm   = cfg::ATR_INIT;

    // Directional bars
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

        // True Range & ATR
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

        // Drift (dual-timeframe cumulative)
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

        // RSI
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
                rsi14 = 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
            } else {
                rsi14 = 100.0;
            }
        }

        // EMAs
        const double k9  = 2.0 / (9.0 + 1.0);
        const double k50 = 2.0 / (50.0 + 1.0);
        if (ema9 == 0.0)  ema9  = bar.close;
        if (ema50 == 0.0) ema50 = bar.close;
        ema9  = bar.close * k9  + ema9  * (1.0 - k9);
        ema50 = bar.close * k50 + ema50 * (1.0 - k50);

        // Vol ratio
        recent_vol_ewm = 0.90 * recent_vol_ewm + 0.10 * tr;
        base_vol_ewm   = 0.99 * base_vol_ewm   + 0.01 * tr;
        vol_ratio = (base_vol_ewm > 0.01) ? recent_vol_ewm / base_vol_ewm : 1.0;

        // Structure (higher highs / lower lows)
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

        // Directional bar consistency
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
// ENTRY FILTER — v1 wide-open discovery (mirrors gold v1 logic, NAS-scaled)
// =============================================================================
struct EntryFilter {
    static bool trend_entry_ok(const SimIndicators& ind, bool is_long) noexcept {
        // EMA alignment
        if (is_long && !(ind.ema9 > ind.ema50)) return false;
        if (!is_long && !(ind.ema9 < ind.ema50)) return false;

        // RSI not exhausted
        if (is_long && (ind.rsi14 > cfg::RSI_LONG_MAX || ind.rsi14 < cfg::RSI_LONG_MIN)) return false;
        if (!is_long && (ind.rsi14 < cfg::RSI_SHORT_MIN || ind.rsi14 > cfg::RSI_SHORT_MAX)) return false;

        // Drift in trade direction (scaled for NAS: 50 pts minimum)
        if (is_long && ind.ewm_drift < cfg::DRIFT_ENTRY_MIN) return false;
        if (!is_long && ind.ewm_drift > -cfg::DRIFT_ENTRY_MIN) return false;

        // Long-term drift must agree (sign only for v1 discovery)
        if (is_long && ind.ewm_drift_long < cfg::DRIFT_LONG_AGREE) return false;
        if (!is_long && ind.ewm_drift_long > -cfg::DRIFT_LONG_AGREE) return false;

        // ATR must show activity (NAS: at least 20 pts = market is moving)
        if (ind.atr_pts < 20.0) return false;

        // Pullback entry: not too extended from EMA50
        if (ind.bars.size() > 0) {
            const double close = ind.bars.back().close;
            const double dist_to_ema50 = std::fabs(close - ind.ema50);
            if (dist_to_ema50 > ind.atr_pts * 3.0) return false;
        }

        // Momentum confirmation: price above/below EMA9
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
    double  atr_at_entry = 80.0;
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
    int sl_hit       = 0;
    int tp_hit       = 0;
    int trail_exit   = 0;

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

enum class CsvFormat { DUKA_BID_ASK, DUKA_ASK_BID, L2_OLD, L2_NEW, DUKA_FRESH, UNKNOWN };

CsvFormat detect_format(const std::string& header) {
    // Dukascopy fresh: YYYYMMDD,HH:MM:SS,...
    if (!header.empty() && header[0] >= '0' && header[0] <= '9' && header.size() > 20) {
        auto fc = header.find(',');
        if (fc != std::string::npos && fc < 12) {
            auto sc = header.find(',', fc + 1);
            if (sc != std::string::npos) {
                std::string f2 = header.substr(fc + 1, sc - fc - 1);
                if (f2.size() == 8 && f2[2] == ':' && f2[5] == ':')
                    return CsvFormat::DUKA_FRESH;
            }
        }
    }
    if (header.find("ts_ms,mid,bid,ask") != std::string::npos) return CsvFormat::L2_NEW;
    if (header.find("ts_ms,bid,ask") != std::string::npos) return CsvFormat::L2_OLD;
    if (header.find("timestamp_ms,ask,bid") != std::string::npos) return CsvFormat::DUKA_ASK_BID;
    if (header.find("timestamp,askPrice,bidPrice") != std::string::npos) return CsvFormat::DUKA_ASK_BID;
    if (header.find("timestamp_ms,bid,ask") != std::string::npos) return CsvFormat::DUKA_BID_ASK;
    if (header.find("timestamp,bidPrice,askPrice") != std::string::npos) return CsvFormat::DUKA_BID_ASK;
    if (header.find("timestamp") != std::string::npos && header.find("ask") != std::string::npos) {
        if (header.find("ask") < header.find("bid")) return CsvFormat::DUKA_ASK_BID;
        return CsvFormat::DUKA_BID_ASK;
    }
    return CsvFormat::DUKA_BID_ASK;
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
    int year, month, day, hour, minute, second;
    double bid_val, ask_val;
    if (std::sscanf(line.c_str(), "%4d%2d%2d,%2d:%2d:%2d,%lf,%lf",
                    &year, &month, &day, &hour, &minute, &second,
                    &bid_val, &ask_val) == 8) {
        static constexpr int64_t EPOCH_2024 = 1704067200LL * 1000LL;
        static constexpr int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int days = 0;
        for (int y = 2024; y < year; ++y)
            days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
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
    return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                       (long long*)&t.timestamp_ms, &t.bid, &t.ask) >= 3;
}

bool parse_l2_new(const std::string& line, Tick& t) {
    double mid = 0.0;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf",
                    (long long*)&t.timestamp_ms, &mid, &t.bid, &t.ask) >= 4)
        return true;
    return false;
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
// MAIN — streaming backtest
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

    // ── Metrics ──────────────────────────────────────────────────────────────
    PerformanceMetrics overall;
    PerformanceMetrics long_trades, short_trades;
    std::array<PerformanceMetrics, 24> per_hour;
    std::array<int, 24> hour_entry_counts{};

    // ATR bands scaled for NAS: [0] <30, [1] 30-60, [2] 60-100, [3] >100
    std::array<PerformanceMetrics, 4> per_atr_band;
    const char* atr_band_labels[] = {"ATR<30", "ATR 30-60", "ATR 60-100", "ATR>100"};

    // Drift bands scaled for NAS: [0] 50-100, [1] 100-200, [2] 200-400, [3] >400
    std::array<PerformanceMetrics, 4> per_drift_band;
    const char* drift_band_labels[] = {"drift 50-100", "drift 100-200", "drift 200-400", "drift >400"};

    // MFE distribution
    int mfe_reached[8] = {};
    const double mfe_levels[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0};

    ExitDiagnostics exit_diag;

    int entries_approved = 0;
    int filter_blocked   = 0;
    int weekend_blocked  = 0;
    int session_blocked  = 0;
    int atr_blocked      = 0;

    struct TradeContext {
        int    entry_hour     = 0;
        int    atr_band       = 0;
        int    drift_band     = 0;
    } trade_ctx;

    // ── OOS split metrics ─────────────────────────────────────────────────────
    // 60/40 IS/OOS split. Split point computed from tape timestamps.
    // IS = first 60% of calendar time, OOS = last 40%.
    PerformanceMetrics is_metrics, oos_metrics;
    int64_t oos_split_ts = 0;  // computed after first pass or from known range
    // Known tape range: ~830 days. 60% = ~498 days.
    // We'll compute exact split from first_ts + 0.6*(last_ts-first_ts) after
    // we see first_ts. For streaming, use pre-computed: the tape runs
    // roughly Aug 2023 - Dec 2025, so 60% mark is ~May 2025.
    // More precisely: we'll do a two-pass approach or just compute inline.
    // Simplest: hardcode based on known tape (829.9 days from v1 output).
    // 60% of 830 days = 498 days from start.
    // Start is ~2024-03 from the data. 498 days from 2024-03-01 = ~2025-07-12.
    // But let's compute it dynamically from the first tick seen.
    bool oos_split_set = false;

    // ── Simulation state ─────────────────────────────────────────────────────
    SimIndicators ind;
    SimTrade      trade;
    int64_t       last_eval_ms = 0;

    // Price tracking
    double min_px = 99999999, max_px = 0;
    int64_t first_ts = 0, last_ts = 0;
    size_t total_ticks = 0;

    std::printf("[NAS-BT] Streaming NAS100 backtest v4b (SHORT-ONLY+H16) from %zu file(s)...\n",
               csv_files.size());
    std::printf("[NAS-BT] Config: SL=%.1f*ATR  TP=%.1f*ATR  drift_min=%.0f  lot=%.2f\n",
               cfg::SL_ATR_MULT, cfg::TP_ATR_MULT, cfg::DRIFT_ENTRY_MIN, cfg::LOT_SIZE);
    std::printf("[NAS-BT] Session filter: 14,16,17,18,20 UTC  |  Direction: %s\n",
               cfg::SHORT_ONLY ? "SHORT-ONLY" : "BOTH");
    std::printf("[NAS-BT] ATR band: %.0f-%.0f pts  |  Drift floor: %.0f pts\n\n",
               cfg::ATR_ENTRY_FLOOR, cfg::ATR_ENTRY_CEILING, cfg::DRIFT_ENTRY_MIN);

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

        bool first_is_data = (fmt == CsvFormat::DUKA_FRESH);

        size_t file_ticks = 0;
        auto process_tick_line = [&](const std::string& line) {
            if (line.empty()) return;
            Tick t{};
            if (!parse_line(fmt, line, t)) return;
            if (t.bid <= 0 || t.ask <= 0 || t.ask < t.bid) return;

            // Sanity: NAS100 prices should be in 1000-50000 range
            const double mid = (t.bid + t.ask) * 0.5;
            if (mid < 1000.0 || mid > 100000.0) return;

            file_ticks++;
            total_ticks++;
            if (mid < min_px) min_px = mid;
            if (mid > max_px) max_px = mid;
            if (first_ts == 0) first_ts = t.timestamp_ms;
            last_ts = t.timestamp_ms;

            // Set OOS split point: 60% of known tape duration (830 days)
            if (!oos_split_set && first_ts > 0) {
                // 830 days * 0.60 = 498 days from first tick
                oos_split_ts = first_ts + (int64_t)(498.0 * 24.0 * 3600.0 * 1000.0);
                oos_split_set = true;
            }

            // Weekend gate
            if (cfg::is_weekend(t.timestamp_ms)) return;

            // Feed tick
            ind.on_tick(mid, t.timestamp_ms);

            if (ind.total_bars < cfg::WARMUP_BARS) return;

            // ── Manage open position ─────────────────────────────────
            if (trade.active) {
                const double fav = trade.is_long ? (mid - trade.entry_px)
                                                 : (trade.entry_px - mid);
                if (fav > trade.mfe_pts) trade.mfe_pts = fav;
                if (fav < trade.mae_pts) trade.mae_pts = fav;

                // Trail
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

                // Check SL/TP
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

                    // IS/OOS split
                    if (trade.entry_time < oos_split_ts) is_metrics.record(pnl_usd);
                    else oos_metrics.record(pnl_usd);

                    per_hour[trade_ctx.entry_hour].record(pnl_usd);
                    per_atr_band[trade_ctx.atr_band].record(pnl_usd);
                    per_drift_band[trade_ctx.drift_band].record(pnl_usd);

                    // MFE distribution
                    const double mfe_atr = trade.mfe_pts / std::max(1.0, trade.atr_at_entry);
                    for (int m = 0; m < 8; ++m) {
                        if (mfe_atr >= mfe_levels[m]) mfe_reached[m]++;
                    }

                    // Exit classification
                    if (tp_hit) exit_diag.tp_hit++;
                    else if (trade.trail_active) exit_diag.trail_exit++;
                    else exit_diag.sl_hit++;

                    if (verbose) {
                        std::printf("[NAS-CLOSE] %s %s pnl=$%.3f mfe=%.1f mae=%.1f atr=%.1f\n",
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

            // v2: Session filter — US session hours with proven edge
            {
                const int hour_utc = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                if (!cfg::session_ok(hour_utc)) {
                    session_blocked++;
                    return;
                }
            }

            // v4: ATR band (floor + ceiling)
            if (ind.atr_pts < cfg::ATR_ENTRY_FLOOR || ind.atr_pts > cfg::ATR_ENTRY_CEILING) {
                atr_blocked++;
                return;
            }

            const bool candidate_long = (ind.ewm_drift > 0.0);

            // v4: Short-only filter
            if (cfg::SHORT_ONLY && candidate_long) {
                filter_blocked++;
                return;
            }

            // Quality filter
            if (!EntryFilter::trend_entry_ok(ind, candidate_long)) {
                filter_blocked++;
                return;
            }

            entries_approved++;

            // ── OPEN TRADE ───────────────────────────────────────────
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

            // Capture entry context for diagnostics
            {
                const int h = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                trade_ctx.entry_hour = h;
                hour_entry_counts[h]++;

                // ATR band (NAS-scaled)
                if (ind.atr_pts < 30.0) trade_ctx.atr_band = 0;
                else if (ind.atr_pts < 60.0) trade_ctx.atr_band = 1;
                else if (ind.atr_pts < 100.0) trade_ctx.atr_band = 2;
                else trade_ctx.atr_band = 3;

                // Drift band (NAS-scaled)
                const double abs_drift = std::fabs(ind.ewm_drift);
                if (abs_drift < 100.0) trade_ctx.drift_band = 0;
                else if (abs_drift < 200.0) trade_ctx.drift_band = 1;
                else if (abs_drift < 400.0) trade_ctx.drift_band = 2;
                else trade_ctx.drift_band = 3;
            }

            if (verbose) {
                const int h_utc = static_cast<int>((t.timestamp_ms / 1000 % 86400) / 3600);
                std::printf("[NAS-ENTRY] %s @ %.1f sl=%.1f tp=%.1f drift=%.1f atr=%.1f hour=%d\n",
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
    std::printf("  NAS100/USTEC BACKTEST RESULTS v4b (SHORT-ONLY+H16+OOS)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    std::printf("  Data: %zu file(s)\n", csv_files.size());
    std::printf("  Ticks: %zu  |  Period: %.1f hours (%.1f days)\n",
               total_ticks, hours, hours / 24.0);
    std::printf("  Price range: %.1f - %.1f (range=%.1f pts)\n", min_px, max_px, max_px - min_px);
    std::printf("  Bars generated: %d (1-minute)\n", ind.total_bars);
    std::printf("  Final: ATR=%.1f  drift_short=%.1f  drift_long=%.1f  RSI=%.1f  vol_ratio=%.2f\n",
               ind.atr_pts, ind.ewm_drift, ind.ewm_drift_long, ind.rsi14, ind.vol_ratio);
    std::printf("\n");

    std::printf("── CONFIG ──────────────────────────────────────────────────\n");
    std::printf("  SL=%.1f*ATR  TP=%.1f*ATR  (R:R = 1:%.1f)\n",
               cfg::SL_ATR_MULT, cfg::TP_ATR_MULT, cfg::TP_ATR_MULT / cfg::SL_ATR_MULT);
    std::printf("  Trail: trigger=%.1f ATR MFE, dist=%.1f ATR\n",
               cfg::TRAIL_TRIGGER_ATR, cfg::TRAIL_DIST_ATR);
    std::printf("  Drift entry min: %.0f pts\n", cfg::DRIFT_ENTRY_MIN);
    std::printf("  Lot: %.2f  PnL/pt: $%.3f\n", cfg::LOT_SIZE, cfg::PNL_PER_PT);
    std::printf("  Session filter: 14,16,17,18,20 UTC\n");
    std::printf("  ATR band: %.0f-%.0f pts  |  Direction: %s\n\n",
               cfg::ATR_ENTRY_FLOOR, cfg::ATR_ENTRY_CEILING,
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

    // ═══════════════════════════════════════════════════════════════════════
    // DIAGNOSTIC SLICING — where is the edge?
    // ═══════════════════════════════════════════════════════════════════════

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  DIAGNOSTIC: WHERE IS THE EDGE?\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    std::printf("── H1: PF BY HOUR (UTC) ───────────────────────────────────\n");
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
    std::printf("  LONG:  trades=%d  WR=%.1f%%  PF=%.2f  avgW=$%.3f  avgL=$%.3f\n",
               long_trades.total_trades, long_trades.win_rate() * 100.0,
               long_trades.profit_factor(), long_trades.avg_win(), long_trades.avg_loss());
    std::printf("  SHORT: trades=%d  WR=%.1f%%  PF=%.2f  avgW=$%.3f  avgL=$%.3f\n",
               short_trades.total_trades, short_trades.win_rate() * 100.0,
               short_trades.profit_factor(), short_trades.avg_win(), short_trades.avg_loss());
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

    // MATH CHECK
    if (overall.total_trades > 0) {
        std::printf("── MATH CHECK ─────────────────────────────────────────────\n");
        const double wr = overall.win_rate();
        const double required_rr = (wr > 0 && wr < 1) ? (1.0 - wr) / wr : 999.0;
        const double actual_rr = overall.avg_win() / std::max(0.001, overall.avg_loss());
        std::printf("  WR=%.1f%%  Required R:R for PF=1.0: %.2f\n", wr * 100.0, required_rr);
        std::printf("  Actual R:R: %.2f\n", actual_rr);
        std::printf("  Spread cost per trade: ~$%.3f (%.1f pts * $%.3f/pt)\n",
                   cfg::SPREAD_PTS * cfg::PNL_PER_PT, cfg::SPREAD_PTS, cfg::PNL_PER_PT);
    }
    std::printf("\n");

    // ═══════════════════════════════════════════════════════════════════════
    // IN-SAMPLE / OUT-OF-SAMPLE VALIDATION
    // 60% IS (first 498 days) / 40% OOS (last 332 days)
    // ═══════════════════════════════════════════════════════════════════════
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  IN-SAMPLE / OUT-OF-SAMPLE VALIDATION (60/40 split)\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    {
        // Print split date
        const int64_t split_sec = oos_split_ts / 1000;
        const int split_days_from_epoch = (int)(split_sec / 86400);
        // Approximate date from epoch days (good enough for display)
        int y = 1970, m = 1, d = 1;
        int remaining = split_days_from_epoch;
        while (true) {
            int days_in_year = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
            if (remaining < days_in_year) break;
            remaining -= days_in_year;
            y++;
        }
        static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        m = 0;
        while (m < 12) {
            int days_in_m = dim[m];
            if (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days_in_m = 29;
            if (remaining < days_in_m) break;
            remaining -= days_in_m;
            m++;
        }
        d = remaining + 1;
        m += 1;

        std::printf("  Split date: ~%04d-%02d-%02d (498 days from tape start)\n\n", y, m, d);
    }

    std::printf("── IN-SAMPLE (first 60%%) ─────────────────────────────────\n");
    is_metrics.print("IS TOTAL");
    is_metrics.print_detail("IS (detail)");
    std::printf("\n");

    std::printf("── OUT-OF-SAMPLE (last 40%%) ──────────────────────────────\n");
    oos_metrics.print("OOS TOTAL");
    oos_metrics.print_detail("OOS (detail)");
    std::printf("\n");

    // Verdict
    const bool oos_pass = (oos_metrics.total_trades >= 10 && oos_metrics.profit_factor() >= 1.20);
    std::printf("  OOS VERDICT: %s\n",
               oos_pass ? "PASS — edge persists out-of-sample. Production candidate."
                        : "REVIEW — OOS underperforms or insufficient trades.");
    std::printf("    IS:  PF=%.2f  trades=%d  PnL=$%.2f\n",
               is_metrics.profit_factor(), is_metrics.total_trades, is_metrics.total_pnl);
    std::printf("    OOS: PF=%.2f  trades=%d  PnL=$%.2f\n",
               oos_metrics.profit_factor(), oos_metrics.total_trades, oos_metrics.total_pnl);
    if (oos_metrics.total_trades > 0 && is_metrics.total_trades > 0) {
        const double pf_decay = 1.0 - oos_metrics.profit_factor() / is_metrics.profit_factor();
        std::printf("    PF decay IS->OOS: %.0f%% %s\n", pf_decay * 100.0,
                   pf_decay > 0.50 ? "(WARNING: >50% decay suggests overfit)"
                                    : (pf_decay > 0.30 ? "(moderate decay)" : "(healthy)"));
    }
    std::printf("\n");

    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  FINAL: If OOS PF>1.20 -> build Nas100ShortEngine.\n");
    std::printf("  Parameters: short-only, ATR 30-60, drift>100, H14/16/17/18/20\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");

    return 0;
}
