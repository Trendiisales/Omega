// =============================================================================
// IndexBracketBacktest.cpp -- Standalone backtest for BracketEngine on indices
// =============================================================================
//
// Replicates the BracketEngine compression-breakout state machine on index tick
// data. Single self-contained .cpp, no external dependencies beyond libc++.
//
// Compilation:
//   clang++ -std=c++17 -O3 -o backtest/idx_bracket_bt backtest/IndexBracketBacktest.cpp
//
// Usage:
//   ./backtest/idx_bracket_bt --instrument SP  ~/Tick/SPXUSD/...csv
//   ./backtest/idx_bracket_bt --instrument NQ  ~/Tick/Nas/...csv
//   ./backtest/idx_bracket_bt --instrument GER40 ~/Tick/GER40/...csv
//   ./backtest/idx_bracket_bt --instrument UK100 ~/Tick/GBRIDXGBP/...csv
//
// Tick format auto-detected: HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX.
//
// Sweep: 81-cell 3x3x3x3 grid over STRUCTURE_LOOKBACK * RR * TRAIL_ACTIVATION
//        * MIN_RANGE_MULT. Per-config one-liner. Best OOS full report.
// OOS verdict: PASS if PF >= 1.20 and trades >= 20.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>

// =============================================================================
// TIME UTILITIES
// =============================================================================
static int64_t ymdhms_to_epoch_ms(int Y, int M, int D, int h, int m, int s, int ms) {
    // Manual UTC epoch calculation (no timegm dependency for portability)
    static constexpr int DAYS_IN_MONTH[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = 0;
    for (int y = 1970; y < Y; ++y)
        days += (y%4==0 && (y%100!=0 || y%400==0)) ? 366 : 365;
    for (int mo = 1; mo < M; ++mo) {
        days += DAYS_IN_MONTH[mo];
        if (mo == 2 && (Y%4==0 && (Y%100!=0 || Y%400==0))) days += 1;
    }
    days += (D - 1);
    return days * 86400000LL + (int64_t)h * 3600000LL + (int64_t)m * 60000LL
         + (int64_t)s * 1000LL + (int64_t)ms;
}

static bool is_weekend(int64_t ts_ms) noexcept {
    const int64_t sec = ts_ms / 1000;
    const int dow = (int)(((sec / 86400) + 4) % 7); // 0=Sun
    if (dow == 0 || dow == 6) return true;
    if (dow == 5) {
        const int hour = (int)((sec % 86400) / 3600);
        if (hour >= 21) return true;
    }
    return false;
}

static int utc_hour_of_day(int64_t ts_ms) {
    return (int)(((ts_ms / 1000) % 86400) / 3600);
}

// =============================================================================
// TICK PARSING -- auto-detect HISTDATA / DUKA_BID_ASK / DUKA_ASK_BID / JFOREX
// =============================================================================
struct Tick {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
};

enum class TickFmt { UNKNOWN, HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX };

static TickFmt detect_format(const std::string& line) {
    // JFOREX header: "Time (EET),Ask,Bid,..."
    if (line.find("Time (EET)") != std::string::npos) return TickFmt::JFOREX;

    // HISTDATA: "YYYYMMDD HHMMSSmmm,bid,ask,0" -- 8 digits then space
    if (line.size() > 20 && line[8] == ' ') {
        bool all_dig = true;
        for (int i = 0; i < 8; ++i) if (line[i] < '0' || line[i] > '9') all_dig = false;
        if (all_dig) return TickFmt::HISTDATA;
    }

    // Count commas
    int commas = 0;
    for (char c : line) if (c == ',') ++commas;

    // Dukascopy numeric: timestamp_ms,val1,val2 (2 commas)
    if (commas == 2) {
        // Heuristic: if first field parses as > 1e12 it's ms timestamp
        // Header strings like "timestamp_ms,bid,ask" or "timestamp_ms,ask,bid"
        if (line.find("bid,ask") != std::string::npos ||
            line.find("bidPrice,askPrice") != std::string::npos) return TickFmt::DUKA_BID_ASK;
        if (line.find("ask,bid") != std::string::npos ||
            line.find("askPrice,bidPrice") != std::string::npos) return TickFmt::DUKA_ASK_BID;
        // No header keywords -- default to DUKA_ASK_BID (Dukascopy standard: ts,ask,bid)
        return TickFmt::DUKA_ASK_BID;
    }

    // JFOREX data line: "2025.01.02 01:02:14.945,19857.645,19849.355,0.00001,0.00002"
    // 4 commas, starts with digits and dots
    if (commas >= 4 && line.size() > 10 && line[4] == '.' && line[7] == '.') {
        return TickFmt::JFOREX;
    }

    return TickFmt::UNKNOWN;
}

static const char* format_name(TickFmt f) {
    switch (f) {
        case TickFmt::HISTDATA:     return "HISTDATA";
        case TickFmt::DUKA_BID_ASK: return "DUKA_BID_ASK";
        case TickFmt::DUKA_ASK_BID: return "DUKA_ASK_BID";
        case TickFmt::JFOREX:       return "JFOREX";
        default:                    return "UNKNOWN";
    }
}

static bool parse_histdata(const std::string& line, Tick& t) {
    if (line.size() < 20) return false;
    int Y = 0, M = 0, D = 0;
    for (int i = 0; i < 4; ++i) Y = Y * 10 + (line[i] - '0');
    for (int i = 4; i < 6; ++i) M = M * 10 + (line[i] - '0');
    for (int i = 6; i < 8; ++i) D = D * 10 + (line[i] - '0');
    if (line[8] != ' ') return false;
    // Parse HHMMSSmmm (9 chars starting at position 9)
    int ti = 9;
    int time_val = 0;
    while (ti < (int)line.size() && line[ti] >= '0' && line[ti] <= '9') {
        time_val = time_val * 10 + (line[ti] - '0');
        ti++;
    }
    if (ti >= (int)line.size() || line[ti] != ',') return false;
    int h  = time_val / 10000000;
    int m  = (time_val / 100000) % 100;
    int s  = (time_val / 1000) % 100;
    int ms = time_val % 1000;

    double bid, ask;
    if (std::sscanf(line.c_str() + ti + 1, "%lf,%lf", &bid, &ask) != 2) return false;
    if (bid <= 0 || ask <= 0) return false;

    // HISTDATA timestamps are EST (UTC-5 fixed). Convert to UTC by adding 5h.
    t.ts_ms = ymdhms_to_epoch_ms(Y, M, D, h, m, s, ms) + 5LL * 3600000LL;
    t.bid = bid;
    t.ask = ask;
    return true;
}

static bool parse_duka_bid_ask(const std::string& line, Tick& t) {
    return std::sscanf(line.c_str(), "%lld,%lf,%lf",
                       (long long*)&t.ts_ms, &t.bid, &t.ask) == 3 && t.bid > 0 && t.ask > 0;
}

static bool parse_duka_ask_bid(const std::string& line, Tick& t) {
    double ask_v, bid_v;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf",
                    (long long*)&t.ts_ms, &ask_v, &bid_v) == 3 && bid_v > 0 && ask_v > 0) {
        t.bid = bid_v; t.ask = ask_v; return true;
    }
    return false;
}

static bool parse_jforex(const std::string& line, Tick& t) {
    int Y, M, D, h, m, s, ms;
    double ask_v, bid_v;
    if (std::sscanf(line.c_str(), "%4d.%2d.%2d %2d:%2d:%2d.%3d,%lf,%lf",
                    &Y, &M, &D, &h, &m, &s, &ms, &ask_v, &bid_v) == 9) {
        // EET = UTC+2 (approximate; DST adds +/-1h error)
        t.ts_ms = ymdhms_to_epoch_ms(Y, M, D, h, m, s, ms) - 2LL * 3600000LL;
        t.bid = bid_v;
        t.ask = ask_v;
        return true;
    }
    return false;
}

static bool parse_line(TickFmt fmt, const std::string& line, Tick& t) {
    switch (fmt) {
        case TickFmt::HISTDATA:     return parse_histdata(line, t);
        case TickFmt::DUKA_BID_ASK: return parse_duka_bid_ask(line, t);
        case TickFmt::DUKA_ASK_BID: return parse_duka_ask_bid(line, t);
        case TickFmt::JFOREX:       return parse_jforex(line, t);
        default: return false;
    }
}

// =============================================================================
// PER-INSTRUMENT CONFIG
// =============================================================================
struct InstrConfig {
    const char* name;
    double MAX_RANGE;
    double MAX_SL_DIST;
    double CONFIRM_PTS;
    int    CONFIRM_SECS;
    double TRAIL_ACTIVATION;
    double TRAIL_DISTANCE;
    double RR;
    double LOT_SIZE;
    double PNL_PER_PT;
    double PRICE_LO;   // sanity range
    double PRICE_HI;
    double MIN_RANGE;   // from configure() call
};

static InstrConfig config_for(const std::string& inst) {
    InstrConfig c{};
    c.CONFIRM_SECS = 30;
    c.LOT_SIZE     = 0.01;
    c.RR           = 1.5;
    c.MIN_RANGE    = 12.0; // default, overridden below

    if (inst == "SP") {
        c.name             = "SP (US500.F)";
        c.MAX_RANGE        = 25.0;
        c.MAX_SL_DIST      = 12.0;
        c.CONFIRM_PTS      = 6.0;
        c.TRAIL_ACTIVATION = 3.0;
        c.TRAIL_DISTANCE   = 2.0;
        c.PNL_PER_PT       = 0.50;
        c.PRICE_LO         = 3000.0;
        c.PRICE_HI         = 8000.0;
        c.MIN_RANGE        = 12.0;
    } else if (inst == "NQ") {
        c.name             = "NQ (USTEC.F)";
        c.MAX_RANGE        = 90.0;
        c.MAX_SL_DIST      = 45.0;
        c.CONFIRM_PTS      = 22.0;
        c.TRAIL_ACTIVATION = 8.0;
        c.TRAIL_DISTANCE   = 5.0;
        c.PNL_PER_PT       = 0.20;
        c.PRICE_LO         = 10000.0;
        c.PRICE_HI         = 25000.0;
        c.MIN_RANGE        = 42.0;
    } else if (inst == "GER40") {
        c.name             = "GER40";
        c.MAX_RANGE        = 90.0;
        c.MAX_SL_DIST      = 45.0;
        c.CONFIRM_PTS      = 22.0;
        c.TRAIL_ACTIVATION = 8.0;
        c.TRAIL_DISTANCE   = 5.0;
        c.PNL_PER_PT       = 0.10;
        c.PRICE_LO         = 12000.0;
        c.PRICE_HI         = 25000.0;
        c.MIN_RANGE        = 44.0;
    } else if (inst == "UK100") {
        c.name             = "UK100";
        c.MAX_RANGE        = 40.0;
        c.MAX_SL_DIST      = 20.0;
        c.CONFIRM_PTS      = 10.0;
        c.TRAIL_ACTIVATION = 4.0;
        c.TRAIL_DISTANCE   = 3.0;
        c.PNL_PER_PT       = 0.10;
        c.PRICE_LO         = 5000.0;
        c.PRICE_HI         = 12000.0;
        c.MIN_RANGE        = 20.0;
    } else {
        std::fprintf(stderr, "ERROR: unknown instrument '%s'. Use SP, NQ, GER40, UK100.\n", inst.c_str());
        std::exit(1);
    }
    return c;
}

// =============================================================================
// PERFORMANCE METRICS
// =============================================================================
struct PerfMetrics {
    int    trades      = 0;
    int    wins        = 0;
    int    losses      = 0;
    double gross_wins  = 0.0;
    double gross_loss  = 0.0;
    double total_pnl   = 0.0;
    double peak_eq     = 0.0;
    double max_dd      = 0.0;
    // Per-direction
    int    long_trades  = 0, long_wins = 0;
    double long_pnl     = 0.0, long_gwins = 0.0, long_gloss = 0.0;
    int    short_trades = 0, short_wins = 0;
    double short_pnl    = 0.0, short_gwins = 0.0, short_gloss = 0.0;
    // Exit breakdown
    int    n_sl = 0, n_trail = 0, n_be = 0, n_bf = 0, n_bf_confirm = 0;
    int    n_timeout = 0, n_other = 0;
    // Per-hour
    std::array<int,24>    hour_trades{};
    std::array<int,24>    hour_wins{};
    std::array<double,24> hour_gwins{};
    std::array<double,24> hour_gloss{};
    std::array<double,24> hour_pnl{};

    void record(double pnl_usd, bool is_long, const char* reason, int entry_hour) {
        trades++;
        total_pnl += pnl_usd;
        if (total_pnl > peak_eq) peak_eq = total_pnl;
        double dd = peak_eq - total_pnl;
        if (dd > max_dd) max_dd = dd;

        if (pnl_usd > 0) { wins++; gross_wins += pnl_usd; }
        else              { losses++; gross_loss += std::fabs(pnl_usd); }

        if (is_long) {
            long_trades++; long_pnl += pnl_usd;
            if (pnl_usd > 0) { long_wins++; long_gwins += pnl_usd; }
            else { long_gloss += std::fabs(pnl_usd); }
        } else {
            short_trades++; short_pnl += pnl_usd;
            if (pnl_usd > 0) { short_wins++; short_gwins += pnl_usd; }
            else { short_gloss += std::fabs(pnl_usd); }
        }

        // Classify exit
        if      (std::strcmp(reason, "SL_HIT") == 0)              n_sl++;
        else if (std::strcmp(reason, "TRAIL_HIT") == 0)           n_trail++;
        else if (std::strcmp(reason, "BE_HIT") == 0)              n_be++;
        else if (std::strcmp(reason, "BREAKOUT_FAIL") == 0)       n_bf++;
        else if (std::strcmp(reason, "BREAKOUT_FAIL_CONFIRM") == 0) n_bf_confirm++;
        else if (std::strcmp(reason, "MAX_HOLD_TIMEOUT") == 0)    n_timeout++;
        else                                                      n_other++;

        int h = entry_hour % 24;
        hour_trades[h]++;
        hour_pnl[h] += pnl_usd;
        if (pnl_usd > 0) { hour_wins[h]++; hour_gwins[h] += pnl_usd; }
        else              { hour_gloss[h] += std::fabs(pnl_usd); }
    }

    double wr()  const { return trades > 0 ? 100.0 * wins / trades : 0.0; }
    double pf()  const { return gross_loss > 0 ? gross_wins / gross_loss : (gross_wins > 0 ? 999.0 : 0.0); }
    double avg() const { return trades > 0 ? total_pnl / trades : 0.0; }

    double long_wr()  const { return long_trades > 0 ? 100.0 * long_wins / long_trades : 0.0; }
    double long_pf()  const { return long_gloss > 0 ? long_gwins / long_gloss : (long_gwins > 0 ? 999.0 : 0.0); }
    double short_wr() const { return short_trades > 0 ? 100.0 * short_wins / short_trades : 0.0; }
    double short_pf() const { return short_gloss > 0 ? short_gwins / short_gloss : (short_gwins > 0 ? 999.0 : 0.0); }

    double hour_pf_at(int h) const {
        return hour_gloss[h] > 0 ? hour_gwins[h] / hour_gloss[h] : (hour_gwins[h] > 0 ? 999.0 : 0.0);
    }
};

// =============================================================================
// SIMULATED BRACKET ENGINE (self-contained state machine)
// =============================================================================
// Replicates the BracketEngine state machine without depending on the production
// BracketEngine.hpp (which uses CRTP, OmegaTradeLedger, OmegaCostGuard, and
// system_clock for nowSec). This sim uses tick timestamps for all time logic.
// =============================================================================

struct SimBracket {
    // Config (set from InstrConfig + sweep params)
    int    STRUCTURE_LOOKBACK_SEC = 30;  // time-based window in seconds
    int    MIN_ENTRY_TICKS     = 150;
    double MIN_RANGE           = 0.0;
    double MAX_RANGE           = 0.0;
    double MAX_SL_DIST_PTS     = 0.0;
    double MAX_SPREAD          = 0.0;
    double RR                  = 1.5;
    int    COOLDOWN_MS         = 120000;
    int    MIN_HOLD_MS         = 15000;
    int    FAILURE_WINDOW_MS   = 5000;
    int    MAX_HOLD_SEC        = 1800;
    double CONFIRM_PTS         = 0.0;
    int    CONFIRM_SECS        = 0;
    double TRAIL_ACTIVATION_PTS = 3.0;
    double TRAIL_DISTANCE_PTS   = 2.0;
    double WHIPSAW_OVERLAP_K        = 0.5;
    double WHIPSAW_COOLDOWN_MULT    = 2.0;
    int    WHIPSAW_LOCKOUT_MAX_MS   = 3600000;
    int    CONSEC_SL_KILL_THRESHOLD = 3;
    int    CONSEC_SL_KILL_DURATION_MS = 1800000;
    double ENTRY_SIZE          = 0.01;

    double PNL_PER_PT = 0.50;
    double PRICE_LO   = 0.0;
    double PRICE_HI   = 999999.0;

    // State
    enum Phase { IDLE=0, ARMED, PENDING, CONFIRM, LIVE, COOLDOWN };
    Phase  phase = IDLE;

    struct TimedMid { int64_t ts_ms; double mid; };
    std::deque<TimedMid> window;
    int    ticks_received = 0;
    double bracket_high   = 0.0;
    double bracket_low    = 0.0;
    double recent_range   = 0.0;

    // Locked bracket levels at arm time
    double locked_hi = 0.0, locked_lo = 0.0;
    double locked_long_sl = 0.0, locked_long_tp = 0.0;
    double locked_short_sl = 0.0, locked_short_tp = 0.0;

    // Open position
    bool    pos_active = false;
    bool    pos_is_long = true;
    double  pos_entry = 0.0;
    double  pos_sl = 0.0;
    double  pos_tp = 0.0;
    double  pos_mfe = 0.0;
    double  pos_mae = 0.0;
    int64_t pos_entry_ts_ms = 0;
    bool    pos_sl_locked_be = false;

    int64_t cooldown_end_ms = 0;
    int64_t armed_ts_ms     = 0;

    // Whipsaw
    double  last_stop_hi = 0.0, last_stop_lo = 0.0;
    int64_t last_stop_ts_ms = 0;
    int     whipsaw_count = 0;
    int     cooldown_override_ms = 0;

    // Consecutive SL kill
    int     consec_sl = 0;
    int64_t sl_kill_until_ms = 0;

    // Trade output
    struct TradeResult {
        bool    valid       = false;
        bool    is_long     = true;
        double  entry       = 0.0;
        double  exit_px     = 0.0;
        double  pnl_pts     = 0.0;
        double  pnl_usd     = 0.0;
        double  mfe         = 0.0;
        double  mae         = 0.0;
        int64_t entry_ts_ms = 0;
        int64_t exit_ts_ms  = 0;
        const char* reason  = "";
    };
    TradeResult last_trade;

    void reset_pos() {
        pos_active = false;
        pos_is_long = true;
        pos_entry = 0.0;
        pos_sl = 0.0;
        pos_tp = 0.0;
        pos_mfe = 0.0;
        pos_mae = 0.0;
        pos_entry_ts_ms = 0;
        pos_sl_locked_be = false;
    }

    void close_pos(double exit_px, const char* reason, int64_t ts_ms) {
        double raw_pnl_pts = pos_is_long ? (exit_px - pos_entry) : (pos_entry - exit_px);
        last_trade.valid       = true;
        last_trade.is_long     = pos_is_long;
        last_trade.entry       = pos_entry;
        last_trade.exit_px     = exit_px;
        last_trade.pnl_pts     = raw_pnl_pts;
        last_trade.pnl_usd     = raw_pnl_pts * PNL_PER_PT;
        last_trade.mfe         = pos_mfe;
        last_trade.mae         = pos_mae;
        last_trade.entry_ts_ms = pos_entry_ts_ms;
        last_trade.exit_ts_ms  = ts_ms;
        last_trade.reason      = reason;

        // Whipsaw bookkeeping
        bool is_loss = (raw_pnl_pts < 0.0);
        bool is_win  = (raw_pnl_pts > 0.0);

        if (is_loss) {
            bool overlaps_prev = false;
            if (last_stop_hi > 0 && last_stop_lo > 0 && WHIPSAW_OVERLAP_K > 0) {
                double ov_lo = std::max(locked_lo, last_stop_lo);
                double ov_hi = std::min(locked_hi, last_stop_hi);
                double ov = (ov_hi > ov_lo) ? (ov_hi - ov_lo) : 0.0;
                double rng = locked_hi - locked_lo;
                double frac = rng > 0 ? ov / rng : 0.0;
                overlaps_prev = (frac >= WHIPSAW_OVERLAP_K);
            }
            whipsaw_count = overlaps_prev ? whipsaw_count + 1 : 1;
            last_stop_hi = locked_hi;
            last_stop_lo = locked_lo;
            last_stop_ts_ms = ts_ms;
            if (whipsaw_count >= 2 && WHIPSAW_COOLDOWN_MULT > 1.0) {
                cooldown_override_ms = (int)(COOLDOWN_MS * WHIPSAW_COOLDOWN_MULT);
            } else {
                cooldown_override_ms = 0;
            }
        } else if (is_win) {
            last_stop_hi = 0; last_stop_lo = 0;
            whipsaw_count = 0;
            cooldown_override_ms = 0;
        }

        // Consecutive SL counter
        if (std::strcmp(reason, "SL_HIT") == 0) {
            consec_sl++;
            if (CONSEC_SL_KILL_THRESHOLD > 0 && consec_sl >= CONSEC_SL_KILL_THRESHOLD) {
                sl_kill_until_ms = ts_ms + CONSEC_SL_KILL_DURATION_MS;
            }
        } else {
            consec_sl = 0;
        }

        // Enter cooldown
        int cd_ms = cooldown_override_ms > 0 ? cooldown_override_ms : COOLDOWN_MS;
        cooldown_end_ms = ts_ms + cd_ms;
        cooldown_override_ms = 0;
        phase = COOLDOWN;
        reset_pos();
    }

    // Returns true if a trade closed on this tick (result in last_trade)
    bool on_tick(double bid, double ask, int64_t ts_ms) {
        last_trade.valid = false;
        if (bid <= 0 || ask <= 0 || bid > ask) return false;

        double mid = (bid + ask) * 0.5;
        double spread = ask - bid;

        // Price sanity
        if (mid < PRICE_LO || mid > PRICE_HI) return false;

        // Weekend gate
        if (is_weekend(ts_ms)) return false;

        // COOLDOWN
        if (phase == COOLDOWN) {
            if (ts_ms >= cooldown_end_ms) {
                phase = IDLE;
            } else {
                // Still feed window during cooldown
                window.push_back({ts_ms, mid});
                ticks_received++;
                int64_t cutoff = ts_ms - (int64_t)STRUCTURE_LOOKBACK_SEC * 2000LL;
                while (!window.empty() && window.front().ts_ms < cutoff) window.pop_front();
                return false;
            }
        }

        // CONFIRM
        if (phase == CONFIRM) {
            if (!pos_active) return false;
            double move = pos_is_long ? (mid - pos_entry) : (pos_entry - mid);
            if (move > pos_mfe) pos_mfe = move;
            if (-move > pos_mae) pos_mae = -move;

            // Hard SL
            if (pos_is_long && bid <= pos_sl)  { close_pos(pos_sl, "SL_HIT", ts_ms); return true; }
            if (!pos_is_long && ask >= pos_sl) { close_pos(pos_sl, "SL_HIT", ts_ms); return true; }

            // Confirm threshold met
            if (pos_mfe >= CONFIRM_PTS) { phase = LIVE; return false; }

            // Window expired
            int64_t elapsed_ms = ts_ms - pos_entry_ts_ms;
            if (elapsed_ms >= (int64_t)CONFIRM_SECS * 1000LL) {
                double exit_px = pos_is_long ? bid : ask;
                close_pos(exit_px, "BREAKOUT_FAIL_CONFIRM", ts_ms);
                return true;
            }
            return false;
        }

        // LIVE
        if (phase == LIVE) {
            if (!pos_active) return false;
            double move = pos_is_long ? (mid - pos_entry) : (pos_entry - mid);
            if (move > pos_mfe) pos_mfe = move;
            if (-move > pos_mae) pos_mae = -move;

            int64_t hold_ms = ts_ms - pos_entry_ts_ms;

            // Breakout failure: within FAILURE_WINDOW_MS, price crosses bracket mid
            if (FAILURE_WINDOW_MS > 0 && hold_ms < FAILURE_WINDOW_MS) {
                double bracket_mid = (locked_hi + locked_lo) * 0.5;
                if (pos_is_long && bid < bracket_mid)  { close_pos(bid, "BREAKOUT_FAIL", ts_ms); return true; }
                if (!pos_is_long && ask > bracket_mid) { close_pos(ask, "BREAKOUT_FAIL", ts_ms); return true; }
            }

            // Min hold
            if (hold_ms < MIN_HOLD_MS) return false;

            // Max hold timeout (losers only)
            if (MAX_HOLD_SEC > 0 && hold_ms >= (int64_t)MAX_HOLD_SEC * 1000LL) {
                double cur_move = pos_is_long ? (mid - pos_entry) : (pos_entry - mid);
                if (cur_move <= 0.0) {
                    double exit_px = pos_is_long ? bid : ask;
                    close_pos(exit_px, "MAX_HOLD_TIMEOUT", ts_ms);
                    return true;
                }
            }

            // Continuous MFE trail
            {
                double trail_move = pos_is_long ? (mid - pos_entry) : (pos_entry - mid);
                if (trail_move > pos_mfe) pos_mfe = trail_move;
                if (pos_mfe >= TRAIL_ACTIVATION_PTS) {
                    double new_sl = pos_is_long
                        ? (pos_entry + pos_mfe - TRAIL_DISTANCE_PTS)
                        : (pos_entry - pos_mfe + TRAIL_DISTANCE_PTS);
                    bool ratchets = pos_is_long ? (new_sl > pos_sl) : (new_sl < pos_sl);
                    if (ratchets) {
                        bool crossed_be = pos_is_long ? (new_sl >= pos_entry) : (new_sl <= pos_entry);
                        pos_sl = new_sl;
                        if (crossed_be) pos_sl_locked_be = true;
                    }
                }
            }

            // SL check
            if (pos_is_long && bid <= pos_sl) {
                const char* r = pos_sl_locked_be
                    ? (pos_sl > pos_entry + 0.01 ? "TRAIL_HIT" : "BE_HIT") : "SL_HIT";
                close_pos(pos_sl, r, ts_ms);
                return true;
            }
            if (!pos_is_long && ask >= pos_sl) {
                const char* r = pos_sl_locked_be
                    ? (pos_sl < pos_entry - 0.01 ? "TRAIL_HIT" : "BE_HIT") : "SL_HIT";
                close_pos(pos_sl, r, ts_ms);
                return true;
            }
            return false;
        }

        // PENDING -- simulated fill
        if (phase == PENDING) {
            int64_t pend_ms = ts_ms - armed_ts_ms;
            if (pend_ms > 300000LL) { // 5 min timeout
                phase = IDLE;
                return false;
            }
            // Check for fill
            if (ask >= locked_hi) {
                // Long fill
                pos_active = true;
                pos_is_long = true;
                pos_entry = ask;  // fill at ask for long
                pos_sl = locked_long_sl;
                pos_tp = locked_long_tp;
                pos_mfe = 0; pos_mae = 0;
                pos_entry_ts_ms = ts_ms;
                pos_sl_locked_be = false;
                phase = (CONFIRM_PTS > 0 && CONFIRM_SECS > 0) ? CONFIRM : LIVE;
                return false;
            }
            if (bid <= locked_lo) {
                // Short fill
                pos_active = true;
                pos_is_long = false;
                pos_entry = bid;  // fill at bid for short
                pos_sl = locked_short_sl;
                pos_tp = locked_short_tp;
                pos_mfe = 0; pos_mae = 0;
                pos_entry_ts_ms = ts_ms;
                pos_sl_locked_be = false;
                phase = (CONFIRM_PTS > 0 && CONFIRM_SECS > 0) ? CONFIRM : LIVE;
                return false;
            }
            return false;
        }

        // Feed window (always, regardless of phase)
        window.push_back({ts_ms, mid});
        ticks_received++;

        // Evict ticks older than 2x the lookback window
        {
            int64_t cutoff = ts_ms - (int64_t)STRUCTURE_LOOKBACK_SEC * 2000LL;
            while (!window.empty() && window.front().ts_ms < cutoff) window.pop_front();
        }

        // Update recent range (ticks within last 10 seconds)
        {
            int64_t rw_cutoff = ts_ms - 10000LL;
            double rhi = -1e18, rlo = 1e18;
            int rcount = 0;
            for (auto it = window.rbegin(); it != window.rend(); ++it) {
                if (it->ts_ms < rw_cutoff) break;
                if (it->mid > rhi) rhi = it->mid;
                if (it->mid < rlo) rlo = it->mid;
                rcount++;
            }
            recent_range = (rcount > 1) ? (rhi - rlo) : 0.0;
        }

        // Need ticks spanning at least STRUCTURE_LOOKBACK_SEC
        int64_t lb_cutoff = ts_ms - (int64_t)STRUCTURE_LOOKBACK_SEC * 1000LL;
        if (window.empty() || window.front().ts_ms > lb_cutoff) return false;

        // Cold start
        if (ticks_received < MIN_ENTRY_TICKS && phase == IDLE) return false;

        // Structural range: hi/lo over the lookback time window
        double shi = -1e18, slo = 1e18;
        for (auto it = window.rbegin(); it != window.rend(); ++it) {
            if (it->ts_ms < lb_cutoff) break;
            if (it->mid > shi) shi = it->mid;
            if (it->mid < slo) slo = it->mid;
        }
        double range = shi - slo;

        if (range < MIN_RANGE) {
            if (phase != ARMED) { phase = IDLE; bracket_high = 0; bracket_low = 0; }
            return false;
        }

        double buf = spread * 0.5;
        bracket_high = shi + buf;
        bracket_low  = slo - buf;

        // Consecutive SL kill check
        if (phase == IDLE && CONSEC_SL_KILL_THRESHOLD > 0 && sl_kill_until_ms > 0 && ts_ms < sl_kill_until_ms)
            return false;
        if (sl_kill_until_ms > 0 && ts_ms >= sl_kill_until_ms) {
            sl_kill_until_ms = 0;
            consec_sl = 0;
        }

        // Whipsaw lockout check
        if (phase == IDLE && WHIPSAW_OVERLAP_K > 0 && last_stop_hi > 0 && last_stop_lo > 0) {
            int64_t age_ms = ts_ms - last_stop_ts_ms;
            if (age_ms >= WHIPSAW_LOCKOUT_MAX_MS) {
                last_stop_hi = 0; last_stop_lo = 0;
                whipsaw_count = 0;
            } else {
                double ov_lo = std::max(bracket_low, last_stop_lo);
                double ov_hi = std::min(bracket_high, last_stop_hi);
                double ov = (ov_hi > ov_lo) ? (ov_hi - ov_lo) : 0.0;
                double new_rng = bracket_high - bracket_low;
                double frac = new_rng > 0 ? ov / new_rng : 0.0;
                if (frac >= WHIPSAW_OVERLAP_K) return false;
                // Range moved away -- release
                last_stop_hi = 0; last_stop_lo = 0;
            }
        }

        // IDLE -> ARMED
        if (phase == IDLE) {
            phase = ARMED;
            armed_ts_ms = ts_ms;
            return false;
        }

        // ARMED -> PENDING: arm both sides
        if (phase == ARMED) {
            double dist = bracket_high - bracket_low;
            double raw_range = dist - spread;

            // Checks before arming
            if (raw_range < MIN_RANGE)         { phase = IDLE; return false; }
            if (MAX_RANGE > 0 && recent_range > MAX_RANGE) { phase = IDLE; return false; }
            if (MAX_SL_DIST_PTS > 0 && dist > MAX_SL_DIST_PTS) { phase = IDLE; return false; }
            if (MAX_SPREAD > 0 && spread > MAX_SPREAD)     { phase = IDLE; return false; }

            // Lock levels
            locked_hi = bracket_high;
            locked_lo = bracket_low;

            double long_entry  = bracket_high;  // buy stop at ask when price crosses high
            double short_entry = bracket_low;   // sell stop at bid when price crosses low
            locked_long_sl  = bracket_low;
            locked_short_sl = bracket_high;
            locked_long_tp  = long_entry  + dist * RR;
            locked_short_tp = short_entry - dist * RR;

            phase = PENDING;
            return false;
        }

        return false;
    }

    void full_reset() {
        phase = IDLE;
        window.clear();
        ticks_received = 0;
        bracket_high = 0; bracket_low = 0;
        recent_range = 0;
        locked_hi = 0; locked_lo = 0;
        reset_pos();
        cooldown_end_ms = 0;
        armed_ts_ms = 0;
        last_stop_hi = 0; last_stop_lo = 0;
        last_stop_ts_ms = 0;
        whipsaw_count = 0;
        cooldown_override_ms = 0;
        consec_sl = 0;
        sl_kill_until_ms = 0;
        last_trade.valid = false;
    }
};

// =============================================================================
// RUN-ONE: execute a single backtest config on the loaded ticks
// =============================================================================
struct SweepResult {
    int    lookback          = 0;
    double rr                = 0.0;
    double trail_act         = 0.0;
    double min_range         = 0.0;
    // IS
    int    is_trades         = 0;
    double is_wr             = 0.0;
    double is_pf             = 0.0;
    double is_pnl            = 0.0;
    // OOS
    int    oos_trades        = 0;
    double oos_wr            = 0.0;
    double oos_pf            = 0.0;
    double oos_pnl           = 0.0;
    double oos_maxdd         = 0.0;
    // Full run for detailed report
    PerfMetrics oos_full;
    PerfMetrics is_full;
    PerfMetrics overall_full;
};

static SweepResult run_one(const std::vector<Tick>& ticks,
                           const InstrConfig& cfg,
                           int lookback, double rr, double trail_act_mult,
                           double min_range_override,
                           int64_t oos_split_ms)
{
    SweepResult res;
    res.lookback  = lookback;
    res.rr        = rr;
    res.trail_act = cfg.TRAIL_ACTIVATION * trail_act_mult;
    res.min_range = min_range_override;

    SimBracket eng;
    eng.STRUCTURE_LOOKBACK_SEC = lookback;
    eng.MIN_ENTRY_TICKS       = 150;
    eng.MIN_RANGE             = min_range_override;
    eng.MAX_RANGE             = cfg.MAX_RANGE;
    eng.MAX_SL_DIST_PTS       = cfg.MAX_SL_DIST;
    eng.RR                    = rr;
    eng.COOLDOWN_MS           = 120000;
    eng.MIN_HOLD_MS           = 15000;
    eng.FAILURE_WINDOW_MS     = 5000;
    eng.MAX_HOLD_SEC          = 1800;
    eng.CONFIRM_PTS           = cfg.CONFIRM_PTS;
    eng.CONFIRM_SECS          = cfg.CONFIRM_SECS;
    eng.TRAIL_ACTIVATION_PTS  = cfg.TRAIL_ACTIVATION * trail_act_mult;
    eng.TRAIL_DISTANCE_PTS    = cfg.TRAIL_DISTANCE;
    eng.ENTRY_SIZE            = cfg.LOT_SIZE;
    eng.PNL_PER_PT            = cfg.PNL_PER_PT;
    eng.PRICE_LO              = cfg.PRICE_LO;
    eng.PRICE_HI              = cfg.PRICE_HI;

    PerfMetrics is_met, oos_met, overall_met;

    for (size_t i = 0; i < ticks.size(); ++i) {
        const Tick& tk = ticks[i];
        bool closed = eng.on_tick(tk.bid, tk.ask, tk.ts_ms);
        if (closed && eng.last_trade.valid) {
            int entry_hour = utc_hour_of_day(eng.last_trade.entry_ts_ms);
            overall_met.record(eng.last_trade.pnl_usd, eng.last_trade.is_long,
                               eng.last_trade.reason, entry_hour);
            if (eng.last_trade.entry_ts_ms < oos_split_ms)
                is_met.record(eng.last_trade.pnl_usd, eng.last_trade.is_long,
                              eng.last_trade.reason, entry_hour);
            else
                oos_met.record(eng.last_trade.pnl_usd, eng.last_trade.is_long,
                               eng.last_trade.reason, entry_hour);
        }
    }

    res.is_trades  = is_met.trades;
    res.is_wr      = is_met.wr();
    res.is_pf      = is_met.pf();
    res.is_pnl     = is_met.total_pnl;
    res.oos_trades = oos_met.trades;
    res.oos_wr     = oos_met.wr();
    res.oos_pf     = oos_met.pf();
    res.oos_pnl    = oos_met.total_pnl;
    res.oos_maxdd  = oos_met.max_dd;
    res.oos_full   = oos_met;
    res.is_full    = is_met;
    res.overall_full = overall_met;
    return res;
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: idx_bracket_bt --instrument <SP|NQ|GER40|UK100> <tick_csv> [tick_csv2 ...]\n"
            "\nReads tick CSVs and runs an 81-cell sweep over:\n"
            "  STRUCTURE_LOOKBACK_SEC: {30, 60, 120}\n"
            "  RR:                {1.0, 1.5, 2.0}\n"
            "  TRAIL_ACTIVATION:  {0.5x, 1.0x, 1.5x} of instrument default\n"
            "  MIN_RANGE:         {0.25x, 0.50x, 1.0x} of instrument default\n"
            "\nTick formats auto-detected: HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX\n");
        return 1;
    }

    std::string instrument;
    std::vector<std::string> csv_files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instrument") == 0 && i+1 < argc) {
            instrument = argv[++i];
        } else {
            csv_files.push_back(argv[i]);
        }
    }
    if (instrument.empty()) {
        std::fprintf(stderr, "ERROR: --instrument required (SP, NQ, GER40, UK100)\n");
        return 1;
    }
    if (csv_files.empty()) {
        std::fprintf(stderr, "ERROR: no tick CSV files provided\n");
        return 1;
    }

    InstrConfig cfg = config_for(instrument);

    // =========================================================================
    // LOAD TICKS
    // =========================================================================
    std::vector<Tick> all_ticks;
    all_ticks.reserve(100000000);

    for (const auto& path : csv_files) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "WARNING: cannot open %s -- skipping\n", path.c_str());
            continue;
        }

        // Read first line for format detection
        std::string first_line;
        std::getline(f, first_line);
        TickFmt fmt = detect_format(first_line);
        if (fmt == TickFmt::UNKNOWN) {
            // Try second line (first might be header)
            std::string second_line;
            if (std::getline(f, second_line)) {
                fmt = detect_format(second_line);
            }
            if (fmt == TickFmt::UNKNOWN) {
                std::fprintf(stderr, "WARNING: cannot detect format in %s -- skipping\n", path.c_str());
                continue;
            }
            // second_line is data
            Tick t;
            if (parse_line(fmt, second_line, t)) all_ticks.push_back(t);
        } else {
            // first_line might be header (JFOREX) or data
            if (fmt == TickFmt::JFOREX && first_line.find("Time") != std::string::npos) {
                // header, skip
            } else {
                Tick t;
                if (parse_line(fmt, first_line, t)) all_ticks.push_back(t);
            }
        }

        std::fprintf(stderr, "[IDX-BKT] Loading %s (format=%s)...\n", path.c_str(), format_name(fmt));

        std::string line;
        size_t line_count = 0;
        while (std::getline(f, line)) {
            Tick t;
            if (parse_line(fmt, line, t)) all_ticks.push_back(t);
            if (++line_count % 10000000 == 0)
                std::fprintf(stderr, "  ... %zuM lines\n", line_count / 1000000);
        }
    }

    if (all_ticks.empty()) {
        std::fprintf(stderr, "ERROR: no ticks loaded\n");
        return 1;
    }

    // Sort by timestamp
    std::sort(all_ticks.begin(), all_ticks.end(),
              [](const Tick& a, const Tick& b) { return a.ts_ms < b.ts_ms; });

    int64_t first_ts = all_ticks.front().ts_ms;
    int64_t last_ts  = all_ticks.back().ts_ms;
    double days = (last_ts - first_ts) / 86400000.0;

    // IS/OOS split at 60%
    int64_t oos_split_ms = first_ts + (int64_t)((last_ts - first_ts) * 0.60);

    std::printf("================================================================\n");
    std::printf("  Index Bracket Backtest -- %s\n", cfg.name);
    std::printf("================================================================\n");
    std::printf("  Ticks loaded : %zu\n", all_ticks.size());
    std::printf("  Period       : %.1f days\n", days);
    std::printf("  Price range  : %.1f - %.1f\n",
                all_ticks.front().bid, all_ticks.back().bid);
    std::printf("  IS/OOS split : 60/40 at tick %zu\n",
                (size_t)(all_ticks.size() * 0.60));
    std::printf("  Config       : MAX_RANGE=%.0f  MAX_SL_DIST=%.0f  MIN_RANGE=%.0f  CONFIRM=%.0f/%.0fs\n",
                cfg.MAX_RANGE, cfg.MAX_SL_DIST, cfg.MIN_RANGE, cfg.CONFIRM_PTS, (double)cfg.CONFIRM_SECS);
    std::printf("  Trail default: ACT=%.1f  DIST=%.1f  PNL_PER_PT=%.2f\n",
                cfg.TRAIL_ACTIVATION, cfg.TRAIL_DISTANCE, cfg.PNL_PER_PT);
    std::printf("================================================================\n\n");

    // =========================================================================
    // 81-CELL SWEEP (3x3x3x3)
    // =========================================================================
    const int    lookbacks[]       = {30, 60, 120};  // seconds (time-based window)
    const double rrs[]             = {1.0, 1.5, 2.0};
    const double trail_act_mults[] = {0.5, 1.0, 1.5};
    const double min_range_mults[] = {0.25, 0.50, 1.0};
    const int    total_cells = 81;

    std::vector<SweepResult> results;
    results.reserve(total_cells);

    std::printf("%-4s  %4s  %4s  %6s  %6s  | %5s %5s %6s %8s  | %5s %5s %6s %8s %8s\n",
                "Cell", "LB", "RR", "T_ACT", "MINRG",
                "IS_N", "IS_WR", "IS_PF", "IS_PnL",
                "OOS_N", "OOS_WR", "OOS_PF", "OOS_PnL", "OOS_DD");
    std::printf("------------------------------------------------------------------------"
                "------------------------------------------------\n");

    int cell = 0;
    int best_idx = -1;
    double best_oos_pf = 0.0;

    for (int lb : lookbacks) {
        for (double rr : rrs) {
            for (double tam : trail_act_mults) {
                for (double mrm : min_range_mults) {
                    ++cell;
                    double min_rng = cfg.MIN_RANGE * mrm;
                    std::fprintf(stderr, "[IDX-BKT] sweep %2d/%d  LB=%d RR=%.1f TACT=%.1fx MINRG=%.1f ...\n",
                                 cell, total_cells, lb, rr, tam, min_rng);

                    SweepResult r = run_one(all_ticks, cfg, lb, rr, tam, min_rng, oos_split_ms);
                    results.push_back(r);

                    std::printf(" %2d   %4d  %4.1f  %6.1f  %6.1f  | %5d %5.1f %6.2f %8.2f  | %5d %5.1f %6.2f %8.2f %8.2f\n",
                                cell, lb, rr, cfg.TRAIL_ACTIVATION * tam, min_rng,
                                r.is_trades, r.is_wr, r.is_pf, r.is_pnl,
                                r.oos_trades, r.oos_wr, r.oos_pf, r.oos_pnl, r.oos_maxdd);

                    // Track best OOS by PF (with minimum trade count)
                    if (r.oos_trades >= 20 && r.oos_pf > best_oos_pf) {
                        best_oos_pf = r.oos_pf;
                        best_idx = (int)results.size() - 1;
                    }
                }
            }
        }
    }

    std::printf("\n");

    // =========================================================================
    // BEST OOS FULL REPORT
    // =========================================================================
    if (best_idx < 0) {
        // Fall back to most trades if none hit PF threshold
        int max_trades = 0;
        for (int i = 0; i < (int)results.size(); ++i) {
            if (results[i].oos_trades > max_trades) {
                max_trades = results[i].oos_trades;
                best_idx = i;
            }
        }
    }

    if (best_idx >= 0) {
        const SweepResult& best = results[best_idx];
        const PerfMetrics& oos = best.oos_full;
        const PerfMetrics& is  = best.is_full;
        const PerfMetrics& all = best.overall_full;

        std::printf("================================================================\n");
        std::printf("  BEST OOS CONFIG: LB=%d  RR=%.1f  TRAIL_ACT=%.1f  MIN_RANGE=%.1f\n",
                    best.lookback, best.rr, best.trail_act, best.min_range);
        std::printf("================================================================\n\n");

        // OVERALL
        std::printf("-- OVERALL -------------------------------------------------------\n");
        std::printf("  Trades: %d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  Avg=$%.3f  MaxDD=$%.2f\n",
                    all.trades, all.wr(), all.pf(), all.total_pnl, all.avg(), all.max_dd);

        // LONG/SHORT
        std::printf("\n-- LONG ----------------------------------------------------------\n");
        std::printf("  Trades: %d  WR=%.1f%%  PF=%.2f  PnL=$%.2f\n",
                    all.long_trades, all.long_wr(), all.long_pf(), all.long_pnl);
        std::printf("-- SHORT ---------------------------------------------------------\n");
        std::printf("  Trades: %d  WR=%.1f%%  PF=%.2f  PnL=$%.2f\n",
                    all.short_trades, all.short_wr(), all.short_pf(), all.short_pnl);

        // EXIT BREAKDOWN
        std::printf("\n-- EXIT BREAKDOWN ------------------------------------------------\n");
        std::printf("  SL=%d  TRAIL=%d  BE=%d  BF=%d  BF_CONFIRM=%d  TIMEOUT=%d  OTHER=%d\n",
                    all.n_sl, all.n_trail, all.n_be, all.n_bf, all.n_bf_confirm,
                    all.n_timeout, all.n_other);

        // PER-HOUR PF
        std::printf("\n-- PF BY HOUR (UTC) ----------------------------------------------\n");
        std::printf("  Hour  Trades   WR%%    PF      PnL\n");
        for (int h = 0; h < 24; ++h) {
            if (all.hour_trades[h] > 0) {
                std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%8.2f  %s\n",
                            h, all.hour_trades[h],
                            all.hour_trades[h] > 0
                                ? 100.0 * all.hour_wins[h] / all.hour_trades[h] : 0.0,
                            all.hour_pf_at(h),
                            all.hour_pnl[h],
                            all.hour_pf_at(h) >= 1.0 ? " <<<" : "");
            }
        }

        // IS / OOS
        std::printf("\n================================================================\n");
        std::printf("  IN-SAMPLE / OUT-OF-SAMPLE (60/40 split)\n");
        std::printf("================================================================\n\n");
        std::printf("  IS:  trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  MaxDD=$%.2f\n",
                    is.trades, is.wr(), is.pf(), is.total_pnl, is.max_dd);
        std::printf("  OOS: trades=%d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  MaxDD=$%.2f\n",
                    oos.trades, oos.wr(), oos.pf(), oos.total_pnl, oos.max_dd);

        // PF decay
        if (is.trades > 0 && oos.trades > 0 && is.pf() > 0) {
            double decay = 1.0 - oos.pf() / is.pf();
            std::printf("  PF decay IS->OOS: %.0f%%\n", decay * 100.0);
        }

        // Verdict
        bool pass = (oos.trades >= 20 && oos.pf() >= 1.20);
        std::printf("\n  OOS VERDICT: %s\n",
                    pass ? "PASS (PF >= 1.20, trades >= 20)"
                         : "FAIL (needs PF >= 1.20 and trades >= 20)");
    } else {
        std::printf("  No configs produced any trades.\n");
    }

    std::printf("\n");
    return 0;
}
