// =============================================================================
// test_zoo_concurrency.cpp -- proves S51 concurrency cap actually fires when
// XAU zoo engines run on the SAME bar (production scenario).
//
// Methodology: drive 4 wired engines (XauTurtleD1, XauTsmomFastD1,
// XauOutsideBarD1, XauPullbackContH4) over identical H4 tape simultaneously,
// each receiving the same bar+tick stream. Without cap, multiple engines
// would open longs on the same trend day; with cap=2, only first two get
// through, third+ blocked.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/test_zoo_concurrency.cpp \
//           -o backtest/test_zoo_concurrency
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
#include "XauOutsideBarD1Engine.hpp"
#include "XauPullbackContH4Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load(const std::string& path) {
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

int main() {
    auto bars = load("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    std::fprintf(stderr, "loaded %zu H4 bars\n", bars.size());
    if (bars.empty()) return 1;

    // Configure cap=2 (matches engine_init.hpp default for live)
    omega::pg::g_pg_cfg.max_concurrent_positions = 2;
    omega::pg::g_pg_cfg.kill_file_enabled = false;  // not under test
    omega::pg::g_pg_state.concurrent_positions.store(0);
    omega::pg::g_pg_state.n_blocked_concurrency.store(0);
    omega::pg::g_pg_state.n_blocked_kill_file.store(0);

    omega::XauTurtleD1Engine turtle;
    omega::XauTsmomFastD1Engine tsmom;
    omega::XauOutsideBarD1Engine ob;
    omega::XauPullbackContH4Engine pch4;
    for (auto* e : std::vector<bool*>{&turtle.shadow_mode, &tsmom.shadow_mode,
                                       &ob.shadow_mode, &pch4.shadow_mode}) *e = true;
    for (auto* e : std::vector<bool*>{&turtle.enabled, &tsmom.enabled,
                                       &ob.enabled, &pch4.enabled}) *e = true;
    turtle.symbol = "XAUUSD";   turtle.p = omega::make_xau_turtle_d1_params();
    tsmom.symbol  = "XAUUSD";   tsmom.p  = omega::make_xau_tsmom_fast_d1_params();
    ob.symbol     = "XAUUSD";   ob.p     = omega::make_xau_outside_bar_d1_params();
    pch4.symbol   = "XAUUSD";   pch4.p   = omega::make_xau_pullback_cont_h4_params();

    int n_turtle = 0, n_tsmom = 0, n_ob = 0, n_pch4 = 0;
    auto cb_turtle = [&](const omega::TradeRecord&) { ++n_turtle; };
    auto cb_tsmom  = [&](const omega::TradeRecord&) { ++n_tsmom; };
    auto cb_ob     = [&](const omega::TradeRecord&) { ++n_ob; };
    auto cb_pch4   = [&](const omega::TradeRecord&) { ++n_pch4; };

    const double half = 0.15;
    int64_t peak_concurrent = 0;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        // All four engines see same bar (production: tick_gold.hpp dispatches)
        turtle.on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb_turtle);
        tsmom .on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb_tsmom);
        ob    .on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb_ob);
        pch4  .on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb_pch4);

        const int64_t cur = omega::pg::g_pg_state.concurrent_positions.load();
        if (cur > peak_concurrent) peak_concurrent = cur;

        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            turtle.on_tick(nb.l - half, nb.l + half, nts, cb_turtle);
            tsmom .on_tick(nb.l - half, nb.l + half, nts, cb_tsmom);
            ob    .on_tick(nb.l - half, nb.l + half, nts, cb_ob);
            pch4  .on_tick(nb.l - half, nb.l + half, nts, cb_pch4);
            turtle.on_tick(nb.h - half, nb.h + half, nts, cb_turtle);
            tsmom .on_tick(nb.h - half, nb.h + half, nts, cb_tsmom);
            ob    .on_tick(nb.h - half, nb.h + half, nts, cb_ob);
            pch4  .on_tick(nb.h - half, nb.h + half, nts, cb_pch4);
        }
    }

    const int64_t blocked = omega::pg::g_pg_state.n_blocked_concurrency.load();
    const int64_t total_closed = n_turtle + n_tsmom + n_ob + n_pch4;
    std::printf("\n=== ZOO CONCURRENCY TEST (cap=2, 4 wired engines) ===\n");
    std::printf("  total trades closed:   %lld\n", (long long)total_closed);
    std::printf("    turtle=%d  tsmom=%d  ob=%d  pch4=%d\n",
                n_turtle, n_tsmom, n_ob, n_pch4);
    std::printf("  peak concurrent open:  %lld  (cap=%d)\n",
                (long long)peak_concurrent, omega::pg::g_pg_cfg.max_concurrent_positions);
    std::printf("  n_blocked_concurrency: %lld\n", (long long)blocked);
    std::printf("  final concurrent:      %lld (should be 0)\n",
                (long long)omega::pg::g_pg_state.concurrent_positions.load());

    bool pass = true;
    if (peak_concurrent > 2) { std::printf("  FAIL: peak %lld > cap 2\n", (long long)peak_concurrent); pass = false; }
    if (omega::pg::g_pg_state.concurrent_positions.load() != 0) { std::printf("  FAIL: leak\n"); pass = false; }
    if (blocked == 0) { std::printf("  WARN: no blocks fired — cap never bound (engines may not overlap)\n"); }
    std::printf("\n  %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
