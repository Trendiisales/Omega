// =============================================================================
// ustec_trend_follow_5m_planA_sweep.cpp -- Plan A: exit-side sweep
// =============================================================================
//
// 2026-05-12 (Claude / Jo): USTEC TrendFollow 5m parameter sweep harness.
//   Forks UstecTrendFollow5mEngine into a parameterizable per-instance class so
//   constexpr (sl_mult, tp_mult, MIN_ATR_PTS, MIN_SL_PTS_FLOOR,
//   PROVE_IT_SECS, PROVE_IT_MIN_FAVOURABLE_PTS) become instance fields and we
//   can build N engines and dispatch one tick stream to all of them.
//
//   Plan A: sweep cell sl_mult x tp_mult x PROVE_IT_SECS x PROVE_IT_MIN_PTS.
//   Plus baseline run.
//
// TICK INPUT
//   HistData NSXUSD ASCII tick CSV: YYYYMMDD HHMMSSnnn,bid,ask,vol
//   (underlying ticker -- engine internally stamps tr.symbol = "USTEC.F")
//
// COST MODEL (close-side, matches OmegaCostGuard "USTEC.F" branch)
//   commission_usd = 0.0  (indices have no per-lot commission)
//   spread_usd     = spread_at_entry_pts * 20 * lot
//   slippage_usd   = 2.00 * 20 * lot
//   net_usd        = gross_pts*lot*20 - (spread + slip)
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

namespace ustec {

// -----------------------------------------------------------------------------
// TradeRecord: emitted per-close. raw points * lot semantics; USD applied here.
// -----------------------------------------------------------------------------
struct TradeRecord {
    int64_t ts_ms_entry  = 0;
    int64_t ts_ms_exit   = 0;
    int     cell         = 0;   // 0=Donchian, 1=Keltner
    bool    is_long      = false;
    double  entry_price  = 0.0;
    double  exit_price   = 0.0;
    std::string exit_reason;
    int     duration_s   = 0;
    double  gross_pts    = 0.0;        // raw points*lot
    double  gross_usd    = 0.0;        // gross_pts * $20/pt
    double  spread_at_entry_pts = 0.0;
    double  modeled_cost_usd = 0.0;
    double  net_usd      = 0.0;
    int     hour_utc     = 0;
    double  size         = 0.10;
    double  mfe_pts      = 0.0;
    double  mae_pts      = 0.0;
    double  atr_at_entry = 0.0;
    int     bars_held    = 0;
};

struct UstecBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

enum class CellFamily { Donchian, Keltner };

// -----------------------------------------------------------------------------
// Engine fork. All instance-level params; defaults match
// include/UstecTrendFollow5mEngine.hpp.
// -----------------------------------------------------------------------------
class UstecTfEngine {
public:
    // --- Sweep-able (instance members; defaults = live constexpr) ---
    int     donchian_N            = 20;
    double  keltner_K             = 2.0;
    int     ema_period            = 20;
    int     atr_period            = 14;

    // Per-cell SL/TP multipliers on ATR. Live: both cells use 2.0/4.0.
    double  donchian_sl_mult      = 2.0;
    double  donchian_tp_mult      = 4.0;
    double  keltner_sl_mult       = 2.0;
    double  keltner_tp_mult       = 4.0;

    double  prove_it_secs         = 90.0;
    double  prove_it_min_fav_pts  = 4.0;
    double  min_sl_pts_floor      = 15.0;
    double  min_atr_pts           = 10.0;
    double  max_spread            = 5.0;
    double  cost_ratio_min        = 1.5;
    double  lot                   = 0.1;

    // Session filter (UTC hours). Default = all hours.
    int     session_start_utc     = 0;
    int     session_end_utc       = 24;

    // Mutual exclusion + cell enables
    bool    enable_donchian       = true;
    bool    enable_keltner        = true;
    bool    enable_mutex          = true;

    std::function<void(const TradeRecord&)> trade_sink = nullptr;

    struct Pos {
        bool        active             = false;
        bool        is_long            = false;
        double      entry_px           = 0.0;
        double      tp_px              = 0.0;
        double      sl_px              = 0.0;
        double      atr_at_entry       = 0.0;
        int64_t     entry_ts_ms        = 0;
        int         bars_held          = 0;
        int         cooldown_bars      = 0;
        double      mfe_pts            = 0.0;
        double      mae_pts            = 0.0;
        bool        proved             = false;
        double      spread_at_entry    = 0.0;
        int         hour_utc_at_entry  = 0;
    };
    std::array<Pos, 2> pos{};   // [0]=Donchian, [1]=Keltner

    // State (replicates UstecTrendFollow5mEngine internals)
    static constexpr int kBarHistoryMax = 64;
    std::deque<UstecBar> bars_;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    void reset() {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        for (auto& p : pos) p = {};
    }

    // Called per 5m-bar-close. Shared bars: each engine still independently
    // computes its own ATR/EMA so different param values are honored.
    void on_5m_bar(const UstecBar& bar, double bid, double ask, int64_t now_ms,
                   int hour_utc) noexcept {
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistoryMax) bars_.pop_front();

        _update_local_atr();
        _update_ema();

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        // need enough history; engine source requires >= 22 bars
        if ((int)bars_.size() < std::max(donchian_N, ema_period) + 2) return;
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        // Session gate
        if (hour_utc < session_start_utc || hour_utc >= session_end_utc) return;

        // Min ATR floor
        if (atr14_ < min_atr_pts) return;

        // Try each cell
        for (int ci = 0; ci < 2; ++ci) {
            if (ci == 0 && !enable_donchian) continue;
            if (ci == 1 && !enable_keltner)  continue;
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;

            int side = (ci == 0) ? _sig_donchian() : _sig_keltner();
            if (side == 0) continue;

            // Mutex: same-direction block
            if (enable_mutex && _other_cell_open_same_direction(ci, side)) continue;

            _fire_entry(ci, side, bid, ask, now_ms, hour_utc);
        }
    }

    // Per-tick management of open positions only.
    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        for (int ci = 0; ci < 2; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms);
        }
    }

    bool any_open() const noexcept {
        return pos[0].active || pos[1].active;
    }

private:
    void _update_local_atr() noexcept {
        if ((int)bars_.size() < 2) { atr14_ = 0.0; return; }
        const auto& cur  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];
        const double tr = std::max(cur.high - cur.low,
                             std::max(std::abs(cur.high - prev.close),
                                      std::abs(cur.low  - prev.close)));
        if (atr_warmup_count_ < atr_period) {
            atr14_ = (atr14_ * atr_warmup_count_ + tr) / (atr_warmup_count_ + 1);
            ++atr_warmup_count_;
        } else {
            atr14_ = (atr14_ * (atr_period - 1) + tr) / atr_period;
        }
    }

    void _update_ema() noexcept {
        if (bars_.empty()) return;
        double c = bars_.back().close;
        if (!ema_initialised_) { ema20_ = c; ema_initialised_ = true; }
        else {
            const double a = 2.0 / (ema_period + 1);
            ema20_ = a * c + (1.0 - a) * ema20_;
        }
    }

    bool _other_cell_open_same_direction(int ci, int side) const noexcept {
        const bool want_long = (side > 0);
        for (int k = 0; k < 2; ++k) {
            if (k == ci) continue;
            if (!pos[k].active) continue;
            if (pos[k].is_long == want_long) return true;
        }
        return false;
    }

    int _sig_donchian() const noexcept {
        const int N = donchian_N;
        if ((int)bars_.size() < N + 1) return 0;
        const int last = (int)bars_.size() - 1;
        double hi = bars_[last - N].high, lo = bars_[last - N].low;
        for (int k = last - N + 1; k <= last - 1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }

    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if ((int)bars_.size() < ema_period + 2) return 0;
        const auto& cur = bars_.back();
        const double up = ema20_ + keltner_K * atr14_;
        const double lo = ema20_ - keltner_K * atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }

    // Cost gate replicates ExecutionCostGuard::is_viable for USTEC.F.
    bool _cost_viable(double spread_pts, double tp_dist_pts) const noexcept {
        if (cost_ratio_min <= 0.0) return true;
        const double tick_usd_per_lot = 20.0;
        const double slippage_pts     = 2.0;
        const double cost = spread_pts * tick_usd_per_lot * lot
                          + slippage_pts * tick_usd_per_lot * lot;
        const double gross = tp_dist_pts * tick_usd_per_lot * lot;
        return gross >= cost * cost_ratio_min;
    }

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms,
                     int hour_utc) noexcept {
        const double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        const double sl_mult = (ci == 0) ? donchian_sl_mult : keltner_sl_mult;
        const double tp_mult = (ci == 0) ? donchian_tp_mult : keltner_tp_mult;
        double sl_dist_atr = sl_mult * atr14_;
        double sl_dist     = std::max(sl_dist_atr, min_sl_pts_floor);
        double tp_dist     = tp_mult * atr14_;
        if (sl_dist > sl_dist_atr && sl_dist_atr > 0.0) {
            const double ratio = sl_dist / sl_dist_atr;
            tp_dist = tp_dist * ratio;
        }

        const double spread = ask - bid;
        if (!_cost_viable(spread, tp_dist)) return;

        auto& p = pos[ci];
        p.active = true;
        p.is_long = (side > 0);
        p.entry_px = entry;
        p.sl_px = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry = atr14_;
        p.entry_ts_ms = now_ms;
        p.bars_held = 0;
        p.cooldown_bars = 0;
        p.mfe_pts = 0.0;
        p.mae_pts = 0.0;
        p.proved = false;
        p.spread_at_entry = spread;
        p.hour_utc_at_entry = hour_utc;
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms) noexcept {
        auto& p = pos[ci];
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && p.entry_px > 0.0) {
            const double fav = p.is_long ? (mid - p.entry_px) : (p.entry_px - mid);
            if (fav > p.mfe_pts) p.mfe_pts = fav;
            if (fav < p.mae_pts) p.mae_pts = fav;
            if (!p.proved && p.mfe_pts >= prove_it_min_fav_pts) p.proved = true;
        }

        const double elapsed_s = (p.entry_ts_ms > 0)
            ? (now_ms - p.entry_ts_ms) / 1000.0 : 0.0;
        if (!p.proved && elapsed_s >= prove_it_secs) {
            const double xp = p.is_long ? bid : ask;
            _close(ci, xp, "PROVE_IT_FAIL", now_ms);
            return;
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0;
        if (p.is_long) {
            if (bid <= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;
        _close(ci, xp, hit_tp ? "TP_HIT" : "SL_HIT", now_ms);
    }

    void _close(int ci, double exit_px, const char* reason, int64_t now_ms) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;
        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);
        const double gross_pts_lot = pts_move * lot;
        const double tick_usd_per_lot = 20.0;
        const double gross_usd = pts_move * tick_usd_per_lot * lot;
        const double slip_cost  = 2.0 * tick_usd_per_lot * lot;
        const double spread_cost = p.spread_at_entry * tick_usd_per_lot * lot;
        const double modeled_cost = slip_cost + spread_cost;
        const double net_usd = gross_usd - modeled_cost;

        TradeRecord tr;
        tr.ts_ms_entry = p.entry_ts_ms;
        tr.ts_ms_exit  = now_ms;
        tr.cell        = ci;
        tr.is_long     = p.is_long;
        tr.entry_price = p.entry_px;
        tr.exit_price  = exit_px;
        tr.exit_reason = reason;
        tr.duration_s  = (int)((now_ms - p.entry_ts_ms) / 1000);
        tr.gross_pts   = gross_pts_lot;
        tr.gross_usd   = gross_usd;
        tr.spread_at_entry_pts = p.spread_at_entry;
        tr.modeled_cost_usd = modeled_cost;
        tr.net_usd     = net_usd;
        tr.hour_utc    = p.hour_utc_at_entry;
        tr.size        = lot;
        tr.mfe_pts     = p.mfe_pts;
        tr.mae_pts     = p.mae_pts;
        tr.atr_at_entry = p.atr_at_entry;
        tr.bars_held    = p.bars_held;
        if (trade_sink) trade_sink(tr);

        p.active = false;
        p.cooldown_bars = 1;
    }
};

// -----------------------------------------------------------------------------
// HistData ASCII tick CSV streamer.
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

    static int64_t parse_hist_ts(const char* s, size_t len) {
        if (len < 18) return -1;
        int Y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
        int M = (s[4]-'0')*10 + (s[5]-'0');
        int D = (s[6]-'0')*10 + (s[7]-'0');
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
            const char* p = line.c_str();
            const char* c1 = std::strchr(p, ',');
            if (!c1) continue;
            const char* c2 = std::strchr(c1 + 1, ',');
            if (!c2) continue;
            ts_ms = parse_hist_ts(p, (size_t)(c1 - p));
            if (ts_ms < 0) continue;
            char buf[32];
            size_t L1 = (size_t)(c2 - (c1 + 1));
            if (L1 >= sizeof(buf)) continue;
            std::memcpy(buf, c1 + 1, L1); buf[L1] = 0;
            bid = std::strtod(buf, nullptr);
            const char* c3 = std::strchr(c2 + 1, ',');
            size_t L2 = c3 ? (size_t)(c3 - (c2 + 1)) : std::strlen(c2 + 1);
            if (L2 >= sizeof(buf)) continue;
            std::memcpy(buf, c2 + 1, L2); buf[L2] = 0;
            ask = std::strtod(buf, nullptr);
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

// 5m bar builder. Aligns to 5m boundary in UTC.
struct BarBuilder {
    int64_t cur_bar_start_ms = 0;
    UstecBar cur;
    bool     has_open = false;

    // Returns true and sets `out` if a bar just closed (this tick belongs to next bar).
    bool on_tick(int64_t ts_ms, double bid, double ask, UstecBar& out) {
        const double mid = (bid + ask) * 0.5;
        const int64_t bar_ms = 5 * 60 * 1000;
        const int64_t bar_start = (ts_ms / bar_ms) * bar_ms;
        if (!has_open) {
            cur_bar_start_ms = bar_start;
            cur.bar_start_ms = bar_start;
            cur.open = cur.high = cur.low = cur.close = mid;
            has_open = true;
            return false;
        }
        if (bar_start != cur_bar_start_ms) {
            // bar closed
            out = cur;
            cur_bar_start_ms = bar_start;
            cur.bar_start_ms = bar_start;
            cur.open = cur.high = cur.low = cur.close = mid;
            return true;
        }
        if (mid > cur.high) cur.high = mid;
        if (mid < cur.low)  cur.low  = mid;
        cur.close = mid;
        return false;
    }
};

} // namespace ustec

// -----------------------------------------------------------------------------
// Aggregate stats per engine instance
// -----------------------------------------------------------------------------
struct ComboConfig {
    double donchian_sl_mult;
    double donchian_tp_mult;
    double keltner_sl_mult;
    double keltner_tp_mult;
    double prove_it_secs;
    double prove_it_min_fav_pts;
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
    int    sl_count = 0;
    int    prove_it_count = 0;
    int    donchian_count = 0;
    int    keltner_count = 0;
    double equity = 0.0;
    double peak_eq = 0.0;
    double max_dd = 0.0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <histdata_month.csv> [more ...] "
            "[--baseline-out PATH] [--stats-out PATH] [--leaderboard-out PATH]\n", argv[0]);
        return 2;
    }
    std::vector<std::string> csv_paths;
    std::string baseline_out = "outputs/ustec_trend_follow_5m_planA_baseline.csv";
    std::string stats_out    = "outputs/ustec_trend_follow_5m_planA_stats.csv";
    std::string lb_out       = "outputs/ustec_trend_follow_5m_planA_leaderboard.csv";
    std::string monthly_out  = "outputs/ustec_trend_follow_5m_planA_baseline_monthly.csv";
    std::string engine_summary_out;  // optional per-engine summary for split-merge
    bool write_baseline_ledger = true;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a, "--baseline-out")    == 0 && i+1 < argc) baseline_out = argv[++i];
        else if (std::strcmp(a, "--stats-out")       == 0 && i+1 < argc) stats_out    = argv[++i];
        else if (std::strcmp(a, "--leaderboard-out") == 0 && i+1 < argc) lb_out       = argv[++i];
        else if (std::strcmp(a, "--monthly-out")     == 0 && i+1 < argc) monthly_out  = argv[++i];
        else if (std::strcmp(a, "--engine-summary-out") == 0 && i+1 < argc) engine_summary_out = argv[++i];
        else if (std::strcmp(a, "--no-baseline-ledger") == 0) write_baseline_ledger = false;
        else if (a[0] != '-') csv_paths.push_back(a);
    }
    if (csv_paths.empty()) { std::fprintf(stderr, "[err] no CSVs\n"); return 2; }
    std::sort(csv_paths.begin(), csv_paths.end());

    // ---- Build combo grid -------------------------------------------------
    // Baseline: live config (sl=2.0, tp=4.0, prove_it=90s/4pts)
    // Sweep dimensions (kept under ~120 cells):
    //   sl_mult:    {1.5, 2.0, 2.5, 3.0}        (4)
    //   tp_mult:    {2.5, 4.0, 5.5, 7.0}        (4)  -- RR = tp/sl from 1.0 to 2.8
    //   prove_secs: {60, 90, 150}               (3)
    //   prove_pts:  {2.0, 4.0, 7.0}             (3)
    //   -> 4 * 4 * 3 * 3 = 144 cells (both cells get same mults for simplicity)
    std::vector<double> sl_grid    = {1.5, 2.0, 2.5, 3.0};
    std::vector<double> tp_grid    = {2.5, 4.0, 5.5, 7.0};
    std::vector<double> psec_grid  = {60.0, 150.0};
    std::vector<double> ppts_grid  = {2.0, 7.0};

    std::vector<ComboConfig> combos;
    combos.push_back({2.0, 4.0, 2.0, 4.0, 90.0, 4.0, 1});  // baseline
    for (double sl : sl_grid)
      for (double tp : tp_grid)
        for (double ps : psec_grid)
          for (double pp : ppts_grid)
            combos.push_back({sl, tp, sl, tp, ps, pp, 0});

    const int N = (int)combos.size();
    std::printf("[ustec-planA] %d combos (1 baseline + %d sweep)\n", N, N-1);
    std::printf("[ustec-planA] sl x tp x psec x ppts = %zu x %zu x %zu x %zu = %zu\n",
                sl_grid.size(), tp_grid.size(), psec_grid.size(), ppts_grid.size(),
                sl_grid.size()*tp_grid.size()*psec_grid.size()*ppts_grid.size());
    std::printf("[ustec-planA] input files: %zu\n", csv_paths.size());

    // ---- Build engines + results ------------------------------------------
    std::vector<ustec::UstecTfEngine> engines(N);
    std::vector<ComboResult> results(N);
    std::ofstream bf;
    if (write_baseline_ledger) {
        bf.open(baseline_out);
        bf << "ts_ms_entry,ts_ms_exit,cell,side,entry,exit,exit_reason,duration_s,"
              "gross_pts_lot,gross_usd,spread_pts,modeled_cost_usd,net_usd,hour_utc,"
              "size,mfe_pts,mae_pts,atr_at_entry,bars_held\n";
    }

    for (int i = 0; i < N; ++i) {
        engines[i].donchian_sl_mult     = combos[i].donchian_sl_mult;
        engines[i].donchian_tp_mult     = combos[i].donchian_tp_mult;
        engines[i].keltner_sl_mult      = combos[i].keltner_sl_mult;
        engines[i].keltner_tp_mult      = combos[i].keltner_tp_mult;
        engines[i].prove_it_secs        = combos[i].prove_it_secs;
        engines[i].prove_it_min_fav_pts = combos[i].prove_it_min_fav_pts;
        // Cost gate replicates live engine: 1.5x ratio. (Live engine has this on.)
        engines[i].cost_ratio_min = 1.5;

        results[i].cfg = combos[i];
        ComboResult* rp = &results[i];
        const bool is_baseline = (combos[i].tag == 1);
        std::ofstream* bfp = (is_baseline && write_baseline_ledger) ? &bf : nullptr;

        engines[i].trade_sink = [rp, bfp](const ustec::TradeRecord& tr) {
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
            if      (tr.exit_reason == "TP_HIT")        r.tp_count       += 1;
            else if (tr.exit_reason == "SL_HIT")        r.sl_count       += 1;
            else if (tr.exit_reason == "PROVE_IT_FAIL") r.prove_it_count += 1;
            if (tr.cell == 0) r.donchian_count += 1;
            else              r.keltner_count  += 1;

            if (bfp) {
                (*bfp) << tr.ts_ms_entry << "," << tr.ts_ms_exit << ","
                       << tr.cell << ","
                       << (tr.is_long ? "L" : "S") << ","
                       << std::fixed << std::setprecision(3) << tr.entry_price << ","
                       << tr.exit_price << ","
                       << tr.exit_reason << "," << tr.duration_s << ","
                       << std::setprecision(4) << tr.gross_pts << ","
                       << std::setprecision(2) << tr.gross_usd << ","
                       << std::setprecision(4) << tr.spread_at_entry_pts << ","
                       << std::setprecision(2) << tr.modeled_cost_usd << ","
                       << std::setprecision(2) << tr.net_usd << ","
                       << tr.hour_utc << "," << std::setprecision(3) << tr.size << ","
                       << std::setprecision(2) << tr.mfe_pts << ","
                       << tr.mae_pts << ","
                       << std::setprecision(2) << tr.atr_at_entry << ","
                       << tr.bars_held << "\n";
            }
        };
    }

    // ---- Stream + dispatch -------------------------------------------------
    ustec::BarBuilder builder;
    ustec::HistTickStreamer streamer;
    int64_t ts_ms = 0; double bid = 0.0, ask = 0.0;
    int64_t total_ticks = 0;

    // Per-month baseline aggregation for the monthly table.
    struct MonthAgg { int trades=0; double net=0.0; double gross=0.0; };
    std::vector<std::pair<std::string, MonthAgg>> monthly;
    std::string cur_month_label;
    MonthAgg cur_month_agg;

    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : csv_paths) {
        // extract month label from path (e.g., 202401)
        std::string month_label;
        {
            const std::string& p = path;
            auto pos = p.find("NSXUSD_T");
            if (pos != std::string::npos) month_label = p.substr(pos+8, 6);
        }
        if (!cur_month_label.empty() && month_label != cur_month_label) {
            monthly.push_back({cur_month_label, cur_month_agg});
            cur_month_agg = {};
        }
        cur_month_label = month_label;
        // capture baseline trades during this month by hooking the baseline sink
        // (done lazily via results[0])
        const int baseline_trades_before = results[0].n_trades;
        const double baseline_net_before = results[0].net_usd;
        const double baseline_gross_before = results[0].gross_usd;

        std::printf("[ustec-planA] streaming %s\n", path.c_str());
        std::fflush(stdout);
        if (!streamer.open(path.c_str())) continue;
        int64_t file_ticks = 0;
        while (streamer.next(ts_ms, bid, ask)) {
            ++file_ticks; ++total_ticks;
            if (bid <= 0.0 || ask <= 0.0) continue;

            // Per-tick: manage open positions on each engine.
            for (int i = 0; i < N; ++i) {
                if (engines[i].any_open()) engines[i].on_tick(bid, ask, ts_ms);
            }

            // 5m bar builder
            ustec::UstecBar closed_bar;
            const bool bar_closed = builder.on_tick(ts_ms, bid, ask, closed_bar);
            if (bar_closed) {
                const int hour_utc = ustec::hour_utc_from_ts_ms(closed_bar.bar_start_ms);
                for (int i = 0; i < N; ++i) {
                    engines[i].on_5m_bar(closed_bar, bid, ask, ts_ms, hour_utc);
                }
            }
        }
        streamer.f.close();
        cur_month_agg.trades += (results[0].n_trades - baseline_trades_before);
        cur_month_agg.net    += (results[0].net_usd  - baseline_net_before);
        cur_month_agg.gross  += (results[0].gross_usd - baseline_gross_before);
        const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("  [planA] %lld ticks in %s elapsed=%llds\n",
                    (long long)file_ticks, path.c_str(), (long long)dt);
        std::fflush(stdout);
    }
    if (!cur_month_label.empty()) monthly.push_back({cur_month_label, cur_month_agg});
    bf.close();

    const auto dt_total = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[ustec-planA] processed %lld ticks across %d engines in %llds\n",
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
        sf << "cost_usd," << r.cost_usd << "\n";
        sf << "net_usd,"  << r.net_usd  << "\n";
        sf << "mean_net_per_trade,"
           << (r.n_trades > 0 ? r.net_usd / r.n_trades : 0.0) << "\n";
        const double pf = (std::fabs(r.sum_loss_net) > 1e-9)
            ? -r.sum_win_net / r.sum_loss_net : 0.0;
        sf << "PF," << std::setprecision(4) << pf << "\n";
        sf << "max_dd_usd," << std::setprecision(2) << r.max_dd << "\n";
        sf << "tp_count," << r.tp_count << "\n";
        sf << "sl_count," << r.sl_count << "\n";
        sf << "prove_it_count," << r.prove_it_count << "\n";
        sf << "donchian_count," << r.donchian_count << "\n";
        sf << "keltner_count," << r.keltner_count << "\n";
        sf.close();
    }

    // ---- Write monthly CSV ------------------------------------------------
    {
        std::ofstream mf(monthly_out);
        mf << "month,trades,gross_usd,net_usd\n";
        for (auto& kv : monthly) {
            mf << kv.first << "," << kv.second.trades << ","
               << std::fixed << std::setprecision(2) << kv.second.gross << ","
               << kv.second.net << "\n";
        }
        mf.close();
    }

    // ---- Optional engine-summary CSV (one row per engine, additive) ----
    if (!engine_summary_out.empty()) {
        std::ofstream es(engine_summary_out);
        es << "combo_id,tag,sl_mult,tp_mult,prove_secs,prove_pts,"
              "n_trades,n_wins,gross_usd,net_usd,cost_usd,sum_win_net,sum_loss_net,"
              "tp_count,sl_count,prove_it_count,donchian_count,keltner_count\n";
        for (int i = 0; i < N; ++i) {
            const ComboResult& r = results[i];
            es << i << "," << r.cfg.tag << ","
               << std::fixed << std::setprecision(2) << r.cfg.donchian_sl_mult << ","
               << r.cfg.donchian_tp_mult << ","
               << std::setprecision(1) << r.cfg.prove_it_secs << ","
               << std::setprecision(2) << r.cfg.prove_it_min_fav_pts << ","
               << r.n_trades << "," << r.n_wins << ","
               << std::setprecision(4) << r.gross_usd << ","
               << r.net_usd << "," << r.cost_usd << ","
               << r.sum_win_net << "," << r.sum_loss_net << ","
               << r.tp_count << "," << r.sl_count << "," << r.prove_it_count << ","
               << r.donchian_count << "," << r.keltner_count << "\n";
        }
        es.close();
    }

    // ---- Write leaderboard CSV --------------------------------------------
    std::vector<int> order(N);
    for (int i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return results[a].net_usd > results[b].net_usd; });

    std::ofstream lf(lb_out);
    lf << "rank,combo_id,tag,sl_mult,tp_mult,prove_secs,prove_pts,"
          "n_trades,n_wins,WR_pct,net_usd,gross_usd,cost_usd,mean_net,PF,max_dd_usd,"
          "tp_count,sl_count,prove_it_count,donchian_count,keltner_count\n";
    for (int rank = 0; rank < N; ++rank) {
        const int ci = order[rank];
        const ComboResult& r = results[ci];
        const double wr = r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0;
        const double mn = r.n_trades > 0 ? r.net_usd / r.n_trades : 0.0;
        const double pf = (std::fabs(r.sum_loss_net) > 1e-9)
            ? -r.sum_win_net / r.sum_loss_net : 0.0;
        lf << (rank+1) << "," << ci << "," << r.cfg.tag << ","
           << std::fixed << std::setprecision(2) << r.cfg.donchian_sl_mult << ","
           << r.cfg.donchian_tp_mult << ","
           << std::setprecision(1) << r.cfg.prove_it_secs << ","
           << std::setprecision(2) << r.cfg.prove_it_min_fav_pts << ","
           << r.n_trades << "," << r.n_wins << ","
           << wr << ","
           << r.net_usd << "," << r.gross_usd << "," << r.cost_usd << ","
           << std::setprecision(4) << mn << "," << pf << ","
           << std::setprecision(2) << r.max_dd << ","
           << r.tp_count << "," << r.sl_count << "," << r.prove_it_count << ","
           << r.donchian_count << "," << r.keltner_count << "\n";
    }
    lf.close();

    // ---- Print baseline + top 10 -----------------------------------------
    {
        const ComboResult& b = results[0];
        std::printf("\n=== BASELINE (live constexpr) ===\n");
        std::printf("  trades=%d wins=%d (WR=%.2f%%) net=$%.2f gross=$%.2f cost=$%.2f\n",
                    b.n_trades, b.n_wins,
                    b.n_trades > 0 ? 100.0 * b.n_wins / b.n_trades : 0.0,
                    b.net_usd, b.gross_usd, b.cost_usd);
        std::printf("  TP=%d SL=%d PROVE_IT_FAIL=%d Donchian=%d Keltner=%d max_dd=$%.2f\n",
                    b.tp_count, b.sl_count, b.prove_it_count,
                    b.donchian_count, b.keltner_count, b.max_dd);
    }
    std::printf("\n=== TOP 10 by net_usd ===\n");
    std::printf("%4s %5s %4s %6s %6s %6s %6s %8s %10s %10s %7s\n",
                "rank","id","tag","SL_M","TP_M","PSEC","PPTS","trades","net$","gross$","WR%");
    for (int rank = 0; rank < std::min(10, N); ++rank) {
        const int ci = order[rank];
        const ComboResult& r = results[ci];
        const double wr = r.n_trades > 0 ? 100.0 * r.n_wins / r.n_trades : 0.0;
        std::printf("%4d %5d %4d %6.2f %6.2f %6.1f %6.2f %8d %10.2f %10.2f %7.2f\n",
                    rank+1, ci, r.cfg.tag,
                    r.cfg.donchian_sl_mult, r.cfg.donchian_tp_mult,
                    r.cfg.prove_it_secs, r.cfg.prove_it_min_fav_pts,
                    r.n_trades, r.net_usd, r.gross_usd, wr);
    }
    std::printf("\n[ustec-planA] wrote %s, %s, %s, %s\n",
                baseline_out.c_str(), stats_out.c_str(), lb_out.c_str(),
                monthly_out.c_str());
    return 0;
}
