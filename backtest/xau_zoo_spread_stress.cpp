// =============================================================================
// xau_zoo_spread_stress.cpp -- spread stress test for surviving XAU engines.
//
// Runs each engine at three spread levels and compares the resulting Sharpe:
//   0.15 half-spread  -> $0.30 total (audit baseline, optimistic for BB OTC)
//   0.30 half-spread  -> $0.60 total (realistic BB OTC normal hours)
//   0.50 half-spread  -> $1.00 total (BB OTC stressed / news window)
//
// Engines that retain edge at $0.60 are safe for shadow->LIVE progression.
// Engines that collapse between $0.30 and $0.60 had unrealistic edge that
// doesn't survive a realistic execution cost.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_zoo_spread_stress.cpp \
//           -o backtest/xau_zoo_spread_stress
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "XauTurtleD1Engine.hpp"
#include "XauTsmomFastD1Engine.hpp"
#include "XauStopRunD1Engine.hpp"
#include "XauPullbackContH4Engine.hpp"
#include "XauEmaCrossH4Engine.hpp"
#include "XauDojiRejD1Engine.hpp"
#include "XauOutsideBarD1Engine.hpp"
#include "XauInsideBarD1Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        H4Bar b; const char* p = line.c_str(); char* e;
        b.ts_sec = std::strtoll(p, &e, 10); if (*e != ',') continue; p = e + 1;
        b.o = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.h = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.l = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.c = std::strtod(p, &e);
        bars.push_back(b);
    }
    return bars;
}

static double sharpe(const std::vector<double>& xs, int ann = 252) {
    if (xs.size() < 2) return 0.0;
    double sum = 0; for (double x : xs) sum += x;
    double mean = sum / xs.size();
    double var = 0; for (double x : xs) var += (x - mean) * (x - mean);
    var /= (xs.size() - 1);
    double sd = std::sqrt(var);
    return sd > 0 ? (mean / sd) * std::sqrt(ann) : 0.0;
}

struct Stat { double sharpe = 0; double gross = 0; int64_t n = 0; };

template<typename Engine>
Stat run_one(const std::vector<H4Bar>& bars, double half, Engine& eng) {
    std::vector<double> pnls; double gross = 0; int64_t n = 0;
    auto cb = [&](const omega::TradeRecord& tr) {
        ++n; gross += tr.pnl; pnls.push_back(tr.pnl);
    };
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        eng.on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
    return {sharpe(pnls), gross, n};
}

template<typename Engine, typename ParamFactory>
void audit(const std::string& name, const std::vector<H4Bar>& bars, ParamFactory mk) {
    Stat s30, s60, s100;
    {
        Engine e; e.shadow_mode = true; e.enabled = true; e.symbol = "XAUUSD"; e.p = mk();
        s30 = run_one(bars, 0.15, e);
    }
    {
        Engine e; e.shadow_mode = true; e.enabled = true; e.symbol = "XAUUSD"; e.p = mk();
        s60 = run_one(bars, 0.30, e);
    }
    {
        Engine e; e.shadow_mode = true; e.enabled = true; e.symbol = "XAUUSD"; e.p = mk();
        s100 = run_one(bars, 0.50, e);
    }
    const char* verdict;
    if (s60.sharpe > 0.5 && s100.sharpe > 0.0)        verdict = "ROBUST";
    else if (s60.sharpe > 0.0)                         verdict = "MARGINAL";
    else                                                verdict = "FRAGILE";
    std::printf("%-22s  $0.30: Sh=%+5.2f gr=%+6.2f | "
                "$0.60: Sh=%+5.2f gr=%+6.2f | "
                "$1.00: Sh=%+5.2f gr=%+6.2f | %s\n",
                name.c_str(),
                s30.sharpe,  s30.gross,
                s60.sharpe,  s60.gross,
                s100.sharpe, s100.gross,
                verdict);
}

int main() {
    auto bars = load_h4("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    std::fprintf(stderr, "loaded %zu H4 bars\n", bars.size());
    if (bars.empty()) return 1;
    omega::pg::g_pg_cfg.max_concurrent_positions = 0;

    std::printf("\n%s\n", std::string(120, '=').c_str());
    std::printf("  XAU ZOO SPREAD STRESS  ($0.30 / $0.60 / $1.00 round-trip cost)\n");
    std::printf("%s\n", std::string(120, '=').c_str());

    audit<omega::XauTurtleD1Engine>("XauTurtleD1", bars, omega::make_xau_turtle_d1_params);
    audit<omega::XauTsmomFastD1Engine>("XauTsmomFastD1", bars, omega::make_xau_tsmom_fast_d1_params);
    audit<omega::XauStopRunD1Engine>("XauStopRunD1", bars, omega::make_xau_stop_run_d1_params);
    audit<omega::XauPullbackContH4Engine>("XauPullbackContH4", bars, omega::make_xau_pullback_cont_h4_params);
    audit<omega::XauEmaCrossH4Engine>("XauEmaCrossH4", bars, omega::make_xau_ema_cross_h4_params);
    audit<omega::XauDojiRejD1Engine>("XauDojiRejD1", bars, omega::make_xau_doji_rej_d1_params);
    audit<omega::XauOutsideBarD1Engine>("XauOutsideBarD1", bars, omega::make_xau_outside_bar_d1_params);
    audit<omega::XauInsideBarD1Engine>("XauInsideBarD1", bars, omega::make_xau_inside_bar_d1_params);

    std::printf("\nROBUST: Sh > 0.5 at $0.60 AND Sh > 0 at $1.00\n");
    std::printf("MARGINAL: Sh > 0 at $0.60 only\n");
    std::printf("FRAGILE: edge collapses below $0.60\n");
    return 0;
}
