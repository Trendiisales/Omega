// =============================================================================
// mgc_panic_port_bt.cpp -- faithful PORT TEST of GoldPanicBounceEngine onto MGC
// micro-gold FUTURES (30m continuous, 2024-06..2026-07).
//
// Drives the REAL engine class (include/GoldPanicBounceEngine.hpp) through its
// live on_tick path, exactly like backtest/clip_path_goldpanic.cpp does for XAU
// spot: each 30m bar becomes synthetic ticks in order o,h,l,c (c LAST) with
// bid=px-0.15 / ask=px+0.15 (spread 0.30 < SPREAD_CAP 0.80), ts advancing
// within the bar (+0s,+10s,+20s,+1799s). The engine aggregates to H1 internally
// (BAR_SECS=3600) and runs its real per-tick chandelier check (_manage_intrabar)
// + H1-close entry/TIME logic. Registry-sanctioned synth-tick trick: intrabar
// touches reproduced at 30m resolution.
//
// DRIVE FIDELITY NOTE: the engine's _accumulate drops the first tick that lands
// in a new H1 bucket (that tick only triggers the previous bar's close). To keep
// the H1 open faithful we feed the 'o' tick TWICE (+0s and +1s) -- the duplicate
// is a no-op within an hour and preserves 'o' across hour boundaries.
//
// CONFIG = live engine_init (L4608-4618): TREND_GATE=true, SLOPE_LB=200,
// SLOPE_MIN=0.0. Sensitivity row with TREND_GATE=false also reported (the
// original XAU validation figures predate the gate). gold_regime() is left
// unseeded -> long_blocked()=false throughout (same as the sanctioned
// goldpanic_intrabar_vs_h1 faithful harness: macro feed absent in BT; the gate
// can only ADD protection live).
//
// COST: 0.30 pts round-trip per trade debited on RAW (exit-entry) pts, plus a
// 2x stress at 0.60. Engine's baked COST_COVER_PTS only touches tr.pnl, which
// we ignore -- pts are recomputed from tr.exitPrice/tr.entryPrice, 1 contract.
//
// KNOWN XAU SPOT REFERENCE (engine header): XAU H1 24-26 PF 1.97 net +967pt
// n=113 WR28%, WF both halves + (PF1.82/2.01); at correct IBKR cost bull
// PF~1.80 both-halves+; 2022 bear PF 1.08 (breakeven). NOTE: this MGC window
// (2024-06..2026-07) has NO 2022-class bear -- the regime axis is unavailable.
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include \
//          backtest/mgc_panic_port_bt.cpp -o /tmp/mgc_panic_bt
// Do NOT commit results as live figures -- this is a port feasibility test.
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "GoldPanicBounceEngine.hpp"

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_csv(const char* path){
    std::vector<Bar> bars; std::ifstream f(path);
    if(!f){ std::fprintf(stderr,"cannot open %s\n",path); return bars; }
    std::string line; long skipped=0;
    while(std::getline(f,line)){
        if(line.empty() || !std::isdigit((unsigned char)line[0])) continue; // header
        Bar b; const char* p=line.c_str(); char* e;
        b.ts=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
        b.o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.c=std::strtod(p,&e);                       // trailing ,v ignored
        if(b.o<=0 || b.h<b.l || b.h<=0 || b.l<=0 || b.c<=0){ ++skipped; continue; }
        bars.push_back(b);
    }
    if(skipped) std::fprintf(stderr,"[load] skipped %ld malformed bars\n",skipped);
    return bars;
}

struct Tr { int64_t entryTs, exitTs; double raw_pts; std::string reason; };

struct Stats {
    int n=0, wins=0; double net=0, gw=0, gl=0, maxdd=0, cum=0, peak=0, worst=1e18;
    void add(double p){
        n++; net+=p; if(p>0){wins++;gw+=p;} else gl+=-p;
        if(p<worst)worst=p;
        cum+=p; if(cum>peak)peak=cum; if(peak-cum>maxdd)maxdd=peak-cum;
    }
    double pf() const { return gl>1e-9 ? gw/gl : (gw>0?999.0:0.0); }
    double wr() const { return n ? 100.0*wins/n : 0.0; }
};

// run the REAL engine over the 30m bars; return closed trades (raw pts, no cost)
static std::vector<Tr> run_engine(const std::vector<Bar>& B, bool trend_gate){
    const double half = 0.15;                       // spread 0.30
    std::vector<Tr> trades;
    omega::GoldPanicBounceEngine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.TREND_GATE = trend_gate;                    // live cfg = true (engine_init L4615)
    eng.TREND_SLOPE_LB = 200; eng.TREND_SLOPE_MIN = 0.0;
    eng.on_close_cb = [&trades](const omega::TradeRecord& tr){
        trades.push_back({tr.entryTs, tr.exitTs, tr.exitPrice - tr.entryPrice, tr.exitReason});
    };
    for(const Bar& b : B){
        const int64_t ms = b.ts * 1000LL;
        eng.on_tick(b.o-half, b.o+half, ms);        // boundary tick (dropped on new H1)
        eng.on_tick(b.o-half, b.o+half, ms+1000);   // duplicate 'o' -> preserves H1 open
        eng.on_tick(b.h-half, b.h+half, ms+10000);
        eng.on_tick(b.l-half, b.l+half, ms+20000);
        eng.on_tick(b.c-half, b.c+half, ms+1799000);// close LAST
    }
    return trades;
}

static void report(const char* label, const std::vector<Tr>& trades,
                   int64_t mid_ts, double cost_rt)
{
    Stats all, h1, h2;
    for(const auto& t : trades){
        const double p = t.raw_pts - cost_rt;
        all.add(p);
        (t.entryTs < mid_ts ? h1 : h2).add(p);
    }
    std::printf("| %-34s | %4d | %5.1f | %5.2f | %+9.1f | %8.1f | %+8.1f | %+8.1f | %+8.1f |\n",
        label, all.n, all.wr(), all.pf(), all.net, all.maxdd,
        h1.net, h2.net, (all.n?all.worst:0.0));
    std::printf("|   halves: H1 n=%d PF=%.2f / H2 n=%d PF=%.2f %*s|\n",
        h1.n, h1.pf(), h2.n, h2.pf(), 62, "");
}

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "/Users/jo/Tick/mgc_30m_hist.csv";
    auto B = load_csv(path);
    std::fprintf(stderr,"[data] %zu 30m bars  %lld..%lld\n",
        B.size(), B.empty()?0LL:(long long)B.front().ts, B.empty()?0LL:(long long)B.back().ts);
    if(B.size() < 2000){ std::fprintf(stderr,"data too small\n"); return 1; }
    const int64_t mid_ts = B.front().ts + (B.back().ts - B.front().ts)/2;

    auto live = run_engine(B, /*trend_gate=*/true);   // live cfg
    auto nog  = run_engine(B, /*trend_gate=*/false);  // sensitivity (pre-gate cfg)

    std::printf("\nGoldPanicBounceEngine PORT TEST -> MGC micro-gold futures 30m (synth-tick drive)\n");
    std::printf("data=%s  bars=%zu  cost_rt=0.30pt (stress 0.60)  pts per 1 contract\n", path, B.size());
    std::printf("| config                             |    n |   WR%% |    PF |   net pts | maxDD pt |  H1 pts  |  H2 pts  |  worst   |\n");
    std::printf("|------------------------------------|------|-------|-------|-----------|----------|----------|----------|----------|\n");
    report("LIVE cfg (TREND_GATE on), 0.30 RT",  live, mid_ts, 0.30);
    report("LIVE cfg, 2x cost stress 0.60 RT",   live, mid_ts, 0.60);
    report("no trend gate (pre-gate cfg), 0.30", nog,  mid_ts, 0.30);
    report("no trend gate, 2x cost 0.60",        nog,  mid_ts, 0.60);

    std::printf("\ntrades (LIVE cfg): entryTs,exitTs,raw_pts,reason\n");
    for(const auto& t : live)
        std::printf("  %lld,%lld,%+8.1f,%s\n",(long long)t.entryTs,(long long)t.exitTs,t.raw_pts,t.reason.c_str());
    return 0;
}
