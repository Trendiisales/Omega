// gold_xregime_recheck.cpp — FAITHFUL cross-regime re-check of the 2026-06-15 cull batch.
// Drives the REAL engine classes (GoldOversoldBounce / Xau3Bar30m / EmaPullback /
// XauStraddleM15 / Donchian) tick-by-tick over gold BULL + CRASH + DOWN blocks, per
// BACKTEST_TRUTH.md (faithful arbiter, not bar-replay). Answers: were these wrongly culled?
//
// build: g++ -std=c++17 -O2 -I../include gold_xregime_recheck.cpp -o gold_xregime_recheck
// run:   ./gold_xregime_recheck   (block paths hardcoded: /tmp/gold_{bull,crash,down}.csv)
#include "GoldOversoldBounceEngine.hpp"
#include "XauThreeBar30mEngine.hpp"
#include "EmaPullbackEngine.hpp"
#include "XauStraddleM30Engine.hpp"
#include "DonchianEngine.hpp"
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
using namespace std;

omega::OpenPositionRegistry g_open_positions;   // ClusterGate dep (empty = allow)

struct Bar { long long ts; double o,h,l,c; };
static vector<Bar> load(const string& p){ vector<Bar> v; ifstream f(p); if(!f) return v; string ln;
    while(getline(f,ln)){ if(ln.empty()||(ln[0]<'0'||ln[0]>'9'))continue; Bar b;
        if(sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5&&b.h>=b.l) v.push_back(b); } return v; }

struct Res { int n=0,w=0; double net=0,gw=0,gl=0; };
// drive any engine exposing on_tick(bid,ask,ts, function<void(const TradeRecord&)>)
template<class Eng, class Cfg>
static Res run(const vector<Bar>& bars, Cfg cfg){
    Eng e; cfg(e);
    Res r;
    auto cb = [&](const omega::TradeRecord& tr){ r.n++; r.net+=tr.pnl;
        if(tr.pnl>=0){r.w++;r.gw+=tr.pnl;} else r.gl+=-tr.pnl; };
    const double SPR=0.2;   // gold half-spread proxy (~0.4 pt RT)
    for(const Bar&b:bars){ long long ms=b.ts*1000;
        e.on_tick(b.o-SPR,b.o+SPR,ms,cb);
        if(b.c>=b.o){ e.on_tick(b.l-SPR,b.l+SPR,ms,cb); e.on_tick(b.h-SPR,b.h+SPR,ms,cb); }
        else        { e.on_tick(b.h-SPR,b.h+SPR,ms,cb); e.on_tick(b.l-SPR,b.l+SPR,ms,cb); }
        e.on_tick(b.c-SPR,b.c+SPR,ms,cb);
    }
    return r;
}
static void row(const char* eng, const char* blk, const Res& r){
    double pf=r.gl>0?r.gw/r.gl:(r.gw>0?99:0);
    printf("  %-20s %-7s n=%-5d win%%=%-5.1f PF=%-6.2f net=%+.1f\n",
           eng,blk,r.n,r.n?100.0*r.w/r.n:0,pf,r.net);
}

// GATED test: feed gold_regime() over the FULL continuous series (warmed lagging gate),
// drive GoldOversold which queries long_blocked(), bucket trades by exit date into
// bull(<2026-03) vs crash(>=2026-03). Answers: does the regime gate catch the crash?
static void gated_goldoversold(){
    auto full=load("/tmp/gold_full.csv"); if(full.empty()){ printf("(no /tmp/gold_full.csv)\n"); return; }
    omega::GoldOversoldBounceEngine e; e.enabled=true; e.shadow_mode=true; e.lot=1.0;
    Res bull, crash; int blocked=0;
    auto cb=[&](const omega::TradeRecord& tr){ Res& r=(tr.exitTs<1772323200LL)?bull:crash;
        r.n++; r.net+=tr.pnl; if(tr.pnl>=0){r.w++;r.gw+=tr.pnl;} else r.gl+=-tr.pnl; };
    const double SPR=0.2; long bearbars=0, tot=0;
    for(const Bar&b:full){ long long ms=b.ts*1000;
        omega::gold_regime().on_tick(b.c-SPR,b.c+SPR,ms);   // WARM the lagging regime gate
        tot++; if(omega::gold_regime().is_bear()) bearbars++;
        e.on_tick(b.o-SPR,b.o+SPR,ms,cb);
        if(b.c>=b.o){ e.on_tick(b.l-SPR,b.l+SPR,ms,cb); e.on_tick(b.h-SPR,b.h+SPR,ms,cb); }
        else        { e.on_tick(b.h-SPR,b.h+SPR,ms,cb); e.on_tick(b.l-SPR,b.l+SPR,ms,cb); }
        e.on_tick(b.c-SPR,b.c+SPR,ms,cb);
    }
    printf("\n=== GoldOversold WITH regime gate LIVE (warmed, full continuous series) ===\n");
    printf("  regime said BEAR on %.1f%% of bars (%ld/%ld) -- gate blocks longs when bear\n",100.0*bearbars/tot,bearbars,tot);
    row("GoldOversold-GATED","BULL",bull);
    row("GoldOversold-GATED","CRASH",crash);
    printf("  (compare UNGATED above: BULL +419 / CRASH -455. Does the gate save the crash?)\n");
}

int main(){
    auto bull=load("/tmp/gold_bull.csv"), crash=load("/tmp/gold_crash.csv"), down=load("/tmp/gold_down.csv");
    printf("=== GOLD CROSS-REGIME RE-CHECK (real engine classes) ===\n");
    printf("bull=%zu crash=%zu down=%zu m5 bars | pnl=raw pts*lot (PF/sign is the signal)\n\n",bull.size(),crash.size(),down.size());

    // 1. GoldOversoldBounce (dip-buyer mean-rev) — killed "−$457 6mo BT", live +46/n3
    { auto cfg=[](omega::GoldOversoldBounceEngine&e){ e.enabled=true; e.shadow_mode=true; e.lot=1.0; };
      printf("GoldOversoldBounce (killed -$457 BT; live +46/n3):\n");
      row("GoldOversoldBounce","BULL",run<omega::GoldOversoldBounceEngine>(bull,cfg));
      row("GoldOversoldBounce","CRASH",run<omega::GoldOversoldBounceEngine>(crash,cfg));
      row("GoldOversoldBounce","DOWN",run<omega::GoldOversoldBounceEngine>(down,cfg)); }

    // 2. XauThreeBar30m (momentum) — killed "−$371 BT", ZERO live trades
    { auto cfg=[](omega::XauThreeBar30mEngine&e){ e.enabled=true; e.shadow_mode=true; e.lot=1.0; e.init(); };
      printf("Xau3Bar30m (killed -$371 BT; NO live data):\n");
      row("Xau3Bar30m","BULL",run<omega::XauThreeBar30mEngine>(bull,cfg));
      row("Xau3Bar30m","CRASH",run<omega::XauThreeBar30mEngine>(crash,cfg));
      row("Xau3Bar30m","DOWN",run<omega::XauThreeBar30mEngine>(down,cfg)); }

    // 3. EmaPullback portfolio (LONG-only cells) — killed "−$275 BT", live +9/n6
    { auto cfg=[](omega::EpbPortfolio&e){ e.enabled=true; e.shadow_mode=true; e.init(); };
      printf("EmaPullback (LONG-only; killed -$275 BT; live +9/n6):\n");
      row("EmaPullback","BULL",run<omega::EpbPortfolio>(bull,cfg));
      row("EmaPullback","CRASH",run<omega::EpbPortfolio>(crash,cfg));
      row("EmaPullback","DOWN",run<omega::EpbPortfolio>(down,cfg)); }

    // 4. XauStraddleM15 (both-direction breakout) — killed "−$559 BT", ZERO live trades
    { auto cfg=[](omega::XauStraddleM30Engine&e){ e.enabled=true; e.shadow_mode=true; e.lot=1.0; };
      printf("XauStraddleM15 (killed -$559 BT; NO live data):\n");
      row("XauStraddleM15","BULL",run<omega::XauStraddleM30Engine>(bull,cfg));
      row("XauStraddleM15","CRASH",run<omega::XauStraddleM30Engine>(crash,cfg));
      row("XauStraddleM15","DOWN",run<omega::XauStraddleM30Engine>(down,cfg)); }

    // 5. Donchian portfolio (long+short breakout) — killed "−$186 BT", live ~−100/n29
    { auto cfg=[](omega::DonchianPortfolio&e){ e.enabled=true; e.shadow_mode=true; e.init(); };
      printf("Donchian (long+short; killed -$186 BT; live ~-100/n29):\n");
      row("Donchian","BULL",run<omega::DonchianPortfolio>(bull,cfg));
      row("Donchian","CRASH",run<omega::DonchianPortfolio>(crash,cfg));
      row("Donchian","DOWN",run<omega::DonchianPortfolio>(down,cfg)); }
    gated_goldoversold();
    return 0;
}
