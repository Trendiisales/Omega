// =============================================================================
// gold_fixed_rr_combined_bt.cpp -- alternative mechanism: hard SL + hard TP, no trail
// =============================================================================
// Build: clang++ -O3 -std=c++17 -Iinclude -o backtest/gold_fixed_rr_combined_bt backtest/gold_fixed_rr_combined_bt.cpp
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

#include "../include/GoldFixedRREngine.hpp"

struct Config {
    double sl_atr_mult;
    double rr_ratio;
    char   label[40];
};

struct Result {
    Config cfg;
    int    n_trades=0, n_wins=0, n_losses=0;
    int    n_tp_hit=0, n_sl_hit=0, n_time_stop=0;
    double gross_pnl_usd=0, max_dd_usd=0, profit_factor=0, wr_pct=0, avg_win=0, avg_loss=0;
};

struct Driver {
    omega::GoldFixedRREngine engine;
    std::vector<omega::TradeRecord> trades;
    double cum_pnl_usd=0, peak_pnl=0, max_dd=0;
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
    void reset() { engine.m_pos = decltype(engine.m_pos){}; trades.clear(); cum_pnl_usd=0; peak_pnl=0; max_dd=0; }
    void on_tick(int64_t ts_ms, double bid, double ask) {
        engine.on_tick(bid, ask, ts_ms, true, 0.5, 0.0, false,false,false,false,false, nullptr);
    }
};

static int64_t parse_csv_and_run(Driver& drv, const char* path, const Config& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    std::string line;
    int64_t n_ticks=0, n_skipped=0;
    if (std::getline(f, line)) {
        if (!line.empty() && !std::isdigit((unsigned char)line[0])) {}
        else { f.clear(); f.seekg(0); }
    }
    while (std::getline(f, line)) {
        if (line.empty()||line[0]=='#') continue;
        char *p1,*p2,*p3;
        const int64_t ts_ms = std::strtoll(line.c_str(), &p1, 10);
        if (!p1 || *p1 != ',') { ++n_skipped; continue; }
        const double ask = std::strtod(p1+1, &p2);
        if (!p2 || *p2 != ',') { ++n_skipped; continue; }
        double bid = std::strtod(p2+1, &p3); (void)p3;
        if (!std::isfinite(ask)||!std::isfinite(bid)||ask<=0||bid<=0) { ++n_skipped; continue; }
        if (ts_ms < 1735689600000LL) { ++n_skipped; continue; }  // 2025-01-01 cutoff
        double a=ask, b=bid; if (b>a) std::swap(a,b);
        const double spread = a-b;
        if (spread > 5.0) { ++n_skipped; continue; }
        drv.on_tick(ts_ms, b, a); ++n_ticks;
        if (n_ticks % 20000000LL == 0) {
            std::fprintf(stderr, "[GRR-SWEEP] %lldM ticks (%s) cum_pnl=$%.2f n=%zu\n",
                (long long)(n_ticks/1000000LL), cfg.label, drv.cum_pnl_usd, drv.trades.size());
        }
    }
    std::fprintf(stderr, "[GRR-SWEEP] Done: ticks=%lld skipped=%lld\n",
                 (long long)n_ticks, (long long)n_skipped);
    return n_ticks;
}

static void apply_config(omega::GoldFixedRREngine& e, const Config& c) {
    e.LOOKBACK            = 8;
    e.SL_ATR_MULT         = c.sl_atr_mult;
    e.RR_RATIO            = c.rr_ratio;
    e.LONG_ONLY           = true;       // bull-regime directional bias
    e.TIGHT_SESSION       = true;       // London open + NY open windows only
    e.ATR_FLOOR_OVERRIDE  = 2.0;        // skip very low-vol M5 bars
    e.VOL_EXPANSION       = true;       // require ATR > 1.3x median
    e.VOL_EXPANSION_MULT  = 1.3;        // looser than M5 vol-expansion to fire more
    e.shadow_mode         = true;
    e.enabled             = true;
}

static Result summarize(const Config& cfg, const std::vector<omega::TradeRecord>& trades, double max_dd_usd) {
    constexpr double TV = 100.0;
    Result r; r.cfg=cfg; r.max_dd_usd=max_dd_usd; r.n_trades=(int)trades.size();
    double gw=0, gl=0;
    for (const auto& t : trades) {
        const double pnl = t.pnl * TV;
        r.gross_pnl_usd += pnl;
        if (pnl > 0) { ++r.n_wins; gw += pnl; }
        else if (pnl < 0) { ++r.n_losses; gl += -pnl; }
        if (t.exitReason=="TP_HIT") ++r.n_tp_hit;
        else if (t.exitReason=="SL_HIT") ++r.n_sl_hit;
        else if (t.exitReason=="TIME_STOP") ++r.n_time_stop;
    }
    r.wr_pct = (r.n_trades>0) ? (100.0*r.n_wins/r.n_trades) : 0;
    r.profit_factor = (gl > 1e-9) ? (gw/gl) : 0.0;
    r.avg_win = (r.n_wins>0) ? (gw/r.n_wins) : 0;
    r.avg_loss = (r.n_losses>0) ? (-gl/r.n_losses) : 0;
    return r;
}

int main(int argc, char** argv) {
    const char* csv_path = (argc >= 2) ? argv[1]
        : "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv";
    std::fprintf(stderr, "[GRR-SWEEP] CSV: %s\n", csv_path);

    const Config configs_init[] = {
        {1.0, 2.0, "sl=1.0/rr=2.0"},
        {1.5, 2.0, "sl=1.5/rr=2.0"},
        {1.0, 3.0, "sl=1.0/rr=3.0"},
        {1.5, 3.0, "sl=1.5/rr=3.0"},
        {2.0, 2.0, "sl=2.0/rr=2.0"},
        {2.0, 3.0, "sl=2.0/rr=3.0"},
    };
    std::vector<Config> configs(std::begin(configs_init), std::end(configs_init));

    std::printf("===================================================================================\n");
    std::printf("  GoldFixedRR sweep -- M5 entry + hard SL + hard TP, no trail\n");
    std::printf("  Sweep: SL_ATR_MULT {1.0, 1.5, 2.0} x RR_RATIO {2.0, 3.0}\n");
    std::printf("===================================================================================\n\n");

    std::vector<Result> results;
    const auto t_start = std::chrono::steady_clock::now();
    for (size_t i=0; i<configs.size(); ++i) {
        const auto& cfg = configs[i];
        std::fprintf(stderr, "[GRR-SWEEP] === Cfg %zu/%zu: %s ===\n", i+1, configs.size(), cfg.label);
        Driver drv; drv.reset(); apply_config(drv.engine, cfg);
        const auto t0 = std::chrono::steady_clock::now();
        const int64_t n = parse_csv_and_run(drv, csv_path, cfg);
        const auto t1 = std::chrono::steady_clock::now();
        if (n<0) return 1;
        const double secs = std::chrono::duration<double>(t1-t0).count();
        Result r = summarize(cfg, drv.trades, drv.max_dd);
        results.push_back(r);
        std::fprintf(stderr,
            "[GRR-SWEEP] Cfg %zu (%s) %.1fs: n=%d wr=%.1f%% pnl=$%.2f pf=%.2f tp=%d sl=%d\n\n",
            i+1, cfg.label, secs, r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor,
            r.n_tp_hit, r.n_sl_hit);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const double total_secs = std::chrono::duration<double>(t_end-t_start).count();

    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) { return a.gross_pnl_usd > b.gross_pnl_usd; });

    std::printf("\n=================================================================================================================================\n");
    std::printf("  FIXED-RR RESULTS (sorted by PnL desc) -- runtime %.1f min\n", total_secs/60.0);
    std::printf("=================================================================================================================================\n");
    std::printf("%-18s %-7s %-6s %-12s %-6s %-9s %-9s %-9s %-5s %-5s %-5s\n",
                "Config","N","WR%","PnL$","PF","DD$","AvgWin","AvgLoss","TP","SL","TS");
    std::printf("---------------------------------------------------------------------------------------------------------------------------------\n");
    for (const auto& r : results) {
        std::printf("%-18s %-7d %-6.1f %-+12.2f %-6.2f %-9.2f %+9.2f %+9.2f %-5d %-5d %-5d\n",
            r.cfg.label, r.n_trades, r.wr_pct, r.gross_pnl_usd, r.profit_factor, r.max_dd_usd,
            r.avg_win, r.avg_loss, r.n_tp_hit, r.n_sl_hit, r.n_time_stop);
    }
    std::printf("=================================================================================================================================\n\n");

    const double PNL_BAR=5000.0, PF_BAR=1.20;
    const Result* best_pass=nullptr;
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
        const Result* top = results.empty()?nullptr:&results.front();
        std::printf("  -> FAIL. ");
        if (top) std::printf("Top: %s PnL=$%.2f PF=%.2f WR=%.1f%% avgWin=$%.2f avgLoss=$%.2f\n",
                             top->cfg.label, top->gross_pnl_usd, top->profit_factor,
                             top->wr_pct, top->avg_win, top->avg_loss);
    }
    std::printf("\n");
    return 0;
}
