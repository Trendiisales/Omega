// =============================================================================
// IndexVwapReversionBacktest.cpp -- standalone backtest for VWAPReversion on
// index tick data (SPX / NAS100)
// =============================================================================
// Self-contained single-file harness.  No external dependencies beyond
// standard C++17 headers.
//
// Compilation:
//   clang++ -std=c++17 -O3 -o backtest/idx_vwap_rev_bt \
//           backtest/IndexVwapReversionBacktest.cpp
//
// Usage:
//   ./backtest/idx_vwap_rev_bt --instrument SP \
//       ~/Tick/SPXUSD/HISTDATA_COM_ASCII_SPXUSD_T*/DAT_ASCII_SPXUSD_T_*.csv
//   ./backtest/idx_vwap_rev_bt --instrument NQ \
//       ~/Tick/Nas/HISTDATA_COM_ASCII_NSXUSD_T*/DAT_ASCII_NSXUSD_T_*.csv
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <cfloat>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <array>
#include <numeric>
#include <climits>
#include <deque>

// =============================================================================
// Tick format auto-detection and parsing
// =============================================================================
enum class TickFmt { UNKNOWN, HISTDATA, DUKA_BID_ASK, DUKA_ASK_BID, JFOREX };

struct TickRow {
    int64_t ts_ms = 0;
    double  bid   = 0.0;
    double  ask   = 0.0;
    double  mid() const { return 0.5 * (bid + ask); }
};

// Detect format from the first line of a file.
// Returns UNKNOWN if we can't determine.
static TickFmt detect_format(const std::string& line) {
    // JFOREX: header contains "Time"
    if (line.find("Time") != std::string::npos)
        return TickFmt::JFOREX;
    // DUKA_BID_ASK or DUKA_ASK_BID: header contains "timestamp"
    if (line.find("timestamp") != std::string::npos ||
        line.find("Timestamp") != std::string::npos) {
        // Heuristic: if "ask" appears before "bid" in the header, it's ASK_BID
        auto pa = line.find("ask");
        if (pa == std::string::npos) pa = line.find("Ask");
        auto pb = line.find("bid");
        if (pb == std::string::npos) pb = line.find("Bid");
        if (pa != std::string::npos && pb != std::string::npos && pa < pb)
            return TickFmt::DUKA_ASK_BID;
        return TickFmt::DUKA_BID_ASK;
    }
    // HISTDATA: no header, first line is data.  Format: YYYYMMDD HHMMSSmmm,...
    // Check: first 8 chars are digits, char 8 is space
    if (line.size() > 18) {
        bool all_digits = true;
        for (int i = 0; i < 8; ++i)
            if (!std::isdigit(static_cast<unsigned char>(line[i]))) all_digits = false;
        if (all_digits && line[8] == ' ')
            return TickFmt::HISTDATA;
    }
    return TickFmt::UNKNOWN;
}

// Convert YYYYMMDD HHMMSSmmm to epoch ms (UTC)
static int64_t parse_histdata_ts(const char* p) {
    // p points to "YYYYMMDD HHMMSSmmm"
    int Y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int M = (p[4]-'0')*10 + (p[5]-'0');
    int D = (p[6]-'0')*10 + (p[7]-'0');
    // p[8] = ' '
    int h = (p[9]-'0')*10 + (p[10]-'0');
    int m = (p[11]-'0')*10 + (p[12]-'0');
    int s = (p[13]-'0')*10 + (p[14]-'0');
    int ms = (p[15]-'0')*100 + (p[16]-'0')*10 + (p[17]-'0');

    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    tm.tm_isdst = 0;
#if defined(__APPLE__) || defined(__linux__)
    int64_t epoch_s = static_cast<int64_t>(timegm(&tm));
#else
    int64_t epoch_s = static_cast<int64_t>(_mkgmtime(&tm));
#endif
    return epoch_s * 1000LL + ms;
}

// Parse HISTDATA line: "YYYYMMDD HHMMSSmmm,bid,ask,0"
static bool parse_histdata(const char* p, TickRow& out) {
    if (std::strlen(p) < 20) return false;
    out.ts_ms = parse_histdata_ts(p);
    // Find first comma
    const char* c1 = std::strchr(p, ',');
    if (!c1) return false;
    char* e;
    out.bid = std::strtod(c1 + 1, &e);
    if (*e != ',') return false;
    out.ask = std::strtod(e + 1, &e);
    if (out.bid <= 0 || out.ask <= 0) return false;
    return true;
}

// Parse DUKA line: "timestamp_ms,col2,col3,..."
static bool parse_duka(const char* p, TickFmt fmt, TickRow& out) {
    char* e;
    out.ts_ms = std::strtoll(p, &e, 10);
    if (*e != ',') return false;
    double v1 = std::strtod(e + 1, &e);
    if (*e != ',') return false;
    double v2 = std::strtod(e + 1, &e);
    if (v1 <= 0 || v2 <= 0) return false;
    if (fmt == TickFmt::DUKA_BID_ASK) {
        out.bid = v1; out.ask = v2;
    } else {
        out.ask = v1; out.bid = v2;
    }
    return true;
}

// Parse JFOREX line: "DD.MM.YYYY HH:MM:SS.mmm EET,ask,bid,..."
// EET = UTC+2
static bool parse_jforex(const char* p, TickRow& out) {
    // "DD.MM.YYYY HH:MM:SS.mmm EET,ask,bid,..."
    int D, M, Y, h, m, s, ms;
    if (std::sscanf(p, "%2d.%2d.%4d %2d:%2d:%2d.%3d", &D, &M, &Y, &h, &m, &s, &ms) != 7)
        return false;
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    tm.tm_isdst = 0;
#if defined(__APPLE__) || defined(__linux__)
    int64_t epoch_s = static_cast<int64_t>(timegm(&tm));
#else
    int64_t epoch_s = static_cast<int64_t>(_mkgmtime(&tm));
#endif
    // EET is UTC+2, so subtract 2 hours
    epoch_s -= 7200;
    out.ts_ms = epoch_s * 1000LL + ms;

    // Find "EET," or the first comma after the timestamp
    const char* q = std::strstr(p, ",");
    if (!q) return false;
    char* e;
    out.ask = std::strtod(q + 1, &e);
    if (*e != ',') return false;
    out.bid = std::strtod(e + 1, &e);
    if (out.bid <= 0 || out.ask <= 0) return false;
    return true;
}

static bool parse_tick_line(const char* p, TickFmt fmt, TickRow& out) {
    switch (fmt) {
        case TickFmt::HISTDATA:     return parse_histdata(p, out);
        case TickFmt::DUKA_BID_ASK:
        case TickFmt::DUKA_ASK_BID: return parse_duka(p, fmt, out);
        case TickFmt::JFOREX:       return parse_jforex(p, out);
        default: return false;
    }
}

// =============================================================================
// Time helpers
// =============================================================================
struct UtcTime {
    int year, mon, mday, hour, min, sec, dow, yday;
};

static UtcTime ts_to_utc(int64_t ts_ms) {
    time_t t = static_cast<time_t>(ts_ms / 1000LL);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return { tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday, tm.tm_yday };
}

static bool is_weekend(int64_t ts_ms) {
    auto u = ts_to_utc(ts_ms);
    if (u.dow == 0 || u.dow == 6) return true;          // Sun or Sat
    if (u.dow == 5 && u.hour >= 21) return true;         // Fri after 21:00
    return false;
}

// Session open hour for this day (midnight UTC of same day)
static int64_t session_open_ms(int64_t ts_ms) {
    auto u = ts_to_utc(ts_ms);
    std::tm tm{};
    tm.tm_year = u.year - 1900;
    tm.tm_mon  = u.mon - 1;
    tm.tm_mday = u.mday;
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    tm.tm_isdst = 0;
#if defined(__APPLE__) || defined(__linux__)
    return static_cast<int64_t>(timegm(&tm)) * 1000LL;
#else
    return static_cast<int64_t>(_mkgmtime(&tm)) * 1000LL;
#endif
}

// =============================================================================
// Instrument config
// =============================================================================
struct InstrumentCfg {
    const char* name;
    double lot_size;
    double pnl_per_pt;
    double spread_typical;
    double price_lo;
    double price_hi;
};

static InstrumentCfg get_instrument(const std::string& code) {
    if (code == "SP")
        return { "SP (US500.F)", 0.01, 0.50, 0.5, 3000.0, 8000.0 };
    if (code == "NQ")
        return { "NQ (USTEC.F)", 0.01, 0.20, 1.0, 10000.0, 25000.0 };
    return { "UNKNOWN", 0.01, 0.50, 0.5, 0.0, 1e9 };
}

// =============================================================================
// Sweep config
// =============================================================================
struct SweepCfg {
    double EXTENSION_THRESH_PCT;
    double MAX_EXTENSION_PCT;
    double TP_FRACTION;
    double LOSS_CUT_PCT;
    double BE_ARM_PCT;
    double BE_BUFFER_PCT;
    double EXTENSION_SL_RATIO;
    double MAE_EXIT_RATIO;
    double HALF_LIFE_SEC;
    int    MAX_HOLD_SEC;
    int    COOLDOWN_SEC;
    int    ADVERSE_COOLDOWN_SEC;
    int    CONSEC_ADVERSE_COOLDOWN_SEC;
    int    TP_FLIP_COOLDOWN_SEC;
    int    MIN_SESSION_ELAPSED_MIN;
    int    SESSION_OPEN_HOUR;
    int    SESSION_CLOSE_HOUR;
    int    MOMENTUM_BUF_SIZE;
};

static SweepCfg default_cfg() {
    return {
        .EXTENSION_THRESH_PCT = 0.20,
        .MAX_EXTENSION_PCT    = 0.80,
        .TP_FRACTION          = 1.00,
        .LOSS_CUT_PCT         = 0.0,
        .BE_ARM_PCT           = 0.0,
        .BE_BUFFER_PCT        = 0.0,
        .EXTENSION_SL_RATIO   = 1.0,
        .MAE_EXIT_RATIO       = 0.50,
        .HALF_LIFE_SEC        = 7200.0,
        .MAX_HOLD_SEC         = 900,
        .COOLDOWN_SEC         = 180,
        .ADVERSE_COOLDOWN_SEC = 600,
        .CONSEC_ADVERSE_COOLDOWN_SEC = 1800,
        .TP_FLIP_COOLDOWN_SEC = 1200,
        .MIN_SESSION_ELAPSED_MIN = 120,
        .SESSION_OPEN_HOUR    = 8,
        .SESSION_CLOSE_HOUR   = 22,
        .MOMENTUM_BUF_SIZE    = 20,
    };
}

// =============================================================================
// Trade record
// =============================================================================
struct Trade {
    bool   active     = false;
    bool   is_long    = false;
    double entry_px   = 0.0;
    double tp_px      = 0.0;
    double sl_px      = 0.0;
    double tp_dist    = 0.0;   // absolute distance entry->tp at open
    double sl_offset  = 0.0;   // absolute SL distance from entry at open
    int64_t entry_ts  = 0;
    double mfe        = 0.0;   // max favorable excursion (pts)
    double mae        = 0.0;   // max adverse excursion (pts)
    bool   be_locked  = false;
    bool   mid_locked = false;
    bool   trailing   = false;
    double trail_peak = 0.0;   // best favorable price seen (for trailing)
    bool   extended   = false;  // progressive timeout extension
    const char* exit_reason = nullptr;
    int    exit_hour  = 0;
    double pnl_usd    = 0.0;

    // Close the trade
    void close(double exit_px, const char* reason, int64_t exit_ts,
               double pnl_per_pt, int hour) {
        double raw_pts = is_long ? (exit_px - entry_px) : (entry_px - exit_px);
        pnl_usd = raw_pts * pnl_per_pt;
        exit_reason = reason;
        exit_hour = hour;
        active = false;
    }
};

// =============================================================================
// Per-config metrics
// =============================================================================
struct Metrics {
    int    total_trades   = 0;
    int    wins           = 0;
    int    longs          = 0;
    int    shorts         = 0;
    int    long_wins      = 0;
    int    short_wins     = 0;
    double total_pnl      = 0.0;
    double gross_win      = 0.0;
    double gross_loss     = 0.0;
    double long_pnl       = 0.0;
    double short_pnl      = 0.0;
    double best_trade     = -1e18;
    double worst_trade    = 1e18;
    int    n_tp = 0, n_sl = 0, n_loss_cut = 0, n_be_cut = 0;
    int    n_mae_exit = 0, n_timeout = 0;
    std::array<int, 24>    hour_trades{};
    std::array<int, 24>    hour_wins{};
    std::array<double, 24> hour_pnl{};
    std::array<double, 24> hour_win_pnl{};
    std::array<double, 24> hour_loss_pnl{};

    void record(const Trade& t) {
        total_trades++;
        total_pnl += t.pnl_usd;
        if (t.pnl_usd > 0) {
            wins++;
            gross_win += t.pnl_usd;
        } else {
            gross_loss += std::fabs(t.pnl_usd);
        }
        if (t.is_long) {
            longs++;
            long_pnl += t.pnl_usd;
            if (t.pnl_usd > 0) long_wins++;
        } else {
            shorts++;
            short_pnl += t.pnl_usd;
            if (t.pnl_usd > 0) short_wins++;
        }
        if (t.pnl_usd > best_trade)  best_trade  = t.pnl_usd;
        if (t.pnl_usd < worst_trade) worst_trade = t.pnl_usd;

        const char* r = t.exit_reason;
        if      (std::strcmp(r, "TP_HIT") == 0)    n_tp++;
        else if (std::strcmp(r, "SL_HIT") == 0)    n_sl++;
        else if (std::strcmp(r, "LOSS_CUT") == 0)  n_loss_cut++;
        else if (std::strcmp(r, "BE_CUT") == 0)    n_be_cut++;
        else if (std::strcmp(r, "MAE_EXIT") == 0)   n_mae_exit++;
        else if (std::strcmp(r, "TIMEOUT") == 0)   n_timeout++;

        int h = t.exit_hour;
        if (h >= 0 && h < 24) {
            hour_trades[h]++;
            hour_pnl[h] += t.pnl_usd;
            if (t.pnl_usd > 0) {
                hour_wins[h]++;
                hour_win_pnl[h] += t.pnl_usd;
            } else {
                hour_loss_pnl[h] += std::fabs(t.pnl_usd);
            }
        }
    }

    double profit_factor() const {
        if (gross_loss < 1e-12) return (gross_win > 0 ? 999.9 : 0.0);
        return gross_win / gross_loss;
    }
    double win_rate() const {
        return total_trades > 0 ? 100.0 * wins / total_trades : 0.0;
    }
    double avg_pnl() const {
        return total_trades > 0 ? total_pnl / total_trades : 0.0;
    }
};

// =============================================================================
// EWM VWAP state
// =============================================================================
struct EwmVwap {
    double vwap       = 0.0;
    int64_t last_ts   = 0;
    bool   seeded     = false;

    void reset(double mid) {
        vwap   = mid;
        seeded = true;
        // last_ts left untouched; will be set on next tick
    }

    void update(double mid, int64_t ts_ms, double half_life_sec) {
        if (!seeded) {
            vwap = mid;
            last_ts = ts_ms;
            seeded = true;
            return;
        }
        double dt_sec = (ts_ms - last_ts) / 1000.0;
        if (dt_sec <= 0) { last_ts = ts_ms; return; }

        // Gap > 1hr: reset (reconnect safety)
        if (dt_sec > 3600.0) {
            vwap = mid;
            last_ts = ts_ms;
            return;
        }

        double alpha = 1.0 - std::exp(-dt_sec / half_life_sec);
        vwap += alpha * (mid - vwap);
        last_ts = ts_ms;
    }
};

// =============================================================================
// Momentum ring buffer
// =============================================================================
struct MomentumBuf {
    std::vector<double> buf;
    int head   = 0;
    int count  = 0;
    int capacity = 0;

    void init(int n) {
        capacity = n;
        buf.assign(n, 0.0);
        head = 0;
        count = 0;
    }

    void push(double val) {
        buf[head] = val;
        head = (head + 1) % capacity;
        if (count < capacity) count++;
    }

    bool full() const { return count >= capacity; }

    double oldest() const {
        if (count < capacity) return buf[0];
        return buf[head % capacity];
    }

    double newest() const {
        return buf[(head - 1 + capacity) % capacity];
    }

    // trend_move = newest - oldest
    double trend_move() const {
        if (count < 2) return 0.0;
        return newest() - oldest();
    }
};

// =============================================================================
// Cooldown state
// =============================================================================
struct CooldownState {
    int64_t last_exit_ts               = 0;
    bool    last_exit_adverse          = false;
    int     consecutive_same_adverse   = 0;
    bool    last_adverse_dir_long      = false;
    int64_t last_tp_ts                 = 0;
    bool    last_tp_dir_long           = false;

    void record_exit(int64_t ts, const char* reason, bool was_long) {
        last_exit_ts = ts;
        bool adverse = (std::strcmp(reason, "SL_HIT") == 0 ||
                        std::strcmp(reason, "LOSS_CUT") == 0 ||
                        std::strcmp(reason, "MAE_EXIT") == 0 ||
                        std::strcmp(reason, "TIMEOUT") == 0);
        if (adverse) {
            if (last_exit_adverse && last_adverse_dir_long == was_long)
                consecutive_same_adverse++;
            else
                consecutive_same_adverse = 1;
            last_adverse_dir_long = was_long;
        } else {
            consecutive_same_adverse = 0;
        }
        last_exit_adverse = adverse;

        if (std::strcmp(reason, "TP_HIT") == 0) {
            last_tp_ts = ts;
            last_tp_dir_long = was_long;
        }
    }

    bool can_enter(int64_t ts, bool want_long, const SweepCfg& cfg) const {
        if (last_exit_ts == 0) return true;
        double elapsed = (ts - last_exit_ts) / 1000.0;

        // Basic cooldown
        if (elapsed < cfg.COOLDOWN_SEC) return false;

        // Adverse cooldown
        if (last_exit_adverse && elapsed < cfg.ADVERSE_COOLDOWN_SEC)
            return false;

        // Consecutive same-direction adverse: block that direction
        if (consecutive_same_adverse >= 2 &&
            last_adverse_dir_long == want_long &&
            elapsed < cfg.CONSEC_ADVERSE_COOLDOWN_SEC)
            return false;

        // TP flip cooldown: blocks OPPOSITE direction after a TP hit
        if (last_tp_ts > 0) {
            double tp_elapsed = (ts - last_tp_ts) / 1000.0;
            if (tp_elapsed < cfg.TP_FLIP_COOLDOWN_SEC &&
                last_tp_dir_long != want_long)
                return false;
        }

        return true;
    }
};

// =============================================================================
// Run one config over pre-loaded ticks, fill IS and OOS metrics
// =============================================================================
static void run_config(const std::vector<TickRow>& ticks,
                       const SweepCfg& cfg,
                       const InstrumentCfg& inst,
                       int64_t is_oos_split_ts,
                       Metrics& is_met, Metrics& oos_met,
                       Metrics& all_met) {
    is_met  = Metrics{};
    oos_met = Metrics{};
    all_met = Metrics{};

    EwmVwap       vwap;
    MomentumBuf   mom;
    mom.init(cfg.MOMENTUM_BUF_SIZE);
    CooldownState cooldown;
    Trade         pos;
    double        prev_mid = 0.0;
    int64_t       session_open = 0;  // first tick of today's session

    int cur_yday = -1;

    for (size_t i = 0; i < ticks.size(); ++i) {
        const TickRow& tk = ticks[i];
        double mid = tk.mid();
        auto utc = ts_to_utc(tk.ts_ms);

        // Weekend gate
        if (is_weekend(tk.ts_ms)) { prev_mid = mid; continue; }

        // Price sanity
        if (mid < inst.price_lo || mid > inst.price_hi) { prev_mid = mid; continue; }

        // Day boundary: reset VWAP
        if (utc.yday != cur_yday) {
            cur_yday = utc.yday;
            vwap.reset(mid);
            session_open = tk.ts_ms;
        }

        // Update VWAP
        vwap.update(mid, tk.ts_ms, cfg.HALF_LIFE_SEC);

        // Momentum buffer
        mom.push(mid);

        // ── Manage open position ──
        if (pos.active) {
            // Track MFE / MAE
            double fav = pos.is_long ? (tk.bid - pos.entry_px) : (pos.entry_px - tk.ask);
            double adv = pos.is_long ? (pos.entry_px - tk.bid) : (tk.ask - pos.entry_px);
            if (fav > pos.mfe) { pos.mfe = fav; pos.trail_peak = pos.is_long ? tk.bid : tk.ask; }
            if (adv > pos.mae) pos.mae = adv;

            int hold_sec = static_cast<int>((tk.ts_ms - pos.entry_ts) / 1000LL);
            bool closed = false;

            // Phase 1: BE RATCHET
            if (!closed && cfg.BE_ARM_PCT > 0.0) {
                double arm_pts = pos.entry_px * cfg.BE_ARM_PCT / 100.0;
                double buf_pts = pos.entry_px * cfg.BE_BUFFER_PCT / 100.0;
                double move = pos.is_long ? (tk.bid - pos.entry_px) : (pos.entry_px - tk.ask);
                if (pos.mfe >= arm_pts && move <= buf_pts) {
                    pos.close(pos.is_long ? tk.bid : tk.ask, "BE_CUT",
                              tk.ts_ms, inst.pnl_per_pt, utc.hour);
                    closed = true;
                }
            }

            // Phase 2: COLD LOSS CUT
            if (!closed && cfg.LOSS_CUT_PCT > 0.0) {
                double lc_dist = pos.entry_px * cfg.LOSS_CUT_PCT / 100.0;
                if (adv >= lc_dist) {
                    pos.close(pos.is_long ? tk.bid : tk.ask, "LOSS_CUT",
                              tk.ts_ms, inst.pnl_per_pt, utc.hour);
                    closed = true;
                }
            }

            // Phase 3: Progressive timeout
            if (!closed) {
                int effective_max = cfg.MAX_HOLD_SEC;
                // Winners never time out
                double cur_move = pos.is_long ? (tk.bid - pos.entry_px) : (pos.entry_px - tk.ask);
                if (cur_move > 0) {
                    effective_max = INT_MAX;
                } else if (!pos.extended && hold_sec >= cfg.MAX_HOLD_SEC) {
                    // Check progress toward TP
                    double progress = (pos.tp_dist > 1e-9) ? (pos.mfe / pos.tp_dist) : 0.0;
                    if (progress > 0.30) {
                        pos.extended = true;
                        effective_max = cfg.MAX_HOLD_SEC + 300; // extend 5 min
                    }
                }
                if (hold_sec >= effective_max && cur_move <= 0) {
                    pos.close(pos.is_long ? tk.bid : tk.ask, "TIMEOUT",
                              tk.ts_ms, inst.pnl_per_pt, utc.hour);
                    closed = true;
                }
            }

            // Phase 4: MAE_EXIT
            if (!closed) {
                if (adv > pos.tp_dist * cfg.MAE_EXIT_RATIO) {
                    pos.close(pos.is_long ? tk.bid : tk.ask, "MAE_EXIT",
                              tk.ts_ms, inst.pnl_per_pt, utc.hour);
                    closed = true;
                }
            }

            // CrossPosition base manage: BE lock, mid-lock, trail, SL/TP
            if (!closed) {
                double fav_frac = (pos.tp_dist > 1e-9) ? (pos.mfe / pos.tp_dist) : 0.0;

                // BE lock at 40% of TP distance
                if (!pos.be_locked && fav_frac >= 0.40) {
                    pos.be_locked = true;
                    double spread = inst.spread_typical;
                    pos.sl_px = pos.is_long ? (pos.entry_px + spread)
                                            : (pos.entry_px - spread);
                }

                // Mid-lock at 50% of TP distance
                if (!pos.mid_locked && fav_frac >= 0.50) {
                    pos.mid_locked = true;
                    double mid_lock_offset = pos.tp_dist * 0.25;
                    pos.sl_px = pos.is_long ? (pos.entry_px + mid_lock_offset)
                                            : (pos.entry_px - mid_lock_offset);
                }

                // Trail at 60% of TP distance
                if (fav_frac >= 0.60) {
                    pos.trailing = true;
                    double trail_offset = pos.tp_dist * 0.20;
                    double new_sl = pos.is_long
                        ? (pos.trail_peak - trail_offset)
                        : (pos.trail_peak + trail_offset);
                    // Only ratchet SL forward, never backward
                    if (pos.is_long && new_sl > pos.sl_px)
                        pos.sl_px = new_sl;
                    if (!pos.is_long && new_sl < pos.sl_px)
                        pos.sl_px = new_sl;
                }

                // TP hit: bid >= tp (long) or ask <= tp (short)
                bool tp_hit = pos.is_long ? (tk.bid >= pos.tp_px) : (tk.ask <= pos.tp_px);
                bool sl_hit = pos.is_long ? (tk.bid <= pos.sl_px) : (tk.ask >= pos.sl_px);

                if (tp_hit) {
                    // Mean-reversion: close AT VWAP (tp_px is the target)
                    pos.close(pos.tp_px, "TP_HIT", tk.ts_ms, inst.pnl_per_pt, utc.hour);
                    closed = true;
                } else if (sl_hit) {
                    pos.close(pos.sl_px, "SL_HIT", tk.ts_ms, inst.pnl_per_pt, utc.hour);
                    closed = true;
                }
            }

            if (closed) {
                cooldown.record_exit(tk.ts_ms, pos.exit_reason, pos.is_long);
                all_met.record(pos);
                if (pos.entry_ts < is_oos_split_ts)
                    is_met.record(pos);
                else
                    oos_met.record(pos);
            }

            prev_mid = mid;
            continue;
        }

        // ── Entry logic (no open position) ──

        // Session gate: hours 8-22 UTC
        if (utc.hour < cfg.SESSION_OPEN_HOUR || utc.hour >= cfg.SESSION_CLOSE_HOUR) {
            prev_mid = mid;
            continue;
        }

        // Min session elapsed
        if (session_open > 0) {
            double elapsed_min = (tk.ts_ms - session_open) / 60000.0;
            if (elapsed_min < cfg.MIN_SESSION_ELAPSED_MIN) {
                prev_mid = mid;
                continue;
            }
        }

        // Need prev_mid and momentum buffer
        if (prev_mid <= 0 || !mom.full()) {
            prev_mid = mid;
            continue;
        }

        // VWAP must be seeded
        if (!vwap.seeded || vwap.vwap <= 0) {
            prev_mid = mid;
            continue;
        }

        // Deviation from VWAP
        double deviation_pct = (mid - vwap.vwap) / vwap.vwap * 100.0;
        double abs_dev = std::fabs(deviation_pct);

        if (abs_dev < cfg.EXTENSION_THRESH_PCT || abs_dev > cfg.MAX_EXTENSION_PCT) {
            prev_mid = mid;
            continue;
        }

        // Direction: below VWAP = long, above VWAP = short
        bool want_long = (deviation_pct < 0);

        // Cooldown check
        if (!cooldown.can_enter(tk.ts_ms, want_long, cfg)) {
            prev_mid = mid;
            continue;
        }

        // Momentum filter: block if price still trending away from VWAP
        double trend_move = mom.trend_move();
        double half_thresh = cfg.EXTENSION_THRESH_PCT / 2.0;
        // Trending away: long (below vwap) but price still falling
        //                short (above vwap) but price still rising
        // Normalize trend_move to pct of price
        double trend_pct = (vwap.vwap > 0) ? (trend_move / vwap.vwap * 100.0) : 0.0;
        if (want_long && trend_pct < -half_thresh) {
            prev_mid = mid;
            continue;
        }
        if (!want_long && trend_pct > half_thresh) {
            prev_mid = mid;
            continue;
        }

        // Reversal tick: above VWAP -> mid < prev_mid; below VWAP -> mid > prev_mid
        if (want_long && mid <= prev_mid) {
            prev_mid = mid;
            continue;
        }
        if (!want_long && mid >= prev_mid) {
            prev_mid = mid;
            continue;
        }

        // Compute TP / SL
        double extension_abs = std::fabs(mid - vwap.vwap);
        double tp = mid + (vwap.vwap - mid) * cfg.TP_FRACTION;
        double sl_offset = extension_abs * cfg.EXTENSION_SL_RATIO;
        double sl = want_long ? (mid - sl_offset) : (mid + sl_offset);

        double tp_dist = std::fabs(tp - mid);
        if (tp_dist <= 0 || tp_dist < sl_offset * 0.5) {
            prev_mid = mid;
            continue;
        }

        // Open position
        pos.active    = true;
        pos.is_long   = want_long;
        pos.entry_px  = mid;
        pos.tp_px     = tp;
        pos.sl_px     = sl;
        pos.tp_dist   = tp_dist;
        pos.sl_offset = sl_offset;
        pos.entry_ts  = tk.ts_ms;
        pos.mfe       = 0.0;
        pos.mae       = 0.0;
        pos.be_locked = false;
        pos.mid_locked = false;
        pos.trailing  = false;
        pos.trail_peak = mid;
        pos.extended  = false;

        prev_mid = mid;
    }

    // Force close at end of data
    if (pos.active) {
        const TickRow& last = ticks.back();
        auto utc = ts_to_utc(last.ts_ms);
        pos.close(pos.is_long ? last.bid : last.ask, "TIMEOUT",
                  last.ts_ms, inst.pnl_per_pt, utc.hour);
        cooldown.record_exit(last.ts_ms, pos.exit_reason, pos.is_long);
        all_met.record(pos);
        if (pos.entry_ts < is_oos_split_ts)
            is_met.record(pos);
        else
            oos_met.record(pos);
    }
}

// =============================================================================
// Print helpers
// =============================================================================
static void print_metrics_line(const char* label, const Metrics& m) {
    std::printf("  %-12s  Trades=%4d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  Avg=$%.2f\n",
                label, m.total_trades, m.win_rate(), m.profit_factor(),
                m.total_pnl, m.avg_pnl());
}

static void print_full_diagnostics(const char* tag, const SweepCfg& cfg,
                                   const Metrics& all, const Metrics& is_m,
                                   const Metrics& oos_m) {
    std::printf("\n");
    std::printf("=================================================================\n");
    std::printf("  %s  FULL DIAGNOSTICS\n", tag);
    std::printf("=================================================================\n");
    std::printf("  Config: EXT_THRESH=%.2f  MAX_EXT=%.2f  TP_FRAC=%.2f  "
                "LOSS_CUT=%.2f  BE_ARM=%.2f  BE_BUF=%.2f\n",
                cfg.EXTENSION_THRESH_PCT, cfg.MAX_EXTENSION_PCT,
                cfg.TP_FRACTION, cfg.LOSS_CUT_PCT,
                cfg.BE_ARM_PCT, cfg.BE_BUFFER_PCT);
    std::printf("  MAE_EXIT_RATIO=%.2f  SL_RATIO=%.2f  MAX_HOLD=%ds  HALF_LIFE=%.0fs\n",
                cfg.MAE_EXIT_RATIO, cfg.EXTENSION_SL_RATIO,
                cfg.MAX_HOLD_SEC, cfg.HALF_LIFE_SEC);
    std::printf("\n");

    // Overall
    std::printf("-- OVERALL -------------------------------------------------------\n");
    std::printf("  Trades: %d  |  Wins: %d  |  WR: %.1f%%  |  PF: %.2f\n",
                all.total_trades, all.wins, all.win_rate(), all.profit_factor());
    std::printf("  PnL: $%.2f  |  Avg: $%.4f  |  Best: $%.4f  |  Worst: $%.4f\n",
                all.total_pnl, all.avg_pnl(),
                all.total_trades > 0 ? all.best_trade : 0.0,
                all.total_trades > 0 ? all.worst_trade : 0.0);
    std::printf("\n");

    // Long / Short breakdown
    std::printf("-- LONG / SHORT --------------------------------------------------\n");
    std::printf("  LONG:   %4d trades  WR=%.1f%%  PnL=$%.2f\n",
                all.longs,
                all.longs > 0 ? 100.0 * all.long_wins / all.longs : 0.0,
                all.long_pnl);
    std::printf("  SHORT:  %4d trades  WR=%.1f%%  PnL=$%.2f\n",
                all.shorts,
                all.shorts > 0 ? 100.0 * all.short_wins / all.shorts : 0.0,
                all.short_pnl);
    std::printf("\n");

    // Exit breakdown
    std::printf("-- EXIT BREAKDOWN ------------------------------------------------\n");
    std::printf("  TP_HIT:    %4d  |  SL_HIT:    %4d  |  LOSS_CUT: %4d\n",
                all.n_tp, all.n_sl, all.n_loss_cut);
    std::printf("  BE_CUT:    %4d  |  MAE_EXIT:  %4d  |  TIMEOUT:  %4d\n",
                all.n_be_cut, all.n_mae_exit, all.n_timeout);
    std::printf("\n");

    // Per-hour PF table
    std::printf("-- PF BY HOUR (UTC) ----------------------------------------------\n");
    std::printf("  Hour  Trades   WR%%    PF       PnL\n");
    for (int h = 0; h < 24; ++h) {
        if (all.hour_trades[h] > 0) {
            double wr = 100.0 * all.hour_wins[h] / all.hour_trades[h];
            double pf = (all.hour_loss_pnl[h] > 1e-12)
                        ? (all.hour_win_pnl[h] / all.hour_loss_pnl[h]) : 999.9;
            std::printf("  %02d:00  %5d  %5.1f  %5.2f  $%8.2f%s\n",
                        h, all.hour_trades[h], wr, pf, all.hour_pnl[h],
                        pf >= 1.0 ? "  <<<" : "");
        }
    }
    std::printf("\n");

    // IS / OOS
    std::printf("-- IS / OOS (60/40 split) ----------------------------------------\n");
    print_metrics_line("IS", is_m);
    print_metrics_line("OOS", oos_m);
    if (is_m.total_trades > 0 && oos_m.total_trades > 0) {
        double is_pf  = is_m.profit_factor();
        double oos_pf = oos_m.profit_factor();
        double decay  = (is_pf > 1e-6) ? (1.0 - oos_pf / is_pf) : 0.0;
        std::printf("  PF decay IS->OOS: %.0f%%\n", decay * 100.0);
    }
    bool oos_pass = (oos_m.total_trades >= 20 && oos_m.profit_factor() >= 1.20);
    std::printf("\n  OOS VERDICT: %s\n",
                oos_pass ? "PASS (PF >= 1.20 && trades >= 20)"
                         : "FAIL");
    std::printf("=================================================================\n\n");
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: idx_vwap_rev_bt --instrument <SP|NQ> <file1.csv> [file2.csv ...]\n\n"
            "  --instrument <SP|NQ>   Select instrument (required)\n"
            "  SP = US500.F / SPXUSD,  NQ = USTEC.F / NSXUSD / NAS100\n\n"
            "Example:\n"
            "  ./idx_vwap_rev_bt --instrument SP ~/Tick/SPXUSD/*.csv\n"
            "  ./idx_vwap_rev_bt --instrument NQ ~/Tick/Nas/*.csv\n");
        return 1;
    }

    // Parse args
    std::string instrument;
    std::vector<std::string> csv_files;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            instrument = argv[++i];
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "[ERROR] Unknown option: %s\n", argv[i]);
            return 1;
        } else {
            csv_files.push_back(argv[i]);
        }
    }

    if (instrument.empty()) {
        std::fprintf(stderr, "[ERROR] --instrument is required (SP or NQ)\n");
        return 1;
    }
    if (csv_files.empty()) {
        std::fprintf(stderr, "[ERROR] No CSV files specified\n");
        return 1;
    }

    InstrumentCfg inst = get_instrument(instrument);
    std::fprintf(stderr, "[INFO] Instrument: %s  |  PnL/pt: $%.2f  |  Spread: %.1f pt\n",
                 inst.name, inst.pnl_per_pt, inst.spread_typical);
    std::fprintf(stderr, "[INFO] %zu CSV file(s) to process\n", csv_files.size());

    // ── Load all ticks from all CSV files ──
    std::vector<TickRow> all_ticks;
    all_ticks.reserve(100000000);  // pre-allocate for ~100M ticks
    size_t total_lines = 0;
    size_t skipped_lines = 0;

    for (size_t fi = 0; fi < csv_files.size(); ++fi) {
        std::ifstream f(csv_files[fi]);
        if (!f.is_open()) {
            std::fprintf(stderr, "[WARN] Cannot open: %s -- skipping\n",
                         csv_files[fi].c_str());
            continue;
        }

        std::string first_line;
        if (!std::getline(f, first_line)) continue;

        // Strip trailing \r
        if (!first_line.empty() && first_line.back() == '\r')
            first_line.pop_back();

        TickFmt fmt = detect_format(first_line);
        if (fmt == TickFmt::UNKNOWN) {
            std::fprintf(stderr, "[WARN] Unknown format in %s -- skipping\n",
                         csv_files[fi].c_str());
            continue;
        }

        // For HISTDATA, first line IS data (no header)
        bool first_line_is_data = (fmt == TickFmt::HISTDATA);

        if (first_line_is_data) {
            TickRow tk;
            if (parse_tick_line(first_line.c_str(), fmt, tk)) {
                if (tk.mid() >= inst.price_lo && tk.mid() <= inst.price_hi)
                    all_ticks.push_back(tk);
                else
                    skipped_lines++;
            } else {
                skipped_lines++;
            }
            total_lines++;
        }

        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            total_lines++;

            TickRow tk;
            if (parse_tick_line(line.c_str(), fmt, tk)) {
                if (tk.mid() >= inst.price_lo && tk.mid() <= inst.price_hi)
                    all_ticks.push_back(tk);
                else
                    skipped_lines++;
            } else {
                skipped_lines++;
            }

            if (total_lines % 10000000 == 0)
                std::fprintf(stderr, "[INFO] %zuM lines read (%zuM ticks kept)\n",
                             total_lines / 1000000, all_ticks.size() / 1000000);
        }

        std::fprintf(stderr, "[INFO] File %zu/%zu done: %s  (fmt=%s)\n",
                     fi + 1, csv_files.size(), csv_files[fi].c_str(),
                     fmt == TickFmt::HISTDATA ? "HISTDATA" :
                     fmt == TickFmt::DUKA_BID_ASK ? "DUKA_BID_ASK" :
                     fmt == TickFmt::DUKA_ASK_BID ? "DUKA_ASK_BID" :
                     fmt == TickFmt::JFOREX ? "JFOREX" : "?");
    }

    std::fprintf(stderr, "[INFO] Total ticks loaded: %zu  (skipped: %zu)\n",
                 all_ticks.size(), skipped_lines);

    if (all_ticks.size() < 100) {
        std::fprintf(stderr, "[ERROR] Too few ticks (%zu). Check file paths and price sanity range.\n",
                     all_ticks.size());
        return 1;
    }

    // Sort by timestamp (files should be chronological, but be safe)
    std::sort(all_ticks.begin(), all_ticks.end(),
              [](const TickRow& a, const TickRow& b) { return a.ts_ms < b.ts_ms; });

    // IS/OOS split: 60/40 by calendar time
    int64_t first_ts = all_ticks.front().ts_ms;
    int64_t last_ts  = all_ticks.back().ts_ms;
    int64_t is_oos_split_ts = first_ts + static_cast<int64_t>(0.60 * (last_ts - first_ts));

    auto split_utc = ts_to_utc(is_oos_split_ts);
    auto first_utc = ts_to_utc(first_ts);
    auto last_utc  = ts_to_utc(last_ts);
    std::fprintf(stderr, "[INFO] Data range: %04d-%02d-%02d to %04d-%02d-%02d  (%.1f days)\n",
                 first_utc.year, first_utc.mon, first_utc.mday,
                 last_utc.year, last_utc.mon, last_utc.mday,
                 (last_ts - first_ts) / 86400000.0);
    std::fprintf(stderr, "[INFO] IS/OOS split at: %04d-%02d-%02d\n",
                 split_utc.year, split_utc.mon, split_utc.mday);

    // ── Build sweep grid ──
    // EXTENSION_THRESH_PCT: {0.15, 0.20, 0.25}
    // TP_FRACTION: {0.80, 1.00}
    // LOSS_CUT_PCT: {0.0, 0.06, 0.08, 0.10}
    struct SweepEntry {
        SweepCfg cfg;
        Metrics  is_met, oos_met, all_met;
    };

    double ext_vals[]  = { 0.15, 0.20, 0.25 };
    double tp_vals[]   = { 0.80, 1.00 };
    double lc_vals[]   = { 0.0, 0.06, 0.08, 0.10 };

    std::vector<SweepEntry> sweep;
    int n_configs = 3 * 2 * 4;
    sweep.resize(n_configs);

    int idx = 0;
    for (double ext : ext_vals) {
        for (double tp : tp_vals) {
            for (double lc : lc_vals) {
                SweepCfg c = default_cfg();
                c.EXTENSION_THRESH_PCT = ext;
                c.TP_FRACTION = tp;
                c.LOSS_CUT_PCT = lc;
                // If LOSS_CUT is on, also set BE params
                if (lc > 0.0) {
                    c.BE_ARM_PCT    = 0.05;
                    c.BE_BUFFER_PCT = 0.02;
                }
                sweep[idx].cfg = c;
                idx++;
            }
        }
    }

    std::fprintf(stderr, "[INFO] Running %d configs...\n", n_configs);

    // ── Run sweep ──
    for (int i = 0; i < n_configs; ++i) {
        run_config(all_ticks, sweep[i].cfg, inst, is_oos_split_ts,
                   sweep[i].is_met, sweep[i].oos_met, sweep[i].all_met);
        std::fprintf(stderr, "[INFO] Config %d/%d done: trades=%d\n",
                     i + 1, n_configs, sweep[i].all_met.total_trades);
    }

    // ── Print one-line summaries ──
    std::printf("\n");
    std::printf("=================================================================\n");
    std::printf("  INDEX VWAP REVERSION SWEEP -- %s\n", inst.name);
    std::printf("  Data: %04d-%02d-%02d to %04d-%02d-%02d  (%zu ticks)\n",
                first_utc.year, first_utc.mon, first_utc.mday,
                last_utc.year, last_utc.mon, last_utc.mday,
                all_ticks.size());
    std::printf("=================================================================\n\n");

    std::printf("%-6s %-6s %-6s  | IS: Trades   PF   WR%%  | OOS: Trades   PF   WR%%  | Verdict\n",
                "EXT", "TP_FR", "LC");
    std::printf("------  ------  ------  | ------  -----  -----  | ------  -----  -----  | -------\n");

    int best_oos_idx = -1;
    double best_oos_pf = -1.0;

    for (int i = 0; i < n_configs; ++i) {
        const auto& c = sweep[i].cfg;
        const auto& is_m = sweep[i].is_met;
        const auto& oos_m = sweep[i].oos_met;

        bool oos_pass = (oos_m.total_trades >= 20 && oos_m.profit_factor() >= 1.20);
        const char* verdict = oos_pass ? "PASS" : "FAIL";

        std::printf("%.2f   %.2f   %.2f   |  %4d   %5.2f  %5.1f  |  %4d   %5.2f  %5.1f  | %s\n",
                    c.EXTENSION_THRESH_PCT, c.TP_FRACTION, c.LOSS_CUT_PCT,
                    is_m.total_trades, is_m.profit_factor(), is_m.win_rate(),
                    oos_m.total_trades, oos_m.profit_factor(), oos_m.win_rate(),
                    verdict);

        // Track best OOS config (by PF, with minimum trade threshold)
        if (oos_m.total_trades >= 10) {
            double oos_pf = oos_m.profit_factor();
            if (oos_pf > best_oos_pf) {
                best_oos_pf = oos_pf;
                best_oos_idx = i;
            }
        }
    }

    std::printf("\n");

    // ── Print full diagnostics for best OOS config ──
    if (best_oos_idx >= 0) {
        print_full_diagnostics("BEST OOS CONFIG",
                               sweep[best_oos_idx].cfg,
                               sweep[best_oos_idx].all_met,
                               sweep[best_oos_idx].is_met,
                               sweep[best_oos_idx].oos_met);
    } else {
        std::printf("[WARN] No config had >= 10 OOS trades. Cannot select best.\n");
    }

    return 0;
}
