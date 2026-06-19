// trendrider_faithful.cpp -- faithful class-driven WF for TrendRiderPortfolio.
// Drives the REAL engine (include/TrendRiderEngine.hpp) bar-by-bar on H1 CSV.
// Cost enters faithfully via the half-spread offset on the bid/ask feed (the
// engine fills entry at ask / exit at bid, so tr.pnl already nets round-trip
// cost = 2*hs per pt * size). XAUUSD = $100/pt.
//
// Build (mac): g++ -std=c++20 -O2 -Iinclude backtest/trendrider_faithful.cpp -o /tmp/tr_bt
// Run:         /tmp/tr_bt <h1.csv> <LABEL> [risk_pct=0.0025] [half_spread=0.25]
//
// S-2026-06-19 Phase 1 item 3 gate: TrendRider live-size promotion requires a
// fresh 2022-current WF (both-halves + 2022 positive). This is that harness.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "TrendRiderEngine.hpp"

struct H1 { int64_t ts; double o, h, l, c; };

static std::vector<H1> load(const std::string& path) {
    std::vector<H1> v; std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return v; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!(line[0] == '-' || (line[0] >= '0' && line[0] <= '9'))) continue; // skip header
        H1 b; char* p = const_cast<char*>(line.c_str());
        b.ts = std::strtoll(p, &p, 10); if (*p != ',') continue; ++p;
        b.o  = std::strtod(p, &p);      if (*p != ',') continue; ++p;
        b.h  = std::strtod(p, &p);      if (*p != ',') continue; ++p;
        b.l  = std::strtod(p, &p);      if (*p != ',') continue; ++p;
        b.c  = std::strtod(p, &p);
        if (b.o > 0 && b.h > 0 && b.l > 0 && b.c > 0) v.push_back(b);
    }
    return v;
}

static int year_of(int64_t ts) { time_t t = ts; struct tm g; gmtime_r(&t, &g); return g.tm_year + 1900; }

struct Stats {
    int64_t n = 0, wins = 0; double pos = 0, neg = 0, gross = 0, best = 0, worst = 0;
    std::vector<double> pnl;
    void record(const omega::TradeRecord& tr) {
        const double v = tr.pnl * 100.0;   // XAUUSD $/pt
        ++n; gross += v; pnl.push_back(v);
        if (v > 0) { ++wins; pos += v; } else neg += -v;
        if (v > best) best = v; if (v < worst) worst = v;
    }
};

static double maxdd(const std::vector<double>& xs) {
    double eq = 0, pk = 0, dd = 0;
    for (double x : xs) { eq += x; if (eq > pk) pk = eq; if (pk - eq > dd) dd = pk - eq; }
    return -dd;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <h1.csv> <LABEL> [risk_pct=0.0025] [half_spread=0.25]\n", argv[0]); return 2; }
    const std::string path = argv[1], label = argv[2];
    const double rp = argc > 3 ? std::atof(argv[3]) : 0.0025;
    const double hs = argc > 4 ? std::atof(argv[4]) : 0.25;

    auto bars = load(path);
    if (bars.size() < 300) { std::fprintf(stderr, "[%s] only %zu bars -- abort\n", label.c_str(), bars.size()); return 1; }
    std::fprintf(stderr, "[%s] %zu H1 bars (%d..%d) rp=%.4f hs=%.2f\n",
                 label.c_str(), bars.size(), year_of(bars.front().ts), year_of(bars.back().ts), rp, hs);

    omega::TrendRiderPortfolio eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.risk_pct = rp; eng.start_equity = 10000.0; eng.max_lot_cap = 0.50;
    eng.block_on_risk_off = false;   // no macro feed in BT
    eng.init();

    Stats all, y22, pre, post;
    auto cb = [&](const omega::TradeRecord& tr) {
        all.record(tr); int y = year_of(tr.exitTs);
        if (y == 2022) y22.record(tr); else if (y < 2022) pre.record(tr); else post.record(tr);
    };

    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i]; const int64_t ms = b.ts * 1000LL;
        omega::TrBar tb; tb.bar_start_ms = ms; tb.open = b.o; tb.high = b.h; tb.low = b.l; tb.close = b.c;
        eng.on_h1_bar(tb, b.c - hs, b.c + hs, 0.0, ms, cb);
    }
    eng.force_close_all(bars.back().c - hs, bars.back().c + hs, bars.back().ts * 1000LL, cb);

    std::vector<double> h1v, h2v;
    for (size_t i = 0; i < all.pnl.size(); ++i) (i < all.pnl.size() / 2 ? h1v : h2v).push_back(all.pnl[i]);
    auto sum = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return s; };

    const double pf = all.neg > 0 ? all.pos / all.neg : (all.pos > 0 ? 999.0 : 0.0);
    const double wr = all.n ? 100.0 * all.wins / all.n : 0.0;

    std::printf("\n=== %s TrendRider FAITHFUL (class-driven, risk_pct=%.4f hs=%.2f) ===\n", label.c_str(), rp, hs);
    std::printf("  trades : %lld   WR %.1f%%\n", (long long)all.n, wr);
    std::printf("  net $  : %+.0f   (best %+.0f / worst %+.0f)\n", all.gross, all.best, all.worst);
    std::printf("  PF     : %.2f\n", pf);
    std::printf("  maxDD$ : %+.0f\n", maxdd(all.pnl));
    std::printf("  regime : pre22 %+.0f/%lld   2022 %+.0f/%lld   post22 %+.0f/%lld\n",
                pre.gross, (long long)pre.n, y22.gross, (long long)y22.n, post.gross, (long long)post.n);
    std::printf("  WF halves (trade-order): H1 %+.0f   H2 %+.0f   %s\n",
                sum(h1v), sum(h2v), (sum(h1v) > 0 && sum(h2v) > 0) ? "BOTH+" : "NOT both+");
    return 0;
}
