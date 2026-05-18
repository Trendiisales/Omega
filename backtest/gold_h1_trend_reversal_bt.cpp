// =============================================================================
// gold_h1_trend_reversal_bt.cpp -- GoldH1TrendReversalEngine sweep harness
// =============================================================================
//
// V1 sweep (gold_reversal_scalp_bt.cpp) showed tick-direction reversal
// detector over-fires on bid-ask flutter, cutting winners early (avgWin
// dropped from $5.50 to $2.80 vs GSP baseline). V2 uses M1 bar-close
// direction signs instead -- ~60-3000x smoother signal.
//
// Build:  clang++ -O3 -std=c++17 -Iinclude -o backtest/gold_h1_trend_reversal_bt \
//                  backtest/gold_h1_trend_reversal_bt.cpp
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <algorithm>

#include "../include/GoldH1TrendReversalEngine.hpp"

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
    double gross_pnl_usd   = 0.0;
    double max_dd_usd      = 0.0;
    double profit_factor   = 0.0;
    double wr_pct          = 0.0;
    double avg_win         = 0.0;
    double avg_loss        = 0.0;
};

struct Driver {
    omega::GoldH1TrendReversalEngine engine;
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
        cum_pnl_usd = 0.0; peak_pnl = 0.0; max_dd = 0.0;
    }

    void on_tick(int64_t ts_ms, double bid, double ask) {
        engine.on_tick(bid, ask, ts_ms, true,
                       0.5, 0.0, false, false, false, false, false,
                       nullptr);
    }
};

static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[H1T-SWEEP] FAIL: cannot open %s\n", path);
        return -1;
    }

    std::string line;
    int64_t n_ticks = 0, n_skipped = 0;

    if (std::getline(f, line)) {
        if (!line.empty() && !std::isdigit((unsigned char)line[0])) {
        } else { f.clear(); f.seekg(0); }
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
                "[H1T-SWEEP] %lldM ticks (%s) cum_pnl=$%.2f n_trades=%zu\n",
                (long long)(n_ticks / 1000000LL),
                cfg.label, drv.cum_pnl_usd, drv.trades.size());
        }
    }
    std::fprintf(stderr, "[H1T-SWEEP] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

static void apply_config(omega::GoldH1TrendReversalEngine& e, const Config& c) {
    e.LOOKBACK              = 24;     // 24h Donchian on H1
    e.SL_ATR_MULT           = 1.5;
    e.TP_ATR_MULT           = 3.0;
    e.COST_COVER_PTS        = 0.50;
    e.BE_BUFFER_PTS         = 0.10;
    // 2026-05-19 part-C: disable ratchet so the BE-lock SL stays at
    // entry+BE_BUFFER and the TREND_FLIP_EXIT becomes the dominant exit.
    // Prior run showed ratchet wins race vs trend-flip (1519 TRAIL_HIT,
    // 0 TREND_FLIP_EXIT). With ratchet disabled, the trade holds until
    // either BE-lock SL is hit (retracement), TP, trend-flip, or time-stop.
    e.TRAIL_DIST            = 99.00;
    e.REVERSAL_ADVERSE_GATE = 0.50;
    e.REVERSAL_M1_WINDOW    = c.window;
    e.REVERSAL_M1_THRESHOLD = c.threshold;
    e.shadow_mode = true;
    e.enabled = true;
}

static Result summarize(const Config& cfg,
                        const std::vector<omega::TradeRecord>& trades,
                        double max_dd_usd) {
    constexpr double XAUUSD_TICK_VALUE = 100.0;
    Result r; r.cfg = cfg; r.max_dd_usd = max_dd_usd; r.n_trades = (int)trades.size();
    double gross_win = 0.0, gross_loss = 0.0;
    for (const auto& t : trades) {
        const double pnl_usd = t.pnl * XAUUSD_TICK_VALUE;
        r.gross_pnl_usd += pnl_usd;
        if (pnl_usd > 0.0)      { ++r.n_wins;   gross_win  += pnl_usd; }
        else if (pnl_usd < 0.0) { ++r.n_losses; gross_loss += -pnl_usd; }
        if      (t.exitReason == "TRAIL_HIT")     ++r.n_trail_hit;
        else if (t.exitReason == "TP_HIT")        ++r.n_tp_hit;
        else if (t.exitReason == "SL_HIT")        ++r.n_sl_hit;
        else if (t.exitReason == "TREND_FLIP_EXIT") ++r.n_reversal_exit;
        else if (t.exitReason == "TIME_STOP")     ++r.n_time_stop;
    }
    r.wr_pct = (r.n_trades > 0) ? (100.0 * r.n_wins / r.n_trades) : 0.0;
    r.profit_factor = (gross_loss > 1e-9) ? (gross_win / gross_loss) : 0.0;
    r.avg_win = (r.n_wins > 0) ? (gross_win / r.n_wins) : 0.0;
    r.avg_loss = (r.n_losses > 0) ? (-gross_loss / r.n_losses) : 0.0;
    return r;
}

int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";
    std::fprintf(stderr, "[H1T-SWEEP] CSV: %s\n", csv_path);

    const int    windows[]    = { 5, 8 };
    const double thresholds[] = { 0.65, 0.75 };

    std::vector<Config> configs;
    for (int w : windows) for (double th : thresholds) {
        Config c{}; c.window = w; c.threshold = th;
        std::snprintf(c.label, sizeof(c.label), "m1w=%d/th=%.2f", w, th);
        configs.push_back(c);
    }

    std::printf("===================================================================================\n");
    std::printf("  GoldH1TrendReversal sweep -- M1-bar reversal detector\n");
    std::printf("  Entry: GSP best (LB=8, SL=2.0xATR, TP=3.0xATR)\n");
    std::printf("  Exit:  cost-cover BE (0.50pts) + tight trail (0.30pts) + M1-close reversal\n");
    std::printf("  Sweep: REVERSAL_M1_WINDOW {5,8} x REVERSAL_M1_THRESHOLD {0.65,0.75}\n");
    std::printf("===================================================================================\n\n");

    std::vector<Result> results;
    const auto t_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr, "[H1T-SWEEP] === Cfg %zu/%zu: %s ===\n", i+1, configs.size(), cfg.label);
        Driver drv; drv.reset(); apply_config(drv.engine, cfg);
        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n_ticks = parse_csv_and_run(drv, csv_path, cfg);
        const auto t1 = std::chrono::steady_clock::now();
        if (n_ticks < 0) return 1;
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);
        std::fprintf(stderr,
            "[H1T-SWEEP] Cfg %zu (%s) done %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f rev=%d sl=%d tp=%d\n\n",
            i+1, cfg.label, secs, r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor,
            r.n_reversal_exit, r.n_sl_hit, r.n_tp_hit);
    }

    const auto t_end = std::chrono::steady_clock::now();
    const double total_secs = std::chrono::duration<double>(t_end - t_start).count();

    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) { return a.gross_pnl_usd > b.gross_pnl_usd; });

    std::printf("\n=================================================================================================================================\n");
    std::printf("  H1T RESULTS (sorted by PnL desc) -- runtime %.1f min\n", total_secs/60.0);
    std::printf("=================================================================================================================================\n");
    std::printf("%-18s %-7s %-6s %-12s %-6s %-9s %-9s %-9s %-5s %-5s %-5s %-5s %-5s\n",
                "Config","N","WR%","PnL$","PF","DD$","AvgWin","AvgLoss","REV","TR","TP","SL","TS");
    std::printf("---------------------------------------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-18s %-7d %-6.1f %-+12.2f %-6.2f %-9.2f %+9.2f %+9.2f %-5d %-5d %-5d %-5d %-5d\n",
            r.cfg.label, r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor, r.max_dd_usd,
            r.avg_win, r.avg_loss,
            r.n_reversal_exit, r.n_trail_hit, r.n_tp_hit, r.n_sl_hit, r.n_time_stop);
    }
    std::printf("=================================================================================================================================\n\n");

    const double PNL_BAR = 5000.0, PF_BAR = 1.20;
    const Result* best_pass = nullptr;
    for (const auto& r : results) {
        if (r.gross_pnl_usd > PNL_BAR && r.profit_factor > PF_BAR) {
            if (!best_pass || r.gross_pnl_usd > best_pass->gross_pnl_usd) best_pass = &r;
        }
    }
    std::printf("VERDICT (criterion: PnL > $%.0f AND PF > %.2f):\n", PNL_BAR, PF_BAR);
    if (best_pass) {
        std::printf("  -> PASS. Best: %s PnL=$%.2f PF=%.2f WR=%.1f%% N=%d DD=$%.2f\n",
                    best_pass->cfg.label, best_pass->gross_pnl_usd, best_pass->profit_factor,
                    best_pass->wr_pct, best_pass->n_trades, best_pass->max_dd_usd);
    } else {
        const Result* top = results.empty() ? nullptr : &results.front();
        std::printf("  -> FAIL. ");
        if (top) std::printf("Top: %s PnL=$%.2f PF=%.2f WR=%.1f%% avgWin=$%.2f avgLoss=$%.2f\n",
                             top->cfg.label, top->gross_pnl_usd, top->profit_factor,
                             top->wr_pct, top->avg_win, top->avg_loss);
    }
    std::printf("\n");
    return 0;
}
