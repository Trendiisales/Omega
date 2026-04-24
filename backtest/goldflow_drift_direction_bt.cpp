// =============================================================================
// goldflow_drift_direction_bt.cpp
//
// PURE DRIFT-SIGNAL CORRELATION TEST -- 2026-04-24 (S18)
// S19 PATCH: dual-format parser (cTrader ms-epoch + Dukascopy YYYYMMDD,HH:MM:SS)
//
// HYPOTHESIS UNDER TEST:
//   Claim: GoldFlow's drift-continuation entry direction is anti-correlated
//   with subsequent gold M1 price movement.
//
// WHAT THIS HARNESS DOES:
//   For every tick in the 24-month CSV, compute the same EWM drift the live
//   GFE engine computes. When drift persists directionally for the same
//   GFE_DRIFT_PERSIST_TICKS window (12 ticks, >=70% directional) -- i.e. when
//   the live engine WOULD fire a signal -- we record:
//     - signal_dir: +1 (fires LONG) or -1 (fires SHORT)
//     - forward_move_Npts over the next N minutes (from mid price)
//
// WHAT THIS HARNESS DOES NOT DO:
//   - Does NOT simulate SL/TP hits (no exits, no PnL with cost)
//   - Does NOT apply L2 gates, ATR floors, Asia gap floors, spread gates
//   - Does NOT apply RSI, VWAP, overextension, or any other GFE refinements
//   - Does NOT simulate entry fills, commissions, or slippage
//
// WHY THIS TEST IS HONEST:
//   The inversion hypothesis is ONLY about signal direction correlation.
//   If drift-persistence-up predicts future-price-down more often than
//   future-price-up across 2 years of data, the live engine direction is
//   wrong regardless of L2/ATR/SL/TP refinements. Those refinements reduce
//   trade count and adjust SL geometry -- they cannot flip direction edge.
//
//   Conversely, if drift-persistence-up predicts future-price-up, the
//   direction is correct and the 7/7 live losses are regime-specific,
//   not structural. Inversion should NOT be deployed.
//
//   This is a pure correlation test of the signal's directional validity.
//   No fabrication: drift is computed from real bid/ask, forward move is
//   real price over a real window. Every input is observed data.
//
// OUTPUT:
//   For multiple forward windows (1min, 5min, 15min, 30min, 60min):
//     - Total signals generated (LONG + SHORT separately)
//     - Directional hit rate (pct of signals where forward move agrees with signal)
//     - Mean forward move per signal direction (in $)
//     - If mean forward move aligned with signal > 0: GFE direction CORRECT
//     - If mean forward move aligned with signal < 0: GFE direction INVERTED
//
// CSV FORMATS SUPPORTED (autodetected from first data row):
//
//   FORMAT A -- cTrader export (original):
//     timestamp_ms,askPrice,bidPrice
//     1735776000181,2624.15,2623.66
//     Header row (first line): present ("timestamp,askPrice,bidPrice" or similar).
//     Detection: first token parses as integer >= 1000000000000 (13-digit ms epoch).
//
//   FORMAT B -- Dukascopy export (S19 addition):
//     YYYYMMDD,HH:MM:SS,bid,ask,last,vol
//     20230927,00:00:00,1901.455,1901.745,1901.455,0
//     Header row: optional ("Gmt time,Ask,Bid,..." or absent).
//     Detection: first token is exactly 8 digits (YYYYMMDD).
//     All timestamps are assumed UTC (Dukascopy default).
//     Bid/ask column order is bid-first (cols 3,4) -- OPPOSITE of cTrader.
//     Sub-second precision: Dukascopy has none. A monotonic tie-break counter
//     (0..999) is appended to ts_ms so same-second ticks retain stable EWM
//     ordering. Counter resets on each new second.
//
// BUILD (Mac):
//   clang++ -O3 -std=c++20 -o goldflow_drift_direction_bt goldflow_drift_direction_bt.cpp
// BUILD (Linux/Windows w/ g++):
//   g++ -O3 -std=c++17 -o goldflow_drift_direction_bt goldflow_drift_direction_bt.cpp
//
// RUN:
//   ./goldflow_drift_direction_bt ~/tick/2yr_XAUUSD_tick.csv
// =============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <ctime>

// -----------------------------------------------------------------------------
// GFE constants mirrored from live GoldFlowEngine.hpp (commit 9ceae160)
// -----------------------------------------------------------------------------
static constexpr int    GFE_DRIFT_PERSIST_TICKS = 12;
static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.5;
static constexpr double GFE_ATR_MIN = 2.0;
static constexpr double GFE_MAX_SPREAD = 2.5;
static constexpr double EWM_ALPHA_FAST = 0.05;
static constexpr double EWM_ALPHA_SLOW = 0.005;
static constexpr double CHOP_RANGE_THRESHOLD = 4.0;
static constexpr int    CHOP_MIN_SIG_TICKS = GFE_DRIFT_PERSIST_TICKS / 4;  // =3
static constexpr double DRIFT_THRESH_RATIO = 0.70;  // 70% of persist window directional

// Forward windows to test (in minutes)
static const std::vector<int> FORWARD_WINDOWS_MIN = {1, 5, 15, 30, 60};

// Cooldown between signals (prevents counting the same drift episode repeatedly)
// Matches live engine's trade cooldown of 30-60 seconds. Use 60s as conservative.
static constexpr int64_t SIGNAL_COOLDOWN_MS = 60000;

// -----------------------------------------------------------------------------
// Format mode (S19)
// -----------------------------------------------------------------------------
enum class CsvFormat {
    UNKNOWN,
    CTRADER,     // ms_epoch,ask,bid
    DUKASCOPY    // YYYYMMDD,HH:MM:SS,bid,ask,last,vol
};

// -----------------------------------------------------------------------------
// Core data structures
// -----------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
    double  mid() const { return (bid + ask) * 0.5; }
    double  spread() const { return ask - bid; }
};

struct EWM {
    double fast = 0, slow = 0;
    bool   seeded = false;
    void update(double p) {
        if (!seeded) { fast = slow = p; seeded = true; return; }
        fast = EWM_ALPHA_FAST * p + (1.0 - EWM_ALPHA_FAST) * fast;
        slow = EWM_ALPHA_SLOW * p + (1.0 - EWM_ALPHA_SLOW) * slow;
    }
    double drift() const { return fast - slow; }
};

// Rudimentary rolling ATR (1-minute based). Mirrors GFE's ATR in spirit.
// Used only for the dynamic drift threshold (max(fallback, 0.18*ATR)).
struct RollingATR {
    std::deque<double> m1_ranges;   // last 100 1-minute bar ranges
    double current_high = -1e18;
    double current_low  =  1e18;
    int64_t current_bar_min = 0;
    double  atr = GFE_ATR_MIN;
    static constexpr int PERIOD = 100;

    void update(int64_t ts_ms, double mid) {
        int64_t bar_min = ts_ms / 60000;
        if (current_bar_min == 0) {
            current_bar_min = bar_min;
            current_high = mid;
            current_low  = mid;
            return;
        }
        if (bar_min != current_bar_min) {
            // Close out the completed bar
            double range = current_high - current_low;
            m1_ranges.push_back(range);
            if ((int)m1_ranges.size() > PERIOD) m1_ranges.pop_front();
            if ((int)m1_ranges.size() >= 10) {
                double sum = 0;
                for (double r : m1_ranges) sum += r;
                atr = std::max(GFE_ATR_MIN, sum / m1_ranges.size());
            }
            current_bar_min = bar_min;
            current_high = mid;
            current_low  = mid;
        } else {
            if (mid > current_high) current_high = mid;
            if (mid < current_low)  current_low  = mid;
        }
    }
};

// -----------------------------------------------------------------------------
// S19: UTC calendar -> epoch seconds (timegm equivalent, portable).
// timegm is non-standard. This avoids the TZ-env dance.
// -----------------------------------------------------------------------------
static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static int days_in_month(int y, int m) {
    static const int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && is_leap(y)) return 29;
    return dm[m - 1];
}
static int64_t utc_to_epoch_sec(int year, int mon, int day,
                                int hour, int minute, int sec) {
    // Reject out-of-range before iterating
    if (year < 1970 || year > 2099) return -1;
    if (mon < 1 || mon > 12) return -1;
    if (day < 1 || day > days_in_month(year, mon)) return -1;
    if (hour < 0 || hour > 23) return -1;
    if (minute < 0 || minute > 59) return -1;
    if (sec < 0 || sec > 60) return -1;  // allow leap sec

    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += is_leap(y) ? 366 : 365;
    for (int m = 1; m < mon; ++m)      days += days_in_month(year, m);
    days += (day - 1);
    return days * 86400LL
         + (int64_t)hour * 3600
         + (int64_t)minute * 60
         + sec;
}

// -----------------------------------------------------------------------------
// S19: Sub-second tie-break state for Dukascopy (preserves EWM ordering).
// Same-second ticks get ms = second*1000 + 0,1,2,... counter (capped at 999).
// Resets each new second.
// -----------------------------------------------------------------------------
struct DukaTieBreak {
    int64_t last_sec = -1;
    int     counter  = 0;

    int64_t next_ms(int64_t sec_epoch) {
        if (sec_epoch != last_sec) {
            last_sec = sec_epoch;
            counter = 0;
        } else if (counter < 999) {
            ++counter;
        }
        // If >999 ticks in one second (vanishingly rare in Dukascopy gold),
        // they collide at 999. Acceptable: caller's EWM still advances on value,
        // only tie-break ordering is clipped.
        return sec_epoch * 1000LL + counter;
    }
};

// -----------------------------------------------------------------------------
// S19: Detect format from first data line (not header).
// Rule:
//   - If first comma-separated token is exactly 8 digits AND parses in
//     reasonable calendar range -> DUKASCOPY.
//   - Else if first token parses as int64 >= 1e12 (13-digit ms epoch) -> CTRADER.
//   - Else UNKNOWN.
// -----------------------------------------------------------------------------
static CsvFormat detect_format(const std::string& sample_line) {
    if (sample_line.empty()) return CsvFormat::UNKNOWN;
    size_t comma = sample_line.find(',');
    if (comma == std::string::npos) return CsvFormat::UNKNOWN;

    std::string tok0 = sample_line.substr(0, comma);

    // Strip trailing whitespace
    while (!tok0.empty() && std::isspace((unsigned char)tok0.back())) tok0.pop_back();

    // Check all-digit
    if (tok0.empty()) return CsvFormat::UNKNOWN;
    for (char c : tok0) if (!std::isdigit((unsigned char)c)) return CsvFormat::UNKNOWN;

    if (tok0.size() == 8) {
        // YYYYMMDD candidate
        int y = std::atoi(tok0.substr(0, 4).c_str());
        int m = std::atoi(tok0.substr(4, 2).c_str());
        int d = std::atoi(tok0.substr(6, 2).c_str());
        if (y >= 1970 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            return CsvFormat::DUKASCOPY;
        }
        return CsvFormat::UNKNOWN;
    }

    if (tok0.size() >= 12) {
        // ms_epoch candidate (13 digits for 2001+, 12 for 1970s)
        int64_t v = std::strtoll(tok0.c_str(), nullptr, 10);
        if (v >= 1000000000000LL) return CsvFormat::CTRADER;
    }

    return CsvFormat::UNKNOWN;
}

// -----------------------------------------------------------------------------
// Parse one CSV line (cTrader). Format: timestamp_ms,askPrice,bidPrice
// -----------------------------------------------------------------------------
static bool parse_tick_ctrader(const std::string& line, Tick& t) {
    if (line.empty()) return false;
    if (!std::isdigit((unsigned char)line[0])) return false;  // skip header / bad row
    const char* p = line.c_str();
    char* end = nullptr;

    // ts_ms
    t.ts_ms = std::strtoll(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;

    // ask
    t.ask = std::strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;

    // bid
    t.bid = std::strtod(p, &end);
    if (end == p) return false;

    if (t.bid <= 0 || t.ask <= 0 || t.ask < t.bid) return false;
    if (t.ask - t.bid > GFE_MAX_SPREAD) return false;
    return true;
}

// -----------------------------------------------------------------------------
// S19: Parse one CSV line (Dukascopy). Format:
//   YYYYMMDD,HH:MM:SS,bid,ask,last,vol
//   20230927,00:00:00,1901.455,1901.745,1901.455,0
// Column order: DATE, TIME, BID, ASK, LAST, VOL (bid before ask -- cTrader is reversed).
// Timezone: UTC (Dukascopy default).
// Sub-second precision: none -- tie-break counter appended.
// -----------------------------------------------------------------------------
static bool parse_tick_dukascopy(const std::string& line, Tick& t, DukaTieBreak& tb) {
    if (line.empty()) return false;
    if (!std::isdigit((unsigned char)line[0])) return false;  // skip header / bad row
    if (line.size() < 20) return false;

    // Field 1: YYYYMMDD (positions 0..7), comma at 8
    if (line[8] != ',') return false;
    for (int i = 0; i < 8; ++i) if (!std::isdigit((unsigned char)line[i])) return false;

    int year  = (line[0]-'0')*1000 + (line[1]-'0')*100 + (line[2]-'0')*10 + (line[3]-'0');
    int month = (line[4]-'0')*10 + (line[5]-'0');
    int day   = (line[6]-'0')*10 + (line[7]-'0');

    // Field 2: HH:MM:SS (positions 9,10,':',12,13,':',15,16), comma at 17
    if (line[11] != ':' || line[14] != ':' || line[17] != ',') return false;
    for (int i : {9,10,12,13,15,16}) if (!std::isdigit((unsigned char)line[i])) return false;

    int hour   = (line[9]-'0')*10  + (line[10]-'0');
    int minute = (line[12]-'0')*10 + (line[13]-'0');
    int sec    = (line[15]-'0')*10 + (line[16]-'0');

    int64_t ep_sec = utc_to_epoch_sec(year, month, day, hour, minute, sec);
    if (ep_sec < 0) return false;

    // Field 3 (bid), Field 4 (ask) -- start scanning at char 18
    const char* p = line.c_str() + 18;
    char* end = nullptr;

    double bid = std::strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;

    double ask = std::strtod(p, &end);
    // trailing char may be ',' (more cols follow) OR end-of-string/CR/LF
    if (end == p) return false;
    if (*end != ',' && *end != '\0' && *end != '\r' && *end != '\n') return false;

    if (bid <= 0 || ask <= 0 || ask < bid) return false;
    if (ask - bid > GFE_MAX_SPREAD) return false;

    t.ts_ms = tb.next_ms(ep_sec);
    t.bid   = bid;
    t.ask   = ask;
    return true;
}

// -----------------------------------------------------------------------------
// Signal record: every time the engine WOULD have fired
// -----------------------------------------------------------------------------
struct Signal {
    int64_t ts_ms;
    double  mid_at_signal;
    int     dir;           // +1 LONG, -1 SHORT
    // Forward moves filled in on the second pass
    std::vector<double> forward_moves;  // one per FORWARD_WINDOWS_MIN
};

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: goldflow_drift_direction_bt <tick_csv> [--max N]\n";
        return 1;
    }
    const std::string path = argv[1];

    // Optional cap for testing
    int64_t max_ticks = 0;
    if (argc >= 4 && std::string(argv[2]) == "--max") {
        max_ticks = std::atoll(argv[3]);
    }

    std::ifstream f(path);
    if (!f) {
        std::cerr << "Cannot open: " << path << "\n";
        return 1;
    }

    std::cerr << "Loading: " << path << "\n";
    if (max_ticks > 0) std::cerr << "Max ticks: " << max_ticks << "\n";

    // ---------------------------------------------------------------------
    // S19: Format detection
    //   Peek first non-empty line. If it's a header (non-digit start),
    //   consume it and peek the next line. Detect format from that sample.
    //   File position left at the start of the first data line.
    // ---------------------------------------------------------------------
    CsvFormat fmt = CsvFormat::UNKNOWN;
    std::string first_line;
    std::streampos data_start_pos = f.tellg();

    if (!std::getline(f, first_line)) {
        std::cerr << "Empty file\n";
        return 1;
    }
    if (!first_line.empty() && first_line.back() == '\r') first_line.pop_back();

    bool first_is_header = first_line.empty() ||
                           !std::isdigit((unsigned char)first_line[0]);

    if (first_is_header) {
        std::cerr << "Header detected: " << first_line << "\n";
        data_start_pos = f.tellg();   // data starts after the header
        std::string second_line;
        if (!std::getline(f, second_line)) {
            std::cerr << "No data rows after header\n";
            return 1;
        }
        if (!second_line.empty() && second_line.back() == '\r') second_line.pop_back();
        fmt = detect_format(second_line);
    } else {
        // No header -- first_line IS the first data row
        fmt = detect_format(first_line);
    }

    if (fmt == CsvFormat::UNKNOWN) {
        std::cerr << "Could not detect CSV format from sample line.\n";
        std::cerr << "Expected cTrader (ms_epoch,ask,bid) or Dukascopy (YYYYMMDD,HH:MM:SS,bid,ask,last,vol).\n";
        return 1;
    }

    std::cerr << "Format: " << (fmt == CsvFormat::CTRADER ? "cTrader (ts_ms,ask,bid)"
                                                          : "Dukascopy (YYYYMMDD,HH:MM:SS,bid,ask,last,vol) [UTC]")
              << "\n";

    // Rewind to start of data section
    f.clear();
    f.seekg(data_start_pos);

    // ---------------------------------------------------------------------
    // Pass 1: stream ticks, compute drift, record every signal firing time
    // ---------------------------------------------------------------------
    std::vector<Tick> all_ticks;  // we need these for forward-move lookup
    all_ticks.reserve(150000000);

    EWM ewm;
    RollingATR atr;
    DukaTieBreak duka_tb;
    std::deque<int> drift_persist_window;
    std::deque<double> drift_val_window;
    std::vector<Signal> signals;
    int64_t last_signal_ts = 0;

    std::string line;
    int64_t tick_count = 0;
    int64_t skip_count = 0;
    auto t0 = std::chrono::steady_clock::now();

    while (std::getline(f, line)) {
        // Trim \r (Windows line endings in VPS-sourced CSVs)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        Tick t;
        bool ok = false;
        if (fmt == CsvFormat::CTRADER) {
            ok = parse_tick_ctrader(line, t);
        } else {
            ok = parse_tick_dukascopy(line, t, duka_tb);
        }
        if (!ok) { skip_count++; continue; }
        all_ticks.push_back(t);
        tick_count++;

        // Update EWM + ATR
        ewm.update(t.mid());
        atr.update(t.ts_ms, t.mid());

        // Dynamic drift threshold -- mirror GFE line 563-565
        const double eff_drift_threshold = std::max(
            GFE_DRIFT_FALLBACK_THRESHOLD,
            0.18 * atr.atr
        );

        // Classify this tick's drift
        const double d = ewm.drift();
        const int drift_dir = (d > eff_drift_threshold) ? 1
                            : (d < -eff_drift_threshold) ? -1
                            : 0;
        drift_persist_window.push_back(drift_dir);
        drift_val_window.push_back(d);
        if ((int)drift_persist_window.size() > GFE_DRIFT_PERSIST_TICKS)
            drift_persist_window.pop_front();
        if ((int)drift_val_window.size() > GFE_DRIFT_PERSIST_TICKS)
            drift_val_window.pop_front();

        // Need a full window before we can signal
        if ((int)drift_persist_window.size() < GFE_DRIFT_PERSIST_TICKS) {
            if (max_ticks > 0 && tick_count >= max_ticks) break;
            continue;
        }

        // Chop guard -- mirror GFE lines 588-623
        double dmin = drift_val_window[0], dmax = drift_val_window[0];
        for (double v : drift_val_window) {
            if (v < dmin) dmin = v;
            if (v > dmax) dmax = v;
        }
        int long_count = 0, short_count = 0;
        for (int dd : drift_persist_window) {
            if (dd ==  1) ++long_count;
            if (dd == -1) ++short_count;
        }
        const double drift_range = dmax - dmin;
        const bool both_signs = (dmin < 0.0) && (dmax > 0.0);
        const bool drift_mixed = both_signs
                              && (long_count  >= CHOP_MIN_SIG_TICKS)
                              && (short_count >= CHOP_MIN_SIG_TICKS);
        const bool drift_choppy = (drift_range > CHOP_RANGE_THRESHOLD) && drift_mixed;

        if (drift_choppy) {
            if (max_ticks > 0 && tick_count >= max_ticks) break;
            continue;
        }

        // Persistence check: 70% of window directional
        const int persist_thresh = (int)(GFE_DRIFT_PERSIST_TICKS * DRIFT_THRESH_RATIO);
        bool fast_long  = (long_count  >= persist_thresh);
        bool fast_short = (short_count >= persist_thresh);

        if (!fast_long && !fast_short) {
            if (max_ticks > 0 && tick_count >= max_ticks) break;
            continue;
        }

        // Cooldown: don't double-count the same drift episode
        if (t.ts_ms - last_signal_ts < SIGNAL_COOLDOWN_MS) {
            if (max_ticks > 0 && tick_count >= max_ticks) break;
            continue;
        }

        // Record signal
        Signal s;
        s.ts_ms = t.ts_ms;
        s.mid_at_signal = t.mid();
        s.dir = fast_long ? 1 : -1;
        signals.push_back(s);
        last_signal_ts = t.ts_ms;

        if (max_ticks > 0 && tick_count >= max_ticks) break;

        if (tick_count % 10000000 == 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - t0).count();
            std::cerr << "[P1] tick=" << tick_count
                      << " signals=" << signals.size()
                      << " elapsed=" << std::fixed << std::setprecision(1)
                      << elapsed << "s\n";
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    std::cerr << "\n=== Pass 1 complete ===\n";
    std::cerr << "Ticks parsed: " << tick_count << "\n";
    std::cerr << "Ticks skipped: " << skip_count << "\n";
    std::cerr << "Signals generated: " << signals.size() << "\n";
    std::cerr << "Elapsed: " << std::fixed << std::setprecision(1)
              << std::chrono::duration<double>(t1 - t0).count() << "s\n\n";

    if (signals.empty()) {
        std::cout << "No signals generated. Check CSV format / drift threshold.\n";
        return 0;
    }

    // ---------------------------------------------------------------------
    // Pass 2: For each signal, compute forward move over each window.
    // Uses binary search into all_ticks (sorted by ts_ms).
    // ---------------------------------------------------------------------
    std::cerr << "=== Pass 2: computing forward moves ===\n";
    auto t2 = std::chrono::steady_clock::now();

    auto find_tick_after = [&](int64_t target_ts) -> const Tick* {
        auto it = std::lower_bound(all_ticks.begin(), all_ticks.end(), target_ts,
            [](const Tick& a, int64_t ts) { return a.ts_ms < ts; });
        if (it == all_ticks.end()) return nullptr;
        return &(*it);
    };

    for (auto& s : signals) {
        s.forward_moves.resize(FORWARD_WINDOWS_MIN.size());
        for (size_t i = 0; i < FORWARD_WINDOWS_MIN.size(); ++i) {
            int64_t target = s.ts_ms + (int64_t)FORWARD_WINDOWS_MIN[i] * 60000;
            const Tick* fp = find_tick_after(target);
            if (!fp) {
                s.forward_moves[i] = std::nan("");  // not enough data
            } else {
                s.forward_moves[i] = fp->mid() - s.mid_at_signal;
            }
        }
    }
    auto t3 = std::chrono::steady_clock::now();
    std::cerr << "Pass 2 elapsed: " << std::fixed << std::setprecision(1)
              << std::chrono::duration<double>(t3 - t2).count() << "s\n\n";

    // ---------------------------------------------------------------------
    // Aggregate results per forward window
    // ---------------------------------------------------------------------
    std::cout << "\n======================================================================\n";
    std::cout << "  GOLDFLOW DRIFT-DIRECTION CORRELATION TEST\n";
    std::cout << "  Dataset: " << path << "\n";
    std::cout << "  Format:  " << (fmt == CsvFormat::CTRADER ? "cTrader" : "Dukascopy") << "\n";
    std::cout << "  Total signals: " << signals.size() << "\n";
    std::cout << "======================================================================\n";

    int total_long = 0, total_short = 0;
    for (const auto& s : signals) {
        if (s.dir == 1) ++total_long;
        else            ++total_short;
    }
    std::cout << "Signal sides: LONG=" << total_long << "  SHORT=" << total_short << "\n\n";

    std::cout << std::setw(8) << "FWD_MIN"
              << std::setw(10) << "N_LONG"
              << std::setw(12) << "LONG_HIT%"
              << std::setw(14) << "LONG_MEAN$"
              << std::setw(10) << "N_SHORT"
              << std::setw(12) << "SHORT_HIT%"
              << std::setw(14) << "SHORT_MEAN$"
              << std::setw(16) << "SIGNAL_ALIGNED$"
              << std::setw(12) << "VERDICT"
              << "\n";
    std::cout << std::string(108, '-') << "\n";

    for (size_t w = 0; w < FORWARD_WINDOWS_MIN.size(); ++w) {
        int n_long = 0, n_short = 0;
        int long_hits = 0, short_hits = 0;  // hit = forward move agrees with signal direction
        double long_sum = 0, short_sum = 0;
        double aligned_sum = 0;  // + if signal direction and forward move agree

        for (const auto& s : signals) {
            double fm = s.forward_moves[w];
            if (std::isnan(fm)) continue;
            if (s.dir == 1) {
                ++n_long;
                long_sum += fm;
                if (fm > 0) ++long_hits;
                aligned_sum += fm;  // LONG aligns with + forward move
            } else {
                ++n_short;
                short_sum += fm;
                if (fm < 0) ++short_hits;
                aligned_sum += -fm;  // SHORT aligns with - forward move
            }
        }

        double long_mean   = n_long  > 0 ? long_sum  / n_long  : 0.0;
        double short_mean  = n_short > 0 ? short_sum / n_short : 0.0;
        double long_hit_p  = n_long  > 0 ? 100.0 * long_hits  / n_long  : 0.0;
        double short_hit_p = n_short > 0 ? 100.0 * short_hits / n_short : 0.0;
        int total_n = n_long + n_short;
        double aligned_mean = total_n > 0 ? aligned_sum / total_n : 0.0;

        const char* verdict;
        if (aligned_mean > 0.10)       verdict = "CORRECT";
        else if (aligned_mean < -0.10) verdict = "INVERTED";
        else                            verdict = "NOISE";

        std::cout << std::setw(8) << FORWARD_WINDOWS_MIN[w]
                  << std::setw(10) << n_long
                  << std::setw(11) << std::fixed << std::setprecision(1) << long_hit_p << "%"
                  << std::setw(14) << std::fixed << std::setprecision(3) << long_mean
                  << std::setw(10) << n_short
                  << std::setw(11) << std::fixed << std::setprecision(1) << short_hit_p << "%"
                  << std::setw(14) << std::fixed << std::setprecision(3) << short_mean
                  << std::setw(16) << std::fixed << std::setprecision(3) << aligned_mean
                  << std::setw(12) << verdict
                  << "\n";
    }
    std::cout << "\n";
    std::cout << "Legend:\n";
    std::cout << "  N_LONG/N_SHORT    Number of LONG / SHORT signals with a forward tick available\n";
    std::cout << "  HIT%              Pct of signals where forward move agrees with signal direction\n";
    std::cout << "                    LONG hit = forward_move > 0; SHORT hit = forward_move < 0\n";
    std::cout << "  LONG_MEAN$        Mean forward price move ($) following LONG signals\n";
    std::cout << "  SHORT_MEAN$       Mean forward price move ($) following SHORT signals\n";
    std::cout << "  SIGNAL_ALIGNED$   Mean of (+forward if LONG, -forward if SHORT). Positive = engine\n";
    std::cout << "                    direction correct. Negative = engine direction INVERTED.\n";
    std::cout << "  VERDICT           CORRECT  = aligned_mean > +0.10  (signal has real directional edge)\n";
    std::cout << "                    INVERTED = aligned_mean < -0.10  (inversion hypothesis CONFIRMED)\n";
    std::cout << "                    NOISE    = |aligned_mean| <= 0.10 (no directional edge either way)\n";

    return 0;
}
