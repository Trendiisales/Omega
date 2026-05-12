// =============================================================================
// gbpusd_london_open_planB_sweep.cpp -- Plan B: TP/SL/Trail/MFE 4D sweep
// =============================================================================
//
// 2026-05-12 (Claude / Jo): GBPUSD-London-Open analogue of MidScalper Plan B.
//   4D grid: TP_RR x SL_FRAC x TRAIL_FRAC x MFE_TRAIL_FRAC = 4 x 5 x 3 x 3 = 180
//   MAX_SPREAD pinned to Plan A best (0.00020). Same engine fork as Plan A.
//
//   ALSO computes per-engine OOS stats on the last 4 months of the input. The
//   final 4 input files are flagged via --oos-files-trailing N (default 4)
//   and the harness tracks per-engine trades with entry_ts >= oos_cutoff_ms
//   separately. This means the IS results match the prior MidScalper "stats
//   across the full 24m tape" definition while OOS is genuine held-out.
//
// USAGE
//   ./gbpusd_london_open_planB_sweep <list of monthly HistData csvs ...>
//       [--oos-trailing 4]   (use last N input files for OOS)
//       [--max-spread 0.00020]
//       [--leaderboard-out PATH] [--oos-out PATH]
//
// BUILD
//   g++ -std=c++17 -O3 -DNDEBUG -I include \
//       backtest/gbpusd_london_open_planB_sweep.cpp -o /tmp/gbpusd_planB
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

struct TradeRecord {
    int64_t ts_ms_entry  = 0;
    int64_t ts_ms_exit   = 0;
    bool    is_long      = false;
    double  entry_price  = 0.0;
    double  exit_price   = 0.0;
    std::string exit_reason;
    int     duration_s   = 0;
    double  gross_pts    = 0.0;
    double  gross_usd    = 0.0;
    double  spread_at_entry_pts = 0.0;
    double  modeled_cost_usd = 0.0;
    double  net_usd      = 0.0;
    int     hour_utc     = 0;
    double  size         = 0.10;
};

class GbpusdLondonOpenEngine {
public:
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

    // Swept
    double  MIN_RANGE       = 0.0012;
    double  MAX_RANGE       = 0.0075;
    double  SL_FRAC         = 0.80;
    double  TP_RR           = 2.0;
    double  TRAIL_FRAC      = 0.30;
    double  MFE_TRAIL_FRAC  = 0.40;
    double  MAX_SPREAD      = 0.00020;
    double  cost_ratio_min  = 0.0;

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

    static bool cost_viable_gbpusd(double spread, double tp_dist, double lot,
                                   double ratio) noexcept {
        if (ratio <= 0.0) return true;
        const double cost = spread*100000.0*lot + 0.0002*100000.0*lot + 6.0*lot;
        const double gross = tp_dist*100000.0*lot;
        return gross >= cost * ratio;
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
        if (phase == Phase::LIVE) { _manage(bid, ask, mid, now_s, now_ms); return; }
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
        if (hour_utc < SESSION_START_HOUR_UTC || hour_utc >= SESSION_END_HOUR_UTC) return;

        const double w_hi = w_hi_shared, w_lo = w_lo_shared;
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
                bracket_high = w_hi; bracket_low = w_lo;
                m_inside_ticks = 0; m_armed_ts = now_s;
            }
            return;
        }

        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE) { phase = Phase::IDLE; return; }
            if (mid >= bracket_low && mid <= bracket_high) ++m_inside_ticks;
            else { m_inside_ticks = 0; phase = Phase::IDLE;
                   bracket_high = bracket_low = range = 0.0; return; }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            const double min_tp  = spread * 2.0 + 0.0001;
            if (tp_dist < min_tp) { phase = Phase::IDLE; return; }

            m_range_history.push_back(range);
            if ((int)m_range_history.size() > EXPANSION_HISTORY_LEN) m_range_history.pop_front();
            if ((int)m_range_history.size() < EXPANSION_MIN_HISTORY) {
                phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
            }
            {
                std::vector<double> sorted(m_range_history.begin(), m_range_history.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t n = sorted.size();
                const double median = (n % 2 == 1) ? sorted[n/2] : 0.5*(sorted[n/2-1]+sorted[n/2]);
                if (range < median * EXPANSION_MULT) {
                    phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
                }
            }
            const double risk_lot = (sl_dist * USD_PER_PRICE_UNIT > 0.0)
                ? (RISK_DOLLARS / (sl_dist * USD_PER_PRICE_UNIT)) : LOT_MAX;
            const double base_lot = std::max(LOT_MIN, std::min(LOT_MAX, risk_lot));
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

    void _confirm_fill(bool is_long, double fill_px, double, double sp, int64_t now_ms, int hour) noexcept {
        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active = true; pos.is_long = is_long; pos.entry = fill_px;
        pos.sl = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp = is_long ? (fill_px + tp_dist) : (fill_px - tp_dist);
        pos.size = m_pending_lot; pos.mfe = 0; pos.mae = 0;
        pos.spread_at_entry = sp; pos.entry_ts_ms = now_ms;
        pos.hour_utc_at_entry = hour; pos.be_locked = false;
        phase = Phase::LIVE;
    }

    void _manage(double bid, double ask, double mid, int64_t now_s, int64_t now_ms) noexcept {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;
        const int64_t held_s = now_s - pos.entry_ts_ms / 1000;

        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            const double off = (move >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double t = pos.is_long ? (pos.entry + off) : (pos.entry - off);
            if (pos.is_long  && t > pos.sl) pos.sl = t;
            if (!pos.is_long && t < pos.sl) pos.sl = t;
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
            const bool sl_at_be = (pos.sl <= pos.entry + 0.00005) && (pos.sl >= pos.entry - 0.00005);
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

    void _close(double exit_px, const char* reason, int64_t now_s, int64_t now_ms) noexcept {
        if (!pos.active) return;
        const bool   is_long_ = pos.is_long; const double entry_ = pos.entry;
        const double size_ = pos.size; const double sp_ = pos.spread_at_entry;
        const int64_t entry_ts_ms_ = pos.entry_ts_ms;
        const int hour_ = pos.hour_utc_at_entry;
        const double gross_pts = is_long_ ? (exit_px - entry_) : (entry_ - exit_px);
        const double gross_usd = gross_pts * size_ * 100000.0;
        const double cost = 6.0*size_ + sp_*100000.0*size_ + 0.0002*100000.0*size_;
        const double net = gross_usd - cost;

        if (std::string(reason) == "SL_HIT") {
            m_sl_cooldown_ts = now_s + SAME_LEVEL_POST_SL_BLOCK_S; m_sl_price = entry_;
        }
        if (std::string(reason) == "TRAIL_HIT" || std::string(reason) == "TP_HIT") {
            m_win_exit_price = exit_px; m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
        }
        TradeRecord tr;
        tr.ts_ms_entry = entry_ts_ms_; tr.ts_ms_exit = now_ms;
        tr.is_long = is_long_; tr.entry_price = entry_; tr.exit_price = exit_px;
        tr.exit_reason = reason; tr.duration_s = (int)(now_s - entry_ts_ms_ / 1000);
        tr.gross_pts = gross_pts; tr.gross_usd = gross_usd;
        tr.spread_at_entry_pts = sp_; tr.modeled_cost_usd = cost;
        tr.net_usd = net; tr.hour_utc = hour_; tr.size = size_;

        pos = LivePos{}; phase = Phase::COOLDOWN;
        m_cooldown_start = now_s; bracket_high = bracket_low = range = 0.0;
        if (trade_sink) trade_sink(tr);
    }
};

struct HistTickStreamer {
    std::ifstream f;
    std::string line;
    bool opened = false;
    bool open(const char* p) {
        f.open(p);
        if (!f.is_open()) { std::fprintf(stderr,"[err] cannot open %s\n",p); return false; }
        opened = true; return true;
    }
    static int64_t parse_hist_ts(const char* s, size_t len) {
        if (len < 18) return -1;
        int Y = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
        int M = (s[4]-'0')*10+(s[5]-'0');
        int D = (s[6]-'0')*10+(s[7]-'0');
        int hh = (s[9]-'0')*10+(s[10]-'0');
        int mm = (s[11]-'0')*10+(s[12]-'0');
        int ss = (s[13]-'0')*10+(s[14]-'0');
        int ms = (s[15]-'0')*100+(s[16]-'0')*10+(s[17]-'0');
        struct tm utc{}; utc.tm_year = Y-1900; utc.tm_mon = M-1; utc.tm_mday = D;
        utc.tm_hour = hh; utc.tm_min = mm; utc.tm_sec = ss;
        return (int64_t)timegm(&utc)*1000 + ms;
    }
    bool next(int64_t& ts_ms, double& bid, double& ask) {
        if (!opened) return false;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const char* p = line.c_str();
            const char* c1 = std::strchr(p, ',');
            if (!c1) continue;
            const char* c2 = std::strchr(c1+1, ',');
            if (!c2) continue;
            ts_ms = parse_hist_ts(p, (size_t)(c1-p));
            if (ts_ms < 0) continue;
            char buf[32]; size_t L1 = (size_t)(c2-(c1+1));
            if (L1 >= sizeof(buf)) continue;
            std::memcpy(buf,c1+1,L1); buf[L1]=0; bid = std::strtod(buf,nullptr);
            const char* c3 = std::strchr(c2+1,',');
            size_t L2 = c3 ? (size_t)(c3-(c2+1)) : std::strlen(c2+1);
            if (L2 >= sizeof(buf)) continue;
            std::memcpy(buf,c2+1,L2); buf[L2]=0; ask = std::strtod(buf,nullptr);
            return true;
        }
        return false;
    }
};

static inline int hour_utc_from_ts_ms(int64_t ts_ms) {
    time_t t = (time_t)(ts_ms/1000); struct tm utc{}; gmtime_r(&t,&utc); return utc.tm_hour;
}

} // namespace gbpfx

struct Cfg { double TP_RR, SL_FRAC, TRAIL_FRAC, MFE_TRAIL_FRAC; };
struct Result {
    Cfg cfg;
    // IS = full sample
    int n_trades=0, n_wins=0, tp=0, trail=0, sl=0, be=0;
    double gross=0, net=0, cost=0, win_sum=0, loss_sum=0;
    double equity=0, peak=0, dd=0;
    // OOS slice
    int oos_trades=0, oos_wins=0;
    double oos_net=0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <histdata_month.csv> ... "
            "[--oos-trailing N] [--max-spread V] [--leaderboard-out P] [--oos-out P]\n", argv[0]);
        return 2;
    }
    std::vector<std::string> csv_paths;
    int oos_trailing = 4;
    double max_spread = 0.00020;
    std::string lb_out  = "backtest/gbpusd_planB_leaderboard.csv";
    std::string oos_out = "backtest/gbpusd_planB_oos.csv";
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a,"--oos-trailing")==0 && i+1<argc) oos_trailing = std::atoi(argv[++i]);
        else if (std::strcmp(a,"--max-spread")==0 && i+1<argc) max_spread = std::atof(argv[++i]);
        else if (std::strcmp(a,"--leaderboard-out")==0 && i+1<argc) lb_out = argv[++i];
        else if (std::strcmp(a,"--oos-out")==0 && i+1<argc) oos_out = argv[++i];
        else if (a[0] != '-') csv_paths.push_back(a);
    }
    if (csv_paths.empty()) { std::fprintf(stderr,"[err] no CSV inputs\n"); return 2; }
    std::sort(csv_paths.begin(), csv_paths.end());

    // OOS cutoff = ts of the first tick in the (M - oos_trailing)-th file.
    // We compute this by scanning each file's first ts; sufficient.
    int64_t oos_cutoff_ms = 0;
    {
        const int M = (int)csv_paths.size();
        const int oos_start = std::max(0, M - oos_trailing);
        if (oos_start < M) {
            gbpfx::HistTickStreamer s;
            if (s.open(csv_paths[oos_start].c_str())) {
                int64_t ts; double b,a;
                if (s.next(ts,b,a)) oos_cutoff_ms = ts;
                s.f.close();
            }
        }
        std::printf("[planB] oos_trailing=%d cutoff_ms=%lld (file: %s)\n",
                    oos_trailing, (long long)oos_cutoff_ms,
                    oos_start < (int)csv_paths.size() ? csv_paths[oos_start].c_str() : "(none)");
    }

    // 4D grid
    std::vector<double> tp_grid    = {1.5, 2.0, 2.5, 3.0};
    std::vector<double> sl_grid    = {0.60, 0.70, 0.80, 0.90, 1.00};
    std::vector<double> tf_grid    = {0.20, 0.30, 0.45};
    std::vector<double> mt_grid    = {0.30, 0.40, 0.55};
    std::vector<Cfg> combos;
    for (double tp : tp_grid)
        for (double sl : sl_grid)
            for (double tf : tf_grid)
                for (double mt : mt_grid)
                    combos.push_back({tp, sl, tf, mt});
    const int N = (int)combos.size();
    std::printf("[planB] %d combos (TP %zu x SL %zu x TF %zu x MT %zu) MAX_SPREAD=%.5f\n",
                N, tp_grid.size(), sl_grid.size(), tf_grid.size(), mt_grid.size(), max_spread);
    std::printf("[planB] input files: %zu\n", csv_paths.size());

    std::vector<gbpfx::GbpusdLondonOpenEngine> engines(N);
    std::vector<Result> results(N);
    for (int i = 0; i < N; ++i) {
        engines[i].MAX_SPREAD     = max_spread;
        engines[i].TP_RR          = combos[i].TP_RR;
        engines[i].SL_FRAC        = combos[i].SL_FRAC;
        engines[i].TRAIL_FRAC     = combos[i].TRAIL_FRAC;
        engines[i].MFE_TRAIL_FRAC = combos[i].MFE_TRAIL_FRAC;
        engines[i].cost_ratio_min = 0.0;
        results[i].cfg = combos[i];
        Result* rp = &results[i];
        engines[i].trade_sink = [rp, oos_cutoff_ms](const gbpfx::TradeRecord& tr) {
            Result& r = *rp;
            r.n_trades++; r.gross += tr.gross_usd; r.net += tr.net_usd; r.cost += tr.modeled_cost_usd;
            if (tr.net_usd > 0) { r.n_wins++; r.win_sum += tr.net_usd; }
            else                {              r.loss_sum += tr.net_usd; }
            r.equity += tr.net_usd;
            if (r.equity > r.peak) r.peak = r.equity;
            const double dd = r.peak - r.equity;
            if (dd > r.dd) r.dd = dd;
            if      (tr.exit_reason == "TP_HIT")    r.tp++;
            else if (tr.exit_reason == "TRAIL_HIT") r.trail++;
            else if (tr.exit_reason == "SL_HIT")    r.sl++;
            else if (tr.exit_reason == "BE_HIT")    r.be++;
            // OOS slice (entry inside the held-out tail)
            if (oos_cutoff_ms > 0 && tr.ts_ms_entry >= oos_cutoff_ms) {
                r.oos_trades++; r.oos_net += tr.net_usd;
                if (tr.net_usd > 0) r.oos_wins++;
            }
        };
    }

    using Engine = gbpfx::GbpusdLondonOpenEngine;
    constexpr int WIN_CAP = Engine::STRUCTURE_LOOKBACK * 2;
    std::vector<double> ring(WIN_CAP, 0.0);
    int ring_size = 0;
    int64_t tick_idx = 0;
    std::deque<int64_t> dq_max, dq_min;

    gbpfx::HistTickStreamer streamer;
    int64_t ts_ms = 0; double bid = 0, ask = 0;
    int64_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : csv_paths) {
        std::printf("[planB] streaming %s\n", path.c_str());
        std::fflush(stdout);
        if (!streamer.open(path.c_str())) continue;
        int64_t fc = 0;
        while (streamer.next(ts_ms, bid, ask)) {
            ++fc; ++total;
            if (bid <= 0.0 || ask <= 0.0) continue;
            const double mid = 0.5 * (bid + ask);
            const int64_t expire = tick_idx - WIN_CAP + 1;
            while (!dq_max.empty() && dq_max.front() < expire) dq_max.pop_front();
            while (!dq_min.empty() && dq_min.front() < expire) dq_min.pop_front();
            while (!dq_max.empty() && ring[dq_max.back() % WIN_CAP] <= mid) dq_max.pop_back();
            while (!dq_min.empty() && ring[dq_min.back() % WIN_CAP] >= mid) dq_min.pop_back();
            ring[tick_idx % WIN_CAP] = mid;
            dq_max.push_back(tick_idx); dq_min.push_back(tick_idx);
            if (ring_size < WIN_CAP) ++ring_size;
            int win_count = ring_size;
            double w_hi = 0, w_lo = 0;
            if (win_count >= Engine::STRUCTURE_LOOKBACK) {
                w_hi = ring[dq_max.front() % WIN_CAP];
                w_lo = ring[dq_min.front() % WIN_CAP];
            }
            const int hour_utc = gbpfx::hour_utc_from_ts_ms(ts_ms);
            for (int i = 0; i < N; ++i)
                engines[i].on_tick(bid, ask, ts_ms, win_count, w_hi, w_lo, hour_utc);
            ++tick_idx;
            if (total % 5000000 == 0) {
                const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
                std::printf("  [planB] %lld ticks elapsed=%llds\n",
                            (long long)total, (long long)dt);
                std::fflush(stdout);
            }
        }
        streamer.f.close();
        std::printf("  [planB] %lld ticks in %s\n", (long long)fc, path.c_str());
        std::fflush(stdout);
    }
    const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[planB] processed %lld ticks across %d engines in %llds\n",
                (long long)total, N, (long long)dt);

    // Leaderboard sorted by IS net
    std::vector<int> order(N);
    for (int i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return results[a].net > results[b].net; });

    std::ofstream lf(lb_out);
    lf << "rank,combo_id,TP_RR,SL_FRAC,TRAIL_FRAC,MFE_TRAIL_FRAC,"
          "n_trades,n_wins,WR_pct,net_usd,gross_usd,cost_usd,mean_net,PF,max_dd,"
          "tp_count,trail_count,sl_count,be_count,"
          "oos_trades,oos_wins,oos_WR_pct,oos_net_usd\n";
    for (int rank = 0; rank < N; ++rank) {
        const int ci = order[rank];
        const Result& r = results[ci];
        const double wr = r.n_trades > 0 ? 100.0*r.n_wins/r.n_trades : 0.0;
        const double mn = r.n_trades > 0 ? r.net/r.n_trades : 0.0;
        const double pf = (std::fabs(r.loss_sum) > 1e-9) ? -r.win_sum/r.loss_sum : 0.0;
        const double oos_wr = r.oos_trades > 0 ? 100.0*r.oos_wins/r.oos_trades : 0.0;
        lf << (rank+1) << "," << ci << ","
           << std::fixed << std::setprecision(2) << r.cfg.TP_RR << ","
           << r.cfg.SL_FRAC << "," << r.cfg.TRAIL_FRAC << "," << r.cfg.MFE_TRAIL_FRAC << ","
           << r.n_trades << "," << r.n_wins << ","
           << wr << "," << r.net << "," << r.gross << "," << r.cost << ","
           << std::setprecision(4) << mn << "," << pf << ","
           << std::setprecision(2) << r.dd << ","
           << r.tp << "," << r.trail << "," << r.sl << "," << r.be << ","
           << r.oos_trades << "," << r.oos_wins << ","
           << oos_wr << "," << r.oos_net << "\n";
    }
    lf.close();

    // OOS-sorted leaderboard
    std::vector<int> oos_order(N);
    for (int i = 0; i < N; ++i) oos_order[i] = i;
    std::sort(oos_order.begin(), oos_order.end(),
              [&](int a, int b){ return results[a].oos_net > results[b].oos_net; });
    std::ofstream of(oos_out);
    of << "rank,combo_id,TP_RR,SL_FRAC,TRAIL_FRAC,MFE_TRAIL_FRAC,"
          "is_trades,is_net_usd,oos_trades,oos_wins,oos_WR_pct,oos_net_usd\n";
    for (int rank = 0; rank < N; ++rank) {
        const int ci = oos_order[rank];
        const Result& r = results[ci];
        const double oos_wr = r.oos_trades > 0 ? 100.0*r.oos_wins/r.oos_trades : 0.0;
        of << (rank+1) << "," << ci << ","
           << std::fixed << std::setprecision(2) << r.cfg.TP_RR << ","
           << r.cfg.SL_FRAC << "," << r.cfg.TRAIL_FRAC << "," << r.cfg.MFE_TRAIL_FRAC << ","
           << r.n_trades << "," << r.net << ","
           << r.oos_trades << "," << r.oos_wins << ","
           << oos_wr << "," << r.oos_net << "\n";
    }
    of.close();

    // Print top 10 IS
    std::printf("\n=== TOP 10 by IS net_usd ===\n");
    std::printf("%4s %5s %7s %7s %7s %7s %8s %12s %8s %8s %12s\n",
                "rank","id","TP_RR","SL_F","TRL_F","MFE_T","trades","net$","WR%","OOS_n","OOS_net$");
    for (int rank = 0; rank < std::min(10, N); ++rank) {
        const int ci = order[rank];
        const Result& r = results[ci];
        const double wr = r.n_trades > 0 ? 100.0*r.n_wins/r.n_trades : 0.0;
        std::printf("%4d %5d %7.2f %7.2f %7.2f %7.2f %8d %12.2f %8.2f %8d %12.2f\n",
                    rank+1, ci, r.cfg.TP_RR, r.cfg.SL_FRAC, r.cfg.TRAIL_FRAC, r.cfg.MFE_TRAIL_FRAC,
                    r.n_trades, r.net, wr, r.oos_trades, r.oos_net);
    }
    std::printf("[planB] wrote %s, %s\n", lb_out.c_str(), oos_out.c_str());
    return 0;
}
