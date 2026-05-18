// =============================================================================
// quick_scalp_bt.cpp -- Standalone backtest harness for QuickScalpEngine
// =============================================================================
//
// Build (Mac/Linux):
//   clang++ -O3 -std=c++17 -o backtest/quick_scalp_bt backtest/quick_scalp_bt.cpp
//
// Run:
//   ./backtest/quick_scalp_bt /path/to/XAUUSD_combined.csv
//   or
//   ./backtest/quick_scalp_bt   (uses default: ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv)
//
// CSV format expected: timestamp_ms,askPrice,bidPrice (Dukascopy combined)
//
// What this does:
//   1. Streams the tick CSV, building synthesized M1 bars in memory.
//   2. Maintains M1 Wilder ATR14 + Wilder RSI14 from M1 bar closes.
//   3. Synthesizes L2 imbalance + book_slope from tick-level price velocity
//      (the historical tape has no DOM; live runs will use real L2 from
//      MacroContext, this proxy is a conservative stand-in for backtest).
//   4. Drives QuickScalpEngine::on_tick() with bid/ask + synth L2 + RSI/ATR.
//   5. Records all closed trades via the engine's on_close_cb callback.
//   6. Sweeps a small parameter grid and reports PnL/WR/PF/DD per config.
//
// Cost model: ExecutionCostGuard is stubbed to a header-only stub that
//   returns true if (tp_dist * USD_PER_PT_LOT * lot) covers (RT_COST_PTS *
//   USD_PER_PT_LOT * lot * coverage_mult), matching the production gate's
//   shape. Spread + half-cost-on-each-side built into the fill model
//   (LONG buys at ask, sells at bid -- ditto SHORT inverted, so spread is
//   paid on every open and close).
//
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <functional>

// Use the real production headers (no stubs -- avoids redefinition clashes
// against the engine's includes).
#include "../include/QuickScalpEngine.hpp"

// -----------------------------------------------------------------------------
// Param config struct for the sweep
// -----------------------------------------------------------------------------
struct Config {
    omega::QuickScalpEngine::Mode mode;
    double velocity_pts;
    double be_arm_pts;
    double tp_min_pts;
    double sl_pts;
    int    vel_window_sec;
    int    l2_avg_window;
};

static const char* mode_name(omega::QuickScalpEngine::Mode m) {
    return m == omega::QuickScalpEngine::Mode::MOMENTUM ? "MOMENT" : "FADE";
}

// -----------------------------------------------------------------------------
// Per-config result
// -----------------------------------------------------------------------------
struct Result {
    Config cfg;
    int    n_trades    = 0;
    int    n_wins      = 0;
    int    n_losses    = 0;
    int    n_scratches = 0;  // BE_SCRATCH outcomes
    double gross_pnl   = 0.0;
    double max_dd      = 0.0;
    double profit_factor = 0.0;
    double wr_pct      = 0.0;
};

// -----------------------------------------------------------------------------
// M1 indicator engine: Wilder ATR14 + Wilder RSI14
// -----------------------------------------------------------------------------
struct M1Indicators {
    // ATR14
    bool   atr_primed = false;
    double atr14      = 0.0;
    double atr_avg    = 0.0;
    std::deque<double> atr_seed;

    // RSI14
    bool   rsi_primed = false;
    double rsi14      = 50.0;
    double rsi_avg_gain = 0.0;
    double rsi_avg_loss = 0.0;
    std::deque<double> rsi_seed_gain;
    std::deque<double> rsi_seed_loss;

    // Bar tracking
    bool   have_prev_close = false;
    double prev_close      = 0.0;

    void on_m1_close(double open, double high, double low, double close) {
        // ATR
        double tr;
        if (!have_prev_close) tr = high - low;
        else {
            tr = std::max({high - low,
                           std::fabs(high - prev_close),
                           std::fabs(low - prev_close)});
        }
        if (!atr_primed) {
            atr_seed.push_back(tr);
            if ((int)atr_seed.size() >= 14) {
                double sum = 0.0;
                for (double t : atr_seed) sum += t;
                atr_avg    = sum / 14.0;
                atr14      = atr_avg;
                atr_primed = true;
            }
        } else {
            atr_avg = (atr_avg * 13.0 + tr) / 14.0;
            atr14   = atr_avg;
        }

        // RSI
        if (have_prev_close) {
            const double change = close - prev_close;
            const double gain   = std::max(0.0,  change);
            const double loss   = std::max(0.0, -change);
            if (!rsi_primed) {
                rsi_seed_gain.push_back(gain);
                rsi_seed_loss.push_back(loss);
                if ((int)rsi_seed_gain.size() >= 14) {
                    double sg = 0.0, sl = 0.0;
                    for (double v : rsi_seed_gain) sg += v;
                    for (double v : rsi_seed_loss) sl += v;
                    rsi_avg_gain = sg / 14.0;
                    rsi_avg_loss = sl / 14.0;
                    rsi_primed   = true;
                }
            } else {
                rsi_avg_gain = (rsi_avg_gain * 13.0 + gain) / 14.0;
                rsi_avg_loss = (rsi_avg_loss * 13.0 + loss) / 14.0;
            }
            if (rsi_primed) {
                if (rsi_avg_loss < 1e-9) rsi14 = 100.0;
                else {
                    const double rs = rsi_avg_gain / rsi_avg_loss;
                    rsi14 = 100.0 - (100.0 / (1.0 + rs));
                }
            }
        }

        have_prev_close = true;
        prev_close      = close;
    }
};

// -----------------------------------------------------------------------------
// Driver: streams ticks, builds M1 bars, synthesizes L2, drives engine
// -----------------------------------------------------------------------------
struct Driver {
    omega::QuickScalpEngine engine;
    M1Indicators            ind;

    // M1 bar state
    int64_t cur_anchor_min = -1;
    double  cur_open       = 0.0;
    double  cur_high       = 0.0;
    double  cur_low        = 0.0;
    double  cur_close      = 0.0;
    int     cur_n          = 0;

    // Synth L2 from short-window price velocity
    struct TickPx { int64_t ts_ms; double px; };
    std::deque<TickPx> synth_buf;  // last ~5 seconds

    // Trade log + running PnL
    std::vector<omega::TradeRecord> trades;
    double cum_pnl  = 0.0;
    double peak_pnl = 0.0;
    double max_dd   = 0.0;

    Driver() {
        engine.on_close_cb = [this](const omega::TradeRecord& tr) {
            trades.push_back(tr);
            // S99h fix (2026-05-18 part C): engine reports tr.pnl in pts*lots
            // (production path multiplies by tick_value downstream). The
            // harness does not run trade_lifecycle, so multiply here to
            // track the USD equity curve / max DD.
            constexpr double XAUUSD_TICK_VALUE = 100.0;
            const double pnl_usd = tr.pnl * XAUUSD_TICK_VALUE;
            cum_pnl += pnl_usd;
            if (cum_pnl > peak_pnl) peak_pnl = cum_pnl;
            double dd = peak_pnl - cum_pnl;
            if (dd > max_dd) max_dd = dd;
        };
    }

    void reset() {
        engine.m_pos = decltype(engine.m_pos){};
        ind = M1Indicators{};
        cur_anchor_min = -1;
        synth_buf.clear();
        trades.clear();
        cum_pnl = 0.0;
        peak_pnl = 0.0;
        max_dd = 0.0;
    }

    void on_tick(int64_t ts_ms, double bid, double ask) {
        const double mid = (bid + ask) * 0.5;

        // ---- Build M1 bars ----
        const int64_t ts_min = (ts_ms / 1000) / 60;
        if (cur_anchor_min < 0) {
            cur_anchor_min = ts_min;
            cur_open = cur_high = cur_low = cur_close = mid;
            cur_n = 1;
        } else if (ts_min != cur_anchor_min) {
            // Close prior M1 bar -> update indicators
            ind.on_m1_close(cur_open, cur_high, cur_low, cur_close);
            // Open new bar
            cur_anchor_min = ts_min;
            cur_open = cur_high = cur_low = cur_close = mid;
            cur_n = 1;
        } else {
            if (mid > cur_high) cur_high = mid;
            if (mid < cur_low)  cur_low  = mid;
            cur_close = mid;
            ++cur_n;
        }

        // ---- Synthesize L2 imbalance + slope from short-window velocity ----
        synth_buf.push_back({ts_ms, mid});
        while (!synth_buf.empty()
               && synth_buf.front().ts_ms < ts_ms - 5000LL) {
            synth_buf.pop_front();
        }

        double synth_imb   = 0.5;
        double synth_slope = 0.0;
        bool   l2_real     = false;
        if (synth_buf.size() >= 10) {
            const double dpx = mid - synth_buf.front().px;
            const double dt_s = std::max(1.0,
                (ts_ms - synth_buf.front().ts_ms) / 1000.0);
            const double v = dpx / dt_s;  // pts/sec
            // Map velocity -> imbalance via tanh, scaled so v=0.3pt/sec saturates
            synth_imb   = 0.5 + std::tanh(v * 1.5) * 0.5;
            synth_slope = v;
            l2_real     = true;
        }

        // ---- Drive the engine ----
        engine.on_tick(bid, ask, ts_ms,
                       /*can_enter=*/true,
                       synth_imb, synth_slope, l2_real,
                       ind.rsi14, ind.atr14);
    }
};

// -----------------------------------------------------------------------------
// CSV streamer: reads Dukascopy combined CSV (timestamp_ms,ask,bid)
// -----------------------------------------------------------------------------
static int64_t parse_csv_and_run(Driver& drv, const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[QSC-BT] FAIL: cannot open %s\n", path);
        return -1;
    }

    std::string line;
    int64_t n_ticks = 0;
    int64_t n_skipped = 0;

    // Skip header line if present
    if (std::getline(f, line)) {
        // Check if first line is numeric (data) or header
        if (!line.empty() && !std::isdigit((unsigned char)line[0])) {
            // Header, skip
        } else {
            // Data line, rewind
            f.clear();
            f.seekg(0);
        }
    }

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        // Parse: timestamp_ms,ask,bid  (CSV)
        char* p1;
        int64_t ts_ms = std::strtoll(line.c_str(), &p1, 10);
        if (!p1 || *p1 != ',') { ++n_skipped; continue; }
        char* p2;
        double ask = std::strtod(p1 + 1, &p2);
        if (!p2 || *p2 != ',') { ++n_skipped; continue; }
        char* p3;
        double bid = std::strtod(p2 + 1, &p3);
        (void)p3;

        if (!std::isfinite(ask) || !std::isfinite(bid)) { ++n_skipped; continue; }
        if (ask <= 0.0 || bid <= 0.0) { ++n_skipped; continue; }
        if (bid > ask) std::swap(bid, ask);  // tape sanity
        const double spread = ask - bid;
        if (spread > 5.0) { ++n_skipped; continue; }  // anomaly filter

        drv.on_tick(ts_ms, bid, ask);
        ++n_ticks;

        // Periodic progress every 10M ticks
        if (n_ticks % 10000000LL == 0) {
            std::fprintf(stderr, "[QSC-BT] %lld M ticks processed (cum_pnl=%.2f n_trades=%zu)\n",
                         (long long)(n_ticks / 1000000LL), drv.cum_pnl, drv.trades.size());
        }
    }

    std::fprintf(stderr, "[QSC-BT] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

// -----------------------------------------------------------------------------
// Apply a config to the engine and reset
// -----------------------------------------------------------------------------
static void apply_config(omega::QuickScalpEngine& e, const Config& c) {
    e.mode                = c.mode;
    e.VELOCITY_PTS        = c.velocity_pts;
    e.BE_ARM_PTS          = c.be_arm_pts;
    e.TP_MIN_PTS          = c.tp_min_pts;
    e.SL_PTS              = c.sl_pts;
    e.VELOCITY_WINDOW_SEC = c.vel_window_sec;
    e.L2_AVG_WINDOW       = c.l2_avg_window;
    e.shadow_mode         = true;
    e.enabled             = true;
}

// -----------------------------------------------------------------------------
// Compute summary statistics from trade log
// -----------------------------------------------------------------------------
static Result summarize(const Config& cfg, const std::vector<omega::TradeRecord>& trades, double max_dd) {
    Result r;
    r.cfg     = cfg;
    r.max_dd  = max_dd;
    r.n_trades = (int)trades.size();

    // S99h fix: engine reports tr.pnl in pts*lots; harness scales to USD.
    constexpr double XAUUSD_TICK_VALUE = 100.0;
    double gross_win = 0.0, gross_loss = 0.0;
    for (const auto& t : trades) {
        const double pnl_usd = t.pnl * XAUUSD_TICK_VALUE;
        r.gross_pnl += pnl_usd;
        if (pnl_usd > 0.0)      { ++r.n_wins;   gross_win  += pnl_usd; }
        else if (pnl_usd < 0.0) { ++r.n_losses; gross_loss += -pnl_usd; }
        if (t.exitReason == "BE_SCRATCH") ++r.n_scratches;
    }
    r.wr_pct = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    return r;
}

// -----------------------------------------------------------------------------
// Main: parameter sweep
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";

    std::fprintf(stderr, "[QSC-BT] CSV: %s\n", csv_path);

    // Parameter sweep grid -- FAST tunings + mode comparison
    std::vector<Config> configs;
    using Mode = omega::QuickScalpEngine::Mode;
    const Mode modes[]               = {Mode::FADE, Mode::MOMENTUM};
    const double velocity_pts_grid[] = {0.20, 0.30, 0.50};
    const double be_arm_pts_grid[]   = {0.20, 0.30, 0.40};
    const double tp_min_pts_grid[]   = {0.80, 1.00};
    const int    vel_window_grid[]   = {2, 3, 5};
    for (Mode m : modes)
        for (double v : velocity_pts_grid)
            for (double b : be_arm_pts_grid)
                for (double t : tp_min_pts_grid)
                    for (int vw : vel_window_grid)
                        configs.push_back({m, v, b, t, /*sl_pts*/0.60, vw, /*l2_avg*/3});

    std::printf("============================================================================\n");
    std::printf("  QuickScalpEngine sweep -- %zu configs\n", configs.size());
    std::printf("  CSV: %s\n", csv_path);
    std::printf("============================================================================\n\n");

    std::vector<Result> results;
    results.reserve(configs.size());

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr, "[QSC-BT] === Config %zu/%zu: MODE=%s VEL=%.2f(%ds) BE=%.2f TP=%.2f ===\n",
                     i + 1, configs.size(), mode_name(cfg.mode),
                     cfg.velocity_pts, cfg.vel_window_sec, cfg.be_arm_pts, cfg.tp_min_pts);

        Driver drv;
        apply_config(drv.engine, cfg);

        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n_ticks = parse_csv_and_run(drv, csv_path);
        const auto t1 = std::chrono::steady_clock::now();
        if (n_ticks < 0) {
            std::fprintf(stderr, "[QSC-BT] CSV read failed, aborting\n");
            return 1;
        }
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);

        std::fprintf(stderr, "[QSC-BT] Config %zu done in %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f dd=$%.2f\n\n",
                     i + 1, secs, r.n_trades, r.wr_pct, r.gross_pnl,
                     r.profit_factor, r.max_dd);
    }

    // Sort by gross PnL descending
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) { return a.gross_pnl > b.gross_pnl; });

    std::printf("\n=========================================================================================\n");
    std::printf("  RESULTS (sorted by gross PnL)\n");
    std::printf("=========================================================================================\n");
    std::printf("%-7s %-6s %-6s %-6s %-6s %-7s %-7s %-12s %-7s %-9s %-7s\n",
                "MODE", "VEL", "VW", "BE", "TP", "N", "WR%", "PnL$", "PF", "DD$", "Scratch");
    std::printf("-----------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-7s %-6.2f %-6d %-6.2f %-6.2f %-7d %-7.1f %-+12.2f %-7.2f %-9.2f %-7d\n",
                    mode_name(r.cfg.mode), r.cfg.velocity_pts, r.cfg.vel_window_sec,
                    r.cfg.be_arm_pts, r.cfg.tp_min_pts,
                    r.n_trades, r.wr_pct, r.gross_pnl, r.profit_factor, r.max_dd, r.n_scratches);
    }
    std::printf("=========================================================================================\n");

    if (!results.empty()) {
        const auto& best = results.front();
        std::printf("\nBest config:\n");
        std::printf("  MODE                = %s\n", mode_name(best.cfg.mode));
        std::printf("  VELOCITY_PTS        = %.2f over %d sec\n", best.cfg.velocity_pts, best.cfg.vel_window_sec);
        std::printf("  BE_ARM_PTS          = %.2f\n", best.cfg.be_arm_pts);
        std::printf("  TP_MIN_PTS          = %.2f\n", best.cfg.tp_min_pts);
        std::printf("  SL_PTS              = %.2f\n", best.cfg.sl_pts);
        std::printf("  Trades=%d  WR=%.1f%%  PnL=$%+.2f  PF=%.2f  DD=$%.2f  Scratch=%d\n",
                    best.n_trades, best.wr_pct, best.gross_pnl,
                    best.profit_factor, best.max_dd, best.n_scratches);
    }

    return 0;
}
