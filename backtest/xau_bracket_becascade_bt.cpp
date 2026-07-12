// ─────────────────────────────────────────────────────────────────────────────
// xau_bracket_becascade_bt — GOLD two-sided BRACKET + BE-CASCADE mimic stack
// (S-2026-07-12c, operator spec):
//
//   1. "movement" trigger: |close/close[W] - 1| >= thr while flat
//   2. place OCO BRACKET: buy-stop at c*(1+b), sell-stop at c*(1-b), TTL bars
//   3. first side to fill wins, other side CANCELLED
//   4. filled side = PARENT trade, rides to reversal (opposite W-move >= thr)
//   5. once parent covers BE (+be_bp), open MIMIC 1; each mimic covering ITS
//      BE (+be_bp) spawns the next (BE-cascade, arms {0.2,2,3,4,6,8}%)
//   6. each mimic: once mfe >= arm, clip on 50%-of-peak giveback (g50 rev-only,
//      the proven crypto config); ALL mimics + parent exit on reversal
//
// WHY: the long-only up-jump BE-cascade port (XS_BECASCADE_GOLD_INDEX_FINDINGS)
// passed every gate on gold but the random-entry control said the return was
// mostly 2023-26 bull BETA (z=1.1). The bracket makes the book TWO-SIDED so a
// bear fires the short leg — this BT answers "is gold protected from bear".
//
// Fidelity notes:
//   • H1 OHLC bars; signal + mimic management on bar CLOSE (same convention the
//     live crypto companion uses); bracket fills use H/L with OPEN-gap handling;
//     a bar that touches BOTH bracket sides = AMBIGUOUS -> bracket cancelled and
//     counted (no lookahead coin-flip).
//   • entries/exits next-open where the crypto BT used next-open (parent exit).
//   • cost per leg RT applied to every leg incl. parent; 2x-cost = full re-sim.
//   • This is a SimBook-style replica (the live UpJumpLadderCompanion header is
//     long-only, so it cannot drive the short side); if the verdict is WIRE,
//     the engine gets the usual byte-exact parity treatment then.
//
// Long-only control included: same datasets through the PRIOR methodology
// (up-move -> long parent + cascade, no shorts) so the bear-protection delta
// is shown, not asserted.
//
// Build: g++ -O2 -std=c++17 backtest/xau_bracket_becascade_bt.cpp -o backtest/xau_bracket_becascade_bt
// Run:   ./backtest/xau_bracket_becascade_bt   (datasets hardcoded, sweep inside)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

struct Bars { std::vector<double> o, h, l, c; std::vector<long long> ts; int N = 0; };

static Bars load(const std::string& path) {
    Bars b; std::ifstream f(path); std::string ln; std::getline(f, ln); // header
    while (std::getline(f, ln)) {
        std::stringstream ss(ln); std::string t; std::vector<double> v;
        long long ts = 0; int k = 0;
        while (std::getline(ss, t, ',')) { if (k == 0) ts = atoll(t.c_str()); else v.push_back(atof(t.c_str())); k++; }
        if (v.size() < 4) continue;
        b.ts.push_back(ts); b.o.push_back(v[0]); b.h.push_back(v[1]); b.l.push_back(v[2]); b.c.push_back(v[3]);
    }
    b.N = (int)b.c.size(); return b;
}

struct Leg { double epx, mfe = 0, arm; bool open = true; };
struct Res {
    int nwin = 0, nlong = 0, nshort = 0, namb = 0, nlegs = 0;
    double par_net = 0, par_long = 0, par_short = 0;
    double mim_net = 0, mim_long = 0, mim_short = 0;
    double gw = 0, gl = 0, maxdd = 0;
    std::vector<double> booked;   // clip-order nets, for WF halves
    double pf() const { return gl > 0 ? gw / gl : (gw > 0 ? 999 : 0); }
    double h1() const { double s=0; for (size_t k=0;k<booked.size()/2;k++) s+=booked[k]; return s; }
    double h2() const { double s=0; for (size_t k=booked.size()/2;k<booked.size();k++) s+=booked[k]; return s; }
};

struct P {
    int W = 240;            // movement window, H1 bars (~10 trading days)
    double thr = 0.02;      // movement + reversal threshold
    double b = 0.004;       // bracket offset from trigger close
    int ttl = 48;           // bracket pending TTL (bars)
    double be_bp = 20;      // BE-coverage that spawns the next mimic (bp)
    double gb = 0.50;       // per-mimic giveback of peak (g50)
    double rt_bp = 5.0;     // round-trip cost per leg
    bool long_only = false; // control mode: prior methodology, no bracket/shorts
    bool dirstop = false;   // variant: movement DIRECTION picks the side; single
                            // stop at +-b in that direction confirms (no OCO)
};

static Res run(const Bars& b, const P& p) {
    Res r; std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};
    double cum = 0, peak = 0;
    auto book = [&](double net_pct) {
        r.mim_net += net_pct;
        if (net_pct > 0) r.gw += net_pct; else r.gl -= net_pct;
        cum += net_pct; peak = std::max(peak, cum); r.maxdd = std::max(r.maxdd, peak - cum);
        r.booked.push_back(net_pct);
    };
    int i = p.W;
    while (i < b.N - 2) {
        double j = b.c[i] / b.c[i - p.W] - 1.0;
        int dir = 0; double epx = 0; int ei = -1;
        if (p.long_only) {
            if (j >= p.thr) { dir = +1; ei = i + 1; epx = b.o[ei]; }
        } else if (p.dirstop && std::fabs(j) >= p.thr) {
            int want = j > 0 ? +1 : -1;
            double st = b.c[i] * (1.0 + want * p.b);
            for (int k = i + 1; k <= std::min(i + p.ttl, b.N - 2); k++) {
                if (want > 0 && b.o[k] >= st)      { dir = +1; epx = b.o[k]; ei = k; }
                else if (want < 0 && b.o[k] <= st) { dir = -1; epx = b.o[k]; ei = k; }
                else if (want > 0 && b.h[k] >= st) { dir = +1; epx = st; ei = k; }
                else if (want < 0 && b.l[k] <= st) { dir = -1; epx = st; ei = k; }
                if (dir) break;
            }
            if (!dir) { i++; continue; }
        } else if (std::fabs(j) >= p.thr) {
            double bs = b.c[i] * (1.0 + p.b), ss = b.c[i] * (1.0 - p.b);
            for (int k = i + 1; k <= std::min(i + p.ttl, b.N - 2); k++) {
                if (b.o[k] >= bs)      { dir = +1; epx = b.o[k]; ei = k; }
                else if (b.o[k] <= ss) { dir = -1; epx = b.o[k]; ei = k; }
                else if (b.h[k] >= bs && b.l[k] <= ss) { r.namb++; break; }   // ambiguous bar
                else if (b.h[k] >= bs) { dir = +1; epx = bs; ei = k; }
                else if (b.l[k] <= ss) { dir = -1; epx = ss; ei = k; }
                if (dir) break;
            }
            if (!dir) { i++; continue; }   // TTL expired or ambiguous
        }
        if (!dir) { i++; continue; }

        r.nwin++; if (dir > 0) r.nlong++; else r.nshort++;
        std::vector<Leg> legs; size_t next_arm = 0; double par_mfe = 0;
        int x = ei;
        for (x = ei; x < b.N - 1; x++) {
            double fav_par = dir * (b.c[x] / epx - 1.0) * 1e4;   // bp
            par_mfe = std::max(par_mfe, fav_par);
            // spawn: parent covers BE -> mimic 1; mimic k covers BE -> mimic k+1
            bool spawn = false;
            if (next_arm == 0) spawn = (par_mfe >= p.be_bp);
            else if (next_arm < arms.size() && !legs.empty()) {
                Leg& prev = legs.back();
                double fav_prev = dir * (b.c[x] / prev.epx - 1.0) * 1e4;
                spawn = prev.open && fav_prev >= p.be_bp;
            }
            if (spawn && next_arm < arms.size())
                { legs.push_back({b.c[x], 0, arms[next_arm++]}); r.nlegs++; }
            // manage mimics: arm at tier, clip on g50 giveback of peak
            for (auto& lg : legs) {
                if (!lg.open) continue;
                double fav = dir * (b.c[x] / lg.epx - 1.0) * 100.0;  // pct
                lg.mfe = std::max(lg.mfe, fav);
                if (lg.mfe >= lg.arm && fav <= lg.mfe * (1.0 - p.gb)) {
                    lg.open = false;
                    double net = fav - p.rt_bp / 100.0;
                    book(net); if (dir > 0) r.mim_long += net; else r.mim_short += net;
                }
            }
            // reversal: opposite W-move
            double jx = b.c[x] / b.c[x - p.W] - 1.0;
            if ((dir > 0 && jx <= -p.thr) || (dir < 0 && jx >= p.thr)) { x++; break; }
        }
        int xi = std::min(x, b.N - 1);
        double xpx = b.o[xi];
        double pnet = dir * (xpx / epx - 1.0) * 100.0 - p.rt_bp / 100.0;
        r.par_net += pnet; if (dir > 0) r.par_long += pnet; else r.par_short += pnet;
        for (auto& lg : legs) if (lg.open) {
            double net = dir * (xpx / lg.epx - 1.0) * 100.0 - p.rt_bp / 100.0;
            book(net); if (dir > 0) r.mim_long += net; else r.mim_short += net;
        }
        i = xi + 1;
    }
    return r;
}

int main() {
    struct DS { const char* name; const char* path; };
    std::vector<DS> data = {
        {"2022-26(full)", "/Users/jo/Tick/XAUUSD_2022_2026.h1.csv"},
        {"2013(bear)",    "/Users/jo/Tick/XAU2013_bear_h1.csv"},
        {"2015(bear)",    "/Users/jo/Tick/XAU2015_bear_h1.csv"},
        {"2022(bear)",    "/Users/jo/Tick/XAU2022_bear_h1.csv"},
    };
    std::printf("%-14s %-9s %4s %3s %3s %3s | %8s %8s %8s | %8s %8s %8s %6s %8s %8s | %7s %7s | %8s\n",
        "dataset", "mode", "thr", "b", "nw", "amb", "parNet%", "parL%", "parS%",
        "mimNet%", "mimL%", "mimS%", "PF", "maxDD%", "2xcost%", "H1", "H2", "legs");
    for (auto& d : data) {
        Bars b = load(d.path);
        if (!b.N) { std::printf("%-14s LOAD FAIL\n", d.name); continue; }
        const char* we=getenv("XBW"); int Wo=we?atoi(we):240;
        for (double thr : {0.02, 0.03}) {
            for (double off : {0.003, 0.005}) {
                P p; p.W=Wo; p.thr = thr; p.b = off;
                Res r = run(b, p);
                P p2 = p; p2.rt_bp = 10.0; Res r2 = run(b, p2);
                std::printf("%-14s %-9s %3.0f%% %3.1f %3d %3d | %+8.1f %+8.1f %+8.1f | %+8.1f %+8.1f %+8.1f %6.2f %8.1f %+8.1f | %+7.1f %+7.1f | %5d\n",
                    d.name, "bracket", thr * 100, off * 1000, r.nwin, r.namb,
                    r.par_net, r.par_long, r.par_short,
                    r.mim_net, r.mim_long, r.mim_short, r.pf(), r.maxdd, r2.mim_net, r.h1(), r.h2(), r.nlegs);
            }
            for (double off : {0.003, 0.005}) {
                P p; p.thr = thr; p.b = off; p.dirstop = true;
                Res r = run(b, p);
                P p2 = p; p2.rt_bp = 10.0; Res r2 = run(b, p2);
                std::printf("%-14s %-9s %3.0f%% %3.1f %3d %3d | %+8.1f %+8.1f %+8.1f | %+8.1f %+8.1f %+8.1f %6.2f %8.1f %+8.1f | %+7.1f %+7.1f | %5d\n",
                    d.name, "dirstop", thr * 100, off * 1000, r.nwin, r.namb,
                    r.par_net, r.par_long, r.par_short,
                    r.mim_net, r.mim_long, r.mim_short, r.pf(), r.maxdd, r2.mim_net, r.h1(), r.h2(), r.nlegs);
            }
            // long-only control (prior methodology) at this thr
            P pl; pl.thr = thr; pl.long_only = true;
            Res rl = run(b, pl);
            P pl2 = pl; pl2.rt_bp = 10.0; Res rl2 = run(b, pl2);
            std::printf("%-14s %-9s %3.0f%%  -  %3d %3d | %+8.1f %+8.1f %+8.1f | %+8.1f %+8.1f %+8.1f %6.2f %8.1f %+8.1f | %+7.1f %+7.1f | %5d\n",
                d.name, "LONGONLY", thr * 100, rl.nwin, rl.namb,
                rl.par_net, rl.par_long, rl.par_short,
                rl.mim_net, rl.mim_long, rl.mim_short, rl.pf(), rl.maxdd, rl2.mim_net, rl.h1(), rl.h2(), rl.nlegs);
        }
        std::printf("\n");
    }
    return 0;
}
