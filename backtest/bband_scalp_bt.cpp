// =============================================================================
// bband_scalp_bt.cpp -- Standalone backtest harness for BBandScalpEngine
// =============================================================================
//
// Build (Mac/Linux):
//   clang++ -O3 -std=c++17 -o backtest/bband_scalp_bt backtest/bband_scalp_bt.cpp
//
// Run:
//   ./backtest/bband_scalp_bt /path/to/XAUUSD_combined.csv
//   or
//   ./backtest/bband_scalp_bt   (uses default:
//       ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv)
//
// CSV format expected: timestamp_ms,askPrice,bidPrice (Dukascopy combined).
//
// What this does:
//   1. Streams the tick CSV, building synthesized M1 bars in memory.
//   2. Maintains M1 Bollinger(period, stdev_mult), Wilder ATR14, and
//      Wilder RSI14 from M1 bar closes. Same Bollinger formula as
//      production OHLCBarEngine::_update_bollinger.
//   3. Drives BBandScalpEngine::on_tick() with bid/ask + bb_upper/mid/lower
//      + rsi14 + atr14.
//   4. Records all closed trades via the engine's on_close_cb callback.
//   5. Sweeps a parameter grid and reports PnL/WR/PF/DD per config.
//
// Cost model: uses the production ExecutionCostGuard via the same header
//   chain BBandScalpEngine.hpp pulls in.
//
// Spread model: fills LONG at ask, exit LONG at bid (and inverted for
//   SHORT) -- spread is paid on every open and close, matching live.
//
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <functional>

// Pull in the real production header (no stub -- ExecutionCostGuard etc.
// come along for free via OmegaCostGuard.hpp).
#include "../include/BBandScalpEngine.hpp"

// -----------------------------------------------------------------------------
// Sweep config struct
// -----------------------------------------------------------------------------
struct Config {
    int    bb_period;
    double bb_stdev_mult;
    double rsi_oversold;     // long-side threshold
    double rsi_overbought;   // short-side threshold
    double be_arm_pts;
    double sl_pts;
};

// -----------------------------------------------------------------------------
// Per-config aggregate result
// -----------------------------------------------------------------------------
struct Result {
    Config cfg;
    int    n_trades       = 0;
    int    n_wins         = 0;
    int    n_losses       = 0;
    int    n_be_scratch   = 0;   // BE_SCRATCH (lock saved the bad signal)
    int    n_trail_win    = 0;   // TRAIL_WIN  (BE arm + trail captured the win)
    int    n_tp_hit       = 0;
    int    n_sl_hit       = 0;
    int    n_time_stop    = 0;
    double gross_pnl      = 0.0;
    double max_dd         = 0.0;
    double profit_factor  = 0.0;
    double wr_pct         = 0.0;
};

// -----------------------------------------------------------------------------
// M1 indicator engine: Bollinger(period, stdev_mult), Wilder ATR14, Wilder RSI14
//
// Bollinger formula matches OHLCBarEngine::_update_bollinger exactly:
//   mid   = simple mean of last BB_P closes
//   sigma = population sqrt(sum_sq / BB_P)
//   upper = mid + STDEV * sigma
//   lower = mid - STDEV * sigma
// -----------------------------------------------------------------------------
struct M1Indicators {
    // Config -- set per-sweep before driving bars
    int    bb_period       = 20;
    double bb_stdev_mult   = 2.0;

    // Rolling close buffer for Bollinger
    std::deque<double> close_buf;
    double bb_upper = 0.0;
    double bb_mid   = 0.0;
    double bb_lower = 0.0;
    bool   bb_primed = false;

    // ATR14 (Wilder)
    bool   atr_primed   = false;
    double atr14        = 0.0;
    double atr_avg      = 0.0;
    std::deque<double> atr_seed;

    // RSI14 (Wilder)
    bool   rsi_primed       = false;
    double rsi14            = 50.0;
    double rsi_avg_gain     = 0.0;
    double rsi_avg_loss     = 0.0;
    std::deque<double> rsi_seed_gain;
    std::deque<double> rsi_seed_loss;

    // Prev close
    bool   have_prev_close = false;
    double prev_close      = 0.0;

    void reset(int bb_p, double bb_sd) {
        bb_period     = bb_p;
        bb_stdev_mult = bb_sd;
        close_buf.clear();
        bb_upper = bb_mid = bb_lower = 0.0;
        bb_primed = false;

        atr_primed = false;
        atr14 = atr_avg = 0.0;
        atr_seed.clear();

        rsi_primed = false;
        rsi14 = 50.0;
        rsi_avg_gain = rsi_avg_loss = 0.0;
        rsi_seed_gain.clear();
        rsi_seed_loss.clear();

        have_prev_close = false;
        prev_close = 0.0;
    }

    void on_m1_close(double open, double high, double low, double close) {
        (void)open;
        // ---- Bollinger ----
        close_buf.push_back(close);
        while ((int)close_buf.size() > bb_period) close_buf.pop_front();
        if ((int)close_buf.size() >= bb_period) {
            double sum = 0.0;
            for (double v : close_buf) sum += v;
            const double mid = sum / bb_period;
            double sq = 0.0;
            for (double v : close_buf) { const double d = v - mid; sq += d * d; }
            const double sigma = std::sqrt(sq / bb_period);
            bb_mid   = mid;
            bb_upper = mid + bb_stdev_mult * sigma;
            bb_lower = mid - bb_stdev_mult * sigma;
            bb_primed = true;
        }

        // ---- ATR (Wilder, 14) ----
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

        // ---- RSI (Wilder, 14) ----
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
// Driver: streams ticks, builds M1 bars, runs engine
// -----------------------------------------------------------------------------
struct Driver {
    omega::BBandScalpEngine engine;
    M1Indicators            ind;

    // M1 bar state
    int64_t cur_anchor_min = -1;
    double  cur_open       = 0.0;
    double  cur_high       = 0.0;
    double  cur_low        = 0.0;
    double  cur_close      = 0.0;

    // Trade log + running PnL
    std::vector<omega::TradeRecord> trades;
    double cum_pnl  = 0.0;
    double peak_pnl = 0.0;
    double max_dd   = 0.0;

    Driver() {
        engine.on_close_cb = [this](const omega::TradeRecord& tr) {
            trades.push_back(tr);
            // S99h fix: engine reports tr.pnl in pts*lots. Harness multiplies
            // by XAUUSD tick value (100) to track USD equity curve / max DD.
            constexpr double XAUUSD_TICK_VALUE = 100.0;
            const double pnl_usd = tr.pnl * XAUUSD_TICK_VALUE;
            cum_pnl += pnl_usd;
            if (cum_pnl > peak_pnl) peak_pnl = cum_pnl;
            double dd = peak_pnl - cum_pnl;
            if (dd > max_dd) max_dd = dd;
        };
    }

    void reset(int bb_p, double bb_sd) {
        engine.m_pos = decltype(engine.m_pos){};
        ind.reset(bb_p, bb_sd);
        cur_anchor_min = -1;
        cur_open = cur_high = cur_low = cur_close = 0.0;
        trades.clear();
        cum_pnl = 0.0;
        peak_pnl = 0.0;
        max_dd = 0.0;
    }

    void on_tick(int64_t ts_ms, double bid, double ask) {
        const double mid = (bid + ask) * 0.5;

        // ---- Build M1 bars (UTC-aligned, ts_ms / 60000) ----
        const int64_t ts_min = (ts_ms / 1000) / 60;
        if (cur_anchor_min < 0) {
            cur_anchor_min = ts_min;
            cur_open = cur_high = cur_low = cur_close = mid;
        } else if (ts_min != cur_anchor_min) {
            // Close prior M1 bar -> update indicators
            ind.on_m1_close(cur_open, cur_high, cur_low, cur_close);
            // Open new bar
            cur_anchor_min = ts_min;
            cur_open = cur_high = cur_low = cur_close = mid;
        } else {
            if (mid > cur_high) cur_high = mid;
            if (mid < cur_low)  cur_low  = mid;
            cur_close = mid;
        }

        // ---- Drive engine ----
        // Engine sees indicator values that are stable across all ticks of
        // the current M1 bar (set at the *previous* bar's close). This
        // matches the live model exactly: g_bars_gold.m1.ind.bb_* atomics
        // are updated only on bar close.
        engine.on_tick(bid, ask, ts_ms,
                       /*can_enter=*/true,
                       ind.bb_upper, ind.bb_mid, ind.bb_lower,
                       ind.rsi14, ind.atr14,
                       /*l2_imbalance=*/0.5, /*l2_real=*/false);
    }
};

// -----------------------------------------------------------------------------
// CSV streamer (Dukascopy combined: timestamp_ms,ask,bid)
// -----------------------------------------------------------------------------
static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[BBS-BT] FAIL: cannot open %s\n", path);
        return -1;
    }

    std::string line;
    int64_t n_ticks   = 0;
    int64_t n_skipped = 0;

    // Skip header if first line is not numeric
    if (std::getline(f, line)) {
        if (!line.empty() && !std::isdigit((unsigned char)line[0])) {
            // header, drop
        } else {
            f.clear();
            f.seekg(0);
        }
    }

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        char* p1;
        const int64_t ts_ms = std::strtoll(line.c_str(), &p1, 10);
        if (!p1 || *p1 != ',') { ++n_skipped; continue; }
        char* p2;
        const double ask = std::strtod(p1 + 1, &p2);
        if (!p2 || *p2 != ',') { ++n_skipped; continue; }
        char* p3;
        double bid = std::strtod(p2 + 1, &p3);
        (void)p3;

        if (!std::isfinite(ask) || !std::isfinite(bid)) { ++n_skipped; continue; }
        if (ask <= 0.0 || bid <= 0.0)                   { ++n_skipped; continue; }
        double a = ask, b = bid;
        if (b > a) std::swap(a, b);  // tape sanity
        const double spread = a - b;
        if (spread > 5.0) { ++n_skipped; continue; }  // anomaly filter

        drv.on_tick(ts_ms, b, a);
        ++n_ticks;

        if (n_ticks % 10000000LL == 0) {
            std::fprintf(stderr, "[BBS-BT] %lldM ticks (cfg P=%d K=%.2f RSI=%.0f/%.0f BE=%.2f SL=%.2f) "
                         "cum_pnl=%.2f n_trades=%zu\n",
                         (long long)(n_ticks / 1000000LL),
                         cfg.bb_period, cfg.bb_stdev_mult,
                         cfg.rsi_oversold, cfg.rsi_overbought,
                         cfg.be_arm_pts, cfg.sl_pts,
                         drv.cum_pnl, drv.trades.size());
        }
    }

    std::fprintf(stderr, "[BBS-BT] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

// -----------------------------------------------------------------------------
// Apply config to engine + indicator
// -----------------------------------------------------------------------------
static void apply_config(omega::BBandScalpEngine& e, const Config& c) {
    e.RSI_OVERSOLD    = c.rsi_oversold;
    e.RSI_OVERBOUGHT  = c.rsi_overbought;
    e.BE_ARM_PTS      = c.be_arm_pts;
    e.SL_PTS          = c.sl_pts;
    e.BE_BUFFER_PTS   = 0.05;
    e.TRAIL_TIGHT_PTS = 0.15;
    e.SPREAD_CAP_PTS  = 0.40;
    e.ATR_FLOOR_M1    = 0.50;
    e.ATR_CAP_M1      = 8.00;
    e.MAX_HOLD_SEC    = 600;
    e.COOLDOWN_SEC    = 60;
    e.LOT_BASE        = 0.01;
    e.COST_COVER_MULT = 1.0;
    e.shadow_mode     = true;
    e.enabled         = true;
}

// -----------------------------------------------------------------------------
// Summarise trades for a single config
// -----------------------------------------------------------------------------
static Result summarize(const Config& cfg,
                        const std::vector<omega::TradeRecord>& trades,
                        double max_dd)
{
    Result r;
    r.cfg      = cfg;
    r.max_dd   = max_dd;
    r.n_trades = (int)trades.size();

    // S99h fix (2026-05-18 part C): post-fix the engine reports tr.pnl in
    // pts*lots (production path multiplies by tick_value downstream). The
    // harness does not run trade_lifecycle, so multiply here to display USD.
    constexpr double XAUUSD_TICK_VALUE = 100.0;
    double gross_win = 0.0, gross_loss = 0.0;
    for (const auto& t : trades) {
        const double pnl_usd = t.pnl * XAUUSD_TICK_VALUE;
        r.gross_pnl += pnl_usd;
        if (pnl_usd > 0.0)      { ++r.n_wins;   gross_win  += pnl_usd; }
        else if (pnl_usd < 0.0) { ++r.n_losses; gross_loss += -pnl_usd; }

        if      (t.exitReason == "BE_SCRATCH") ++r.n_be_scratch;
        else if (t.exitReason == "TRAIL_WIN")  ++r.n_trail_win;
        else if (t.exitReason == "TP_HIT")     ++r.n_tp_hit;
        else if (t.exitReason == "SL_HIT")     ++r.n_sl_hit;
        else if (t.exitReason == "TIME_STOP")  ++r.n_time_stop;
    }
    r.wr_pct        = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    return r;
}

// -----------------------------------------------------------------------------
// Main: parameter sweep
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";

    std::fprintf(stderr, "[BBS-BT] CSV: %s\n", csv_path);

    // ---- Sweep grid -- 27 configs ---------------------------------------------
    // BB period:    {14, 20, 30}
    // BB stdev:     {1.8, 2.0, 2.5}
    // RSI threshold:{tight 30/70, medium 35/65, loose 40/60}
    // BE_ARM:       fixed 0.30 (proven cost-coverage threshold)
    // SL:           fixed 0.40 (structural)
    std::vector<Config> configs;
    const int    bb_period_grid[] = {14, 20, 30};
    const double bb_stdev_grid[]  = {1.8, 2.0, 2.5};
    struct RsiPair { double oversold; double overbought; };
    const RsiPair rsi_grid[]      = {{30.0, 70.0}, {35.0, 65.0}, {40.0, 60.0}};

    for (int p : bb_period_grid)
        for (double k : bb_stdev_grid)
            for (RsiPair r : rsi_grid)
                configs.push_back({p, k, r.oversold, r.overbought,
                                   /*be_arm_pts*/0.30, /*sl_pts*/0.40});

    std::printf("============================================================================\n");
    std::printf("  BBandScalpEngine sweep -- %zu configs\n", configs.size());
    std::printf("  CSV: %s\n", csv_path);
    std::printf("============================================================================\n\n");

    std::vector<Result> results;
    results.reserve(configs.size());

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr,
            "[BBS-BT] === Config %zu/%zu: P=%d K=%.2f RSI=%.0f/%.0f BE=%.2f SL=%.2f ===\n",
            i + 1, configs.size(),
            cfg.bb_period, cfg.bb_stdev_mult,
            cfg.rsi_oversold, cfg.rsi_overbought,
            cfg.be_arm_pts, cfg.sl_pts);

        Driver drv;
        drv.reset(cfg.bb_period, cfg.bb_stdev_mult);
        apply_config(drv.engine, cfg);

        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n_ticks = parse_csv_and_run(drv, csv_path, cfg);
        const auto t1 = std::chrono::steady_clock::now();
        if (n_ticks < 0) {
            std::fprintf(stderr, "[BBS-BT] CSV read failed, aborting\n");
            return 1;
        }
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);

        std::fprintf(stderr,
            "[BBS-BT] Config %zu done in %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f dd=$%.2f "
            "TP=%d SL=%d Trail=%d Scratch=%d Time=%d\n\n",
            i + 1, secs,
            r.n_trades, r.wr_pct, r.gross_pnl, r.profit_factor, r.max_dd,
            r.n_tp_hit, r.n_sl_hit, r.n_trail_win, r.n_be_scratch, r.n_time_stop);
    }

    // Sort by gross PnL descending
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) { return a.gross_pnl > b.gross_pnl; });

    std::printf("\n=================================================================================================\n");
    std::printf("  RESULTS (sorted by gross PnL)\n");
    std::printf("=================================================================================================\n");
    std::printf("%-4s %-6s %-5s %-5s %-6s %-5s %-7s %-12s %-7s %-9s %-5s %-5s %-7s %-7s %-5s\n",
                "P", "K", "RSIo", "RSIb", "BE", "SL",
                "N", "PnL$", "PF", "DD$", "TP", "SL", "Trail", "Scratch", "Time");
    std::printf("-------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-4d %-6.2f %-5.0f %-5.0f %-6.2f %-5.2f %-7d %-+12.2f %-7.2f %-9.2f %-5d %-5d %-7d %-7d %-5d\n",
                    r.cfg.bb_period, r.cfg.bb_stdev_mult,
                    r.cfg.rsi_oversold, r.cfg.rsi_overbought,
                    r.cfg.be_arm_pts, r.cfg.sl_pts,
                    r.n_trades, r.gross_pnl, r.profit_factor, r.max_dd,
                    r.n_tp_hit, r.n_sl_hit, r.n_trail_win, r.n_be_scratch, r.n_time_stop);
    }
    std::printf("=================================================================================================\n");

    if (!results.empty()) {
        const auto& best = results.front();
        std::printf("\nBest config:\n");
        std::printf("  BB_PERIOD      = %d\n", best.cfg.bb_period);
        std::printf("  BB_STDEV_MULT  = %.2f\n", best.cfg.bb_stdev_mult);
        std::printf("  RSI_OVERSOLD   = %.0f\n", best.cfg.rsi_oversold);
        std::printf("  RSI_OVERBOUGHT = %.0f\n", best.cfg.rsi_overbought);
        std::printf("  BE_ARM_PTS     = %.2f\n", best.cfg.be_arm_pts);
        std::printf("  SL_PTS         = %.2f\n", best.cfg.sl_pts);
        std::printf("  N=%d  WR=%.1f%%  PnL=$%+.2f  PF=%.2f  DD=$%.2f\n",
                    best.n_trades, best.wr_pct, best.gross_pnl,
                    best.profit_factor, best.max_dd);
        std::printf("  Exit mix: TP=%d SL=%d Trail=%d Scratch=%d Time=%d\n",
                    best.n_tp_hit, best.n_sl_hit,
                    best.n_trail_win, best.n_be_scratch, best.n_time_stop);
    }

    return 0;
}
