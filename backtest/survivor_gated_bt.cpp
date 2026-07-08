// =============================================================================
// survivor_gated_bt.cpp -- SurvivorPortfolio bear-chokepoint gate BT (S-2026-07-08c)
//
// Tombstone valid-use follow-up. g_survivor was tombstoned 2026-06-24:
//   "bull-only (faithful bear PF0.81; USTEC_RSI_N7 = bull-driver/bear-loss) AND
//    was fully un-gated (self-enters, bypassed the bear chokepoint)".
// This drives the REAL omega::survivor::Portfolio (prod cells, dedup_mode=1) over
// 2022-2026 XAUUSD + USTEC(NSXUSD) H4 tape and tests the NEW Portfolio::entry_veto
// hook -- the same hook live wiring uses -- with an asym sustained-bear veto
// (daily close<SMA200 AND SMA falling for K=20 consecutive days blocks NEW LONG
// entries of that symbol; shorts + exits unaffected). Same gate family as
// index_market_regime().long_blocked() / NasTurtleD1Gated's 200DMA proxy.
//
// Variants: OFF | USTEC-veto | USTEC+XAU-veto.  USTEC daily SMA seeded from NDX
// daily 2016+ so the veto is hot from the 2022 tape start.
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include backtest/survivor_gated_bt.cpp -o /tmp/survivor_gated
// run:   /tmp/survivor_gated /Users/jo/Tick/XAUUSD_2022_2026.h4.csv /Users/jo/Tick/NSXUSD_2022_2026.h4.csv /Users/jo/Tick/NDX_daily_2016_2026.csv
// =============================================================================
#include "SurvivorPortfolio.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Tick { long long ts_ms; std::string sym; double px; };

static int load_bars(const std::string& path, const std::string& sym, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f) { std::printf("[load] CANNOT OPEN %s\n", path.c_str()); return 0; }
    std::string line; int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || (line[0] < '0' || line[0] > '9')) continue;
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss, t, ',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        long long ts = std::atoll(tok[0].c_str());
        long long ts_ms = ts > 100000000000LL ? ts : ts * 1000LL;
        double o = std::atof(tok[1].c_str()), h = std::atof(tok[2].c_str());
        double l = std::atof(tok[3].c_str()), c = std::atof(tok[4].c_str());
        if (h <= 0) continue;
        out.push_back({ ts_ms + 0,   sym, o });
        out.push_back({ ts_ms + 250, sym, h });
        out.push_back({ ts_ms + 500, sym, l });
        out.push_back({ ts_ms + 750, sym, c });
        ++n;
    }
    std::printf("[load] %-8s %6d bars  %s\n", sym.c_str(), n, path.c_str());
    return n;
}

// --- asym sustained-bear regime state, per symbol, daily granularity ---------
struct BearState {
    std::deque<double> closes;          // daily closes (<=210 kept)
    double prev_sma = 0;
    int    k_bear   = 0;                // consecutive sustained-bear days
    bool   bear     = false;
    long long cur_day = -1;
    double  day_close = 0;
    void push_daily(double c) {
        closes.push_back(c);
        if (closes.size() > 210) closes.pop_front();
        if (closes.size() < 200) { prev_sma = 0; return; }
        double s = 0; for (size_t i = closes.size() - 200; i < closes.size(); ++i) s += closes[i];
        double sma = s / 200.0;
        if (prev_sma > 0 && c < sma && sma < prev_sma) k_bear++; else k_bear = 0;
        bear = (k_bear >= 20);
        prev_sma = sma;
    }
    // feed a tick; rolls the day on UTC-midnight change using the PRIOR day's close
    void on_tick(long long ts_ms, double px) {
        long long day = (ts_ms / 1000) / 86400;
        if (cur_day < 0) { cur_day = day; day_close = px; return; }
        if (day != cur_day) { push_daily(day_close); cur_day = day; }
        day_close = px;
    }
};

struct Closed { long long entry_ts; std::string cell, sym; double usd_gross_pts_lot; };

static double tick_mult(const std::string& sym) {
    if (sym == "XAUUSD") return 100.0;
    if (sym == "USTEC.F" || sym == "USTEC") return 20.0;
    return 100.0;
}
static double rt_cost_usd(const std::string& sym, double mult) {
    double base = 0.50;                       // XAU lot0.01
    if (sym == "USTEC.F" || sym == "USTEC") base = 4.00;  // USTEC lot0.10
    return base * mult;
}

struct Agg { int n=0,w=0; double net=0,gw=0,gl=0; std::vector<double> v; };
static void add(Agg& a, double p){ a.n++; a.net+=p; a.v.push_back(p); if(p>=0){a.w++;a.gw+=p;} else a.gl+=-p; }
static double pf(const Agg& a){ return a.gl>0? a.gw/a.gl : (a.gw>0?99.0:0.0); }
static double wr(const Agg& a){ return a.n? 100.0*a.w/a.n : 0.0; }
static double top3(Agg a){ if(a.v.empty()||a.net<=0) return 0; std::sort(a.v.begin(),a.v.end(),std::greater<double>()); double s=0; for(int i=0;i<3&&i<(int)a.v.size();++i)s+=a.v[i]; return 100.0*s/a.net; }
static int year_of(long long sec){ std::time_t t=(std::time_t)sec; std::tm g{}; gmtime_r(&t,&g); return g.tm_year+1900; }

static std::vector<Closed> run(const std::vector<Tick>& ticks, int veto_mode,
                               const std::vector<double>& ndx_seed) {
    omega::survivor::Portfolio p;
    p.init_default_cells();
    p.dedup_mode = 1;
    if (veto_mode == 3)                       // veto + cull the kill-reason cell
        for (auto& c : p.cells)
            if (std::strcmp(c.cfg.tag, "USTEC_4h_RSI_N7") == 0) c.st.enabled = false;
    std::map<std::string, BearState> reg;
    for (double c : ndx_seed) reg["USTEC.F"].push_daily(c);   // veto hot at tape start
    if (veto_mode > 0) {
        p.entry_veto = [&reg, veto_mode](const char* sym, int side) -> bool {
            if (side <= 0) return false;                       // longs only
            if (std::strcmp(sym, "USTEC.F") == 0) return reg["USTEC.F"].bear;
            if (veto_mode >= 2 && std::strcmp(sym, "XAUUSD") == 0) {
                auto it = reg.find("XAUUSD");
                return it != reg.end() && it->second.bear;
            }
            return false;
        };
    }
    std::vector<Closed> out;
    auto cb = [&](const omega::TradeRecord& tr) {
        out.push_back({ tr.entryTs, tr.engine, tr.symbol, tr.pnl });
    };
    for (const auto& tk : ticks) {
        reg[tk.sym].on_tick(tk.ts_ms, tk.px);
        p.on_tick(tk.sym, tk.px, tk.px, tk.ts_ms, cb);
    }
    return out;
}

static void report(const char* tag, const std::vector<Closed>& tr, long long tmid_s) {
    std::printf("\n===== %s =====\n", tag);
    for (double m : {1.0, 2.0}) {
        Agg all, bull, bear, h1, h2;
        std::map<std::string, Agg> cells;
        for (const auto& t : tr) {
            double usd = t.usd_gross_pts_lot * tick_mult(t.sym) - rt_cost_usd(t.sym, m);
            add(all, usd);
            int y = year_of(t.entry_ts);
            if (y == 2022) add(bear, usd);
            if (y >= 2024) add(bull, usd);
            if (t.entry_ts < tmid_s) add(h1, usd); else add(h2, usd);
            if (m == 1.0) add(cells[t.cell], usd);
        }
        std::printf("cost x%.0f: ALL n=%d WR=%.1f%% PF=%.2f net=%+.0f | BULL n=%d PF=%.2f %+.0f | BEAR22 n=%d PF=%.2f %+.0f | H1 %+.0f / H2 %+.0f both+=%s | top3=%.0f%%\n",
            m, all.n, wr(all), pf(all), all.net,
            bull.n, pf(bull), bull.net, bear.n, pf(bear), bear.net,
            h1.net, h2.net, (h1.net>0&&h2.net>0)?"YES":"NO", top3(all));
        if (m == 1.0)
            for (auto& kv : cells)
                std::printf("    %-20s n=%3d WR=%5.1f%% PF=%5.2f net=%+8.0f\n",
                    kv.first.c_str(), kv.second.n, wr(kv.second), pf(kv.second), kv.second.net);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: %s xau_h4 ustec_h4 ndx_daily\n", argv[0]); return 1; }
    std::vector<Tick> ticks;
    load_bars(argv[1], "XAUUSD", ticks);
    load_bars(argv[2], "USTEC.F", ticks);
    std::stable_sort(ticks.begin(), ticks.end(),
        [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    // NDX daily closes strictly BEFORE the tape start -> seed the USTEC bear SMA
    long long t0_s = ticks.front().ts_ms / 1000;
    std::vector<double> seed;
    { std::ifstream f(argv[3]); std::string ln;
      while (std::getline(f, ln)) {
          long long ts; double o,h,l,c;
          if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c)==5 && c>0 && ts < t0_s)
              seed.push_back(c);
      } }
    std::printf("[seed] USTEC daily closes pre-tape: %zu\n", seed.size());
    long long tmid_s = (ticks.front().ts_ms/1000 + ticks.back().ts_ms/1000) / 2;

    auto off  = run(ticks, 0, seed);
    auto gate = run(ticks, 1, seed);
    auto both = run(ticks, 2, seed);
    auto cut  = run(ticks, 3, seed);
    report("GATE=OFF (as-culled config)", off, tmid_s);
    report("GATE=USTEC long-veto (asym sustained-bear, seeded NDX 2016+)", gate, tmid_s);
    report("GATE=USTEC+XAU long-veto", both, tmid_s);
    report("GATE=USTEC+XAU veto + USTEC_RSI_N7 culled (kill-reason cell)", cut, tmid_s);
    return 0;
}
