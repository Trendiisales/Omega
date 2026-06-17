// faithful_engine_bt_TEMPLATE.cpp — THE arbiter pattern (per BACKTEST_TRUTH.md §3).
// Drives the REAL engine class tick-by-tick over m1 OHLC, so the deploy decision is made on
// exactly the code that ships — not a bar-replay re-implementation (which overstates ~0.5-0.7 PF).
//
// Reference implementation: fvg_engine_bt.cpp (drives omega::FvgContinuationEngine).
// To validate a NEW engine, clone this file and change ONLY the 4 marked <<EDIT>> spots.
//
// build: g++ -std=c++17 -O2 -I../include faithful_engine_bt_<ENGINE>.cpp -o faithful_<ENGINE>
// run:   ./faithful_<ENGINE> <label> <m1_block1.csv> [block2.csv ...]   # one csv per regime
//        (cross-regime gate: a config is only deploy-worthy if net>0 AND both-WF-halves>0 in
//         EVERY regime block — see HARNESS_FIDELITY_CHECKLIST.md.)
//
// WHY tick-feed o,h,l,c with conservative intrabar order: the live engine sees ticks, not bars.
// Feeding the ADVERSE extreme before the favorable one (bullish bar: o,l,h,c / bearish: o,h,l,c)
// removes the long-favoring "high-before-low" bias and refuses the within-bar look-ahead that
// makes bar-replay harnesses lie. Still coarse (4 ticks/bar) — it is a FLOOR on honesty, not a
// ceiling. If it says net-negative, the edge is not real. (Live density helps fills but also
// adds slippage this under-models — the disagreement is the signal: do not deploy into it.)

#include "FvgContinuationEngine.hpp"      // <<EDIT 1>> the engine header under test
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace std;

// Most engines reference g_open_positions (ClusterGate). Define it once; empty registry = allow.
omega::OpenPositionRegistry g_open_positions;   // <<EDIT 2>> add other required globals if the
                                                //   linker complains (each names one missing symbol)

struct Bar { long long ts; double o,h,l,c; };
static vector<Bar> load(const string& p){
    vector<Bar> v; ifstream f(p); if(!f) return v; string ln; bool first=true;
    while(getline(f,ln)){ if(ln.empty())continue;
        if(first){first=false; if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
        Bar b; if(sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5 && b.h>=b.l) v.push_back(b); }
    return v;
}

struct Res { int n=0,w=0; double net=0,gw=0,gl=0,h1=0,h2=0; };
static Res run_block(const vector<Bar>& bars){
    // <<EDIT 3>> instantiate + configure the engine to the EXACT config you want to deploy.
    omega::FvgContinuationEngine e;
    e.symbol="NAS100"; e.engine_name="BT"; e.enabled=true; e.shadow_mode=true; e.verbose=false;
    // ... set every config field to the deploy config; disable live-feed gates that can't run in
    //     backtest (e.g. macro risk-off) only if they are NOT part of the edge being tested ...
    e.init();

    Res r; size_t mid=bars.size()/2;
    long long mid_ts = bars.empty()?0:bars[mid].ts;
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        r.n++; r.net+=tr.pnl; if(tr.pnl>=0){r.w++;r.gw+=tr.pnl;} else r.gl+=-tr.pnl;
        if(tr.entryTs < mid_ts) r.h1+=tr.pnl; else r.h2+=tr.pnl;   // walk-forward halves
    };
    const double SPR=0.5;   // <<EDIT 4>> half-spread proxy in PRICE units for this instrument
    for(size_t i=0;i<bars.size();++i){ const Bar&b=bars[i]; long long ms=b.ts*1000;
        e.on_tick(b.o-SPR,b.o+SPR,ms);                              // open
        if (b.c>=b.o){ e.on_tick(b.l-SPR,b.l+SPR,ms); e.on_tick(b.h-SPR,b.h+SPR,ms); }  // adverse first
        else         { e.on_tick(b.h-SPR,b.h+SPR,ms); e.on_tick(b.l-SPR,b.l+SPR,ms); }
        e.on_tick(b.c-SPR,b.c+SPR,ms);                              // close
    }
    if(!bars.empty()) e.force_close(bars.back().c-SPR,bars.back().c+SPR,(long long)bars.back().ts*1000);
    return r;
}

int main(int argc,char**argv){
    if(argc<3){ printf("usage: %s <label> <m1_block1.csv> [block2.csv ...]\n",argv[0]); return 1; }
    printf("=== %s | FAITHFUL (real engine class) ===\n",argv[1]);
    printf("%-18s %-6s %-7s %-7s %-9s %-9s %-9s %s\n","block","n","win%","PF","net","H1","H2","both+");
    bool allpos=true;
    for(int a=2;a<argc;++a){
        auto bars=load(argv[a]); if(bars.size()<200){ printf("  %-18s (few bars)\n",argv[a]); allpos=false; continue; }
        Res r=run_block(bars); double pf=r.gl>0?r.gw/r.gl:(r.gw>0?99:0);
        const char* nm=strrchr(argv[a],'/'); nm=nm?nm+1:argv[a];
        bool bp=(r.h1>0&&r.h2>0); if(r.net<=0||!bp) allpos=false;
        printf("%-18s %-6d %-7.1f %-7.2f %-9.0f %-9.0f %-9.0f %s\n",nm,r.n,r.n?100.0*r.w/r.n:0,pf,r.net,r.h1,r.h2,bp?"YES":"no");
    }
    printf("DEPLOY-WORTHY (net+ AND both-halves+ in EVERY regime): %s\n", allpos?"YES":"no");
    return 0;
}
