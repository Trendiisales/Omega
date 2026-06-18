// nq_momentum_faithful.cpp — ENGINE-FAITHFUL BT for NqMomentumEngine.
// Drives the REAL omega::NqMomentumEngine class (include/NqMomentumEngine.hpp)
// tick-by-tick on NAS100 tape (NQ proxy), cross-regime. This is the deploy
// arbiter per BACKTEST_TRUTH — NOT the bar-replay discovery (momo_cont_nq.cpp).
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/nqf backtest/nq_momentum_faithful.cpp
// run:   /tmp/nqf <nas_tick.csv> [ig_pct=0.30] [lb=6] [cost_pts=2.0]   (NOGATE=1 env to disable regime gate)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include "NqMomentumEngine.hpp"

static int yr(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick.csv> [ig_pct] [lb] [cost]\n",argv[0]); return 2; }
    const char* path=argv[1];
    const double IG  = argc>2?atof(argv[2]):0.30;
    const int    LB  = argc>3?atoi(argv[3]):6;
    const double cost= argc>4?atof(argv[4]):2.0;   // round-trip pts, applied to net at report time

    omega::NqMomentumEngine eng;
    eng.shadow_mode = true;
    eng.p.ig_pct = IG; eng.p.lb = LB; eng.p.atr_len = 30; eng.p.atr_mult = 4.0;
    eng.p.be_arm_pct = 0.03; eng.p.be_floor_pct = 0.02; eng.p.maxhold_bars = 48;
    eng.p.regime_sma = 200; eng.p.regime_gate = (getenv("NOGATE")==nullptr);
    eng.p.dollars_per_pt = 1.0; eng.p.lot = 1.0;

    struct Tr{ int64_t ts; double pnl_pts; };
    std::vector<Tr> trs;
    auto cb=[&](const omega::TradeRecord& tr){ trs.push_back({tr.exitTs, (tr.exitPrice-tr.entryPrice)}); };

    std::ifstream f(path); if(!f){fprintf(stderr,"open fail\n");return 1;}
    std::string line; bool histdata=false; int probe=0; size_t ticks=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        if(probe<1){ histdata = (line.find(' ')!=std::string::npos && line.find('-')==std::string::npos); probe=1; }
        char* p=line.data(); double bid=0,ask=0; int64_t s=0;
        if(histdata){ // YYYYMMDD HHMMSS...,bid,ask
            long d=strtol(p,&p,10); while(*p==' ')++p; long t=strtol(p,&p,10); if(*p==',')++p;
            bid=strtod(p,&p); if(*p==',')++p; ask=strtod(p,&p);
            if(bid<=0||ask<=0)continue;
            struct tm g{}; g.tm_year=d/10000-1900; g.tm_mon=(d/100)%100-1; g.tm_mday=d%100;
            g.tm_hour=t/10000; g.tm_min=(t/100)%100; g.tm_sec=t%100; s=timegm(&g);
        } else { // ts,ask,bid
            long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p;
            ask=strtod(p,&p); if(*p!=',')continue; ++p; bid=strtod(p,&p);
            if(bid<=0||ask<=0)continue; s = ts>1000000000000LL?ts/1000:ts;
        }
        eng.on_tick(bid,ask,(int64_t)s*1000,cb); ++ticks;
    }
    fprintf(stderr,"# %zu ticks, %zu trades\n",ticks,trs.size());

    // apply round-trip cost to each closed trade, report cross-regime
    for(auto&t:trs) t.pnl_pts -= cost;
    auto rep=[&](int y0,int y1,double&net,long&n,double&pf,double&wr){ double gw=0,gl=0;long w=0;net=0;n=0;
        for(auto&t:trs){int y=yr(t.ts); if(y<y0||y>y1)continue; net+=t.pnl_pts;++n; if(t.pnl_pts>0){gw+=t.pnl_pts;++w;}else gl+=-t.pnl_pts;}
        pf=gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0; };
    double net;long n;double pf,wr;
    printf("\n=== NqMomentumEngine FAITHFUL BT (IG=%.2f%% LB=%d gate=%s cost=%.1fpt) ===\n",
           IG,LB, eng.p.regime_gate?"ON":"OFF", cost);
    if(trs.empty()){ printf("  (no trades)\n"); return 0; }
    int y0=yr(trs.front().ts), y1=yr(trs.back().ts);
    for(int y=y0;y<=y1;++y){ rep(y,y,net,n,pf,wr); if(n>0) printf("  %d  n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt\n",y,n,wr,pf,net); }
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl_pts;
    rep(y0,y1,net,n,pf,wr);
    printf("  ALL n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt  WF-H1=%+.0f H2=%+.0f %s\n",
           n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    return 0;
}
