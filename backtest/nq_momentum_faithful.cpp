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

    auto envd=[&](const char*k,double d){ const char*v=getenv(k); return v?atof(v):d; };
    auto envi=[&](const char*k,int d){ const char*v=getenv(k); return v?atoi(v):d; };
    omega::NqMomentumEngine eng;
    eng.shadow_mode = true;
    eng.p.ig_pct = IG; eng.p.lb = LB;
    eng.p.atr_len = envi("ATRLEN",30); eng.p.atr_mult = envd("ATRMULT",4.0);
    eng.p.be_arm_pct = envd("BEARM",0.03); eng.p.be_floor_pct = envd("BEFLOOR",0.02);
    // S-2026-06-19 adverse-protection levers
    eng.p.loss_cut_atr = envd("LOSSCUT",0.0);
    eng.p.be_arm_atr   = envd("BEARMATR",0.0);
    eng.p.be_floor_atr = envd("BEFLOORATR",0.0);
    eng.p.maxhold_bars = envi("MAXHOLD",48);
    eng.p.regime_sma = envi("RGSMA",200); eng.p.regime_gate = (getenv("NOGATE")==nullptr);
    eng.p.dollars_per_pt = 1.0; eng.p.lot = 1.0;

    struct Tr{ int64_t ts; double pnl_pts; std::string reason; };
    std::vector<Tr> trs;
    auto cb=[&](const omega::TradeRecord& tr){ trs.push_back({tr.exitTs, (tr.exitPrice-tr.entryPrice), tr.exitReason}); };

    std::ifstream f(path); if(!f){fprintf(stderr,"open fail\n");return 1;}
    std::string line; bool histdata=false; int probe=0; size_t ticks=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        if(probe<1){ histdata = (line.find(' ')!=std::string::npos && line.find('-')==std::string::npos); probe=1; }
        char* p=line.data(); double bid=0,ask=0; int64_t s=0;
        if(histdata){ // YYYYMMDD HHMMSS[mmm],bid,ask  (time token may carry a millisecond suffix)
            long d=strtol(p,&p,10); while(*p==' ')++p;
            char* ts0=p; while(*p && *p!=',') ++p; int tlen=(int)(p-ts0);  // time digits incl leading zeros
            long t=strtol(ts0,nullptr,10); if(*p==',')++p;
            bid=strtod(p,&p); if(*p==',')++p; ask=strtod(p,&p);
            if(bid<=0||ask<=0)continue;
            struct tm g{}; g.tm_year=d/10000-1900; g.tm_mon=(d/100)%100-1; g.tm_mday=d%100;
            int hh,mm,ss;
            if(tlen>=9){ hh=(int)(t/10000000); mm=(int)((t/100000)%100); ss=(int)((t/1000)%100); } // HHMMSSmmm
            else       { hh=(int)(t/10000);     mm=(int)((t/100)%100);     ss=(int)(t%100); }       // HHMMSS
            g.tm_hour=hh; g.tm_min=mm; g.tm_sec=ss; s=timegm(&g);
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
    // worst single trade + exit-reason breakdown (shows adverse-protection effect)
    double worst=0; long n_lc=0,n_tr=0,n_mh=0,n_fc=0; double sum_lc=0;
    for(auto&t:trs){ if(t.pnl_pts<worst)worst=t.pnl_pts;
        if(t.reason=="LOSS_CUT"){++n_lc; sum_lc+=t.pnl_pts;} else if(t.reason=="TRAIL")++n_tr;
        else if(t.reason=="MAXHOLD")++n_mh; else ++n_fc; }
    printf("  worst trade=%+.0fpt | exits: LOSS_CUT=%ld(net%+.0f) TRAIL=%ld MAXHOLD=%ld other=%ld\n",
           worst,n_lc,sum_lc,n_tr,n_mh,n_fc);
    return 0;
}
