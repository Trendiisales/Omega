// =============================================================================
// survivor_faithful_revalidate.cpp -- ENGINE-FAITHFUL tick BT for g_survivor
// (SurvivorPortfolio). Drives the REAL omega::survivor::Portfolio class with the
// PRODUCTION config from engine_init.hpp:
//   init_default_cells()  (5 ENABLED prod cells: XAU DonchN20/N100, XAU MA10_30,
//                          USTEC RSI_N7, USTEC ZMR -- reclaim_exit baked into the
//                          Donchian cell cfgs)
//   dedup_mode = 1        (blanket per-(symbol,side) cap, S-2026-06-17 prod)
//
// Feeds each symbol's H4 bars as 4 sub-ticks (O,H,L,C) -- same intrabar pattern as
// survivor_cap_test.cpp -- so the real ATR / intrabar SL/TP / reclaim logic runs.
// tr.pnl from the engine is RAW pts*lot; we apply tick_value_multiplier per symbol
// (XAU 100, USTEC 20) to get USD, then subtract a per-symbol round-trip cost.
//
// Reports per-cell + book-level: n, WR, PF, net, both walk-forward halves, a cost
// sweep, and the top-3 fat-tail share.
//
// build: g++ -std=c++17 -O2 -Iinclude backtest/survivor_faithful_revalidate.cpp \
//          -o backtest/survivor_faithful_revalidate
// run:   backtest/survivor_faithful_revalidate <xau_h4.csv> <ustec_h4.csv> [spx_h4.csv]
// =============================================================================
#include "SurvivorPortfolio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Tick { long long ts_ms; std::string sym; double px; };

// load O,H,L,C bar csv -> 4 sub-ticks/bar. Handles header (ts in s or ms auto).
static int load_bars(const std::string& path, const std::string& sym,
                     std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f) { std::printf("[load] CANNOT OPEN %s\n", path.c_str()); return 0; }
    std::string line; bool first = true; int n = 0;
    while (std::getline(f, line)) {
        if (first) { first = false;
            if (!line.empty() && (line[0] < '0' || line[0] > '9') && line[0] != '-') continue; }
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss, t, ',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        long long ts = std::atoll(tok[0].c_str());
        long long ts_ms = ts > 100000000000LL ? ts : ts * 1000LL;  // s vs ms autodetect
        double o = std::atof(tok[1].c_str()), h = std::atof(tok[2].c_str());
        double l = std::atof(tok[3].c_str()), c = std::atof(tok[4].c_str());
        if (h <= 0) continue;
        out.push_back({ ts_ms + 0,   sym, o });
        out.push_back({ ts_ms + 250, sym, h });
        out.push_back({ ts_ms + 500, sym, l });
        out.push_back({ ts_ms + 750, sym, c });
        ++n;
    }
    std::printf("[load] %-10s %6d bars  %s\n", sym.c_str(), n, path.c_str());
    return n;
}

// tick_value_multiplier mirror (ledger applies this to tr.pnl raw pts*lot)
static double tick_mult(const std::string& sym) {
    if (sym == "XAUUSD")  return 100.0;
    if (sym == "USTEC.F" || sym == "USTEC") return 20.0;
    if (sym == "USDJPY")  return 667.0;
    if (sym == "GER40")   return 1.10;
    return 100.0;
}
// per-symbol realistic round-trip cost in USD at lot=0.01/0.10 (spread+comm).
// XAU lot0.01: ~0.50pt spread * (100*0.01=1/pt) = $0.50 ; USTEC lot0.10:
// ~2pt spread * (20*0.10=2/pt) = $4.00. base=IBKR-ish; *stress multiplier.
static double rt_cost_usd(const std::string& sym, double mult) {
    double base = 0.50;
    if (sym == "USTEC.F" || sym == "USTEC") base = 4.00;
    return base * mult;
}

struct Agg { int n=0, wins=0; double net=0, gw=0, gl=0; std::vector<double> v; };
static void add(Agg& a, double pnl) {
    a.n++; a.net += pnl; a.v.push_back(pnl);
    if (pnl >= 0) { a.wins++; a.gw += pnl; } else a.gl += -pnl;
}
static double pf(const Agg& a){ return a.gl>0 ? a.gw/a.gl : (a.gw>0?99.0:0.0); }
static double wr(const Agg& a){ return a.n>0 ? 100.0*a.wins/a.n : 0.0; }
static double top3share(Agg a){
    if (a.v.empty() || a.net <= 0) return 0;
    std::sort(a.v.begin(), a.v.end(), std::greater<double>());
    double s=0; for (int i=0;i<3 && i<(int)a.v.size();++i) s+=a.v[i];
    return 100.0*s/a.net;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: %s xau_h4.csv ustec_h4.csv [spx_h4.csv]\n", argv[0]); return 1; }
    std::vector<Tick> ticks;
    load_bars(argv[1], "XAUUSD",  ticks);
    load_bars(argv[2], "USTEC.F", ticks);
    std::sort(ticks.begin(), ticks.end(),
              [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    if (ticks.empty()) { std::printf("no ticks\n"); return 1; }
    long long tmin = ticks.front().ts_ms, tmax = ticks.back().ts_ms;
    long long tmid = (tmin + tmax) / 2;
    std::printf("[tape] %zu ticks  range %lld..%lld  mid=%lld\n\n",
                ticks.size(), tmin/1000, tmax/1000, tmid/1000);

    // PRODUCTION engine
    omega::survivor::Portfolio p;
    p.init_default_cells();
    p.dedup_mode = 1;        // S-2026-06-17 prod: blanket cap
    if (argc >= 4) p.spx.seed_from_csv(argv[3]);

    struct Raw { std::string cell, sym, side; double rawpts_x_lot; long long exitTs; double mfe; };
    std::vector<Raw> trades;
    auto cb = [&](const omega::TradeRecord& tr) {
        trades.push_back({ tr.engine, tr.symbol, tr.side, tr.pnl, tr.exitTs, tr.mfe });
    };
    for (const auto& tk : ticks) p.on_tick(tk.sym, tk.px, tk.px, tk.ts_ms, cb);

    std::printf("\n[engine] cells=%zu  trades=%zu\n", p.cells.size(), trades.size());
    std::printf("enabled prod cells:");
    for (auto& c : p.cells) std::printf(" %s", c.cfg.tag);
    std::printf("\n\n");

    // ---- cost sweep at book level ----
    std::printf("================ BOOK-LEVEL COST SWEEP (USD net) ================\n");
    std::printf("%-12s %8s %8s %10s %10s\n", "cost_mult", "n", "WR%", "PF", "net$");
    for (double m : {0.0, 0.5, 1.0, 1.5, 2.0, 3.0}) {
        Agg b;
        for (auto& t : trades) {
            double usd = t.rawpts_x_lot * tick_mult(t.sym) - rt_cost_usd(t.sym, m);
            add(b, usd);
        }
        std::printf("x%-11.1f %8d %8.1f %10.2f %+10.0f\n", m, b.n, wr(b), pf(b), b.net);
    }

    // ---- per-cell @ baseline cost (x1.0 = ~IBKR) ----
    auto cost_mult = 1.0;
    std::printf("\n================ PER-CELL @ baseline cost (x1.0 ~IBKR) ================\n");
    std::printf("%-20s %5s %6s %6s %9s %9s %9s %7s\n",
                "cell","n","WR%","PF","net$","H1net","H2net","top3%");
    std::map<std::string,Agg> by_cell, h1, h2;
    Agg book, bh1, bh2;
    for (auto& t : trades) {
        double usd = t.rawpts_x_lot * tick_mult(t.sym) - rt_cost_usd(t.sym, cost_mult);
        add(by_cell[t.cell], usd); add(book, usd);
        if (t.exitTs*1000LL < tmid) { add(h1[t.cell], usd); add(bh1, usd); }
        else                        { add(h2[t.cell], usd); add(bh2, usd); }
    }
    for (auto& kv : by_cell) {
        auto& a = kv.second;
        std::printf("%-20s %5d %6.1f %6.2f %+9.0f %+9.0f %+9.0f %6.0f%%\n",
            kv.first.c_str(), a.n, wr(a), pf(a), a.net,
            h1[kv.first].net, h2[kv.first].net, top3share(a));
    }
    std::printf("%-20s %5d %6.1f %6.2f %+9.0f %+9.0f %+9.0f %6.0f%%\n",
        "** BOOK **", book.n, wr(book), pf(book), book.net, bh1.net, bh2.net, top3share(book));

    return 0;
}
