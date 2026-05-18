// =============================================================================
// gsp_exit_variants_bt.cpp -- GSP exit-philosophy variants sweep
// =============================================================================
//
// Build (Mac/Linux):
//   clang++ -O3 -std=c++17 -o backtest/gsp_exit_variants_bt \
//           backtest/gsp_exit_variants_bt.cpp
//
// Run:
//   ./backtest/gsp_exit_variants_bt /path/to/XAUUSD_combined.csv
//   or
//   ./backtest/gsp_exit_variants_bt   (default = Dukascopy combined)
//
// PURPOSE (next-session focus from SESSION_HANDOFF_2026-05-18b):
//   The 2026-05-18b session proved the validated GSP $15K backtest is a
//   bar-level-harness artifact. Class-direct tick-level audit reproduces
//   -$16,935 / PF 0.62 / 0 TP_HITs on the same tape. The trail fires intra-
//   bar at tick level when at bar level it would only fire at bar close.
//
//   This harness drives the GoldScalpPyramidEngine class directly and sweeps
//   exit philosophy × trail tightness to find if ANY variant recovers $5-8K
//   of tradeable edge in tick-level execution.
//
//   12 configs:
//     trail_tight     in {0.12, 0.20, 0.30, 0.40}     -- 4 levels
//     exit_philosophy in {TICK, BAR_CLOSE, GIVE_BACK} -- 3 levels
//
//   All other parameters held at GSP best-config:
//     LOOKBACK = 8, SL_ATR_MULT = 1.5, TP_ATR_MULT = 3.0,
//     PYRAMID_ON = true,
//     LOSS_CUT_PCT = 0, BE_ARM_PCT = 0, BE_BUFFER_PCT = 0  (S63 OFF per S100c)
//
// SUCCESS CRITERION:
//   Any single config produces PnL > +$5,000 AND PF > 1.20 on the full tape.
//   If yes -> propose wiring that variant as a new shadow-mode engine.
//   If no  -> trail-based exits are tick-fragile for gold M5; pivot to
//             fixed-RR (TP at 2R/3R, hard SL, no trail).
//
// COST CONVENTION (S99h):
//   GSP class reports tr.pnl in pts*lots. This harness multiplies by
//   tick_value=100 at summarize() time to display USD (production scaling).
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
#include <chrono>
#include <algorithm>
#include <functional>

#include "../include/GoldScalpPyramidEngine.hpp"

// -----------------------------------------------------------------------------
// Sweep config: trail_tight × exit_philosophy
// -----------------------------------------------------------------------------
struct Config {
    double                  trail_tight;
    omega::ExitPhilosophy   philosophy;
    const char*             phil_label;
    char                    label[40];
};

static const char* phil_str(omega::ExitPhilosophy p) {
    switch (p) {
        case omega::ExitPhilosophy::TICK_LEVEL:     return "TICK";
        case omega::ExitPhilosophy::BAR_CLOSE_ONLY: return "BAR_CLOSE";
        case omega::ExitPhilosophy::GIVE_BACK:      return "GIVE_BACK";
    }
    return "?";
}

// -----------------------------------------------------------------------------
// Per-config result
// -----------------------------------------------------------------------------
struct Result {
    Config cfg;
    int    n_trades       = 0;
    int    n_wins         = 0;
    int    n_losses       = 0;
    int    n_trail_hit    = 0;
    int    n_tp_hit       = 0;
    int    n_sl_hit       = 0;
    int    n_giveback_hit = 0;
    int    n_time_stop    = 0;
    int    n_pyramid      = 0;
    double gross_pnl_usd  = 0.0;
    double max_dd_usd     = 0.0;
    double profit_factor  = 0.0;
    double wr_pct         = 0.0;
    double avg_win        = 0.0;
    double avg_loss       = 0.0;
};

// -----------------------------------------------------------------------------
// Driver: streams ticks into the GSP class, accumulates trades
// -----------------------------------------------------------------------------
struct Driver {
    omega::GoldScalpPyramidEngine engine;

    std::vector<omega::TradeRecord> trades;
    double cum_pnl_usd = 0.0;
    double peak_pnl    = 0.0;
    double max_dd      = 0.0;

    static constexpr double XAUUSD_TICK_VALUE = 100.0;

    Driver() {
        engine.on_close_cb = [this](const omega::TradeRecord& tr) {
            trades.push_back(tr);
            // S99h convention: tr.pnl is pts*lots; production scales by
            // tick_value=100 in trade_lifecycle.hpp. This harness does the
            // same here since trade_lifecycle isn't called.
            const double pnl_usd = tr.pnl * XAUUSD_TICK_VALUE;
            cum_pnl_usd += pnl_usd;
            if (cum_pnl_usd > peak_pnl) peak_pnl = cum_pnl_usd;
            const double dd = peak_pnl - cum_pnl_usd;
            if (dd > max_dd) max_dd = dd;
        };
    }

    void reset() {
        engine.m_pos = decltype(engine.m_pos){};
        trades.clear();
        cum_pnl_usd = 0.0;
        peak_pnl    = 0.0;
        max_dd      = 0.0;
    }

    void on_tick(int64_t ts_ms, double bid, double ask) {
        // Feed stub L2 (l2_real=false -> all L2 filters degrade to neutral,
        // matching the Dukascopy no-DOM environment).
        engine.on_tick(bid, ask, ts_ms,
                       /*can_enter=*/true,
                       /*l2_imbalance=*/0.5,
                       /*book_slope=*/0.0,
                       /*vacuum_ask=*/false,
                       /*vacuum_bid=*/false,
                       /*wall_above=*/false,
                       /*wall_below=*/false,
                       /*l2_real=*/false,
                       /*ext_close=*/nullptr);
    }
};

// -----------------------------------------------------------------------------
// CSV streamer
// -----------------------------------------------------------------------------
static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[GSP-EXIT] FAIL: cannot open %s\n", path);
        return -1;
    }

    std::string line;
    int64_t n_ticks = 0, n_skipped = 0;

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
        if (ask <= 0.0 || bid <= 0.0) { ++n_skipped; continue; }
        double a = ask, b = bid;
        if (b > a) std::swap(a, b);
        const double spread = a - b;
        if (spread > 5.0) { ++n_skipped; continue; }

        drv.on_tick(ts_ms, b, a);
        ++n_ticks;

        if (n_ticks % 20000000LL == 0) {
            std::fprintf(stderr,
                "[GSP-EXIT] %lldM ticks (%s) cum_pnl=$%.2f n_trades=%zu\n",
                (long long)(n_ticks / 1000000LL),
                cfg.label, drv.cum_pnl_usd, drv.trades.size());
        }
    }

    std::fprintf(stderr, "[GSP-EXIT] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

// -----------------------------------------------------------------------------
// Apply config -- holds GSP best-config core constant, varies trail+philosophy
// -----------------------------------------------------------------------------
static void apply_config(omega::GoldScalpPyramidEngine& e, const Config& c) {
    // ---- GSP best-config core (held constant across all configs) ----
    e.LOOKBACK     = 8;
    e.SL_ATR_MULT  = 1.5;
    e.TP_ATR_MULT  = 3.0;
    e.PYRAMID_ON   = true;

    // ---- S63 OFF per S100c (LOSS_CUT/BE_ARM/BE_BUFFER held at 0) ----
    e.LOSS_CUT_PCT  = 0.0;
    e.BE_ARM_PCT    = 0.0;
    e.BE_BUFFER_PCT = 0.0;

    // ---- The variables under test ----
    e.TRAIL_TIGHT     = c.trail_tight;
    e.exit_philosophy = c.philosophy;

    // ---- Shadow + enable (harness only) ----
    e.shadow_mode = true;
    e.enabled     = true;
}

// -----------------------------------------------------------------------------
// Summarise
// -----------------------------------------------------------------------------
static Result summarize(const Config& cfg,
                        const std::vector<omega::TradeRecord>& trades,
                        double max_dd_usd)
{
    constexpr double XAUUSD_TICK_VALUE = 100.0;

    Result r;
    r.cfg        = cfg;
    r.max_dd_usd = max_dd_usd;
    r.n_trades   = (int)trades.size();

    double gross_win = 0.0, gross_loss = 0.0;
    for (const auto& t : trades) {
        const double pnl_usd = t.pnl * XAUUSD_TICK_VALUE;
        r.gross_pnl_usd += pnl_usd;
        if (pnl_usd > 0.0)      { ++r.n_wins;   gross_win  += pnl_usd; }
        else if (pnl_usd < 0.0) { ++r.n_losses; gross_loss += -pnl_usd; }

        if      (t.exitReason == "TRAIL_HIT")    ++r.n_trail_hit;
        else if (t.exitReason == "TP_HIT")       ++r.n_tp_hit;
        else if (t.exitReason == "SL_HIT")       ++r.n_sl_hit;
        else if (t.exitReason == "GIVEBACK_HIT") ++r.n_giveback_hit;
        else if (t.exitReason == "TIME_STOP")    ++r.n_time_stop;

        if (t.size > 0.051) ++r.n_pyramid;
    }
    r.wr_pct        = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    r.avg_win       = (r.n_wins   > 0) ? (gross_win  / r.n_wins)   : 0.0;
    r.avg_loss      = (r.n_losses > 0) ? (-gross_loss / r.n_losses) : 0.0;
    return r;
}

// -----------------------------------------------------------------------------
// Main: build 12-config grid, run each on full tape, print sorted summary
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";

    std::fprintf(stderr, "[GSP-EXIT] CSV: %s\n", csv_path);

    // Build 12-config grid: trail_tight {0.12, 0.20, 0.30, 0.40}
    //                    × philosophy   {TICK, BAR_CLOSE, GIVE_BACK}
    const double          trails[] = { 0.12, 0.20, 0.30, 0.40 };
    const omega::ExitPhilosophy phils[] = {
        omega::ExitPhilosophy::TICK_LEVEL,
        omega::ExitPhilosophy::BAR_CLOSE_ONLY,
        omega::ExitPhilosophy::GIVE_BACK,
    };

    std::vector<Config> configs;
    configs.reserve(12);
    for (double tt : trails) {
        for (omega::ExitPhilosophy ph : phils) {
            Config c{};
            c.trail_tight = tt;
            c.philosophy  = ph;
            c.phil_label  = phil_str(ph);
            std::snprintf(c.label, sizeof(c.label), "t=%.2f/%s", tt, phil_str(ph));
            configs.push_back(c);
        }
    }

    std::printf("===================================================================================\n");
    std::printf("  GSP Exit-Philosophy Variants Sweep\n");
    std::printf("  Core held at GSP best: LB=8 SL=1.5 TP=3.0 Pyr=Y S63=OFF\n");
    std::printf("  Sweep: trail_tight {0.12, 0.20, 0.30, 0.40} x philosophy {TICK, BAR_CLOSE, GIVE_BACK}\n");
    std::printf("  CSV: %s\n", csv_path);
    std::printf("===================================================================================\n\n");

    std::vector<Result> results;
    results.reserve(configs.size());

    const auto t_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr,
            "[GSP-EXIT] === Cfg %zu/%zu: %s trail=%.2f phil=%s ===\n",
            i + 1, configs.size(),
            cfg.label, cfg.trail_tight, cfg.phil_label);

        Driver drv;
        drv.reset();
        apply_config(drv.engine, cfg);

        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n_ticks = parse_csv_and_run(drv, csv_path, cfg);
        const auto t1 = std::chrono::steady_clock::now();
        if (n_ticks < 0) return 1;
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);

        std::fprintf(stderr,
            "[GSP-EXIT] Cfg %zu (%s) done in %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f dd=$%.2f\n\n",
            i + 1, cfg.label, secs,
            r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor, r.max_dd_usd);
    }

    const auto t_end = std::chrono::steady_clock::now();
    const double total_secs = std::chrono::duration<double>(t_end - t_start).count();

    // -------------------------------------------------------------------------
    // Sort by gross PnL descending and print
    // -------------------------------------------------------------------------
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) {
                  return a.gross_pnl_usd > b.gross_pnl_usd;
              });

    std::printf("\n=================================================================================================================================\n");
    std::printf("  RESULTS (sorted by gross PnL, desc)  -- total runtime %.1f min\n", total_secs / 60.0);
    std::printf("=================================================================================================================================\n");
    std::printf("%-22s %-7s %-6s %-12s %-6s %-9s %-9s %-9s %-5s %-5s %-5s %-5s %-5s %-5s\n",
                "Config", "N", "WR%", "PnL$", "PF", "DD$",
                "AvgWin", "AvgLoss",
                "TR", "TP", "SL", "GB", "TS", "Pyr");
    std::printf("---------------------------------------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-22s %-7d %-6.1f %-+12.2f %-6.2f %-9.2f %+9.2f %+9.2f %-5d %-5d %-5d %-5d %-5d %-5d\n",
                    r.cfg.label,
                    r.n_trades, r.wr_pct, r.gross_pnl_usd,
                    r.profit_factor, r.max_dd_usd,
                    r.avg_win, r.avg_loss,
                    r.n_trail_hit, r.n_tp_hit, r.n_sl_hit,
                    r.n_giveback_hit, r.n_time_stop, r.n_pyramid);
    }
    std::printf("=================================================================================================================================\n");
    std::printf("  Exit-mix legend: TR=TRAIL_HIT TP=TP_HIT SL=SL_HIT GB=GIVEBACK_HIT TS=TIME_STOP Pyr=trades_with_pyramid_layers\n\n");

    // -------------------------------------------------------------------------
    // Success-criterion verdict
    // -------------------------------------------------------------------------
    const double PNL_BAR = 5000.0;
    const double PF_BAR  = 1.20;

    const Result* best_pass = nullptr;
    for (const auto& r : results) {
        if (r.gross_pnl_usd > PNL_BAR && r.profit_factor > PF_BAR) {
            if (!best_pass || r.gross_pnl_usd > best_pass->gross_pnl_usd) {
                best_pass = &r;
            }
        }
    }

    std::printf("VERDICT (criterion: PnL > $%.0f AND PF > %.2f):\n", PNL_BAR, PF_BAR);
    if (best_pass) {
        std::printf("  -> PASS. Best config: %s  PnL=$%.2f  PF=%.2f  WR=%.1f%%  N=%d  DD=$%.2f\n",
                    best_pass->cfg.label,
                    best_pass->gross_pnl_usd,
                    best_pass->profit_factor,
                    best_pass->wr_pct,
                    best_pass->n_trades,
                    best_pass->max_dd_usd);
        std::printf("     Action: propose new shadow-mode engine wired with this variant\n");
        std::printf("     (enabled=false until operator approves).\n");
    } else {
        // Find best PnL anyway for context
        const Result* best_pnl = results.empty() ? nullptr : &results.front();
        std::printf("  -> FAIL. No config met the bar. ");
        if (best_pnl) {
            std::printf("Top: %s  PnL=$%.2f  PF=%.2f\n",
                        best_pnl->cfg.label,
                        best_pnl->gross_pnl_usd,
                        best_pnl->profit_factor);
        } else {
            std::printf("\n");
        }
        std::printf("     Action: abandon trail-based exits for gold M5; pivot to\n");
        std::printf("     fixed-RR (TP at 2R/3R, hard SL, no trail) as next research direction.\n");
    }
    std::printf("\n");

    return 0;
}
