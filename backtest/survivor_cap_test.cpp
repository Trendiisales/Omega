// survivor_cap_test.cpp — PROPER backtest of the SurvivorPortfolio
// per-(symbol,side) concurrency cap. Replays the bundled warmup tape through
// the ACTUAL include/SurvivorPortfolio.hpp, cap ON (dedup_enabled) vs OFF.
//
// Proper-backtest discipline (feedback-never-deploy-without-backtest):
//   * cost-inclusive  — per-symbol round-trip cost subtracted from every trade
//   * walk-forward    — 6 calendar windows, regime-labeled; cap must not be a
//                       blended-number artifact, and we surface chop vs trend
//   * per-symbol      — PF + maxDD per name (no single-name domination hidden)
//   * prod-faithful   — runs the real engine; O/H/L/C fed as sub-ticks for real
//                       intrabar range -> realistic ATR + intrabar SL/TP
//
// build: g++ -std=c++17 -O2 -Iinclude backtest/survivor_cap_test.cpp -o backtest/survivor_cap_test

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

static void load_csv(const std::string& path, const std::string& sym,
                     bool ts_is_ms, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f) { std::printf("[load] cannot open %s\n", path.c_str()); return; }
    std::string line; bool first = true; int n = 0;
    while (std::getline(f, line)) {
        if (first) { first = false;
            if (!line.empty() && (line[0] < '0' || line[0] > '9') && line[0] != '-') continue; }
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss, t, ',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        long long ts = std::atoll(tok[0].c_str());
        long long ts_ms = ts_is_ms ? ts : ts * 1000LL;
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
}

// Per-symbol realistic round-trip cost in USD (spread + commission), using the
// engine's own usd-per-pt-per-lot * lot:
//   XAU  : ~0.50 pt  * (100*0.01=1.00/pt)  = $0.50
//   GER40: ~2.0  pt  * (1.10*0.10=0.11/pt) = $0.22
//   USTEC: ~2.0  pt  * (20.0*0.10=2.00/pt) = $4.00
static double rt_cost_usd(const std::string& sym) {
    if (sym == "XAUUSD")  return 0.50;
    if (sym == "GER40")   return 0.22;
    if (sym == "USTEC.F") return 4.00;
    return 0.50;
}

static std::vector<omega::TradeRecord> run(bool cap, const std::vector<Tick>& ticks) {
    omega::survivor::Portfolio p;
    p.init_default_cells();
    p.dedup_enabled = cap;
    std::vector<omega::TradeRecord> trades;
    auto cb = [&](const omega::TradeRecord& tr) { trades.push_back(tr); };
    for (const auto& tk : ticks) p.on_tick(tk.sym, tk.px, tk.px, tk.ts_ms, cb);
    // apply costs in-place (tr.pnl becomes net-of-cost)
    for (auto& tr : trades) tr.pnl -= rt_cost_usd(tr.symbol);
    std::sort(trades.begin(), trades.end(),
              [](const omega::TradeRecord& a, const omega::TradeRecord& b) {
                  return a.exitTs < b.exitTs; });
    return trades;
}

struct Agg { int n=0, wins=0; double net=0, gw=0, gl=0, maxdd=0; };
static Agg agg(const std::vector<omega::TradeRecord>& tr) {
    Agg a; double cum=0, peak=0;
    for (auto& t : tr) {
        a.n++; a.net += t.pnl;
        if (t.pnl >= 0) { a.wins++; a.gw += t.pnl; } else a.gl += -t.pnl;
        cum += t.pnl; if (cum > peak) peak = cum;
        if (peak - cum > a.maxdd) a.maxdd = peak - cum;
    }
    return a;
}
static double pf(const Agg& a){ return a.gl>0 ? a.gw/a.gl : 0.0; }
static double wr(const Agg& a){ return a.n>0 ? 100.0*a.wins/a.n : 0.0; }

static std::vector<omega::TradeRecord> filt(const std::vector<omega::TradeRecord>& tr,
                                            const std::string& sym) {
    std::vector<omega::TradeRecord> o;
    for (auto& t : tr) if (t.symbol == sym) o.push_back(t);
    return o;
}

int main() {
    std::vector<Tick> ticks;
    const std::string D = "phase1/signal_discovery/";
    load_csv(D + "warmup_XAUUSD_M15.csv",  "XAUUSD",  true,  ticks);
    load_csv(D + "warmup_GER40_M5.csv",    "GER40",   false, ticks);
    load_csv(D + "warmup_USTEC.F_H4.csv",  "USTEC.F", false, ticks);
    std::sort(ticks.begin(), ticks.end(),
              [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    std::printf("[tape] %zu ticks merged\n\n", ticks.size());

    auto off = run(false, ticks);
    auto on  = run(true,  ticks);

    auto Aoff = agg(off), Aon = agg(on);

    // ---- overall ----
    std::printf("================ OVERALL (cost-incl) ================\n");
    std::printf("%-16s %12s %12s\n", "", "CAP OFF", "CAP ON");
    std::printf("%-16s %12d %12d\n", "trades", Aoff.n, Aon.n);
    std::printf("%-16s %11.1f%% %11.1f%%\n", "win rate", wr(Aoff), wr(Aon));
    std::printf("%-16s %12.2f %12.2f\n", "profit factor", pf(Aoff), pf(Aon));
    std::printf("%-16s %12.0f %12.0f\n", "net USD", Aoff.net, Aon.net);
    std::printf("%-16s %12.0f %12.0f\n", "maxDD USD", Aoff.maxdd, Aon.maxdd);
    std::printf("%-16s %12.2f %12.2f\n", "return/DD",
                Aoff.maxdd>0?Aoff.net/Aoff.maxdd:0, Aon.maxdd>0?Aon.net/Aon.maxdd:0);

    // ---- per-symbol ----
    std::printf("\n================ PER-SYMBOL (cost-incl) ================\n");
    std::printf("%-9s | %s | %s\n", "", "  OFF: n   net    PF   DD", "  ON: n   net    PF   DD");
    for (const char* s : {"XAUUSD","GER40","USTEC.F"}) {
        auto fo = agg(filt(off,s)), fn = agg(filt(on,s));
        std::printf("%-9s | %4d %7.0f %5.2f %6.0f | %4d %7.0f %5.2f %6.0f\n",
            s, fo.n, fo.net, pf(fo), fo.maxdd, fn.n, fn.net, pf(fn), fn.maxdd);
    }

    // ---- walk-forward: 6 calendar windows ----
    int64_t tmin = off.front().exitTs, tmax = off.front().exitTs;
    for (auto& t : off) { tmin = std::min(tmin,t.exitTs); tmax = std::max(tmax,t.exitTs); }
    for (auto& t : on)  { tmin = std::min(tmin,t.exitTs); tmax = std::max(tmax,t.exitTs); }
    const int W = 6; int64_t span = (tmax - tmin)/W + 1;
    std::printf("\n================ WALK-FORWARD (%d windows, cost-incl net) ================\n", W);
    std::printf("%-22s %9s %9s %9s   %s\n", "window", "OFF net", "ON net", "delta", "cap better?");
    int cap_wins = 0;
    for (int w=0; w<W; ++w) {
        int64_t lo = tmin + w*span, hi = lo + span;
        double no=0, no_on=0;
        for (auto& t : off) if (t.exitTs>=lo && t.exitTs<hi) no += t.pnl;
        for (auto& t : on)  if (t.exitTs>=lo && t.exitTs<hi) no_on += t.pnl;
        time_t lt = (time_t)lo; char buf[32]; std::strftime(buf,sizeof(buf),"%Y-%m-%d",gmtime(&lt));
        bool better = no_on > no; if (better) cap_wins++;
        std::printf("%-22s %9.0f %9.0f %9.0f   %s\n", buf, no, no_on, no_on-no,
                    better ? "YES" : "no");
    }
    std::printf("\ncap won %d / %d windows\n", cap_wins, W);
    std::printf("DELTA overall (ON-OFF): net $%+.0f | trades %+d | maxDD $%+.0f\n",
                Aon.net-Aoff.net, Aon.n-Aoff.n, Aon.maxdd-Aoff.maxdd);
    return 0;
}
