// =============================================================================
// gbpusd_london_open_planA_sweep.cpp -- Plan A: baseline + spread/trail grid
// =============================================================================
//
// 2026-05-12 (Claude / Jo): GBPUSD-London-Open analogue of the MidScalper
//   Plan A sweep. Forks the GbpusdLondonOpenEngine class (constexpr -> instance
//   members for MAX_SPREAD, TRAIL_FRAC, MFE_TRAIL_FRAC, SL_FRAC, TP_RR) so the
//   harness can build N parameterized engine instances and dispatch one tick
//   stream to all of them.
//
//   Plan A: sweep MAX_SPREAD x {TRAIL_FRAC x MFE_TRAIL_FRAC} -- one combo per
//   engine -- against the full GBPUSD 16-month HistData tick tape. Writes:
//     - gbpusd_planA_baseline.csv (per-trade ledger, live constexpr config)
//     - gbpusd_planA_stats.csv    (baseline summary)
//     - gbpusd_planA_leaderboard.csv (one row per swept combo, sorted by net)
//
// TICK INPUT
//   HistData ASCII tick CSV:  YYYYMMDD HHMMSSnnn,bid,ask,vol
//     e.g. 20250101 170123324,1.251340,1.252400,0
//   The minute/second portion is "HHMMSS" then 3-digit millisecond suffix.
//
// COST MODEL (applied at close-side, matches MidScalper baseline)
//   commission_usd  = 6.0 * lot       ($0.60 at 0.10 lot, round-trip)
//   spread_usd      = spread_at_entry * 100000 * lot   ($1/pip at 0.10 lot)
//   slippage_usd    = 0.0002 * 100000 * lot   ($2 at 0.10 lot)
//   net_usd         = gross_usd - (commission_usd + spread_usd + slippage_usd)
//
// USAGE
//   ./gbpusd_london_open_planA_sweep <list of monthly HistData csvs ...>
//       --baseline-out PATH --stats-out PATH --leaderboard-out PATH
//
// BUILD
//   g++ -std=c++17 -O3 -DNDEBUG -I include \
//       backtest/gbpusd_london_open_planA_sweep.cpp -o /tmp/gbpusd_london_open_planA_sweep
// =============================================================================

#define OMEGA_BACKTEST 1

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

namespace gbpfx {

// -----------------------------------------------------------------------------
// TradeRecord: emitted per-close.
// -----------------------------------------------------------------------------
struct TradeRecord {
    int64_t ts_ms_entry  = 0;
    int64_t ts_ms_exit   = 0;
    bool    is_long      = false;
    double  entry_price  = 0.0;
    double  exit_price   = 0.0;
    std::string exit_reason;
    int     duration_s   = 0;
    double  gross_pts    = 0.0;   // price units (raw)
    double  gross_usd    = 0.0;   // gross_pts * size * 100000  (USD-quote pair)
    double  spread_at_entry_pts = 0.0;
    double  modeled_cost_usd = 0.0;
    double  net_usd      = 0.0;
    int     hour_utc     = 0;
    double  size         = 0.10;
    double  mfe          = 0.0;
    double  mae          = 0.0;
};

// -----------------------------------------------------------------------------
// Engine: 1:1 fork of GbpusdLondonOpenEngine from include/GbpusdLondonOpenEngine.hpp.
//   Constexpr -> instance members for the swept params. All trade-side guards
//   (BE-lock, trail-arm, same-level block, cold-start, ATR-expansion) preserved
//   identical to the live engine. The session window 07-10 UTC, news blackout
//   bypass for backtest (no g_news_blackout), cost-guard gate replicated.
// -----------------------------------------------------------------------------
class GbpusdLondonOpenEngine {
public:
    // -- Held-fixed --
    static constexpr int    STRUCTURE_LOOKBACK   = 600;
    static constexpr int    MIN_ENTRY_TICKS      = 60;
    static constexpr int    MIN_BREAK_TICKS      = 5;
    static constexpr double SL_BUFFER            = 0.0002;
    static constexpr double MIN_TRAIL_ARM_PTS    = 0.0006;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 30;
    static constexpr double BE_TRIGGER_PTS       = 0.0006;
    static constexpr double BE_OFFSET_PTS        = 0.00015;
    static constexpr double SAME_LEVEL_BLOCK_PTS         = 0.0012;
    static constexpr int    SAME_LEVEL_POST_SL_BLOCK_S   = 1200;
    static constexpr int    SAME_LEVEL_POST_WIN_BLOCK_S  = 600;
    static constexpr double MAX_FILL_SPREAD      = 0.00050;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double USD_PER_PRICE_UNIT   = 10000.0;
    static constexpr double ENTRY_SIZE_DEFAULT   = 0.10;
    static constexpr double LOT_MIN              = 0.01;
    static constexpr double LOT_MAX              = 0.10;
    static constexpr int    PENDING_TIMEOUT_S    = 180;
    static constexpr int    COOLDOWN_S           = 120;
    static constexpr int    SESSION_START_HOUR_UTC = 7;
    static constexpr int    SESSION_END_HOUR_UTC   = 10;
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;

    // -- Swept (instance members; defaults match live constexpr) --
    double  MIN_RANGE       = 0.0012;
    double  MAX_RANGE       = 0.0075;
    double  SL_FRAC         = 0.80;
    double  TP_RR           = 2.0;
    double  TRAIL_FRAC      = 0.30;
    double  MFE_TRAIL_FRAC  = 0.40;
    double  MAX_SPREAD      = 0.00025;

    // Cost gate ratio. Set to 0.0 to disable.
    double  cost_ratio_min  = 1.5;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = ENTRY_SIZE_DEFAULT;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts_ms = 0;
        int     hour_utc_at_entry = 0;
        bool    be_locked = false;
    } pos;

    double bracket_high = 0.0;
    double bracket_low  = 0.0;
    double range        = 0.0;

    std::function<void(const TradeRecord&)> trade_sink = nullptr;

    // Cost-gate viability: replicates ExecutionCostGuard::is_viable for GBPUSD.
    static bool cost_viable_gbpusd(double spread_pts, double tp_dist_pts,
                                   double lot, double cost_ratio_min) noexcept {
        if (cost_ratio_min <= 0.0) return true;
        const double tick_usd_per_lot = 100000.0;
        const double commission_per_lot = 6.0;
        const double slippage_pts = 0.0002;
        const double cost = spread_pts * tick_usd_per_lot * lot
                          + slippage_pts * tick_usd_per_lot * lot
                          + commission_per_lot * lot;
        const double gross = tp_dist_pts * tick_usd_per_lot * lot;
        return gross >= cost * cost_ratio_min;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 int win_count, double w_hi_shared, double w_lo_shared,
                 int hour_utc) noexcept
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
                _confirm_fill(true, bracket_high, ENTRY_SIZE_DEFAULT, spread, now_ms, hour_utc); return;
            }
            if (would_fill_long)  { _confirm_fill(true,  bracket_high, ENTRY_SIZE_DEFAULT, spread, now_ms, hour_utc); return; }
            if (would_fill_short) { _confirm_fill(false, bracket_low,  ENTRY_SIZE_DEFAULT, spread, now_ms, hour_utc); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0;
            }
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if (win_count < STRUCTURE_LOOKBACK) return;
        if (spread > MAX_SPREAD) return;

        // Session gate 07-10 UTC
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
                phase = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_inside_ticks = 0;
                m_armed_ts = now_s;
            }
            return;
        }

        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE) { phase = Phase::IDLE; return; }

            if (mid >= bracket_low && mid <= bracket_high) {
                ++m_inside_ticks;
            } else {
                m_inside_ticks = 0; phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0; return;
            }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            const double min_tp  = spread * 2.0 + 0.0001;
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

            // Lot sizing identical to live engine.
            const double risk_lot = (sl_dist * USD_PER_PRICE_UNIT > 0.0)
                ? (RISK_DOLLARS / (sl_dist * USD_PER_PRICE_UNIT)) : LOT_MAX;
            const double base_lot = std::max(LOT_MIN, std::min(LOT_MAX, risk_lot));

            // Cost-gate (replicates the 2026-05-12 cost gate at lines ~682-689)
            if (!cost_viable_gbpusd(spread, tp_dist, base_lot, cost_ratio_min)) {
                phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return;
            }

            m_pending_lot = base_lot;
            phase = Phase::PENDING;
            m_armed_ts = now_s;
        }
    }

private:
    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start = 0;
    int64_t m_sl_cooldown_ts = 0;
    double  m_sl_price       = 0.0;
    double  m_win_exit_price = 0.0;
    int64_t m_win_exit_block_ts = 0;
    double  m_pending_lot    = ENTRY_SIZE_DEFAULT;
    std::deque<double> m_range_history;

    void _confirm_fill(bool is_long, double fill_px, double fill_lot,
                       double spread_at_fill, int64_t now_ms, int hour_utc) noexcept
    {
        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active = true;
        pos.is_long = is_long;
        pos.entry = fill_px;
        pos.sl = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp = is_long ? (fill_px + tp_dist) : (fill_px - tp_dist);
        pos.size = m_pending_lot;
        pos.mfe = 0.0;
        pos.mae = 0.0;
        pos.spread_at_entry = spread_at_fill;
        pos.entry_ts_ms = now_ms;
        pos.hour_utc_at_entry = hour_utc;
        pos.be_locked = false;
        phase = Phase::LIVE;
    }

    void _manage(double bid, double ask, double mid, int64_t now_s, int64_t now_ms) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        const int64_t held_s = now_s - pos.entry_ts_ms / 1000;

        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            const double effective_offset = (move >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double be_target = pos.is_long ? (pos.entry + effective_offset)
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
            const bool sl_at_be        = (pos.sl <= pos.entry + 0.00005)
                                      && (pos.sl >= pos.entry - 0.00005);
            const bool trail_in_profit = pos.is_long
                ? (pos.sl > pos.entry + 0.00005)
                : (pos.sl < pos.entry - 0.00005);
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
        const bool   is_long_ = pos.is_long;
        const double entry_   = pos.entry;
        const double size_    = pos.size;
        const double sp_     = pos.spread_at_entry;
        const int64_t entry_ts_ms_ = pos.entry_ts_ms;
        const double mfe_     = pos.mfe;
        const double mae_     = pos.mae;
        const int hour_utc_   = pos.hour_utc_at_entry;

        const double gross_pts = is_long_ ? (exit_px - entry_) : (entry_ - exit_px);
        const double gross_usd = gross_pts * size_ * 100000.0;

        // Cost model (close-side):
        //   commission $6/lot RT * size
        //   spread  spread_at_entry * 100000 * size
        //   slippage 0.0002 * 100000 * size
        const double commission_usd = 6.0 * size_;
        const double spread_usd     = sp_ * 100000.0 * size_;
        const double slippage_usd   = 0.0002 * 100000.0 * size_;
        const double modeled_cost   = commission_usd + spread_usd + slippage_usd;
        const double net_usd = gross_usd - modeled_cost;

        if (std::string(reason) == "SL_HIT") {
            m_sl_cooldown_ts = now_s + SAME_LEVEL_POST_SL_BLOCK_S;
            m_sl_price       = entry_;
        }
        if (std::string(reason) == "TRAIL_HIT" || std::string(reason) == "TP_HIT") {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
        }

        TradeRecord tr;
        tr.ts_ms_entry = entry_ts_ms_;
        tr.ts_ms_exit  = now_ms;
        tr.is_long     = is_long_;
        tr.entry_price = entry_;
        tr.exit_price  = exit_px;
        tr.exit_reason = reason;
        tr.duration_s  = (int)(now_s - entry_ts_ms_ / 1000);
        tr.gross_pts   = gross_pts;
        tr.gross_usd   = gross_usd;
        tr.spread_at_entry_pts = sp_;
        tr.modeled_cost_usd = modeled_cost;
        tr.net_usd     = net_usd;
        tr.hour_utc    = hour_utc_;
        tr.size        = size_;
        tr.mfe         = mfe_;
        tr.mae         = mae_;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        if (trade_sink) trade_sink(tr);
    }
};

// -----------------------------------------------------------------------------
// HistData ASCII tick CSV streamer: format YYYYMMDD HHMMSSnnn,bid,ask,vol
//   (note: bid is the FIRST price column for HistData!)
// -----------------------------------------------------------------------------
struct HistTickStreamer {
    std::ifstream f;
    std::string   path;
    std::string   line;
    bool          opened = false;

    bool open(const char* p) {
        path = p;
        f.open(p);
        if (!f.is_open()) {
            std::fprintf(stderr, "[err] cannot open %s\n", p);
            return false;
        }
        opened = true;
        return true;
    }

    // Convert YYYYMMDD HHMMSSnnn to epoch ms (UTC).
    static int64_t parse_hist_ts(const char* s, size_t len) {
        // expected: "YYYYMMDD HHMMSSnnn"  18 chars (no separators in time)
        // Be defensive: also accept trailing whitespace
        if (len < 18) return -1;
        int Y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
        int M = (s[4]-'0')*10 + (s[5]-'0');
        int D = (s[6]-'0')*10 + (s[7]-'0');
        // s[8] is space
        int hh = (s[9]-'0')*10 + (s[10]-'0');
        int mm = (s[11]-'0')*10 + (s[12]-'0');
        int ss = (s[13]-'0')*10 + (s[14]-'0');
        int ms = (s[15]-'0')*100 + (s[16]-'0')*10 + (s[17]-'0');
        struct tm utc{};
        utc.tm_year = Y - 1900; utc.tm_mon = M - 1; utc.tm_mday = D;
        utc.tm_hour = hh; utc.tm_min = mm; utc.tm_sec = ss;
        time_t t = timegm(&utc);
        return (int64_t)t * 1000 + ms;
    }

    bool next(int64_t& ts_ms, double& bid, double& ask) {
        if (!opened) return false;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            // Find commas
            const char* p = line.c_str();
            const char* c1 = std::strchr(p, ',');
            if (!c1) continue;
            const char* c2 = std::strchr(c1 + 1, ',');
            if (!c2) continue;
            // ts is [p .. c1)
            ts_ms = parse_hist_ts(p, (size_t)(c1 - p));
            if (ts_ms < 0) continue;
            // bid is [c1+1 .. c2)
            char buf[32];
            size_t L1 = (size_t)(c2 - (c1 + 1));
            if (L1 >= sizeof(buf)) continue;
            std::memcpy(buf, c1 + 1, L1); buf[L1] = 0;
            bid = std::strtod(buf, nullptr);
            // ask is [c2+1 .. next comma or end]
            const char* c3 = std::strchr(c2 + 1, ',');
            size_t L2 = c3 ? (size_t)(c3 - (c2 + 1)) : std::strlen(c2 + 1);
            if (L2 >= sizeof(buf)) continue;
            std::memcpy(buf, c2 + 1, L2); buf[L2] = 0;
            ask = std::strtod(buf, nullptr);
            // strip trailing whitespace from ask (may have \r\n)
            return true;
        }
        return false;
    }
};

static inline int hour_utc_from_ts_ms(int64_t ts_ms) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm utc{};
    gmtime_r(&t, &utc);
    return utc.tm_hour;
}

} // namespace gbpfx

// -----------------------------------------------------------------------------
// Aggregate stats per engine instance
// -----------------------------------------------------------------------------
struct ComboConfig {
    double MAX_SPREAD;
    double TRAIL_FRAC;
    double MFE_TRAIL_FRAC;
    double TP_RR;
    double SL_FRAC;
    int    tag = 0; // 0 = sweep, 1 = baseline
};

struct ComboResult {
    ComboConfig cfg;
    int    n_trades = 0;
    int    n_wins   = 0;
    double gross_usd = 0.0;
    double net_usd = 0.0;
    double cost_usd = 0.0;
    double sum_win_net = 0.0;
    double sum_loss_net = 0.0;
    int    tp_count = 0;
    int    trail_count = 0;
    int    sl_count = 0;
    int    be_count = 0;
    double equity = 0.0;
    double peak_eq = 0.0;
    double max_dd = 0.0;
    int64_t first_ts_ms = 0;
    int64_t last_ts_ms  = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <histdata_month.csv> [more ...] "
            "[--baseline-out PATH] [--stats-out PATH] [--leaderboard-out PATH]\n", argv[0]);
        return 2;
    }
    std::vector<std::string> csv_paths;
    std::string baseline_out = "backtest/gbpusd_planA_baseline.csv";
    std::string stats_out    = "backtest/gbpusd_planA_stats.csv";
    std::string lb_out       = "backtest/gbpusd_planA_leaderboard.csv";
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a, "--baseline-out")    == 0 && i+1 < argc) baseline_out = argv[++i];
        else if (std::strcmp(a, "--stats-out")       == 0 && i+1 < argc) stats_out    = argv[++i];
        else if (std::strcmp(a, "--leaderboard-out") == 0 && i+1 < argc) lb_out       = argv[++i];
        else if (a[0] != '-') csv_paths.push_back(a);
    }
    if (csv_paths.empty()) {
        std::fprintf(stderr, "[err] no CSV inputs\n");
        return 2;
    }
    std::sort(csv_paths.begin(), csv_paths.end());

    // ---- Build combo grid -------------------------------------------------
    // [0] = baseline (live constexpr)
    // [1..] = sweep: MAX_SPREAD x (TRAIL_FRAC x MFE_TRAIL_FRAC)
    std::vector<double> spread_grid = {0.00015, 0.00020, 0.00025, 0.00030, 0.00040, 0.00050};
    std::vector<double> trail_grid  = {0.15, 0.20, 0.25, 0.30, 0.40, 0.55};
    std::vector<double> mfet_grid   = {0.20, 0.30, 0.40, 0.55, 0.70};

    std::vector<ComboConfig> combos;
    // Baseline first (tag=1)
    combos.push_back({0.00025, 0.30, 0.40, 2.0, 0.80, 1});
    // Sweep
    for (double sp : spread_grid)
        for (double tf : trail_grid)
            for (double mt : mfet_grid)
                combos.push_back({sp, tf, mt, 2.0, 0.80, 0});

    const int N = (int)combos.size();
    std::printf("[planA] %d combos (1 baseline + %d sweep)\n", N, N-1);
    std::printf("[planA] spread x trail x mfet = %zu x %zu x %zu = %zu sweep combos\n",
                spread_grid.size(), trail_grid.size(), mfet_grid.size(),
                spread_grid.size()*trail_grid.size()*mfet_grid.size());
    std::printf("[planA] input files: %zu\n", csv_paths.size());

    // ---- Build engines + results ------------------------------------------
    std::vector<gbpfx::GbpusdLondonOpenEngine> engines(N);
    std::vector<ComboResult> results(N);
    // Baseline ledger writer
    std::ofstream bf(baseline_out);
    bf << "ts_ms_entry,ts_ms_exit,side,entry_price,exit_price,exit_reason,duration_s,"
          "gross_pts,gross_usd,spread_at_entry_pts,modeled_cost_usd,net_usd,hour_utc,size\n";

    for (int i = 0; i < N; ++i) {
        engines[i].MAX_SPREAD     = combos[i].MAX_SPREAD;
        engines[i].TRAIL_FRAC     = combos[i].TRAIL_FRAC;
        engines[i].MFE_TRAIL_FRAC = combos[i].MFE_TRAIL_FRAC;
        engines[i].TP_RR          = combos[i].TP_RR;
        engines[i].SL_FRAC        = combos[i].SL_FRAC;
        // Disable cost gate for clean baseline / sweep; we apply costs in the
        // ledger close path. The live engine carries a 1.5x cost gate; including
        // it here would bias the comparison vs the unfiltered exit-side question.
        // We document this in the report.
        engines[i].cost_ratio_min = 0.0;

        results[i].cfg = combos[i];
        ComboResult* rp = &results[i];
        const bool is_baseline = (combos[i].tag == 1);
        std::ofstream* bfp = is_baseline ? &bf : nullptr;

        engines[i].trade_sink = [rp, bfp](const gbpfx::TradeRecord& tr) {
            ComboResult& r = *rp;
            r.n_trades += 1;
            r.gross_usd += tr.gross_usd;
            r.cost_usd  += tr.modeled_cost_usd;
            r.net_usd   += tr.net_usd;
            if (tr.net_usd > 0) { r.n_wins += 1; r.sum_win_net += tr.net_usd; }
            else                {                 r.sum_loss_net += tr.net_usd; }
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

            if (bfp) {
                (*bfp) << tr.ts_ms_entry << "," << tr.ts_ms_exit << ","
                       << (tr.is_long ? "L" : "S") << ","
                       << std::fixed << std::setprecision(5) << tr.entry_price << ","
                       << tr.exit_price << ","
                       << tr.exit_reason << "," << tr.duration_s << ","
                       << std::setprecision(6) << tr.gross_pts << ","
                       << std::setprecision(4) << tr.gross_usd << ","
                       << std::setprecision(6) << tr.spread_at_entry_pts << ","
                       << std::setprecision(4) << tr.modeled_cost_usd << ","
                       << std::setprecision(4) << tr.net_usd << ","
                       << tr.hour_utc << "," << std::setprecision(3) << tr.size << "\n";
            }
        };
    }

    // ---- Stream + dispatch -------------------------------------------------
    using Engine = gbpfx::GbpusdLondonOpenEngine;
    constexpr int WIN_CAP = Engine::STRUCTURE_LOOKBACK * 2; // matches live engine cap
    // Ring buffer + monotonic deques for O(1) sliding max/min.
    std::vector<double> ring(WIN_CAP, 0.0);
    int ring_head = 0;
    int ring_size = 0;
    int64_t tick_idx = 0;
    std::deque<int64_t> dq_max; // indices, decreasing values
    std::deque<int64_t> dq_min; // indices, increasing values

    gbpfx::HistTickStreamer streamer;
    int64_t ts_ms = 0; double bid = 0.0, ask = 0.0;
    int64_t total_ticks = 0;
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : csv_paths) {
        std::printf("[planA] streaming %s\n", path.c_str());
        std::fflush(stdout);
        if (!streamer.open(path.c_str())) continue;
        int64_t file_ticks = 0;
        while (streamer.next(ts_ms, bid, ask)) {
            ++file_ticks; ++total_ticks;
            if (bid <= 0.0 || ask <= 0.0) continue;
            const double mid = 0.5 * (bid + ask);

            // Push mid into ring + monotonic deques. Index domain is tick_idx
            // (global monotonic) so we evict when idx_oldest <= tick_idx - WIN_CAP.
            const int64_t expire_threshold = tick_idx - WIN_CAP + 1;
            while (!dq_max.empty() && dq_max.front() < expire_threshold) dq_max.pop_front();
            while (!dq_min.empty() && dq_min.front() < expire_threshold) dq_min.pop_front();
            // Pop back while strictly less / greater
            while (!dq_max.empty() && ring[dq_max.back() % WIN_CAP] <= mid) dq_max.pop_back();
            while (!dq_min.empty() && ring[dq_min.back() % WIN_CAP] >= mid) dq_min.pop_back();
            ring[tick_idx % WIN_CAP] = mid;
            dq_max.push_back(tick_idx);
            dq_min.push_back(tick_idx);
            if (ring_size < WIN_CAP) ++ring_size;

            const int win_count = ring_size;
            double w_hi = 0.0, w_lo = 0.0;
            // Engine expects max/min over the rolling window (live engine caps
            // m_window at STRUCTURE_LOOKBACK*2). Once the window has at least
            // STRUCTURE_LOOKBACK ticks, we expose the rolling max/min so the
            // engine can attempt to arm. Pre-warmup ticks still pass through.
            if (win_count >= Engine::STRUCTURE_LOOKBACK) {
                w_hi = ring[dq_max.front() % WIN_CAP];
                w_lo = ring[dq_min.front() % WIN_CAP];
            }
            const int hour_utc = gbpfx::hour_utc_from_ts_ms(ts_ms);
            for (int i = 0; i < N; ++i) {
                engines[i].on_tick(bid, ask, ts_ms, win_count, w_hi, w_lo, hour_utc);
            }
            ++tick_idx;
            if (total_ticks % 5000000 == 0) {
                const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
                std::printf("  [planA] %lld ticks elapsed=%llds\n",
                            (long long)total_ticks, (long long)dt);
                std::fflush(stdout);
            }
        }
        streamer.f.close();
        std::printf("  [planA] %lld ticks in %s\n", (long long)file_ticks, path.c_str());
        std::fflush(stdout);
        (void)ring_head;
    }
    bf.close();

    const auto dt_total = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[planA] processed %lld ticks across %d engines in %llds\n",
                (long long)total_ticks, N, (long long)dt_total);

    // ---- Write stats CSV (baseline only) ----------------------------------
    {
        const ComboResult& r = results[0];
        std::ofstream sf(stats_out);
        sf << "metric,value\n";
        sf << "n_trades," << r.n_trades << "\n";
        sf << "n_wins," << r.n_wins << "\n";
        sf << "n_losses," << (r.n_trades - r.n_wins) << "\n";
        sf << "WR_pct," << std::fixed << std::setprecision(2)
           << (r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0) << "\n";
        sf << "gross_usd," << std::setprecision(2) << r.gross_usd << "\n";
        sf << "cost_usd,"  << r.cost_usd  << "\n";
        sf << "net_usd,"   << r.net_usd   << "\n";
        sf << "mean_net_per_trade,"
           << (r.n_trades > 0 ? r.net_usd / r.n_trades : 0.0) << "\n";
        const double pf = (std::fabs(r.sum_loss_net) > 1e-9)
            ? -r.sum_win_net / r.sum_loss_net : 0.0;
        sf << "PF," << std::setprecision(4) << pf << "\n";
        sf << "max_dd_usd," << std::setprecision(2) << r.max_dd << "\n";
        sf << "tp_count," << r.tp_count << "\n";
        sf << "trail_count," << r.trail_count << "\n";
        sf << "sl_count," << r.sl_count << "\n";
        sf << "be_count," << r.be_count << "\n";
        sf.close();
    }

    // ---- Write leaderboard CSV --------------------------------------------
    std::vector<int> order(N);
    for (int i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return results[a].net_usd > results[b].net_usd; });

    std::ofstream lf(lb_out);
    lf << "rank,combo_id,tag,MAX_SPREAD,TRAIL_FRAC,MFE_TRAIL_FRAC,TP_RR,SL_FRAC,"
          "n_trades,n_wins,WR_pct,net_usd,gross_usd,cost_usd,mean_net,PF,max_dd_usd,"
          "tp_count,trail_count,sl_count,be_count\n";
    for (int rank = 0; rank < N; ++rank) {
        const int ci = order[rank];
        const ComboResult& r = results[ci];
        const double wr = r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0;
        const double mn = r.n_trades > 0 ? r.net_usd / r.n_trades : 0.0;
        const double pf = (std::fabs(r.sum_loss_net) > 1e-9)
            ? -r.sum_win_net / r.sum_loss_net : 0.0;
        lf << (rank+1) << "," << ci << "," << r.cfg.tag << ","
           << std::fixed << std::setprecision(5) << r.cfg.MAX_SPREAD << ","
           << std::setprecision(3) << r.cfg.TRAIL_FRAC << "," << r.cfg.MFE_TRAIL_FRAC << ","
           << r.cfg.TP_RR << "," << r.cfg.SL_FRAC << ","
           << r.n_trades << "," << r.n_wins << ","
           << std::setprecision(2) << wr << ","
           << r.net_usd << "," << r.gross_usd << "," << r.cost_usd << ","
           << std::setprecision(4) << mn << "," << pf << ","
           << std::setprecision(2) << r.max_dd << ","
           << r.tp_count << "," << r.trail_count << "," << r.sl_count << "," << r.be_count << "\n";
    }
    lf.close();

    // ---- Print baseline + top 10 -----------------------------------------
    {
        const ComboResult& b = results[0];
        std::printf("\n=== BASELINE (live constexpr) ===\n");
        std::printf("  n_trades=%d wins=%d (WR=%.2f%%) net=$%.2f gross=$%.2f cost=$%.2f\n",
                    b.n_trades, b.n_wins,
                    b.n_trades > 0 ? 100.0 * b.n_wins / b.n_trades : 0.0,
                    b.net_usd, b.gross_usd, b.cost_usd);
        std::printf("  TP=%d TRAIL=%d SL=%d BE=%d max_dd=$%.2f\n",
                    b.tp_count, b.trail_count, b.sl_count, b.be_count, b.max_dd);
    }
    std::printf("\n=== TOP 10 by net_usd ===\n");
    std::printf("%4s %5s %4s %9s %7s %7s %7s %7s %8s %12s %12s %8s\n",
                "rank","id","tag","MAX_SPR","TRL_F","MFE_T","TP_RR","SL_F","trades","net$","gross$","WR%");
    for (int rank = 0; rank < std::min(10, N); ++rank) {
        const int ci = order[rank];
        const ComboResult& r = results[ci];
        const double wr = r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0;
        std::printf("%4d %5d %4d %9.5f %7.2f %7.2f %7.2f %7.2f %8d %12.2f %12.2f %8.2f\n",
                    rank+1, ci, r.cfg.tag, r.cfg.MAX_SPREAD, r.cfg.TRAIL_FRAC, r.cfg.MFE_TRAIL_FRAC,
                    r.cfg.TP_RR, r.cfg.SL_FRAC,
                    r.n_trades, r.net_usd, r.gross_usd, wr);
    }
    std::printf("\n[planA] wrote %s, %s, %s\n",
                baseline_out.c_str(), stats_out.c_str(), lb_out.c_str());
    return 0;
}
