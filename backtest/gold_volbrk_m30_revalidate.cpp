// =============================================================================
// gold_volbrk_m30_revalidate.cpp -- Tier-2 re-opt #3 (2026-06-12) for
// GoldVolBreakoutM30Engine ("beoff" config).
//
// Original validation (2026-06-03, /tmp/xauvb -- harness since deleted) was
// bull-tape-only, 43 trades. This re-validates the REAL engine class on:
//   - 2yr M30 (full + halves)
//   - XAU 2022 bear M30 slice
//   - 3x cost stress
//
// Feed convention: M30 bars closed in order; every 2nd M30 close also feeds
// on_h1_close (H1 EMA200 trend gate), matching tick_gold wiring. Intra-bar
// exits approximated by next-bar low/high management via on_m30_bar's own
// stop logic (engine manages on bar close; mirrors prod M30 cadence).
//
// Usage: gold_volbrk_m30_revalidate <m30.csv> [--half 0.15] [--start ts]
//        [--end ts] [--name label]
// Build: clang++ -O3 -std=c++17 -I include backtest/gold_volbrk_m30_revalidate.cpp \
//        -o backtest/gold_volbrk_m30_revalidate
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "GoldVolBreakoutM30Engine.hpp"

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
    if (argc < 2) { std::fprintf(stderr, "usage: %s m30.csv [--half 0.15] [--start ts] [--end ts] [--name x]\n", argv[0]); return 1; }
    double half = 0.15; int64_t t0 = 0, t1 = 0; const char* name = "";
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--half" && i + 1 < argc)  half = std::atof(argv[++i]);
        else if (a == "--start" && i + 1 < argc) t0 = std::atoll(argv[++i]);
        else if (a == "--end" && i + 1 < argc)   t1 = std::atoll(argv[++i]);
        else if (a == "--name" && i + 1 < argc)  name = argv[++i];
    }
    auto m30 = load_csv(argv[1], t0, t1);
    if (m30.empty()) { std::fprintf(stderr, "no bars\n"); return 1; }

    int64_t n = 0, wins = 0;
    double gross = 0, wsum = 0, lsum = 0;
    std::vector<double> pnl;

    omega::GoldVolBreakoutM30Engine eng;
    eng.shadow_mode = true; eng.enabled = true; eng.lot = 0.01;
    eng.init(); eng.enabled = true;
    auto cb = [&](const omega::TradeRecord& tr) {
        ++n; gross += tr.pnl; pnl.push_back(tr.pnl);
        if (tr.pnl > 0) { ++wins; wsum += tr.pnl; } else lsum += -tr.pnl;
    };
    // H1 close = every M30 bar whose end falls on the hour boundary.
    // Intra-bar SL/trail management: feed next bar's low then high through
    // on_tick (engine checks bid <= sl_px per tick in prod via tick_gold:2269).
    for (size_t i = 0; i < m30.size(); ++i) {
        const auto& b = m30[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        eng.on_m30_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb);
        const int64_t bar_end = b.ts_sec + 1800;
        if (bar_end % 3600 == 0) eng.on_h1_close(b.c);
        if (i + 1 < m30.size()) {
            const auto& nb = m30[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
    double eq = 0, peak = 0, mdd = 0;
    for (double x : pnl) { eq += x; if (eq > peak) peak = eq; if (peak - eq > mdd) mdd = peak - eq; }
    const double pf = lsum > 0 ? wsum / lsum : (wsum > 0 ? 99.0 : 0.0);
    std::printf("%-12s half=%.2f bars=%zu  n=%-4lld WR=%5.1f%%  net=%+9.2f  PF=%5.2f  mdd=%8.2f\n",
                name, half, m30.size(), (long long)n,
                n > 0 ? 100.0 * wins / n : 0.0, gross, pf, mdd);
    return 0;
}
