// momo_cont_nq.cpp — DISCOVERY: intraday momentum-continuation on NQ (NAS100 proxy)
// with the VALIDATED BigCap exit (ATR-trail + BE-ratchet + ride-in-profit). Liquid
// futures -> no micro-cap slippage (the thing that caps BigCap equities). Question:
// does ignition-then-ride have an edge on the index, cross-regime? If yes -> build engine.
//
// Signal (no-lookahead): on a CLOSED 5m bar, if close is up >= IG_PCT over the prior LB
// bars AND price > intraday VWAP-proxy (session up) -> ENTER LONG at next bar open.
// Exit: ATR-trail(atr_len x atr_mult) off peak + BE-ratchet(arm/floor) + ride-past-maxhold
// while in profit + hard maxhold. One position; long-only (index momentum = up-bias).
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/mcq backtest/momo_cont_nq.cpp
// run:   /tmp/mcq <nas_tick.csv> [ig_pct=0.4] [lb=6] [cost_pts=2.0]
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <deque>

static int yr(int64_t s){ time_t t=s; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }
struct Bar{ int64_t ts; double o,h,l,c; };

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tick.csv> [ig_pct] [lb] [cost]\n",argv[0]); return 2; }
    const char* path=argv[1];
    const double IG=argc>2?atof(argv[2]):0.4;      // ignition % over LB bars
    const int    LB=argc>3?atoi(argv[3]):6;        // lookback 5m bars (6=30min)
    const double cost=argc>4?atof(argv[4]):2.0;    // round-trip pts
    const int    TF=300;
    const int ATR_LEN=30; const double ATR_MULT=4.0, BE_ARM=0.03, BE_FLOOR=0.02; const int MAXHOLD=48;
    std::ifstream f(path); if(!f){fprintf(stderr,"open fail\n");return 1;}

    // aggregate ticks -> 5m bars (NAS tick: histdata "YYYYMMDD HHMMSSmmm,bid,ask" OR duka "ts,ask,bid")
    std::vector<Bar> bars; int64_t bk=-1; Bar b{};
    std::string line; bool histdata=false; int probe=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        double mid=0; int64_t s=0;
        if(probe<1){ histdata = (line.find(' ')!=std::string::npos && line.find('-')==std::string::npos); probe=1; }
        char* p=line.data();
        if(histdata){ // YYYYMMDD HHMMSS...,bid,ask
            long d=strtol(p,&p,10); while(*p==' ')++p; long t=strtol(p,&p,10); if(*p==',')++p;
            double bid=strtod(p,&p); if(*p==',')++p; double ask=strtod(p,&p);
            if(bid<=0||ask<=0)continue;
            struct tm g{}; g.tm_year=d/10000-1900; g.tm_mon=(d/100)%100-1; g.tm_mday=d%100;
            g.tm_hour=t/10000; g.tm_min=(t/100)%100; g.tm_sec=t%100; s=timegm(&g);
            mid=(bid+ask)/2;
        } else { // ts,ask,bid
            long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p;
            double ask=strtod(p,&p); if(*p!=',')continue; ++p; double bid=strtod(p,&p);
            if(bid<=0||ask<=0)continue; s = ts>1000000000000LL?ts/1000:ts; mid=(bid+ask)/2;
        }
        int64_t k=s/TF;
        if(k!=bk){ if(bk>=0) bars.push_back(b); bk=k; b={s,mid,mid,mid,mid}; }
        else { if(mid>b.h)b.h=mid; if(mid<b.l)b.l=mid; b.c=mid; }
    }
    if(bk>=0)bars.push_back(b);
    fprintf(stderr,"# %zu 5m bars\n",bars.size());
    if(bars.size()<200){fprintf(stderr,"few bars\n");return 1;}

    // ATR(14) on 5m bars
    std::vector<double> atr(bars.size(),0); { double s=0,pc=0; int n=0; double a=0;
        for(size_t i=0;i<bars.size();++i){ double tr=bars[i].h-bars[i].l; if(pc>0){double x=fabs(bars[i].h-pc),y=fabs(pc-bars[i].l); if(x>tr)tr=x; if(y>tr)tr=y;} pc=bars[i].c;
            if(n<14){s+=tr; if(++n==14)a=s/14;} else a=(a*13+tr)/14; atr[i]=a; } }

    struct Tr{ int64_t ts; double pnl; }; std::vector<Tr> trs;
    bool inpos=false; double entry=0,peak=0,sl_atr=0; int hold=0;
    for(size_t i=(size_t)LB;i+1<bars.size();++i){
        if(inpos){
            const Bar& nb=bars[i];
            if(nb.h>peak)peak=nb.h; hold++;
            double tstop=peak-ATR_MULT*sl_atr;
            if(peak>=entry*(1+BE_ARM)){ double bf=entry*(1+BE_FLOOR); if(bf>tstop)tstop=bf; }
            if(nb.l<=tstop){ trs.push_back({nb.ts,(tstop-entry)-cost}); inpos=false; continue; }
            if(hold>=MAXHOLD && !(nb.c>entry)){ trs.push_back({nb.ts,(nb.c-entry)-cost}); inpos=false; continue; }
            continue;
        }
        // signal on closed bar i: ignition over LB
        double base=bars[i-LB].c; if(base<=0||atr[i]<=0) continue;
        double up=(bars[i].c/base-1)*100;
        // bull-regime gate (proxy: price > SMA(RG) of 5m closes = uptrend; BigCap-style)
        const int RG=200; bool regime_ok=true;
        if(i>=(size_t)RG){ double sma=0; for(int k=0;k<RG;k++)sma+=bars[i-k].c; sma/=RG; regime_ok=(bars[i].c>sma); }
        if(up>=IG && bars[i].c>bars[i].o && (getenv("NOGATE")||regime_ok)){   // momentum + green + bull-regime
            inpos=true; entry=bars[i+1].o; peak=entry; sl_atr=atr[i]; hold=0;  // fill next-bar open
        }
    }
    // stats
    auto rep=[&](int y0,int y1,double&net,long&n,double&pf,double&wr){ double gw=0,gl=0;long w=0;net=0;n=0;
        for(auto&t:trs){int y=yr(t.ts); if(y<y0||y>y1)continue; net+=t.pnl;++n; if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl;}
        pf=gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0; };
    double net;long n;double pf,wr;
    printf("\n=== NQ momentum-continuation DISCOVERY (IG=%.1f%% LB=%d, ATR-trail+BE+ride, cost=%.1fpt) ===\n",IG,LB,cost);
    int y0=yr(bars.front().ts), y1=yr(bars.back().ts);
    for(int y=y0;y<=y1;++y){ rep(y,y,net,n,pf,wr); if(n>0) printf("  %d  n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt\n",y,n,wr,pf,net); }
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?h1:h2)+=trs[i].pnl;
    rep(y0,y1,net,n,pf,wr);
    printf("  ALL n=%4ld WR=%.0f%% PF=%.2f net=%+.0fpt  WF-H1=%+.0f H2=%+.0f %s\n",n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    return 0;
}
