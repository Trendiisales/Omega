// =============================================================================
// IndexFlowBacktest.cpp  (v2 — real tick replay, multi-config sweep)
// =============================================================================
// Standalone backtest harness that replays IndexFlowEngine strategy logic on
// real index tick data (HISTDATA / DUKA / JFOREX CSV formats).
//
// BUILD:
//   clang++ -std=c++17 -O3 -o backtest/idx_flow_bt backtest/IndexFlowBacktest.cpp
//
// USAGE:
//   ./backtest/idx_flow_bt --instrument SP ~/Tick/SPXUSD/HISTDATA_COM_ASCII_SPXUSD_T*/DAT_ASCII_SPXUSD_T_*.csv
//   ./backtest/idx_flow_bt --instrument NQ ~/Tick/Nas/HISTDATA_COM_ASCII_NSXUSD_T*/DAT_ASCII_NSXUSD_T_*.csv
//
// TICK FORMAT AUTO-DETECTION:
//   HISTDATA:      YYYYMMDD HHMMSSmmm,bid,ask,0         (no header)
//   DUKA_BID_ASK:  timestamp_ms,bid,ask,...              (header with "timestamp")
//   DUKA_ASK_BID:  timestamp_ms,ask,bid,...              (header, ask before bid)
//   JFOREX:        Time (EET),Ask,Bid,...                (header with "Time", EET +2h)
//
// SWEEP:
//   drift_threshold:    {0.5, 0.8, 1.2} SP; {1.5, 2.0, 3.0} NQ
//   drift_persist_ticks: {12, 20, 30}
//   LOSS_CUT_PCT:       {0.0, 0.05, 0.07, 0.10}
//   = 36 configs per instrument.
//
// OUTPUT:
//   1. Per-config one-liner (params, IS trades/PF, OOS trades/PF, verdict)
//   2. Best OOS config full report (overall, long/short, exit breakdown,
//      per-hour PF, IS/OOS with decay)
//   3. OOS verdict: PASS if PF >= 1.20 and trades >= 20
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Tick data structures
// ─────────────────────────────────────────────────────────────────────────────
struct Tick {
    int64_t ts_ms;   // milliseconds since epoch UTC
    double  bid;
    double  ask;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tick format detection and parsing
// ─────────────────────────────────────────────────────────────────────────────
enum TickFormat { FMT_HISTDATA, FMT_DUKA_BID_ASK, FMT_DUKA_ASK_BID, FMT_JFOREX, FMT_UNKNOWN };

static TickFormat detect_format(const char* first_line) {
    std::string s(first_line);
    // JFOREX: header starts with "Time"
    if (s.find("Time") != std::string::npos && s.find("EET") != std::string::npos)
        return FMT_JFOREX;
    // DUKA: header contains "timestamp" (case-insensitive check)
    {
        std::string lower = s;
        for (auto& c : lower) c = (char)tolower(c);
        if (lower.find("timestamp") != std::string::npos) {
            // Determine bid/ask vs ask/bid order from header
            auto pos_ask = lower.find("ask");
            auto pos_bid = lower.find("bid");
            if (pos_ask != std::string::npos && pos_bid != std::string::npos)
                return (pos_ask < pos_bid) ? FMT_DUKA_ASK_BID : FMT_DUKA_BID_ASK;
            return FMT_DUKA_BID_ASK;
        }
    }
    // HISTDATA: first 8 chars should be digits (YYYYMMDD)
    if (s.size() >= 8) {
        bool all_digit = true;
        for (int i = 0; i < 8; ++i)
            if (!isdigit((unsigned char)s[i])) { all_digit = false; break; }
        if (all_digit) return FMT_HISTDATA;
    }
    return FMT_UNKNOWN;
}

// Parse YYYYMMDD HHMMSSmmm -> epoch ms UTC
static int64_t parse_histdata_ts(const char* s) {
    // YYYYMMDD HHMMSSmmm
    // 01234567 8901234567
    // Position 8 is space, then HHMMSS starts at 9, mmm at 15
    if (strlen(s) < 18) return 0;
    struct tm ti{};
    ti.tm_year = ((s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0')) - 1900;
    ti.tm_mon  = (s[4]-'0')*10 + (s[5]-'0') - 1;
    ti.tm_mday = (s[6]-'0')*10 + (s[7]-'0');
    // s[8] is space
    ti.tm_hour = (s[9]-'0')*10 + (s[10]-'0');
    ti.tm_min  = (s[11]-'0')*10 + (s[12]-'0');
    ti.tm_sec  = (s[13]-'0')*10 + (s[14]-'0');
    int mmm    = (s[15]-'0')*100 + (s[16]-'0')*10 + (s[17]-'0');
    // timegm for UTC
    time_t t = timegm(&ti);
    return (int64_t)t * 1000 + mmm;
}

static bool parse_histdata_line(const char* line, Tick& out) {
    // YYYYMMDD HHMMSSmmm,bid,ask,0
    const char* comma1 = strchr(line, ',');
    if (!comma1) return false;
    int ts_len = (int)(comma1 - line);
    if (ts_len < 18) return false;
    out.ts_ms = parse_histdata_ts(line);
    if (out.ts_ms == 0) return false;
    out.bid = strtod(comma1 + 1, nullptr);
    const char* comma2 = strchr(comma1 + 1, ',');
    if (!comma2) return false;
    out.ask = strtod(comma2 + 1, nullptr);
    return (out.bid > 0.0 && out.ask > 0.0);
}

static bool parse_duka_line(const char* line, Tick& out, bool ask_first) {
    // timestamp_ms,field1,field2,...
    char* end;
    out.ts_ms = strtoll(line, &end, 10);
    if (*end != ',') return false;
    double v1 = strtod(end + 1, &end);
    if (*end != ',') return false;
    double v2 = strtod(end + 1, &end);
    if (ask_first) { out.ask = v1; out.bid = v2; }
    else           { out.bid = v1; out.ask = v2; }
    return (out.bid > 0.0 && out.ask > 0.0 && out.ts_ms > 0);
}

static bool parse_jforex_line(const char* line, Tick& out) {
    // Time (EET),Ask,Bid,...
    // Example: 01.04.2026 00:00:00.123,23801.683,23802.866,...
    // or similar date format. EET = UTC+2.
    // Try parsing "DD.MM.YYYY HH:MM:SS.mmm,ask,bid,..."
    if (strlen(line) < 23) return false;
    struct tm ti{};
    int mmm = 0;
    // Try DD.MM.YYYY HH:MM:SS.mmm
    if (sscanf(line, "%d.%d.%d %d:%d:%d.%d",
               &ti.tm_mday, &ti.tm_mon, &ti.tm_year,
               &ti.tm_hour, &ti.tm_min, &ti.tm_sec, &mmm) >= 6) {
        ti.tm_mon -= 1;
        ti.tm_year -= 1900;
        // EET = UTC+2, subtract 2 hours
        time_t t = timegm(&ti);
        t -= 2 * 3600;
        out.ts_ms = (int64_t)t * 1000 + mmm;
    } else {
        return false;
    }
    const char* comma1 = strchr(line, ',');
    if (!comma1) return false;
    out.ask = strtod(comma1 + 1, nullptr);
    const char* comma2 = strchr(comma1 + 1, ',');
    if (!comma2) return false;
    out.bid = strtod(comma2 + 1, nullptr);
    return (out.bid > 0.0 && out.ask > 0.0 && out.ts_ms > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Load ticks from multiple CSV files
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<Tick> load_ticks(const std::vector<std::string>& paths,
                                     double price_lo, double price_hi) {
    std::vector<Tick> ticks;
    ticks.reserve(50'000'000);

    for (const auto& path : paths) {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) {
            fprintf(stderr, "WARN: cannot open %s\n", path.c_str());
            continue;
        }
        char line[512];
        TickFormat fmt = FMT_UNKNOWN;
        bool first = true;
        int line_no = 0;
        while (fgets(line, sizeof(line), f)) {
            ++line_no;
            // Strip trailing newline
            size_t len = strlen(line);
            while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len] = '\0';
            if (len == 0) continue;

            if (first) {
                first = false;
                fmt = detect_format(line);
                if (fmt == FMT_UNKNOWN) {
                    fprintf(stderr, "WARN: unknown format in %s, skipping\n", path.c_str());
                    break;
                }
                // If format has a header line, skip it
                if (fmt == FMT_DUKA_BID_ASK || fmt == FMT_DUKA_ASK_BID || fmt == FMT_JFOREX)
                    continue;
            }

            Tick t;
            bool ok = false;
            switch (fmt) {
                case FMT_HISTDATA:     ok = parse_histdata_line(line, t); break;
                case FMT_DUKA_BID_ASK: ok = parse_duka_line(line, t, false); break;
                case FMT_DUKA_ASK_BID: ok = parse_duka_line(line, t, true); break;
                case FMT_JFOREX:       ok = parse_jforex_line(line, t); break;
                default: break;
            }
            if (!ok) continue;

            // Price sanity check
            double mid = (t.bid + t.ask) * 0.5;
            if (mid < price_lo || mid > price_hi) continue;
            // Spread sanity
            if (t.ask - t.bid < 0.0 || t.ask - t.bid > 50.0) continue;

            ticks.push_back(t);
        }
        fclose(f);
        printf("  Loaded %s (%d lines, %zu ticks so far)\n",
               path.c_str(), line_no, ticks.size());
    }

    // Sort by timestamp
    std::sort(ticks.begin(), ticks.end(),
              [](const Tick& a, const Tick& b) { return a.ts_ms < b.ts_ms; });

    return ticks;
}

// ─────────────────────────────────────────────────────────────────────────────
// Weekend / session helpers
// ─────────────────────────────────────────────────────────────────────────────
static void ts_to_utc(int64_t ts_ms, struct tm& ti) {
    time_t t = (time_t)(ts_ms / 1000);
#ifdef _WIN32
    gmtime_s(&ti, &t);
#else
    gmtime_r(&t, &ti);
#endif
}

static bool is_weekend(int64_t ts_ms) {
    struct tm ti{};
    ts_to_utc(ts_ms, ti);
    int wday = ti.tm_wday; // 0=Sun, 6=Sat
    int mins = ti.tm_hour * 60 + ti.tm_min;
    if (wday == 0) return true;  // Sunday
    if (wday == 6) return true;  // Saturday
    if (wday == 5 && mins >= 21 * 60) return true;  // Friday after 21:00 UTC
    return false;
}

static bool is_session_blocked(int64_t ts_ms) {
    struct tm ti{};
    ts_to_utc(ts_ms, ti);
    int mins = ti.tm_hour * 60 + ti.tm_min;
    // Block 22:00-08:00 UTC
    if (mins >= 22 * 60 || mins < 8 * 60) return true;
    // NY open noise: 13:30-14:00 UTC
    if (mins >= 13 * 60 + 30 && mins < 14 * 60) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ATR tracker (mirrors IdxATRTracker)
// ─────────────────────────────────────────────────────────────────────────────
struct ATRTracker {
    static constexpr int BUF = 256;
    double buf[BUF] = {};
    int    head  = 0;
    int    count = 0;
    double ewm   = 0.0;
    bool   init  = false;
    static constexpr double ALPHA = 0.05;

    void push(double mid) {
        buf[head % BUF] = mid;
        ++head; ++count;
        if (count % 25 != 0) return;
        int look = std::min(count, 100);
        if (look < 10) return;
        double hi = buf[(head - 1 + BUF * 4) % BUF];
        double lo = hi;
        for (int k = 1; k < look; ++k) {
            double p = buf[(head - k - 1 + BUF * 4) % BUF];
            if (p > hi) hi = p;
            if (p < lo) lo = p;
        }
        double range = hi - lo;
        if (!init) { ewm = range; init = true; }
        else        ewm = ALPHA * range + (1.0 - ALPHA) * ewm;
    }

    double atr() const { return ewm; }
    bool   ready() const { return count >= 50; }
};

// ─────────────────────────────────────────────────────────────────────────────
// EWM drift tracker (mirrors IdxRegimeGovernor)
// ─────────────────────────────────────────────────────────────────────────────
struct DriftTracker {
    double ewm_fast = 0.0;
    double ewm_slow = 0.0;
    bool   inited   = false;
    static constexpr double A_FAST = 0.05;
    static constexpr double A_SLOW = 0.005;

    void update(double mid) {
        if (!inited) { ewm_fast = ewm_slow = mid; inited = true; return; }
        ewm_fast = A_FAST * mid + (1.0 - A_FAST) * ewm_fast;
        ewm_slow = A_SLOW * mid + (1.0 - A_SLOW) * ewm_slow;
    }

    double drift() const { return inited ? (ewm_fast - ewm_slow) : 0.0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Trade record
// ─────────────────────────────────────────────────────────────────────────────
struct Trade {
    bool   is_long;
    double entry_px;
    double exit_px;
    double sl;
    double tp;
    double atr_at_entry;
    double pnl_pts;       // (exit - entry) or (entry - exit) in points
    double pnl_usd;
    double mfe;
    double mae;
    int64_t entry_ts;
    int64_t exit_ts;
    int     entry_hour;   // UTC hour of entry
    const char* exit_reason;  // SL_HIT, TP_HIT, LOSS_CUT, TRAIL, TIMEOUT
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-instrument config
// ─────────────────────────────────────────────────────────────────────────────
struct InstrumentCfg {
    const char* name;
    double lot_size;
    double pnl_per_pt;
    double atr_min;
    double max_spread;
    double drift_threshold;     // default, overridden in sweep
    double price_lo, price_hi;  // sanity bounds
};

static const InstrumentCfg CFG_SP = {
    "SP", 0.01, 0.50, 3.0, 1.0, 0.8, 3000.0, 8000.0
};
static const InstrumentCfg CFG_NQ = {
    "NQ", 0.01, 0.20, 8.0, 1.5, 2.0, 10000.0, 25000.0
};

// ─────────────────────────────────────────────────────────────────────────────
// Sweep config
// ─────────────────────────────────────────────────────────────────────────────
struct SweepConfig {
    double drift_threshold;
    int    drift_persist_ticks;
    double loss_cut_pct;
};

// ─────────────────────────────────────────────────────────────────────────────
// Engine state for one config run
// ─────────────────────────────────────────────────────────────────────────────
struct EngineState {
    ATRTracker  atr;
    DriftTracker drift;

    // Mid buffer for momentum
    static constexpr int MID_BUF = 128;
    double mid_buf[MID_BUF] = {};
    int    mid_head = 0;

    // Drift persistence
    int drift_persist_long  = 0;
    int drift_persist_short = 0;

    // Drift window for chop guard
    double drift_window[64] = {};
    int    drift_win_head = 0;
    double drift_range    = 0.0;

    int    tick_count = 0;

    // Position state
    bool   pos_active      = false;
    bool   pos_is_long     = false;
    double pos_entry       = 0.0;
    double pos_sl          = 0.0;
    double pos_tp          = 0.0;
    double pos_mfe         = 0.0;
    double pos_mae         = 0.0;
    double pos_atr_at_entry = 0.0;
    int64_t pos_entry_ts   = 0;
    int    pos_trail_stage = 0;
    bool   pos_be_locked   = false;

    // Cooldowns
    int64_t cooldown_until_ms = 0;
    int64_t sl_cooldown_until_ms = 0;

    // Config
    double cfg_drift_threshold   = 0.8;
    int    cfg_drift_persist     = 20;
    double cfg_loss_cut_pct      = 0.07;
    double cfg_atr_min           = 3.0;
    double cfg_max_spread        = 1.0;
    double cfg_atr_sl_mult       = 1.0;

    // Results
    std::vector<Trade> trades;

    void reset(const SweepConfig& sc, const InstrumentCfg& ic) {
        *this = EngineState{};
        cfg_drift_threshold = sc.drift_threshold;
        cfg_drift_persist   = sc.drift_persist_ticks;
        cfg_loss_cut_pct    = sc.loss_cut_pct;
        cfg_atr_min         = ic.atr_min;
        cfg_max_spread      = ic.max_spread;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Run one config over all ticks
// ─────────────────────────────────────────────────────────────────────────────
static void run_config(EngineState& es, const std::vector<Tick>& ticks,
                       const InstrumentCfg& ic) {
    const int N = (int)ticks.size();

    for (int i = 0; i < N; ++i) {
        const Tick& t = ticks[i];

        // Progress
        if (i > 0 && i % 10'000'000 == 0)
            printf("    [progress] %dM / %dM ticks\n", i / 1'000'000, N / 1'000'000);

        // Weekend skip
        if (is_weekend(t.ts_ms)) continue;

        double mid    = (t.bid + t.ask) * 0.5;
        double spread = t.ask - t.bid;

        // Always update state
        es.drift.update(mid);
        es.atr.push(mid);
        es.mid_buf[es.mid_head % EngineState::MID_BUF] = mid;
        ++es.mid_head;
        ++es.tick_count;

        double d = es.drift.drift();

        // Drift persistence
        if (d > es.cfg_drift_threshold)        { ++es.drift_persist_long;  es.drift_persist_short = 0; }
        else if (d < -es.cfg_drift_threshold)  { ++es.drift_persist_short; es.drift_persist_long  = 0; }
        else                                   { es.drift_persist_long = 0; es.drift_persist_short = 0; }

        // Chop guard: drift range over last 64 values
        es.drift_window[es.drift_win_head % 64] = d;
        ++es.drift_win_head;
        {
            int n = std::min(es.drift_win_head, 64);
            double hi = es.drift_window[0], lo = es.drift_window[0];
            for (int k = 1; k < n; ++k) {
                if (es.drift_window[k] > hi) hi = es.drift_window[k];
                if (es.drift_window[k] < lo) lo = es.drift_window[k];
            }
            es.drift_range = hi - lo;
        }

        // ── Manage open position ────────────────────────────────────────────
        if (es.pos_active) {
            double move = es.pos_is_long ? (mid - es.pos_entry) : (es.pos_entry - mid);
            if (move > es.pos_mfe) es.pos_mfe = move;
            if (-move > es.pos_mae) es.pos_mae = -move;

            bool closed = false;
            const char* why = "";

            // LOSS_CUT (runs first)
            if (!closed && es.cfg_loss_cut_pct > 0.0 && es.pos_entry > 0.0) {
                double adverse = -move;
                double loss_cut_dist = es.pos_entry * es.cfg_loss_cut_pct / 100.0;
                if (adverse >= loss_cut_dist) {
                    double exit_px = es.pos_is_long ? t.bid : t.ask;
                    Trade tr;
                    tr.is_long     = es.pos_is_long;
                    tr.entry_px    = es.pos_entry;
                    tr.exit_px     = exit_px;
                    tr.sl          = es.pos_sl;
                    tr.tp          = es.pos_tp;
                    tr.atr_at_entry = es.pos_atr_at_entry;
                    tr.pnl_pts     = es.pos_is_long ? (exit_px - es.pos_entry) : (es.pos_entry - exit_px);
                    tr.pnl_usd     = tr.pnl_pts * ic.pnl_per_pt;
                    tr.mfe         = es.pos_mfe;
                    tr.mae         = es.pos_mae;
                    tr.entry_ts    = es.pos_entry_ts;
                    tr.exit_ts     = t.ts_ms;
                    tr.entry_hour  = -1;
                    { struct tm ti{}; ts_to_utc(es.pos_entry_ts, ti); tr.entry_hour = ti.tm_hour; }
                    tr.exit_reason = "LOSS_CUT";
                    es.trades.push_back(tr);
                    es.pos_active = false;
                    es.cooldown_until_ms = t.ts_ms + 30000;
                    closed = true;
                    why = "LOSS_CUT";
                }
            }

            // Staircase trail
            if (!closed) {
                double a = (es.pos_atr_at_entry > 0.0) ? es.pos_atr_at_entry : es.atr.atr();
                if (a > 0.0) {
                    // Stage 1: BE lock at 1x ATR
                    if (!es.pos_be_locked && move >= a * 1.0) {
                        es.pos_be_locked = true;
                        if (es.pos_is_long  && es.pos_entry > es.pos_sl) es.pos_sl = es.pos_entry;
                        if (!es.pos_is_long && es.pos_entry < es.pos_sl) es.pos_sl = es.pos_entry;
                        es.pos_trail_stage = 1;
                    }
                    // Stage 2: trail 0.5x ATR behind peak from 2x ATR
                    if (es.pos_be_locked && move >= a * 2.0) {
                        double trail = es.pos_is_long ? (es.pos_entry + es.pos_mfe - a * 0.5)
                                                       : (es.pos_entry - es.pos_mfe + a * 0.5);
                        if (es.pos_is_long  && trail > es.pos_sl) es.pos_sl = trail;
                        if (!es.pos_is_long && trail < es.pos_sl) es.pos_sl = trail;
                        es.pos_trail_stage = 2;
                    }
                    // Stage 3: tight trail 0.25x ATR from 4x ATR
                    if (es.pos_be_locked && move >= a * 4.0) {
                        double trail = es.pos_is_long ? (es.pos_entry + es.pos_mfe - a * 0.25)
                                                       : (es.pos_entry - es.pos_mfe + a * 0.25);
                        if (es.pos_is_long  && trail > es.pos_sl) es.pos_sl = trail;
                        if (!es.pos_is_long && trail < es.pos_sl) es.pos_sl = trail;
                        es.pos_trail_stage = 3;
                    }
                }

                // TP hit
                bool tp_hit = (es.pos_tp > 0.0) &&
                              (es.pos_is_long ? (t.bid >= es.pos_tp) : (t.ask <= es.pos_tp));
                // SL hit
                bool sl_hit = es.pos_is_long ? (t.bid <= es.pos_sl) : (t.ask >= es.pos_sl);

                // Timeout: 60 min, suppressed if in profit
                int64_t held_s = (t.ts_ms - es.pos_entry_ts) / 1000;
                bool timed_out = false;
                if (held_s >= 3600 && !tp_hit && !sl_hit) {
                    double cur_move = es.pos_is_long ? (mid - es.pos_entry) : (es.pos_entry - mid);
                    bool trail_profit = es.pos_is_long ? (es.pos_sl >= es.pos_entry)
                                                        : (es.pos_sl <= es.pos_entry);
                    if (!trail_profit && cur_move <= 0.0)
                        timed_out = true;
                }

                if (tp_hit || sl_hit || timed_out) {
                    // Fill at the actual crossing price: flattening a long sells at
                    // BID, a short buys at ASK. Filling at the literal TP/SL level
                    // overstated SL exits (the triggering bid/ask was already past
                    // the level), understating losses. Tick-accurate fill below.
                    double exit_px = es.pos_is_long ? t.bid : t.ask;
                    if (tp_hit)        { why = "TP_HIT"; }
                    else if (sl_hit)   {
                        // Determine if this is a trail exit (stage > 0 and SL above/below entry)
                        bool trail_exit = (es.pos_trail_stage >= 2);
                        why = trail_exit ? "TRAIL" : "SL_HIT";
                    }
                    else               { why = "TIMEOUT"; }

                    Trade tr;
                    tr.is_long      = es.pos_is_long;
                    tr.entry_px     = es.pos_entry;
                    tr.exit_px      = exit_px;
                    tr.sl           = es.pos_sl;
                    tr.tp           = es.pos_tp;
                    tr.atr_at_entry = es.pos_atr_at_entry;
                    tr.pnl_pts      = es.pos_is_long ? (exit_px - es.pos_entry) : (es.pos_entry - exit_px);
                    tr.pnl_usd      = tr.pnl_pts * ic.pnl_per_pt;
                    tr.mfe          = es.pos_mfe;
                    tr.mae          = es.pos_mae;
                    tr.entry_ts     = es.pos_entry_ts;
                    tr.exit_ts      = t.ts_ms;
                    tr.entry_hour   = -1;
                    { struct tm ti{}; ts_to_utc(es.pos_entry_ts, ti); tr.entry_hour = ti.tm_hour; }
                    tr.exit_reason  = why;
                    es.trades.push_back(tr);
                    es.pos_active = false;
                    es.cooldown_until_ms = t.ts_ms + 30000;
                    // 90s SL cooldown
                    if (sl_hit || (strcmp(why, "SL_HIT") == 0)) {
                        int64_t sl_block = t.ts_ms + 90000;
                        if (sl_block > es.sl_cooldown_until_ms)
                            es.sl_cooldown_until_ms = sl_block;
                    }
                    closed = true;
                }
            }

            continue;  // always skip entry logic when position is active
        }

        // ── Entry gates ─────────────────────────────────────────────────────
        // 1. No open position (handled above)

        // 2. Cooldown
        if (t.ts_ms < es.cooldown_until_ms) continue;

        // 3. ATR ready
        if (!es.atr.ready()) continue;

        // 4. Min ticks
        if (es.tick_count < 50) continue;

        double atr_val = es.atr.atr();

        // 5. ATR >= atr_min
        if (atr_val < es.cfg_atr_min) continue;

        // 6. Spread <= max_spread
        if (spread > es.cfg_max_spread) continue;

        // 7. Session gate: block 22:00-08:00 UTC
        if (is_session_blocked(t.ts_ms)) continue;

        // 8. SL cooldown (90s after SL hit)
        if (t.ts_ms < es.sl_cooldown_until_ms) continue;

        // ── Signal ──────────────────────────────────────────────────────────
        bool drift_long  = (d >  es.cfg_drift_threshold) && (es.drift_persist_long  >= es.cfg_drift_persist);
        bool drift_short = (d < -es.cfg_drift_threshold) && (es.drift_persist_short >= es.cfg_drift_persist);

        if (!drift_long && !drift_short) continue;

        // 9. Momentum confirmation: mid[now] - mid[12 ticks ago] same sign
        if (es.mid_head < 13) continue;
        double momentum = es.mid_buf[(es.mid_head - 1 + EngineState::MID_BUF) % EngineState::MID_BUF]
                         - es.mid_buf[(es.mid_head - 13 + EngineState::MID_BUF * 4) % EngineState::MID_BUF];
        if (drift_long  && momentum <= 0.0) continue;
        if (drift_short && momentum >= 0.0) continue;

        // 10. Chop guard
        if (es.drift_range > es.cfg_drift_threshold * 4.0 &&
            std::fabs(d) < es.cfg_drift_threshold * 1.5)
            continue;

        // ── Build entry ─────────────────────────────────────────────────────
        bool is_long = drift_long;
        double sl_dist = std::max(es.cfg_atr_min, atr_val * es.cfg_atr_sl_mult);
        double tp_dist = sl_dist * 3.0;

        es.pos_active       = true;
        es.pos_is_long      = is_long;
        es.pos_entry        = is_long ? t.ask : t.bid;
        es.pos_sl           = is_long ? (t.bid - sl_dist) : (t.ask + sl_dist);
        es.pos_tp           = is_long ? (t.ask + tp_dist) : (t.bid - tp_dist);
        es.pos_atr_at_entry = atr_val;
        es.pos_mfe          = 0.0;
        es.pos_mae          = 0.0;
        es.pos_entry_ts     = t.ts_ms;
        es.pos_trail_stage  = 0;
        es.pos_be_locked    = false;
    }

    // Force close any remaining position at last tick
    if (es.pos_active && !ticks.empty()) {
        const Tick& t = ticks.back();
        double mid = (t.bid + t.ask) * 0.5;
        Trade tr;
        tr.is_long      = es.pos_is_long;
        tr.entry_px     = es.pos_entry;
        tr.exit_px      = mid;
        tr.sl           = es.pos_sl;
        tr.tp           = es.pos_tp;
        tr.atr_at_entry = es.pos_atr_at_entry;
        tr.pnl_pts      = es.pos_is_long ? (mid - es.pos_entry) : (es.pos_entry - mid);
        tr.pnl_usd      = tr.pnl_pts * ic.pnl_per_pt;
        tr.mfe          = es.pos_mfe;
        tr.mae          = es.pos_mae;
        tr.entry_ts     = es.pos_entry_ts;
        tr.exit_ts      = t.ts_ms;
        tr.entry_hour   = -1;
        { struct tm ti{}; ts_to_utc(es.pos_entry_ts, ti); tr.entry_hour = ti.tm_hour; }
        tr.exit_reason  = "FORCE_CLOSE";
        es.trades.push_back(tr);
        es.pos_active = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stats computation
// ─────────────────────────────────────────────────────────────────────────────
struct Stats {
    int    n_trades = 0;
    int    n_wins   = 0;
    int    n_long   = 0;
    int    n_short  = 0;
    int    n_long_wins  = 0;
    int    n_short_wins = 0;
    double gross_win  = 0.0;
    double gross_loss = 0.0;
    double total_pnl  = 0.0;
    double max_dd     = 0.0;

    // Exit breakdown
    int exit_sl = 0, exit_tp = 0, exit_lc = 0, exit_trail = 0, exit_timeout = 0, exit_other = 0;

    // Per-hour PF
    double hour_win[24]  = {};
    double hour_loss[24] = {};

    double profit_factor() const {
        return (gross_loss != 0.0) ? (gross_win / -gross_loss) : (gross_win > 0.0 ? 999.0 : 0.0);
    }
    double win_rate() const { return n_trades > 0 ? 100.0 * n_wins / n_trades : 0.0; }
};

static Stats compute_stats(const std::vector<Trade>& trades) {
    Stats s;
    s.n_trades = (int)trades.size();
    double eq = 0.0;
    double peak = 0.0;

    for (auto& tr : trades) {
        double pnl = tr.pnl_usd;
        eq += pnl;
        s.total_pnl += pnl;
        if (pnl > 0.0) { ++s.n_wins; s.gross_win += pnl; }
        else            { s.gross_loss += pnl; }

        if (tr.is_long) { ++s.n_long; if (pnl > 0.0) ++s.n_long_wins; }
        else            { ++s.n_short; if (pnl > 0.0) ++s.n_short_wins; }

        if (eq > peak) peak = eq;
        double dd = peak - eq;
        if (dd > s.max_dd) s.max_dd = dd;

        // Exit breakdown
        if      (strcmp(tr.exit_reason, "SL_HIT")   == 0) ++s.exit_sl;
        else if (strcmp(tr.exit_reason, "TP_HIT")   == 0) ++s.exit_tp;
        else if (strcmp(tr.exit_reason, "LOSS_CUT") == 0) ++s.exit_lc;
        else if (strcmp(tr.exit_reason, "TRAIL")    == 0) ++s.exit_trail;
        else if (strcmp(tr.exit_reason, "TIMEOUT")  == 0) ++s.exit_timeout;
        else ++s.exit_other;

        // Per-hour PF
        int h = tr.entry_hour;
        if (h >= 0 && h < 24) {
            if (pnl > 0.0) s.hour_win[h] += pnl;
            else            s.hour_loss[h] += pnl;
        }
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// IS/OOS split
// ─────────────────────────────────────────────────────────────────────────────
static void split_is_oos(const std::vector<Trade>& all,
                          std::vector<Trade>& is_trades,
                          std::vector<Trade>& oos_trades,
                          int64_t ts_start, int64_t ts_end) {
    int64_t split_ts = ts_start + (int64_t)((ts_end - ts_start) * 0.60);
    for (auto& tr : all) {
        if (tr.entry_ts < split_ts) is_trades.push_back(tr);
        else                         oos_trades.push_back(tr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // Parse args
    std::string instrument = "";
    std::vector<std::string> csv_files;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            instrument = argv[++i];
        } else if (argv[i][0] != '-') {
            csv_files.push_back(argv[i]);
        }
    }

    if (instrument.empty() || csv_files.empty()) {
        fprintf(stderr, "Usage: %s --instrument SP|NQ <csv_files...>\n", argv[0]);
        return 1;
    }

    const InstrumentCfg* ic = nullptr;
    if (instrument == "SP") ic = &CFG_SP;
    else if (instrument == "NQ") ic = &CFG_NQ;
    else {
        fprintf(stderr, "ERROR: unknown instrument '%s' (use SP or NQ)\n", instrument.c_str());
        return 1;
    }

    printf("==========================================================================\n");
    printf("  IndexFlowEngine Backtest — %s\n", ic->name);
    printf("  Files: %zu CSVs\n", csv_files.size());
    printf("==========================================================================\n\n");

    // Load ticks
    printf("Loading ticks...\n");
    auto ticks = load_ticks(csv_files, ic->price_lo, ic->price_hi);
    printf("Loaded %zu ticks total\n\n", ticks.size());

    if (ticks.empty()) {
        fprintf(stderr, "ERROR: no valid ticks loaded\n");
        return 1;
    }

    int64_t ts_start = ticks.front().ts_ms;
    int64_t ts_end   = ticks.back().ts_ms;
    {
        struct tm ts{}, te{};
        ts_to_utc(ts_start, ts);
        ts_to_utc(ts_end, te);
        printf("Date range: %04d-%02d-%02d to %04d-%02d-%02d\n\n",
               ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday,
               te.tm_year+1900, te.tm_mon+1, te.tm_mday);
    }

    // Build sweep configs
    std::vector<double> drift_thresholds;
    if (instrument == "SP") drift_thresholds = {0.5, 0.8, 1.2};
    else                    drift_thresholds = {1.5, 2.0, 3.0};

    std::vector<int>    persist_values = {12, 20, 30};
    std::vector<double> lc_values      = {0.0, 0.05, 0.07, 0.10};

    std::vector<SweepConfig> configs;
    for (double dt : drift_thresholds)
        for (int dp : persist_values)
            for (double lc : lc_values)
                configs.push_back({dt, dp, lc});

    printf("Running %zu configs...\n\n", configs.size());

    // ── Sweep ───────────────────────────────────────────────────────────────
    struct ConfigResult {
        SweepConfig cfg;
        Stats is_stats;
        Stats oos_stats;
        std::vector<Trade> all_trades;
    };
    std::vector<ConfigResult> results;
    results.reserve(configs.size());

    int best_idx = -1;
    double best_oos_pf = 0.0;

    printf("%-8s %-8s %-8s | IS: %-6s %-8s | OOS: %-6s %-8s | VERDICT\n",
           "drift_t", "persist", "lc_pct", "trades", "PF", "trades", "PF");
    printf("─────────────────────────────────────────────────────────────────────\n");

    for (int ci = 0; ci < (int)configs.size(); ++ci) {
        const auto& sc = configs[ci];

        EngineState es;
        es.reset(sc, *ic);

        run_config(es, ticks, *ic);

        // Split IS/OOS
        std::vector<Trade> is_trades, oos_trades;
        split_is_oos(es.trades, is_trades, oos_trades, ts_start, ts_end);

        Stats is_s  = compute_stats(is_trades);
        Stats oos_s = compute_stats(oos_trades);

        // Verdict
        bool pass = (oos_s.profit_factor() >= 1.20 && oos_s.n_trades >= 20);
        const char* verdict = pass ? "PASS" : "FAIL";

        printf("%-8.2f %-8d %-8.2f | %6d %8.2f | %6d %8.2f | %s\n",
               sc.drift_threshold, sc.drift_persist_ticks, sc.loss_cut_pct,
               is_s.n_trades, is_s.profit_factor(),
               oos_s.n_trades, oos_s.profit_factor(),
               verdict);

        ConfigResult cr;
        cr.cfg        = sc;
        cr.is_stats   = is_s;
        cr.oos_stats  = oos_s;
        cr.all_trades = std::move(es.trades);
        results.push_back(std::move(cr));

        // Track best OOS
        if (oos_s.n_trades >= 10 && oos_s.profit_factor() > best_oos_pf) {
            best_oos_pf = oos_s.profit_factor();
            best_idx = ci;
        }
    }

    // ── Best OOS full report ────────────────────────────────────────────────
    printf("\n==========================================================================\n");
    if (best_idx < 0) {
        printf("  No config with >= 10 OOS trades found.\n");
        printf("==========================================================================\n");
        return 0;
    }

    const auto& best = results[best_idx];
    bool oos_pass = (best.oos_stats.profit_factor() >= 1.20 && best.oos_stats.n_trades >= 20);

    printf("  BEST OOS CONFIG — %s\n", ic->name);
    printf("  drift_threshold=%.2f  drift_persist=%d  LOSS_CUT_PCT=%.2f\n",
           best.cfg.drift_threshold, best.cfg.drift_persist_ticks, best.cfg.loss_cut_pct);
    printf("  OOS VERDICT: %s (PF=%.2f, trades=%d)\n",
           oos_pass ? "PASS" : "FAIL",
           best.oos_stats.profit_factor(), best.oos_stats.n_trades);
    printf("==========================================================================\n\n");

    // Split trades for detailed report
    std::vector<Trade> is_trades, oos_trades;
    split_is_oos(best.all_trades, is_trades, oos_trades, ts_start, ts_end);
    Stats is_s  = compute_stats(is_trades);
    Stats oos_s = compute_stats(oos_trades);
    Stats all_s = compute_stats(best.all_trades);

    auto print_stats = [](const char* label, const Stats& s) {
        printf("  %-10s  %5dT  WR=%5.1f%%  PF=%5.2f  Total=$%8.2f  MaxDD=$%7.2f\n",
               label, s.n_trades, s.win_rate(), s.profit_factor(), s.total_pnl, s.max_dd);
    };

    printf("── Overall ──\n");
    print_stats("ALL", all_s);
    print_stats("IS", is_s);
    print_stats("OOS", oos_s);

    printf("\n── Long / Short ──\n");
    printf("  LONG:  %5d trades  WR=%5.1f%%\n", all_s.n_long,
           all_s.n_long > 0 ? 100.0 * all_s.n_long_wins / all_s.n_long : 0.0);
    printf("  SHORT: %5d trades  WR=%5.1f%%\n", all_s.n_short,
           all_s.n_short > 0 ? 100.0 * all_s.n_short_wins / all_s.n_short : 0.0);

    printf("\n── Exit Breakdown ──\n");
    printf("  SL_HIT=%d  TP_HIT=%d  LOSS_CUT=%d  TRAIL=%d  TIMEOUT=%d  OTHER=%d\n",
           all_s.exit_sl, all_s.exit_tp, all_s.exit_lc, all_s.exit_trail,
           all_s.exit_timeout, all_s.exit_other);

    printf("\n── Per-Hour PF (UTC, entry hour) ──\n");
    for (int h = 8; h < 22; ++h) {
        double w = all_s.hour_win[h];
        double l = all_s.hour_loss[h];
        double pf = (l != 0.0) ? (w / -l) : (w > 0.0 ? 999.0 : 0.0);
        int nt = 0;
        for (auto& tr : best.all_trades) if (tr.entry_hour == h) ++nt;
        if (nt == 0) continue;
        printf("  %02d:00  %4d trades  PF=%5.2f  Win=$%7.2f  Loss=$%7.2f\n",
               h, nt, pf, w, l);
    }

    // IS/OOS decay: split IS into two halves
    printf("\n── IS/OOS Decay ──\n");
    {
        int64_t is_split = ts_start + (int64_t)((ts_end - ts_start) * 0.30);
        std::vector<Trade> is1, is2;
        for (auto& tr : is_trades) {
            if (tr.entry_ts < is_split) is1.push_back(tr);
            else is2.push_back(tr);
        }
        Stats s1 = compute_stats(is1);
        Stats s2 = compute_stats(is2);
        print_stats("IS-early", s1);
        print_stats("IS-late", s2);
        print_stats("OOS", oos_s);
    }

    printf("\n==========================================================================\n");
    printf("  Done. %zu total configs, best OOS PF=%.2f\n", configs.size(), best_oos_pf);
    printf("==========================================================================\n");

    return 0;
}
