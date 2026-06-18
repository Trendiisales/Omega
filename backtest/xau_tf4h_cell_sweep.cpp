// =============================================================================
// xau_tf4h_cell_sweep.cpp -- per-cell WF sweep for XauTrendFollow4hEngine
//
// Tier-2 re-opt (2026-06-12): validate cell_enable_mask 0xC9, esp. bit 6
// EmaCross8_21 (S116, enabled in prod without WF provenance).
//
// Real-class driver: same bar->tick replay as xau_trendfollow_audit.cpp
// (bar close + next-bar low-first/high tick replay, configurable half-spread).
//
// Usage:
//   xau_tf4h_cell_sweep <h4.csv> [--mask 0xC9] [--half 0.15]
//                       [--start <ts_sec>] [--end <ts_sec>] [--name label]
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/xau_tf4h_cell_sweep.cpp \
//           -o backtest/xau_tf4h_cell_sweep
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "XauTrendFollow4hEngine.hpp"

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
    if (argc < 2) { std::fprintf(stderr, "usage: %s h4.csv [--mask 0xC9] [--half 0.15] [--start ts] [--end ts] [--name x]\n", argv[0]); return 1; }
    uint32_t mask = 0xC9; double half = 0.15;
    int64_t t0 = 0, t1 = 0; const char* name = "";
    const char* h1_path = nullptr; bool regime_gate = false;
    double be_arm = 0.0, be_buf = 0.0;   // S-2026-06-19 deep-BE profit-lock sweep (% of entry)
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--mask" && i + 1 < argc)  mask = (uint32_t)std::stoul(argv[++i], nullptr, 0);
        else if (a == "--half" && i + 1 < argc)  half = std::atof(argv[++i]);
        else if (a == "--start" && i + 1 < argc) t0 = std::atoll(argv[++i]);
        else if (a == "--end" && i + 1 < argc)   t1 = std::atoll(argv[++i]);
        else if (a == "--name" && i + 1 < argc)  name = argv[++i];
        else if (a == "--h1" && i + 1 < argc)    h1_path = argv[++i];
        else if (a == "--regime-gate")           regime_gate = true;
        else if (a == "--be-arm" && i + 1 < argc) be_arm = std::atof(argv[++i]);
        else if (a == "--be-buf" && i + 1 < argc) be_buf = std::atof(argv[++i]);
    }
    auto h4 = load_csv(argv[1], t0, t1);
    if (h4.empty()) { std::fprintf(stderr, "no bars\n"); return 1; }
    // Regime feed: always load full H1 history (no t0 cut) so gold_regime()
    // is warm at the H4 window start — mirrors prod warm-seed mandate.
    std::vector<Bar> h1;
    if (h1_path) {
        h1 = load_csv(h1_path, 0, t1);
        if (h1.empty()) { std::fprintf(stderr, "no h1 bars\n"); return 1; }
    }
    if (regime_gate && !h1_path) { std::fprintf(stderr, "--regime-gate needs --h1\n"); return 1; }

    int64_t n = 0, wins = 0;
    double gross = 0, wsum = 0, lsum = 0;
    std::vector<double> pnl;

    omega::XauTrendFollow4hEngine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.cell_enable_mask = mask;
    eng.use_regime_long_gate = regime_gate;
    eng.BE_ARM_PCT = be_arm; eng.BE_BUFFER_PCT = be_buf;   // deep-BE profit-lock
    eng.lot = 0.01; eng.max_spread = 1.0; eng.init();
    size_t h1_i = 0;
    double sum_mfe = 0, worst = 0; long n_be = 0;
    auto cb = [&](const omega::TradeRecord& tr) {
        ++n; gross += tr.pnl; pnl.push_back(tr.pnl);
        if (tr.pnl > 0) { ++wins; wsum += tr.pnl; } else lsum += -tr.pnl;
        sum_mfe += tr.mfe; if (tr.pnl < worst) worst = tr.pnl;
        if (std::string(tr.exitReason).find("BE") != std::string::npos) ++n_be;
    };
    for (size_t i = 0; i < h4.size(); ++i) {
        const auto& b = h4[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        // Feed every CLOSED H1 bar up to this H4 bar's open into gold_regime().
        while (h1_i < h1.size() && h1[h1_i].ts_sec < b.ts_sec) {
            const auto& hb = h1[h1_i++];
            omega::gold_regime().on_h1_bar(hb.o, hb.h, hb.l, hb.c);
        }
        omega::XauTfBar bar;
        bar.bar_start_ms = ts_ms; bar.open = b.o; bar.high = b.h;
        bar.low = b.l; bar.close = b.c;
        eng.on_h4_bar(bar, b.c - half, b.c + half, 0.0, ts_ms, cb);
        if (i + 1 < h4.size()) {
            const auto& nb = h4[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }
    double eq = 0, peak = 0, mdd = 0;
    for (double x : pnl) { eq += x; if (eq > peak) peak = eq; if (peak - eq > mdd) mdd = peak - eq; }
    const double pf = lsum > 0 ? wsum / lsum : (wsum > 0 ? 99.0 : 0.0);
    std::printf("%-16s be=%.1f/%.1f n=%-4lld WR=%5.1f%% net=%+9.2f PF=%5.2f mdd=%8.2f worst=%+8.2f sumMFE=%9.2f be_exits=%ld\n",
                name, be_arm, be_buf, (long long)n,
                n > 0 ? 100.0 * wins / n : 0.0, gross, pf, mdd, worst, sum_mfe, n_be);
    return 0;
}
