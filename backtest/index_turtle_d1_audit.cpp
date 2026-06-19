// =============================================================================
// index_turtle_d1_audit.cpp -- class-driven faithful BT for NasTurtleD1Engine.
//
// Drives the REAL omega::NasTurtleD1Engine class (the shipped chassis for
// g_nas_turtle_d1 / g_spx_turtle_d1 / g_dj30_turtle_d1) over 10yr DAILY bars.
// D1 engine -> must be tested on DAILY granularity over a LONG history (the
// 2.5yr tick corpus gives n~10-19 = the underpowered "tick-cull" the operator
// already rejected). NAS was confirmed PF2.69 on 10yr Yahoo daily; SPX + DJ30
// are OWED. This closes that gap on the SHIPPED config (Donch20, no ema100,
// sl1.5ATR/tp5ATR/hold20 -- NOT the PF2.69 ema100/wide-trail variant).
//
// Feed: engine has only on_tick (self-aggregates D1 by 86400000ms bucket). Per
// daily bar feed open->low->high->close ticks; the next day's first tick rolls
// the bucket and evaluates the breakout on the just-closed bar => next-open
// entry at ask, no lookahead. Intra-bar SL/TP via the low (adverse-first) then
// high ticks. Pessimistic.
//
// Build:
//   clang++ -O3 -std=c++17 -I include backtest/index_turtle_d1_audit.cpp \
//           -o backtest/index_turtle_d1_audit
// Run:
//   ./backtest/index_turtle_d1_audit <daily.csv> <LABEL> <dollars_per_pt> [half_spread]
//   daily.csv format: ts_sec,open,high,low,close  (no header assumed; auto-detects)
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "NasTurtleD1Engine.hpp"

struct DBar { int64_t ts_sec; double o, h, l, c; };

static std::vector<DBar> load_daily_csv(const std::string& path) {
    std::vector<DBar> bars;
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return bars; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // skip a header line if first field isn't numeric
        if (!(line[0] == '-' || (line[0] >= '0' && line[0] <= '9'))) continue;
        DBar b; char* p = const_cast<char*>(line.c_str());
        b.ts_sec = std::strtoll(p, &p, 10); if (*p != ',') continue; ++p;
        b.o = std::strtod(p, &p); if (*p != ',') continue; ++p;
        b.h = std::strtod(p, &p); if (*p != ',') continue; ++p;
        b.l = std::strtod(p, &p); if (*p != ',') continue; ++p;
        b.c = std::strtod(p, &p);
        if (b.o > 0 && b.h > 0 && b.l > 0 && b.c > 0) bars.push_back(b);
    }
    return bars;
}

static int year_of(int64_t ts_sec) {
    time_t t = ts_sec; struct tm g; gmtime_r(&t, &g); return g.tm_year + 1900;
}

struct Stats {
    int64_t n = 0, wins = 0, sl = 0, tp = 0, wknd = 0, to = 0, other = 0;
    double gross = 0, pos_sum = 0, neg_sum = 0, best = 0, worst = 0;
    std::vector<double> pnl; std::vector<int64_t> ts;
    void record(const omega::TradeRecord& tr) {
        ++n; gross += tr.pnl; pnl.push_back(tr.pnl); ts.push_back(tr.exitTs);
        if (tr.pnl > 0) { ++wins; pos_sum += tr.pnl; } else { neg_sum += -tr.pnl; }
        if (tr.pnl > best) best = tr.pnl; if (tr.pnl < worst) worst = tr.pnl;
        const std::string& r = tr.exitReason;
        if (r == "SL_HIT") ++sl; else if (r == "TP_HIT") ++tp;
        else if (r == "WEEKEND_CLOSE") ++wknd; else if (r == "TIMEOUT") ++to; else ++other;
    }
};

static double maxdd(const std::vector<double>& xs) {
    double eq = 0, peak = 0, dd = 0;
    for (double x : xs) { eq += x; if (eq > peak) peak = eq; if (peak - eq > dd) dd = peak - eq; }
    return -dd;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <daily.csv> <LABEL> <dollars_per_pt> [half_spread]\n", argv[0]); return 2; }
    const std::string path = argv[1];
    const std::string label = argv[2];
    const double dpp = std::atof(argv[3]);
    const double hs = argc > 4 ? std::atof(argv[4]) : 0.5;
    // optional exit-variant overrides (faithful — engine's own knobs)
    const double tp_mult_ov  = argc > 5 ? std::atof(argv[5]) : 0.0;  // 0 = keep default
    const int    hold_ov     = argc > 6 ? std::atoi(argv[6]) : 0;
    const double sl_mult_ov  = argc > 7 ? std::atof(argv[7]) : 0.0;
    const bool   ema100_ov   = argc > 8 ? std::atoi(argv[8]) != 0 : false;
    const double be_arm_ov   = argc > 9 ? std::atof(argv[9])  : 0.0;  // BE_ARM_PCT (0=off)
    const double be_buf_ov   = argc >10 ? std::atof(argv[10]) : 0.0;  // BE_BUFFER_PCT

    auto bars = load_daily_csv(path);
    if (bars.size() < 50) { std::fprintf(stderr, "[%s] only %zu bars -- abort\n", label.c_str(), bars.size()); return 1; }
    std::fprintf(stderr, "[%s] loaded %zu daily bars (%d..%d)\n", label.c_str(),
                 bars.size(), year_of(bars.front().ts_sec), year_of(bars.back().ts_sec));

    omega::NasTurtleD1Engine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.symbol = label;
    eng.p = omega::make_nas_turtle_d1_params();
    eng.p.dollars_per_pt = dpp;
    if (tp_mult_ov > 0.0) eng.p.tp_atr_mult  = tp_mult_ov;
    if (hold_ov    > 0)   eng.p.hold_max_bars = hold_ov;
    if (sl_mult_ov > 0.0) eng.p.sl_atr_mult  = sl_mult_ov;
    if (ema100_ov)        eng.p.use_ema100_filter = true;
    if (be_arm_ov > 0.0){ eng.p.BE_ARM_PCT = be_arm_ov; eng.p.BE_BUFFER_PCT = be_buf_ov; }

    Stats all, y2022, pre22, post22;
    std::vector<double> first_half, second_half;
    auto cb = [&](const omega::TradeRecord& tr) {
        all.record(tr);
        int yr = year_of(tr.exitTs);
        if (yr == 2022) y2022.record(tr);
        else if (yr < 2022) pre22.record(tr); else post22.record(tr);
    };

    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ms = b.ts_sec * 1000LL;
        // open first (rolls prior bucket -> eval -> possible entry at this ask),
        // then low (adverse for long), then high, then close.
        eng.on_tick(b.o - hs, b.o + hs, ms, cb);
        eng.on_tick(b.l - hs, b.l + hs, ms, cb);
        eng.on_tick(b.h - hs, b.h + hs, ms, cb);
        eng.on_tick(b.c - hs, b.c + hs, ms, cb);
    }

    // WF halves by trade order
    for (size_t i = 0; i < all.pnl.size(); ++i)
        (i < all.pnl.size() / 2 ? first_half : second_half).push_back(all.pnl[i]);
    auto sum = [](const std::vector<double>& v){ double s=0; for(double x:v) s+=x; return s; };

    const double pf = all.neg_sum > 0 ? all.pos_sum / all.neg_sum : (all.pos_sum > 0 ? 999.0 : 0.0);
    const double wr = all.n ? 100.0 * all.wins / all.n : 0.0;

    std::printf("\n=== %s NasTurtleD1 FAITHFUL (class-driven, 10yr daily, dpp=%.0f) ===\n", label.c_str(), dpp);
    std::printf("  trades        : %lld   WR %.1f%%\n", (long long)all.n, wr);
    std::printf("  net $         : %+.0f   (best %+.0f / worst %+.0f)\n", all.gross, all.best, all.worst);
    std::printf("  PROFIT FACTOR : %.2f\n", pf);
    std::printf("  maxDD $       : %+.0f\n", maxdd(all.pnl));
    std::printf("  exits         : SL=%lld TP=%lld WKND=%lld T/O=%lld other=%lld\n",
                (long long)all.sl, (long long)all.tp, (long long)all.wknd, (long long)all.to, (long long)all.other);
    std::printf("  --- regime split (net $ / n) ---\n");
    std::printf("    pre-2022 (bull): %+.0f / %lld\n", pre22.gross, (long long)pre22.n);
    std::printf("    2022 (BEAR)    : %+.0f / %lld   <- the gate\n", y2022.gross, (long long)y2022.n);
    std::printf("    2023-26 (bull) : %+.0f / %lld\n", post22.gross, (long long)post22.n);
    std::printf("  --- WF halves (net $) ---\n");
    std::printf("    H1: %+.0f   H2: %+.0f   %s\n", sum(first_half), sum(second_half),
                (sum(first_half) > 0 && sum(second_half) > 0) ? "BOTH+ " : "NOT both+");
    return 0;
}
