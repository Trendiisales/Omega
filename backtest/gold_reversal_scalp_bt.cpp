// =============================================================================
// gold_reversal_scalp_bt.cpp -- GoldReversalScalpEngine sweep harness
// =============================================================================
//
// Build:
//   clang++ -O3 -std=c++17 -o backtest/gold_reversal_scalp_bt \
//           backtest/gold_reversal_scalp_bt.cpp
//
// Run:
//   ./backtest/gold_reversal_scalp_bt [csv_path]
//
// PURPOSE (2026-05-19 part-B):
//   S101 (part-A) ruled out trail philosophy as the GSP edge bottleneck.
//   The structural deficit was avgWin ~$5.50 vs avgLoss ~$21.78 -- the
//   loss tail was hitting hard SL at 1.5xATR. This engine tests whether
//   a tick-imbalance reversal detector can:
//     a) shrink avgLoss by exiting before the hard SL fires on losers
//     b) lock profit by exiting on winners that begin to reverse
//
//   Same entry stack as GSP best (M5 Donchian + EMA trend + momentum +
//   session + ATR floor/cap). Exit stack replaced with:
//     - Hard SL at 2.0xATR (wider than GSP's 1.5xATR; reversal-detect
//       is expected to catch losers first)
//     - Hard TP at 3.0xATR (unchanged)
//     - Cost-cover BE arm at MFE >= 0.50 pts
//     - Tight trail at TRAIL_DIST pts behind mfe_price (post-BE)
//     - Tick-imbalance reversal detector with gate
//
// SWEEP (6 configs):
//     REVERSAL_WINDOW    in {30, 60, 100}
//     REVERSAL_THRESHOLD in {0.60, 0.68}
//   Other params fixed: COST_COVER=0.50, BE_BUFFER=0.10, TRAIL_DIST=0.30,
//                       SL=2.0, TP=3.0, GATE=0.50
//
// SUCCESS:
//   PnL > +$5,000 AND PF > 1.20 on the full tape.
//   If yes -> propose new shadow-mode engine (enabled=false) for operator.
//   If no  -> honest null result + pivot. The 71% WR entry shape may
//             simply not carry tradable edge on gold M5.
//
// COST CONVENTION (S99h):
//   tr.pnl in pts*lots; summarize() multiplies by tick_value=100 -> USD.
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

#include "../include/GoldReversalScalpEngine.hpp"

struct Config {
    int    window;
    double threshold;
    char   label[40];
};

struct Result {
    Config cfg;
    int    n_trades        = 0;
    int    n_wins          = 0;
    int    n_losses        = 0;
    int    n_trail_hit     = 0;
    int    n_tp_hit        = 0;
    int    n_sl_hit        = 0;
    int    n_reversal_exit = 0;
    int    n_time_stop     = 0;
    int    n_be_armed      = 0;
    double gross_pnl_usd   = 0.0;
    double max_dd_usd      = 0.0;
    double profit_factor   = 0.0;
    double wr_pct          = 0.0;
    double avg_win         = 0.0;
    double avg_loss        = 0.0;
};

struct Driver {
    omega::GoldReversalScalpEngine engine;

    std::vector<omega::TradeRecord> trades;
    double cum_pnl_usd = 0.0;
    double peak_pnl    = 0.0;
    double max_dd      = 0.0;

    static constexpr double XAUUSD_TICK_VALUE = 100.0;

    Driver() {
        engine.on_close_cb = [this](const omega::TradeRecord& tr) {
            trades.push_back(tr);
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
        engine.on_tick(bid, ask, ts_ms,
                       /*can_enter=*/true,
                       /*l2_imbalance=*/0.5, /*book_slope=*/0.0,
                       /*vacuum_ask=*/false, /*vacuum_bid=*/false,
                       /*wall_above=*/false, /*wall_below=*/false,
                       /*l2_real=*/false,
                       /*ext_close=*/nullptr);
    }
};

static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[GRS-SWEEP] FAIL: cannot open %s\n", path);
        return -1;
    }

    std::string line;
    int64_t n_ticks = 0, n_skipped = 0;

    if (std::getline(f, line)) {
        if (!line.empty() && !std::isdigit((unsigned char)line[0])) {
            // header line, drop
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
                "[GRS-SWEEP] %lldM ticks (%s) cum_pnl=$%.2f n_trades=%zu\n",
                (long long)(n_ticks / 1000000LL),
                cfg.label, drv.cum_pnl_usd, drv.trades.size());
        }
    }

    std::fprintf(stderr, "[GRS-SWEEP] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

static void apply_config(omega::GoldReversalScalpEngine& e, const Config& c) {
    // ---- Entry stack (held constant -- GSP best-config shape) ----
    e.LOOKBACK    = 8;
    e.SL_ATR_MULT = 2.0;
    e.TP_ATR_MULT = 3.0;

    // ---- Exit stack defaults (held constant in this sweep) ----
    e.COST_COVER_PTS        = 0.50;
    e.BE_BUFFER_PTS         = 0.10;
    e.TRAIL_DIST            = 0.30;
    e.REVERSAL_ADVERSE_GATE = 0.50;

    // ---- The variables under test ----
    e.REVERSAL_WINDOW    = c.window;
    e.REVERSAL_THRESHOLD = c.threshold;

    e.shadow_mode = true;
    e.enabled     = true;
}

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

        if      (t.exitReason == "TRAIL_HIT")     ++r.n_trail_hit;
        else if (t.exitReason == "TP_HIT")        ++r.n_tp_hit;
        else if (t.exitReason == "SL_HIT")        ++r.n_sl_hit;
        else if (t.exitReason == "REVERSAL_EXIT") ++r.n_reversal_exit;
        else if (t.exitReason == "TIME_STOP")     ++r.n_time_stop;
    }
    r.wr_pct        = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    r.avg_win       = (r.n_wins   > 0) ? (gross_win  / r.n_wins)   : 0.0;
    r.avg_loss      = (r.n_losses > 0) ? (-gross_loss / r.n_losses) : 0.0;
    return r;
}

int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";

    std::fprintf(stderr, "[GRS-SWEEP] CSV: %s\n", csv_path);

    const int    windows[]    = { 30, 60, 100 };
    const double thresholds[] = { 0.60, 0.68 };

    std::vector<Config> configs;
    configs.reserve(6);
    for (int w : windows) {
        for (double th : thresholds) {
            Config c{};
            c.window    = w;
            c.threshold = th;
            std::snprintf(c.label, sizeof(c.label), "w=%d/th=%.2f", w, th);
            configs.push_back(c);
        }
    }

    std::printf("=====================================================================================\n");
    std::printf("  GoldReversalScalpEngine sweep\n");
    std::printf("  Entry: GSP best (LB=8, SL=2.0xATR, TP=3.0xATR, M5 Donchian+EMA+momentum+session)\n");
    std::printf("  Exit:  cost-cover BE (0.50pts) + tight trail (0.30pts) + tick-reversal detector\n");
    std::printf("  Sweep: REVERSAL_WINDOW {30,60,100} x REVERSAL_THRESHOLD {0.60,0.68}\n");
    std::printf("  CSV:   %s\n", csv_path);
    std::printf("=====================================================================================\n\n");

    std::vector<Result> results;
    results.reserve(configs.size());

    const auto t_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr,
            "[GRS-SWEEP] === Cfg %zu/%zu: %s ===\n",
            i + 1, configs.size(), cfg.label);

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
            "[GRS-SWEEP] Cfg %zu (%s) done in %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f dd=$%.2f rev=%d sl=%d tp=%d\n\n",
            i + 1, cfg.label, secs,
            r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor, r.max_dd_usd,
            r.n_reversal_exit, r.n_sl_hit, r.n_tp_hit);
    }

    const auto t_end = std::chrono::steady_clock::now();
    const double total_secs = std::chrono::duration<double>(t_end - t_start).count();

    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) {
                  return a.gross_pnl_usd > b.gross_pnl_usd;
              });

    std::printf("\n=================================================================================================================================\n");
    std::printf("  RESULTS (sorted by gross PnL, desc)  -- total runtime %.1f min\n", total_secs / 60.0);
    std::printf("=================================================================================================================================\n");
    std::printf("%-18s %-7s %-6s %-12s %-6s %-9s %-9s %-9s %-5s %-5s %-5s %-5s %-5s\n",
                "Config", "N", "WR%", "PnL$", "PF", "DD$",
                "AvgWin", "AvgLoss",
                "REV", "TR", "TP", "SL", "TS");
    std::printf("---------------------------------------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-18s %-7d %-6.1f %-+12.2f %-6.2f %-9.2f %+9.2f %+9.2f %-5d %-5d %-5d %-5d %-5d\n",
                    r.cfg.label,
                    r.n_trades, r.wr_pct, r.gross_pnl_usd,
                    r.profit_factor, r.max_dd_usd,
                    r.avg_win, r.avg_loss,
                    r.n_reversal_exit, r.n_trail_hit, r.n_tp_hit, r.n_sl_hit, r.n_time_stop);
    }
    std::printf("=================================================================================================================================\n");
    std::printf("  Exit-mix legend: REV=REVERSAL_EXIT TR=TRAIL_HIT TP=TP_HIT SL=SL_HIT TS=TIME_STOP\n\n");

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
        const Result* best_pnl = results.empty() ? nullptr : &results.front();
        std::printf("  -> FAIL. No config met the bar. ");
        if (best_pnl) {
            std::printf("Top: %s  PnL=$%.2f  PF=%.2f  WR=%.1f%%  avgWin=$%.2f  avgLoss=$%.2f\n",
                        best_pnl->cfg.label,
                        best_pnl->gross_pnl_usd,
                        best_pnl->profit_factor,
                        best_pnl->wr_pct,
                        best_pnl->avg_win,
                        best_pnl->avg_loss);
        } else {
            std::printf("\n");
        }
        std::printf("     Action: reversal-detect did not rescue the GSP entry-shape\n");
        std::printf("     -- pivot to varying the entry shape itself or shelve gold M5.\n");
    }
    std::printf("\n");

    return 0;
}
