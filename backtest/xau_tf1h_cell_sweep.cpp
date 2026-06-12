// =============================================================================
// xau_tf1h_cell_sweep.cpp -- per-cell + pyramid/vol-target WF sweep for
// XauTrendFollow1hEngine (Tier-2 re-opt 2026-06-12).
//
// Validates the S40/S118 ensemble stamps engine_init ships:
//   cell_enable_mask=0x0F, use_vol_target=true (unit 0.10),
//   pyramid_max_adds=2 / step 1.0 ATR / sl 3.0 ATR.
//
// Real-class driver, same replay convention as xau_trendfollow_audit.cpp:
// bar close -> next-bar low tick then high tick, configurable half-spread.
//
// Usage:
//   xau_tf1h_cell_sweep <h1.csv> [--mask 0x0F] [--half 0.15] [--start ts]
//                       [--end ts] [--pyr N] [--volt 0|1] [--name label]
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_tf1h_cell_sweep.cpp \
//           -o backtest/xau_tf1h_cell_sweep
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "XauTrendFollow1hEngine.hpp"

struct Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<Bar> load_csv(const std::string& path, int64_t t0, int64_t t1) {
    std::vector<Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        Bar b; const char* p = line.c_str(); char* e;
        b.ts_sec = std::strtoll(p, &e, 10); if (*e != ',') continue; p = e + 1;
        b.o = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.h = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.l = std::strtod(p, &e); if (*e != ',') continue; p = e + 1;
        b.c = std::strtod(p, &e);
        if (b.ts_sec < t0 || (t1 > 0 && b.ts_sec > t1)) continue;
        bars.push_back(b);
    }
    return bars;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s h1.csv [--mask 0x0F] [--half 0.15] [--start ts] [--end ts] [--pyr N] [--volt 0|1] [--name x]\n", argv[0]); return 1; }
    uint32_t mask = 0x0F; double half = 0.15;
    int64_t t0 = 0, t1 = 0; const char* name = "";
    int pyr = 0; int volt = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--mask" && i + 1 < argc)  mask = (uint32_t)std::stoul(argv[++i], nullptr, 0);
        else if (a == "--half" && i + 1 < argc)  half = std::atof(argv[++i]);
        else if (a == "--start" && i + 1 < argc) t0 = std::atoll(argv[++i]);
        else if (a == "--end" && i + 1 < argc)   t1 = std::atoll(argv[++i]);
        else if (a == "--pyr" && i + 1 < argc)   pyr = std::atoi(argv[++i]);
        else if (a == "--volt" && i + 1 < argc)  volt = std::atoi(argv[++i]);
        else if (a == "--name" && i + 1 < argc)  name = argv[++i];
    }
    auto h1 = load_csv(argv[1], t0, t1);
    if (h1.empty()) { std::fprintf(stderr, "no bars\n"); return 1; }

    int64_t n = 0, wins = 0;
    double gross = 0, wsum = 0, lsum = 0;
    std::vector<double> pnl;

    omega::XauTrendFollow1hEngine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.cell_enable_mask = mask;
    eng.lot = 0.01; eng.max_spread = 1.0;
    eng.use_vol_target = (volt != 0);
    eng.vol_target_unit = 0.10;
    eng.pyramid_max_adds = pyr;
    eng.pyramid_step_atr = 1.0;
    eng.pyramid_sl_atr   = 3.0;
    eng.init();
    auto cb = [&](const omega::TradeRecord& tr) {
        ++n; gross += tr.pnl; pnl.push_back(tr.pnl);
        if (tr.pnl > 0) { ++wins; wsum += tr.pnl; } else lsum += -tr.pnl;
    };
    for (size_t i = 0; i < h1.size(); ++i) {
        const auto& b = h1[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        omega::XauTfBar1h bar;
        bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
        bar.low = b.l; bar.close = b.c;
        eng.on_h1_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);
        if (i + 1 < h1.size()) {
            const auto& nb = h1[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
    double eq = 0, peak = 0, mdd = 0;
    for (double x : pnl) { eq += x; if (eq > peak) peak = eq; if (peak - eq > mdd) mdd = peak - eq; }
    const double pf = lsum > 0 ? wsum / lsum : (wsum > 0 ? 99.0 : 0.0);
    std::printf("%-12s mask=0x%02X pyr=%d volt=%d half=%.2f bars=%zu  n=%-4lld WR=%5.1f%%  net=%+9.2f  PF=%5.2f  mdd=%8.2f\n",
                name, mask, pyr, volt, half, h1.size(), (long long)n,
                n > 0 ? 100.0 * wins / n : 0.0, gross, pf, mdd);
    return 0;
}
