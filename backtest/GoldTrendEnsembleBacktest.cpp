// =============================================================================
// GoldTrendEnsembleBacktest.cpp -- C++ harness for S114 long-only trend ensemble
// =============================================================================
//
// Validates the Python research result (S114, scripts/gold_trend_ensemble_bt.py)
// in C++ before live-engine port (planned S116). Reads the same Duka CSV.
//
// CELLS (long-only, equal-vol weighted):
//   A_H1_EMA20_80  : H1 EMA(20,80) cross + slow rising filter, ATR(14)x4 stop
//   B_H4_EMA8_21   : H4 EMA(8,21)  cross + slow rising filter, ATR(14)x2.5 stop
//   C_H1_Donchian40: H1 Donchian-40 long breakout / exit on N=40 low, ATRx5 stop
//
// No take-profit. Vol target 10% annualised per cell (sizing from rolling
// log-return stdev on the cell's own timeframe, scaled to 1.0).
//
// DATA FORMAT: Dukascopy combined CSV: timestamp,askPrice,bidPrice
//   (header: "timestamp,askPrice,bidPrice")
//
// PYTHON REFERENCE (full 2024-03 -> 2026-04 walk-forward, no train fit):
//   ensemble  : +$28,145 (+28.15%)  CAGR +12.55%  Sharpe +3.21  MDD -2.29%
//   A_H1_E20_80: +$25,478 (+25.48%)  Sharpe +1.97 MDD -2.48%  181 trades
//   B_H4_E8_21 : +$30,966 (+30.97%)  Sharpe +1.96 MDD -2.12%  103 trades
//   C_H1_Don40 : +$29,634 (+29.63%)  Sharpe +2.11 MDD -2.33%   75 trades
//   Buy & hold: +$130,280 (+130.3%)  MDD -24.42%
//
// COMPILATION:
//   g++ -std=c++17 -O2 -o backtest/gold_trend_ensemble_bt \
//       backtest/GoldTrendEnsembleBacktest.cpp
//   ./backtest/gold_trend_ensemble_bt \
//       ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
//
// OUTPUT:
//   - stdout: per-cell + ensemble summary
//   - bt_gold_trend_ensemble_report.csv : per-cell stats
//   - bt_gold_trend_ensemble_trades.csv : every trade with entry/exit/pnl
//   - bt_gold_trend_ensemble_equity.csv : per-bar equity (H1 timeline)
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <deque>
#include <ctime>

namespace cfg {
    constexpr double STARTING_EQUITY      = 100000.0;
    constexpr double VOL_TARGET_ANN       = 0.10;
    // IBKR gold round-trip cost in price points (commission + slippage +
    // spread, GC-futures basis ~0.37). Deducted per closed trade x units.
    constexpr double COST_RT_PTS          = 0.37;
    constexpr int    ATR_PERIOD           = 14;
    constexpr double ATR_INIT             = 5.0;
    constexpr double ATR_MIN              = 0.5;
    constexpr double MAX_FRAC_NOTIONAL    = 0.50;  // cap units to 50% notional
    constexpr int    VOL_LOOKBACK_BARS    = 60;
    constexpr int    SLOW_RISE_LOOKBACK   = 3;     // slow EMA must rise vs N bars ago
    constexpr double BPY_H1               = 252.0 * 24.0;
    constexpr double BPY_H4               = 252.0 * 6.0;

    inline bool is_weekend(int64_t ts_ms) noexcept {
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;
        if (dow == 0 || dow == 6) return true;
        if (dow == 5 && (int)((sec % 86400) / 3600) >= 21) return true;
        return false;
    }
}

// ---------------- Bar -----------------------------------------------------
struct Bar {
    int64_t open_ms = 0;
    double  open = 0, high = 0, low = 0, close = 0;
    int     tick_count = 0;
    void reset(int64_t ts, double p) { open_ms=ts; open=high=low=close=p; tick_count=1; }
    void update(double p) {
        if (p > high) high = p;
        if (p < low)  low  = p;
        close = p; tick_count++;
    }
};

// ---------------- Bar builder + indicators --------------------------------
struct BarBuilder {
    int64_t period_ms;
    Bar     current{};
    std::deque<Bar> bars;
    bool    active = false;
    int     total_bars = 0;
    double  atr = cfg::ATR_INIT;
    double  ema_fast = 0, ema_slow = 0;  // configured by user
    double  ema_fast_b = 0, ema_slow_b = 0;  // second EMA pair (for Donchian cell's separate trend gate, unused here)
    double  don_high = 0, don_low = 0;
    int     don_period = 40;
    int     fast_period = 20, slow_period = 80;
    bool    ema_init = false;
    std::deque<double> tr_hist;
    std::deque<double> ema_slow_hist;       // for slow-rising check
    std::deque<double> log_ret_hist;        // for vol-target sizing

    BarBuilder(int64_t period, int fast_p, int slow_p, int donch_n)
        : period_ms(period), don_period(donch_n),
          fast_period(fast_p), slow_period(slow_p) {}

    // returns true when a new bar just closed (caller should consult bars.back())
    bool on_tick(double mid, int64_t ts_ms) {
        if (!active) { current.reset(ts_ms, mid); active = true; return false; }
        int64_t boundary = (current.open_ms / period_ms) * period_ms + period_ms;
        if (ts_ms >= boundary) {
            bars.push_back(current); total_bars++;
            _on_close();
            current.reset(ts_ms, mid);
            // keep enough history for indicators + sizing
            while ((int)bars.size() > 260) bars.pop_front();
            return true;
        }
        current.update(mid);
        return false;
    }

private:
    void _on_close() {
        if (bars.size() < 2) {
            const Bar& b = bars.back();
            ema_fast = ema_slow = b.close;
            ema_init = true;
            return;
        }
        const Bar& b    = bars.back();
        const Bar& prev = bars[bars.size() - 2];
        double tr = std::max({b.high - b.low,
                              std::fabs(b.high - prev.close),
                              std::fabs(b.low  - prev.close)});
        tr_hist.push_back(tr);
        if ((int)tr_hist.size() > cfg::ATR_PERIOD) tr_hist.pop_front();
        if ((int)tr_hist.size() >= cfg::ATR_PERIOD) atr = atr * 13.0 / 14.0 + tr / 14.0;
        else { double s = 0; for (double v : tr_hist) s += v; atr = s / tr_hist.size(); }
        atr = std::max(cfg::ATR_MIN, atr);

        double kf = 2.0 / (fast_period + 1);
        double ks = 2.0 / (slow_period + 1);
        if (!ema_init) { ema_fast = ema_slow = b.close; ema_init = true; }
        else {
            ema_fast = b.close * kf + ema_fast * (1.0 - kf);
            ema_slow = b.close * ks + ema_slow * (1.0 - ks);
        }
        ema_slow_hist.push_back(ema_slow);
        while ((int)ema_slow_hist.size() > 16) ema_slow_hist.pop_front();

        // Donchian-N (computed from prior N bars, excluding current)
        if ((int)bars.size() >= don_period + 1) {
            don_high = 0; don_low = 1e18;
            int start = (int)bars.size() - don_period - 1;
            int end   = (int)bars.size() - 1;
            for (int i = start; i < end; ++i) {
                if (bars[i].high > don_high) don_high = bars[i].high;
                if (bars[i].low  < don_low)  don_low  = bars[i].low;
            }
        }

        // log return for vol-target sizing
        if (prev.close > 0 && b.close > 0) {
            double lr = std::log(b.close / prev.close);
            log_ret_hist.push_back(lr);
            while ((int)log_ret_hist.size() > cfg::VOL_LOOKBACK_BARS) log_ret_hist.pop_front();
        }
    }
public:
    bool slow_rising(int lookback = cfg::SLOW_RISE_LOOKBACK) const {
        if ((int)ema_slow_hist.size() <= lookback) return false;
        return ema_slow_hist.back() > ema_slow_hist[ema_slow_hist.size() - 1 - lookback];
    }
    bool fast_above_slow() const { return ema_init && (ema_fast > ema_slow); }

    // rolling stdev of log returns (sample stdev)
    double log_ret_stdev() const {
        int n = (int)log_ret_hist.size();
        if (n < 10) return 0.0;
        double m = 0; for (double v : log_ret_hist) m += v; m /= n;
        double s = 0; for (double v : log_ret_hist) s += (v - m) * (v - m);
        return std::sqrt(s / (n - 1));
    }
};

// ---------------- Cell config ---------------------------------------------
enum class CellKind { EMA, Donchian };

struct CellConfig {
    const char* name;
    CellKind    kind;
    char        tf;            // '1' = H1, '4' = H4
    int         fast;          // EMA fast or unused
    int         slow;          // EMA slow or unused
    int         don_n;         // Donchian N or unused
    double      atr_stop_mult; // ATR stop multiplier
    double      bars_per_year; // for vol-target sizing
};

static constexpr CellConfig kCells[] = {
    { "A_H1_EMA20_80",  CellKind::EMA,      '1', 20, 80, 0,  4.0, cfg::BPY_H1 },
    { "B_H4_EMA8_21",   CellKind::EMA,      '4',  8, 21, 0,  2.5, cfg::BPY_H4 },
    { "C_H1_Donchian40",CellKind::Donchian, '1',  0,  0, 40, 5.0, cfg::BPY_H1 },
};
static constexpr int kNumCells = sizeof(kCells) / sizeof(kCells[0]);

// ---------------- Per-cell position state ---------------------------------
struct Pos {
    bool    active = false;
    double  entry_px = 0, stop_px = 0;
    double  units = 0;          // ounces
    int64_t entry_ts_ms = 0;
    int     entry_bar_idx = 0;
    double  mfe = 0, mae = 0;
};

struct Trade {
    std::string cell;
    int64_t entry_ts_ms = 0, exit_ts_ms = 0;
    double  entry_px = 0, exit_px = 0;
    double  units = 0;
    double  pnl = 0;
    int     bars_held = 0;
    const char* reason = "";
};

// ---------------- Cell runner ---------------------------------------------
struct CellRunner {
    int          ci;             // cell index
    BarBuilder*  bb;
    Pos          pos{};
    double       cum_pnl = 0;
    double       peak_pnl = 0;
    double       max_dd = 0;
    int          n_wins = 0, n_losses = 0;
    double       gross_win = 0, gross_loss = 0;   // for PF (post-cost)
    int          bar_idx = 0;
    std::vector<Trade> trades;
    std::vector<double> bar_pnl_track;   // PnL per bar close on this TF (debug/equity)

    CellRunner(int i, BarBuilder* b) : ci(i), bb(b) {}

    // on every tick: check stop hit (use bid for long exit; safer than mid)
    void on_tick(double bid, double ask, int64_t ts_ms) {
        if (!pos.active) return;
        // intra-bar high/low tracked by current bar
        const Bar& cur = bb->current;
        double mid = (bid + ask) * 0.5;
        // update MFE/MAE (long-only)
        double fav = mid - pos.entry_px;
        if (fav > pos.mfe) pos.mfe = fav;
        if (-fav > pos.mae) pos.mae = -fav;
        // stop hit if low of intra-bar traded through stop
        if (cur.low <= pos.stop_px) {
            _close(pos.stop_px, ts_ms, "stop");
        }
    }

    // on new bar close: check signal; exit-on-signal-flat or entry
    void on_bar_close() {
        const CellConfig& c = kCells[ci];
        bar_idx = bb->total_bars;
        if ((int)bb->bars.size() < std::max({c.slow, c.don_n, cfg::VOL_LOOKBACK_BARS}) + 4) return;
        const Bar& b = bb->bars.back();
        int64_t now_ms = b.open_ms + bb->period_ms;

        bool sig_long = false;
        bool sig_exit = false;
        if (c.kind == CellKind::EMA) {
            sig_long = bb->fast_above_slow() && bb->slow_rising();
            sig_exit = !sig_long;
        } else if (c.kind == CellKind::Donchian) {
            // long breakout above don_high; exit on close < don_low
            if (!pos.active && b.close > bb->don_high && bb->don_high > 0) sig_long = true;
            if (pos.active && b.close < bb->don_low) sig_exit = true;
        }

        // Exit first (close exit on bar close at close px) -- never both same bar
        if (pos.active && sig_exit) {
            _close(b.close, now_ms, "signal");
        }
        if (!pos.active && sig_long) {
            double atr = bb->atr;
            if (atr <= 0) return;
            double stop_dist = c.atr_stop_mult * atr;
            // vol-target sizing: size such that one-bar move at realized stdev * sqrt(BPY) = vol target
            double sd = bb->log_ret_stdev();
            if (sd <= 0) return;
            double ann_vol = sd * std::sqrt(c.bars_per_year);
            if (ann_vol <= 0) return;
            double scalar = std::min(4.0, cfg::VOL_TARGET_ANN / ann_vol);
            // 1% risk-of-equity scaled by scalar; units = risk_usd / stop_dist
            double eq = cfg::STARTING_EQUITY + cum_pnl;
            double risk_usd = eq * 0.01 * scalar;
            double units = (stop_dist > 0) ? risk_usd / stop_dist : 0;
            double notional_cap = (eq * cfg::MAX_FRAC_NOTIONAL) / b.close;
            units = std::min(units, notional_cap);
            if (units <= 0) return;
            pos.active = true;
            pos.entry_px = b.close;
            pos.stop_px = b.close - stop_dist;
            pos.units = units;
            pos.entry_ts_ms = now_ms;
            pos.entry_bar_idx = bar_idx;
            pos.mfe = pos.mae = 0;
        }
    }

    void force_close(double bid, int64_t ts_ms) {
        if (pos.active) _close(bid, ts_ms, "eod");
    }

private:
    void _close(double exit_px, int64_t ts_ms, const char* reason) {
        double pnl = (exit_px - pos.entry_px) * pos.units;
        pnl -= cfg::COST_RT_PTS * std::fabs(pos.units);   // IBKR round-trip cost
        cum_pnl += pnl;
        if (cum_pnl > peak_pnl) peak_pnl = cum_pnl;
        double dd = peak_pnl - cum_pnl;
        if (dd > max_dd) max_dd = dd;
        if (pnl > 0) { ++n_wins; gross_win += pnl; }
        else if (pnl < 0) { ++n_losses; gross_loss += -pnl; }
        Trade t;
        t.cell = kCells[ci].name;
        t.entry_ts_ms = pos.entry_ts_ms;
        t.exit_ts_ms = ts_ms;
        t.entry_px = pos.entry_px;
        t.exit_px = exit_px;
        t.units = pos.units;
        t.pnl = pnl;
        t.bars_held = bar_idx - pos.entry_bar_idx;
        t.reason = reason;
        trades.push_back(t);
        pos = {};
    }
};

// ---------------- Stats helpers -------------------------------------------
struct CellStats {
    std::string name;
    int n_trades = 0, n_wins = 0;
    double pnl = 0, mdd = 0, sharpe = 0;
    double hit_pct = 0;
};

static double compute_sharpe(const std::vector<double>& bar_pnl, double bars_per_year) {
    if (bar_pnl.size() < 30) return 0.0;
    double m = 0; for (double v : bar_pnl) m += v; m /= bar_pnl.size();
    double s = 0; for (double v : bar_pnl) s += (v - m) * (v - m);
    s = std::sqrt(s / (bar_pnl.size() - 1));
    if (s <= 0) return 0.0;
    // bar PnL is in $, equity ~ starting equity. Approximate as % return.
    // SR = mean_return / std_return * sqrt(BPY); use $/$100k as fraction.
    double m_frac = m / cfg::STARTING_EQUITY;
    double s_frac = s / cfg::STARTING_EQUITY;
    return m_frac / s_frac * std::sqrt(bars_per_year);
}

// ---------------- Main streaming loop -------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <duka_combined.csv>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::string line;
    // header line
    std::getline(f, line);

    // build two bar builders sharing tick stream
    BarBuilder bb_h1(3600LL * 1000LL, kCells[0].fast, kCells[0].slow, kCells[2].don_n);
    BarBuilder bb_h4(14400LL * 1000LL, kCells[1].fast, kCells[1].slow, 0);

    // cells: A,C use bb_h1; B uses bb_h4
    CellRunner runners[kNumCells] = {
        CellRunner(0, &bb_h1),
        CellRunner(1, &bb_h4),
        CellRunner(2, &bb_h1),
    };

    int64_t n_ticks = 0, last_progress = 0;
    int64_t first_ts = 0, last_ts = 0;
    double first_mid = 0, last_mid = 0;
    double last_bid = 0;
    // per-H1-bar equity track for sharpe
    std::vector<double> h1_bar_pnl;     // ensemble per-H1-bar PnL
    double prev_total_pnl = 0;

    auto on_bar_h1 = [&](void) {
        for (auto& r : runners) if (r.bb == &bb_h1) r.on_bar_close();
        double total = 0; for (auto& r : runners) total += r.cum_pnl;
        h1_bar_pnl.push_back(total - prev_total_pnl);
        prev_total_pnl = total;
    };
    auto on_bar_h4 = [&](void) {
        for (auto& r : runners) if (r.bb == &bb_h4) r.on_bar_close();
    };

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // parse timestamp,bid,ask  (format_a column order: ts_ms,bid,ask)
        const char* s = line.c_str();
        char* end = nullptr;
        int64_t ts = strtoll(s, &end, 10);
        if (*end != ',') continue;
        double bid = strtod(end + 1, &end);
        if (*end != ',') continue;
        double ask = strtod(end + 1, &end);
        if (ask <= 0 || bid <= 0 || ask <= bid) continue;
        if (cfg::is_weekend(ts)) continue;
        double mid = (ask + bid) * 0.5;
        if (first_ts == 0) { first_ts = ts; first_mid = mid; }
        last_ts = ts; last_mid = mid;
        last_bid = bid;
        // on_tick stops
        for (auto& r : runners) r.on_tick(bid, ask, ts);
        // build bars (H4 first so cells can see fresh data either way)
        bool h4_closed = bb_h4.on_tick(mid, ts);
        bool h1_closed = bb_h1.on_tick(mid, ts);
        if (h4_closed) on_bar_h4();
        if (h1_closed) on_bar_h1();
        ++n_ticks;
        if (n_ticks - last_progress > 10000000) {
            std::fprintf(stderr, "  %lld ticks processed\n", (long long)n_ticks);
            last_progress = n_ticks;
        }
    }
    // force-close any open positions
    for (auto& r : runners) r.force_close(last_bid, last_ts);

    // --- aggregate metrics ---
    double ensemble_pnl = 0;
    for (auto& r : runners) ensemble_pnl += r.cum_pnl;
    double ensemble_ret_pct = ensemble_pnl / cfg::STARTING_EQUITY * 100.0;

    // Inverse-vol-weighted ensemble (matches Python harness exactly).
    // Per-cell weight = (1/sigma_i) / sum_j(1/sigma_j). sigma = stdev of trade PnL.
    double inv_vol[kNumCells] = {0}, inv_sum = 0;
    for (int i = 0; i < kNumCells; ++i) {
        const auto& trades_i = runners[i].trades;
        if (trades_i.size() < 2) { inv_vol[i] = 0; continue; }
        double m = 0; for (const auto& t : trades_i) m += t.pnl; m /= trades_i.size();
        double s = 0; for (const auto& t : trades_i) s += (t.pnl - m) * (t.pnl - m);
        s = std::sqrt(s / (trades_i.size() - 1));
        inv_vol[i] = (s > 0) ? 1.0 / s : 0.0;
        inv_sum += inv_vol[i];
    }
    double weight[kNumCells] = {0};
    if (inv_sum > 0) {
        for (int i = 0; i < kNumCells; ++i) weight[i] = inv_vol[i] / inv_sum;
    }
    double weighted_pnl = 0;
    for (int i = 0; i < kNumCells; ++i) weighted_pnl += weight[i] * runners[i].cum_pnl;
    double weighted_ret_pct = weighted_pnl / cfg::STARTING_EQUITY * 100.0;

    // years span for CAGR
    double years = (last_ts - first_ts) / 1000.0 / 86400.0 / 365.25;
    double cagr_pct = 0;
    if (years > 0 && cfg::STARTING_EQUITY + ensemble_pnl > 0) {
        cagr_pct = (std::pow((cfg::STARTING_EQUITY + ensemble_pnl) / cfg::STARTING_EQUITY,
                             1.0 / years) - 1.0) * 100.0;
    }

    double ensemble_sharpe = compute_sharpe(h1_bar_pnl, cfg::BPY_H1);

    // ensemble MDD on H1 ensemble equity (sum of cell cum PnL approximated per H1 bar)
    double eq = 0, peak = 0, mdd = 0;
    for (double p : h1_bar_pnl) {
        eq += p;
        if (eq > peak) peak = eq;
        double dd = (eq - peak) / (cfg::STARTING_EQUITY + peak);
        if (dd < mdd) mdd = dd;
    }
    double ensemble_mdd_pct = mdd * 100.0;

    // buy & hold benchmark
    double bh_units = cfg::STARTING_EQUITY / first_mid;
    double bh_pnl = (last_mid - first_mid) * bh_units;
    double bh_ret_pct = bh_pnl / cfg::STARTING_EQUITY * 100.0;

    std::printf("\n=== GoldTrendEnsembleBacktest (S114 C++ validation) ===\n");
    std::printf("data: %s\n", path);
    std::printf("ticks: %lld   years: %.3f\n", (long long)n_ticks, years);
    {
        time_t t0 = first_ts / 1000, t1 = last_ts / 1000;
        char b0[32], b1[32];
        std::strftime(b0, sizeof(b0), "%Y-%m-%d %H:%M:%S", gmtime(&t0));
        std::strftime(b1, sizeof(b1), "%Y-%m-%d %H:%M:%S", gmtime(&t1));
        std::printf("window: %s -> %s UTC\n", b0, b1);
    }
    std::printf("\n%-22s %12s %9s %7s %7s %5s %6s\n",
                "cell", "pnl_usd", "return%", "MDD_usd", "trades", "win%", "");
    std::printf("--------------------------------------------------------------------------\n");
    int total_trades = 0, total_wins = 0;
    for (auto& r : runners) {
        int nt = (int)r.trades.size();
        double hit = (r.n_wins + r.n_losses > 0)
                     ? 100.0 * r.n_wins / (r.n_wins + r.n_losses) : 0;
        double pf = (r.gross_loss > 0) ? r.gross_win / r.gross_loss
                                       : (r.gross_win > 0 ? 999.0 : 0.0);
        // trade-based per-cell Sharpe: mean/std of trade PnL, annualized by
        // trades-per-year (years computed above). Real, post-cost.
        double cell_sharpe = 0.0;
        if (nt >= 2 && years > 0) {
            double m = 0; for (const auto& t : r.trades) m += t.pnl; m /= nt;
            double v = 0; for (const auto& t : r.trades) v += (t.pnl - m) * (t.pnl - m);
            double sd = std::sqrt(v / (nt - 1));
            if (sd > 0) cell_sharpe = (m / sd) * std::sqrt((double)nt / years);
        }
        std::printf("%-22s %+12.2f %+9.2f %7.2f %7d %5.1f  PF=%.2f Sh=%+.2f\n",
                    kCells[r.ci].name,
                    r.cum_pnl,
                    r.cum_pnl / cfg::STARTING_EQUITY * 100.0,
                    r.max_dd,
                    nt, hit, pf, cell_sharpe);
        total_trades += nt; total_wins += r.n_wins;
    }
    std::printf("--------------------------------------------------------------------------\n");
    std::printf("ensemble (sum=full leverage)     %+12.2f %+9.2f %7s %7d %5.1f\n",
                ensemble_pnl, ensemble_ret_pct, "-", total_trades,
                total_trades ? 100.0 * total_wins / total_trades : 0.0);
    std::printf("ensemble (inv-vol weighted)      %+12.2f %+9.2f %7s   weights:",
                weighted_pnl, weighted_ret_pct, "-");
    for (int i = 0; i < kNumCells; ++i)
        std::printf(" %s=%.3f", kCells[i].name, weight[i]);
    std::printf("\n");

    std::printf("\nensemble Sharpe (per-H1-bar PnL): %+.3f\n", ensemble_sharpe);
    std::printf("ensemble approx MDD %%           : %+.3f\n", ensemble_mdd_pct);
    std::printf("ensemble CAGR %%                 : %+.3f\n", cagr_pct);
    std::printf("\nbuy & hold                       : %+.2f USD  (%+.2f%%)\n", bh_pnl, bh_ret_pct);
    std::printf("\nPython reference (S114):\n");
    std::printf("  A_H1_EMA20_80  : +$25,478 (+25.48%%)  Sharpe +1.97 trades 181\n");
    std::printf("  B_H4_EMA8_21   : +$30,966 (+30.97%%)  Sharpe +1.96 trades 103\n");
    std::printf("  C_H1_Donchian40: +$29,634 (+29.63%%)  Sharpe +2.11 trades 75\n");
    std::printf("  ensemble (inv-vol weighted): +$28,145 (+28.15%%) Sharpe +3.21 MDD -2.29%%\n");

    // --- CSV outputs ---
    {
        std::ofstream rep("bt_gold_trend_ensemble_report.csv");
        rep << "cell,pnl_usd,return_pct,max_dd_usd,n_trades,n_wins,n_losses,hit_pct\n";
        for (auto& r : runners) {
            double hit = (r.n_wins + r.n_losses > 0)
                         ? 100.0 * r.n_wins / (r.n_wins + r.n_losses) : 0;
            rep << kCells[r.ci].name << ',' << r.cum_pnl << ','
                << (r.cum_pnl / cfg::STARTING_EQUITY * 100.0) << ','
                << r.max_dd << ','
                << r.trades.size() << ',' << r.n_wins << ',' << r.n_losses << ','
                << hit << '\n';
        }
        rep << "ensemble," << ensemble_pnl << ',' << ensemble_ret_pct << ",-,"
            << total_trades << ',' << total_wins << ',' << (total_trades - total_wins) << ','
            << (total_trades ? 100.0 * total_wins / total_trades : 0.0) << '\n';
    }
    {
        std::ofstream trd("bt_gold_trend_ensemble_trades.csv");
        trd << "cell,entry_ts_ms,exit_ts_ms,entry_px,exit_px,units,pnl,bars_held,reason\n";
        for (auto& r : runners) {
            for (const auto& t : r.trades) {
                trd << t.cell << ',' << t.entry_ts_ms << ',' << t.exit_ts_ms << ','
                    << t.entry_px << ',' << t.exit_px << ',' << t.units << ','
                    << t.pnl << ',' << t.bars_held << ',' << t.reason << '\n';
            }
        }
    }
    {
        std::ofstream eqf("bt_gold_trend_ensemble_equity.csv");
        eqf << "bar_idx,ensemble_pnl_usd\n";
        double cum = 0;
        for (size_t i = 0; i < h1_bar_pnl.size(); ++i) {
            cum += h1_bar_pnl[i];
            eqf << i << ',' << cum << '\n';
        }
    }
    std::printf("\nwrote: bt_gold_trend_ensemble_report.csv\n");
    std::printf("wrote: bt_gold_trend_ensemble_trades.csv\n");
    std::printf("wrote: bt_gold_trend_ensemble_equity.csv\n");
    return 0;
}
