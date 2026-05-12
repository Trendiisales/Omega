// =============================================================================
// midscalper_ev_pilot.cpp -- Path B EV-guard pilot for GoldMidScalperEngine
// =============================================================================
//
// 2026-05-12 (Claude / Jo): Pilot harness for the universal pre-fire EV gate.
//   Re-uses the MidScalper engine port from midscalper_crtp_sweep.cpp at a
//   single configuration (LIVE: CD=180, PW=600, PSL=900, MET=30, MSP=2.5).
//   Streams monthly XAUUSD tick CSVs end-to-end.
//
// PASSES
//   Pass 1 (BASELINE):
//     - Run engine with NO EV gate.
//     - Write baseline trade ledger.
//     - Aggregate per-bucket (spread x hour) statistics: n, wins, sum_winner,
//       sum_loser. Persist to ev_stats CSV.
//
//   Pass 2 (EV-GATED):
//     - Load baseline stats into OmegaEVGuard.
//     - Re-run engine; at the moment a FIRE would otherwise occur, consult
//       EVGuard.evaluate(spread_pt, hour_utc). If ev > safety_margin (default
//       $0.20 USD), allow the FIRE; otherwise skip and return engine to IDLE.
//     - Write EV-gated trade ledger.
//
// FEATURES at trade entry (recorded to ledger):
//   ts_ms, side, entry_price, exit_price, exit_reason, duration_s, gross_pts,
//   gross_usd, spread_at_entry_pts, modeled_cost_usd, net_usd, hour_of_day_utc,
//   vol_regime (ATR ratio: low/med/high)
//
// VOL REGIME: ATR(60-tick) of the last 60 mids, bucketed by tercile thresholds
//   computed on-the-fly from a rolling 5000-sample reservoir.
//
// LEDGER COLUMNS (output CSV):
//   ts_ms,side,entry_price,exit_price,exit_reason,duration_s,gross_pts,
//   gross_usd,spread_at_entry_pts,modeled_cost_usd,net_usd,hour_utc,vol_regime
//
// USAGE
//   ./midscalper_ev_pilot <list of monthly XAUUSD csvs ...>
//       --baseline-out PATH --evgated-out PATH --stats-out PATH
//       [--month-cutoff YYYY-MM]   (calibration cutoff; default = all)
//
// BUILD
//   g++ -std=c++17 -O3 -DNDEBUG -I include \
//       backtest/midscalper_ev_pilot.cpp -o /tmp/midscalper_ev_pilot
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

#include "../include/OmegaEVGuard.hpp"

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
    static constexpr double SL_FRAC               = 0.6;
    static constexpr double SL_BUFFER             = 1.0;
    static constexpr double TP_RR                 = 4.0;
    static constexpr double TRAIL_FRAC            = 0.25;
    static constexpr double MIN_TRAIL_ARM_PTS     = 5.0;
    static constexpr int    MIN_TRAIL_ARM_SECS    = 15;
    static constexpr double MFE_TRAIL_FRAC        = 0.55;
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
    static constexpr double MAX_SPREAD            = 2.5;
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
// Run a single pass over the given tick files. Returns total trades emitted.
// -----------------------------------------------------------------------------
struct PassStats {
    int    n_trades  = 0;
    int    n_wins    = 0;
    double gross_usd = 0.0;
    double net_usd   = 0.0;
    double total_cost_usd = 0.0;
    double sum_winner_net = 0.0;
    double sum_loser_net  = 0.0;
    int    n_skipped_by_ev = 0;
    int64_t first_ts_ms = 0;
    int64_t last_ts_ms  = 0;
    double  peak_equity = 0.0;
    double  equity      = 0.0;
    double  max_dd      = 0.0;
};

template <typename TradeFn>
static PassStats run_pass(const std::vector<std::string>& csv_paths,
                          omega::ev_pilot::MidScalperEngine& engine,
                          TradeFn&& on_trade,
                          bool verbose)
{
    PassStats st{};
    using Engine = omega::ev_pilot::MidScalperEngine;
    constexpr int WIN_CAP = Engine::STRUCTURE_LOOKBACK * 2;
    std::deque<double> window;

    VolRegime vol;

    engine.trade_sink = [&](const omega::ev_pilot::TradeRecord& tr) {
        st.n_trades += 1;
        st.gross_usd += tr.gross_usd;
        st.net_usd   += tr.net_usd;
        st.total_cost_usd += tr.modeled_cost_usd;
        if (tr.net_usd > 0) { st.n_wins += 1; st.sum_winner_net += tr.net_usd; }
        else                { st.sum_loser_net  += tr.net_usd; }
        st.equity += tr.net_usd;
        if (st.equity > st.peak_equity) st.peak_equity = st.equity;
        const double dd = st.peak_equity - st.equity;
        if (dd > st.max_dd) st.max_dd = dd;
        if (st.first_ts_ms == 0) st.first_ts_ms = tr.ts_ms_entry;
        st.last_ts_ms = tr.ts_ms_exit;
        on_trade(tr);
    };

    TickStreamer streamer;
    int64_t ts_ms = 0; double bid = 0.0, ask = 0.0;
    int64_t total_ticks = 0;
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : csv_paths) {
        if (verbose) std::printf("[bt] streaming %s\n", path.c_str());
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
            engine.on_tick(bid, ask, ts_ms, win_count, w_hi, w_lo, hour_utc, vol_reg);

            if (verbose && total_ticks % 5'000'000 == 0) {
                const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
                std::printf("  pass: %lld ticks, %d trades, elapsed=%llds\n",
                            (long long)total_ticks, st.n_trades, (long long)dt);
                std::fflush(stdout);
            }
        }
        streamer.f.close();
        if (verbose) std::printf("[bt]   %lld ticks in %s\n", (long long)file_ticks, path.c_str());
    }
    st.n_skipped_by_ev = engine.skipped_by_ev;
    return st;
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <month1.csv> [<month2.csv>...] \n"
            "       --baseline-out PATH --evgated-out PATH --stats-out PATH\n"
            "       [--cal-months N]   (use first N files for calibration)\n"
            "       [--verbose]\n", argv[0]);
        return 2;
    }

    std::vector<std::string> all_paths;
    std::string baseline_out = "backtest/midscalper_2yr_baseline_ledger.csv";
    std::string evgated_out  = "backtest/midscalper_2yr_evgated_ledger.csv";
    std::string stats_out    = "backtest/midscalper_2yr_ev_stats.csv";
    int  cal_months = 0;  // 0 = use all data for both calibration and test (in-sample)
    int  pass_mode = 0;   // 0 = both, 1 = baseline only, 2 = ev-gated only
    bool verbose = false;
    bool ledger_append = false;  // append to ledger (for chunked runs)
    bool no_train = false;       // when true, pass1 writes ledger but does NOT update guard stats
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--baseline-out") == 0 && i+1 < argc) baseline_out = argv[++i];
        else if (std::strcmp(a, "--evgated-out") == 0 && i+1 < argc) evgated_out = argv[++i];
        else if (std::strcmp(a, "--stats-out") == 0 && i+1 < argc) stats_out = argv[++i];
        else if (std::strcmp(a, "--cal-months") == 0 && i+1 < argc) cal_months = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--pass") == 0 && i+1 < argc) pass_mode = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--append") == 0) ledger_append = true;
        else if (std::strcmp(a, "--no-train") == 0) no_train = true;
        else if (std::strcmp(a, "--verbose") == 0) verbose = true;
        else if (a[0] != '-') all_paths.push_back(a);
    }
    if (all_paths.empty()) {
        std::fprintf(stderr, "[err] no CSV inputs\n");
        return 2;
    }
    std::sort(all_paths.begin(), all_paths.end());

    std::vector<std::string> cal_paths, test_paths;
    if (cal_months <= 0 || cal_months >= (int)all_paths.size()) {
        cal_paths = all_paths;
        test_paths = all_paths;
        std::printf("[bt] EV pilot mode: IN-SAMPLE (all %zu files for cal + test)\n", all_paths.size());
    } else {
        cal_paths.assign(all_paths.begin(), all_paths.begin() + cal_months);
        test_paths.assign(all_paths.begin() + cal_months, all_paths.end());
        std::printf("[bt] EV pilot mode: OOS split. cal=%zu files, test=%zu files\n",
                    cal_paths.size(), test_paths.size());
    }

    // ============ PASS 1: BASELINE on cal_paths (also used as full record) ===
    omega::EVGuard guard;
    guard.set_cost_xauusd_01lot();

    PassStats baseline_cal_stats, baseline_test_stats;

    if (pass_mode == 0 || pass_mode == 1) {
        std::printf("\n=== PASS 1 (BASELINE) ===\n");
        omega::ev_pilot::MidScalperEngine eng1;
        eng1.ev_gate = nullptr;

        LedgerWriter baseline_writer;
        // For the baseline ledger we want the FULL run (both cal + test segments
        // if OOS mode), so we run two sub-passes: first cal, write ledger AND
        // record stats; then test, write ledger (continue) but NO stats record.
        // Open mode depends on --append flag (true for chunked runs).
        std::ios_base::openmode mode = ledger_append
            ? (std::ios_base::out | std::ios_base::app)
            : std::ios_base::out;
        baseline_writer.f.open(baseline_out.c_str(), mode);
        if (!baseline_writer.f.is_open()) {
            std::fprintf(stderr, "[err] cannot write %s\n", baseline_out.c_str()); return 1;
        }
        if (!ledger_append) {
            baseline_writer.f << "ts_ms_entry,ts_ms_exit,side,entry_price,exit_price,exit_reason,"
                                 "duration_s,gross_pts,gross_usd,spread_at_entry_pts,"
                                 "modeled_cost_usd,net_usd,hour_utc,vol_regime\n";
        }
        // Load existing stats if appending so totals accumulate.
        if (ledger_append) guard.load_stats(stats_out.c_str());

        baseline_cal_stats = run_pass(cal_paths, eng1,
            [&](const omega::ev_pilot::TradeRecord& tr) {
                baseline_writer.write(tr);
                if (!no_train) {
                    const int hr = tr.hour_utc;
                    guard.record_trade(tr.gross_usd, tr.spread_at_entry_pts, hr);
                }
            }, verbose);

        if (!test_paths.empty() && test_paths != cal_paths) {
            baseline_test_stats = run_pass(test_paths, eng1,
                [&](const omega::ev_pilot::TradeRecord& tr) {
                    baseline_writer.write(tr);
                }, verbose);
        }
        baseline_writer.f.close();

        guard.freeze();
        guard.save_stats(stats_out.c_str());
    }

    PassStats baseline_total;
    baseline_total.n_trades  = baseline_cal_stats.n_trades + baseline_test_stats.n_trades;
    baseline_total.n_wins    = baseline_cal_stats.n_wins   + baseline_test_stats.n_wins;
    baseline_total.gross_usd = baseline_cal_stats.gross_usd + baseline_test_stats.gross_usd;
    baseline_total.net_usd   = baseline_cal_stats.net_usd   + baseline_test_stats.net_usd;
    baseline_total.total_cost_usd = baseline_cal_stats.total_cost_usd + baseline_test_stats.total_cost_usd;
    baseline_total.sum_winner_net = baseline_cal_stats.sum_winner_net + baseline_test_stats.sum_winner_net;
    baseline_total.sum_loser_net  = baseline_cal_stats.sum_loser_net + baseline_test_stats.sum_loser_net;
    baseline_total.first_ts_ms = baseline_cal_stats.first_ts_ms ?: baseline_test_stats.first_ts_ms;
    baseline_total.last_ts_ms  = baseline_test_stats.last_ts_ms ?: baseline_cal_stats.last_ts_ms;
    // Recompute max DD across segments? we keep the simple sum for the report;
    // detailed DD comes from the ledger.
    baseline_total.max_dd = std::max(baseline_cal_stats.max_dd, baseline_test_stats.max_dd);

    if (pass_mode == 0 || pass_mode == 1) {
        std::printf("\n[BASELINE] trades=%d wins=%d gross=%.2f net=%.2f cost=%.2f\n",
                    baseline_total.n_trades, baseline_total.n_wins,
                    baseline_total.gross_usd, baseline_total.net_usd,
                    baseline_total.total_cost_usd);
        std::printf("[BASELINE] sum_winner_net=%.2f sum_loser_net=%.2f max_dd=%.2f\n",
                    baseline_total.sum_winner_net, baseline_total.sum_loser_net,
                    baseline_total.max_dd);
        std::printf("[BASELINE] wrote %s, %s\n", baseline_out.c_str(), stats_out.c_str());
    }

    // ============ PASS 2: EV-GATED ============================================
    PassStats ev_stats{};
    if (pass_mode == 0 || pass_mode == 2) {
        std::printf("\n=== PASS 2 (EV-GATED) ===\n");
        // For pass 2 in standalone mode, load stats from disk.
        if (pass_mode == 2) {
            if (!guard.load_stats(stats_out.c_str())) {
                std::fprintf(stderr, "[err] cannot load stats from %s\n", stats_out.c_str());
                return 1;
            }
            std::printf("[bt] loaded ev_stats from %s\n", stats_out.c_str());
        }

        omega::ev_pilot::MidScalperEngine eng2;
        eng2.ev_gate = [&guard](double spread_pt, int hour_utc) -> bool {
            return guard.should_fire(spread_pt, hour_utc);
        };

        LedgerWriter ev_writer;
        std::ios_base::openmode mode = ledger_append
            ? (std::ios_base::out | std::ios_base::app)
            : std::ios_base::out;
        ev_writer.f.open(evgated_out.c_str(), mode);
        if (!ev_writer.f.is_open()) {
            std::fprintf(stderr, "[err] cannot write %s\n", evgated_out.c_str()); return 1;
        }
        if (!ledger_append) {
            ev_writer.f << "ts_ms_entry,ts_ms_exit,side,entry_price,exit_price,exit_reason,"
                           "duration_s,gross_pts,gross_usd,spread_at_entry_pts,"
                           "modeled_cost_usd,net_usd,hour_utc,vol_regime\n";
        }

        ev_stats = run_pass(test_paths, eng2,
            [&](const omega::ev_pilot::TradeRecord& tr) { ev_writer.write(tr); },
            verbose);
        ev_writer.f.close();
    }

    if (pass_mode == 1) return 0;  // baseline-only done

    std::printf("\n[EV-GATED] trades=%d wins=%d gross=%.2f net=%.2f cost=%.2f\n",
                ev_stats.n_trades, ev_stats.n_wins,
                ev_stats.gross_usd, ev_stats.net_usd,
                ev_stats.total_cost_usd);
    std::printf("[EV-GATED] sum_winner_net=%.2f sum_loser_net=%.2f max_dd=%.2f\n",
                ev_stats.sum_winner_net, ev_stats.sum_loser_net, ev_stats.max_dd);
    std::printf("[EV-GATED] skipped_by_ev=%d (would have fired without gate)\n",
                ev_stats.n_skipped_by_ev);

    // ============ COMPARISON =================================================
    // For comparison we use whichever scope ev_stats covered (test segment).
    // If cal == test (in-sample mode), baseline_total spans the same data;
    // for OOS mode, we should compare baseline_test_stats vs ev_stats.
    const PassStats& baseline_cmp = (cal_paths == test_paths) ? baseline_total : baseline_test_stats;

    auto pct = [](double a, double b) {
        if (std::fabs(b) < 1e-9) return 0.0;
        return 100.0 * (a - b) / std::fabs(b);
    };
    auto days_between = [](int64_t a_ms, int64_t b_ms) {
        if (a_ms == 0 || b_ms == 0 || b_ms <= a_ms) return 1.0;
        return (b_ms - a_ms) / 1000.0 / 86400.0;
    };
    const double base_days = days_between(baseline_cmp.first_ts_ms, baseline_cmp.last_ts_ms);
    const double ev_days   = days_between(ev_stats.first_ts_ms, ev_stats.last_ts_ms);

    auto wr   = [](const PassStats& s){ return s.n_trades>0 ? 100.0 * s.n_wins / s.n_trades : 0.0; };
    auto mgr  = [](const PassStats& s){ return s.n_trades>0 ? s.gross_usd / s.n_trades : 0.0; };
    auto mc   = [](const PassStats& s){ return s.n_trades>0 ? s.total_cost_usd / s.n_trades : 0.0; };
    auto mnt  = [](const PassStats& s){ return s.n_trades>0 ? s.net_usd / s.n_trades : 0.0; };
    auto tpd  = [](const PassStats& s, double d){ return d>0 ? s.n_trades / d : 0.0; };
    auto pf   = [](const PassStats& s){
        return (s.sum_loser_net != 0.0) ? -s.sum_winner_net / s.sum_loser_net : 0.0;
    };

    std::printf("\n=== SIDE-BY-SIDE COMPARISON ===\n");
    std::printf("                          BASELINE         EV-GATED       DELTA\n");
    std::printf("trades total          %12d   %12d   %+8.1f%%\n",
                baseline_cmp.n_trades, ev_stats.n_trades,
                pct(ev_stats.n_trades, baseline_cmp.n_trades));
    std::printf("trades/day            %12.2f   %12.2f   %+8.1f%%\n",
                tpd(baseline_cmp, base_days), tpd(ev_stats, ev_days),
                pct(tpd(ev_stats, ev_days), tpd(baseline_cmp, base_days)));
    std::printf("win rate              %11.2f%%   %11.2f%%   %+5.2fpp\n",
                wr(baseline_cmp), wr(ev_stats), wr(ev_stats) - wr(baseline_cmp));
    std::printf("mean gross/trade  $   %12.4f   %12.4f   %+8.1f%%\n",
                mgr(baseline_cmp), mgr(ev_stats),
                pct(mgr(ev_stats), mgr(baseline_cmp)));
    std::printf("mean cost/trade   $   %12.4f   %12.4f   %+8.1f%%\n",
                mc(baseline_cmp), mc(ev_stats),
                pct(mc(ev_stats), mc(baseline_cmp)));
    std::printf("mean net/trade    $   %12.4f   %12.4f   %+8.1f%%\n",
                mnt(baseline_cmp), mnt(ev_stats),
                pct(mnt(ev_stats), mnt(baseline_cmp)));
    std::printf("total net         $   %12.2f   %12.2f   %+8.1f%%\n",
                baseline_cmp.net_usd, ev_stats.net_usd,
                pct(ev_stats.net_usd, baseline_cmp.net_usd));
    std::printf("PF                    %12.3f   %12.3f\n",
                pf(baseline_cmp), pf(ev_stats));
    std::printf("max DD            $   %12.2f   %12.2f   %+8.1f%%\n",
                baseline_cmp.max_dd, ev_stats.max_dd,
                pct(ev_stats.max_dd, baseline_cmp.max_dd));

    // Dump per-bucket EV table.
    std::printf("\n=== EV BUCKET TABLE (frozen) ===\n");
    guard.dump_buckets(stdout);

    return 0;
}
