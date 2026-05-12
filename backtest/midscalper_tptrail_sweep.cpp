// =============================================================================
// midscalper_tptrail_sweep.cpp -- Plan B TP/SL/Trail sweep harness
// =============================================================================
//
// 2026-05-12 (Claude / Jo): Plan B sweep harness. Runs N MidScalperEngine
//   instances in parallel, each with its own (TP_RR, SL_FRAC, MFE_TRAIL_FRAC)
//   combo. Pinned to MAX_SPREAD = 1.0 (Plan A's winning param).
//   Reuses the engine class from midscalper_ev_pilot_param.cpp.
//   Outputs a leaderboard CSV sorted by net PnL across the 24-month tape.
//
//   Pattern mirrors midscalper_crtp_sweep.cpp: one shared tick stream,
//   N independent engine instances, each on_tick dispatches to all N.
//
// USAGE
//   ./midscalper_tptrail_sweep <list of monthly XAUUSD csvs ...>
//       --out PATH
//
// BUILD
//   g++ -std=c++17 -O3 -DNDEBUG -I include \
//       backtest/midscalper_tptrail_sweep.cpp -o /tmp/midscalper_tptrail_sweep
// =============================================================================

#define OMEGA_BACKTEST 1

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// NOTE: OmegaEVGuard.hpp intentionally NOT included -- the sweep does not use
// the EV gate. Each engine instance has ev_gate=nullptr (its default).

namespace omega::ev_pilot {

// -----------------------------------------------------------------------------
// Engine: faithful port of GoldMidScalperEngine.hpp at LIVE configuration.
//   Cross-config invariants identical to the sweep harness; the swept params
//   are pinned to live values:
//     COOLDOWN_S=180, SAME_LEVEL_POST_WIN_BLOCK_S=600,
//     SAME_LEVEL_POST_SL_BLOCK_S=900, MIN_ENTRY_TICKS=30, MAX_SPREAD=2.5
// -----------------------------------------------------------------------------
struct TradeRecord {
    int64_t ts_ms_entry;
    int64_t ts_ms_exit;
    bool    is_long;
    double  entry_price;
    double  exit_price;
    std::string exit_reason;
    int     duration_s;
    double  gross_pts;
    double  gross_usd;
    double  spread_at_entry_pts;
    double  modeled_cost_usd;
    double  net_usd;
    int     hour_utc;
    int     vol_regime;   // 0=low, 1=med, 2=high
};

class MidScalperEngine {
public:
    // Held-fixed constants (live config)
    static constexpr int    STRUCTURE_LOOKBACK    = 300;
    static constexpr int    MIN_BREAK_TICKS       = 5;
    static constexpr int    MIN_ENTRY_TICKS       = 30;
    static constexpr double MIN_RANGE             = 8.0;
    static constexpr double MAX_RANGE             = 20.0;
    // SWEPT (now instance members; defaults match live config):
    double                  SL_FRAC               = 0.6;
    double                  TP_RR                 = 4.0;
    double                  MFE_TRAIL_FRAC        = 0.55;
    double                  TRAIL_FRAC            = 0.25;
    // PINNED (Plan B holds MAX_SPREAD at 1.0 -- Plan A's winning value):
    static constexpr double MAX_SPREAD            = 1.0;
    // Remaining held-fixed constants:
    static constexpr double SL_BUFFER             = 1.0;
    static constexpr double MIN_TRAIL_ARM_PTS     = 5.0;
    static constexpr int    MIN_TRAIL_ARM_SECS    = 15;
    static constexpr double BE_TRIGGER_PTS        = 3.0;
    static constexpr double BE_OFFSET_PTS         = 2.5;
    static constexpr double SAME_LEVEL_BLOCK_PTS  = 8.0;
    static constexpr int    SAME_LEVEL_POST_WIN_BLOCK_S = 600;
    static constexpr int    SAME_LEVEL_POST_SL_BLOCK_S  = 900;
    static constexpr int    COOLDOWN_S            = 180;
    static constexpr int    PENDING_TIMEOUT_S     = 120;
    static constexpr int    SESSION_START_HOUR_UTC = 6;
    static constexpr int    SESSION_END_HOUR_UTC   = 22;
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;
    static constexpr double MAX_FILL_SPREAD       = 5.0;
    static constexpr double USD_PER_PT            = 100.0;
    static constexpr double LIVE_LOT              = 0.01;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };

    Phase phase = Phase::IDLE;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts_ms = 0;
        int     hour_utc_at_entry = 0;
        int     vol_regime_at_entry = 0;
        bool    be_locked = false;
    } pos;

    double bracket_high = 0.0;
    double bracket_low  = 0.0;
    double range        = 0.0;

    // EV gate callback: returns true if the fire is allowed.
    //   spread_pt, hour_utc are passed as the trade attempts to fire (at the
    //   moment the engine would transition ARMED -> PENDING). Set nullptr to
    //   disable the gate (baseline pass).
    std::function<bool(double, int)> ev_gate = nullptr;

    int   skipped_by_ev = 0;

    // Sink: emit completed trades.
    std::function<void(const TradeRecord&)> trade_sink = nullptr;

    void on_tick(double bid, double ask, int64_t now_ms,
                 int win_count, double w_hi_shared, double w_lo_shared,
                 int hour_utc, int vol_regime) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        ++m_ticks_received;

        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        if (phase == Phase::LIVE) {
            _manage(bid, ask, mid, now_s, now_ms);
            return;
        }

        if (phase == Phase::PENDING) {
            const bool would_fill_long  = (ask >= bracket_high);
            const bool would_fill_short = (bid <= bracket_low);
            if ((would_fill_long || would_fill_short) && spread > MAX_FILL_SPREAD) {
                phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return;
            }
            if (would_fill_long && would_fill_short) {
                // No L2; default to LONG (conservative tiebreak; rare event)
                _confirm_fill(true, bracket_high, LIVE_LOT, spread, now_ms, hour_utc, vol_regime);
                return;
            }
            if (would_fill_long)  { _confirm_fill(true,  bracket_high, LIVE_LOT, spread, now_ms, hour_utc, vol_regime); return; }
            if (would_fill_short) { _confirm_fill(false, bracket_low,  LIVE_LOT, spread, now_ms, hour_utc, vol_regime); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0;
            }
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if (win_count < STRUCTURE_LOOKBACK) return;
        if (spread > MAX_SPREAD) return;

        // Session gate 06-22 UTC
        if (hour_utc < SESSION_START_HOUR_UTC || hour_utc >= SESSION_END_HOUR_UTC) return;

        const double w_hi = w_hi_shared;
        const double w_lo = w_lo_shared;
        range = w_hi - w_lo;

        if (phase == Phase::IDLE) {
            if (m_sl_price > 0.0 && now_s < m_sl_cooldown_ts) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) return;
            }
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) return;
            }
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase          = Phase::ARMED;
                bracket_high   = w_hi;
                bracket_low    = w_lo;
                m_inside_ticks = 0;
                m_armed_ts     = now_s;
            }
            return;
        }

        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE)  { phase = Phase::IDLE; return; }

            if (mid >= bracket_low && mid <= bracket_high) {
                ++m_inside_ticks;
            } else {
                m_inside_ticks = 0; phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0; return;
            }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            const double min_tp  = spread * 2.0 + 0.12;
            if (tp_dist < min_tp) { phase = Phase::IDLE; return; }

            m_range_history.push_back(range);
            if ((int)m_range_history.size() > EXPANSION_HISTORY_LEN)
                m_range_history.pop_front();
            if ((int)m_range_history.size() < EXPANSION_MIN_HISTORY) {
                phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
            }
            {
                std::vector<double> sorted(m_range_history.begin(), m_range_history.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t n = sorted.size();
                const double median = (n % 2 == 1) ? sorted[n / 2]
                                                   : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
                const double threshold = median * EXPANSION_MULT;
                if (range < threshold) {
                    phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
                }
            }

            // -------- EV gate (Path B) -----------------------------------
            if (ev_gate) {
                if (!ev_gate(spread, hour_utc)) {
                    ++skipped_by_ev;
                    phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
                }
            }

            phase = Phase::PENDING;
            m_armed_ts = now_s;
        }
    }

    void force_close_if_open(int64_t now_ms) noexcept {
        if (!pos.active) return;
        // Emit a synthetic close at last known mid via exit_reason "FORCE_CLOSE"
        // using the engine's last seen entry price as a crude exit; in practice
        // the last tick's bid/ask drives this. We'll skip emitting force-close
        // trades for simplicity; they distort no statistics if rare.
        pos = LivePos{}; phase = Phase::IDLE;
        bracket_high = bracket_low = range = 0.0;
    }

private:
    void _confirm_fill(bool is_long, double fill_px, double fill_lot,
                       double spread_at_fill, int64_t now_ms,
                       int hour_utc, int vol_regime) noexcept
    {
        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = fill_px;
        pos.sl              = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp              = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size            = fill_lot;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.spread_at_entry = spread_at_fill;
        pos.entry_ts_ms     = now_ms;
        pos.be_locked       = false;
        pos.hour_utc_at_entry = hour_utc;
        pos.vol_regime_at_entry = vol_regime;
        phase               = Phase::LIVE;
    }

    void _manage(double bid, double ask, double mid,
                 int64_t now_s, int64_t now_ms) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        const int64_t held_s = now_s - pos.entry_ts_ms / 1000;

        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            const double effective_offset = (move >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double be_target = pos.is_long
                ? (pos.entry + effective_offset)
                : (pos.entry - effective_offset);
            if (pos.is_long  && be_target > pos.sl) pos.sl = be_target;
            if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
            pos.be_locked = true;
        }

        const bool arm_mfe_ok  = (pos.mfe >= MIN_TRAIL_ARM_PTS);
        const bool arm_hold_ok = (held_s  >= MIN_TRAIL_ARM_SECS);
        if (move > 0 && arm_mfe_ok && arm_hold_ok) {
            const double mfe_trail   = pos.mfe * MFE_TRAIL_FRAC;
            const double range_trail = range * TRAIL_FRAC;
            const double trail_dist  = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
            const double trail_sl = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) { _close(pos.tp, "TP_HIT", now_s, now_ms); return; }

        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool sl_at_be        = (pos.sl <= pos.entry + 0.01)
                                      && (pos.sl >= pos.entry - 0.01);
            const bool trail_in_profit = pos.is_long
                ? (pos.sl > pos.entry + 0.01)
                : (pos.sl < pos.entry - 0.01);
            const char* reason;
            if      (sl_at_be)        reason = "BE_HIT";
            else if (trail_in_profit) reason = "TRAIL_HIT";
            else                      reason = "SL_HIT";
            _close(exit_px, reason, now_s, now_ms);
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, int64_t now_ms) noexcept
    {
        if (!pos.active) return;
        const bool   is_long_  = pos.is_long;
        const double entry_    = pos.entry;
        const double size_     = pos.size;
        const double spread_at_entry_ = pos.spread_at_entry;
        const int64_t entry_ts_ms_ = pos.entry_ts_ms;
        const int hour_utc_ = pos.hour_utc_at_entry;
        const int vol_reg_  = pos.vol_regime_at_entry;

        const double gross_pts = is_long_ ? (exit_px - entry_) : (entry_ - exit_px);
        const double gross_usd = gross_pts * size_ * USD_PER_PT;

        // Modeled cost: $0.66 fixed + 2 * spread_at_entry * $1/pt for 0.01 lot
        const double modeled_cost_usd = 0.66 + 2.0 * spread_at_entry_;
        const double net_usd = gross_usd - modeled_cost_usd;

        if (std::string(reason) == "SL_HIT") {
            m_sl_cooldown_ts = now_s + SAME_LEVEL_POST_SL_BLOCK_S;
            m_sl_price       = entry_;
        }
        if (std::string(reason) == "TRAIL_HIT" || std::string(reason) == "TP_HIT") {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
        }

        TradeRecord tr;
        tr.ts_ms_entry        = entry_ts_ms_;
        tr.ts_ms_exit         = now_ms;
        tr.is_long            = is_long_;
        tr.entry_price        = entry_;
        tr.exit_price         = exit_px;
        tr.exit_reason        = reason;
        tr.duration_s         = (int)(now_s - entry_ts_ms_ / 1000);
        tr.gross_pts          = gross_pts;
        tr.gross_usd          = gross_usd;
        tr.spread_at_entry_pts = spread_at_entry_;
        tr.modeled_cost_usd   = modeled_cost_usd;
        tr.net_usd            = net_usd;
        tr.hour_utc           = hour_utc_;
        tr.vol_regime         = vol_reg_;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        if (trade_sink) trade_sink(tr);
    }

    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start = 0;
    int64_t m_sl_cooldown_ts = 0;
    double  m_sl_price       = 0.0;
    double  m_win_exit_price = 0.0;
    int64_t m_win_exit_block_ts = 0;
    std::deque<double> m_range_history;
};

} // namespace omega::ev_pilot

// -----------------------------------------------------------------------------
// CSV streaming reader: yields one tick at a time.
//   File schema: timestamp_ms,ask,bid[,ask_vol,bid_vol]   (header on first row)
// -----------------------------------------------------------------------------
struct TickStreamer {
    std::ifstream f;
    std::string   line;
    int           c_ts = 0;
    int           c_ask = 1;
    int           c_bid = 2;
    bool          opened = false;

    bool open(const char* path, bool verbose) {
        f.open(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "[err] cannot open %s\n", path);
            return false;
        }
        if (!std::getline(f, line)) {
            std::fprintf(stderr, "[err] empty %s\n", path);
            return false;
        }
        // Parse header
        std::vector<std::string> cols;
        {
            std::string cur;
            for (char c : line) {
                if (c == ',') { cols.push_back(cur); cur.clear(); }
                else if (c == '\r' || c == '\n') {}
                else cur.push_back(c);
            }
            cols.push_back(cur);
        }
        c_ts = c_ask = c_bid = -1;
        for (int i = 0; i < (int)cols.size(); ++i) {
            if (cols[i] == "timestamp_ms" || cols[i] == "timestamp") c_ts = i;
            else if (cols[i] == "ask" || cols[i] == "askPrice") c_ask = i;
            else if (cols[i] == "bid" || cols[i] == "bidPrice") c_bid = i;
        }
        if (c_ts < 0 || c_ask < 0 || c_bid < 0) {
            std::fprintf(stderr, "[err] missing required columns in %s header: %s\n", path, line.c_str());
            return false;
        }
        opened = true;
        if (verbose) std::printf("[bt] opened %s (ts@%d ask@%d bid@%d)\n", path, c_ts, c_ask, c_bid);
        return true;
    }

    bool next(int64_t& ts_ms, double& bid, double& ask) {
        if (!opened) return false;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            // Fast field split
            int field_idx = 0;
            const char* p = line.c_str();
            const char* start = p;
            const char* tsv = nullptr; size_t tsl = 0;
            const char* askv = nullptr; size_t askl = 0;
            const char* bidv = nullptr; size_t bidl = 0;
            while (true) {
                if (*p == ',' || *p == '\0' || *p == '\r' || *p == '\n') {
                    if (field_idx == c_ts) { tsv = start; tsl = (size_t)(p - start); }
                    else if (field_idx == c_ask) { askv = start; askl = (size_t)(p - start); }
                    else if (field_idx == c_bid) { bidv = start; bidl = (size_t)(p - start); }
                    if (*p == '\0' || *p == '\r' || *p == '\n') break;
                    start = p + 1;
                    ++field_idx;
                }
                ++p;
            }
            if (!tsv || !askv || !bidv) continue;
            // Parse
            char buf[32];
            if (tsl >= sizeof(buf)) continue;
            std::memcpy(buf, tsv, tsl); buf[tsl] = 0;
            ts_ms = (int64_t)std::strtoll(buf, nullptr, 10);
            if (askl >= sizeof(buf)) continue;
            std::memcpy(buf, askv, askl); buf[askl] = 0;
            ask = std::strtod(buf, nullptr);
            if (bidl >= sizeof(buf)) continue;
            std::memcpy(buf, bidv, bidl); buf[bidl] = 0;
            bid = std::strtod(buf, nullptr);
            return true;
        }
        return false;
    }
};

// -----------------------------------------------------------------------------
// Vol-regime estimator: ATR(60-tick) of mids, bucketed by tercile thresholds
//   updated from a streaming reservoir.
// -----------------------------------------------------------------------------
struct VolRegime {
    static constexpr int ATR_WIN = 60;
    static constexpr int SAMPLE_CAP = 5000;
    std::deque<double> mids;
    std::vector<double> samples;
    double thresh_low = 0.0;
    double thresh_high = 0.0;
    int    n_seen = 0;

    int update_and_bucket(double mid) {
        mids.push_back(mid);
        if ((int)mids.size() > ATR_WIN) mids.pop_front();
        if ((int)mids.size() < ATR_WIN) return 1;  // default med during warmup
        double sum = 0.0;
        for (size_t i = 1; i < mids.size(); ++i) sum += std::fabs(mids[i] - mids[i-1]);
        const double atr = sum / (mids.size() - 1);

        // Reservoir-style sampling
        if ((int)samples.size() < SAMPLE_CAP) {
            samples.push_back(atr);
        } else {
            // Random replace
            int idx = std::rand() % SAMPLE_CAP;
            samples[idx] = atr;
        }
        // Recompute terciles every 1000 samples (cheap O(N log N))
        ++n_seen;
        if (n_seen % 1000 == 0 && samples.size() >= 100) {
            std::vector<double> sorted = samples;
            std::sort(sorted.begin(), sorted.end());
            thresh_low  = sorted[sorted.size() * 1 / 3];
            thresh_high = sorted[sorted.size() * 2 / 3];
        }
        if (thresh_high <= 0.0) return 1;
        if (atr < thresh_low)  return 0;
        if (atr < thresh_high) return 1;
        return 2;
    }
};

// -----------------------------------------------------------------------------
// Hour-of-day UTC from epoch seconds
// -----------------------------------------------------------------------------
static inline int hour_utc_from_ts_s(int64_t ts_s) {
    std::time_t t = static_cast<std::time_t>(ts_s);
    std::tm utc{};
    gmtime_r(&t, &utc);
    return utc.tm_hour;
}

// -----------------------------------------------------------------------------
// Ledger CSV writer
// -----------------------------------------------------------------------------
static const char* exit_reason_str_unused = "";  // silence unused warnings

struct LedgerWriter {
    std::ofstream f;
    bool open(const char* path) {
        f.open(path);
        if (!f.is_open()) return false;
        f << "ts_ms_entry,ts_ms_exit,side,entry_price,exit_price,exit_reason,"
             "duration_s,gross_pts,gross_usd,spread_at_entry_pts,"
             "modeled_cost_usd,net_usd,hour_utc,vol_regime\n";
        return true;
    }
    void write_header() {
        f << "ts_ms_entry,ts_ms_exit,side,entry_price,exit_price,exit_reason,"
             "duration_s,gross_pts,gross_usd,spread_at_entry_pts,"
             "modeled_cost_usd,net_usd,hour_utc,vol_regime\n";
    }
    void write(const omega::ev_pilot::TradeRecord& tr) {
        f << tr.ts_ms_entry << "," << tr.ts_ms_exit << ","
          << (tr.is_long ? "L" : "S") << ","
          << std::fixed << std::setprecision(3) << tr.entry_price << ","
          << tr.exit_price << ","
          << tr.exit_reason << "," << tr.duration_s << ","
          << std::setprecision(4) << tr.gross_pts << ","
          << tr.gross_usd << ","
          << std::setprecision(4) << tr.spread_at_entry_pts << ","
          << tr.modeled_cost_usd << ","
          << tr.net_usd << ","
          << tr.hour_utc << ","
          << tr.vol_regime << "\n";
    }
};

// -----------------------------------------------------------------------------
// Sweep: combo grid + parallel engine harness
// -----------------------------------------------------------------------------
struct ComboConfig {
    double TP_RR;
    double SL_FRAC;
    double MFE_TRAIL_FRAC;
    double TRAIL_FRAC;
};

struct ComboResult {
    ComboConfig cfg;
    int    n_trades  = 0;
    int    n_wins    = 0;
    double gross_usd = 0.0;
    double net_usd   = 0.0;
    double cost_usd  = 0.0;
    double sum_winner_net = 0.0;
    double sum_loser_net  = 0.0;
    int    tp_count    = 0;
    int    trail_count = 0;
    int    sl_count    = 0;
    int    be_count    = 0;
    double equity     = 0.0;
    double peak_eq    = 0.0;
    double max_dd     = 0.0;
    int64_t first_ts_ms = 0;
    int64_t last_ts_ms  = 0;
};

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <month1.csv> [<month2.csv>...] \n"
            "       [--out PATH] [--verbose]\n", argv[0]);
        return 2;
    }

    std::vector<std::string> csv_paths;
    std::string out_path = "backtest/midscalper_tptrail_sweep.csv";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--out") == 0 && i+1 < argc) out_path = argv[++i];
        else if (std::strcmp(a, "--verbose") == 0) verbose = true;
        else if (a[0] != '-') csv_paths.push_back(a);
    }
    if (csv_paths.empty()) {
        std::fprintf(stderr, "[err] no CSV inputs\n");
        return 2;
    }
    std::sort(csv_paths.begin(), csv_paths.end());

    // -------- Grid -----------------------------------------------------------
    // After diagnostic: TP_RR was dead (TP_HIT=0 across all configs); TRAIL_FRAC
    // was the binding constraint. Plan B v2 grid sweeps TRAIL_FRAC, MFE_TRAIL_FRAC,
    // and SL_FRAC; TP_RR pinned at live (4.0).
    // TRAIL_FRAC x MFE_TRAIL_FRAC x SL_FRAC = 5 x 3 x 4 = 60 combos
    const std::vector<double> trail_frac_grid = {0.25, 0.50, 0.75, 1.00, 2.00};
    const std::vector<double> mfe_trail_grid  = {0.55, 0.75, 0.95};
    const std::vector<double> sl_frac_grid    = {0.5, 0.6, 0.7, 0.8};
    const double PINNED_TP_RR = 4.0;

    std::vector<ComboConfig> combos;
    for (double tf : trail_frac_grid)
        for (double mt : mfe_trail_grid)
            for (double sf : sl_frac_grid)
                combos.push_back({PINNED_TP_RR, sf, mt, tf});

    const int N = (int)combos.size();
    std::printf("[sweep] grid v2: %d combos (TRAIL_FRAC x %zu, MFE_TRAIL x %zu, SL_FRAC x %zu); TP_RR pinned at %.2f\n",
                N, trail_frac_grid.size(), mfe_trail_grid.size(), sl_frac_grid.size(), PINNED_TP_RR);
    std::printf("[sweep] MAX_SPREAD pinned at %.3f (Plan A finding)\n",
                (double)omega::ev_pilot::MidScalperEngine::MAX_SPREAD);
    std::printf("[sweep] input files: %zu\n", csv_paths.size());

    // -------- Build N engines + results --------------------------------------
    std::vector<omega::ev_pilot::MidScalperEngine> engines(N);
    std::vector<ComboResult> results(N);
    for (int i = 0; i < N; ++i) {
        engines[i].TP_RR          = combos[i].TP_RR;
        engines[i].SL_FRAC        = combos[i].SL_FRAC;
        engines[i].MFE_TRAIL_FRAC = combos[i].MFE_TRAIL_FRAC;
        engines[i].TRAIL_FRAC     = combos[i].TRAIL_FRAC;
        engines[i].ev_gate        = nullptr;
        results[i].cfg = combos[i];
        // Capture pointer to result slot; vector is sized once and never resized.
        ComboResult* rp = &results[i];
        engines[i].trade_sink = [rp](const omega::ev_pilot::TradeRecord& tr) {
            ComboResult& r = *rp;
            r.n_trades += 1;
            r.gross_usd += tr.gross_usd;
            r.cost_usd  += tr.modeled_cost_usd;
            r.net_usd   += tr.net_usd;
            if (tr.net_usd > 0) { r.n_wins += 1; r.sum_winner_net += tr.net_usd; }
            else                {                 r.sum_loser_net  += tr.net_usd; }
            r.equity += tr.net_usd;
            if (r.equity > r.peak_eq) r.peak_eq = r.equity;
            const double dd = r.peak_eq - r.equity;
            if (dd > r.max_dd) r.max_dd = dd;
            if      (tr.exit_reason == "TP_HIT")    r.tp_count    += 1;
            else if (tr.exit_reason == "TRAIL_HIT") r.trail_count += 1;
            else if (tr.exit_reason == "SL_HIT")    r.sl_count    += 1;
            else if (tr.exit_reason == "BE_HIT")    r.be_count    += 1;
            if (r.first_ts_ms == 0) r.first_ts_ms = tr.ts_ms_entry;
            r.last_ts_ms = tr.ts_ms_exit;
        };
    }

    // -------- Stream + dispatch to all engines -------------------------------
    using Engine = omega::ev_pilot::MidScalperEngine;
    constexpr int WIN_CAP = Engine::STRUCTURE_LOOKBACK * 2;
    std::deque<double> window;
    VolRegime vol;

    TickStreamer streamer;
    int64_t ts_ms = 0; double bid = 0.0, ask = 0.0;
    int64_t total_ticks = 0;
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : csv_paths) {
        if (verbose) std::printf("[sweep] streaming %s\n", path.c_str());
        if (!streamer.open(path.c_str(), false)) continue;
        int64_t file_ticks = 0;
        while (streamer.next(ts_ms, bid, ask)) {
            ++file_ticks; ++total_ticks;
            if (bid <= 0.0 || ask <= 0.0) continue;
            const double mid = 0.5 * (bid + ask);
            window.push_back(mid);
            if ((int)window.size() > WIN_CAP) window.pop_front();
            double w_hi = 0.0, w_lo = 0.0;
            int win_count = (int)window.size();
            if (win_count >= Engine::STRUCTURE_LOOKBACK) {
                w_hi = *std::max_element(window.begin(), window.end());
                w_lo = *std::min_element(window.begin(), window.end());
            }
            const int hour_utc = hour_utc_from_ts_s(ts_ms / 1000);
            const int vol_reg  = vol.update_and_bucket(mid);
            for (int i = 0; i < N; ++i) {
                engines[i].on_tick(bid, ask, ts_ms, win_count, w_hi, w_lo, hour_utc, vol_reg);
            }
            if (verbose && total_ticks % 5000000 == 0) {
                const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
                std::printf("  sweep: %lld ticks, elapsed=%llds\n",
                            (long long)total_ticks, (long long)dt);
                std::fflush(stdout);
            }
        }
        streamer.f.close();
        if (verbose) std::printf("[sweep]   %lld ticks in %s\n", (long long)file_ticks, path.c_str());
    }

    const auto dt_total = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[sweep] processed %lld ticks across %d engines in %llds\n",
                (long long)total_ticks, N, (long long)dt_total);

    // -------- Sort by net_usd descending -------------------------------------
    std::vector<int> order(N);
    for (int i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return results[a].net_usd > results[b].net_usd; });

    // -------- Write leaderboard CSV ------------------------------------------
    std::ofstream f(out_path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[err] cannot write %s\n", out_path.c_str());
        return 1;
    }
    f << "rank,combo_id,TP_RR,SL_FRAC,MFE_TRAIL_FRAC,TRAIL_FRAC,"
         "n_trades,n_wins,net_WR_pct,gross_WR_pct,"
         "net_usd,gross_usd,cost_usd,mean_net_per_trade,"
         "PF,max_dd_usd,tp_count,trail_count,sl_count,be_count\n";
    for (int rank = 0; rank < N; ++rank) {
        const int ci = order[rank];
        const ComboResult& r = results[ci];
        const double net_wr   = r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0;
        const double gross_wr = r.n_trades > 0 ? 100.0 * (r.tp_count + r.trail_count) / r.n_trades : 0.0;
        const double mean_net = r.n_trades > 0 ? r.net_usd / r.n_trades : 0.0;
        const double pf       = (std::fabs(r.sum_loser_net) > 1e-9) ? -r.sum_winner_net / r.sum_loser_net : 0.0;
        f << (rank+1) << "," << ci << ","
          << std::fixed << std::setprecision(3) << r.cfg.TP_RR << ","
          << r.cfg.SL_FRAC << "," << r.cfg.MFE_TRAIL_FRAC << "," << r.cfg.TRAIL_FRAC << ","
          << r.n_trades << "," << r.n_wins << ","
          << std::setprecision(2) << net_wr << "," << gross_wr << ","
          << r.net_usd << "," << r.gross_usd << "," << r.cost_usd << ","
          << std::setprecision(4) << mean_net << ","
          << std::setprecision(4) << pf << ","
          << std::setprecision(2) << r.max_dd << ","
          << r.tp_count << "," << r.trail_count << "," << r.sl_count << "," << r.be_count << "\n";
    }
    f.close();

    // -------- Print top 10 ---------------------------------------------------
    std::printf("\n=== TOP 10 by net_usd ===\n");
    std::printf("%4s %5s %7s %7s %7s %7s %7s %8s %8s %12s %12s %12s %6s %6s %6s\n",
                "rank", "id", "TP_RR", "SL_F", "MFE_T", "TRL_F", "trades", "netWR%", "grWR%",
                "net$", "gross$", "cost$", "TP", "TRAIL", "SL");
    for (int rank = 0; rank < std::min(10, N); ++rank) {
        const int ci = order[rank];
        const ComboResult& r = results[ci];
        const double net_wr   = r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0;
        const double gross_wr = r.n_trades > 0 ? 100.0 * (r.tp_count + r.trail_count) / r.n_trades : 0.0;
        std::printf("%4d %5d %7.2f %7.2f %7.2f %7.2f %7d %8.2f %8.2f %12.2f %12.2f %12.2f %6d %6d %6d\n",
                    rank+1, ci, r.cfg.TP_RR, r.cfg.SL_FRAC, r.cfg.MFE_TRAIL_FRAC, r.cfg.TRAIL_FRAC,
                    r.n_trades, net_wr, gross_wr,
                    r.net_usd, r.gross_usd, r.cost_usd,
                    r.tp_count, r.trail_count, r.sl_count);
    }
    std::printf("\n[sweep] wrote %s\n", out_path.c_str());

    return 0;
}
