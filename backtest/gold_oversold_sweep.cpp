// =============================================================================
// gold_oversold_sweep.cpp -- Tier-2 re-opt #5 (2026-06-12): RSI entry/exit grid
// + WF halves + bear slice for GoldOversoldBounceEngine.
//
// Derived from GoldOversoldBounceBacktest.cpp (engine-driven, M5 closes as
// ticks, $0.37 RT cost as pct of entry). Adds: --entry/--exit RSI overrides,
// --start/--end window, --name label. Defaults = prod (30/50).
//
// Build: clang++ -O3 -std=c++17 -Iinclude backtest/gold_oversold_sweep.cpp \
//        -o backtest/gold_oversold_sweep
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include "GoldOversoldBounceEngine.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s m5.csv [--entry 30] [--exit 50] [--start ts] [--end ts] [--name x]\n", argv[0]); return 1; }
    double e_rsi = 30.0, x_rsi = 50.0;
    long t0 = 0, t1 = 0; const char* name = ""; const char* h1_path = nullptr;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--entry" && i + 1 < argc) e_rsi = std::atof(argv[++i]);
        else if (a == "--exit" && i + 1 < argc)  x_rsi = std::atof(argv[++i]);
        else if (a == "--start" && i + 1 < argc) t0 = std::atol(argv[++i]);
        else if (a == "--end" && i + 1 < argc)   t1 = std::atol(argv[++i]);
        else if (a == "--name" && i + 1 < argc)  name = argv[++i];
        else if (a == "--h1" && i + 1 < argc)    h1_path = argv[++i];
    }
    std::ifstream f(argv[1]); std::string ln; std::getline(f, ln);
    std::vector<std::pair<long,double>> bars;
    while (std::getline(f, ln)) {
        double ts,o,h,l,c;
        if (sscanf(ln.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) != 5) continue;
        if (c <= 0) continue;
        long t = (long)ts; if (t > 1000000000000L) t /= 1000;
        if (t < t0 || (t1 > 0 && t > t1)) continue;
        bars.push_back({t, c});
    }
    if (bars.size() < 1000) { std::printf("few bars\n"); return 1; }
    omega::GoldOversoldBounceEngine g;
    g.shadow_mode = false; g.enabled = true; g.lot = 0.01;
    g.entry_rsi = e_rsi; g.exit_rsi = x_rsi;
    std::vector<double> rets; double cum=0, peak=0, mdd=0, gw=0, gl=0; int nw=0, nl=0;
    auto cb = [&](const omega::TradeRecord& tr) {
        double pct = (tr.exitPrice - tr.entryPrice) / tr.entryPrice * 100.0;
        pct -= 0.37 / tr.entryPrice * 100.0;
        rets.push_back(pct); cum += pct;
        if (cum > peak) peak = cum; if (peak - cum > mdd) mdd = peak - cum;
        if (pct > 0) { nw++; gw += pct; } else { nl++; gl += -pct; }
    };
    // Optional regime feed: H1 closes -> gold_regime(), activating the engine's
    // own long_blocked() gate (wired 2026-06-12). Without --h1 the gate is inert.
    std::vector<std::pair<long,double>> h1c;
    if (h1_path) {
        std::ifstream hf(h1_path); std::string hl; std::getline(hf, hl);
        while (std::getline(hf, hl)) {
            double ts,o,h,l,c;
            if (sscanf(hl.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) != 5) continue;
            long t = (long)ts; if (t > 1000000000000L) t /= 1000;
            h1c.push_back({t, c});
        }
        std::fprintf(stderr, "regime feed: %zu H1 closes\n", h1c.size());
    }
    size_t hi = 0;
    const double SP = 0.37;
    for (auto& b : bars) {
        while (hi < h1c.size() && h1c[hi].first < b.first) {
            omega::gold_regime().on_h1_bar(0, 0, 0, h1c[hi].second); ++hi;
        }
        g.on_tick(b.second - SP/2, b.second + SP/2, (int64_t)b.first * 1000, cb);
    }
    g.force_close((int64_t)bars.back().first * 1000, cb);
    int n = (int)rets.size();
    double pf = gl > 0 ? gw / gl : (gw > 0 ? 99.0 : 0.0);
    std::printf("%-10s entry=%2.0f exit=%2.0f  n=%-3d WR=%5.1f%%  net=%+7.2f%%  PF=%5.2f  mdd=%6.2f%%\n",
                name, e_rsi, x_rsi, n, n ? 100.0 * nw / n : 0.0, cum, pf, mdd);
    return 0;
}
