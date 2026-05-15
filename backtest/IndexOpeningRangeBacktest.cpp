// =============================================================================
// IndexOpeningRangeBacktest.cpp
// =============================================================================
// Standalone backtest harness for OpeningRangeEngine (first-N-minutes range
// breakout) on European index tick data (GER40, UK100).
//
// No external dependencies — single self-contained .cpp.
//
// COMPILATION
// -----------
//   clang++ -std=c++17 -O3 -o backtest/idx_orb_bt backtest/IndexOpeningRangeBacktest.cpp
//
// USAGE
// -----
//   ./backtest/idx_orb_bt --instrument GER40 ~/Tick/GER40/DEUIDXEUR_Ticks_*.csv
//   ./backtest/idx_orb_bt --instrument UK100 ~/Tick/GBRIDXGBP/GBRIDXGBP_Ticks_*.csv
//
// TICK FORMAT AUTO-DETECTION
// --------------------------
//   HISTDATA:     YYYYMMDD HHMMSSmmm,bid,ask,0         (UTC)
//   DUKA_BID_ASK: timestamp_ms,bid,ask                  (UTC)
//   DUKA_ASK_BID: timestamp_ms,ask,bid                  (UTC)
//   JFOREX:       DD.MM.YYYY HH:MM:SS.mmm,Ask,Bid,...  (EET, UTC+2 fixed)
//
// STRATEGY
// --------
//   Opening range breakout: build high/low range during first N minutes after
//   exchange open, then trade the breakout with buffered entry, fixed TP/SL
//   as percentages, and CrossPosition-style tiered manage (BE lock, mid-lock,
//   trail). One-shot per day (armed flag).
//
// SWEEP
// -----
//   81-config sweep: RANGE_WINDOW_MIN x BUFFER_PCT x TP_PCT x SL_PCT
//   One-line per config. Full diagnostics for best OOS.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <climits>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <fstream>

// =============================================================================
// INSTRUMENT CONFIG
// =============================================================================
struct InstrumentConfig {
    const char* name;
    int    open_hour;        // UTC hour of exchange open
    int    open_min;         // UTC minute of exchange open
    double lot_size;
    double pnl_per_pt;
    double price_lo;         // sanity floor
    double price_hi;         // sanity ceiling
    int    default_range_min; // default range window minutes
};

static const InstrumentConfig INSTRUMENTS[] = {
    { "GER40", 8, 0, 0.01, 0.10, 12000.0, 25000.0, 30 },
    { "UK100", 8, 0, 0.01, 0.10,  5000.0, 12000.0, 15 },
};
static constexpr int N_INSTRUMENTS = 2;

static const InstrumentConfig* find_instrument(const char* name) {
    for (int i = 0; i < N_INSTRUMENTS; ++i)
        if (std::strcmp(INSTRUMENTS[i].name, name) == 0) return &INSTRUMENTS[i];
    return nullptr;
}

// =============================================================================
// SWEEP PARAMETERS
// =============================================================================
static const int    RANGE_WINDOW_VALS[] = { 15, 30, 45 };
static const double BUFFER_PCT_VALS[]   = { 0.02, 0.03, 0.05 };
static const double TP_PCT_VALS[]       = { 0.08, 0.10, 0.15 };
static const double SL_PCT_VALS[]       = { 0.04, 0.06, 0.08 };
static constexpr int N_RW = 3, N_BUF = 3, N_TP = 3, N_SL = 3;
static constexpr int N_CONFIGS = N_RW * N_BUF * N_TP * N_SL; // 81

struct SweepConfig {
    int    range_window_min;
    double buffer_pct;
    double tp_pct;
    double sl_pct;
};

// Manage thresholds (CrossPosition-style, fixed)
static constexpr double BE_LOCK_FRAC   = 0.40;  // 40% of TP dist -> SL = entry + spread
static constexpr double MID_LOCK_FRAC  = 0.50;  // 50% -> SL = entry + 25% of TP dist
static constexpr double TRAIL_FRAC     = 0.60;  // 60% -> trail 20% of TP dist behind MFE
static constexpr double MID_LOCK_SL_FRAC = 0.25; // SL offset for mid-lock
static constexpr double TRAIL_DIST_FRAC  = 0.20; // trail distance as frac of TP dist

// Timeout
static constexpr int64_t MAX_HOLD_SEC_LOSER = 3600; // 1 hour for losers
// Winners: no timeout (INT_MAX)

// =============================================================================
// TICK STRUCT + CSV PARSING
// =============================================================================
struct Tick {
    int64_t timestamp_ms = 0;
    double  bid          = 0.0;
    double  ask          = 0.0;
};

enum class CsvFormat { HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX, HARNESS, UNKNOWN };

// ---- Epoch helpers ----
static int64_t ymd_hms_to_epoch_ms(int year, int month, int day,
                                    int hour, int minute, int second, int ms) {
    // Reference: 2025-01-01 00:00:00 UTC = 1735689600
    static constexpr int64_t EPOCH_2025 = 1735689600LL * 1000LL;
    static constexpr int DAYS_IN_MONTH[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

    auto is_leap = [](int y) { return (y%4==0 && (y%100!=0 || y%400==0)); };
    int days = 0;
    if (year >= 2025) {
        for (int y = 2025; y < year; ++y) days += is_leap(y) ? 366 : 365;
    } else {
        for (int y = year; y < 2025; ++y) days -= is_leap(y) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) {
        days += DAYS_IN_MONTH[m];
        if (m == 2 && is_leap(year)) days += 1;
    }
    days += (day - 1);

    return EPOCH_2025 + (int64_t)days * 86400000LL
         + (int64_t)hour * 3600000LL
         + (int64_t)minute * 60000LL
         + (int64_t)second * 1000LL
         + (int64_t)ms;
}

// HISTDATA: "20240101 180000178,4770.867000,4771.384000,0"
static bool parse_histdata(const std::string& line, Tick& t) {
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

    const int year   = date_int / 10000;
    const int month  = (date_int / 100) % 100;
    const int day    = date_int % 100;
    const int hour   = time_int / 10000000;
    const int minute = (time_int / 100000) % 100;
    const int second = (time_int / 1000) % 100;
    const int ms     = time_int % 1000;

    t.timestamp_ms = ymd_hms_to_epoch_ms(year, month, day, hour, minute, second, ms);
    t.bid = bid_val;
    t.ask = ask_val;
    return true;
}

// DUKA_BID_ASK: "timestamp_ms,bid,ask[,...]"
static bool parse_duka_bid_ask(const std::string& line, Tick& t) {
    return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                       (long long*)&t.timestamp_ms, &t.bid, &t.ask) == 3;
}

// DUKA_ASK_BID: "timestamp_ms,ask,bid[,...]"
static bool parse_duka_ask_bid(const std::string& line, Tick& t) {
    double ask_val = 0, bid_val = 0;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf",
                    (long long*)&t.timestamp_ms, &ask_val, &bid_val) == 3) {
        t.bid = bid_val; t.ask = ask_val; return true;
    }
    return false;
}

// JFOREX: "2025.01.02 01:02:14.945,19857.645,19849.355,0.00001,0.00002"
// Header: "Time (EET),Ask,Bid,AskVolume,BidVolume"
// EET = UTC+2 fixed offset. Subtract 2 hours.
static bool parse_jforex(const std::string& line, Tick& t) {
    int year, month, day, hour, minute, second, ms;
    double ask_val, bid_val;
    if (std::sscanf(line.c_str(), "%4d.%2d.%2d %2d:%2d:%2d.%3d,%lf,%lf",
                    &year, &month, &day, &hour, &minute, &second, &ms,
                    &ask_val, &bid_val) == 9) {
        // Convert EET (UTC+2) to UTC: subtract 2 hours
        hour -= 2;
        int day_adj = 0;
        if (hour < 0) { hour += 24; day_adj = -1; }
        // Compute epoch then adjust day
        t.timestamp_ms = ymd_hms_to_epoch_ms(year, month, day, hour, minute, second, ms)
                       + (int64_t)day_adj * 86400000LL;
        t.bid = bid_val;
        t.ask = ask_val;
        return true;
    }
    return false;
}

static CsvFormat detect_format(const std::string& header) {
    if (header.find("Time (EET)") != std::string::npos) return CsvFormat::JFOREX;

    // HISTDATA: starts with 8 digits then space
    if (header.size() > 20 && header[8] == ' ') {
        bool all_digits = true;
        for (int i = 0; i < 8; ++i)
            if (header[i] < '0' || header[i] > '9') all_digits = false;
        if (all_digits) return CsvFormat::HISTDATA;
    }

    if (header.find("timestamp_ms,bid,ask") != std::string::npos ||
        header.find("timestamp,bidPrice,askPrice") != std::string::npos)
        return CsvFormat::DUKA_BID_ASK;
    if (header.find("timestamp_ms,ask,bid") != std::string::npos ||
        header.find("timestamp,askPrice,bidPrice") != std::string::npos)
        return CsvFormat::DUKA_ASK_BID;

    // HARNESS: "2025.01.02,01:02:14.945,19849.355,19857.645,0.00003"
    // Starts with YYYY.MM.DD, (comma after date, not space)
    if (header.size() > 20 && header[4] == '.' && header[7] == '.' && header[10] == ',')
        return CsvFormat::HARNESS;

    return CsvFormat::UNKNOWN;
}

// HARNESS: "2025.01.02,01:02:14.945,19849.355,19857.645,0.00003"
// Fields: date, time, BID, ASK, vol  (EET timezone like JForex)
static bool parse_harness(const std::string& line, Tick& t) {
    int year, month, day, hour, minute, second, ms;
    double bid_val, ask_val;
    if (std::sscanf(line.c_str(), "%4d.%2d.%2d,%2d:%2d:%2d.%3d,%lf,%lf",
                    &year, &month, &day, &hour, &minute, &second, &ms,
                    &bid_val, &ask_val) == 9) {
        // EET to UTC: subtract 2 hours
        hour -= 2;
        int day_adj = 0;
        if (hour < 0) { hour += 24; day_adj = -1; }
        t.timestamp_ms = ymd_hms_to_epoch_ms(year, month, day, hour, minute, second, ms)
                       + (int64_t)day_adj * 86400000LL;
        t.bid = bid_val;
        t.ask = ask_val;
        return true;
    }
    return false;
}

static const char* format_name(CsvFormat fmt) {
    switch (fmt) {
        case CsvFormat::HISTDATA:     return "HISTDATA";
        case CsvFormat::DUKA_BID_ASK: return "DUKA_BID_ASK";
        case CsvFormat::DUKA_ASK_BID: return "DUKA_ASK_BID";
        case CsvFormat::JFOREX:       return "JFOREX";
        case CsvFormat::HARNESS:      return "HARNESS";
        default: return "UNKNOWN";
    }
}

static bool parse_line(CsvFormat fmt, const std::string& line, Tick& t) {
    switch (fmt) {
        case CsvFormat::HISTDATA:     return parse_histdata(line, t);
        case CsvFormat::DUKA_BID_ASK: return parse_duka_bid_ask(line, t);
        case CsvFormat::DUKA_ASK_BID: return parse_duka_ask_bid(line, t);
        case CsvFormat::JFOREX:       return parse_jforex(line, t);
        case CsvFormat::HARNESS:      return parse_harness(line, t);
        default: return false;
    }
}

// =============================================================================
// WEEKEND GATE
// =============================================================================
static bool is_weekend(int64_t ts_ms) noexcept {
    const int64_t sec = ts_ms / 1000;
    const int dow = (int)(((sec / 86400) + 4) % 7); // 0=Sun..6=Sat
    if (dow == 0 || dow == 6) return true;           // Sat, Sun
    if (dow == 5) {                                   // Fri after 21:00 UTC
        const int hour = (int)((sec % 86400) / 3600);
        if (hour >= 21) return true;
    }
    return false;
}

// =============================================================================
// UTC DECOMPOSITION
// =============================================================================
struct UtcTime {
    int year, month, day, hour, minute, second;
    int yday; // day of year (0-based)
};

static UtcTime ms_to_utc(int64_t ts_ms) {
    const time_t sec = (time_t)(ts_ms / 1000);
    struct tm t;
#ifdef _WIN32
    gmtime_s(&t, &sec);
#else
    gmtime_r(&sec, &t);
#endif
    return { t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, t.tm_yday };
}

// =============================================================================
// TRADE + METRICS
// =============================================================================
enum class ExitReason { NONE, SL_HIT, TP_HIT, TRAIL, TIMEOUT };

struct SimTrade {
    bool    active   = false;
    bool    is_long  = true;
    double  entry_px = 0.0;
    double  sl_px    = 0.0;
    double  tp_px    = 0.0;
    double  spread_at_entry = 0.0;
    double  tp_dist  = 0.0;   // absolute TP distance from entry
    int64_t entry_time = 0;
    double  mfe      = 0.0;   // max favorable excursion (pts)

    // Manage state
    bool    be_locked   = false;
    bool    mid_locked  = false;
    bool    trailing    = false;
};

struct Metrics {
    int    trades      = 0;
    int    wins        = 0;
    int    losses      = 0;
    double total_pnl   = 0.0;
    double gross_wins  = 0.0;
    double gross_losses = 0.0;
    double peak_equity = 0.0;
    double max_dd      = 0.0;
    std::vector<double> pnl_vec;

    // Exit breakdown
    int sl_exits   = 0;
    int tp_exits   = 0;
    int trail_exits = 0;
    int timeout_exits = 0;

    void record(double pnl, ExitReason reason) {
        trades++;
        total_pnl += pnl;
        pnl_vec.push_back(pnl);
        if (pnl > 0) { wins++; gross_wins += pnl; }
        else         { losses++; gross_losses += std::fabs(pnl); }
        if (total_pnl > peak_equity) peak_equity = total_pnl;
        double dd = peak_equity - total_pnl;
        if (dd > max_dd) max_dd = dd;
        switch (reason) {
            case ExitReason::SL_HIT:  sl_exits++; break;
            case ExitReason::TP_HIT:  tp_exits++; break;
            case ExitReason::TRAIL:   trail_exits++; break;
            case ExitReason::TIMEOUT: timeout_exits++; break;
            default: break;
        }
    }

    double wr()  const { return trades > 0 ? (double)wins / trades : 0.0; }
    double pf()  const { return gross_losses > 0.001 ? gross_wins / gross_losses : (gross_wins > 0 ? 999.0 : 0.0); }
    double avg() const { return trades > 0 ? total_pnl / trades : 0.0; }
    double avg_win()  const { return wins > 0 ? gross_wins / wins : 0.0; }
    double avg_loss() const { return losses > 0 ? gross_losses / losses : 0.0; }

    double sharpe() const {
        if (pnl_vec.size() < 2) return 0.0;
        double mean = total_pnl / pnl_vec.size();
        double var = 0.0;
        for (double p : pnl_vec) var += (p - mean) * (p - mean);
        var /= (pnl_vec.size() - 1);
        double sd = std::sqrt(var);
        return sd > 0 ? (mean / sd) * std::sqrt(250.0 * 20.0) : 0.0;
    }
};

// =============================================================================
// PER-HOUR METRICS (for best-config report)
// =============================================================================
struct HourMetrics {
    std::array<Metrics, 24> hours;
};

// =============================================================================
// SINGLE-CONFIG ENGINE STATE
// =============================================================================
struct EngineState {
    // Range tracking (reset daily)
    double  range_high = 0.0;
    double  range_low  = 0.0;
    bool    armed      = false; // true once entry taken (or day skipped)
    int     last_yday  = -1;

    // Open trade
    SimTrade trade;

    // Aggregate metrics
    Metrics is_metrics;     // in-sample
    Metrics oos_metrics;    // out-of-sample
    Metrics overall;
    Metrics long_m, short_m;
};

// =============================================================================
// LOAD ALL TICKS
// =============================================================================
static std::vector<Tick> load_ticks(const std::vector<std::string>& files,
                                     const InstrumentConfig& inst) {
    std::vector<Tick> ticks;
    ticks.reserve(50000000); // pre-alloc for speed

    for (const auto& path : files) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "  WARNING: cannot open %s\n", path.c_str());
            continue;
        }

        std::string first_line;
        std::getline(f, first_line);
        CsvFormat fmt = detect_format(first_line);
        std::printf("  %s: format=%s", path.c_str(), format_name(fmt));

        if (fmt == CsvFormat::UNKNOWN) {
            std::printf(" -> SKIPPED (unknown format)\n");
            continue;
        }

        // HISTDATA first line IS data; JFOREX/DUKA have headers
        bool first_is_data = (fmt == CsvFormat::HISTDATA);

        size_t file_ticks = 0;
        auto process = [&](const std::string& line) {
            if (line.empty()) return;
            Tick t{};
            if (!parse_line(fmt, line, t)) return;
            if (t.bid <= 0 || t.ask <= 0 || t.ask < t.bid) return;
            double mid = (t.bid + t.ask) * 0.5;
            if (mid < inst.price_lo || mid > inst.price_hi) return;
            ticks.push_back(t);
            file_ticks++;
        };

        if (first_is_data) process(first_line);
        std::string line;
        while (std::getline(f, line)) process(line);
        std::printf(" -> %zu ticks\n", file_ticks);
    }

    // Sort by timestamp
    std::sort(ticks.begin(), ticks.end(),
              [](const Tick& a, const Tick& b) { return a.timestamp_ms < b.timestamp_ms; });

    return ticks;
}

// =============================================================================
// RUN ONE CONFIG
// =============================================================================
struct ConfigResult {
    SweepConfig cfg;
    Metrics is_m, oos_m, overall_m;
    Metrics long_m, short_m;
    // Exit breakdown (overall)
    int sl_exits = 0, tp_exits = 0, trail_exits = 0, timeout_exits = 0;
    // Per-hour (overall)
    std::array<Metrics, 24> hour_m;
};

static ConfigResult run_config(const std::vector<Tick>& ticks,
                                const InstrumentConfig& inst,
                                const SweepConfig& sc,
                                int64_t oos_split_ts) {
    ConfigResult res;
    res.cfg = sc;

    EngineState eng;
    const int open_minute_of_day = inst.open_hour * 60 + inst.open_min;
    const int range_end_minute   = open_minute_of_day + sc.range_window_min;

    for (const auto& t : ticks) {
        if (is_weekend(t.timestamp_ms)) continue;

        const double mid    = (t.bid + t.ask) * 0.5;
        const double spread = t.ask - t.bid;
        UtcTime utc = ms_to_utc(t.timestamp_ms);
        const int minute_of_day = utc.hour * 60 + utc.minute;

        // ── Daily reset ──
        if (utc.yday != eng.last_yday) {
            eng.range_high = 0.0;
            eng.range_low  = 0.0;
            eng.armed      = false;
            eng.last_yday  = utc.yday;
        }

        // ── Manage open trade ──
        if (eng.trade.active) {
            const double fav = eng.trade.is_long
                ? (mid - eng.trade.entry_px)
                : (eng.trade.entry_px - mid);
            if (fav > eng.trade.mfe) eng.trade.mfe = fav;

            const double tp_dist = eng.trade.tp_dist;

            // -- BE lock at 40% of TP distance --
            if (!eng.trade.be_locked && fav >= tp_dist * BE_LOCK_FRAC) {
                eng.trade.be_locked = true;
                double be_sl = eng.trade.entry_px;
                if (eng.trade.is_long)
                    be_sl += eng.trade.spread_at_entry; // entry + spread
                else
                    be_sl -= eng.trade.spread_at_entry;
                // Only tighten, never widen
                if (eng.trade.is_long && be_sl > eng.trade.sl_px)
                    eng.trade.sl_px = be_sl;
                else if (!eng.trade.is_long && be_sl < eng.trade.sl_px)
                    eng.trade.sl_px = be_sl;
            }

            // -- Mid-lock at 50%: SL = entry + 25% of TP dist --
            if (!eng.trade.mid_locked && fav >= tp_dist * MID_LOCK_FRAC) {
                eng.trade.mid_locked = true;
                double mid_sl;
                if (eng.trade.is_long)
                    mid_sl = eng.trade.entry_px + tp_dist * MID_LOCK_SL_FRAC;
                else
                    mid_sl = eng.trade.entry_px - tp_dist * MID_LOCK_SL_FRAC;
                if (eng.trade.is_long && mid_sl > eng.trade.sl_px)
                    eng.trade.sl_px = mid_sl;
                else if (!eng.trade.is_long && mid_sl < eng.trade.sl_px)
                    eng.trade.sl_px = mid_sl;
            }

            // -- Trail at 60%: 20% of TP dist behind MFE --
            if (fav >= tp_dist * TRAIL_FRAC) {
                eng.trade.trailing = true;
                double trail_dist = tp_dist * TRAIL_DIST_FRAC;
                double trail_sl;
                if (eng.trade.is_long)
                    trail_sl = eng.trade.entry_px + (eng.trade.mfe - trail_dist);
                else
                    trail_sl = eng.trade.entry_px - (eng.trade.mfe - trail_dist);
                if (eng.trade.is_long && trail_sl > eng.trade.sl_px)
                    eng.trade.sl_px = trail_sl;
                else if (!eng.trade.is_long && trail_sl < eng.trade.sl_px)
                    eng.trade.sl_px = trail_sl;
            }

            // -- Check exits --
            bool sl_hit = eng.trade.is_long ? (t.bid <= eng.trade.sl_px)
                                            : (t.ask >= eng.trade.sl_px);
            bool tp_hit = eng.trade.is_long ? (t.ask >= eng.trade.tp_px)
                                            : (t.bid <= eng.trade.tp_px);

            // Timeout: losers only, MAX_HOLD_SEC_LOSER
            int64_t hold_sec = (t.timestamp_ms - eng.trade.entry_time) / 1000;
            bool is_losing = (fav < 0.0);
            bool timed_out = is_losing && (hold_sec >= MAX_HOLD_SEC_LOSER);

            ExitReason reason = ExitReason::NONE;
            double exit_px = 0.0;

            if (tp_hit) {
                reason = ExitReason::TP_HIT;
                exit_px = eng.trade.tp_px;
            } else if (sl_hit) {
                reason = eng.trade.trailing ? ExitReason::TRAIL : ExitReason::SL_HIT;
                exit_px = eng.trade.sl_px;
            } else if (timed_out) {
                reason = ExitReason::TIMEOUT;
                exit_px = eng.trade.is_long ? t.bid : t.ask;
            }

            if (reason != ExitReason::NONE) {
                double pnl_pts = eng.trade.is_long
                    ? (exit_px - eng.trade.entry_px)
                    : (eng.trade.entry_px - exit_px);
                double pnl_usd = pnl_pts * inst.pnl_per_pt;

                res.overall_m.record(pnl_usd, reason);
                if (eng.trade.is_long) res.long_m.record(pnl_usd, reason);
                else                   res.short_m.record(pnl_usd, reason);

                if (eng.trade.entry_time < oos_split_ts)
                    res.is_m.record(pnl_usd, reason);
                else
                    res.oos_m.record(pnl_usd, reason);

                int entry_hour = (int)((eng.trade.entry_time / 1000 % 86400) / 3600);
                res.hour_m[entry_hour].record(pnl_usd, reason);

                eng.trade.active = false;
            }
            continue; // no new entries while in trade
        }

        // ── Range build phase ──
        if (minute_of_day >= open_minute_of_day && minute_of_day < range_end_minute) {
            if (eng.range_high == 0.0 && eng.range_low == 0.0) {
                eng.range_high = mid;
                eng.range_low  = mid;
            } else {
                if (mid > eng.range_high) eng.range_high = mid;
                if (mid < eng.range_low)  eng.range_low  = mid;
            }
            continue; // no signals during range build
        }

        // ── Breakout detection (one-shot per day) ──
        if (eng.armed) continue;
        if (eng.range_high <= 0.0) continue; // no range built today
        if (minute_of_day < range_end_minute) continue; // still in range window

        double buffer = mid * sc.buffer_pct / 100.0;

        if (mid > eng.range_high + buffer) {
            // Long breakout
            eng.armed = true;
            eng.trade.active   = true;
            eng.trade.is_long  = true;
            eng.trade.entry_px = t.ask;
            eng.trade.spread_at_entry = spread;
            eng.trade.tp_dist  = eng.trade.entry_px * sc.tp_pct / 100.0;
            eng.trade.tp_px    = eng.trade.entry_px * (1.0 + sc.tp_pct / 100.0);
            eng.trade.sl_px    = eng.trade.entry_px * (1.0 - sc.sl_pct / 100.0);
            eng.trade.entry_time = t.timestamp_ms;
            eng.trade.mfe      = 0.0;
            eng.trade.be_locked = false;
            eng.trade.mid_locked = false;
            eng.trade.trailing  = false;
        } else if (mid < eng.range_low - buffer) {
            // Short breakout
            eng.armed = true;
            eng.trade.active   = true;
            eng.trade.is_long  = false;
            eng.trade.entry_px = t.bid;
            eng.trade.spread_at_entry = spread;
            eng.trade.tp_dist  = eng.trade.entry_px * sc.tp_pct / 100.0;
            eng.trade.tp_px    = eng.trade.entry_px * (1.0 - sc.tp_pct / 100.0);
            eng.trade.sl_px    = eng.trade.entry_px * (1.0 + sc.sl_pct / 100.0);
            eng.trade.entry_time = t.timestamp_ms;
            eng.trade.mfe      = 0.0;
            eng.trade.be_locked = false;
            eng.trade.mid_locked = false;
            eng.trade.trailing  = false;
        }
    }

    // Summarize exit breakdown
    res.sl_exits      = res.overall_m.sl_exits;
    res.tp_exits      = res.overall_m.tp_exits;
    res.trail_exits   = res.overall_m.trail_exits;
    res.timeout_exits = res.overall_m.timeout_exits;

    return res;
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: %s --instrument <GER40|UK100> <tick_csv1> [tick_csv2 ...]\n",
            argv[0]);
        return 1;
    }

    // Parse args
    const char* instrument_name = nullptr;
    std::vector<std::string> csv_files;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            instrument_name = argv[++i];
        } else if (argv[i][0] != '-') {
            csv_files.push_back(argv[i]);
        }
    }

    if (!instrument_name) {
        std::fprintf(stderr, "ERROR: --instrument required (GER40 or UK100)\n");
        return 1;
    }
    const InstrumentConfig* inst = find_instrument(instrument_name);
    if (!inst) {
        std::fprintf(stderr, "ERROR: unknown instrument '%s' (use GER40 or UK100)\n",
                     instrument_name);
        return 1;
    }
    if (csv_files.empty()) {
        std::fprintf(stderr, "ERROR: no tick CSV files specified\n");
        return 1;
    }

    std::sort(csv_files.begin(), csv_files.end());

    std::printf("=================================================================\n");
    std::printf("  IndexOpeningRangeBacktest — %s\n", inst->name);
    std::printf("=================================================================\n");
    std::printf("  Open: %02d:%02d UTC  |  Lot: %.2f  |  PnL/pt: $%.2f\n",
               inst->open_hour, inst->open_min, inst->lot_size, inst->pnl_per_pt);
    std::printf("  Price sanity: %.0f - %.0f\n", inst->price_lo, inst->price_hi);
    std::printf("  Sweep: %d configs (RW x BUF x TP x SL = %dx%dx%dx%d)\n",
               N_CONFIGS, N_RW, N_BUF, N_TP, N_SL);
    std::printf("  Manage: BE@%.0f%% Mid@%.0f%% Trail@%.0f%% (of TP dist)\n",
               BE_LOCK_FRAC*100, MID_LOCK_FRAC*100, TRAIL_FRAC*100);
    std::printf("  Timeout: %llds losers, unlimited winners\n\n",
               (long long)MAX_HOLD_SEC_LOSER);

    // ── Load ticks ──
    std::printf("[LOAD] Reading tick data from %zu file(s)...\n", csv_files.size());
    std::vector<Tick> ticks = load_ticks(csv_files, *inst);

    if (ticks.empty()) {
        std::fprintf(stderr, "ERROR: no valid ticks loaded\n");
        return 1;
    }

    int64_t first_ts = ticks.front().timestamp_ms;
    int64_t last_ts  = ticks.back().timestamp_ms;
    double hours = (last_ts - first_ts) / 3600000.0;
    double days  = hours / 24.0;

    std::printf("[LOAD] %zu ticks  |  %.1f days  |  %s to %s\n",
               ticks.size(), days,
               [](int64_t ts) -> const char* {
                   static char buf[32];
                   UtcTime u = ms_to_utc(ts);
                   std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                                u.year, u.month, u.day);
                   return buf;
               }(first_ts),
               [](int64_t ts) -> const char* {
                   static char buf[32];
                   UtcTime u = ms_to_utc(ts);
                   std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                                u.year, u.month, u.day);
                   return buf;
               }(last_ts));

    // IS/OOS split: 60/40 by time
    int64_t oos_split_ts = first_ts + (int64_t)(days * 0.60 * 24.0 * 3600.0 * 1000.0);
    UtcTime split_utc = ms_to_utc(oos_split_ts);
    std::printf("[SPLIT] IS: first 60%% (to %04d-%02d-%02d)  |  OOS: last 40%%\n\n",
               split_utc.year, split_utc.month, split_utc.day);

    // ── Run sweep ──
    std::printf("[SWEEP] Running %d configs...\n", N_CONFIGS);

    std::vector<ConfigResult> results;
    results.reserve(N_CONFIGS);

    int config_idx = 0;
    size_t progress_threshold = 10000000;

    for (int rw = 0; rw < N_RW; ++rw)
    for (int bf = 0; bf < N_BUF; ++bf)
    for (int tp = 0; tp < N_TP; ++tp)
    for (int sl = 0; sl < N_SL; ++sl) {
        SweepConfig sc;
        sc.range_window_min = RANGE_WINDOW_VALS[rw];
        sc.buffer_pct       = BUFFER_PCT_VALS[bf];
        sc.tp_pct           = TP_PCT_VALS[tp];
        sc.sl_pct           = SL_PCT_VALS[sl];

        ConfigResult res = run_config(ticks, *inst, sc, oos_split_ts);
        results.push_back(res);
        config_idx++;

        if (config_idx % 10 == 0 || config_idx == N_CONFIGS) {
            std::printf("\r  [%d/%d configs]", config_idx, N_CONFIGS);
            std::fflush(stdout);
        }
    }
    std::printf("\n\n");

    // ── One-liner per config ──
    std::printf("=================================================================\n");
    std::printf("  SWEEP RESULTS — %s  (%d configs)\n", inst->name, N_CONFIGS);
    std::printf("=================================================================\n\n");

    std::printf("  RW  BUF%%   TP%%   SL%%   IS_tr IS_PF  IS_WR%%  OOS_tr OOS_PF OOS_WR%%  OOS_PnL   OOS_DD   verdict\n");
    std::printf("  --- ----- ----- -----  ----- ------ ------  ------ ------ ------  --------  -------  -------\n");

    for (const auto& r : results) {
        const char* verdict = (r.oos_m.pf() >= 1.20 && r.oos_m.trades >= 20) ? "PASS" : "FAIL";
        std::printf("  %2d  %.2f  %.2f  %.2f   %4d  %5.2f  %5.1f   %4d  %5.2f  %5.1f   $%7.2f  $%6.2f   %s\n",
                   r.cfg.range_window_min,
                   r.cfg.buffer_pct,
                   r.cfg.tp_pct,
                   r.cfg.sl_pct,
                   r.is_m.trades,
                   r.is_m.pf(),
                   r.is_m.wr() * 100.0,
                   r.oos_m.trades,
                   r.oos_m.pf(),
                   r.oos_m.wr() * 100.0,
                   r.oos_m.total_pnl,
                   r.oos_m.max_dd,
                   verdict);
    }

    // ── Find best OOS config ──
    int best_idx = -1;
    double best_pf = 0.0;
    for (int i = 0; i < (int)results.size(); ++i) {
        const auto& r = results[i];
        if (r.oos_m.trades >= 20 && r.oos_m.pf() > best_pf) {
            best_pf = r.oos_m.pf();
            best_idx = i;
        }
    }

    if (best_idx < 0) {
        // Fallback: best by OOS PnL
        double best_pnl = -1e18;
        for (int i = 0; i < (int)results.size(); ++i) {
            if (results[i].oos_m.trades >= 5 && results[i].oos_m.total_pnl > best_pnl) {
                best_pnl = results[i].oos_m.total_pnl;
                best_idx = i;
            }
        }
    }

    if (best_idx < 0) best_idx = 0; // absolute fallback

    const auto& best = results[best_idx];

    std::printf("\n");
    std::printf("=================================================================\n");
    std::printf("  BEST OOS CONFIG — %s\n", inst->name);
    std::printf("=================================================================\n\n");

    std::printf("  RANGE_WINDOW_MIN = %d\n", best.cfg.range_window_min);
    std::printf("  BUFFER_PCT       = %.2f\n", best.cfg.buffer_pct);
    std::printf("  TP_PCT           = %.2f\n", best.cfg.tp_pct);
    std::printf("  SL_PCT           = %.2f\n", best.cfg.sl_pct);
    std::printf("\n");

    // Overall
    std::printf("── OVERALL ─────────────────────────────────────────────────\n");
    std::printf("  trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  avg=$%.4f  maxDD=$%.2f  Sharpe=%.2f\n",
               best.overall_m.trades,
               best.overall_m.wr() * 100.0,
               best.overall_m.pf(),
               best.overall_m.total_pnl,
               best.overall_m.avg(),
               best.overall_m.max_dd,
               best.overall_m.sharpe());
    std::printf("\n");

    // Long / Short
    std::printf("── LONG / SHORT ────────────────────────────────────────────\n");
    std::printf("  LONG:  trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  avgW=$%.4f  avgL=$%.4f\n",
               best.long_m.trades,
               best.long_m.wr() * 100.0,
               best.long_m.pf(),
               best.long_m.total_pnl,
               best.long_m.avg_win(),
               best.long_m.avg_loss());
    std::printf("  SHORT: trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  avgW=$%.4f  avgL=$%.4f\n",
               best.short_m.trades,
               best.short_m.wr() * 100.0,
               best.short_m.pf(),
               best.short_m.total_pnl,
               best.short_m.avg_win(),
               best.short_m.avg_loss());
    std::printf("\n");

    // Exit breakdown
    std::printf("── EXIT BREAKDOWN ──────────────────────────────────────────\n");
    std::printf("  SL hit:    %d\n", best.sl_exits);
    std::printf("  TP hit:    %d\n", best.tp_exits);
    std::printf("  Trail:     %d\n", best.trail_exits);
    std::printf("  Timeout:   %d\n", best.timeout_exits);
    std::printf("\n");

    // Per-hour PF
    std::printf("── PER-HOUR PF (entry hour UTC) ───────────────────────────\n");
    std::printf("  Hour  Trades   WR%%    PF      PnL\n");
    for (int h = 0; h < 24; ++h) {
        const auto& hm = best.hour_m[h];
        if (hm.trades > 0) {
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%8.2f  %s\n",
                       h, hm.trades,
                       hm.wr() * 100.0,
                       hm.pf(),
                       hm.total_pnl,
                       hm.pf() >= 1.0 ? "<<<" : "");
        }
    }
    std::printf("\n");

    // IS/OOS
    std::printf("── IS / OOS VALIDATION (60/40 split) ──────────────────────\n");
    std::printf("  IS:   trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  maxDD=$%.2f  Sharpe=%.2f\n",
               best.is_m.trades,
               best.is_m.wr() * 100.0,
               best.is_m.pf(),
               best.is_m.total_pnl,
               best.is_m.max_dd,
               best.is_m.sharpe());
    std::printf("  OOS:  trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  maxDD=$%.2f  Sharpe=%.2f\n",
               best.oos_m.trades,
               best.oos_m.wr() * 100.0,
               best.oos_m.pf(),
               best.oos_m.total_pnl,
               best.oos_m.max_dd,
               best.oos_m.sharpe());

    // Decay
    if (best.is_m.trades > 0 && best.oos_m.trades > 0) {
        double is_pf  = best.is_m.pf();
        double oos_pf = best.oos_m.pf();
        double decay  = (is_pf > 0.001) ? (1.0 - oos_pf / is_pf) * 100.0 : 0.0;
        std::printf("  Decay: IS PF=%.2f -> OOS PF=%.2f  (%.1f%% decay)\n",
                   is_pf, oos_pf, decay);
    }
    std::printf("\n");

    // OOS verdict
    bool pass = (best.oos_m.pf() >= 1.20 && best.oos_m.trades >= 20);
    std::printf("=================================================================\n");
    std::printf("  OOS VERDICT: %s  (PF >= 1.20 && trades >= 20)\n",
               pass ? "PASS" : "FAIL");
    std::printf("=================================================================\n\n");

    // Count passing configs
    int pass_count = 0;
    for (const auto& r : results) {
        if (r.oos_m.pf() >= 1.20 && r.oos_m.trades >= 20) pass_count++;
    }
    std::printf("  Configs passing OOS: %d / %d (%.1f%%)\n\n",
               pass_count, N_CONFIGS, 100.0 * pass_count / N_CONFIGS);

    return 0;
}
