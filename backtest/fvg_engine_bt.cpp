// fvg_engine_bt.cpp — FAITHFUL backtest: drives the REAL omega::FvgContinuationEngine
// class (production code, not a re-implementation) tick-by-tick over m1 OHLC, so the
// validation is on exactly the code that would deploy. Confirms the 2026-06-16 trend-beta
// re-validation config (M30 + TRAIL + ER gate) reproduces cross-regime robustness.
//
// build: g++ -std=c++17 -O2 -I../include fvg_engine_bt.cpp -o fvg_engine_bt
// run:   ./fvg_engine_bt <label> <htf_sec> <gap_atr> <max_age> <trail_atr> <er_gate> <m1.csv> [m1b.csv ...]
//   er_gate: 0=off 1=on(ER>=0.30,period20). Each csv = one regime block (ts,o,h,l,c).
#include "FvgContinuationEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace std;

// ClusterGate references this global (normally defined in globals.hpp). Backtest is
// single-engine with no registered sources, so a fresh empty registry → always allows.
omega::OpenPositionRegistry g_open_positions;

struct Bar { long long ts; double o,h,l,c; };
static vector<Bar> load(const string& p){
    vector<Bar> v; ifstream f(p); if(!f) return v; string ln; bool first=true;
    while(getline(f,ln)){ if(ln.empty())continue;
        if(first){first=false; if(!ln.empty()&&(ln[0]<'0'||ln[0]>'9'))continue;}
        Bar b; if(sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5 && b.h>=b.l) v.push_back(b); }
    return v;
}

struct Res { int n=0,w=0; double net=0,gw=0,gl=0,h1=0,h2=0; };
static Res run_block(const vector<Bar>& bars, int htf, double gap, int age, double trail, int er){
    omega::FvgContinuationEngine e;
    e.symbol="NAS100"; e.engine_name="FvgBT"; e.enabled=true; e.shadow_mode=true; e.verbose=false;
    e.HTF_SEC=htf; e.MIN_GAP_ATR=gap; e.MAX_FVG_AGE=age; e.MAX_DOL_ATR=3.0; e.MIN_RR=1.0;
    e.TRENDN=0; e.MACD_GATE=false; e.REV_EXIT=false; e.MIN_RETRACE=0.0;  // match research harness (no MACD/rev-exit)
    e.TRAIL_EXIT=(trail>0.0); e.TRAIL_ATR=trail; e.RISK_OFF_BLOCK=false; // trail=0 -> hold-to-DOL (original)
    e.ER_GATE=(er!=0); e.ER_PERIOD=20; e.ER_THR=0.30;
    e.SESS_OPEN_HM=0; e.SESS_CLOSE_HM=2400;                              // all-session (matches sweep)
    Res r; size_t mid=bars.size()/2;
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        double half = (tr.entryTs*1000 < (long long)(bars.empty()?0:bars[mid].ts)*1000) ? 1.0 : 2.0;
        r.n++; r.net+=tr.pnl; if(tr.pnl>=0){r.w++;r.gw+=tr.pnl;} else r.gl+=-tr.pnl;
        if(half==1.0) r.h1+=tr.pnl; else r.h2+=tr.pnl;
    };
    e.init();
    for(size_t i=0;i<bars.size();++i){ const Bar&b=bars[i]; long long ms=b.ts*1000;
        // feed o, then ADVERSE extreme first (conservative: bullish bar o,l,h,c / bearish o,h,l,c),
        // then c. 1pt spread proxy. Conservative intrabar order removes the long-favoring h-before-l bias.
        e.on_tick(b.o-0.5,b.o+0.5,ms);
        if (b.c>=b.o){ e.on_tick(b.l-0.5,b.l+0.5,ms); e.on_tick(b.h-0.5,b.h+0.5,ms); }
        else         { e.on_tick(b.h-0.5,b.h+0.5,ms); e.on_tick(b.l-0.5,b.l+0.5,ms); }
        e.on_tick(b.c-0.5,b.c+0.5,ms);
    }
    if(!bars.empty()) e.force_close(bars.back().c-0.5,bars.back().c+0.5,(long long)bars.back().ts*1000);
    return r;
}

int main(int argc,char**argv){
    if(argc<8){ printf("usage: %s <label> <htf_sec> <gap> <age> <trail> <er> <csv> [csv...]\n",argv[0]); return 1; }
    string label=argv[1]; int htf=atoi(argv[2]); double gap=atof(argv[3]); int age=atoi(argv[4]);
    double trail=atof(argv[5]); int er=atoi(argv[6]);
    printf("=== %s | HTF=%ds gap=%.2f age=%d trail=%.1fATR ER=%d | REAL ENGINE ===\n",label.c_str(),htf,gap,age,trail,er);
    printf("%-14s %-6s %-7s %-7s %-9s %-9s %-9s %s\n","block","n","win%","PF","net","H1","H2","both+");
    bool allpos=true;
    for(int a=7;a<argc;++a){
        auto bars=load(argv[a]); if(bars.size()<200){ printf("  %-14s (few bars)\n",argv[a]); continue; }
        Res r=run_block(bars,htf,gap,age,trail,er);
        double pf=r.gl>0?r.gw/r.gl:(r.gw>0?99:0);
        const char* nm=strrchr(argv[a],'/'); nm=nm?nm+1:argv[a];
        bool bp=(r.h1>0&&r.h2>0); if(r.net<=0||!bp) allpos=false;
        printf("%-14s %-6d %-7.1f %-7.2f %-9.0f %-9.0f %-9.0f %s\n",nm,r.n,r.n?100.0*r.w/r.n:0,pf,r.net,r.h1,r.h2,bp?"YES":"no");
    }
    printf("ALL-BLOCKS-ROBUST: %s\n", allpos?"YES":"no");
    return 0;
}
